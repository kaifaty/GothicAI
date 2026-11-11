# The generator's head (raw.npz: +Z up, front -Y, canonical -0.5..0.5) placed on the MPFB fit by the cameras the
# views were made with, rendered with the fit's cameras: front, profile, three quarters — ref alone and over MPFB.
#   blender -b <fit dir>/fit.blend -P ref_view.py -- raw.npz params.json <out dir> [unit_m] [ox oy oz]
import bpy, bmesh, sys, os, json, math
import numpy as np
from mathutils import Vector, Matrix
a=sys.argv[sys.argv.index('--')+1:]; raw=np.load(a[0]); P=json.load(open(a[1])); OUT=a[2]; os.makedirs(OUT,exist_ok=True)
sc=bpy.context.scene; bm=bpy.data.objects['Human']; cam=sc.camera
# the MPFB eye: the frame of the views
vg={g.name:g.index for g in bm.vertex_groups}
mods=[(m,m.show_viewport) for m in bm.modifiers]
for m,_ in mods: m.show_viewport=False
bpy.context.view_layer.update(); dg=bpy.context.evaluated_depsgraph_get(); ev=bm.evaluated_get(dg); me=ev.to_mesh()
def grp(n):
    gi=vg[n]; return [me.vertices[i].co.copy() for i,v in enumerate(bm.data.vertices) if any(g.group==gi and g.weight>0.5 for g in v.groups)]
eye=sum(grp('helper-l-eye')+grp('helper-r-eye'),Vector())/len(grp('helper-l-eye')+grp('helper-r-eye'))
ev.to_mesh_clear()
for m,v in mods: m.show_viewport=v
# views: a crop of N px holds 1.1 canonical units (fov 0.349 at d 3.119); m/px from the pupils
mpp=0.0615/P['ipd_px']; unit=float(a[3]) if len(a)>3 else 887/1.1*mpp
fx,fy=P['front_eye']; px,py=P['profile_eye']
crop_c=P.get('crop_centre',{'front':[471.5,443.5],'profile':[440,443.5]})
origin=Vector((0, eye.y+(crop_c['profile'][0]-px)*mpp, eye.z-(crop_c['front'][1]-fy)*mpp))
if len(a)>6: origin+=Vector(tuple(map(float,a[4:7])))
# the raw arrays come +Y up, +Z front (not meta.json's +Z up): turn them into Blender's +Z up, -Y front
R=np.array([[1,0,0],[0,0,-1],[0,1,0]],np.float32)
v=(raw['vertices']@R.T)*unit; f=raw['faces']
mesh=bpy.data.meshes.new('ref'); mesh.from_pydata([tuple(x) for x in v],[],[tuple(x) for x in f]); mesh.update()
ref=bpy.data.objects.new('ref',mesh); sc.collection.objects.link(ref); ref.location=origin
for p in mesh.polygons: p.use_smooth=True
mat=bpy.data.materials.new('refclay'); mat.use_nodes=True; mat.node_tree.nodes['Principled BSDF'].inputs['Base Color'].default_value=(0.55,0.6,0.7,1)
mesh.materials.append(mat)
print('REF verts',len(v),'faces',len(f),'unit_m %.4f'%unit,'origin',tuple(round(x,4) for x in origin))
s=mpp; N=P['res'][0]; c=N/2
views=[('front',Vector((-(fx-c)*s,-3,eye.z+(fy-c)*s)),(math.radians(90),0,0),s*N),
       ('profile',Vector((3,eye.y-(px-c)*s,eye.z+(py-c)*s)),(math.radians(90),0,math.radians(90)),s*N),
       ('quarter',Vector((math.sin(math.radians(35))*3,-math.cos(math.radians(35))*3,eye.z-0.03)),(math.radians(90),0,math.radians(35)),0.42)]
hide={o.name:o.hide_render for o in bpy.data.objects}
for show_mpfb in (False,True):
    for o in bpy.data.objects:
        if o.type=='MESH' and o is not ref: o.hide_render = hide[o.name] if show_mpfb else True
    if show_mpfb:
        mat.blend_method='BLEND' if hasattr(mat,'blend_method') else None
    for name,loc,rot,osc in views:
        cam.location=loc; cam.rotation_euler=rot; cam.data.ortho_scale=osc
        sc.render.filepath=os.path.join(OUT,f'ref_{name}{"_mpfb" if show_mpfb else ""}.png'); bpy.ops.render.render(write_still=True)
    if not show_mpfb: ref.hide_render=False
bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT,'ref.blend'))
