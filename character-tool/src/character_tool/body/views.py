# blender -b -P views.py -- <glb> <outdir>
import bpy, sys, json, numpy as np
from mathutils import Vector
glb, out = sys.argv[sys.argv.index('--') + 1:][:2]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=glb)
objs = [o for o in bpy.context.scene.objects if o.type == 'MESH']
o = objs[0]
me = o.data
co = np.empty(len(me.vertices) * 3); me.vertices.foreach_get('co', co); co = co.reshape(-1, 3)
M = np.array(o.matrix_world); co = co @ M[:3, :3].T + M[:3, 3]
g = np.c_[co[:, 0], co[:, 2], -co[:, 1]]  # glTF coords
lo, hi = g.min(0), g.max(0)
json.dump({'min': lo.tolist(), 'max': hi.tolist(), 'verts': len(g), 'objects': len(objs)}, open(out + '/bounds.json', 'w'))
sc = bpy.context.scene
sc.render.engine = 'BLENDER_WORKBENCH'
sc.display.shading.light = 'FLAT'
sc.display.shading.color_type = 'TEXTURE'
sc.render.film_transparent = True
sc.render.resolution_x, sc.render.resolution_y = 1400, 2000
cam = bpy.data.objects.new('cam', bpy.data.cameras.new('cam')); sc.collection.objects.link(cam); sc.camera = cam
cam.data.type = 'ORTHO'
H = hi[1] - lo[1]; pad = 0.02
cam.data.ortho_scale = (H + 2 * pad) * 1.0  # vertical extent (sensor fit auto: larger dim = height)
cam.data.sensor_fit = 'VERTICAL'
cy = (lo[1] + hi[1]) / 2
views = {'front': ((0, -5, cy), (np.pi / 2, 0, 0), 'x'), 'left': ((5, 0, cy), (np.pi / 2, 0, np.pi / 2), 'z'),
         'back': ((0, 5, cy), (np.pi / 2, 0, np.pi), 'x')}
meta = {'ortho_scale': cam.data.ortho_scale, 'res': [1400, 2000], 'cy': cy}
for name, (loc, rot, axis) in views.items():
    cam.location = Vector(loc); cam.rotation_euler = rot
    sc.render.filepath = f'{out}/{name}.png'
    bpy.ops.render.render(write_still=True)
json.dump(meta, open(out + '/views.json', 'w'))
