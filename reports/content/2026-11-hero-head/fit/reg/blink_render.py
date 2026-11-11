# Blink frames and the grey-blue eyes on the registered head.
#   blender -b reg_sub.blend -P blink_render.py -- blink.npz eye_texture.png <out dir> <blink in neutral l> <r>
import bpy, sys, os, gzip, math
import numpy as np
from mathutils import Vector
a=sys.argv[sys.argv.index('--')+1:]; B=np.load(a[0]); TEX=a[1]; OUT=a[2]; b0=(float(a[3])+float(a[4]))/2; os.makedirs(OUT,exist_ok=True)
bm=bpy.data.objects['Human']; n=len(bm.data.vertices); ref=bm.data.shape_keys.reference_key
basis=np.empty(n*3); ref.data.foreach_get('co',basis); basis=basis.reshape(n,3)
T=list(B['t']); kbs=[]
for k_,t_ in enumerate(T):
    kk=bm.shape_key_add(name='hero_blink_%03d'%int(round(100*t_)),from_mix=False); kk.data.foreach_set('co',(basis+B['l'][k_]+B['r'][k_]).ravel()); kk.value=0; kbs.append(kk)
FU=os.path.expanduser('~/.local/share/gothic-ai/mpfb/blender/extensions/.user/user_default/mpfb/data/targets/faceunits')
def unit(name):
    a_=np.zeros((n,3))
    for line in open(os.path.join(FU,name+'.target')):
        s=line.split()
        if len(s)>=4 and not line.startswith('#'): i=int(s[0]); x,y,z=map(float,s[1:4]); a_[i]=(0.1*x,-0.1*z,0.1*y)
    return a_
ka=bm.shape_key_add(name='arkit_blink',from_mix=False); ka.data.foreach_set('co',(basis+(1-b0)*(unit('eyeBlinkLeft')+unit('eyeBlinkRight'))).ravel()); ka.value=0
# grey-blue iris
eo=[o for o in bpy.data.objects if 'high-poly' in o.name][0]
img=bpy.data.images.load(TEX)
for mat in eo.data.materials:
    for nd in mat.node_tree.nodes:
        if nd.type=='TEX_IMAGE' and nd.image and 'normal' not in nd.image.name.lower() and 'bump' not in nd.image.name.lower(): print('TEX',mat.name,nd.image.name); nd.image=img
sc=bpy.context.scene; cam=sc.camera
for o in bpy.data.objects:
    if o.type=='LIGHT': bpy.data.objects.remove(o)
for (rx,rz,en) in [(55,-40,3.0),(80,140,1.2)]:
    ld=bpy.data.lights.new('l','SUN'); ld.energy=en; lo=bpy.data.objects.new('l',ld); sc.collection.objects.link(lo); lo.rotation_euler=(math.radians(rx),0,math.radians(rz))
vg={g.name:g.index for g in bm.vertex_groups}
mods=[(m_,m_.show_viewport) for m_ in bm.modifiers]
for m_,_ in mods: m_.show_viewport=False
bpy.context.view_layer.update(); me=bm.evaluated_get(bpy.context.evaluated_depsgraph_get()).to_mesh()
eye=Vector(np.mean([me.vertices[i].co for i,v in enumerate(bm.data.vertices) if any(g.group in (vg['helper-l-eye'],vg['helper-r-eye']) for g in v.groups)],0))
for m_,v_ in mods: m_.show_viewport=v_
sc.render.resolution_x=900; sc.render.resolution_y=420
frames=[('open',None,0),('t025',0,0),('t050',1,0),('t075',2,0),('closed',3,0),('arkit_half',None,0.5),('arkit_closed',None,1)]
for name,vb,va in frames:
    for kk in kbs: kk.value=0
    if vb is not None: kbs[vb].value=1
    ka.value=va
    for view,loc,rot in (('front',Vector((eye.x,-3,eye.z)),(math.radians(90),0,0)),
                         ('quarter',eye+Vector((math.sin(math.radians(35))*3,-math.cos(math.radians(35))*3,0)),(math.radians(90),0,math.radians(35)))):
        cam.location=loc; cam.rotation_euler=rot; cam.data.ortho_scale=0.11
        sc.render.filepath=os.path.join(OUT,f'{name}_{view}.png'); bpy.ops.render.render(write_still=True)
ka.value=0
for kk in kbs: kk.value=0
bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT,'blink.blend'))
