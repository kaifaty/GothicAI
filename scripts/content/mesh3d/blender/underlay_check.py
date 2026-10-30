"""Our model over the original's mesh: three orthographic views and both bboxes, for Claude to compare.

blender -b --factory-startup -P underlay_check.py -- BASE_MESH.glb OUR_MODEL.glb OUT_PREFIX
Writes OUT_PREFIX-{top,front,side}.png: the original in flat grey, ours as a red wireframe on top, and
OUT_PREFIX.json with both bboxes in Blender metres and the share of the original's bbox each axis of
ours spans. The original stays an underlay (docs/own-content.md, «граница с оригиналом»): nothing of it
is written but the images and the numbers; they never go to a generator or into a model.
Both GLBs are glTF with X mirrored the same way, so the views line up as the game places them.
"""
import json
import sys

import bpy
from mathutils import Vector

base, ours, out = sys.argv[sys.argv.index("--") + 1:][:3]
bpy.ops.wm.read_factory_settings(use_empty=True)


def load(path):
    before = set(bpy.context.scene.objects)
    bpy.ops.import_scene.gltf(filepath=path)
    return [o for o in bpy.context.scene.objects if o not in before and o.type == "MESH"]


def bbox(objs):
    pts = [o.matrix_world @ Vector(c) for o in objs for c in o.bound_box]
    return [min(p[i] for p in pts) for i in range(3)], [max(p[i] for p in pts) for i in range(3)]


orig, mine = load(base), load(ours)
grey = bpy.data.materials.new("underlay")
grey.diffuse_color = (0.55, 0.55, 0.55, 1)
for o in orig:
    o.data.materials.clear()
    o.data.materials.append(grey)
red = bpy.data.materials.new("ours")
red.diffuse_color = (0.9, 0.1, 0.05, 1)
for o in mine:
    o.data.materials.clear()
    o.data.materials.append(red)
    wire = o.modifiers.new("wire", "WIREFRAME")
    wire.thickness = 0.006
lo_o, hi_o = bbox(orig)
lo_m, hi_m = bbox(mine)
sc = bpy.context.scene
sc.render.engine = "BLENDER_WORKBENCH"
sc.display.shading.light = "FLAT"
sc.display.shading.color_type = "MATERIAL"
sc.render.resolution_x = sc.render.resolution_y = 768
centre = Vector([(a + b) / 2 for a, b in zip(lo_o, hi_o)])
size = max(max(b - a for a, b in zip(lo_o, hi_o)), max(b - a for a, b in zip(lo_m, hi_m)))
cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
sc.collection.objects.link(cam)
sc.camera = cam
cam.data.type = "ORTHO"
cam.data.ortho_scale = size * 1.1
for view, d in (("top", (0, 0, 1)), ("front", (0, -1, 0)), ("side", (1, 0, 0))):
    dv = Vector(d)
    cam.location = centre + dv * size * 3
    cam.rotation_euler = (-dv).to_track_quat("-Z", "Y").to_euler() if view != "top" else (0, 0, 0)
    sc.render.filepath = f"{out}-{view}.png"
    bpy.ops.render.render(write_still=True)
span = [round((b2 - a2) / max(b1 - a1, 1e-6), 3) for a1, b1, a2, b2 in zip(lo_o, hi_o, lo_m, hi_m)]
report = {"original": [lo_o, hi_o], "ours": [lo_m, hi_m], "span_of_original": span,
          "centre_shift_m": [round((a2 + b2 - a1 - b1) / 2, 3) for a1, b1, a2, b2 in zip(lo_o, hi_o, lo_m, hi_m)]}
with open(f"{out}.json", "w") as f:
    json.dump(report, f, indent=1)
print("UNDERLAY", json.dumps({k: report[k] for k in ("span_of_original", "centre_shift_m")}))
