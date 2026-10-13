#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes the Mjolnir armor's ground models (its world models: what lies
on the ground when a piece is dropped, and turns in the inventory), one
per piece, build/arsenal/Data/meshes/armor/chiefrim/mjolnir/<piece>_go.nif,
from the pieces armor.py splits Chief into, as Skyrim's Steel Plate
armor's (Armor\\NordPlate\\*_GO.nif, read from the user's own Skyrim) are
made: one unskinned shape per shader, and a convex hull a dropped piece
falls, slides and rests on (Havok's, on the clutter layer).

They lie as Steel Plate's do: the helmet upright; the cuirass on its
back, its torso and upper arms only (its thighs left out, as Steel
Plate's legs are); the gauntlets flat, side by side, pointing away, backs
of the hands up; the boots standing side by side. Each touches the ground
(z 0), centred on the origin across it.

The hull is the pieces' 26-sided bounding polyhedron (each axis, face and
edge diagonal), less Havok's convex radius, which pads it back out; its
rigid body is Steel Plate's same piece's (mass, friction, material),
with the hull's centre and a solid box's inertia.

Usage: tools/arsenal/ground.py [--skyrim DATA_DIR] [--dir build/arsenal]
"""
import argparse
import itertools
import json
import sys
from pathlib import Path

import numpy

sys.path.insert(0, str(Path(__file__).resolve().parent))
from armor import MESHES, bound, chief_shader, chief_vertices, rows, split  # noqa: E402
from bsa import Archive  # noqa: E402
from fit import DEFAULT_SKYRIM, least_rotation  # noqa: E402
from nif import HAVOK_SCALE, VF_NORMAL, VF_TANGENT, VF_UV, VF_VERTEX, Nif, vertex_desc  # noqa: E402

TEMPLATE = "meshes\\armor\\nordplate\\{}_go.nif"
GROUND_VERTEX = vertex_desc(VF_VERTEX | VF_UV | VF_NORMAL | VF_TANGENT)
SLSF1_SKINNED = 0x2
# Steel Plate's: BSXFlags' (Havok, articulated, ...); a NIF's flags
BSX = 0xC2
FLAGS = 0x8000E
# the inventory's turn of a piece lying flat (milliradians about x), so it
# shows standing up, as Steel Plate's cuirass and gauntlets do
STAND_UP = (1570, 0, 0)
# the cuirass's triangles of these bones (by their names' brackets) are left
# out
LEFT_OUT = {"cuirass": {"LThg", "RThg"}}
# the gauntlets' and boots' halves' centres apart, in Skyrim's units
APART = {"gauntlets": 14.0, "boots": 15.0}


def rotation_about(axis, angle):
	"""The rotation by angle about a unit axis (Rodrigues')."""
	cross = numpy.array([[0, -axis[2], axis[1]], [axis[2], 0, -axis[0]], [-axis[1], axis[0], 0]])
	return numpy.cos(angle) * numpy.eye(3) + numpy.sin(angle) * cross + (1 - numpy.cos(angle)) * numpy.outer(axis, axis)


def dominant(vertices, face):
	totals = {}
	for index in face:
		for bone, weight in zip(vertices[index]["bones"], vertices[index]["weights"]):
			totals[bone] = totals.get(bone, 0) + weight
	return max(totals, key=totals.get)


def bracket(bone):
	return bone[bone.index("[") + 1:-1]


def side(bone):
	"""L or R, of a side's bone (Skyrim's L is at -x)."""
	return bracket(bone)[0]


def bone_axis(fitted, name):
	"""A Skyrim bone's direction (down its z) at rest, and its joint."""
	matrix = numpy.array(next(n["matrix"] for n in fitted["nodes"] if n["name"] == name))
	return matrix[:3, 2], matrix[:3, 3]


def pose(piece, fitted, shapes):
	"""Each of the piece's vertices' rotation and new position, as it lies:
	shape name -> (rotations, positions), by the piece's halves for a pair."""
	result = {}
	for name, (vertices, partitions) in shapes.items():
		positions = numpy.array([v["position"] for v in vertices])
		rotations = [numpy.eye(3)] * len(vertices)
		if piece == "cuirass":
			# on its back: his front (+y) up, his head towards -y
			turn = rotation_about(numpy.array([1.0, 0, 0]), numpy.pi / 2)
			rotations = [turn] * len(vertices)
			positions = positions @ turn.T
		elif piece in APART:
			moved = positions.copy()
			rotations = list(rotations)
			for half in "LR":
				which = [i for i, v in enumerate(vertices) if side(max(zip(v["weights"], v["bones"]))[1]) == half]
				outward = numpy.array([-1.0 if half == "L" else 1.0, 0, 0])
				if piece == "gauntlets":
					direction, joint = bone_axis(fitted, f"NPC {half} Forearm [{half}Lar]")
					# pointing away (+y), then turned about it so the back of
					# the hand (outward, at rest) is up
					turn = least_rotation(direction, numpy.array([0, 1.0, 0]))
					out = turn @ outward
					out[1] = 0
					angle = numpy.arctan2(numpy.cross(out, [0, 0, 1.0])[1], numpy.dot(out, [0, 0, 1.0]))
					turn = rotation_about(numpy.array([0, 1.0, 0]), angle) @ turn
				else:
					direction, joint = bone_axis(fitted, f"NPC {half} Calf [{half}Clf]")
					# the shin upright
					turn = least_rotation(direction, numpy.array([0, 0, -1.0]))
				points = (positions[which] - joint) @ turn.T
				points[:, 0] += (-1 if half == "L" else 1) * APART[piece] / 2 - (points[:, 0].min() + points[:, 0].max()) / 2
				moved[which] = points
				for i in which:
					rotations[i] = turn
			positions = moved
		result[name] = (rotations, positions)
	# all of the piece's shapes together: on the ground, centred across it
	everything = numpy.concatenate([result[name][1][sorted({i for _, faces in partitions for face in faces for i in face})]
		for name, (_, partitions) in shapes.items()])
	low, high = everything.min(axis=0), everything.max(axis=0)
	offset = numpy.array([-(low[0] + high[0]) / 2, -(low[1] + high[1]) / 2, -low[2]])
	return {name: (r, p + offset) for name, (r, p) in result.items()}


def dop_hull(points, radius):
	"""The 26-sided bounding polyhedron of points, its faces moved in by
	radius: its vertices and its faces' planes (normal, -distance, the
	distance before moving in, as Havok keeps them)."""
	normals = [numpy.array(d, dtype=float) for d in itertools.product((-1, 0, 1), repeat=3) if any(d)]
	normals = [n / numpy.linalg.norm(n) for n in normals]
	distances = [float((points @ n).max()) for n in normals]
	inner = [d - radius for d in distances]
	vertices = []
	for a, b, c in itertools.combinations(range(len(normals)), 3):
		matrix = numpy.array([normals[a], normals[b], normals[c]])
		if abs(numpy.linalg.det(matrix)) < 1e-6:
			continue
		point = numpy.linalg.solve(matrix, [inner[a], inner[b], inner[c]])
		if all(numpy.dot(n, point) <= d + 1e-4 for n, d in zip(normals, inner)):
			if not any(numpy.linalg.norm(point - v) < 1e-3 for v in vertices):
				vertices.append(point)
	vertices = numpy.array(vertices)
	planes = []
	for n, d, i in zip(normals, distances, inner):
		# a face: a plane at least three vertices lie on
		if numpy.sum(abs(vertices @ n - i) < 1e-3) >= 3:
			planes.append((*n, -d))
	return vertices, planes


def template(data_dir, piece, cache):
	path = TEMPLATE.format(piece)
	target = cache / f"{piece}_go.nif"
	if not target.exists():
		target.parent.mkdir(parents=True, exist_ok=True)
		target.write_bytes(Archive(Path(data_dir) / "Skyrim - Meshes0.bsa").read(path))
	return Nif(target)


def build(piece, fitted, shapes, steel_plate):
	shapes = {name: (vertices, [(part, [face for face in faces if bracket(dominant(vertices, face)) not in LEFT_OUT.get(piece, ())])
		for part, faces in partitions]) for name, (vertices, partitions) in shapes.items()}
	posed = pose(piece, fitted, shapes)
	nif = Nif()
	root = {"type": "BSFadeNode", "name": f"Mjolnir{piece.title()}GO", "extra_data": [], "controller": -1, "flags": FLAGS,
		"translation": (0.0, 0.0, 0.0), "rotation": rows(numpy.eye(4)), "scale": 1.0, "collision": -1, "children": [], "effects": []}
	nif.blocks.append(root)
	def add(block):
		nif.blocks.append(block)
		return len(nif.blocks) - 1
	root["extra_data"].append(add({"type": "BSXFlags", "name": "BSX", "value": BSX}))
	if piece in ("cuirass", "gauntlets"):
		root["extra_data"].append(add({"type": "BSInvMarker", "name": "INV", "rotation": STAND_UP, "zoom": 1.0}))
	everything = []
	for name, (vertices, partitions) in shapes.items():
		rotations, positions = posed[name]
		faces = [(a, c, b) for _, fs in partitions for a, b, c in fs]  # clockwise (Halo's) to Skyrim's
		used = sorted({i for face in faces for i in face})
		if not used:
			continue
		remap = {old: new for new, old in enumerate(used)}
		source = [dict(vertices[i], position=positions[i].tolist(), normal=(rotations[i] @ numpy.array(vertices[i]["normal"])).tolist())
			for i in used]
		faces = [tuple(remap[i] for i in face) for face in faces]
		ground, _ = chief_vertices(source, faces)
		for vertex in ground:
			del vertex["weights"], vertex["bones"]
		shader, textures = chief_shader(name)
		shader.pop("vertex_desc")
		shader["flags1"] &= ~SLSF1_SKINNED
		points = positions[used]
		everything.append(points)
		texture_set = add({"type": "BSShaderTextureSet", "textures": textures})
		shader_index = add(dict(shader, texture_set=texture_set))
		root["children"].append(add({"type": "BSTriShape", "name": f"Mjolnir{name.title()}", "extra_data": [], "controller": -1,
			"flags": FLAGS, "translation": (0.0, 0.0, 0.0), "rotation": rows(numpy.eye(4)), "scale": 1.0, "collision": -1,
			"bound": bound(points), "skin": -1, "shader_property": shader_index, "alpha_property": -1,
			"vertex_desc": GROUND_VERTEX, "vertices": ground, "triangles": faces, "particle_data_size": 0}))
	# the hull, in Havok's units
	points = numpy.concatenate(everything) / HAVOK_SCALE
	shape_template = next(b for b in steel_plate.blocks if b["type"] == "bhkConvexVerticesShape")
	body_template = next(b for b in steel_plate.blocks if b["type"] == "bhkRigidBody")
	extent = points.max(axis=0) - points.min(axis=0)
	radius = min(shape_template["radius"], extent.min() / 4)
	hull, planes = dop_hull(points, radius)
	shape = add({"type": "bhkConvexVerticesShape", "material": shape_template["material"], "radius": radius,
		"properties": shape_template["properties"], "vertices": [(*v, 0.0) for v in hull.tolist()], "planes": planes})
	body = {k: v for k, v in body_template.items() if k != "raw"}
	low, high = hull.min(axis=0) - radius, hull.max(axis=0) + radius
	size = high - low
	mass = body["mass"]
	inertia = mass / 12 * numpy.array([size[1] ** 2 + size[2] ** 2, size[0] ** 2 + size[2] ** 2, size[0] ** 2 + size[1] ** 2])
	body.update(shape=shape, center=(*((low + high) / 2).tolist(), 0.0),
		inertia=(inertia[0], 0, 0, 0, 0, inertia[1], 0, 0, 0, 0, inertia[2], 0))
	body_index = add(body)
	root["collision"] = add({"type": "bhkCollisionObject", "target": 0, "flags": 0x81, "body": body_index})
	return nif, hull * HAVOK_SCALE, radius * HAVOK_SCALE


def main():
	parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
	parser.add_argument("--skyrim", default=str(DEFAULT_SKYRIM), help="Skyrim's Data directory")
	parser.add_argument("--dir", default=str(Path(__file__).resolve().parents[2] / "build" / "arsenal"))
	args = parser.parse_args()
	directory = Path(args.dir)
	fitted = json.loads((directory / "fitted.json").read_text())
	pieces, _ = split(fitted)
	out = directory / "Data" / Path(MESHES.replace("\\", "/"))
	out.mkdir(parents=True, exist_ok=True)
	for piece, shapes in pieces.items():
		nif, hull, radius = build(piece, fitted, shapes, template(args.skyrim, piece, directory / "skyrim"))
		path = out / f"{piece}_go.nif"
		nif.write(path)
		if Nif(path).write() != path.read_bytes():
			raise SystemExit(f"{path}: doesn't read back as written")
		low, high = hull.min(axis=0) - radius, hull.max(axis=0) + radius
		print(f"wrote {path} ({path.stat().st_size} bytes; hull of {len(hull)} vertices,"
			f" {' x '.join(f'{v:.0f}' for v in high - low)} units)")


if __name__ == "__main__":
	sys.exit(main())
