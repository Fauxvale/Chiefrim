#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fits the model halo_model.py wrote (build/mjolnir/model.json) to
Skyrim's skeleton, read from the user's own Skyrim, for the Mjolnir armor
port: writes build/mjolnir/fitted.json, the same model in Skyrim's units
and frame, bound to Skyrim's bones at their rest pose.

Each of Halo's nodes gets a transform from its rest pose to the fitted one:
its joint moved onto its Skyrim bone's, its segment (to the joint BONES
names after it) turned by the least rotation onto the Skyrim segment and
stretched along itself to that length, and across it scaled by one factor
for the whole model (Skyrim's height over Halo's). A node with no segment
is turned as its parent is (the feet, the hands), or by the least rotation
from its bone's direction to its Skyrim bone's (the pelvis, the head: both
skeletons hold them upright; a Halo foot's bone points down, a Skyrim
foot's to its toes).
The vertices are moved by their nodes' transforms, blended by their
weights, as a skinned mesh is; the weights then name the Skyrim bones.

Halo is +x forward, +y left, +z up, in units of 10 feet; Skyrim is +y
forward, +x right, +z up, 70 units to the metre.

Usage: tools/mjolnir/fit.py [--skyrim DATA_DIR] [--dir build/mjolnir] [--shoulders 1]
         [--torso 1] [--head T] [--arms T]
"""
import argparse
import json
import sys
from pathlib import Path

import numpy

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bsa import Archive  # noqa: E402
from nif import Nif  # noqa: E402

HALO_UNIT = 3.048
SKYRIM_UNIT = 1 / 70
SKELETON = "meshes\\actors\\character\\character assets\\skeleton.nif"
DEFAULT_SKYRIM = Path.home() / ".local/share/Steam/steamapps/common/Skyrim Special Edition/Data"

# Halo's node: its Skyrim bone, and the Halo and Skyrim joints its segment
# runs to (None: turned as its parent; "axes": turned from its bone's
# direction to its Skyrim bone's)
BONES = {
	"bip01 pelvis": ("NPC Pelvis [Pelv]", "axes"),
	"bip01 spine": ("NPC Spine1 [Spn1]", ("bip01 spine1", "NPC Spine2 [Spn2]")),
	"bip01 spine1": ("NPC Spine2 [Spn2]", ("bip01 neck", "NPC Neck [Neck]")),
	"bip01 neck": ("NPC Neck [Neck]", ("bip01 head", "NPC Head [Head]")),
	"bip01 head": ("NPC Head [Head]", "axes"),
}
for side, skyrim in (("l", "L"), ("r", "R")):
	BONES.update({
		f"bip01 {side} thigh": (f"NPC {skyrim} Thigh [{skyrim}Thg]", (f"bip01 {side} calf", f"NPC {skyrim} Calf [{skyrim}Clf]")),
		f"bip01 {side} calf": (f"NPC {skyrim} Calf [{skyrim}Clf]", (f"bip01 {side} foot", f"NPC {skyrim} Foot [{skyrim}ft ]")),
		f"bip01 {side} foot": (f"NPC {skyrim} Foot [{skyrim}ft ]", None),
		f"bip01 {side} clavicle": (f"NPC {skyrim} Clavicle [{skyrim}Clv]", (f"bip01 {side} upperarm", f"NPC {skyrim} UpperArm [{skyrim}Uar]")),
		f"bip01 {side} upperarm": (f"NPC {skyrim} UpperArm [{skyrim}Uar]", (f"bip01 {side} forearm", f"NPC {skyrim} Forearm [{skyrim}Lar]")),
		f"bip01 {side} forearm": (f"NPC {skyrim} Forearm [{skyrim}Lar]", (f"bip01 {side} hand", f"NPC {skyrim} Hand [{skyrim}Hnd]")),
		f"bip01 {side} hand": (f"NPC {skyrim} Hand [{skyrim}Hnd]", None),
	})
# the spine's Skyrim joint is Spine's (Spn0), though its weights go to Spine1
JOINTS = {"bip01 spine": "NPC Spine [Spn0]"}

# Halo's frame and units to Skyrim's
HALO_TO_SKYRIM = numpy.array([[0, -1, 0], [1, 0, 0], [0, 0, 1]]) * HALO_UNIT / SKYRIM_UNIT


def quaternion_matrix(i, j, k, w):
	return numpy.array([
		[1 - 2 * (j * j + k * k), 2 * (i * j - k * w), 2 * (i * k + j * w)],
		[2 * (i * j + k * w), 1 - 2 * (i * i + k * k), 2 * (j * k - i * w)],
		[2 * (i * k - j * w), 2 * (j * k + i * w), 1 - 2 * (i * i + j * j)]])


def halo_world(nodes):
	"""Halo's nodes' rest matrices (4x4, Halo's frame); its quaternions are
	stored conjugated."""
	result = []
	for node in nodes:
		i, j, k, w = node["rotation"]
		local = numpy.eye(4)
		local[:3, :3] = quaternion_matrix(-i, -j, -k, w)
		local[:3, 3] = node["translation"]
		result.append(local if node["parent"] < 0 else result[node["parent"]] @ local)
	return result


def skyrim_skeleton(data_dir):
	"""Skyrim's skeleton's bones: name -> (parent name, world matrix 4x4),
	from loose files first, as the game takes them, then its archive."""
	loose = Path(data_dir) / SKELETON.replace("\\", "/")
	if loose.exists():
		nif = Nif(loose)
	else:
		path = Path(data_dir) / "Skyrim - Meshes0.bsa"
		target = Path(__file__).resolve().parents[2] / "build" / "mjolnir" / "skyrim" / "skeleton.nif"
		target.parent.mkdir(parents=True, exist_ok=True)
		target.write_bytes(Archive(path).read(SKELETON))
		nif = Nif(target)
	world, bones = {}, {}
	for _, index, block, parent in nif.walk():
		local = numpy.eye(4)
		local[:3, :3] = numpy.array(block["rotation"]) * block["scale"]
		local[:3, 3] = block["translation"]
		world[index] = (world[parent] if parent is not None else numpy.eye(4)) @ local
		bones[block["name"]] = (nif.blocks[parent]["name"] if parent is not None else None, world[index])
	return bones


def least_rotation(a, b):
	"""The least rotation taking direction a to direction b."""
	a, b = a / numpy.linalg.norm(a), b / numpy.linalg.norm(b)
	axis = numpy.cross(a, b)
	s, c = numpy.linalg.norm(axis), numpy.dot(a, b)
	if s < 1e-9:
		return numpy.eye(3)
	k = numpy.array([[0, -axis[2], axis[1]], [axis[2], 0, -axis[0]], [-axis[1], axis[0], 0]]) / s
	return numpy.eye(3) + s * k + (1 - c) * k @ k


def fit(model, bones, shoulders=1.0, torso=1.0, head=1.0, arms=1.0):
	names = [node["name"] for node in model["nodes"]]
	halo_matrices = dict(zip(names, halo_world(model["nodes"])))
	halo = {name: HALO_TO_SKYRIM @ matrix[:3, 3] for name, matrix in halo_matrices.items()}
	skyrim = {name: matrix[:3, 3] for name, (_, matrix) in bones.items()}
	def skyrim_joint(name):
		return skyrim[JOINTS.get(name, BONES[name][0])]
	# one scale across: the head's height over Halo's
	across = skyrim_joint("bip01 head")[2] / halo["bip01 head"][2]
	# Skyrim's shoulders are narrower than Chief's: the arms are fitted to
	# virtual shoulder joints out at his width (shoulders 1) and bound to
	# Skyrim's, so his chest keeps its width and his upper arms don't sink
	# into it. The elbows and hands stay Skyrim's (a weapon is held at the
	# hand bone), and the upper arms hang as steeply as Halo's do.
	skyrim = dict(skyrim)
	for side in "lr":
		bone = BONES[f"bip01 {side} upperarm"][0]
		joint = skyrim[bone].copy()
		width = abs(halo[f"bip01 {side} upperarm"][0]) * across
		joint[0] = numpy.sign(joint[0]) * (abs(joint[0]) + shoulders * (width - abs(joint[0])))
		skyrim[bone] = joint
	# Skyrim's spine runs 7.5 cm behind its hips, as a person's does, and
	# Skyrim's body sits forward of it; Halo's runs straight up from Chief's
	# hips, through the middle of him. So his spine's joints (and his
	# collarbones) are fitted as far forward of the pelvis as Halo puts them
	# (torso 1), and his torso keeps its place over his hips; bending about
	# Skyrim's joints behind it is how Skyrim's own body bends. His head and
	# shoulders may follow (head, arms 1), or stay on Skyrim's joints (0):
	# fitted forward of them, a nod or an arm raised forward slips a little.
	pelvis = skyrim[BONES["bip01 pelvis"][0]][1]
	moved = [("bip01 spine", torso), ("bip01 spine1", torso), ("bip01 neck", torso), ("bip01 head", head)]
	moved += [(f"bip01 {side} {part}", fraction) for side in "lr" for part, fraction in (("clavicle", torso), ("upperarm", arms))]
	for name, fraction in moved:
		bone = JOINTS.get(name, BONES[name][0])
		joint = skyrim[bone].copy()
		forward = pelvis + (halo[name][1] - halo["bip01 pelvis"][1]) * across
		joint[1] += fraction * (forward - joint[1])
		skyrim[bone] = joint
	# Chief's knee-to-sole is longer than Skyrim's knee-to-ground (Halo's
	# ankle is 20 cm up his boot, Skyrim's 9): his lower legs are squashed
	# upright by Skyrim's knee height over his, to an ankle as far up as his
	# boot puts it, so his soles meet the ground. The boot's shaft above
	# Skyrim's ankle is then weighted to the calf, as Skyrim's boots are, so
	# only the foot turns at the ankle.
	ankles, squash = {}, {}
	for side in "lr":
		foot, calf = f"bip01 {side} foot", f"bip01 {side} calf"
		k = skyrim[BONES[calf][0]][2] / (halo[calf][2] * across)
		bone = BONES[foot][0]
		ankles[bone] = skyrim[bone][2]
		joint = skyrim[bone].copy()
		joint[2] = halo[foot][2] * across * k
		skyrim[bone] = joint
		squash[foot] = k
	transforms, rotations = {}, {}
	for index, name in enumerate(names):
		bone, segment = BONES[name]
		parent = model["nodes"][index]["parent"]
		if segment is None:
			rotation = rotations[names[parent]] if parent >= 0 else numpy.eye(3)
			upright = numpy.diag([1, 1, squash.get(name, 1)])
			linear = rotation @ upright * across
		elif segment == "axes":
			# Halo's bones run down their x, Skyrim's down their z
			rotation = least_rotation(HALO_TO_SKYRIM @ halo_matrices[name][:3, 0], bones[bone][1][:3, 2])
			linear = rotation * across
		else:
			halo_direction = halo[segment[0]] - halo[name]
			skyrim_direction = skyrim[segment[1]] - skyrim_joint(name)
			along = numpy.linalg.norm(skyrim_direction) / numpy.linalg.norm(halo_direction)
			d = halo_direction / numpy.linalg.norm(halo_direction)
			stretch = across * numpy.eye(3) + (along - across) * numpy.outer(d, d)
			rotation = least_rotation(halo_direction, skyrim_direction)
			linear = rotation @ stretch
		rotations[name] = rotation
		transform = numpy.eye(4)
		transform[:3, :3] = linear
		transform[:3, 3] = skyrim_joint(name) - linear @ halo[name]
		transforms[name] = transform
	parts = []
	for part in model["parts"]:
		vertices = []
		for vertex in part["vertices"]:
			position = HALO_TO_SKYRIM @ numpy.array(vertex["position"])
			normal = HALO_TO_SKYRIM @ numpy.array(vertex["normal"])
			blended = sum(w * transforms[names[n]] for n, w in zip(vertex["nodes"], vertex["weights"]) if n >= 0 and w > 0)
			moved = blended[:3, :3] @ position + blended[:3, 3]
			# normals by the inverse transpose
			normal = numpy.linalg.inv(blended[:3, :3]).T @ normal
			normal /= numpy.linalg.norm(normal)
			weights = {}
			for n, w in zip(vertex["nodes"], vertex["weights"]):
				if n >= 0 and w > 0:
					bone = BONES[names[n]][0]
					weights[bone] = weights.get(bone, 0) + w
			for foot, ankle in ankles.items():
				if foot in weights:
					# 2 cm either side of the ankle, from the foot to the calf
					t = numpy.clip((moved[2] - ankle) / (0.04 / SKYRIM_UNIT) + 0.5, 0, 1)
					t = t * t * (3 - 2 * t)
					calf = bones[foot][0]
					weights[calf] = weights.get(calf, 0) + weights[foot] * t
					weights[foot] *= 1 - t
					if weights[foot] == 0:
						del weights[foot]
			vertices.append({"position": moved.tolist(), "normal": normal.tolist(), "uv": vertex["uv"],
				"bones": list(weights), "weights": list(weights.values())})
		parts.append({"shader": part["shader"], "vertices": vertices, "triangles": part["triangles"]})
	# the Skyrim bones the fit uses, and their ancestors, rest matrices in
	# Skyrim's units
	used, nodes = set(), []
	for bone, _ in BONES.values():
		while bone is not None and bone not in used:
			used.add(bone)
			bone = bones[bone][0]
	order = [name for name in bones if name in used]
	for name in order:
		parent, matrix = bones[name]
		nodes.append({"name": name, "parent": order.index(parent) if parent in used else -1, "matrix": matrix.tolist()})
	return {"name": model["name"], "frame": "skyrim", "unit": SKYRIM_UNIT, "bone_axis": "z", "across": across, "shoulders": shoulders, "torso": torso, "head": head, "arms": arms,
		"nodes": nodes, "shaders": model["shaders"], "parts": parts}


def main():
	parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
	parser.add_argument("--skyrim", default=str(DEFAULT_SKYRIM), help="Skyrim's Data directory")
	parser.add_argument("--dir", default=str(Path(__file__).resolve().parents[2] / "build" / "mjolnir"))
	parser.add_argument("--shoulders", type=float, default=1.0,
		help="how far the arms are fitted out from Skyrim's shoulders (0) to Chief's (1)")
	parser.add_argument("--torso", type=float, default=1.0,
		help="how far the torso is fitted forward, from Skyrim's spine (0) to over his hips as in Halo (1)")
	parser.add_argument("--head", type=float, help="the same for the head (default: --torso)")
	parser.add_argument("--arms", type=float, help="the same for the shoulder joints (default: --torso)")
	args = parser.parse_args()
	directory = Path(args.dir)
	model = json.loads((directory / "model.json").read_text())
	head = args.torso if args.head is None else args.head
	arms = args.torso if args.arms is None else args.arms
	fitted = fit(model, skyrim_skeleton(args.skyrim), args.shoulders, args.torso, head, arms)
	(directory / "fitted.json").write_text(json.dumps(fitted))
	print(f"fitted to Skyrim's skeleton (scaled {fitted['across']:.3f} across, shoulders {args.shoulders:g},"
		f" torso {args.torso:g}, head {head:g}, arms {arms:g})"
		f" -> {directory / 'fitted.json'}")


if __name__ == "__main__":
	sys.exit(main())
