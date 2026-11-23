# Writes a copy of the hero's head.glb whose shapes are replaced by those of transfer.py.
#   blender -b --factory-startup -P apply_shapes.py -- <head.glb> <shapes.npz> <out.glb>
# The npz holds absolute coordinates keyed "<part>|<shape>", in the frame dump_glb.py read the GLB in.
import bpy, sys
import numpy as np

GLB, NPZ, OUT = sys.argv[sys.argv.index('--')+1:][:3]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=GLB)
d = np.load(NPZ)
done = 0
for o in bpy.context.scene.objects:
    if o.type != 'MESH' or not o.data.shape_keys:
        continue
    for k in o.data.shape_keys.key_blocks[1:]:
        key = o.name+'|'+k.name
        if key in d.files:
            a = d[key]
            assert len(a) == len(k.data), key
            k.data.foreach_set('co', a.astype(np.float32).ravel())
            done += 1
bpy.ops.object.select_all(action='SELECT')
bpy.ops.export_scene.gltf(filepath=OUT, export_format='GLB', export_morph=True, export_morph_normal=True,
                          export_apply=False, export_yup=True)
print('shapes set:', done, '->', OUT)
