#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reads and writes Skyrim Special Edition NIFs (Gamebryo 20.2.0.7, user
version 12, Bethesda version 100), for the Mjolnir armor port: the blocks
this port needs, field by field, and every other block kept as its bytes.
What it reads it writes back byte for byte (--check).

Field layouts are nifxml's (niftools' nif.xml) for these versions.

Usage: tools/mjolnir/nif.py FILE.nif [--tree] [--check]
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

# BSLightingShaderProperty's floats after the common ones, by its shader type
# (environment map, skin tint, hair tint, parallax occlusion, multilayer
# parallax, sparkle snow, eye environment map)
SHADER_TYPE_FLOATS = {1: 1, 5: 3, 6: 3, 7: 2, 11: 6, 14: 4, 16: 7}

HEADER = "Gamebryo File Format, Version 20.2.0.7"


def vertex_desc(flags):
	"""A BSVertexDesc for Skyrim SE's vertex layout with these flags: its
	size and each attribute's offset in 4-byte units, then its flags."""
	offsets, size = {}, 0
	for flag, name, length in ((VF_VERTEX, "vertex", 16), (VF_UV, "uv", 4), (VF_UV_2, "uv2", 4), (VF_NORMAL, "normal", 4),
			(VF_TANGENT, "tangent", 4), (VF_COLORS, "color", 4), (VF_SKINNED, "skin", 12), (VF_LAND_DATA, "land", 0),
			(VF_EYE_DATA, "eye", 4)):
		if flags & flag:
			offsets[name] = size
			size += length
	desc = size // 4
	for shift, name in ((8, "uv"), (12, "uv2"), (16, "normal"), (20, "tangent"), (24, "color"), (28, "skin"), (36, "eye")):
		desc |= offsets.get(name, 0) // 4 << shift
	return desc | flags << 44


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


class Writer:
	def __init__(self):
		self.data = bytearray()

	def pack(self, fmt, *values):
		self.data += struct.pack("<" + fmt, *values)

	def string(self, length_fmt, value, terminated=False):
		value = value.encode("latin-1") + (b"\0" if terminated else b"")
		self.pack(length_fmt, len(value))
		self.data += value


