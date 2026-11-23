"""Render every WORK/<item>-<shape>.ply of ../hunyuan_pilot.py sheet the same way: unit size, Y up (glTF), from the
front-left and a little above, workbench with cavity, so forms compare without their colour.

blender -b --factory-startup -P hunyuan_sheet.py -- WORK
"""
import math
import sys
from pathlib import Path

import bpy
from mathutils import Vector

work = Path(sys.argv[sys.argv.index("--") + 1])
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.engine = "BLENDER_WORKBENCH"
scene.display.shading.light = "STUDIO"
scene.display.shading.show_cavity = True
scene.render.resolution_x = scene.render.resolution_y = 300
cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
scene.collection.objects.link(cam)
scene.camera = cam
cam.data.type = "ORTHO"
cam.data.ortho_scale = 1.35
a, e = math.radians(-35), math.radians(25)
cam.location = Vector((math.sin(a) * math.cos(e), -math.cos(a) * math.cos(e), math.sin(e))) * 4
cam.rotation_euler = (-cam.location).to_track_quat("-Z", "Y").to_euler()

for ply in sorted(work.glob("*.ply")):
    bpy.ops.wm.ply_import(filepath=str(ply), forward_axis="NEGATIVE_Z", up_axis="Y")
    ob = bpy.context.selected_objects[0]
    ob.data.shade_flat()
    scene.render.filepath = str(ply.with_suffix(".png"))
    bpy.ops.render.render(write_still=True)
    bpy.data.objects.remove(ob)
print("SHEET DONE")
