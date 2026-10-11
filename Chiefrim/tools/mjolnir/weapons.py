#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes Halo's weapons as Skyrim items' world models (what lies on the
ground and turns in the inventory; docs §8.4's loot rework), from the user's
own Halo map and Skyrim: per weapon, build/mjolnir/Data/meshes/weapons/
chiefrim/<key>.nif and its textures under textures/weapons/chiefrim/<key>/.
plugin.py makes each a WEAP whose world model this is; equipping one gives
Chief the Halo weapon (skse/src/Arsenal.cpp).

Each is the weapon's world model (its weap's model, lod 0: the one Halo
draws lying on the ground) turned into Skyrim's frame and units, one
unskinned shape per Halo shader, upright on the ground (z 0), centred on
the origin across it, its muzzle to +y, as Skyrim's weapons point their blades:
- a model shader (soso) as the armor's are (halo_model.bake, then as
  textures.py writes them): its colour, a flat normal map whose alpha is the
  specular mask, Halo's cube map and where Halo reflects it. The self-
  illumination (the needler's, a small part) is left out: Skyrim's lighting
  shader has an environment map or a glow map, not both;
- a transparent chicago shader (schi: the needler's needles) as its first
  map in Skyrim's effect shader, added onto what's behind it as Halo adds
  it (as Skyrim's own glowing crystals are: the Dwarven staff's), both
  sides drawn.
Its collision is a convex hull (ground.py's 26-sided bounding polyhedron) on
Skyrim's Iron Dagger's rigid body (the weapon layer, its material), made
heavier and duller than a dagger's (BODY): a gun lands and stays, it
doesn't skitter.

Usage: tools/mjolnir/weapons.py HALO_MAP [--skyrim DATA_DIR] [--dir build/mjolnir]
"""
import argparse
import struct
import sys
from pathlib import Path

import numpy

sys.path.insert(0, str(Path(__file__).resolve().parent))
from armor import ARMOR_FLAGS, CHIEF_VERTEX, ENVIRONMENT_MAP, SLSF1_ENVIRONMENT_MAPPING, bound, chief_vertices, rows  # noqa: E402
from bsa import Archive  # noqa: E402
from fit import DEFAULT_SKYRIM, HALO_TO_SKYRIM  # noqa: E402
from ground import BSX, FLAGS, GROUND_VERTEX, SLSF1_SKINNED, dop_hull  # noqa: E402
from halo_model import CacheMap, bake, decode_bitmap, decode_cube, read_model, reference, model_shader, rgba_array  # noqa: E402
from nif import HAVOK_SCALE, Nif  # noqa: E402
from textures import FLAT_NORMAL, turn_cube, write_dds  # noqa: E402

MESHES = "meshes\\weapons\\chiefrim"
TEXTURES = "textures\\weapons\\chiefrim"
TEMPLATE = "meshes\\weapons\\iron\\irondagger.nif"
# per weapon: its key (files, editor IDs), its weap tag, its name in
# Skyrim, its form ID in the plugin (skse/src/Arsenal.cpp's table has the
# same; its inventory model's is INVENTORY_ID past it), its value and weight
# (a one-handed gun as heavy as a sword)
WEAPONS = [
	("needler", "weapons\\needler\\needler", "Needler", 0x900, 300, 10.0),
]
INVENTORY_ID = 0x80
# the rigid body's, over the dagger's: twice a Skyrim weapon's mass, little
# bounce, more grip, its tumbling damped
BODY = {"mass": 20.0, "restitution": 0.1, "friction": 0.8, "linear_damping": 0.2, "angular_damping": 0.6}
# Skyrim's effect shader as its glowing crystals have it (the Dwarven
# staff's): z-buffer test, both sides drawn; blended added (src alpha, one,
# the sorter off)
EFFECT_FLAGS = (0x80000000, 0x10)
ADDITIVE = 0x100D
# the inventory's turn (milliradians about x, y, z): its side to the eye
INVENTORY_TURN = (0, 0, 1571)
# Skyrim's lighting shader's: no environment map, not skinned
PLAIN_FLAGS = (ARMOR_FLAGS[0] & ~SLSF1_ENVIRONMENT_MAPPING & ~SLSF1_SKINNED, ARMOR_FLAGS[1])
DEFAULT_SHADER = 0
# shader_transparent_chicago, after the shader's 0x28-byte header: its
# maps block, each map 0xDC bytes, its bitmap's reference 0x6C in
CHICAGO_MAPS, CHICAGO_MAP_SIZE, CHICAGO_MAP_BITMAP = 0x28 + 0x2C, 0xDC, 0x6C


def weapon_model(cache, weapon):
	"""The weap's model's tag name (its object's model reference, 0x28 in;
	on Xbox a mode)."""
	name = reference(cache, cache.tag("weap", weapon), 0x28)
	if name is None:
		raise SystemExit(f"{weapon}: no model")
	return name


def lighting_shader(type, flags, textures):
	shader = {"type": "BSLightingShaderProperty", "shader_type": type, "name": None, "extra_data": [], "controller": -1,
		"flags1": flags[0], "flags2": flags[1], "uv_offset": (0.0, 0.0), "uv_scale": (1.0, 1.0),
		"emissive_color": (0.0, 0.0, 0.0), "emissive_multiple": 1.0, "texture_clamp_mode": 3, "alpha": 1.0,
		"refraction_strength": 0.0, "glossiness": 80.0, "specular_color": (1.0, 1.0, 1.0), "specular_strength": 1.0,
		"lighting_effects": (0.3, 2.0), "type_floats": (1.0,) if type == ENVIRONMENT_MAP else ()}
	return shader, textures + [""] * (9 - len(textures))


def shader_textures(cache, shader, key, out, size):
	"""A Halo shader's look in Skyrim: the shader, its texture set (None: an
	effect shader names its own texture, and is added), and its textures,
	written under out."""
	stem = Path(shader["name"].replace("\\", "/")).name.replace(" ", "_")
	base = f"{TEXTURES}\\{key}\\{stem}"
	folder = out / Path(TEXTURES.replace("\\", "/")) / key
	written = []
	if shader["class"] == "soso":
		properties = model_shader(cache, shader["name"])
		bitmaps = {}
		for field in ("base_map", "multipurpose_map", "detail_map"):
			if properties[field] and properties[field] not in bitmaps:
				bitmaps[properties[field]] = rgba_array(*decode_bitmap(cache, properties[field]))
		color, _, reflection = (numpy.frombuffer(image, numpy.uint8).reshape(size, size, 4)
			for image in bake(properties, bitmaps, [None] * 4, size))
		written.append(write_dds(folder / f"{stem}.dds", [color]))
		normal = reflection.copy()
		normal[..., :3] = FLAT_NORMAL
		normal[..., 3] = reflection[..., 0]
		written.append(write_dds(folder / f"{stem}_n.dds", [normal]))
		if not properties["reflection_cube_map"]:
			return (*lighting_shader(DEFAULT_SHADER, PLAIN_FLAGS, [f"{base}.dds", f"{base}_n.dds"]), written)
		written.append(write_dds(folder / f"{stem}_m.dds", [reflection]))
		width, height, faces = decode_cube(cache, properties["reflection_cube_map"])
		written.append(write_dds(folder / f"{stem}_e.dds", turn_cube([numpy.frombuffer(bytes(face), numpy.uint8).reshape(height, width, 4) for face in faces])))
		flags = (ARMOR_FLAGS[0] & ~SLSF1_SKINNED, ARMOR_FLAGS[1])
		return (*lighting_shader(ENVIRONMENT_MAP, flags, [f"{base}.dds", f"{base}_n.dds", "", "", f"{base}_e.dds", f"{base}_m.dds"]), written)
	if shader["class"] == "schi":
		address = cache.tag("schi", shader["name"])
		maps = cache.block(address, CHICAGO_MAPS, CHICAGO_MAP_SIZE)
		bitmap = reference(cache, maps[0], CHICAGO_MAP_BITMAP) if maps else None
		if bitmap is None:
			return None
		width, height, rgba = decode_bitmap(cache, bitmap)
		image = numpy.frombuffer(bytes(rgba), numpy.uint8).reshape(height, width, 4).copy()
		image[..., 3] = 255  # added whole, as Halo adds it
		written.append(write_dds(folder / f"{stem}.dds", [image]))
		effect = {"type": "BSEffectShaderProperty", "name": None, "extra_data": [], "controller": -1,
			"flags1": EFFECT_FLAGS[0], "flags2": EFFECT_FLAGS[1], "uv_offset": (0.0, 0.0), "uv_scale": (1.0, 1.0),
			"source_texture": f"{base}.dds", "texture_clamp_mode": 3, "lighting_influence": 0, "env_map_min_lod": 0, "unknown": 0,
			"falloff": (1.0, 1.0, 1.0, 1.0), "base_color": (1.0, 1.0, 1.0, 1.0), "base_color_scale": 1.0,
			"soft_falloff_depth": 100.0, "greyscale_texture": ""}
		return effect, None, written
	print(f"  {shader['name']}: a {shader['class']} shader; its triangles are left out", file=sys.stderr)
	return None


def template(data_dir, cache):
	target = cache / "irondagger.nif"
	if not target.exists():
		target.parent.mkdir(parents=True, exist_ok=True)
		target.write_bytes(Archive(Path(data_dir) / "Skyrim - Meshes1.bsa").read(TEMPLATE))
	return Nif(target)


def build(cache, key, weapon, dagger, out, size):
	model = read_model(cache, weapon_model(cache, weapon), 0)
	if len(model["nodes"]) != 1:
		# a weapon of several nodes has its parts at its nodes' rest poses
		raise SystemExit(f"{weapon}: {len(model['nodes'])} nodes; only a model of one is handled")
	nif = Nif()
	root = {"type": "BSFadeNode", "name": f"Chiefrim{key.title()}", "extra_data": [], "controller": -1, "flags": FLAGS,
		"translation": (0.0, 0.0, 0.0), "rotation": rows(numpy.eye(4)), "scale": 1.0, "collision": -1, "children": [], "effects": []}
	nif.blocks.append(root)
	def add(block):
		nif.blocks.append(block)
		return len(nif.blocks) - 1
	root["extra_data"].append(add({"type": "BSXFlags", "name": "BSX", "value": BSX}))
	root["extra_data"].append(add({"type": "BSInvMarker", "name": "INV", "rotation": INVENTORY_TURN, "zoom": 1.0}))
	# the whole model on the ground (z 0), centred on the origin across it
	everything = numpy.concatenate([[HALO_TO_SKYRIM @ v["position"] for v in part["vertices"]] for part in model["parts"]])
	middle = (everything.min(axis=0) + everything.max(axis=0)) / 2
	middle[2] = everything[:, 2].min()
	shaders, written, kept = {}, [], []
	for part in model["parts"]:
		index = part["shader"]
		if index not in shaders:
			shaders[index] = shader_textures(cache, model["shaders"][index], key, out, size)
			if shaders[index]:
				written += shaders[index][2]
		if not shaders[index]:
			continue
		shader, textures, _ = shaders[index]
		positions = numpy.array([HALO_TO_SKYRIM @ v["position"] for v in part["vertices"]]) - middle
		normals = numpy.array([HALO_TO_SKYRIM @ v["normal"] for v in part["vertices"]])
		normals /= numpy.maximum(numpy.linalg.norm(normals, axis=1, keepdims=True), 1e-12)
		# HALO_TO_SKYRIM turns a quarter about up (a rotation, so winding is
		# kept) and scales; Halo's clockwise triangles to Skyrim's
		faces = [(a, c, b) for a, b, c in part["triangles"]]
		source = [{"position": p.tolist(), "normal": n.tolist(), "uv": v["uv"], "weights": [1.0], "bones": ["frame"]}
			for p, n, v in zip(positions, normals, part["vertices"])]
		vertices, _ = chief_vertices(source, faces)
		for vertex in vertices:
			del vertex["weights"], vertex["bones"]
		kept.append(positions)
		alpha = -1
		if textures is None:
			shader_index = add(dict(shader))
			alpha = add({"type": "NiAlphaProperty", "name": None, "extra_data": [], "controller": -1, "flags": ADDITIVE, "threshold": 0})
		else:
			texture_set = add({"type": "BSShaderTextureSet", "textures": textures})
			shader_index = add(dict(shader, texture_set=texture_set))
		name = Path(model["shaders"][index]["name"].replace("\\", "/")).name.title().replace(" ", "")
		root["children"].append(add({"type": "BSTriShape", "name": f"Chiefrim{key.title()}{name}", "extra_data": [], "controller": -1,
			"flags": FLAGS, "translation": (0.0, 0.0, 0.0), "rotation": rows(numpy.eye(4)), "scale": 1.0, "collision": -1,
			"bound": bound(positions), "skin": -1, "shader_property": shader_index, "alpha_property": alpha,
			"vertex_desc": GROUND_VERTEX, "vertices": vertices, "triangles": faces, "particle_data_size": 0}))
	# the hull, in Havok's units, on the dagger's body; its material the
	# dagger's boxes' (a bhkBoxShape's first 4 bytes)
	points = numpy.concatenate(kept) / HAVOK_SCALE
	body_template = next(b for b in dagger.blocks if b["type"] == "bhkRigidBody")
	box = next(b for b in dagger.blocks if b["type"] == "bhkBoxShape")
	material, = struct.unpack_from("<I", box["raw"])
	extent = points.max(axis=0) - points.min(axis=0)
	radius = min(0.01, extent.min() / 4)
	hull, planes = dop_hull(points, radius)
	shape = add({"type": "bhkConvexVerticesShape", "material": material, "radius": radius, "vertices": [(*v, 0.0) for v in hull.tolist()], "planes": planes})
	body = {k: v for k, v in body_template.items() if k != "raw"}
	low, high = hull.min(axis=0) - radius, hull.max(axis=0) + radius
	size3 = high - low
	mass = body["mass"]
	inertia = mass / 12 * numpy.array([size3[1] ** 2 + size3[2] ** 2, size3[0] ** 2 + size3[2] ** 2, size3[0] ** 2 + size3[1] ** 2])
	body.update(BODY)
	body.update(shape=shape, translation=(0.0, 0.0, 0.0, 0.0), rotation=(0.0, 0.0, 0.0, 1.0),
		center=(*((low + high) / 2).tolist(), 0.0), inertia=(inertia[0], 0, 0, 0, 0, inertia[1], 0, 0, 0, 0, inertia[2], 0))
	root["collision"] = add({"type": "bhkCollisionObject", "target": 0, "flags": 0x81, "body": add(body)})
	path = out / Path(MESHES.replace("\\", "/")) / f"{key}.nif"
	path.parent.mkdir(parents=True, exist_ok=True)
	nif.write(path)
	if Nif(path).write() != path.read_bytes():
		raise SystemExit(f"{path}: doesn't read back as written")
	return path, everything.max(axis=0) - everything.min(axis=0), written


def main():
	parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
	parser.add_argument("map")
	parser.add_argument("--skyrim", default=str(DEFAULT_SKYRIM), help="Skyrim's Data directory")
	parser.add_argument("--dir", default=str(Path(__file__).resolve().parents[2] / "build" / "mjolnir"))
	parser.add_argument("--size", type=int, default=512, help="the baked textures' size")
	args = parser.parse_args()
	directory = Path(args.dir)
	cache = CacheMap(args.map)
	dagger = template(args.skyrim, directory / "skyrim")
	for key, weapon, *_ in WEAPONS:
		path, extent, written = build(cache, key, weapon, dagger, directory / "Data", args.size)
		print(f"{weapon}: wrote {path} ({path.stat().st_size} bytes, {' x '.join(f'{v:.0f}' for v in extent)} units)")
		for texture in written:
			print(f"  wrote {texture} ({texture.stat().st_size} bytes)")


if __name__ == "__main__":
	sys.exit(main())
