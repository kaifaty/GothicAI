# Plays Audio2Face-3D ARKit weights (a2f-weights CSV) on the test head and renders 30 fps frames.
#   blender -b --factory-startup -P render_a2f.py -- <test-head.glb> <weights.csv> <out dir> [<size px, 480>]
# CSV: timestamp in 16 kHz samples, then 52 skin weights in A2F pose order, then 16 tongue weights (unused:
# our tongue has no matching shapes). Weights are applied by name to every part that has the shape.
import bpy, os, sys, math
import numpy as np
from mathutils import Vector

A = sys.argv[sys.argv.index('--')+1:]
GLB, CSV, OUT = A[:3]
RES = int(A[3]) if len(A) > 3 else 480
os.makedirs(OUT, exist_ok=True)
SKIN = ("eyeBlinkLeft eyeLookDownLeft eyeLookInLeft eyeLookOutLeft eyeLookUpLeft eyeSquintLeft eyeWideLeft "
        "eyeBlinkRight eyeLookDownRight eyeLookInRight eyeLookOutRight eyeLookUpRight eyeSquintRight eyeWideRight "
        "jawForward jawLeft jawRight jawOpen mouthClose mouthFunnel mouthPucker mouthLeft mouthRight mouthSmileLeft "
        "mouthSmileRight mouthFrownLeft mouthFrownRight mouthDimpleLeft mouthDimpleRight mouthStretchLeft "
        "mouthStretchRight mouthRollLower mouthRollUpper mouthShrugLower mouthShrugUpper mouthPressLeft "
        "mouthPressRight mouthLowerDownLeft mouthLowerDownRight mouthUpperUpLeft mouthUpperUpRight browDownLeft "
        "browDownRight browInnerUp browOuterUpLeft browOuterUpRight cheekPuff cheekSquintLeft cheekSquintRight "
        "noseSneerLeft noseSneerRight tongueOut").split()

data = np.loadtxt(CSV, delimiter=',')
t = data[:, 0]/16000.0
W = data[:, 1:1+len(SKIN)]
fps = 30
n = int(math.floor(t[-1]*fps))+1
# resample to 30 fps by nearest earlier frame (regression is 30 fps already, diffusion 60)
idx = np.clip(np.searchsorted(t, np.arange(n)/fps + 1e-6, side='right')-1, 0, len(t)-1)
W = W[idx]

bpy.ops.wm.read_factory_settings(use_empty=True)
sc = bpy.context.scene
bpy.ops.import_scene.gltf(filepath=GLB)
parts = [o for o in sc.objects if o.type == 'MESH']
pivot = bpy.data.objects.new('pivot', None); sc.collection.objects.link(pivot)
for o in parts:
    o.parent = pivot
pivot.rotation_euler = (0, math.radians(90), 0)

keys = [(o.data.shape_keys.key_blocks[name], j) for o in parts if o.data.shape_keys
        for j, name in enumerate(SKIN) if name in o.data.shape_keys.key_blocks]

def apply(scene, *_):
    w = W[min(max(scene.frame_current-1, 0), n-1)]
    for k, j in keys:
        k.value = float(w[j])
bpy.app.handlers.frame_change_pre.append(apply)

sc.frame_start, sc.frame_end = 1, n
sc.render.fps = fps
sc.render.engine = 'BLENDER_WORKBENCH'
sc.display.shading.light = 'STUDIO'; sc.display.shading.color_type = 'OBJECT'
sc.display.shading.show_cavity = True
sc.render.resolution_x = sc.render.resolution_y = RES
sc.world = bpy.data.worlds.new('w'); sc.world.color = (0.22, 0.22, 0.22)
for o in parts:
    o.color = (0.85, 0.72, 0.62, 1) if o.name == 'face' else (0.95, 0.95, 0.95, 1)
cam = bpy.data.objects.new('cam', bpy.data.cameras.new('cam')); sc.collection.objects.link(cam); sc.camera = cam
cam.data.type = 'ORTHO'; cam.data.ortho_scale = 0.24
bpy.context.view_layer.update()
pts = [o.matrix_world @ Vector(c) for o in parts for c in o.bound_box]
ctr = sum(pts, Vector())/len(pts)
a = math.radians(20)
cam.location = (ctr.x+math.sin(a)*2, ctr.y-math.cos(a)*2, ctr.z-0.015)
cam.rotation_euler = (math.radians(90), 0, a)
sc.render.image_settings.file_format = 'JPEG'
sc.render.filepath = os.path.join(OUT, 'f')
bpy.ops.render.render(animation=True)
