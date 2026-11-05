# Put the registration into the fit: shape key 'hero_reg' on the human, each eyeball of the eye proxy shifted.
#   blender -b <fit>/fit.blend -P reg_apply.py -- reg.npz mpfb.npz <out.blend>
import bpy, sys
import numpy as np
from mathutils import Vector
a=sys.argv[sys.argv.index('--')+1:]; R=np.load(a[0]); M=np.load(a[1])
bm=bpy.data.objects['Human']
d=R['co']-M['co']                                   # displacement over the fit as exported (targets applied)
if bm.data.shape_keys is None: bm.shape_key_add(name='Basis')
k=bm.shape_key_add(name='hero_reg',from_mix=False)
basis=np.array([v.co for v in bm.data.shape_keys.reference_key.data])
for i in range(len(basis)): k.data[i].co=Vector(basis[i]+d[i])
k.value=1.0
eo=[o for o in bpy.data.objects if 'high-poly' in o.name][0]
inv=eo.matrix_world.inverted_safe().to_3x3()
for v in eo.data.vertices:
    w=eo.matrix_world@v.co
    sh=R['eye_l'] if w.x>0 else R['eye_r']
    v.co=v.co+inv@Vector(sh)
eo.data.update()
bpy.ops.wm.save_as_mainfile(filepath=a[2])
print('APPLY max disp mm %.1f'%(1000*np.linalg.norm(d,axis=1).max()))
