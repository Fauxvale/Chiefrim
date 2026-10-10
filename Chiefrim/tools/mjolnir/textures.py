#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes the textures armor.py's NIFs name, as DDS, under
build/mjolnir/Data/textures/chiefrim/mjolnir/, from what halo_model.py
baked (build/mjolnir/*.png), for the Mjolnir armor port. Per shader (armor,
visor):

  NAME.dds    the baked colour, as Skyrim's diffuse
  NAME_n.dds  a flat normal map (Halo's model shaders have no bump map);
              its alpha, Skyrim's specular mask, is where Halo reflects
  NAME_m.dds  the environment mask: where Halo reflects (multipurpose red)
  NAME_e.dds  Halo's reflection cube map, turned into Skyrim's frame

Halo adds its cube map times the mask times a tint lerped from the shader's
perpendicular to its parallel colour as the surface turns from the eye;
Skyrim's environment map is its cube map times its mask. Chief's armor's
tints are white; his visor's perpendicular one is, so the cube maps are as
Halo's are (his visor's is gold), seen face on.

They are uncompressed (8 bits a channel, BGRA), so the colours are exactly
the bake's, with mipmaps (box filtered, in the textures' gamma-encoded
values, as the bake is).

Usage: tools/mjolnir/textures.py [--dir build/mjolnir] [--mask-size 512]
"""
import argparse
import json
import struct
import sys
import zlib
from pathlib import Path

import numpy

sys.path.insert(0, str(Path(__file__).resolve().parent))
from armor import SHADERS, TEXTURES  # noqa: E402

FLAT_NORMAL = (128, 128, 255)


def read_png(path):
	"""An 8-bit RGBA or RGB PNG (as halo_model.py and Blender write them),
	as a height x width x 4 uint8 array."""
	data = Path(path).read_bytes()
	if data[:8] != b"\x89PNG\r\n\x1a\n":
		raise ValueError(f"{path}: not a PNG")
	position, idat = 8, b""
	while position < len(data):
		length, kind = struct.unpack_from(">I4s", data, position)
		body = data[position + 8:position + 8 + length]
		if kind == b"IHDR":
			width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
			if depth != 8 or colour not in (2, 6) or interlace:
				raise ValueError(f"{path}: only 8-bit RGB(A), not interlaced, is read")
		elif kind == b"IDAT":
			idat += body
		position += 12 + length
	channels = 4 if colour == 6 else 3
	raw = numpy.frombuffer(zlib.decompress(idat), numpy.uint8).reshape(height, 1 + width * channels)
	rows = numpy.zeros((height, width * channels), numpy.int32)
	for y in range(height):
		kind, line = raw[y, 0], raw[y, 1:].astype(numpy.int32)
		above = rows[y - 1] if y else numpy.zeros_like(line)
		if kind == 0:
			rows[y] = line
		elif kind == 2:
			rows[y] = (line + above) & 255
		elif kind in (1, 3, 4):
			# these depend on the row's own decoded bytes to the left
			out = numpy.zeros_like(line)
			for x in range(len(line)):
				left = out[x - channels] if x >= channels else 0
				up_left = above[x - channels] if x >= channels else 0
				if kind == 1:
					predictor = left
				elif kind == 3:
					predictor = (left + above[x]) // 2
				else:
					p = left + above[x] - up_left
					pa, pb, pc = abs(p - left), abs(p - above[x]), abs(p - up_left)
					predictor = left if pa <= pb and pa <= pc else above[x] if pb <= pc else up_left
				out[x] = (line[x] + predictor) & 255
			rows[y] = out
		else:
			raise ValueError(f"{path}: filter {kind}")
	image = rows.astype(numpy.uint8).reshape(height, width, channels)
	if channels == 3:
		image = numpy.concatenate([image, numpy.full((height, width, 1), 255, numpy.uint8)], 2)
	return image


def downsample(image, size):
	"""image (square, a multiple of size) box filtered down to size."""
	factor = image.shape[0] // size
	return image.reshape(size, factor, size, factor, -1).astype(numpy.float64).mean(axis=(1, 3))


def mipmaps(image):
	"""image (a power of two) and its mipmaps, each box filtered from the
	one before, down to 1 x 1."""
	levels = [image.astype(numpy.float64)]
	while levels[-1].shape[0] > 1 or levels[-1].shape[1] > 1:
		level = levels[-1]
		height, width = max(1, level.shape[0] // 2), max(1, level.shape[1] // 2)
		level = level.reshape(height, level.shape[0] // height, width, level.shape[1] // width, -1).mean(axis=(1, 3))
		levels.append(level)
	return [numpy.clip(numpy.round(level), 0, 255).astype(numpy.uint8) for level in levels]


def write_dds(path, faces):
	"""Uncompressed 32-bit BGRA DDS with mipmaps: faces is one RGBA image,
	or a cube map's six (+x, -x, +y, -y, +z, -z)."""
	height, width = faces[0].shape[:2]
	chains = [mipmaps(face) for face in faces]
	cube = len(faces) == 6
	DDSD = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | 0x20000  # caps, height, width, pitch, pixel format, mipmap count
	DDPF_ALPHAPIXELS, DDPF_RGB = 0x1, 0x40
	caps = 0x1000 | 0x400000 | 0x8  # texture, mipmap, complex
	caps2 = 0x200 | 0xFC00 if cube else 0  # cube map, all six faces
	header = struct.pack("<4s7I44x", b"DDS ", 124, DDSD, height, width, width * 4, 0, len(chains[0]))
	header += struct.pack("<2I4s5I", 32, DDPF_RGB | DDPF_ALPHAPIXELS, b"\0\0\0\0", 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
	header += struct.pack("<4I4x", caps, caps2, 0, 0)
	assert len(header) == 128
	body = b"".join(level[..., [2, 1, 0, 3]].tobytes() for chain in chains for level in chain)
	path.parent.mkdir(parents=True, exist_ok=True)
	path.write_bytes(header + body)
	return path


# a cube face's texel to its direction, as Direct3D (both games) samples
# them: per face, the major axis and the axes its s and t run along
CUBE_FACES = [
	((1, 0, 0), (0, 0, -1), (0, -1, 0)),
	((-1, 0, 0), (0, 0, 1), (0, -1, 0)),
	((0, 1, 0), (1, 0, 0), (0, 0, 1)),
	((0, -1, 0), (1, 0, 0), (0, 0, -1)),
	((0, 0, 1), (1, 0, 0), (0, -1, 0)),
	((0, 0, -1), (-1, 0, 0), (0, -1, 0)),
]
# a Skyrim direction (+x right, +y forward, +z up) in Halo's (+x forward,
# +y left, +z up)
SKYRIM_TO_HALO = numpy.array([[0, 1, 0], [-1, 0, 0], [0, 0, 1]])


def turn_cube(faces):
	"""A Halo cube map's faces as Skyrim's: each texel the one Halo's cube
	shows in the same direction in the world. (Skyrim's frame is Halo's
	turned a quarter about up, so each texel is one of Halo's.)"""
	size = faces[0].shape[0]
	centres = (numpy.arange(size) + 0.5) / size * 2 - 1
	s, t = numpy.meshgrid(centres, centres)
	result = []
	for major, s_axis, t_axis in CUBE_FACES:
		direction = numpy.array(major)[:, None, None] + numpy.array(s_axis)[:, None, None] * s + numpy.array(t_axis)[:, None, None] * t
		halo = numpy.tensordot(SKYRIM_TO_HALO, direction, 1)
		axis = numpy.abs(halo).argmax(axis=0)
		face_index = axis * 2 + (numpy.take_along_axis(halo, axis[None], 0)[0] < 0)
		out = numpy.zeros_like(faces[0])
		for index, (major2, s2, t2) in enumerate(CUBE_FACES):
			mask = face_index == index
			if not mask.any():
				continue
			d = halo[:, mask]
			ma = numpy.abs(numpy.dot(major2, d))
			u = (numpy.dot(s2, d) / ma + 1) / 2 * size - 0.5
			v = (numpy.dot(t2, d) / ma + 1) / 2 * size - 0.5
			x, y = numpy.clip(numpy.round(u).astype(int), 0, size - 1), numpy.clip(numpy.round(v).astype(int), 0, size - 1)
			out[mask] = faces[index][y, x]
		result.append(out)
	return result


def main():
	parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
	parser.add_argument("--dir", default=str(Path(__file__).resolve().parents[2] / "build" / "mjolnir"))
	parser.add_argument("--mask-size", type=int, default=512, help="the normal and environment masks' size (Halo's multipurpose map's is 512)")
	args = parser.parse_args()
	directory = Path(args.dir)
	model = json.loads((directory / "model.json").read_text())
	out = directory / "Data" / Path(TEXTURES.replace("\\", "/"))
	for shader in model["shaders"]:
		name = Path(shader["name"].replace("\\", "/")).name
		if name not in SHADERS:
			continue
		baked = {kind: read_png(directory / file) for kind, file in shader["baked"].items()}
		base = out / SHADERS[name]
		written = [write_dds(base.with_name(base.name + ".dds"), [baked["color"]])]
		mask = numpy.clip(numpy.round(downsample(baked["reflection"], args.mask_size)), 0, 255).astype(numpy.uint8)
		normal = numpy.empty_like(mask)
		normal[..., :3] = FLAT_NORMAL
		normal[..., 3] = mask[..., 0]
		written.append(write_dds(base.with_name(base.name + "_n.dds"), [normal]))
		mask[..., 3] = 255
		written.append(write_dds(base.with_name(base.name + "_m.dds"), [mask]))
		strip = baked["cube"]
		size = strip.shape[0]
		faces = [strip[:, i * size:(i + 1) * size] for i in range(6)]
		written.append(write_dds(base.with_name(base.name + "_e.dds"), turn_cube(faces)))
		for path in written:
			print(f"wrote {path} ({path.stat().st_size} bytes)")


if __name__ == "__main__":
	sys.exit(main())
