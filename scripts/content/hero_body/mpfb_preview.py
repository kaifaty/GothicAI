"""Raking-light Cycles preview of the baked relief: the game mesh with skin, with and without the normal map.
    Blender -b <run>/bake.blend -P mpfb_preview.py -- <run> <skin.png> <out.jpg>"""
import bpy, os, sys, math
import numpy as np
from mathutils import Vector
RUN, SKIN, OUT = sys.argv[sys.argv.index('--') + 1:][:3]
sc = bpy.context.scene
low = bpy.data.objects['low']; bpy.data.objects['high'].hide_render = True
for o in bpy.data.objects: o.hide_render = o.name != 'low'
n = np.load(os.path.join(RUN, 'normal.npy')).astype(np.float32)
S = n.shape[0]
img = bpy.data.images.new('nrm', S, S, float_buffer=True); img.colorspace_settings.name = 'Non-Color'
px = np.ones((S, S, 4), np.float32); px[..., :3] = n[::-1]; img.pixels = px.ravel()
skin = bpy.data.images.load(SKIN)
m = bpy.data.materials.new('prev'); m.use_nodes = True; nt = m.node_tree; b = nt.nodes['Principled BSDF']
t = nt.nodes.new('ShaderNodeTexImage'); t.image = skin; nt.links.new(t.outputs['Color'], b.inputs['Base Color'])
b.inputs['Roughness'].default_value = 0.55
tn = nt.nodes.new('ShaderNodeTexImage'); tn.image = img
nm = nt.nodes.new('ShaderNodeNormalMap'); nm.uv_map = 'UVMap'
nt.links.new(tn.outputs['Color'], nm.inputs['Color'])
low.data.materials.clear(); low.data.materials.append(m)
sc.render.engine = 'CYCLES'; sc.cycles.samples = 48; sc.render.resolution_x = sc.render.resolution_y = 700
sc.world.color = (0.05, 0.05, 0.06)
sun = bpy.data.objects.new('sun', bpy.data.lights.new('sun', 'SUN')); sc.collection.objects.link(sun)
sun.data.energy = 4.0
cam = bpy.data.objects.new('cam', bpy.data.cameras.new('cam')); sc.collection.objects.link(cam); sc.camera = cam
cam.data.type = 'ORTHO'; cam.data.ortho_scale = 0.75
paths = []
for side, ang in (('front', 0), ('back', 180)):
    a = math.radians(ang)
    cam.location = Vector((math.sin(a) * 4, -math.cos(a) * 4, 1.22)); cam.rotation_euler = (math.radians(90), 0, a)
    sun.rotation_euler = (math.radians(70), 0, a + math.radians(-60))
    for with_n in (False, True):
        if with_n: nt.links.new(nm.outputs['Normal'], b.inputs['Normal'])
        else:
            for l in list(nt.links):
                if l.to_socket == b.inputs['Normal']: nt.links.remove(l)
        p = os.path.join(RUN, f'prev-{side}-{int(with_n)}.png'); sc.render.filepath = p
        bpy.ops.render.render(write_still=True); paths.append(p)
print('PREVIEW', paths)
