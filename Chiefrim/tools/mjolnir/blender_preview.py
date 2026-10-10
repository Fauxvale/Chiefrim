# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the model halo_model.py wrote (build/mjolnir/model.json) in
Blender, with its skeleton and skin weights, and renders it from the front,
the side and the back, so the port can be checked without opening Blender.

Usage: blender -b --factory-startup -P tools/mjolnir/blender_preview.py -- [--dir build/mjolnir]
         [--bones] [--blend]
  --bones  draw the skeleton over the model
  --blend  also save preview.blend there
  --pose   bend the left arm up and the right knee first (preview_pose_*.png):
           the skeleton and the skin weights are right if the armor follows

Halo's units are 10 feet (3.048 m); Halo is +x forward, +z up. A node's
translation and rotation are its parent's frame; Halo's quaternions are
stored conjugated (the rotation from the node to its parent).
"""
import argparse
import json
import math
import sys
from pathlib import Path

import bpy
from mathutils import Matrix, Quaternion, Vector

HALO_UNIT = 3.048
# the cyborg's change colour (the campaign's olive drab), linear RGB
CHANGE_COLOR = (0.18, 0.22, 0.10)


def node_matrices(nodes):
	matrices = []
	for node in nodes:
		i, j, k, w = node["rotation"]
		local = Matrix.Translation(Vector(node["translation"]) * HALO_UNIT) @ Quaternion((w, i, j, k)).conjugated().to_matrix().to_4x4()
		matrices.append(local if node["parent"] < 0 else matrices[node["parent"]] @ local)
	return matrices


def build_armature(nodes, matrices):
	data = bpy.data.armatures.new("halo_skeleton")
	armature = bpy.data.objects.new("halo_skeleton", data)
	bpy.context.collection.objects.link(armature)
	bpy.context.view_layer.objects.active = armature
	bpy.ops.object.mode_set(mode="EDIT")
	bones = []
	for index, node in enumerate(nodes):
		bone = data.edit_bones.new(node["name"])
		head = matrices[index].to_translation()
		children = [c for c, n in enumerate(nodes) if n["parent"] == index]
		# a bone points down its node's x, to its first child or by a stub
		x_axis = matrices[index].to_3x3() @ Vector((1, 0, 0))
		length = (matrices[children[0]].to_translation() - head).length if children else 0.08
		bone.head = head
		bone.tail = head + x_axis * max(length, 0.02)
		bone.align_roll(matrices[index].to_3x3() @ Vector((0, 0, 1)))
		if node["parent"] >= 0:
			bone.parent = bones[node["parent"]]
		bones.append(bone)
	bpy.ops.object.mode_set(mode="OBJECT")
	return armature


def image(directory, file):
	result = bpy.data.images.load(str(directory / file))
	return result


def build_materials(directory, shaders):
	materials = []
	for shader in shaders:
		material = bpy.data.materials.new(Path(shader["name"]).name)
		material.use_nodes = True
		tree = material.node_tree
		bsdf = tree.nodes["Principled BSDF"]
		files = {Path(b["name"].replace("\\", "/")).name: b["file"] for b in shader["bitmaps"]}
		if shader["class"] != "soso" or "cyborg" not in files:
			# the shield's effect shaders: not drawn
			bsdf.inputs["Alpha"].default_value = 0.0
			material.blend_method = "BLEND" if hasattr(material, "blend_method") else None
			materials.append(material)
			continue
		base = tree.nodes.new("ShaderNodeTexImage")
		base.image = image(directory, files["cyborg"])
		multi = tree.nodes.new("ShaderNodeTexImage")
		multi.image = image(directory, files["cyborg multipurpose"])
		multi.image.colorspace_settings.name = "Non-Color"
		split = tree.nodes.new("ShaderNodeSeparateColor")
		tree.links.new(multi.outputs["Color"], split.inputs["Color"])
		# the base map times the change colour where the multipurpose map's
		# blue says so
		tint = tree.nodes.new("ShaderNodeMix")
		tint.data_type = "RGBA"
		tint.blend_type = "MULTIPLY"
		tint.inputs["B"].default_value = (*CHANGE_COLOR, 1)
		tree.links.new(split.outputs["Blue"], tint.inputs["Factor"])
		tree.links.new(base.outputs["Color"], tint.inputs["A"])
		tree.links.new(tint.outputs["Result"], bsdf.inputs["Base Color"])
		bsdf.inputs["Metallic"].default_value = 0.6
		bsdf.inputs["Roughness"].default_value = 0.45
		# the multipurpose map's green is self-illumination (the lights)
		emission = tree.nodes.new("ShaderNodeMix")
		emission.data_type = "RGBA"
		emission.inputs["A"].default_value = (0, 0, 0, 1)
		tree.links.new(split.outputs["Green"], emission.inputs["Factor"])
		tree.links.new(base.outputs["Color"], emission.inputs["B"])
		tree.links.new(emission.outputs["Result"], bsdf.inputs["Emission Color"])
		bsdf.inputs["Emission Strength"].default_value = 2.0
		materials.append(material)
	return materials


def build_mesh(model, materials, armature):
	positions, triangles, uvs, weights, material_indices, normals = [], [], [], [], [], []
	for part in model["parts"]:
		first = len(positions)
		for vertex in part["vertices"]:
			positions.append(Vector(vertex["position"]) * HALO_UNIT)
			normals.append(Vector(vertex["normal"]))
			weights.append(list(zip(vertex["nodes"], vertex["weights"])))
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
	groups = [obj.vertex_groups.new(name=node["name"]) for node in model["nodes"]]
	for index, pairs in enumerate(weights):
		for node, weight in pairs:
			if node >= 0 and weight > 0:
				groups[node].add([index], weight, "ADD")
	obj.parent = armature
	modifier = obj.modifiers.new("skeleton", "ARMATURE")
	modifier.object = armature
	return obj


POSE_TEST = {"bip01 l upperarm": ("Z", -70), "bip01 l forearm": ("Z", 60), "bip01 r calf": ("Z", 70), "bip01 r thigh": ("Z", -40), "bip01 head": ("Y", 30)}


def pose(armature):
	for name, (axis, degrees) in POSE_TEST.items():
		bone = armature.pose.bones[name]
		bone.rotation_mode = "XYZ"
		setattr(bone.rotation_euler, axis.lower(), math.radians(degrees))


def render(directory, obj, armature, show_bones, prefix="preview"):
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
	# Halo's +x is the model's front
	for name, angle in (("front", 0), ("side", 90), ("back", 180)):
		direction = Vector((math.cos(math.radians(angle)), math.sin(math.radians(angle)), 0))
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
	args = parser.parse_args(argv)
	directory = Path(args.dir).resolve()
	model = json.loads((directory / "model.json").read_text())
	for obj in list(bpy.data.objects):
		bpy.data.objects.remove(obj)
	armature = build_armature(model["nodes"], node_matrices(model["nodes"]))
	obj = build_mesh(model, build_materials(directory, model["shaders"]), armature)
	if args.pose:
		pose(armature)
	render(directory, obj, armature, args.bones, "preview_pose" if args.pose else "preview")
	if args.blend:
		bpy.ops.wm.save_as_mainfile(filepath=str(directory / "preview.blend"))


main()
