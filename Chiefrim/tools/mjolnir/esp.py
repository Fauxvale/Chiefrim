#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reads and writes Skyrim Special Edition plugins (ESM/ESP, form version
44), for the Mjolnir armor port: records as their subrecords, groups by
their records' types.

Layout (as UESP's "Skyrim Mod:Mod File Format" gives it): a record is a
24-byte header (type, data size, flags, form ID, version control, form
version, unknown) then its data, zlib-compressed after its decompressed
size when its flags have 0x40000; its data is subrecords, each a type, a
16-bit size and its bytes (an XXXX subrecord's 32-bit value is the size of
the next, when it is larger). A group is "GRUP", its size (with its 24-byte
header), its label (a top group's: its records' type), its type (0: top),
then 8 bytes, then its records and groups.

Usage: tools/mjolnir/esp.py PLUGIN [--type ARMO] [--edid EDITOR_ID]
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

COMPRESSED = 0x40000


class Record:
	def __init__(self, type, form_id, flags=0, subrecords=None, version=44, vc=(0, 0), unknown=0):
		self.type, self.form_id, self.flags = type, form_id, flags
		self.subrecords = subrecords if subrecords is not None else []  # [(type, bytes)]
		self.version, self.vc, self.unknown = version, vc, unknown

	def get(self, type, default=None):
		return next((data for t, data in self.subrecords if t == type), default)

	def all(self, type):
		return [data for t, data in self.subrecords if t == type]

	@property
	def editor_id(self):
		data = self.get("EDID")
		return data.split(b"\0")[0].decode("latin-1") if data else None

	def set(self, type, data):
		"""Replaces the first subrecord of type (or appends one)."""
		for index, (t, _) in enumerate(self.subrecords):
			if t == type:
				self.subrecords[index] = (type, data)
				return
		self.subrecords.append((type, data))

	def remove(self, type):
		self.subrecords = [(t, d) for t, d in self.subrecords if t != type]

	def write(self):
		body = bytearray()
		for type, data in self.subrecords:
			if len(data) > 0xFFFF:
				body += struct.pack("<4sHI", b"XXXX", 4, len(data)) + struct.pack("<4sH", type.encode(), 0)
			else:
				body += struct.pack("<4sH", type.encode(), len(data))
			body += data
		flags = self.flags
		if flags & COMPRESSED:
			body = struct.pack("<I", len(body)) + zlib.compress(bytes(body))
		return struct.pack("<4sIIIBBHHH", self.type.encode(), len(body), flags, self.form_id, *self.vc, 0, self.version, self.unknown) + bytes(body)


def parse_subrecords(data):
	subrecords, position, extended = [], 0, None
	while position < len(data):
		type, size = struct.unpack_from("<4sH", data, position)
		position += 6
		if extended is not None:
			size, extended = extended, None
		if type == b"XXXX":
			extended, = struct.unpack_from("<I", data, position)
			position += size
			continue
		subrecords.append((type.decode("latin-1"), bytes(data[position:position + size])))
		position += size
	return subrecords


def read_record(data, position):
	type, size, flags, form_id, vc1, vc2, _, version, unknown = struct.unpack_from("<4sIIIBBHHH", data, position)
	body = data[position + 24:position + 24 + size]
	if flags & COMPRESSED:
		body = zlib.decompress(body[4:])
	return Record(type.decode("latin-1"), form_id, flags, parse_subrecords(body), version, (vc1, vc2), unknown), position + 24 + size


class Plugin:
	"""A plugin's header record and its top groups' records, by type."""

	def __init__(self, path=None, types=None):
		self.header = None
		self.groups = {}  # type -> [Record], in order
		if path is not None:
			self.read(Path(path), types)

	def read(self, path, types=None):
		"""Reads the header and the top groups of these types (all, if
		None); the rest are skipped by their sizes."""
		with open(path, "rb") as file:
			data = file.read(24)
			size, = struct.unpack_from("<I", data, 4)
			data += file.read(size)
			self.header, _ = read_record(data, 0)
			while True:
				head = file.read(24)
				if len(head) < 24:
					break
				magic, size, label, group_type = struct.unpack_from("<4sI4sI", head)
				if magic != b"GRUP" or group_type != 0:
					raise ValueError(f"{path}: expected a top group at {file.tell() - 24}")
				label = label.decode("latin-1")
				if types is not None and label not in types:
					file.seek(size - 24, 1)
					continue
				self.groups[label] = self.records(file.read(size - 24))

	def records(self, data):
		"""A group's records, and its subgroups', in order."""
		result, position = [], 0
		while position < len(data):
			if data[position:position + 4] == b"GRUP":
				size, = struct.unpack_from("<I", data, position + 4)
				result += self.records(data[position + 24:position + size])
				position += size
			else:
				record, position = read_record(data, position)
				result.append(record)
		return result

	def find(self, type, editor_id):
		return next((r for r in self.groups.get(type, []) if r.editor_id == editor_id), None)

	def by_id(self, type, form_id):
		return next((r for r in self.groups.get(type, []) if r.form_id == form_id), None)

	def write(self, path):
		"""The header, then a top group per type (only records of one type
		each, as the top groups Skyrim's plugins have that hold no others)."""
		data = bytearray(self.header.write())
		for type, records in self.groups.items():
			body = b"".join(record.write() for record in records)
			data += struct.pack("<4sI4sIHHHH", b"GRUP", 24 + len(body), type.encode(), 0, 0, 0, 0, 0) + body
		Path(path).parent.mkdir(parents=True, exist_ok=True)
		Path(path).write_bytes(data)
		return path


def main():
	parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
	parser.add_argument("plugin")
	parser.add_argument("--type", default=None)
	parser.add_argument("--edid", default=None)
	args = parser.parse_args()
	plugin = Plugin(args.plugin, [args.type] if args.type else None)
	print(f"{args.plugin}: " + ", ".join(f"{t} {len(r)}" for t, r in plugin.groups.items()))
	for type, records in plugin.groups.items():
		for record in records:
			if args.edid and record.editor_id != args.edid:
				continue
			if args.edid:
				print(f"{type} {record.form_id:08X} {record.editor_id} flags {record.flags:#x}")
				for sub, data in record.subrecords:
					text = data[:-1].decode("latin-1") if data.endswith(b"\0") and data[:-1].isascii() and len(data) > 1 else data.hex(" ")
					print(f"  {sub} ({len(data)}): {text[:200]}")


if __name__ == "__main__":
	sys.exit(main())
