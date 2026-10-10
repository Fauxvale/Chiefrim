#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reads a model (mode) and its bitmaps out of the user's own Xbox Halo cache
maps, for the Mjolnir armor port. Nothing of Halo's is in the repo: this
writes what it reads to build/mjolnir/, which git ignores.

The layouts are OpenCE's (halo/.work/source): model_definitions.h, the
parts of port/linux/game/tag_schema_models.c, rasterizer_model_types.h and
bitmap_group.h. On Xbox a part's vertices are in the tag data, at the Data
address of the D3DVertexBuffer its vertex_buffer.hardware_format names; its
indices are a triangle strip of triangle_buffer.count + 2 indices at
triangle_buffer.base_address, with repeated indices between the strips.
A bitmap's pixels_offset is where its pixels are in the (decompressed)
map: the game adds pixel_data.file_offset to it as it loads it
(xbox_texture_cache.c), and that is 0 in a map.

Usage: tools/mjolnir/halo_model.py MAP.map [--model characters\\cyborg\\cyborg]
         [--lod 0] [--out build/mjolnir]
Writes model.json (nodes, shaders, then per part its vertices and
triangles) and, per model shader, its colour, glow and reflection mask
baked as the Xbox draws them (bake()), and its reflection cube map's faces
side by side (+x, -x, +y, -y, +z, -z), as PNGs.
"""
import argparse
import json
import struct
import sys
import zlib
from pathlib import Path


class CacheMap:
	def __init__(self, path):
		raw = Path(path).read_bytes()
		if raw[:4] != b"daeh":
			raise ValueError(f"{path}: not a Halo cache map")
		version, _, _, self.tag_offset, _ = struct.unpack_from("<5I", raw, 4)
		self.data = raw[:0x800] + zlib.decompress(raw[0x800:]) if version == 5 else raw
		instances, _, _, count = struct.unpack_from("<4I", self.data, self.tag_offset)
		self.base = instances - 0x24
		self.tags = {}
		self.names = set()
		for index in range(count):
			cls, _, _, _, name, address = struct.unpack_from("<4sIIIII", self.data, self.offset(instances) + 32 * index)
			self.tags[(cls[::-1].decode("latin-1"), self.string(name))] = address
			self.names.add(name)

	def offset(self, address):
		return self.tag_offset + address - self.base

	def string(self, address):
		start = self.offset(address)
		return self.data[start:self.data.index(b"\0", start)].decode("latin-1")

	def unpack(self, fmt, address, delta=0):
		return struct.unpack_from(fmt, self.data, self.offset(address) + delta)

	def block(self, address, delta, size):
		"""The addresses of a tag block's elements."""
		count, first, _ = self.unpack("<iII", address, delta)
		return [first + size * index for index in range(count)]

	def tag(self, cls, name):
		try:
			return self.tags[(cls, name)]
		except KeyError:
			raise KeyError(f"no {cls} tag {name!r} in this map") from None


def name32(cache, address):
	raw = cache.data[cache.offset(address):cache.offset(address) + 32]
	return raw.split(b"\0")[0].decode("latin-1")


def uncompress_vector(packed):
	"""rasterizer_geometry.c's uncompress_int32_to_real_vector3d: 11, 11 and
	10 signed bits."""
	def signed(value, bits):
		return value - (1 << bits) if value & (1 << (bits - 1)) else value
	i = signed(packed & 0x7FF, 11)
	j = signed((packed >> 11) & 0x7FF, 11)
	k = signed(packed >> 22, 10)
	return [(2 * i + 1) / 2047, (2 * j + 1) / 2047, (2 * k + 1) / 1023]


