# Save MPFB vertex-group masks used by registration: body (the visible skin), joint cubes of the eyes.
import bpy, sys, numpy as np
out=sys.argv[sys.argv.index('--')+1]
bm=bpy.data.objects['Human']; vg={g.name:g.index for g in bm.vertex_groups}
def gm(n):
    gi=vg[n]; return np.array([any(g.group==gi and g.weight>0.5 for g in v.groups) for v in bm.data.vertices])
np.savez(out,body=gm('body'),jl=gm('joint-l-eye')|gm('joint-l-eye-target'),jr=gm('joint-r-eye')|gm('joint-r-eye-target'))
