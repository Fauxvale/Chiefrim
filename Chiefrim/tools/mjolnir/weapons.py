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
the origin across it, its muzzle to +y, as Skyrim's weapons point their
blades. A model's vertices are at its nodes' rest poses (Halo draws them
by each node's matrix times its default's inverse), as a weapon lies, so a
model of several nodes (the pistol's slide, the rifles' bolts) is taken
whole. Its shaders:
- a model shader (soso) as the armor's are (halo_model.bake, then as
  textures.py writes them): its colour, a flat normal map whose alpha is the
  specular mask, Halo's cube map and where Halo reflects it. Skyrim's
  lighting shader has an environment map or a glow map, not both: a shader
  with no cube map and a self-illumination (the plasma weapons' luminous
  parts, the rifle's display) glows instead; one with both (the needler's)
  reflects. An alpha-blended decal (the stencilled markings) is blended
  by its base map's alpha, a decal;
- a transparent shader (chicago, schi, or generic, sotr: the needles, the
  rifle's display, the bolts, the sniper's screens) as its first map in
  Skyrim's effect shader, added onto what's behind it or blended by its
  alpha as Halo's framebuffer blend function says (added as Skyrim's own
  glowing crystals are: the Dwarven staff's), both sides drawn;
- a meter (smet: the plasma weapons' heat gauges) is left out: a cool gun's
  draws next to nothing (its value 0, its background's alpha 0).
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
from halo_model import CacheMap, bake, decode_bitmap, decode_cube, read_model, reference, model_shader, rgba_array, sample  # noqa: E402
from nif import HAVOK_SCALE, Nif  # noqa: E402
from textures import FLAT_NORMAL, turn_cube, write_dds  # noqa: E402

MESHES = "meshes\\weapons\\chiefrim"
TEXTURES = "textures\\weapons\\chiefrim"
TEMPLATE = "meshes\\weapons\\iron\\irondagger.nif"
# per weapon: its key (files, editor IDs), its weap tag, its name in
# Skyrim, its form ID in the plugin (skse/src/Arsenal.cpp's table has the
# same; its inventory model's is INVENTORY_ID past it), its value and weight
# (a one-handed gun as heavy as a sword, a rifle as a greatsword, the
# rocket launcher more). Halo's others (the fuel rod, the flamethrower)
# Chief can't carry.
WEAPONS = [
	("needler", "weapons\\needler\\needler", "Needler", 0x900, 300, 10.0),
	("assault_rifle", "weapons\\assault rifle\\assault rifle", "Assault Rifle", 0x901, 350, 14.0),
	("pistol", "weapons\\pistol\\pistol", "Pistol", 0x902, 250, 8.0),
	("shotgun", "weapons\\shotgun\\shotgun", "Shotgun", 0x903, 400, 15.0),
	("sniper_rifle", "weapons\\sniper rifle\\sniper rifle", "Sniper Rifle", 0x904, 600, 16.0),
	("rocket_launcher", "weapons\\rocket launcher\\rocket launcher", "Rocket Launcher", 0x905, 800, 25.0),
	("plasma_pistol", "weapons\\plasma pistol\\plasma pistol", "Plasma Pistol", 0x906, 250, 8.0),
	("plasma_rifle", "weapons\\plasma rifle\\plasma rifle", "Plasma Rifle", 0x907, 400, 12.0),
	# grenades: equipment (eqip), not weapons; equipping them hands Chief
	# as many as he has room for
	("frag_grenade", "weapons\\frag grenade\\frag grenade", "Frag Grenade", 0x908, 30, 1.0),
	("plasma_grenade", "weapons\\plasma grenade\\plasma grenade", "Plasma Grenade", 0x909, 40, 1.0),
]
INVENTORY_ID = 0x80
# the rigid body's, over the dagger's: little bounce, more grip, its
# tumbling damped; its mass MASS_PER_WEIGHT times the item's weight (the
# needler's 20 kg, twice a Skyrim weapon's; a grenade's 2)
BODY = {"restitution": 0.1, "friction": 0.8, "linear_damping": 0.2, "angular_damping": 0.6}
MASS_PER_WEIGHT = 2.0
# Skyrim's effect shader as its glowing crystals have it (the Dwarven
# staff's): z-buffer test, both sides drawn; blended added (src alpha, one,
# the sorter off)
EFFECT_FLAGS = (0x80000000, 0x10)
ADDITIVE = 0x100D
# blended by the source's alpha (src alpha, inverse src alpha)
BLENDED = 0x00ED
# Skyrim's SLSF1 decal (drawn over the surface it lies on), SLSF2 glow map
SLSF1_DECAL = 0x04000000
SLSF2_GLOW_MAP = 0x40
GLOW_SHADER = 2
# a model shader's flags: an alpha-blended decal; a transparent shader's
# (after the shader): a decal; its framebuffer blend function, add
SOSO_DECAL = 0x8
TRANSPARENT_DECAL = 0x2
BLEND_ADD = 3
# the inventory's turn (milliradians about x, y, z): its side to the eye
INVENTORY_TURN = (0, 0, 1571)
# Skyrim's lighting shader's: no environment map, not skinned
PLAIN_FLAGS = (ARMOR_FLAGS[0] & ~SLSF1_ENVIRONMENT_MAPPING & ~SLSF1_SKINNED, ARMOR_FLAGS[1])
DEFAULT_SHADER = 0
# a transparent shader (OpenCE's tag_schema_render.c), after the shader's
# 0x28-byte header: its flags, its framebuffer blend function, its maps
# block; per class a map's size, its bitmap's reference and its u, v scale
# and offset in it
TRANSPARENT_FLAGS, TRANSPARENT_BLEND, TRANSPARENT_MAPS = 0x29, 0x2C, 0x54
TRANSPARENT_MAP = {"schi": (0xDC, 0x6C, 0x54), "sotr": (0x64, 0x1C, 0x04)}


def editor_name(name):
	"""Its nodes' and editor IDs' name: "Assault Rifle" to AssaultRifle."""
	return name.replace(" ", "")


def weapon_model(cache, weapon):
	"""The weap's (or a grenade's eqip's) model's tag name (its object's
	model reference, 0x28 in; on Xbox a mode)."""
	name = reference(cache, cache.tag("weap" if ("weap", weapon) in cache.tags else "eqip", weapon), 0x28)
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


def effect_shader(base, flags1):
	return {"type": "BSEffectShaderProperty", "name": None, "extra_data": [], "controller": -1,
		"flags1": flags1, "flags2": EFFECT_FLAGS[1], "uv_offset": (0.0, 0.0), "uv_scale": (1.0, 1.0),
		"source_texture": base, "texture_clamp_mode": 3, "lighting_influence": 0, "env_map_min_lod": 0, "unknown": 0,
		"falloff": (1.0, 1.0, 1.0, 1.0), "base_color": (1.0, 1.0, 1.0, 1.0), "base_color_scale": 1.0,
		"soft_falloff_depth": 100.0, "greyscale_texture": ""}


def shader_textures(cache, shader, key, out, size):
	"""A Halo shader's look in Skyrim, or None (left out): the shader, its
	texture set (None: an effect shader names its own texture), its
	NiAlphaProperty's flags (None: opaque), and its textures, written
	under out."""
	stem = Path(shader["name"].replace("\\", "/")).name.replace(" ", "_")
	base = f"{TEXTURES}\\{key}\\{stem}"
	folder = out / Path(TEXTURES.replace("\\", "/")) / key
	written = []
	if shader["class"] == "soso":
		properties = model_shader(cache, shader["name"])
		flags, = cache.unpack("<H", cache.tag("soso", shader["name"]), 0x28)
		bitmaps = {}
		for field in ("base_map", "multipurpose_map", "detail_map"):
			if properties[field] and properties[field] not in bitmaps:
				bitmaps[properties[field]] = rgba_array(*decode_bitmap(cache, properties[field]))
		color, glow, reflection = (numpy.frombuffer(image, numpy.uint8).reshape(size, size, 4)
			for image in bake(properties, bitmaps, [None] * 4, size))
		alpha = None
		if flags & SOSO_DECAL:
			# blended by the base map's alpha
			color = color.copy()
			color[..., 3] = numpy.round(sample(bitmaps[properties["base_map"]], size)[..., 3] * 255).astype(numpy.uint8)
			alpha = BLENDED
		written.append(write_dds(folder / f"{stem}.dds", [color]))
		normal = reflection.copy()
		normal[..., :3] = FLAT_NORMAL
		normal[..., 3] = reflection[..., 0]
		written.append(write_dds(folder / f"{stem}_n.dds", [normal]))
		decal = SLSF1_DECAL if flags & SOSO_DECAL else 0
		if not properties["reflection_cube_map"]:
			if glow[..., :3].any():
				written.append(write_dds(folder / f"{stem}_g.dds", [glow]))
				shader, textures = lighting_shader(GLOW_SHADER, (PLAIN_FLAGS[0] | decal, PLAIN_FLAGS[1] | SLSF2_GLOW_MAP),
					[f"{base}.dds", f"{base}_n.dds", f"{base}_g.dds"])
				shader["emissive_color"] = (1.0, 1.0, 1.0)
				return shader, textures, alpha, written
			return (*lighting_shader(DEFAULT_SHADER, (PLAIN_FLAGS[0] | decal, PLAIN_FLAGS[1]), [f"{base}.dds", f"{base}_n.dds"]), alpha, written)
		written.append(write_dds(folder / f"{stem}_m.dds", [reflection]))
		width, height, faces = decode_cube(cache, properties["reflection_cube_map"])
		written.append(write_dds(folder / f"{stem}_e.dds", turn_cube([numpy.frombuffer(bytes(face), numpy.uint8).reshape(height, width, 4) for face in faces])))
		flags = ((ARMOR_FLAGS[0] & ~SLSF1_SKINNED) | decal, ARMOR_FLAGS[1])
		return (*lighting_shader(ENVIRONMENT_MAP, flags, [f"{base}.dds", f"{base}_n.dds", "", "", f"{base}_e.dds", f"{base}_m.dds"]), alpha, written)
	if shader["class"] in TRANSPARENT_MAP:
		address = cache.tag(shader["class"], shader["name"])
		flags, = cache.unpack("<B", address, TRANSPARENT_FLAGS)
		blend, = cache.unpack("<h", address, TRANSPARENT_BLEND)
		map_size, map_bitmap, map_scale = TRANSPARENT_MAP[shader["class"]]
		maps = cache.block(address, TRANSPARENT_MAPS, map_size)
		bitmap = reference(cache, maps[0], map_bitmap) if maps else None
		if bitmap is None:
			return None
		width, height, rgba = decode_bitmap(cache, bitmap)
		image = numpy.frombuffer(bytes(rgba), numpy.uint8).reshape(height, width, 4).copy()
		if blend == BLEND_ADD:
			image[..., 3] = 255  # added whole, as Halo adds it
		written.append(write_dds(folder / f"{stem}.dds", [image]))
		u_scale, v_scale, u_offset, v_offset = cache.unpack("<4f", maps[0], map_scale)
		effect = effect_shader(f"{base}.dds", EFFECT_FLAGS[0] | (SLSF1_DECAL if flags & TRANSPARENT_DECAL else 0))
		effect.update(uv_scale=(u_scale or 1.0, v_scale or 1.0), uv_offset=(u_offset, v_offset))
		return effect, None, ADDITIVE if blend == BLEND_ADD else BLENDED, written
	if shader["class"] != "smet":
		print(f"  {shader['name']}: a {shader['class']} shader; its triangles are left out", file=sys.stderr)
	return None


def template(data_dir, cache):
	target = cache / "irondagger.nif"
	if not target.exists():
		target.parent.mkdir(parents=True, exist_ok=True)
		target.write_bytes(Archive(Path(data_dir) / "Skyrim - Meshes1.bsa").read(TEMPLATE))
	return Nif(target)


def build(cache, key, weapon, name, weight, dagger, out, size):
	model = read_model(cache, weapon_model(cache, weapon), 0)
	nif = Nif()
	root = {"type": "BSFadeNode", "name": f"Chiefrim{editor_name(name)}", "extra_data": [], "controller": -1, "flags": FLAGS,
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
				written += shaders[index][3]
		if not shaders[index]:
			continue
		shader, textures, blending, _ = shaders[index]
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
		if textures is None:
			shader_index = add(dict(shader))
		else:
			texture_set = add({"type": "BSShaderTextureSet", "textures": textures})
			shader_index = add(dict(shader, texture_set=texture_set))
		alpha = -1 if blending is None else add({"type": "NiAlphaProperty", "name": None, "extra_data": [], "controller": -1, "flags": blending, "threshold": 0})
		part_name = Path(model["shaders"][index]["name"].replace("\\", "/")).name.title().replace(" ", "")
		root["children"].append(add({"type": "BSTriShape", "name": f"Chiefrim{editor_name(name)}{part_name}", "extra_data": [], "controller": -1,
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
	mass = MASS_PER_WEIGHT * weight
	inertia = mass / 12 * numpy.array([size3[1] ** 2 + size3[2] ** 2, size3[0] ** 2 + size3[2] ** 2, size3[0] ** 2 + size3[1] ** 2])
	body.update(BODY, mass=mass)
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
	for key, weapon, name, _, _, weight in WEAPONS:
		path, extent, written = build(cache, key, weapon, name, weight, dagger, directory / "Data", args.size)
		print(f"{weapon}: wrote {path} ({path.stat().st_size} bytes, {' x '.join(f'{v:.0f}' for v in extent)} units)")
		for texture in written:
			print(f"  wrote {texture} ({texture.stat().st_size} bytes)")


if __name__ == "__main__":
	sys.exit(main())
