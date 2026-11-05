# How deep the eyes sit: the front of the MPFB eyeball vs the generator's surface at the pupil, rays from the front.
import bpy, sys, json, numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree
a=sys.argv[sys.argv.index('--')+1:]; raw=np.load(a[0]); P=json.load(open(a[1]))
bm=bpy.data.objects['Human']
mods=[(m,m.show_viewport) for m in bm.modifiers]
for m,_ in mods: m.show_viewport=False
bpy.context.view_layer.update(); ev=bm.evaluated_get(bpy.context.evaluated_depsgraph_get()); me=ev.to_mesh()
vg={g.name:g.index for g in bm.vertex_groups}
co=np.array([v.co for v in me.vertices])
def gm(nm):
    gi=vg[nm]; return np.array([any(g.group==gi and g.weight>0.5 for g in v.groups) for v in bm.data.vertices])
lc=co[gm('helper-l-eye')].mean(0); rc=co[gm('helper-r-eye')].mean(0); eye=(lc+rc)/2
skin=~gm('HelperGeometry')
mpp=0.0615/P['ipd_px']; unit=887/1.1*mpp; fx,fy=P['front_eye']; px,py=P['profile_eye']
origin=np.array(P['ref_origin']) if 'ref_origin' in P else np.array([0, eye[1]+(440-px)*mpp, eye[2]-(443.5-fy)*mpp])
R=np.array([[1,0,0],[0,0,-1],[0,1,0]],np.float32)
rv=(raw['vertices']@R.T)*unit+origin; rf=raw['faces']
box=(np.abs(rv[:,0])<0.11)&(rv[:,2]>eye[2]-0.16)&(rv[:,2]<eye[2]+0.11)&(rv[:,1]<eye[1]+0.07)
keep=box[rf].all(1); rf=rf[keep]; used=np.unique(rf); rm=-np.ones(len(rv),np.int64); rm[used]=np.arange(len(used)); rv=rv[used]; rf=rm[rf]
tree=BVHTree.FromPolygons(rv.tolist(),rf.tolist())
eyeobj=[o for o in bpy.data.objects if 'high-poly' in o.name][0]
eme=eyeobj.evaluated_get(bpy.context.evaluated_depsgraph_get()).to_mesh(); ec=np.array([eyeobj.matrix_world@v.co for v in eme.vertices])
etree=BVHTree.FromPolygons(ec.tolist(),[tuple(p.vertices) for p in eme.polygons])
stree=BVHTree.FromPolygons(co.tolist(),[tuple(p.vertices) for p in me.polygons if all(skin[i] for i in p.vertices)])
def front(tr,x,z):
    r=tr.ray_cast(Vector((x,-1,z)),Vector((0,1,0)),2); return None if r[0] is None else r[0].y
for name,c in (('L',lc),('R',rc)):
    for dz,label in ((0,'pupil'),(0.024,'brow'),(-0.022,'under-eye'),(-0.045,'cheekbone')):
        x=c[0] if label!='cheekbone' else c[0]*1.25
        print('EYE',name,label,'eye_y %.1f'%(1000*eye[1]),'ref %.1f mm'%(1000*(front(tree,x,c[2]+dz)-eye[1])), 'mpfb skin %s'%('%.1f'%(1000*(front(stree,x,c[2]+dz)-eye[1])) if front(stree,x,c[2]+dz) is not None else '-'), 'mpfb eyeball %s'%('%.1f'%(1000*(front(etree,x,c[2])-eye[1])) if label=='pupil' else ''))
