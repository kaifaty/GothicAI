# Deltas of the ARKit face units that move the lids (blink, squint, wide), read from the .target.gz files.
#   blender -b <fit>/fit.blend -P fu_deltas.py -- <eye_deltas.npz to extend>
import bpy, sys, os, gzip, numpy as np
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from head_paths import MPFB_DATA

out=sys.argv[sys.argv.index('--')+1]; E=dict(np.load(out))
n=len(bpy.data.objects['Human'].data.vertices)
d=os.path.join(MPFB_DATA, 'targets', 'faceunits')
if not os.path.isdir(d):
    import bl_ext.user_default.mpfb as mp; d=os.path.join(os.path.dirname(mp.__file__),'data','targets','faceunits')
names=list(E['names']); D=list(E['D'])
for side,suf in (('l','Left'),('r','Right')):
    for u in ('eyeBlink','eyeSquint','eyeWide'):
        p=os.path.join(d,f'{u}{suf}.target')
        if not os.path.exists(p): p+='.gz'
        op=gzip.open if p.endswith('.gz') else open
        a=np.zeros((n,3),np.float32)
        for line in op(p,'rt'):
            s=line.split()
            if len(s)>=4 and not line.startswith('#'):
                i=int(s[0]); x,y,z=map(float,s[1:4]); a[i]=(x,-z,y)    # MakeHuman y-up, decimetres -> Blender
        a*=0.1
        names.append(f'{side}-fu-{u}'); D.append(a)
        nz=np.abs(a).sum(1)>0
        print('FU',side,u,'verts',int(nz.sum()),'max mm %.1f'%(1000*np.abs(a).max()))
np.savez(out,names=np.array(names),D=np.array(D))
