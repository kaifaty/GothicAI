"""Render the quiver hung as the engine hangs it, next to a stand-in hip and belt, at several push-outs.

blender -b --factory-startup -P quiver_place.py -- config.json
config: {"glb": baked model.glb, "maps_dir": the bake's maps/, "out_dir": str, "size": 768,
         "tilt_back": deg, "tilt_out": deg, "hip": [half width, half depth], "clearance": m, "mouth_r": m,
         "mouth_back": m, "belt_z": m, "outs": [m, ...], "stand_in": true, "views": [{"name", "azimuth", "elevation"}]}
Checks the model, not the game: the quiver's frame from quiver.py (mouth at the origin, along -Y, +X to the
body) is turned so that its axis leans back and out as Quiver::fit does, and its mouth sits "clearance" plus
"out" beside an elliptic hip at the belt. The straps must reach the belt at every push-out. Writes
<out_dir>/place-<out cm>-<view>.png.
"""
import json
import math
import sys
from pathlib import Path

import bpy
import numpy as np
from mathutils import Matrix, Vector

cfg = json.loads(Path(sys.argv[sys.argv.index("--") + 1]).read_text())
out_dir = Path(cfg["out_dir"])
out_dir.mkdir(parents=True, exist_ok=True)


def frame():
    """The quiver frame's world up, the wearer's left and back, in the model's Blender axes (as quiver.py)."""
    tb, to = math.tan(math.radians(cfg["tilt_back"])), math.tan(math.radians(cfg["tilt_out"]))
    up, right, fwd = np.array([0, 1.0, 0]), np.array([1.0, 0, 0]), np.array([0, 0, 1.0])
    down = -up - fwd * tb + right * to
    down /= np.linalg.norm(down)
    y = -down
    z = fwd - y * fwd.dot(y)
    z /= np.linalg.norm(z)
    rows = np.stack([np.cross(y, z), y, z])

    def to_blender(v):
        e = rows @ v
        return Vector((-e[0], -e[2], e[1]))
    return to_blender(up), to_blender(-right), to_blender(-fwd)


def material(name, colour, rough=0.8):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (*colour, 1)
    bsdf.inputs["Roughness"].default_value = rough
    return mat


def atlas_material(mat):
    folder = Path(cfg["maps_dir"]) / mat.name.split(".")[0].lower()
    if not folder.exists():
        return
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    nodes.clear()
    out = nodes.new("ShaderNodeOutputMaterial")
    bsdf = nodes.new("ShaderNodeBsdfPrincipled")
    links.new(bsdf.outputs[0], out.inputs[0])

    def image(name, colour):
        node = nodes.new("ShaderNodeTexImage")
        node.image = bpy.data.images.load(str(folder / f"{name}.png"))
        node.image.colorspace_settings.name = "sRGB" if colour else "Non-Color"
        return node
    albedo, ao, rough, normal = image("albedo", True), image("ao", False), image("roughness", False), image("normal", False)
    mix = nodes.new("ShaderNodeMix")
    mix.data_type, mix.blend_type = "RGBA", "MULTIPLY"
    mix.inputs["Factor"].default_value = 1.0
    links.new(albedo.outputs[0], mix.inputs["A"])
    links.new(ao.outputs[0], mix.inputs["B"])
    links.new(mix.outputs["Result"], bsdf.inputs["Base Color"])
    links.new(rough.outputs[0], bsdf.inputs["Roughness"])
    nmap = nodes.new("ShaderNodeNormalMap")
    links.new(normal.outputs[0], nmap.inputs["Color"])
    links.new(nmap.outputs[0], bsdf.inputs["Normal"])


def ellipse_tube(name, a, b, z0, z1, mat, centre=(0, 0), seg=32):
    verts, faces = [], []
    for z in (z0, z1):
        for i in range(seg):
            t = 2 * math.pi * i / seg
            verts.append((centre[0] + a * math.cos(t), centre[1] + b * math.sin(t), z))
    for i in range(seg):
        j = (i + 1) % seg
        faces.append((i, j, seg + j, seg + i))
    faces += [tuple(range(seg))[::-1], tuple(range(seg, 2 * seg))]
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    mesh.materials.append(mat)
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def scene_for(out):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.samples = cfg.get("samples", 48)
    scene.cycles.use_denoising = True
    scene.render.resolution_x = scene.render.resolution_y = cfg.get("size", 768)
    scene.view_settings.view_transform = "AgX"
    world = bpy.data.worlds.new("world")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs[0].default_value = (0.55, 0.6, 0.68, 1)
    world.node_tree.nodes["Background"].inputs[1].default_value = 0.6
    scene.world = world
    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
    sun.data.energy = 3.5
    sun.rotation_euler = (math.radians(50), 0, math.radians(-145))
    scene.collection.objects.link(sun)
    # the stand-in: hips, the right thigh, a belt (wearer faces -Y in Blender, right is -X)
    ha, hb = cfg["hip"]
    cloth = material("cloth", (0.09, 0.075, 0.06))
    stand_in = cfg.get("stand_in", True)  # false: the quiver alone, to set beside the concept
    if stand_in:
        ellipse_tube("hip", ha, hb, 0.80, 1.25, cloth)
        ellipse_tube("thigh", 0.085, 0.085, 0.45, 0.85, cloth, centre=(-0.095, 0.0))
        ellipse_tube("belt", ha + 0.006, hb + 0.006, cfg["belt_z"] - 0.022, cfg["belt_z"] + 0.022,
                     material("belt", (0.05, 0.035, 0.022)))
    # the quiver
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=cfg["glb"])
    quiver = [o for o in bpy.data.objects if o not in before and o.type == "MESH"]
    for o in quiver:
        for slot in o.material_slots:
            atlas_material(slot.material)
    up, left, back = frame()
    # model -> world (Blender: left +X, back +Y, up +Z); rows are the model's left, back and up
    rot = Matrix((tuple(left), tuple(back), tuple(up)))
    mouth = Vector((-(ha + cfg["clearance"] + cfg["mouth_r"] + out), cfg["mouth_back"], cfg["belt_z"]))
    for o in quiver:
        o.matrix_world = Matrix.Translation(mouth) @ rot.to_4x4() @ o.matrix_world
    return scene


for out in cfg["outs"]:
    for view in cfg["views"]:
        scene = scene_for(out)
        cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
        cam.data.lens = view.get("lens", 50)
        target = Vector(view.get("target", (-0.2, 0.05, 0.8)))
        elev, azim = math.radians(view.get("elevation", 10)), math.radians(view["azimuth"])
        d = view.get("distance", 2.2)
        cam.location = target + Vector((d * math.cos(elev) * math.sin(azim), -d * math.cos(elev) * math.cos(azim),
                                        d * math.sin(elev)))
        cam.rotation_euler = (target - cam.location).to_track_quat("-Z", "Y").to_euler()
        scene.collection.objects.link(cam)
        scene.camera = cam
        scene.render.filepath = str(out_dir / f"place-{round(out * 100)}-{view['name']}.png")
        bpy.ops.render.render(write_still=True)
print("PLACED", cfg["outs"])
