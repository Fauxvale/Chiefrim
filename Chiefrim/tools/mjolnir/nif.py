#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reads Skyrim Special Edition NIFs (Gamebryo 20.2.0.7, user version 12,
Bethesda version 100), for the Mjolnir armor port: the blocks this port
needs, and every other block skipped by the size the header gives it.

Field layouts are nifxml's (niftools' nif.xml) for these versions.

Usage: tools/mjolnir/nif.py FILE.nif [--tree]
"""
import argparse
import struct
import sys
from pathlib import Path


class Reader:
	def __init__(self, data, position=0):
		self.data = data
		self.position = position

	def unpack(self, fmt):
		values = struct.unpack_from("<" + fmt, self.data, self.position)
		self.position += struct.calcsize("<" + fmt)
		return values

	def one(self, fmt):
		return self.unpack(fmt)[0]

	def string(self, length_fmt):
		length = self.one(length_fmt)
		value = self.data[self.position:self.position + length]
		self.position += length
		return value.split(b"\0")[0].decode("latin-1")


class Nif:
	def __init__(self, path):
		data = Path(path).read_bytes()
		line_end = data.index(b"\n")
		self.header_string = data[:line_end].decode("latin-1")
		reader = Reader(data, line_end + 1)
		self.version, endian, self.user_version, block_count, self.bs_version = reader.unpack("IBIII")
		if self.version != 0x14020007 or self.user_version != 12:
			raise ValueError(f"{path}: version {self.version:#x}/{self.user_version}, not Skyrim's 20.2.0.7/12")
		self.export_info = [reader.string("B") for _ in range(3)]
		types = [reader.string("I") for _ in range(reader.one("H"))]
		self.block_types = [types[reader.one("H") & 0x7FFF] for _ in range(block_count)]
		sizes = reader.unpack(f"{block_count}I")
		string_count, _ = reader.unpack("II")
		self.strings = [reader.string("I") for _ in range(string_count)]
		reader.unpack(f"{reader.one('I')}I")  # groups
		self.blocks = []
		for type, size in zip(self.block_types, sizes):
			start = reader.position
			parse = getattr(self, "parse_" + type, None)
			block = {"type": type}
			if parse is not None:
				parse(Reader(data, start), block)
			self.blocks.append(block)
			reader.position = start + size
		self.roots = list(reader.unpack(f"{reader.one('I')}i"))

	def name(self, index):
		return self.strings[index] if 0 <= index < len(self.strings) else None

	# ---- NiObjectNET, NiAVObject, NiNode

	def object_net(self, reader, block):
		block["name"] = self.name(reader.one("i"))
		block["extra_data"] = list(reader.unpack(f"{reader.one('I')}i"))
		block["controller"] = reader.one("i")

	def av_object(self, reader, block):
		self.object_net(reader, block)
		block["flags"] = reader.one("I")
		block["translation"] = reader.unpack("3f")
		block["rotation"] = [reader.unpack("3f") for _ in range(3)]  # rows
		block["scale"] = reader.one("f")
		block["collision"] = reader.one("i")

	def parse_NiNode(self, reader, block):
		self.av_object(reader, block)
		block["children"] = list(reader.unpack(f"{reader.one('I')}i"))
		block["effects"] = list(reader.unpack(f"{reader.one('I')}i"))

	parse_BSFadeNode = parse_NiNode
	parse_BSLeafAnimNode = parse_NiNode

	# ---- tree

	def walk(self, index=None, parent=None, depth=0):
		"""(depth, index, block, parent index) for every node from the roots."""
		for root in ([index] if index is not None else self.roots):
			if root < 0:
				continue
			block = self.blocks[root]
			yield depth, root, block, parent
			for child in block.get("children", []):
				yield from self.walk(child, root, depth + 1)


def main():
	parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
	parser.add_argument("nif")
	parser.add_argument("--tree", action="store_true")
	args = parser.parse_args()
	nif = Nif(args.nif)
	print(f"{args.nif}: {len(nif.blocks)} blocks, Bethesda version {nif.bs_version}, {len(nif.strings)} strings")
	if args.tree:
		for depth, index, block, _ in nif.walk():
			extra = ""
			if "translation" in block:
				extra = " t=(" + ", ".join(f"{v:.3f}" for v in block["translation"]) + f") s={block['scale']:.3f}"
			print(f"{'  ' * depth}{index} {block['type']} {block.get('name') or ''}{extra}")
	else:
		for index, type in enumerate(nif.block_types):
			print(index, type, nif.blocks[index].get("name") or "")


if __name__ == "__main__":
	sys.exit(main())
