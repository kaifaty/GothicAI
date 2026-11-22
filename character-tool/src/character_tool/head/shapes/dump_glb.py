# Dumps every mesh of a head GLB to one npz: per part its rest vertices, triangles and each shape key as absolute
# coordinates, in the GLB's own frame (as Blender imports it).
#   blender -b --factory-startup -P dump_glb.py -- <head.glb> <out.npz>
import bpy, sys
import numpy as np

GLB, OUT = sys.argv[sys.argv.index('--')+1:][:2]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=GLB)
out, names = {}, []
for o in bpy.context.scene.objects:
    if o.type != 'MESH':
        continue
    m = o.data
    names.append(o.name)
    n = len(m.vertices)
    co = np.empty(n*3); m.vertices.foreach_get('co', co)
    out[o.name+'|co'] = co.reshape(-1, 3)
    m.calc_loop_triangles()
    tri = np.empty(len(m.loop_triangles)*3, dtype=np.int64); m.loop_triangles.foreach_get('vertices', tri)
    out[o.name+'|tri'] = tri.reshape(-1, 3)
    out[o.name+'|mw'] = np.array(o.matrix_world)
    if m.shape_keys:
        for k in m.shape_keys.key_blocks[1:]:
            a = np.empty(n*3); k.data.foreach_get('co', a)
            out[o.name+'|sk|'+k.name] = a.reshape(-1, 3)
out['parts'] = np.array(names)
np.savez_compressed(OUT, **out)
print('dumped', names)
