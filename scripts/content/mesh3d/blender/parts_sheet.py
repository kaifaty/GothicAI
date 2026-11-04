"""Render the parts pilot's labels on the game meshes: one row per item, one column per labelling, from two sides.

blender -b --factory-startup -P parts_sheet.py -- PILOT_DIR OUT_DIR

PILOT_DIR/<item>/truth.npz (../parts_pilot.py prepare) gives the mesh and the hand cut; PILOT_DIR/<item>/labels.npz
({column name: per-face int}, written by parts_pilot.py sheet) the columns. Each label value is a colour; 0 is the
wood's grey, 1 the cloth's orange, others a palette. Writes OUT_DIR/<item>-<column>-<view>.png (workbench, flat).
"""
import colorsys
import json
import math
import sys
from pathlib import Path

import bpy
import numpy as np
from mathutils import Vector

pilot, out = map(Path, sys.argv[sys.argv.index("--") + 1:][:2])
out.mkdir(parents=True, exist_ok=True)
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.engine = "BLENDER_WORKBENCH"
scene.display.shading.light = "STUDIO"
scene.display.shading.color_type = "VERTEX"
scene.display.shading.show_cavity = True
scene.render.resolution_x = scene.render.resolution_y = 420
cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
scene.collection.objects.link(cam)
scene.camera = cam


def colour(v):
    if v == 0:
        return (0.55, 0.55, 0.55)
    if v == 1:
        return (0.95, 0.45, 0.1)
    return colorsys.hsv_to_rgb((v * 0.137) % 1, 0.65, 0.95)


for folder in sorted(p for p in pilot.iterdir() if (p / "labels.npz").exists()):
    t, labels = np.load(folder / "truth.npz"), np.load(folder / "labels.npz")
    V, F = t["vertices"], t["faces"]
    centre = Vector(((V.min(0) + V.max(0)) / 2).tolist())
    size = float(np.linalg.norm(V.max(0) - V.min(0)))
    for column in labels.files:
        me = bpy.data.meshes.new(column)
        me.from_pydata(V.tolist(), [], F.tolist())
        attr = me.color_attributes.new("Col", "FLOAT_COLOR", "CORNER")
        rgba = np.array([(*colour(int(v)), 1.0) for v in labels[column]], np.float32)
        attr.data.foreach_set("color", np.repeat(rgba, 3, axis=0).ravel())
        ob = bpy.data.objects.new(column, me)
        scene.collection.objects.link(ob)
        for k, az in enumerate((-35, 145)):
            a, e = math.radians(az), math.radians(28)
            cam.location = centre + Vector((math.sin(a) * math.cos(e), -math.cos(a) * math.cos(e), math.sin(e))) * size * 1.25
            cam.rotation_euler = (centre - cam.location).to_track_quat("-Z", "Y").to_euler()
            scene.render.filepath = str(out / f"{folder.name}-{column}-{k}.png")
            bpy.ops.render.render(write_still=True)
        bpy.data.objects.remove(ob)
print("SHEET DONE")
