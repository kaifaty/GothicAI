"""Render GLB props with one camera and one light for side-by-side comparison.

blender -b --factory-startup -P compare.py -- config.json
config: {"out_dir": str, "size": int, "target_height": float,
         "maps_dir": str, "scales": {KEY: float},
         "items": [{"name": str, "glb": str, "materials": "ours"|"own"}]}
Writes <out_dir>/<name>-<view>.png per view and <name>-wire.png from the last view.
views: [{"name", "elevation", "azimuth", "ortho"}], default one 3/4 view.
"""
import json
import math
import sys
from pathlib import Path

import bpy
from mathutils import Vector

config = json.loads(Path(sys.argv[sys.argv.index("--") + 1]).read_text())
out_dir = Path(config["out_dir"])
out_dir.mkdir(parents=True, exist_ok=True)


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.samples = config.get("samples", 64)
    scene.cycles.use_denoising = True
    scene.render.resolution_x = scene.render.resolution_y = config.get("size", 768)
    scene.render.film_transparent = False
    scene.view_settings.view_transform = "AgX"
    world = bpy.data.worlds.new("world")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs[0].default_value = (0.55, 0.6, 0.68, 1)
    world.node_tree.nodes["Background"].inputs[1].default_value = 0.6
    scene.world = world
    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
    sun.data.energy = 3.5
    sun.data.angle = math.radians(3)
    sun.data.color = (1.0, 0.95, 0.88)
    sun.rotation_euler = (math.radians(50), 0, math.radians(35))
    scene.collection.objects.link(sun)
    ground = bpy.data.meshes.new("ground")
    ground.from_pydata([(-60, -60, 0), (60, -60, 0), (60, 60, 0), (-60, 60, 0)], [], [(0, 1, 2, 3)])
    ground_obj = bpy.data.objects.new("ground", ground)
    mat = bpy.data.materials.new("ground")
    mat.use_nodes = True
    mat.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.18, 0.17, 0.16, 1)
    ground.materials.append(mat)
    scene.collection.objects.link(ground_obj)
    return scene


def image_node(nodes, path, colour):
    node = nodes.new("ShaderNodeTexImage")
    node.image = bpy.data.images.load(str(path))
    node.image.colorspace_settings.name = "sRGB" if colour else "Non-Color"
    return node


def our_material(mat):
    key = mat.name.split(".")[0]
    folder = Path(config["maps_dir"]) / key.lower()
    if not folder.exists():
        return
    mat.use_nodes = True
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    nodes.clear()
    out = nodes.new("ShaderNodeOutputMaterial")
    bsdf = nodes.new("ShaderNodeBsdfPrincipled")
    links.new(bsdf.outputs[0], out.inputs[0])
    uv = nodes.new("ShaderNodeUVMap")
    uv.uv_map = "UVMap"
    mapping = nodes.new("ShaderNodeMapping")
    scale = config["scales"].get(key, 1.0)
    mapping.inputs["Scale"].default_value = (scale, scale, 1)
    links.new(uv.outputs[0], mapping.inputs[0])
    albedo = image_node(nodes, folder / "albedo.png", True)
    ao = image_node(nodes, folder / "ao.png", False)
    rough = image_node(nodes, folder / "roughness.png", False)
    metal = image_node(nodes, folder / "metallic.png", False)
    normal = image_node(nodes, folder / "normal.png", False)
    for node in (albedo, ao, rough, metal, normal):
        links.new(mapping.outputs[0], node.inputs[0])
    mix = nodes.new("ShaderNodeMix")
    mix.data_type = "RGBA"
    mix.blend_type = "MULTIPLY"
    mix.inputs["Factor"].default_value = 1.0
    links.new(albedo.outputs[0], mix.inputs["A"])
    links.new(ao.outputs[0], mix.inputs["B"])
    links.new(mix.outputs["Result"], bsdf.inputs["Base Color"])
    links.new(rough.outputs[0], bsdf.inputs["Roughness"])
    links.new(metal.outputs[0], bsdf.inputs["Metallic"])
    nmap = nodes.new("ShaderNodeNormalMap")
    links.new(normal.outputs[0], nmap.inputs["Color"])
    links.new(nmap.outputs[0], bsdf.inputs["Normal"])


def load(item):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=item["glb"])
    objs = [o for o in bpy.data.objects if o not in before and o.type == "MESH"]
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    if len(objs) > 1:
        bpy.ops.object.join()
    obj = bpy.context.view_layer.objects.active
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    corners = [Vector(c) for c in obj.bound_box]
    lo = Vector([min(c[i] for c in corners) for i in range(3)])
    hi = Vector([max(c[i] for c in corners) for i in range(3)])
    factor = config["target_height"] / (hi.z - lo.z)
    obj.scale = (factor,) * 3
    obj.location = (-(lo.x + hi.x) / 2 * factor, -(lo.y + hi.y) / 2 * factor, -lo.z * factor)
    if item["materials"] == "ours":
        for slot in obj.material_slots:
            our_material(slot.material)
    return obj, len(obj.data.polygons)


def camera(scene, height, view):
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    elev, azim = math.radians(min(view.get("elevation", 20), 89.9)), math.radians(view.get("azimuth", 35))
    target = Vector((0, 0, height * 0.5))
    if view.get("ortho"):
        cam.data.type = "ORTHO"
        cam.data.ortho_scale = height * view.get("scale", 1.25)
    else:
        cam.data.lens = 85
    distance = height * view.get("distance", 3.8)
    cam.location = target + Vector((distance * math.cos(elev) * math.sin(azim),
                                    -distance * math.cos(elev) * math.cos(azim), distance * math.sin(elev)))
    cam.rotation_euler = (target - cam.location).to_track_quat("-Z", "Y").to_euler()
    scene.collection.objects.link(cam)
    scene.camera = cam


VIEWS = config.get("views", [{"name": "34", "elevation": 12, "azimuth": 35}])
for item in config["items"]:
    for view in VIEWS:
        scene = reset()
        obj, faces = load(item)
        camera(scene, config["target_height"], view)
        scene.render.filepath = str(out_dir / f"{item['name']}-{view['name']}.png")
        bpy.ops.render.render(write_still=True)
    wire_mat = bpy.data.materials.new("wire")
    wire_mat.use_nodes = True
    wire_mat.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.02, 0.02, 0.02, 1)
    base_mat = bpy.data.materials.new("base")
    base_mat.use_nodes = True
    base_mat.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.7, 0.7, 0.7, 1)
    obj.data.materials.clear()
    obj.data.materials.append(base_mat)
    obj.data.materials.append(wire_mat)
    for poly in obj.data.polygons:
        poly.material_index = 0
    wire = obj.modifiers.new("wire", "WIREFRAME")
    wire.thickness = config["target_height"] * 0.0025
    wire.use_replace = False
    wire.material_offset = 1
    scene.cycles.samples = 16
    scene.render.filepath = str(out_dir / f"{item['name']}-wire.png")
    bpy.ops.render.render(write_still=True)
    print("RENDERED", item["name"], faces)
