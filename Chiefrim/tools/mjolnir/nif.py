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


# a vertex desc's flags (BSVertexDesc's, from its bit 44)
VF_VERTEX = 0x001
VF_UV = 0x002
VF_UV_2 = 0x004
VF_NORMAL = 0x008
VF_TANGENT = 0x010
VF_COLORS = 0x020
VF_SKINNED = 0x040
VF_LAND_DATA = 0x080
VF_EYE_DATA = 0x100
VF_INSTANCE = 0x200
VF_FULL_PRECISION = 0x400


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

	# ---- skinned meshes: BSTriShape, BSDismemberSkinInstance, NiSkinData,
	# NiSkinPartition (Skyrim SE keeps a skinned shape's vertices in its
	# partition, the shape's own data size being 0)

	def transform(self, reader):
		rotation = [reader.unpack("3f") for _ in range(3)]
		return {"rotation": rotation, "translation": reader.unpack("3f"), "scale": reader.one("f")}

	def vertices(self, reader, desc, count):
		"""BSVertexDataSSE: the attributes vertex desc's flags (its bits 44
		up) say it has, in this order. Skyrim SE's positions are always
		floats (VF_FULL_PRECISION is Fallout 4's)."""
		flags = desc >> 44
		result = []
		for _ in range(count):
			vertex = {}
			if flags & VF_VERTEX:
				if flags & VF_FULL_PRECISION or self.bs_version == 100:
					*vertex["position"], vertex["bitangent_x"] = reader.unpack("4f")
				else:
					*vertex["position"], vertex["bitangent_x"] = reader.unpack("4e")
			if flags & VF_UV:
				vertex["uv"] = reader.unpack("2e")
			if flags & VF_NORMAL:
				*vertex["normal"], vertex["bitangent_y"] = (b / 127.5 - 1 for b in reader.unpack("4B"))
			if flags & VF_TANGENT:
				*vertex["tangent"], vertex["bitangent_z"] = (b / 127.5 - 1 for b in reader.unpack("4B"))
			if flags & VF_COLORS:
				vertex["color"] = reader.unpack("4B")
			if flags & VF_SKINNED:
				vertex["weights"] = reader.unpack("4e")
				vertex["bones"] = reader.unpack("4B")
			if flags & VF_EYE_DATA:
				vertex["eye"] = reader.one("f")
			result.append(vertex)
		return result

	def parse_BSTriShape(self, reader, block):
		self.av_object(reader, block)
		block["bound"] = reader.unpack("4f")
		block["skin"], block["shader_property"], block["alpha_property"] = reader.unpack("3i")
		block["vertex_desc"] = reader.one("Q")
		triangle_count, vertex_count, data_size = reader.unpack("HHI")
		block["vertex_count"], block["triangle_count"] = vertex_count, triangle_count
		block["vertices"] = self.vertices(reader, block["vertex_desc"], vertex_count) if data_size else []
		block["triangles"] = [reader.unpack("3H") for _ in range(triangle_count)] if data_size else []

	def parse_BSDismemberSkinInstance(self, reader, block):
		block["data"], block["partition"], block["skeleton_root"] = reader.unpack("3i")
		block["bones"] = list(reader.unpack(f"{reader.one('I')}i"))
		block["partitions"] = [reader.unpack("HH") for _ in range(reader.one("I"))]  # flags, body part

	def parse_NiSkinData(self, reader, block):
		block["skin_transform"] = self.transform(reader)
		count, has_weights = reader.unpack("IB")
		block["bones"] = []
		for _ in range(count):
			bone = {"transform": self.transform(reader), "bound": reader.unpack("4f")}
			weights = reader.one("H")
			bone["weights"] = [reader.unpack("Hf") for _ in range(weights)] if has_weights else []
			block["bones"].append(bone)

	def parse_NiSkinPartition(self, reader, block):
		count = reader.one("I")
		data_size, vertex_size, desc = reader.unpack("IIQ")
		block["vertex_desc"] = desc
		start = reader.position
		block["vertices"] = self.vertices(reader, desc, data_size // vertex_size) if vertex_size else []
		if reader.position != start + data_size:
			raise ValueError(f"NiSkinPartition: {reader.position - start} bytes of vertices read, its data size is {data_size}")
		block["partitions"] = []
		for _ in range(count):
			vertices, triangles, bones, strips, weights_per_vertex = reader.unpack("5H")
			partition = {"bones": list(reader.unpack(f"{bones}H"))}
			if reader.one("B"):
				partition["vertex_map"] = list(reader.unpack(f"{vertices}H"))
			if reader.one("B"):
				partition["vertex_weights"] = [reader.unpack(f"{weights_per_vertex}f") for _ in range(vertices)]
			strip_lengths = reader.unpack(f"{strips}H")
			if reader.one("B"):
				if strips:
					partition["strips"] = [reader.unpack(f"{n}H") for n in strip_lengths]
				else:
					partition["triangles"] = [reader.unpack("3H") for _ in range(triangles)]
			if reader.one("B"):
				partition["bone_indices"] = [reader.unpack(f"{weights_per_vertex}B") for _ in range(vertices)]
			partition["lod_level"], partition["global_vb"] = reader.unpack("BB")
			partition["vertex_desc"] = reader.one("Q")
			partition["triangles_copy"] = [reader.unpack("3H") for _ in range(triangles)]
			block["partitions"].append(partition)

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
