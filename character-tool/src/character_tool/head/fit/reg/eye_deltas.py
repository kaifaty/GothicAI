# Per-target vertex deltas of the MPFB eye targets on the fit (both sides), for a linear fit of the eye region.
#   blender -b <fit>/fit.blend -P eye_deltas.py -- <out.npz>
import bpy, sys, numpy as np
from bl_ext.user_default.mpfb.services import TargetService
out=sys.argv[sys.argv.index('--')+1]
bm=bpy.data.objects['Human']
base=['eye-trans-down','eye-trans-up','eye-trans-in','eye-trans-out','eye-push1-in','eye-push1-out','eye-push2-in','eye-push2-out',
      'eye-scale-decr','eye-scale-incr','eye-height1-decr','eye-height1-incr','eye-height2-decr','eye-height2-incr','eye-height3-decr','eye-height3-incr',
      'eye-corner1-down','eye-corner1-up','eye-corner2-down','eye-corner2-up','eye-bag-decr','eye-bag-incr','eye-eyefold-down','eye-eyefold-up']
names=[]; D=[]
n=len(bm.data.vertices)
for side in ('l','r'):
    for b in base:
        t=f'{side}-{b}'
        if not TargetService.target_full_path(t): print('MISSING',t); continue
        before=set(k.name for k in bm.data.shape_keys.key_blocks)
        TargetService.bulk_load_targets(bm,[{'target':t,'value':1.0}])
        new=[k for k in bm.data.shape_keys.key_blocks if k.name not in before]
        if not new: print('NOKEY',t); continue
        k=new[0]; rel=k.relative_key
        a=np.empty(n*3); k.data.foreach_get('co',a); c=np.empty(n*3); rel.data.foreach_get('co',c)
        D.append((a-c).reshape(n,3).astype(np.float32)); names.append(t)
        bm.shape_key_remove(k)
np.savez(out,names=np.array(names),D=np.array(D))
print('DELTAS',len(names),'max mm',[round(float(np.abs(d).max())*1000,1) for d in D][:8])