class Nif:
	"""A NIF's header and blocks; blocks are dicts with their "type", and
	refer to each other by index (-1: none) and to strings by value."""

	def __init__(self, path=None):
		self.header_string = HEADER
		self.version, self.user_version, self.bs_version = 0x14020007, 12, 100
		self.export_info = ["", "", ""]
		self.types, self.strings, self.blocks, self.roots = [], [], [], [0]
		if path is not None:
			self.read(Path(path).read_bytes(), path)

	def read(self, data, path=""):
		line_end = data.index(b"\n")
		self.header_string = data[:line_end].decode("latin-1")
		reader = Reader(data, line_end + 1)
		self.version, endian, self.user_version, block_count, self.bs_version = reader.unpack("IBIII")
		if self.version != 0x14020007 or self.user_version != 12 or self.bs_version != 100:
			raise ValueError(f"{path}: version {self.version:#x}/{self.user_version}/{self.bs_version}, not Skyrim SE's 20.2.0.7/12/100")
		self.export_info = [reader.string("B") for _ in range(3)]
		self.types = [reader.string("I") for _ in range(reader.one("H"))]
		self.block_types = [self.types[reader.one("H") & 0x7FFF] for _ in range(block_count)]
		sizes = reader.unpack(f"{block_count}I")
		string_count, _ = reader.unpack("II")
		self.strings = [reader.string("I") for _ in range(string_count)]
		reader.unpack(f"{reader.one('I')}I")  # groups
		self.blocks = []
		for type, size in zip(self.block_types, sizes):
			start = reader.position
			block = {"type": type, "raw": data[start:start + size]}
			parse = getattr(self, "parse_" + type, None)
			if parse is not None:
				parse(Reader(data, start), block)
			self.blocks.append(block)
			reader.position = start + size
		self.roots = list(reader.unpack(f"{reader.one('I')}i"))

	def name(self, index):
		return self.strings[index] if 0 <= index < len(self.strings) else None

	def write(self, path=None):
		"""The NIF's bytes (written to path, if given). Block types and
		strings are the ones read, in their order, then any new ones."""
		strings = list(self.strings)
		def string_index(value):
			if value is None:
				return -1
			if value not in strings:
				strings.append(value)
			return strings.index(value)
		self.string_index = string_index
		bodies = []
		for block in self.blocks:
			writer = Writer()
			write = getattr(self, "write_" + block["type"], None)
			if write is not None:
				write(writer, block)
			else:
				writer.data += block["raw"]
			bodies.append(bytes(writer.data))
		types = list(dict.fromkeys(self.types + [block["type"] for block in self.blocks]))
		writer = Writer()
		writer.data += self.header_string.encode("latin-1") + b"\n"
		writer.pack("IBIII", self.version, 1, self.user_version, len(self.blocks), self.bs_version)
		for value in self.export_info:
			writer.string("B", value, terminated=True)
		writer.pack("H", len(types))
		for type in types:
			writer.string("I", type)
		for block in self.blocks:
			writer.pack("H", types.index(block["type"]))
		for body in bodies:
			writer.pack("I", len(body))
		writer.pack("II", len(strings), max((len(s) for s in strings), default=0))
		for value in strings:
			writer.string("I", value)
		writer.pack("I", 0)  # groups
		for body in bodies:
			writer.data += body
		writer.pack("I", len(self.roots))
		writer.pack(f"{len(self.roots)}i", *self.roots)
		if path is not None:
			Path(path).write_bytes(writer.data)
		return bytes(writer.data)

	# ---- NiObjectNET, NiAVObject, NiNode

	def object_net(self, reader, block):
		block["name"] = self.name(reader.one("i"))
		block["extra_data"] = list(reader.unpack(f"{reader.one('I')}i"))
		block["controller"] = reader.one("i")

	def write_object_net(self, writer, block):
		writer.pack("i", self.string_index(block.get("name")))
		extra = block.get("extra_data", [])
		writer.pack(f"I{len(extra)}i", len(extra), *extra)
		writer.pack("i", block.get("controller", -1))

	def av_object(self, reader, block):
		self.object_net(reader, block)
		block["flags"] = reader.one("I")
		block["translation"] = reader.unpack("3f")
		block["rotation"] = [reader.unpack("3f") for _ in range(3)]  # rows
		block["scale"] = reader.one("f")
		block["collision"] = reader.one("i")

	def write_av_object(self, writer, block):
		self.write_object_net(writer, block)
		writer.pack("I3f", block["flags"], *block["translation"])
		for row in block["rotation"]:
			writer.pack("3f", *row)
		writer.pack("fi", block["scale"], block.get("collision", -1))

	def parse_NiNode(self, reader, block):
		self.av_object(reader, block)
		block["children"] = list(reader.unpack(f"{reader.one('I')}i"))
		block["effects"] = list(reader.unpack(f"{reader.one('I')}i"))

	def write_NiNode(self, writer, block):
		self.write_av_object(writer, block)
		for refs in (block["children"], block.get("effects", [])):
			writer.pack(f"I{len(refs)}i", len(refs), *refs)

	parse_BSFadeNode = parse_BSLeafAnimNode = parse_NiNode
	write_BSFadeNode = write_BSLeafAnimNode = write_NiNode

	# ---- skinned meshes: BSTriShape, BSDismemberSkinInstance, NiSkinData,
	# NiSkinPartition (Skyrim SE keeps a skinned shape's vertices in its
	# partition, the shape's own data size being 0)

	def transform(self, reader):
		rotation = [reader.unpack("3f") for _ in range(3)]
		return {"rotation": rotation, "translation": reader.unpack("3f"), "scale": reader.one("f")}

	def write_transform(self, writer, transform):
		for row in transform["rotation"]:
			writer.pack("3f", *row)
		writer.pack("3ff", *transform["translation"], transform["scale"])

	def vertices(self, reader, desc, count):
		"""BSVertexDataSSE: the attributes vertex desc's flags (its bits 44
		up) say it has, in this order. Skyrim SE's positions are always
		floats (VF_FULL_PRECISION is Fallout 4's)."""
		flags = desc >> 44
		result = []
		for _ in range(count):
			vertex = {}
			if flags & VF_VERTEX:
				*vertex["position"], vertex["bitangent_x"] = reader.unpack("4f")
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

	def write_vertices(self, writer, desc, vertices):
		flags = desc >> 44
		def byte(value):
			return max(0, min(255, round((value + 1) * 127.5)))
		for vertex in vertices:
			if flags & VF_VERTEX:
				writer.pack("4f", *vertex["position"], vertex.get("bitangent_x", 0))
			if flags & VF_UV:
				writer.pack("2e", *vertex["uv"])
			if flags & VF_NORMAL:
				writer.pack("4B", *map(byte, (*vertex["normal"], vertex.get("bitangent_y", 0))))
			if flags & VF_TANGENT:
				writer.pack("4B", *map(byte, (*vertex["tangent"], vertex.get("bitangent_z", 0))))
			if flags & VF_COLORS:
				writer.pack("4B", *vertex.get("color", (255, 255, 255, 255)))
			if flags & VF_SKINNED:
				writer.pack("4e4B", *vertex["weights"], *vertex["bones"])
			if flags & VF_EYE_DATA:
				writer.pack("f", vertex.get("eye", 0))

	def parse_BSTriShape(self, reader, block):
		self.av_object(reader, block)
		block["bound"] = reader.unpack("4f")
		block["skin"], block["shader_property"], block["alpha_property"] = reader.unpack("3i")
		block["vertex_desc"] = reader.one("Q")
		triangle_count, vertex_count, data_size = reader.unpack("HHI")
		block["vertex_count"], block["triangle_count"] = vertex_count, triangle_count
		block["vertices"] = self.vertices(reader, block["vertex_desc"], vertex_count) if data_size else []
		block["triangles"] = [reader.unpack("3H") for _ in range(triangle_count)] if data_size else []
		block["particle_data_size"] = reader.one("I")  # Skyrim SE's only

	def write_BSTriShape(self, writer, block):
		self.write_av_object(writer, block)
		writer.pack("4f3iQ", *block["bound"], block["skin"], block["shader_property"], block["alpha_property"], block["vertex_desc"])
		vertices, triangles = block.get("vertices", []), block.get("triangles", [])
		data_size = (block["vertex_desc"] & 0xF) * 4 * len(vertices) + 6 * len(triangles)
		writer.pack("HHI", block.get("triangle_count", len(triangles)), block.get("vertex_count", len(vertices)), data_size)
		if data_size:
			self.write_vertices(writer, block["vertex_desc"], vertices)
			for triangle in triangles:
				writer.pack("3H", *triangle)
		writer.pack("I", block.get("particle_data_size", 0))

	def parse_BSDismemberSkinInstance(self, reader, block):
		block["data"], block["partition"], block["skeleton_root"] = reader.unpack("3i")
		block["bones"] = list(reader.unpack(f"{reader.one('I')}i"))
		block["partitions"] = [reader.unpack("HH") for _ in range(reader.one("I"))]  # flags, body part

	def write_BSDismemberSkinInstance(self, writer, block):
		bones, partitions = block["bones"], block["partitions"]
		writer.pack(f"3iI{len(bones)}iI", block["data"], block["partition"], block["skeleton_root"], len(bones), *bones, len(partitions))
		for flags, part in partitions:
			writer.pack("HH", flags, part)

	def parse_NiSkinData(self, reader, block):
		block["skin_transform"] = self.transform(reader)
		count, block["has_weights"] = reader.unpack("IB")
		block["bones"] = []
		for _ in range(count):
			bone = {"transform": self.transform(reader), "bound": reader.unpack("4f")}
			weights = reader.one("H")
			bone["weights"] = [reader.unpack("Hf") for _ in range(weights)] if block["has_weights"] else []
			block["bones"].append(bone)

	def write_NiSkinData(self, writer, block):
		self.write_transform(writer, block["skin_transform"])
		writer.pack("IB", len(block["bones"]), block.get("has_weights", 1))
		for bone in block["bones"]:
			self.write_transform(writer, bone["transform"])
			writer.pack("4fH", *bone["bound"], len(bone["weights"]))
			if block.get("has_weights", 1):
				for index, weight in bone["weights"]:
					writer.pack("Hf", index, weight)

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
			partition = {"vertex_count": vertices, "weights_per_vertex": weights_per_vertex, "bones": list(reader.unpack(f"{bones}H"))}
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

	def write_NiSkinPartition(self, writer, block):
		desc, vertices = block["vertex_desc"], block["vertices"]
		vertex_size = (desc & 0xF) * 4
		writer.pack("IIIQ", len(block["partitions"]), vertex_size * len(vertices), vertex_size, desc)
		self.write_vertices(writer, desc, vertices)
		for partition in block["partitions"]:
			strips = partition.get("strips", [])
			copy = partition["triangles_copy"]
			per_vertex = partition["weights_per_vertex"]
			count = partition["vertex_count"]
			writer.pack("5H", count, len(copy), len(partition["bones"]), len(strips), per_vertex)
			writer.pack(f"{len(partition['bones'])}H", *partition["bones"])
			for key, fmt in (("vertex_map", "H"), ("vertex_weights", f"{per_vertex}f")):
				writer.pack("B", key in partition)
				for value in partition.get(key, []):
					writer.pack(fmt, *(value if isinstance(value, (tuple, list)) else (value,)))
			writer.pack(f"{len(strips)}H", *map(len, strips))
			faces = partition.get("strips") or partition.get("triangles")
			writer.pack("B", faces is not None)
			for face in faces or []:
				writer.pack(f"{len(face)}H", *face)
			writer.pack("B", "bone_indices" in partition)
			for indices in partition.get("bone_indices", []):
				writer.pack(f"{per_vertex}B", *indices)
			writer.pack("BBQ", partition.get("lod_level", 0), partition.get("global_vb", 1), partition.get("vertex_desc", desc))
			for triangle in copy:
				writer.pack("3H", *triangle)

	# ---- shading: BSLightingShaderProperty, BSShaderTextureSet

	def parse_BSLightingShaderProperty(self, reader, block):
		block["shader_type"] = reader.one("I")
		self.object_net(reader, block)
		block["flags1"], block["flags2"] = reader.unpack("II")
		block["uv_offset"], block["uv_scale"] = reader.unpack("2f"), reader.unpack("2f")
		block["texture_set"] = reader.one("i")
		block["emissive_color"], block["emissive_multiple"] = reader.unpack("3f"), reader.one("f")
		block["texture_clamp_mode"], block["alpha"], block["refraction_strength"], block["glossiness"] = reader.unpack("Ifff")
		block["specular_color"], block["specular_strength"] = reader.unpack("3f"), reader.one("f")
		block["lighting_effects"] = reader.unpack("2f")
		block["type_floats"] = reader.unpack(f"{SHADER_TYPE_FLOATS.get(block['shader_type'], 0)}f")

	def write_BSLightingShaderProperty(self, writer, block):
		writer.pack("I", block["shader_type"])
		self.write_object_net(writer, block)
		writer.pack("II2f2fi", block["flags1"], block["flags2"], *block["uv_offset"], *block["uv_scale"], block["texture_set"])
		writer.pack("3ff", *block["emissive_color"], block["emissive_multiple"])
		writer.pack("Ifff", block["texture_clamp_mode"], block["alpha"], block["refraction_strength"], block["glossiness"])
		writer.pack("3ff2f", *block["specular_color"], block["specular_strength"], *block["lighting_effects"])
		floats = block["type_floats"]
		if len(floats) != SHADER_TYPE_FLOATS.get(block["shader_type"], 0):
			raise ValueError(f"shader type {block['shader_type']} takes {SHADER_TYPE_FLOATS.get(block['shader_type'], 0)} floats")
		writer.pack(f"{len(floats)}f", *floats)

	def parse_BSShaderTextureSet(self, reader, block):
		block["textures"] = [reader.string("I") for _ in range(reader.one("I"))]

	def write_BSShaderTextureSet(self, writer, block):
		writer.pack("I", len(block["textures"]))
		for texture in block["textures"]:
			writer.string("I", texture)

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
	parser.add_argument("--check", action="store_true", help="check what is read writes back byte for byte")
	args = parser.parse_args()
	nif = Nif(args.nif)
	print(f"{args.nif}: {len(nif.blocks)} blocks, Bethesda version {nif.bs_version}, {len(nif.strings)} strings")
	if args.check:
		original = Path(args.nif).read_bytes()
		written = nif.write()
		for index, block in enumerate(nif.blocks):
			if "write_" + block["type"] in dir(nif):
				writer = Writer()
				getattr(nif, "write_" + block["type"])(writer, block)
				if bytes(writer.data) != block["raw"]:
					at = next((i for i, (a, b) in enumerate(zip(writer.data, block["raw"])) if a != b), min(len(writer.data), len(block["raw"])))
					print(f"  block {index} {block['type']}: written differs at byte {at} ({len(writer.data)} bytes written, {len(block['raw'])} read)")
		print("  written back byte for byte" if written == original else f"  written back differs ({len(written)} bytes, {len(original)} read)")
		return 0 if written == original else 1
	if args.tree:
		for depth, index, block, _ in nif.walk():
			extra = ""
			if "translation" in block:
				extra = " t=(" + ", ".join(f"{v:.3f}" for v in block["translation"]) + f") s={block['scale']:.3f}"
			print(f"{'  ' * depth}{index} {block['type']} {block.get('name') or ''}{extra}")
	else:
		for index, block in enumerate(nif.blocks):
			print(index, block["type"], block.get("name") or "")


if __name__ == "__main__":
	sys.exit(main())
