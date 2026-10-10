#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reads files out of the user's own Skyrim Special Edition archives (BSA
version 105), for the Mjolnir armor port. Nothing is written but what is
asked for, to where it is asked.

Layout (as UESP's "Skyrim Mod:Archive File Format" gives it): a 36-byte
header; 24-byte folder records; per folder its name (a length byte, then
the name and a 0) and its 16-byte file records; then the files' names, each
ending in a 0. A file's size has bit 30 set when its compression is the
opposite of the archive's; a compressed file is its original size, then an
LZ4 frame.

Usage: tools/mjolnir/bsa.py ARCHIVE.bsa [--list PATTERN] [--extract PATH --out FILE]
"""
import argparse
import fnmatch
import struct
import sys
from pathlib import Path


def lz4_block(source, output):
	"""Appends an LZ4 block's bytes to output (a bytearray holding what the
	frame decoded before it, which a match may reach back into)."""
	position = 0
	while position < len(source):
		token = source[position]
		position += 1
		length = token >> 4
		if length == 15:
			while True:
				extra = source[position]
				position += 1
				length += extra
				if extra != 255:
					break
		output += source[position:position + length]
		position += length
		if position >= len(source):
			break
		offset = source[position] | source[position + 1] << 8
		position += 2
		length = token & 15
		if length == 15:
			while True:
				extra = source[position]
				position += 1
				length += extra
				if extra != 255:
					break
		length += 4
		start = len(output) - offset
		if offset >= length:
			output += output[start:start + length]
		else:
			for index in range(length):
				output.append(output[start + index])


def lz4_frame(source):
	if source[:4] != b"\x04\x22\x4d\x18":
		raise ValueError("not an LZ4 frame")
	flags = source[4]
	position = 7 + (8 if flags & 0x08 else 0) + (4 if flags & 0x01 else 0)
	output = bytearray()
	while True:
		size, = struct.unpack_from("<I", source, position)
		position += 4
		if size == 0:
			break
		block = source[position:position + (size & 0x7FFFFFFF)]
		position += size & 0x7FFFFFFF
		if size & 0x80000000:
			output += block
		else:
			lz4_block(block, output)
		if flags & 0x10:
			position += 4  # block checksum
	return bytes(output)


class Archive:
	def __init__(self, path):
		self.path = Path(path)
		with open(self.path, "rb") as file:
			header = file.read(36)
			magic, version, _, flags, folder_count, file_count, _, names_length, _ = struct.unpack("<4sIIIIIIIH2x", header)
			if magic != b"BSA\0" or version != 105:
				raise ValueError(f"{path}: not a Skyrim Special Edition archive (version {version})")
			self.compressed = bool(flags & 0x4)
			self.embedded_names = bool(flags & 0x100)
			folders = [struct.unpack("<QIIQ", file.read(24)) for _ in range(folder_count)]
			records = []
			for _, count, _, _ in folders:
				length = file.read(1)[0]
				folder = file.read(length)[:-1].decode("latin-1")
				for _ in range(count):
					_, size, offset = struct.unpack("<QII", file.read(16))
					records.append((folder, size, offset))
			names = file.read(names_length).split(b"\0")
		self.files = {}
		for (folder, size, offset), name in zip(records, names):
			path = f"{folder}\\{name.decode('latin-1')}".lower()
			self.files[path] = (size, offset)

	def read(self, path):
		size, offset = self.files[path.lower().replace("/", "\\")]
		compressed = self.compressed != bool(size & 0x40000000)
		size &= 0x3FFFFFFF
		with open(self.path, "rb") as file:
			file.seek(offset)
			data = file.read(size)
		if self.embedded_names:
			data = data[1 + data[0]:]
		if not compressed:
			return data
		original, = struct.unpack_from("<I", data)
		result = lz4_frame(data[4:])
		if len(result) != original:
			raise ValueError(f"{path}: {len(result)} bytes decompressed, the archive says {original}")
		return result


def main():
	parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
	parser.add_argument("archive")
	parser.add_argument("--list", metavar="PATTERN")
	parser.add_argument("--extract", metavar="PATH")
	parser.add_argument("--out")
	args = parser.parse_args()
	archive = Archive(args.archive)
	if args.list:
		for path in sorted(archive.files):
			if fnmatch.fnmatch(path, args.list.lower()):
				print(path)
	if args.extract:
		data = archive.read(args.extract)
		Path(args.out or Path(args.extract.replace("\\", "/")).name).write_bytes(data)
		print(f"{args.extract}: {len(data)} bytes")


if __name__ == "__main__":
	sys.exit(main())
