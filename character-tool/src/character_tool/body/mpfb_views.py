"""Orthographic workbench views of an MPFB body .blend: front, left, back, with a 10 cm grid.
    Blender -b <body.blend> -P mpfb_views.py -- <out.png> [label]"""
import bpy, sys, math, os
from mathutils import Vector
out = sys.argv[sys.argv.index('--') + 1]
sc = bpy.context.scene
for o in bpy.data.objects:
    if o.type == 'ARMATURE': o.hide_render = True
sc.render.engine = 'BLENDER_WORKBENCH'
sc.display.shading.light = 'STUDIO'; sc.display.shading.color_type = 'MATERIAL'
sc.display.shading.show_cavity = True
sc.render.resolution_x, sc.render.resolution_y = 700, 1000
sc.world = sc.world or bpy.data.worlds.new('w')
cam = bpy.data.cameras.new('c'); cam.type = 'ORTHO'; cam.ortho_scale = 2.0
co = bpy.data.objects.new('c', cam); sc.collection.objects.link(co); sc.camera = co
# grid: thin bars every 10 cm
mat = bpy.data.materials.new('g'); mat.diffuse_color = (0.15, 0.15, 0.15, 1)
for z in range(0, 200, 10):
    bpy.ops.mesh.primitive_cube_add(size=1, location=(0, 0.6, z / 100))
    b = bpy.context.object; b.scale = (2.0, 0.001, 0.0015 if z % 50 else 0.003); b.data.materials.append(mat)
paths = []
for name, ang in (('front', 0), ('left', 90), ('back', 180)):
    a = math.radians(ang)
    co.location = Vector((math.sin(a) * 5, -math.cos(a) * 5, 0.95))
    co.rotation_euler = (math.radians(90), 0, a)
    p = out.replace('.png', f'-{name}.png'); sc.render.filepath = p
    bpy.ops.render.render(write_still=True); paths.append(p)
print('VIEWS', paths)
