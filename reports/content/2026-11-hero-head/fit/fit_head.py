# Hero head fit: an MPFB male with face targets from params.json, rendered as clay in orthographic front and
# left profile at a scale tied to the eye line and the chin, for an overlay on the concept's ortho views.
#   BLENDER_USER_RESOURCES=~/.local/share/gothic-ai/mpfb/blender blender -b -P fit_head.py -- params.json <out dir>
import bpy, os, sys, json, math
from mathutils import Vector
from bl_ext.user_default.mpfb.services import HumanService, TargetService, LocationService
args=sys.argv[sys.argv.index('--')+1:]; P=json.load(open(args[0])); OUT=args[1]
for o in list(bpy.data.objects): bpy.data.objects.remove(o)
macro=TargetService.get_default_macro_info_dict(); macro.update(P['macro']); macro['race']=P['race']
bm=HumanService.create_human(macro_detail_dict=macro)
stack=[{'target':k,'value':v} for k,v in P['targets'].items() if abs(v)>1e-4]
# a modifier name 'x-decr-incr' with a signed value -> the decr or incr target
fixed=[]
for t in stack:
    n,v=t['target'],t['value']
    if n.endswith('-decr-incr') or n.endswith('-down-up') or n.endswith('-in-out') or n.endswith('-backward-forward') or n.endswith('-concave-convex') or n.endswith('-compress-uncompress') or n.endswith('-square-round') or n.endswith('-pointed-triangle'):
        a,b=n.rsplit('-',2)[1:]; base=n.rsplit('-',2)[0]; fixed.append({'target':f'{base}-{b if v>0 else a}','value':abs(v)})
    else: fixed.append(t)
# sided targets (eyes, cheeks): an unsided name sets both sides
both=[]
for t in fixed:
    if TargetService.target_full_path(t['target']) or not TargetService.target_full_path('l-'+t['target']): both.append(t)
    else: both+= [{'target':'l-'+t['target'],'value':t['value']},{'target':'r-'+t['target'],'value':t['value']}]
missing=[t['target'] for t in both if not TargetService.target_full_path(t['target'])]
if missing: raise SystemExit('FIT unknown targets: %s'%missing)
TargetService.bulk_load_targets(bm,both)
eyes=None
if P.get('eyes'):
    D=LocationService.get_user_data(); HumanService.add_mhclo_asset(os.path.join(D,'eyes/high-poly/high-poly.mhclo'),bm,asset_type='Eyes')
bpy.context.view_layer.update()
mods=[(m,m.show_viewport) for m in bm.modifiers]
for m,_ in mods: m.show_viewport=False
bpy.context.view_layer.update()
dg=bpy.context.evaluated_depsgraph_get(); ev=bm.evaluated_get(dg); me=ev.to_mesh()
vg={g.name:g.index for g in bm.vertex_groups}
def grp(n):
    gi=vg[n]; return [ev.matrix_world@me.vertices[i].co for i,v in enumerate(bm.data.vertices) if any(g.group==gi and g.weight>0.5 for g in v.groups)]
helper=vg['HelperGeometry']
body=[ev.matrix_world@me.vertices[i].co for i,v in enumerate(bm.data.vertices) if not any(g.group==helper for g in v.groups)]
le=grp('helper-l-eye'); re_=grp('helper-r-eye')
eyez=(sum(p.z for p in le+re_)/len(le+re_)); eyey=sum(p.y for p in le+re_)/len(le+re_)
head=[p for p in body if p.z>eyez-0.16]
neck_c=[p for p in body if abs(p.z-(eyez-0.13))<0.004]
chin=min((p for p in body if p.z>eyez-0.16 and abs(p.x)<0.01 and p.y<eyey+0.02), key=lambda p:p.z)
top=max(p.z for p in body)
print('FIT eye_z %.4f chin_z %.4f top_z %.4f eye_chin %.4f head_w %.4f'%(eyez,chin.z,top,eyez-chin.z,max(p.x for p in head)-min(p.x for p in head)))
ev.to_mesh_clear()
for m,v in mods: m.show_viewport=v
# clay
mat=bpy.data.materials.new('clay'); mat.use_nodes=True; b=mat.node_tree.nodes['Principled BSDF']; b.inputs['Base Color'].default_value=(0.62,0.5,0.42,1); b.inputs['Roughness'].default_value=0.6
bm.data.materials.clear(); bm.data.materials.append(mat)
sc=bpy.context.scene
for o in bpy.data.objects:
    if o.type=='MESH' and o!=bm and 'high-poly' not in o.name.lower(): o.hide_render=True
w=bpy.data.worlds.new('w'); sc.world=w; w.use_nodes=True; w.node_tree.nodes['Background'].inputs[0].default_value=(0.48,0.49,0.5,1); w.node_tree.nodes['Background'].inputs[1].default_value=0.8
ld=bpy.data.lights.new('k','SUN'); ld.energy=2.5; lo=bpy.data.objects.new('k',ld); sc.collection.objects.link(lo); lo.rotation_euler=(math.radians(70),0,math.radians(15))
cam=bpy.data.cameras.new('c'); cam.type='ORTHO'; co=bpy.data.objects.new('c',cam); sc.collection.objects.link(co); sc.camera=co
sc.render.engine='BLENDER_EEVEE'; sc.render.resolution_x=P['res'][0]; sc.render.resolution_y=P['res'][1]; sc.view_settings.view_transform='Standard'
sc.render.film_transparent=True
# frame: the eye centres on the concept's pupils; the scale from the distance between the pupils
lc=sum(le,Vector())/len(le); rc=sum(re_,Vector())/len(re_); ipd=(lc-rc).length
s=ipd/P['ipd_px']; N=P['res'][0]; cam.ortho_scale=s*N; c=N/2
print('FIT ipd_m %.4f'%ipd)
fx,fy=P['front_eye']; px,py=P['profile_eye']
for view,loc,rot in [('front',Vector((-(fx-c)*s,-3,eyez+(fy-c)*s)),(math.radians(90),0,0)),
                     ('profile',Vector((3,eyey-(px-c)*s,eyez+(py-c)*s)),(math.radians(90),0,math.radians(90)))]:
    co.location=loc; co.rotation_euler=rot
    sc.render.filepath=os.path.join(OUT,f'fit_{view}.png'); bpy.ops.render.render(write_still=True)
if P.get('quarter'):
    a=math.radians(35); r=3
    co.location=Vector((math.sin(a)*r,-math.cos(a)*r,eyez-0.03)); co.rotation_euler=(math.radians(90),0,a)
    cam.ortho_scale=0.42
    sc.render.filepath=os.path.join(OUT,'fit_quarter.png'); bpy.ops.render.render(write_still=True)
# the eye point and the pupils' distance of the fit, for a frame computed without hand picks (scripts/content/npc/auto_params.py)
json.dump({'eye':[0.0,eyey,eyez],'ipd_m':ipd},open(os.path.join(OUT,'fit.json'),'w'))
bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT,'fit.blend'))
