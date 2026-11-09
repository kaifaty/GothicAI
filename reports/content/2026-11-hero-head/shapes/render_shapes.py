# Stills of chosen shape mixes on a head GLB, front and three-quarter, for comparing shape sets.
#   blender -b --factory-startup -P render_shapes.py -- <head.glb> <out dir> "jawOpen=1" "mouthSmileLeft=1,mouthSmileRight=1" ...
# Writes <out dir>/<i>_front.jpg and <i>_quarter.jpg per mix (Workbench, the light of render_a2f.py).
import bpy, os, sys, math
from mathutils import Vector

A = sys.argv[sys.argv.index('--')+1:]
GLB, OUT, MIXES = A[0], A[1], A[2:]
os.makedirs(OUT, exist_ok=True)
bpy.ops.wm.read_factory_settings(use_empty=True)
sc = bpy.context.scene
bpy.ops.import_scene.gltf(filepath=GLB)
parts = [o for o in sc.objects if o.type == 'MESH']
pivot = bpy.data.objects.new('pivot', None); sc.collection.objects.link(pivot)
for o in parts:
    o.parent = pivot
pivot.rotation_euler = (0, math.radians(90), 0)
sc.render.engine = 'BLENDER_WORKBENCH'
sc.display.shading.light = 'STUDIO'; sc.display.shading.color_type = 'OBJECT'
sc.display.shading.show_cavity = True
sc.render.resolution_x = sc.render.resolution_y = 420
sc.world = bpy.data.worlds.new('w'); sc.world.color = (0.22, 0.22, 0.22)
for o in parts:
    o.color = (0.85, 0.72, 0.62, 1) if o.name == 'face' else (0.95, 0.95, 0.95, 1)
cam = bpy.data.objects.new('cam', bpy.data.cameras.new('cam')); sc.collection.objects.link(cam); sc.camera = cam
cam.data.type = 'ORTHO'; cam.data.ortho_scale = 0.17
bpy.context.view_layer.update()
face = next(o for o in parts if o.name == 'face')
pts = [face.matrix_world @ Vector(c) for c in face.bound_box]
ctr = sum(pts, Vector())/len(pts)
sc.render.image_settings.file_format = 'JPEG'
for i, mix in enumerate(MIXES):
    for o in parts:
        if o.data.shape_keys:
            for k in o.data.shape_keys.key_blocks[1:]:
                k.value = 0.0
    for item in filter(None, mix.split(',')):
        n, v = item.split('=')
        for o in parts:
            if o.data.shape_keys and n in o.data.shape_keys.key_blocks:
                o.data.shape_keys.key_blocks[n].value = float(v)
    for view, a in (('front', 0), ('quarter', 35)):
        r = math.radians(a)
        cam.location = (ctr.x+math.sin(r)*2, ctr.y-math.cos(r)*2, ctr.z-0.035)
        cam.rotation_euler = (math.radians(90), 0, r)
        sc.render.filepath = os.path.join(OUT, '%02d_%s.jpg' % (i, view))
        bpy.ops.render.render(write_still=True)
print('rendered', len(MIXES), 'mixes')
