# Wrap the MPFB face onto the generator's head: rays along each skin vertex normal find the reference surface;
# the offsets are masked to the visible skin (no eyelids, lips, beard, ears, scalp) and smoothed over the mesh so
# the large forms move and the generator's noise does not. The result is a shape key 'hero_wrap' on the human.
#   blender -b <fit>/fit.blend -P wrap_head.py -- raw.npz params.json <out dir> [smooth_iters] [ray_cm]
import bpy, sys, os, json, math
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree
a=sys.argv[sys.argv.index('--')+1:]; raw=np.load(a[0]); P=json.load(open(a[1])); OUT=a[2]; os.makedirs(OUT,exist_ok=True)
iters=int(a[3]) if len(a)>3 else 8; ray=float(a[4])/100 if len(a)>4 else 0.025
sc=bpy.context.scene; bm=bpy.data.objects['Human']; cam=sc.camera
vg={g.name:g.index for g in bm.vertex_groups}
mods=[(m,m.show_viewport) for m in bm.modifiers]
for m,_ in mods: m.show_viewport=False
bpy.context.view_layer.update(); dg=bpy.context.evaluated_depsgraph_get(); ev=bm.evaluated_get(dg); me=ev.to_mesh()
n=len(me.vertices)
co=np.array([v.co for v in me.vertices]); nor=np.array([v.normal for v in me.vertices])
edges=np.array([e.vertices for e in me.edges])
ev.to_mesh_clear()
for m,v in mods: m.show_viewport=v
def gmask(name):
    gi=vg[name]; return np.array([any(g.group==gi and g.weight>0.5 for g in v.groups) for v in bm.data.vertices])
helper=gmask('HelperGeometry'); le=gmask('helper-l-eye'); re_=gmask('helper-r-eye')
lc=co[le].mean(0); rc=co[re_].mean(0); eye=(lc+rc)/2
# the reference, placed as in ref_view.py and cropped to the face
mpp=0.0615/P['ipd_px']; unit=887/1.1*mpp
fx,fy=P['front_eye']; px,py=P['profile_eye']
origin=np.array(P['ref_origin']) if 'ref_origin' in P else np.array([0, eye[1]+(440-px)*mpp, eye[2]-(443.5-fy)*mpp])
R=np.array([[1,0,0],[0,0,-1],[0,1,0]],np.float32)
rv=(raw['vertices']@R.T)*unit+origin; rf=raw['faces']
box=(np.abs(rv[:,0])<0.11)&(rv[:,2]>eye[2]-0.16)&(rv[:,2]<eye[2]+0.11)&(rv[:,1]<eye[1]+0.07)
keep=box[rf].all(1); rf=rf[keep]
used=np.unique(rf); remap=-np.ones(len(rv),np.int64); remap[used]=np.arange(len(used)); rv=rv[used]; rf=remap[rf]
print('WRAP ref crop verts',len(rv),'faces',len(rf))
tree=BVHTree.FromPolygons(rv.tolist(),rf.tolist())
# mask: the visible skin of the face
def ramp(d,a0,a1): return np.clip((d-a0)/(a1-a0),0,1)
w=np.ones(n)
w*=~helper
w*=np.clip((-nor[:,1]-0.05)/0.25,0,1)                      # facing forward
w*=ramp(co[:,2],eye[2]-0.15,eye[2]-0.125)                     # not below the jaw
w*=1-ramp(co[:,2],eye[2]+0.075,eye[2]+0.095)                 # not above the hairline
w*=1-ramp(np.abs(co[:,0]),0.060,0.072)                       # not the ears and the side of the skull
# an oval around each eye: the lids (up to ~1.2 cm above and below the centre) stay MPFB's, the brow above is wrapped
sq=np.array([1.0,1.0,1.8])
de=np.minimum(np.linalg.norm((co-lc)*sq,axis=1),np.linalg.norm((co-rc)*sq,axis=1))
w*=ramp(de,0.010,0.016)                                      # the eyelids stay MPFB's
mouth=np.array([0,eye[1]-0.01,eye[2]-0.075])
dm=np.linalg.norm((co-mouth)*np.array([1,1,0.8]),axis=1)
w*=ramp(dm,0.022,0.034)                                      # lips, moustache, goatee stay MPFB's
eyeobj=[o for o in bpy.data.objects if 'high-poly' in o.name][0]
eme=eyeobj.evaluated_get(bpy.context.evaluated_depsgraph_get()).to_mesh(); ec=np.array([eyeobj.matrix_world@v.co for v in eme.vertices])
etree=BVHTree.FromPolygons(ec.tolist(),[tuple(p.vertices) for p in eme.polygons])
def front(tr,x,z):
    r=tr.ray_cast(Vector((x,-1,z)),Vector((0,1,0)),2); return None if r[0] is None else r[0].y
shift=np.mean([front(tree,c[0],c[2])-front(etree,c[0],c[2]) for c in (lc,rc)])
print('WRAP eye shift back mm %.1f'%(1000*shift))
eyeobj.location.y+=shift
off=np.zeros((n,3)); hit=np.zeros(n,bool)
for i in np.nonzero(w>0)[0]:
    p=Vector(co[i]); d=Vector(nor[i]); best=None
    for s in (1,-1):
        r=tree.ray_cast(p,d*s,ray)
        if r[0] is not None and (best is None or r[3]<best[3]): best=r
    if best is not None: off[i]=np.array(best[0])-co[i]; hit[i]=True
w*=hit
print('WRAP masked',int((w>0).sum()),'hit',int(hit.sum()),'mean |off| mm %.1f'%(1000*np.linalg.norm(off[w>0.5],axis=1).mean()))
# smooth the weighted offsets over the mesh (and the weights, so the edges blend)
nb=[[] for _ in range(n)]
for a_,b_ in edges: nb[a_].append(b_); nb[b_].append(a_)
deg=np.array([max(len(x),1) for x in nb])
src=np.concatenate([[i]*len(x) for i,x in enumerate(nb)]).astype(np.int64); dst=np.concatenate(nb).astype(np.int64)
wo=off*w[:,None]
for _ in range(iters):
    acc=np.zeros_like(wo); np.add.at(acc,src,wo[dst]); wo=0.5*wo+0.5*acc/deg[:,None]
    accw=np.zeros(n); np.add.at(accw,src,w[dst]); w=0.5*w+0.5*accw/deg
disp=wo
disp[helper]=0
disp[le|re_]=np.array([0,shift,0])
# a shape key relative to the basis
if bm.data.shape_keys is None: bm.shape_key_add(name='Basis')
k=bm.shape_key_add(name='hero_wrap',from_mix=False)
basis=np.array([v.co for v in bm.data.shape_keys.reference_key.data])
for i in range(n): k.data[i].co=Vector(basis[i]+disp[i])
k.value=1.0
np.save(os.path.join(OUT,'wrap_disp.npy'),disp)
bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT,'wrap.blend'))
print('WRAP max disp mm %.1f'%(1000*np.linalg.norm(disp,axis=1).max()))
