#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Splits the model fit.py fitted to Skyrim's skeleton (build/mjolnir/
fitted.json) into Skyrim's armor pieces, and writes their NIFs under
build/mjolnir/Data/meshes/armor/chiefrim/mjolnir/, for the Mjolnir armor
port.

Each of Chief's triangles goes to the piece of the bone weighted most over
its corners: his helmet (the head, and his visor), his gauntlets (forearms
and hands), his boots (calves and feet), his cuirass (the rest). That is
how Skyrim's own armor divides him: its boots cover the calves, its
gauntlets the forearms. And as Skyrim's cuirasses do, his carries the
body's own forearms and calves (from the user's malebody_0/_1.nif), so
that without his gauntlets or boots, bare forearms and calves reach the
hands and feet Skyrim draws; his gauntlets and boots cover them. The cuirass,
gauntlets and boots are written for both ends of the weight slider (_0,
_1: his armor alike, the body's arms and legs each end's); the helmet once.
The first-person cuirass is his upper arms and the first-person body's
forearms. Only the cuirasses differ by sex (male/, f/: the male or female
body's skin; his armor is bound to the same bones either way, and follows a
woman's skeleton as it does a man's); the rest are male/'s for both.

Partitions are the body parts the biped slots name: 30 the head, 32 the
body, 33 hands, 34 forearms, 37 feet, 38 calves. Skyrim draws a worn
addon's partitions only of the slots it covers (and wins): the helmet's
addon covers the head (30), as Skyrim's full helmets' do, which hides the
face inside it.

