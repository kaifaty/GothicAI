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
# each eyeball follows its helper: the shift and the scale of the helper sphere
inv=eo.matrix_world.inverted_safe()
sim={}
for side,mask in (('l',M['leye']),('r',M['reye'])):
    a0=M['co'][mask]; a1=R['co'][mask]; c0=a0.mean(0); c1=a1.mean(0)
    s_=np.linalg.norm(a1-c1,axis=1).mean()/np.linalg.norm(a0-c0,axis=1).mean(); sim[side]=(c0,c1,s_)
    print('APPLY eye',side,'shift mm',(1000*(c1-c0)).round(1),'scale %.3f'%s_)
for v in eo.data.vertices:
    w=np.array(eo.matrix_world@v.co); c0,c1,s_=sim['l' if w[0]>0 else 'r']
    v.co=inv@Vector(c1+(w-c0)*s_)
eo.data.update()
bpy.ops.wm.save_as_mainfile(filepath=a[2])
print('APPLY max disp mm %.1f'%(1000*np.linalg.norm(d,axis=1).max()))
