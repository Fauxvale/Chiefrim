# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the model halo_model.py wrote (build/mjolnir/model.json), or
fit.py's fitted to Skyrim's skeleton (fitted.json), in Blender, with its
skeleton and skin weights, and renders it from the front, the side and the
back, so the port can be checked without opening Blender.

Usage: blender -b --factory-startup -P tools/mjolnir/blender_preview.py -- [--dir build/mjolnir]
         [--fitted] [--pose] [--bones] [--blend]
  --fitted  the model fitted to Skyrim's skeleton (preview_fitted_*.png)
  --pose    bend an arm, a knee and the head first (preview_*pose_*.png): the
            skeleton and the skin weights are right if the armor follows
  --bones   draw the skeleton over the model
  --blend   also save the scene there

Halo's units are 10 feet (3.048 m); Halo is +x forward, +z up, and its
bones run down their nodes' x. A Halo node's translation and rotation are
its parent's frame; its quaternions are stored conjugated (the rotation
from the node to its parent). Skyrim's are 70 to the metre, +y forward,
and its bones run down their z; fitted.json gives their rest matrices.
"""
import argparse
import json
import math
import sys
from pathlib import Path

import bpy
from mathutils import Matrix, Quaternion, Vector

HALO_UNIT = 3.048
AXES = {"x": Vector((1, 0, 0)), "z": Vector((0, 0, 1))}


def node_matrices(model):
	unit = model.get("unit", HALO_UNIT)
	matrices = []
	for node in model["nodes"]:
		if "matrix" in node:
			matrix = Matrix(node["matrix"])
			matrix.translation = matrix.translation * unit
			matrices.append(matrix)
			continue
		i, j, k, w = node["rotation"]
		local = Matrix.Translation(Vector(node["translation"]) * unit) @ Quaternion((w, i, j, k)).conjugated().to_matrix().to_4x4()
		matrices.append(local if node["parent"] < 0 else matrices[node["parent"]] @ local)
	return matrices


def build_armature(model, matrices):
	nodes = model["nodes"]
	axis = AXES[model.get("bone_axis", "x")]
	up = AXES["z"] if axis == AXES["x"] else Vector((0, 1, 0))
	data = bpy.data.armatures.new("skeleton")
	armature = bpy.data.objects.new("skeleton", data)
	bpy.context.collection.objects.link(armature)
	bpy.context.view_layer.objects.active = armature
	bpy.ops.object.mode_set(mode="EDIT")
	bones = []
	for index, node in enumerate(nodes):
		bone = data.edit_bones.new(node["name"])
		head = matrices[index].to_translation()
		children = [c for c, n in enumerate(nodes) if n["parent"] == index]
		# a bone points down its node's axis, as far as its first child or by
		# a stub
		direction = matrices[index].to_3x3().normalized() @ axis
		length = (matrices[children[0]].to_translation() - head).length if children else 0.08
		bone.head = head
		bone.tail = head + direction * max(length, 0.02)
		bone.align_roll(matrices[index].to_3x3().normalized() @ up)
		if node["parent"] >= 0:
			bone.parent = bones[node["parent"]]
		bones.append(bone)
	bpy.ops.object.mode_set(mode="OBJECT")
	return armature


def build_materials(directory, shaders):
	"""The baked colour (halo_model.py's bake()) as the base colour, its glow
	as emission; the shield's effect shaders are not drawn."""
	materials = []
	for shader in shaders:
		material = bpy.data.materials.new(Path(shader["name"].replace("\\", "/")).name)
		tree = material.node_tree
		bsdf = tree.nodes["Principled BSDF"]
		baked = shader.get("baked")
		if not baked:
			bsdf.inputs["Alpha"].default_value = 0.0
			materials.append(material)
			continue
		color = tree.nodes.new("ShaderNodeTexImage")
		color.image = bpy.data.images.load(str(directory / baked["color"]))
		tree.links.new(color.outputs["Color"], bsdf.inputs["Base Color"])
		glow = tree.nodes.new("ShaderNodeTexImage")
		glow.image = bpy.data.images.load(str(directory / baked["glow"]))
		tree.links.new(glow.outputs["Color"], bsdf.inputs["Emission Color"])
		bsdf.inputs["Emission Strength"].default_value = 2.0
		reflection = tree.nodes.new("ShaderNodeTexImage")
		reflection.image = bpy.data.images.load(str(directory / baked["reflection"]))
		reflection.image.colorspace_settings.name = "Non-Color"
		# Halo adds its reflection over the colour: specular, not metal
		bsdf.inputs["Metallic"].default_value = 0.0
		tree.links.new(reflection.outputs["Color"], bsdf.inputs["Specular IOR Level"])
		bsdf.inputs["Roughness"].default_value = 0.45
		materials.append(material)
	return materials


def build_mesh(model, materials, armature):
	unit = model.get("unit", HALO_UNIT)
	names = [node["name"] for node in model["nodes"]]
	positions, triangles, uvs, weights, material_indices, normals = [], [], [], [], [], []
	for part in model["parts"]:
		first = len(positions)
		for vertex in part["vertices"]:
			positions.append(Vector(vertex["position"]) * unit)
			normals.append(Vector(vertex["normal"]))
			# Halo's vertices name nodes by index, fitted ones bones by name
			bones = vertex.get("bones") or [names[n] if n >= 0 else None for n in vertex["nodes"]]
			weights.append(list(zip(bones, vertex["weights"])))
		for triangle in part["triangles"]:
			triangles.append([first + i for i in triangle])
			uvs.append([part["vertices"][i]["uv"] for i in triangle])
			material_indices.append(part["shader"])
	mesh = bpy.data.meshes.new("mjolnir")
	mesh.from_pydata(positions, [], triangles)
	for material in materials:
		mesh.materials.append(material)
	layer = mesh.uv_layers.new(name="UVMap")
	for polygon, corners, material_index in zip(mesh.polygons, uvs, material_indices):
		polygon.material_index = material_index
		polygon.use_smooth = True
		for loop, uv in zip(polygon.loop_indices, corners):
			# Halo's v runs down the bitmap
			layer.data[loop].uv = (uv[0], 1 - uv[1])
	mesh.normals_split_custom_set_from_vertices(normals)
	obj = bpy.data.objects.new("mjolnir", mesh)
	bpy.context.collection.objects.link(obj)
	groups = {name: obj.vertex_groups.new(name=name) for name in names}
	for index, pairs in enumerate(weights):
		for bone, weight in pairs:
			if bone is not None and weight > 0:
				groups[bone].add([index], weight, "ADD")
	obj.parent = armature
	modifier = obj.modifiers.new("skeleton", "ARMATURE")
	modifier.object = armature
	return obj


# per skeleton, bone: (its pose axis, degrees)
POSE_TEST = {
	"halo": {"bip01 l upperarm": ("Z", -70), "bip01 l forearm": ("Z", 60), "bip01 r calf": ("Z", 70), "bip01 r thigh": ("Z", -40), "bip01 head": ("Y", 30)},
	"skyrim": {"NPC L UpperArm [LUar]": ("X", -70), "NPC L Forearm [LLar]": ("X", -60), "NPC R Calf [RClf]": ("X", 70), "NPC R Thigh [RThg]": ("X", -40), "NPC Head [Head]": ("Z", 30)},
}
# per skeleton, the model's front
FRONT = {"halo": 0, "skyrim": 90}


def pose(armature, frame):
	for name, (axis, degrees) in POSE_TEST[frame].items():
		bone = armature.pose.bones[name]
		bone.rotation_mode = "XYZ"
		setattr(bone.rotation_euler, axis.lower(), math.radians(degrees))


def render(directory, obj, armature, show_bones, prefix, front):
	scene = bpy.context.scene
	for engine in ("BLENDER_EEVEE", "BLENDER_EEVEE_NEXT", "CYCLES"):
		try:
			scene.render.engine = engine
			break
		except TypeError:
			continue
	if scene.render.engine == "CYCLES":
		scene.cycles.samples = 32
	scene.render.resolution_x, scene.render.resolution_y = 768, 1024
	scene.render.film_transparent = False
	# no tone mapping: the baked colours as they are
	scene.view_settings.view_transform = "Standard"
	world = bpy.data.worlds.new("preview")
	world.use_nodes = True
	world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.35, 0.37, 0.40, 1)
	world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.6
	scene.world = world
	sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
	sun.data.energy = 4.0
	sun.rotation_euler = (math.radians(50), 0, math.radians(-30))
	scene.collection.objects.link(sun)
	low, high = (Vector([f(v[i] for v in [obj.matrix_world @ Vector(c) for c in obj.bound_box]) for i in range(3)]) for f in (min, max))
	centre = (low + high) / 2
	height = high.z - low.z
	camera = bpy.data.objects.new("camera", bpy.data.cameras.new("camera"))
	camera.data.type = "ORTHO"
	camera.data.ortho_scale = height * 1.1
	scene.collection.objects.link(camera)
	scene.camera = camera
	armature.show_in_front = show_bones
	armature.hide_render = not show_bones
	if show_bones:
		armature.data.display_type = "STICK"
	for name, angle in (("front", 0), ("side", 90), ("back", 180)):
		direction = Vector((math.cos(math.radians(angle + front)), math.sin(math.radians(angle + front)), 0))
		camera.location = centre + direction * 10
		camera.rotation_euler = (-direction).to_track_quat("-Z", "Y").to_euler()
		scene.render.filepath = str(directory / f"{prefix}_{name}.png")
		bpy.ops.render.render(write_still=True)
		print(f"wrote {scene.render.filepath}")


def main():
	argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
	parser = argparse.ArgumentParser()
	parser.add_argument("--dir", default=str(Path(__file__).resolve().parents[2] / "build" / "mjolnir"))
	parser.add_argument("--bones", action="store_true")
	parser.add_argument("--blend", action="store_true")
	parser.add_argument("--pose", action="store_true")
	parser.add_argument("--fitted", action="store_true")
	args = parser.parse_args(argv)
	directory = Path(args.dir).resolve()
	model = json.loads((directory / ("fitted.json" if args.fitted else "model.json")).read_text())
	frame = model.get("frame", "halo")
	for obj in list(bpy.data.objects):
		bpy.data.objects.remove(obj)
	armature = build_armature(model, node_matrices(model))
	obj = build_mesh(model, build_materials(directory, model["shaders"]), armature)
	if args.pose:
		pose(armature, frame)
	prefix = "preview" + ("_fitted" if args.fitted else "") + ("_pose" if args.pose else "")
	render(directory, obj, armature, args.bones, prefix, FRONT[frame])
	if args.blend:
		bpy.ops.wm.save_as_mainfile(filepath=str(directory / f"{prefix}.blend"))


main()