def read_model(cache, name, lod):
	model = cache.tag("mode", name)
	nodes = []
	for address in cache.block(model, 0xB8, 0x9C):
		_, _, parent = cache.unpack("<3h", address, 32)
		nodes.append({
			"name": name32(cache, address),
			"parent": parent,
			"translation": list(cache.unpack("<3f", address, 40)),
			"rotation": list(cache.unpack("<4f", address, 52)),  # i j k w
		})
	shaders = []
	for address in cache.block(model, 0xDC, 0x20):
		cls, name_address, _, _ = cache.unpack("<4sIiI", address)
		shaders.append({"class": cls[::-1].decode("latin-1"), "name": cache.string(name_address)})
	# the base region's permutation names a geometry per level of detail,
	# super low first (model_region_permutation.geometry_indices); lod 0
	# here is super high
	region = cache.block(model, 0xC4, 0x4C)[0]
	permutation = cache.block(region, 64, 0x58)[0]
	geometry_index = cache.unpack("<5h", permutation, 64)[4 - lod]
	geometry = cache.block(model, 0xD0, 0x30)[geometry_index]
	parts = []
	for part in cache.block(geometry, 0x24, 0x68):
		shader_index, = cache.unpack("<h", part, 4)
		strip_type, _, strip_count, strip_address = cache.unpack("<hhiI", part, 0x44)
		_, _, vertex_count, _, _, hardware = cache.unpack("<hhiiII", part, 0x54)
		if strip_type != 1:
			raise ValueError(f"part with triangle buffer type {strip_type}, not a strip")
		_, vertex_address, _ = cache.unpack("<3I", hardware)
		vertices = []
		for index in range(vertex_count):
			x, y, z, normal, _, _, u, v, node0, node1, weight = cache.unpack("<3f3I2h2Bh", vertex_address, 32 * index)
			# weights are a short of 0 to 32767; a second node of 0xFD (-3 / 3)
			# is none
			w0 = weight / 32767
			vertices.append({
				"position": [x, y, z],
				"normal": uncompress_vector(normal),
				"uv": [(2 * u + 1) / 65535, (2 * v + 1) / 65535],
				"nodes": [node0 // 3, node1 // 3 if node1 != 0xFD else -1],
				"weights": [w0, 1 - w0 if node1 != 0xFD else 0],
			})
		strip = cache.unpack(f"<{strip_count + 2}H", strip_address)
		triangles = []
		for index in range(len(strip) - 2):
			a, b, c = strip[index:index + 3]
			if a == b or b == c or a == c:
				continue
			triangles.append([a, b, c] if index % 2 == 0 else [a, c, b])
		parts.append({"shader": shader_index, "vertices": vertices, "triangles": triangles})
	return {"name": name, "lod": lod, "nodes": nodes, "shaders": shaders, "parts": parts}


# ---- bitmaps

def dxt_colors(c0, c1, four):
	def rgb(c):
		return [((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31]
	a, b = rgb(c0), rgb(c1)
	if four or c0 > c1:
		return [a + [255], b + [255],
			[(2 * x + y) // 3 for x, y in zip(a, b)] + [255],
			[(x + 2 * y) // 3 for x, y in zip(a, b)] + [255]]
	return [a + [255], b + [255], [(x + y) // 2 for x, y in zip(a, b)] + [255], [0, 0, 0, 0]]


def decode_dxt(raw, width, height, format):
	"""DXT1 (14), DXT3 (15) and DXT5 (16) to RGBA rows. Xbox keeps these linear."""
	size = 8 if format == 14 else 16
	pixels = bytearray(width * height * 4)
	offset = 0
	for by in range(0, height, 4):
		for bx in range(0, width, 4):
			block = raw[offset:offset + size]
			offset += size
			alpha = None
			if format == 15:
				bits = int.from_bytes(block[:8], "little")
				alpha = [((bits >> (4 * i)) & 15) * 17 for i in range(16)]
			elif format == 16:
				a0, a1 = block[0], block[1]
				table = [a0, a1] + ([((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)] if a0 > a1
					else [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255])
				bits = int.from_bytes(block[2:8], "little")
				alpha = [table[(bits >> (3 * i)) & 7] for i in range(16)]
			c0, c1, bits = struct.unpack_from("<HHI", block, size - 8)
			colors = dxt_colors(c0, c1, format != 14)
			for i in range(16):
				x, y = bx + i % 4, by + i // 4
				if x < width and y < height:
					color = colors[(bits >> (2 * i)) & 3]
					p = 4 * (y * width + x)
					pixels[p:p + 3] = bytes(color[:3])
					pixels[p + 3] = color[3] if alpha is None else alpha[i]
	return pixels


def unswizzle(raw, width, height, depth):
	"""The Xbox's swizzled (Morton order) texels to rows."""
	out = bytearray(len(raw))
	# interleave the bits of x and y while both have bits left
	def index(x, y):
		result, shift, bitx, bity = 0, 0, 1, 1
		while bitx < width or bity < height:
			if bitx < width:
				result |= (1 << shift) if x & bitx else 0
				shift += 1
				bitx <<= 1
			if bity < height:
				result |= (1 << shift) if y & bity else 0
				shift += 1
				bity <<= 1
		return result
	for y in range(height):
		for x in range(width):
			s = index(x, y) * depth
			d = (y * width + x) * depth
			out[d:d + depth] = raw[s:s + depth]
	return out


def decode_bitmap(cache, name, index=0):
	group = cache.tag("bitm", name)
	bitmap = cache.block(group, 96, 0x30)[index]
	_, width, height, _, type, format, flags, _, _, _, _, start = cache.unpack("<I5hH2h2hi", bitmap)
	if format in (14, 15, 16):
		raw = cache.data[start:start + max(1, width // 4) * max(1, height // 4) * (8 if format == 14 else 16)]
		return width, height, decode_dxt(raw, width, height, format)
	depth = {0: 1, 1: 1, 2: 1, 3: 2, 6: 2, 8: 2, 9: 2, 10: 4, 11: 4}.get(format)
	if depth is None:
		raise ValueError(f"{name}: bitmap format {format} not handled")
	raw = cache.data[start:start + width * height * depth]
	if flags & (1 << 3):
		raw = unswizzle(raw, width, height, depth)
	pixels = bytearray(width * height * 4)
	for p in range(width * height):
		if depth == 4:
			b, g, r, a = raw[4 * p:4 * p + 4]
			pixels[4 * p:4 * p + 4] = bytes([r, g, b, a if format == 11 else 255])
		elif depth == 1:
			value = raw[p]
			pixels[4 * p:4 * p + 4] = bytes([255, 255, 255, value] if format == 0  # a8
				else [value, value, value, value if format == 2 else 255])  # ay8, y8
		else:
			c, = struct.unpack_from("<H", raw, 2 * p)
			if format == 6:
				pixels[4 * p:4 * p + 4] = bytes([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31, 255])
			elif format == 3:
				pixels[4 * p:4 * p + 4] = bytes([c & 255, c & 255, c & 255, c >> 8])
			elif format == 8:
				pixels[4 * p:4 * p + 4] = bytes([((c >> 10) & 31) * 255 // 31, ((c >> 5) & 31) * 255 // 31, (c & 31) * 255 // 31, 255 if c >> 15 else 0])
			else:
				pixels[4 * p:4 * p + 4] = bytes([((c >> s) & 15) * 17 for s in (8, 4, 0, 12)])
	return width, height, pixels


def decode_cube(cache, name, index=0):
	"""A cube map's six faces (+x, -x, +y, -y, +z, -z, in Halo's frame), as
	RGBA rows. On Xbox each face is its mip chain, padded to 128 bytes."""
	group = cache.tag("bitm", name)
	bitmap = cache.block(group, 96, 0x30)[index]
	_, width, height, _, type, format, flags, _, _, mipmaps, _, start = cache.unpack("<I5hH2h2hi", bitmap)
	if type != 2 or format not in (14, 15, 16):
		raise ValueError(f"{name}: not a DXT cube map (type {type}, format {format})")
	block = 8 if format == 14 else 16
	levels = [max(1, (width >> n) // 4) * max(1, (height >> n) // 4) * block for n in range(mipmaps + 1)]
	face = (sum(levels) + 127) // 128 * 128
	return width, height, [decode_dxt(cache.data[start + f * face:start + f * face + levels[0]], width, height, format) for f in range(6)]


def write_png(path, width, height, rgba):
	def chunk(kind, body):
		return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
	rows = b"".join(b"\0" + bytes(rgba[4 * width * y:4 * width * (y + 1)]) for y in range(height))
	Path(path).write_bytes(b"\x89PNG\r\n\x1a\n"
		+ chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
		+ chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def reference(cache, address, delta):
	"""A tag reference's name, or None. Cache maps zero a reference's name
	length; its name address and tag id are kept (NONE when it is empty)."""
	_, name_address, _, tag_id = cache.unpack("<4sIiI", address, delta)
	if tag_id == 0xFFFFFFFF or name_address not in cache.names:
		return None
	return cache.string(name_address)


def model_shader(cache, name):
	"""A soso's fields that say how it colours a model (OpenCE's
	rasterizer_xbox_models.c, shader_model_properties after the shader's
	0x28-byte header)."""
	address = cache.tag("soso", name)
	lower = cache.unpack("<3f", address, 0x78)
	upper = cache.unpack("<3f", address, 0x84)
	detail_function, detail_mask, detail_scale = cache.unpack("<hhf", address, 0xD4)
	return {
		"change_color_source": cache.unpack("<h", address, 0x4C)[0],  # 0 none, 1 to 4 A to D
		"self_illumination_color_source": cache.unpack("<h", address, 0x70)[0],
		"self_illumination_color": [lower, upper],
		"base_map": reference(cache, address, 0xA4),
		"multipurpose_map": reference(cache, address, 0xBC),
		"detail_function": detail_function,  # 0 biased multiply, 1 multiply, 2 biased add
		"detail_mask": detail_mask,
		"detail_scale": [detail_scale, detail_scale * cache.unpack("<f", address, 0xEC)[0]],
		"detail_map": reference(cache, address, 0xDC),
		"reflection_cube_map": reference(cache, address, 0x164),
	}


def object_change_colors(cache, cls, name):
	"""The change colours an object of this tag is created with
	(objects.c's object_choose_random_change_colors): a colour from the
	first permutation the object's position chooses, between its bounds, or
	white. Chief's campaign colour is one permutation whose bounds are the
	same colour, so the position doesn't matter; a colour with no
	permutations is driven by a function (the shield's) and is None here."""
	colors = []
	for definition in cache.block(cache.tag(cls, name), 356, 44):
		permutations = cache.block(definition, 32, 28)
		if not permutations:
			colors.append(None)
			continue
		_, *bounds = cache.unpack("<7f", permutations[0])
		lower, upper = bounds[:3], bounds[3:]
		if lower != upper:
			print(f"  {name}: change colour {'ABCD'[len(colors)]} varies with position; taking its lower bound", file=sys.stderr)
		colors.append(lower)
	return colors + [None] * (4 - len(colors))


# ---- the diffuse a model shader draws, baked

def rgba_array(width, height, rgba):
	import numpy
	return numpy.frombuffer(bytes(rgba), numpy.uint8).reshape(height, width, 4).astype(numpy.float64) / 255


def sample(image, size, scale=(1, 1)):
	"""image bilinearly sampled, wrapping, at size x size texel centres of
	the base map's [0, 1) times scale."""
	import numpy
	height, width = image.shape[:2]
	centres = (numpy.arange(size) + 0.5) / size
	u = (centres * scale[0] * width - 0.5)[None, :].repeat(size, 0)
	v = (centres * scale[1] * height - 0.5)[:, None].repeat(size, 1)
	u0, v0 = numpy.floor(u).astype(int), numpy.floor(v).astype(int)
	fu, fv = (u - u0)[..., None], (v - v0)[..., None]
	def at(y, x):
		return image[y % height, x % width]
	return ((at(v0, u0) * (1 - fu) + at(v0, u0 + 1) * fu) * (1 - fv)
		+ (at(v0 + 1, u0) * (1 - fu) + at(v0 + 1, u0 + 1) * fu) * fv)


def bake(shader, bitmaps, change_colors, size):
	"""The colour, glow and reflection mask of a model shader, per texel,
	as the Xbox's combiners make them (rasterizer_xbox_models.c's
	set_environment_shader_pixel_shader, 8 stages; an alpha input with no
	channel named reads blue). With m the multipurpose map:

	  detail  = lerp(1/2, detail map, mask)      (stage 3; mask 2 is m.r)
	  base   := 2 * base * detail, clamped       (stage 6, biased multiply)
	  colour  = base * lerp(1, change colour, m.b) * (light + m.g * self-illumination)
	  plus      cube map * m.r * reflection      (stages 4 and 7)

	all on the textures' gamma-encoded values, as the Xbox did and as
	Skyrim does: so the bake is in them too, with the change colour as the
	hardware's 8 bits (real_rgb_color_to_pixel32). colour is the albedo
	(light is the engine's); glow is base * m.g * self-illumination."""
	import numpy
	base = sample(bitmaps[shader["base_map"]], size)
	multi = sample(bitmaps[shader["multipurpose_map"]], size) if shader["multipurpose_map"] else None
	if shader["detail_map"] and multi is not None:
		detail = sample(bitmaps[shader["detail_map"]], size, shader["detail_scale"])[..., :3]
		mask = {0: numpy.ones_like(multi[..., :1]), 1: 1 - multi[..., 0:1], 2: multi[..., 0:1]}.get(shader["detail_mask"])
		if mask is None:
			print(f"  detail mask {shader['detail_mask']} not handled; no detail", file=sys.stderr)
			mask = numpy.zeros_like(multi[..., :1])
		if shader["detail_function"] == 0:
			albedo = numpy.clip(2 * base[..., :3] * (0.5 + (detail - 0.5) * mask), 0, 1)
		elif shader["detail_function"] == 1:
			albedo = base[..., :3] * (1 + (detail - 1) * mask)
		else:
			albedo = numpy.clip(base[..., :3] + (2 * detail - 1) * mask, 0, 1)
	else:
		albedo = base[..., :3]
	source = shader["change_color_source"]
	color = change_colors[source - 1] if 0 < source <= 4 else None
	if color is not None and multi is not None:
		color = numpy.round(numpy.clip(color, 0, 1) * 255) / 255
		albedo = albedo * (1 + (color - 1) * multi[..., 2:3])
	lower, upper = (numpy.array(c) for c in shader["self_illumination_color"])
	glow = albedo * 0 if multi is None else numpy.clip(base[..., :3] * multi[..., 1:2] * lower, 0, 1)
	reflection = numpy.zeros((size, size, 1)) if multi is None else multi[..., 0:1]
	def pixels(image):
		image = numpy.round(numpy.clip(image, 0, 1) * 255).astype(numpy.uint8)
		if image.shape[2] == 1:
			image = image.repeat(3, 2)
		return numpy.concatenate([image, numpy.full((size, size, 1), 255, numpy.uint8)], 2).tobytes()
	return pixels(albedo), pixels(glow), pixels(reflection)


def main():
	parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
	parser.add_argument("map")
	parser.add_argument("--model", default="characters\\cyborg\\cyborg")
	parser.add_argument("--object", default="bipd:characters\\cyborg\\cyborg",
		help="the object whose change colours colour the model (class:name)")
	parser.add_argument("--lod", type=int, default=0)
	parser.add_argument("--size", type=int, default=2048, help="the baked textures' size")
	parser.add_argument("--out", default=str(Path(__file__).resolve().parents[2] / "build" / "mjolnir"))
	args = parser.parse_args()
	out = Path(args.out)
	out.mkdir(parents=True, exist_ok=True)
	cache = CacheMap(args.map)
	model = read_model(cache, args.model, args.lod)
	cls, name = args.object.split(":", 1)
	model["change_colors"] = object_change_colors(cache, cls, name)
	for shader in model["shaders"]:
		shader["bitmaps"] = []
		if shader["class"] != "soso":
			continue
		shader.update(model_shader(cache, shader["name"]))
		bitmaps = {}
		for key in ("base_map", "multipurpose_map", "detail_map"):
			if shader[key] and shader[key] not in bitmaps:
				width, height, rgba = decode_bitmap(cache, shader[key])
				bitmaps[shader[key]] = rgba_array(width, height, rgba)
				shader["bitmaps"].append({"name": shader[key], "size": [width, height]})
		stem = Path(shader["name"].replace("\\", "/")).name.replace(" ", "_")
		shader["baked"] = {}
		for kind, rgba in zip(("color", "glow", "reflection"), bake(shader, bitmaps, model["change_colors"], args.size)):
			shader["baked"][kind] = f"{stem}_{kind}.png"
			write_png(out / shader["baked"][kind], args.size, args.size, rgba)
		if shader["reflection_cube_map"]:
			# its faces side by side
			width, height, faces = decode_cube(cache, shader["reflection_cube_map"])
			strip = b"".join(face[4 * width * y:4 * width * (y + 1)] for y in range(height) for face in faces)
			shader["baked"]["cube"] = f"{stem}_cube.png"
			write_png(out / shader["baked"]["cube"], 6 * width, height, strip)
	(out / "model.json").write_text(json.dumps(model))
	vertices = sum(len(part["vertices"]) for part in model["parts"])
	triangles = sum(len(part["triangles"]) for part in model["parts"])
	print(f"{args.model} lod {args.lod}: {len(model['nodes'])} nodes, {len(model['parts'])} parts, "
		f"{vertices} vertices, {triangles} triangles -> {out}")
	for index, color in enumerate(model["change_colors"]):
		if color is not None:
			print(f"  change colour {'ABCD'[index]}: {', '.join(f'{c:.4f}' for c in color)}"
				f" ({', '.join(str(round(c * 255)) for c in color)})")
	for shader in model["shaders"]:
		if shader.get("baked"):
			print(f"  {shader['name']}: change colour {' ABCD'[shader['change_color_source']].strip() or 'none'},"
				f" from {', '.join(b['name'] for b in shader['bitmaps'])} -> {', '.join(shader['baked'].values())}")


if __name__ == "__main__":
	sys.exit(main())
