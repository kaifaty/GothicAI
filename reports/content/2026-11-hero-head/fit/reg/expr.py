# Expressions on the registered head: ARKit face units of MPFB added as shape keys over 'hero_reg', rendered close.
#   blender -b reg_sub.blend -P expr.py -- params.json <out dir> <blink already in the neutral, 0..1>
import bpy, sys, os, gzip, json, math
import numpy as np
from mathutils import Vector
a=sys.argv[sys.argv.index('--')+1:]; P=json.load(open(a[0])); OUT=a[1]; b0=float(a[2]); os.makedirs(OUT,exist_ok=True)
D=os.path.expanduser('~/.local/share/gothic-ai/mpfb/blender/extensions/.user/user_default/mpfb/data/targets/faceunits')
bm=bpy.data.objects['Human']; n=len(bm.data.vertices); ref=bm.data.shape_keys.reference_key
basis=np.empty(n*3); ref.data.foreach_get('co',basis); basis=basis.reshape(n,3)
def unit(name):
    p=os.path.join(D,name+'.target'); a_=np.zeros((n,3))
    for line in open(p):
        s=line.split()
        if len(s)>=4 and not line.startswith('#'): i=int(s[0]); x,y,z=map(float,s[1:4]); a_[i]=(0.1*x,-0.1*z,0.1*y)
    return a_
exprs={'neutral':{},
       'blink':{'eyeBlinkLeft':1-b0,'eyeBlinkRight':1-b0},
       'smile':{'mouthSmileLeft':0.8,'mouthSmileRight':0.8,'cheekSquintLeft':0.4,'cheekSquintRight':0.4},
       'oh':{'jawOpen':0.35,'mouthFunnel':0.7},
       'angry':{'browDownLeft':0.8,'browDownRight':0.8,'noseSneerLeft':0.4,'noseSneerRight':0.4,'eyeSquintLeft':0.4,'eyeSquintRight':0.4}}
keys={}
for e,us in exprs.items():
    if not us: continue
    d=sum(unit(u)*v for u,v in us.items())
    k=bm.shape_key_add(name='x_'+e,from_mix=False); k.data.foreach_set('co',(basis+d).ravel()); k.value=0; keys[e]=k
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
bm.evaluated_get(bpy.context.evaluated_depsgraph_get()).to_mesh_clear()
for m_,v_ in mods: m_.show_viewport=v_
sc.render.resolution_x=sc.render.resolution_y=700
for e in exprs:
    for k in keys.values(): k.value=0
    if e in keys: keys[e].value=1
    for view,loc,rot in (('front',Vector((eye.x,-3,eye.z-0.035)),(math.radians(90),0,0)),
                         ('quarter',eye+Vector((math.sin(math.radians(35))*3,-math.cos(math.radians(35))*3,-0.035)),(math.radians(90),0,math.radians(35)))):
        cam.location=loc; cam.rotation_euler=rot; cam.data.ortho_scale=0.15
        sc.render.filepath=os.path.join(OUT,f'{e}_{view}.png'); bpy.ops.render.render(write_still=True)