Usage: tools/mjolnir/armor.py [--skyrim DATA_DIR] [--dir build/mjolnir]
"""
import argparse
import copy
import json
import sys
from pathlib import Path

import numpy

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bsa import Archive  # noqa: E402
from fit import DEFAULT_SKYRIM, skyrim_skeleton  # noqa: E402
from nif import VF_NORMAL, VF_SKINNED, VF_TANGENT, VF_UV, VF_VERTEX, Nif, vertex_desc  # noqa: E402

BODY = "meshes\\actors\\character\\character assets\\{}.nif"
MESHES = "meshes\\armor\\chiefrim\\mjolnir"
SEXES = {"male": "male", "f": "female"}
TEXTURES = "textures\\chiefrim\\mjolnir"
CHIEF_VERTEX = vertex_desc(VF_VERTEX | VF_UV | VF_NORMAL | VF_TANGENT | VF_SKINNED)

# body parts (biped slots), as partitions
HELMET, BODY_PART, HANDS, FOREARMS, FEET, CALVES = 30, 32, 33, 34, 37, 38
# per piece, its bones (by their names' brackets) and the body part each's
# triangles are a partition of (in Skyrim's order); the cuirass takes the
# rest
PIECES = {
	"helmet": {"Head": HELMET},
	"gauntlets": {"LHnd": HANDS, "RHnd": HANDS, "LLar": FOREARMS, "RLar": FOREARMS},
	"boots": {"LClf": CALVES, "RClf": CALVES, "Lft ": FEET, "Rft ": FEET},
	"cuirass": {},
}
PART_ORDER = (BODY_PART, CALVES, FEET, HANDS, FOREARMS, HELMET)
FIRST_PERSON = ("LUar", "RUar")
# the body's skin the cuirass carries, by the bones its triangles are
# weighted most to: its forearms (with their twist bones) and calves, as
# the forearm and calf partitions
FOREARM_SKIN = {bone: FOREARMS for bone in ("LLar", "RLar", "LLt1", "RLt1", "LLt2", "RLt2")}
CALF_SKIN = {bone: CALVES for bone in ("LClf", "RClf")}
# Halo's shaders Skyrim draws (the shield's effects aren't): their textures
SHADERS = {"armor": "armor", "visor": "visor"}

# BSLightingShaderProperty's flags: Skyrim's armor's (specular, skinned,
# shadows received and cast, own emit, remappable textures, z-buffer test;
# z-buffer write, environment map light fade), and an environment map, for
# Halo's cube map reflections
SLSF1_ENVIRONMENT_MAPPING = 0x80
ARMOR_FLAGS = (0x82400303 | SLSF1_ENVIRONMENT_MAPPING, 0x00008001)
ENVIRONMENT_MAP = 1


def bracket(bone):
	return bone[bone.index("[") + 1:-1]


def body_nif(data_dir, name, cache):
	"""The user's own body NIF, loose files first, as the game takes them,
	then the archives."""
	path = BODY.format(name)
	loose = Path(data_dir) / path.replace("\\", "/")
	if loose.exists():
		return Nif(loose)
	target = cache / f"{name}.nif"
	if not target.exists():
		for archive in ("Skyrim - Meshes0.bsa", "Skyrim - Meshes1.bsa"):
			archive = Archive(Path(data_dir) / archive)
			if path in archive.files:
				target.parent.mkdir(parents=True, exist_ok=True)
				target.write_bytes(archive.read(path))
				break
		else:
			raise FileNotFoundError(f"{path} is in neither of Skyrim's mesh archives")
	return Nif(target)


def split(fitted):
	"""Chief's triangles by piece: piece -> shader name -> (vertices,
	triangles by body part), each piece's vertices its own (shared ones
	copied, so where two pieces meet, they meet exactly)."""
	pieces = {piece: {} for piece in PIECES}
	first_person = {}
	for part in fitted["parts"]:
		name = Path(fitted["shaders"][part["shader"]]["name"].replace("\\", "/")).name
		if name not in SHADERS:
			continue
		vertices = part["vertices"]
		for triangle in part["triangles"]:
			totals = {}
			for index in triangle:
				for bone, weight in zip(vertices[index]["bones"], vertices[index]["weights"]):
					totals[bracket(bone)] = totals.get(bracket(bone), 0) + weight
			dominant = max(totals, key=totals.get)
			piece = next((p for p, bones in PIECES.items() if dominant in bones), "cuirass")
			body_part = PIECES[piece].get(dominant, BODY_PART)
			targets = [pieces[piece]]
			if piece == "cuirass" and dominant in FIRST_PERSON:
				targets.append(first_person)
			for target in targets:
				new, faces, remap = target.setdefault(name, ([], {}, {}))
				for index in triangle:
					if index not in remap:
						remap[index] = len(new)
						new.append(vertices[index])
				faces.setdefault(body_part, []).append(tuple(remap[index] for index in triangle))
	def strip(shapes):
		return {name: (vertices, sorted(faces.items(), key=lambda item: PART_ORDER.index(item[0])))
			for name, (vertices, faces, _) in shapes.items()}
	return {piece: strip(shapes) for piece, shapes in pieces.items()}, strip(first_person)


def tangents(positions, normals, uvs, triangles):
	"""Per vertex, the directions u and v run across the surface (in the
	plane normal to it). Skyrim's vertices hold them swapped: its "tangent"
	is v's, its "bitangent" u's. (They shade through a normal map; Halo's
	armor has none, so his is flat.)"""
	du, dv = numpy.zeros_like(positions), numpy.zeros_like(positions)
	for a, b, c in triangles:
		e1, e2 = positions[b] - positions[a], positions[c] - positions[a]
		(s1, t1), (s2, t2) = uvs[b] - uvs[a], uvs[c] - uvs[a]
		det = s1 * t2 - s2 * t1
		if abs(det) < 1e-12:
			continue
		u, v = (e1 * t2 - e2 * t1) / det, (e2 * s1 - e1 * s2) / det
		for index in (a, b, c):
			du[index] += u
			dv[index] += v
	def plane(d):
		d = d - normals * numpy.sum(d * normals, axis=1, keepdims=True)
		length = numpy.linalg.norm(d, axis=1, keepdims=True)
		fallback = numpy.cross(normals, numpy.where(abs(normals[:, :1]) < 0.9, [[1, 0, 0]], [[0, 1, 0]]))
		fallback /= numpy.linalg.norm(fallback, axis=1, keepdims=True)
		return numpy.where(length > 1e-9, d / numpy.maximum(length, 1e-12), fallback)
	return plane(du), plane(dv)


def bound(points):
	"""A sphere around points: their bounding box's centre, and the
	farthest from it."""
	if not len(points):
		return (0.0, 0.0, 0.0, 0.0)
	centre = (points.min(axis=0) + points.max(axis=0)) / 2
	return (*centre.tolist(), float(numpy.linalg.norm(points - centre, axis=1).max()))


def rows(matrix):
	return [tuple(map(float, row)) for row in matrix[:3, :3]]


def identity_transform():
	return {"rotation": rows(numpy.eye(4)), "translation": (0.0, 0.0, 0.0), "scale": 1.0}


class Builder:
	"""One NIF: its root, a node per bone (at its rest pose in the
	skeleton, as Skyrim's are), and skinned shapes."""

	def __init__(self, name, skeleton):
		self.nif = Nif()
		self.skeleton = skeleton
		self.nif.blocks.append(self.node(name, numpy.eye(4)))
		self.bone_nodes = {}

	def add(self, block):
		self.nif.blocks.append(block)
		return len(self.nif.blocks) - 1

	def node(self, name, matrix):
		return {"type": "NiNode", "name": name, "flags": 14, "translation": tuple(map(float, matrix[:3, 3])),
			"rotation": rows(matrix), "scale": 1.0, "children": [], "effects": []}

	def bone(self, name):
		if name not in self.bone_nodes:
			self.bone_nodes[name] = self.add(self.node(name, self.skeleton[name][1]))
			self.nif.blocks[0]["children"].append(self.bone_nodes[name])
		return self.bone_nodes[name]

	def shape(self, name, vertices, bones, partitions, shader, textures, translation=(0.0, 0.0, 0.0), skin_transform=None, bone_transforms=None):
		"""A skinned BSTriShape. vertices are BSVertexData dicts (their
		"bones" indices into bones), positions in the shape's frame;
		partitions are (body part, triangles), each its own partition;
		bone_transforms are NiSkinData's, from the shape's frame to each
		bone's (by default, the inverse of the bone's rest matrix, the shape
		being at the skeleton's origin)."""
		desc = shader.pop("vertex_desc")
		shape = self.add({"type": "BSTriShape", "name": name, "flags": 14, "translation": translation,
			"rotation": rows(numpy.eye(4)), "scale": 1.0, "vertex_desc": desc, "alpha_property": -1})
		skin = self.add({"type": "BSDismemberSkinInstance", "skeleton_root": 0, "bones": [self.bone(b) for b in bones],
			"partitions": [(257 if index == 0 else 1, part) for index, (part, _) in enumerate(partitions)]})
		positions = numpy.array([v["position"] for v in vertices])
		weighted = [[] for _ in bones]
		for index, vertex in enumerate(vertices):
			for bone, weight in zip(vertex["bones"], vertex["weights"]):
				if weight > 0:
					weighted[bone].append((index, float(weight)))
		skin_bones = []
		for bone_index, bone in enumerate(bones):
			if bone_transforms is not None:
				transform = bone_transforms[bone_index]
				matrix = numpy.eye(4)
				matrix[:3, :3] = numpy.array(transform["rotation"]) * transform["scale"]
				matrix[:3, 3] = transform["translation"]
			else:
				matrix = numpy.linalg.inv(self.skeleton[bone][1])
				transform = {"rotation": rows(matrix), "translation": tuple(map(float, matrix[:3, 3])), "scale": 1.0}
			points = positions[[index for index, _ in weighted[bone_index]]] if weighted[bone_index] else numpy.zeros((0, 3))
			points = points @ matrix[:3, :3].T + matrix[:3, 3]
			skin_bones.append({"transform": transform, "bound": bound(points), "weights": weighted[bone_index]})
		data = self.add({"type": "NiSkinData", "skin_transform": skin_transform or identity_transform(), "has_weights": 1, "bones": skin_bones})
		skin_partitions = []
		for _, faces in partitions:
			used = sorted({index for face in faces for index in face})
			skin_partitions.append({"vertex_count": len(used), "weights_per_vertex": 4, "bones": list(range(len(bones))),
				"vertex_map": used, "vertex_weights": [tuple(vertices[i]["weights"]) for i in used],
				"triangles": [tuple(face) for face in faces], "bone_indices": [tuple(vertices[i]["bones"]) for i in used],
				"lod_level": 0, "global_vb": 0, "vertex_desc": desc, "triangles_copy": [tuple(face) for face in faces]})
		partition = self.add({"type": "NiSkinPartition", "vertex_desc": desc, "vertices": vertices, "partitions": skin_partitions})
		texture_set = self.add({"type": "BSShaderTextureSet", "textures": textures})
		shader = self.add(dict(shader, texture_set=texture_set))
		self.nif.blocks[skin].update(data=data, partition=partition)
		# a skinned shape's own bound is 0, as Skyrim's are: the game bounds
		# it by its bones' (a real one is taken where the shape isn't, and
		# culls it)
		self.nif.blocks[shape].update(skin=skin, shader_property=shader, bound=(0.0, 0.0, 0.0, 0.0))
		self.nif.blocks[0]["children"].append(shape)

	def write(self, path):
		path.parent.mkdir(parents=True, exist_ok=True)
		self.nif.write(path)
		return path


def chief_shader(name):
	"""Halo's look as Skyrim's armor shader draws it: the baked colour,
	a flat normal map (its alpha the specular mask), Halo's cube map,
	masked by where Halo's shader reflects it."""
	shader = {"type": "BSLightingShaderProperty", "shader_type": ENVIRONMENT_MAP, "name": None, "extra_data": [], "controller": -1,
		"flags1": ARMOR_FLAGS[0], "flags2": ARMOR_FLAGS[1], "uv_offset": (0.0, 0.0), "uv_scale": (1.0, 1.0),
		"emissive_color": (0.0, 0.0, 0.0), "emissive_multiple": 1.0, "texture_clamp_mode": 3, "alpha": 1.0,
		"refraction_strength": 0.0, "glossiness": 80.0, "specular_color": (1.0, 1.0, 1.0), "specular_strength": 1.0,
		"lighting_effects": (0.3, 2.0), "type_floats": (1.0,), "vertex_desc": CHIEF_VERTEX}
	base = f"{TEXTURES}\\{SHADERS[name]}"
	return shader, [f"{base}.dds", f"{base}_n.dds", "", "", f"{base}_e.dds", f"{base}_m.dds", "", "", ""]


def chief_vertices(source, faces):
	"""Chief's fitted vertices as BSVertexData, bound to bones (their
	names, in first use), at most four weights each."""
	positions = numpy.array([v["position"] for v in source])
	normals = numpy.array([v["normal"] for v in source])
	uvs = numpy.array([v["uv"] for v in source])
	along_v, along_u = tangents(positions, normals, uvs, faces)
	bones, vertices = [], []
	for index, vertex in enumerate(source):
		pairs = sorted(zip(vertex["weights"], vertex["bones"]), reverse=True)[:4]
		total = sum(w for w, _ in pairs)
		indices, weights = [], []
		for weight, bone in pairs:
			if bone not in bones:
				bones.append(bone)
			indices.append(bones.index(bone))
			weights.append(weight / total)
		indices += [0] * (4 - len(indices))
		weights += [0.0] * (4 - len(weights))
		vertices.append({"position": positions[index].tolist(), "bitangent_x": float(along_u[index][0]),
			"uv": tuple(uvs[index]), "normal": normals[index].tolist(), "bitangent_y": float(along_u[index][1]),
			"tangent": along_v[index].tolist(), "bitangent_z": float(along_u[index][2]),
			"weights": tuple(weights), "bones": tuple(indices)})
	return vertices, bones


def add_chief(builder, shapes):
	"""Chief's shapes. Halo's triangles wind clockwise seen from the front
	(Direct3D's default); Skyrim's counter-clockwise, so each is reversed."""
	for name, (source, partitions) in shapes.items():
		partitions = [(part, [(a, c, b) for a, b, c in faces]) for part, faces in partitions]
		vertices, bones = chief_vertices(source, [face for _, faces in partitions for face in faces])
		shader, textures = chief_shader(name)
		builder.shape(f"Mjolnir{name.title()}", vertices, bones, partitions, shader, textures)


def closest_points(point, triangles):
	"""The point of each triangle (n x 3 x 3) closest to point (Ericson's
	Real-Time Collision Detection, 5.1.5), vectorized."""
	a, b, c = triangles[:, 0], triangles[:, 1], triangles[:, 2]
	ab, ac, ap = b - a, c - a, point - a
	d1, d2 = (ab * ap).sum(1), (ac * ap).sum(1)
	bp, cp = point - b, point - c
	d3, d4 = (ab * bp).sum(1), (ac * bp).sum(1)
	d5, d6 = (ab * cp).sum(1), (ac * cp).sum(1)
	va, vb, vc = d3 * d6 - d5 * d4, d5 * d2 - d1 * d6, d1 * d4 - d3 * d2
	with numpy.errstate(divide="ignore", invalid="ignore"):
		denominator = va + vb + vc
		v, w = vb / denominator, vc / denominator
		result = a + ab * v[:, None] + ac * w[:, None]  # inside
		edge_bc = (d4 - d3) / ((d4 - d3) + (d5 - d6))
		cases = [
			((va <= 0) & (d4 - d3 >= 0) & (d5 - d6 >= 0), b + (c - b) * edge_bc[:, None]),
			((vb <= 0) & (d2 >= 0) & (d6 <= 0), a + ac * (d2 / (d2 - d6))[:, None]),
			((vc <= 0) & (d1 >= 0) & (d3 <= 0), a + ab * (d1 / (d1 - d3))[:, None]),
			((d6 >= 0) & (d5 <= d6), c),
			((d3 >= 0) & (d4 <= d3), b),
			((d1 <= 0) & (d2 <= 0), a),
		]
	for mask, value in cases:  # the last applied wins: vertices, then edges
		result = numpy.where(mask[:, None], value, result)
	return result


def tuck(points, triangles, margin):
	"""points moved inside the surface triangles make (counter-clockwise
	from outside), at least margin under it: each that isn't, to under the
	point of the surface closest to it."""
	normals = numpy.cross(triangles[:, 1] - triangles[:, 0], triangles[:, 2] - triangles[:, 0])
	normals /= numpy.maximum(numpy.linalg.norm(normals, axis=1, keepdims=True), 1e-12)
	moved = 0
	result = points.copy()
	for index, point in enumerate(points):
		closest = closest_points(point, triangles)
		nearest = numpy.linalg.norm(closest - point, axis=1).argmin()
		depth = numpy.dot(point - closest[nearest], normals[nearest])
		if depth > -margin:
			result[index] = closest[nearest] - normals[nearest] * margin
			moved += 1
	return result, moved


def add_body(builder, body, regions, inside=None, margin=0.3):
	"""The body's own skin where its triangles are weighted most to these
	bones (regions: bracket -> body part), as it is: its frame, shading,
	bones' transforms; each region its body part's partition. Skyrim hides
	a worn addon's partition of a body part another worn addon covers with
	a higher priority: so the gauntlets hide these forearms, the boots
	these calves (Skyrim's own forearm and calf partitions are only the
	caps at the hands and feet; the body's partition has the rest, which
	would show through the gap Halo leaves at Chief's elbows). Any of it
	outside the surface inside makes (Chief's) is tucked under it, so that
	without them, none shows through his armor."""
	shape = next(b for b in body.blocks if b["type"] == "BSTriShape" and "UnderwearBody" in b["name"])
	name = shape["name"].split(":")[0] + "Armor"
	skin = body.blocks[shape["skin"]]
	data, partition = body.blocks[skin["data"]], body.blocks[skin["partition"]]
	brackets = [bracket(body.blocks[b]["name"]) for b in skin["bones"]]
	def dominant(face):
		totals = {}
		for i in face:
			vertex = partition["vertices"][i]
			for b, w in zip(vertex["bones"], vertex["weights"]):
				totals[brackets[b]] = totals.get(brackets[b], 0) + w
		return max(totals, key=totals.get)
	by_part = {}
	for p in partition["partitions"]:
		for face in p["triangles_copy"]:
			part = regions.get(dominant(face))
			if part is not None:
				by_part.setdefault(part, []).append(face)
	chosen = [(part, {"triangles_copy": by_part[part]}) for part in PART_ORDER if part in by_part]
	used = sorted({i for _, p in chosen for face in p["triangles_copy"] for i in face})
	remap = {old: new for new, old in enumerate(used)}
	bones_used = sorted({b for i in used for b, w in zip(partition["vertices"][i]["bones"], partition["vertices"][i]["weights"]) if w > 0})
	bone_remap = {old: new for new, old in enumerate(bones_used)}
	vertices = []
	for i in used:
		vertex = dict(partition["vertices"][i])
		vertex["bones"] = tuple(bone_remap[b] if w > 0 else 0 for b, w in zip(vertex["bones"], vertex["weights"]))
		vertices.append(vertex)
	if inside is not None:
		offset = numpy.array(shape["translation"])
		points = numpy.array([v["position"] for v in vertices]) + offset
		points, moved = tuck(points, inside, margin)
		for vertex, point in zip(vertices, points - offset):
			vertex["position"] = point.tolist()
		print(f"  {name}: {moved} of {len(vertices)} vertices tucked under Chief's armor")
	partitions = [(part, [tuple(remap[i] for i in face) for face in p["triangles_copy"]]) for part, p in chosen]
	shader = copy.deepcopy(body.blocks[shape["shader_property"]])
	for key in ("raw", "texture_set"):
		shader.pop(key, None)
	shader["vertex_desc"] = partition["vertex_desc"]
	textures = list(body.blocks[shader_texture_set(body, shape)]["textures"])
	bone_names = [body.blocks[skin["bones"][b]]["name"] for b in bones_used]
	builder.shape(name, vertices, bone_names, partitions, shader, textures, translation=tuple(shape["translation"]),
		skin_transform=data["skin_transform"], bone_transforms=[data["bones"][b]["transform"] for b in bones_used])


def shader_texture_set(nif, shape):
	return nif.blocks[shape["shader_property"]]["texture_set"]


def chief_surface(fitted):
	"""All of Chief's triangles (counter-clockwise), as n x 3 x 3."""
	triangles = []
	for part in fitted["parts"]:
		if Path(fitted["shaders"][part["shader"]]["name"].replace("\\", "/")).name in SHADERS:
			positions = numpy.array([v["position"] for v in part["vertices"]])
			triangles += [positions[[a, c, b]] for a, b, c in part["triangles"]]
	return numpy.array(triangles)


def build(fitted, skeleton, bodies, out):
	pieces, first_person = split(fitted)
	surface = chief_surface(fitted)
	written = []
	for folder, sex in SEXES.items():
		for piece in PIECES if folder == "male" else ["cuirass"]:
			for weight in ([None] if piece == "helmet" else ["0", "1"]):
				name = piece if weight is None else f"{piece}_{weight}"
				builder = Builder(f"{name.title()}.nif", skeleton)
				add_chief(builder, pieces[piece])
				if piece == "cuirass":
					add_body(builder, bodies[f"{sex}body_{weight}"], {**FOREARM_SKIN, **CALF_SKIN}, surface)
				written.append(builder.write(out / folder / f"{name}.nif"))
				if piece == "cuirass":
					name = f"1stperson{name}"
					builder = Builder(f"{name}.nif", skeleton)
					add_chief(builder, first_person)
					add_body(builder, bodies[f"1stperson{sex}body_{weight}"], FOREARM_SKIN, surface)
					written.append(builder.write(out / folder / f"{name}.nif"))
	return pieces, written


def main():
	parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
	parser.add_argument("--skyrim", default=str(DEFAULT_SKYRIM), help="Skyrim's Data directory")
	parser.add_argument("--dir", default=str(Path(__file__).resolve().parents[2] / "build" / "mjolnir"))
	args = parser.parse_args()
	directory = Path(args.dir)
	fitted = json.loads((directory / "fitted.json").read_text())
	skeleton = skyrim_skeleton(args.skyrim)
	bodies = {name: body_nif(args.skyrim, name, directory / "skyrim")
		for sex in SEXES.values() for name in (f"{sex}body_0", f"{sex}body_1", f"1stperson{sex}body_0", f"1stperson{sex}body_1")}
	pieces, written = build(fitted, skeleton, bodies, directory / "Data" / Path(MESHES.replace("\\", "/")))
	for piece, shapes in pieces.items():
		print(f"{piece}: " + ", ".join(f"{name} " + " + ".join(f"{len(faces)} ({part})" for part, faces in partitions)
			+ " triangles" for name, (_, partitions) in shapes.items()))
	for path in written:
		check = Nif(path)
		if check.write() != path.read_bytes():
			raise SystemExit(f"{path}: doesn't read back as written")
		print(f"wrote {path} ({path.stat().st_size} bytes, {len(check.blocks)} blocks)")


if __name__ == "__main__":
	sys.exit(main())
