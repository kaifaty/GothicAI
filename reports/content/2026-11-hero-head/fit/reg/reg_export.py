# Export the MPFB fit and the generator's reference for registration, and render both for landmark picking.
#   blender -b <fit>/fit.blend -P reg_export.py -- raw.npz params.json <out dir>
# Writes mpfb.npz (skin of the human with the fit applied, masks, eyes), ref.npz (the reference cropped to the head,
# with vertex normals), views.json (ortho cameras: pixel -> world) and lm_{mpfb,ref}_{front,profile}.png.
import bpy, sys, os, json, math
import numpy as np
from mathutils import Vector
a=sys.argv[sys.argv.index('--')+1:]; raw=np.load(a[0]); P=json.load(open(a[1])); OUT=a[2]; os.makedirs(OUT,exist_ok=True)
sc=bpy.context.scene; bm=bpy.data.objects['Human']; cam=sc.camera
vg={g.name:g.index for g in bm.vertex_groups}
mods=[(m,m.show_viewport) for m in bm.modifiers]
for m,_ in mods: m.show_viewport=False
bpy.context.view_layer.update(); dg=bpy.context.evaluated_depsgraph_get(); ev=bm.evaluated_get(dg); me=ev.to_mesh()
co=np.array([v.co for v in me.vertices]); me.calc_loop_triangles()
tris=np.array([t.vertices for t in me.loop_triangles]); edges=np.array([e.vertices for e in me.edges])
ev.to_mesh_clear()
for m,v in mods: m.show_viewport=v
def gmask(name):
    gi=vg[name]; return np.array([any(g.group==gi and g.weight>0.5 for g in v.groups) for v in bm.data.vertices])
helper=gmask('HelperGeometry'); le=gmask('helper-l-eye'); re_=gmask('helper-r-eye')
eye=(co[le].mean(0)+co[re_].mean(0))/2
eo=[o for o in bpy.data.objects if 'high-poly' in o.name][0]
eme=eo.evaluated_get(bpy.context.evaluated_depsgraph_get()).to_mesh(); eme.calc_loop_triangles()
ec=np.array([eo.matrix_world@v.co for v in eme.vertices]); et=np.array([t.vertices for t in eme.loop_triangles])
np.savez(os.path.join(OUT,'mpfb.npz'),co=co,tris=tris,edges=edges,helper=helper,leye=le,reye=re_,eye=eye,
         eye_obj_co=ec,eye_obj_tris=et,eye_obj_loc=np.array(eo.location))
# the reference, placed as in ref_view.py, cropped to the head
mpp=0.0615/P['ipd_px']; unit=P['res'][0]/1.1*mpp   # the generator saw a square view of res px
R=np.array([[1,0,0],[0,0,-1],[0,1,0]],np.float32)
rv=(raw['vertices']@R.T)*unit+np.array(P['ref_origin']); rf=raw['faces']
box=(np.abs(rv[:,0])<0.12)&(rv[:,2]>eye[2]-0.20)&(rv[:,2]<eye[2]+0.13)&(rv[:,1]<eye[1]+0.16)
keep=box[rf].all(1); rf=rf[keep]
used=np.unique(rf); remap=-np.ones(len(rv),np.int64); remap[used]=np.arange(len(used)); rv=rv[used].astype(np.float64); rf=remap[rf]
fn=np.cross(rv[rf[:,1]]-rv[rf[:,0]],rv[rf[:,2]]-rv[rf[:,0]]); vn=np.zeros_like(rv)
for k in range(3): np.add.at(vn,rf[:,k],fn)
vn/=np.linalg.norm(vn,axis=1,keepdims=True)+1e-12
np.savez(os.path.join(OUT,'ref.npz'),v=rv.astype(np.float32),f=rf.astype(np.int32),n=vn.astype(np.float32))
print('EXPORT mpfb verts',len(co),'ref verts',len(rv),'faces',len(rf))
mesh=bpy.data.meshes.new('ref'); mesh.from_pydata(rv.tolist(),[],rf.tolist()); mesh.update()
for p in mesh.polygons: p.use_smooth=True
ref=bpy.data.objects.new('ref',mesh); sc.collection.objects.link(ref)
mat=bpy.data.materials.new('clay'); mat.use_nodes=True; mat.node_tree.nodes['Principled BSDF'].inputs['Base Color'].default_value=(0.6,0.6,0.6,1)
mesh.materials.append(mat)
# ortho views 1200 px over 0.24 m, centred on the eyes; pixel (u,v) -> world: right = +x (front) or +y (profile)
N=1200; W=0.24; s=W/N
sc.render.resolution_x=sc.render.resolution_y=N; sc.render.resolution_percentage=100
eye=[float(x) for x in eye]
views={'front':{'loc':[eye[0],-3,eye[2]-0.03],'rot':[math.radians(90),0,0],'right':[1,0,0]},
       'profile':{'loc':[3,eye[1]+0.03,eye[2]-0.03],'rot':[math.radians(90),0,math.radians(90)],'right':[0,1,0]}}
cam.data.type='ORTHO'; cam.data.ortho_scale=W
others=[o for o in bpy.data.objects if o.type=='MESH' and o is not ref]
hide={o.name:o.hide_render for o in others}
for who in ('mpfb','ref'):
    for o in others: o.hide_render = hide[o.name] if who=='mpfb' else True
    ref.hide_render = who!='ref'
    for name,vw in views.items():
        cam.location=vw['loc']; cam.rotation_euler=vw['rot']
        sc.render.filepath=os.path.join(OUT,f'lm_{who}_{name}.png'); bpy.ops.render.render(write_still=True)
json.dump({'N':N,'s':s,'views':views,'eye':eye},open(os.path.join(OUT,'views.json'),'w'),indent=1)
