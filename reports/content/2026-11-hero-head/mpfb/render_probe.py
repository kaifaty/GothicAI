# Counts the head part of the MPFB basemesh (faces above the neck joint, helpers left out) and renders the probe:
# four views, then visemes aa, O, FF and expressions (suspicious, smirk, blink).
#   BLENDER_USER_RESOURCES=~/.local/share/gothic-ai/mpfb/blender blender -b <out dir>/probe.blend -P render_probe.py -- <out dir>
import bpy, bmesh, os, sys, math
from mathutils import Vector
OUT=sys.argv[sys.argv.index('--')+1]
sc=bpy.context.scene
bm_=bpy.data.objects['Human']
me=bm_.data
vg={g.name:g.index for g in bm_.vertex_groups}
def center(gname):
    gi=vg[gname]; ps=[v.co for v in me.vertices if any(g.group==gi and g.weight>0.5 for g in v.groups)]
    return sum(ps,Vector())/len(ps)
neck=center('joint-neck'); head=center('joint-head')
helper=vg['HelperGeometry']
ishelp=[any(g.group==helper for g in v.groups) for v in me.vertices]
cut=neck.z
nf=0;nt=0
for p in me.polygons:
    vs=p.vertices
    if any(ishelp[i] for i in vs): continue
    if min(me.vertices[i].co.z for i in vs)>=cut: nf+=1; nt+=len(vs)-2
print('HEAD above neck joint: faces',nf,'tris',nt)
zs=[v.co.z for i,v in enumerate(me.vertices) if not ishelp[i]]
top=max(zs); print('NECKJOINT z',round(neck.z,3),'HEADJOINT z',round(head.z,3),'TOP',round(top,3))
# chin: lowest point of face front near head x=0
# render setup
for o in bpy.data.objects:
    if o.type in ('LIGHT','CAMERA'): bpy.data.objects.remove(o)
w=bpy.data.worlds.new('w'); sc.world=w; w.use_nodes=True
bg=w.node_tree.nodes['Background']; bg.inputs[0].default_value=(0.35,0.36,0.38,1); bg.inputs[1].default_value=0.5
ld=bpy.data.lights.new('k','SUN'); ld.energy=3.5; lo=bpy.data.objects.new('k',ld); sc.collection.objects.link(lo); lo.rotation_euler=(math.radians(50),0,math.radians(35))
cam=bpy.data.cameras.new('c'); cam.lens=85; co=bpy.data.objects.new('c',cam); sc.collection.objects.link(co); sc.camera=co
sc.render.engine='CYCLES'; sc.cycles.samples=48; sc.cycles.device='CPU'
sc.render.resolution_x=sc.render.resolution_y=640
sc.view_settings.view_transform='AgX'
c=Vector((0,head.y if False else 0, (neck.z+top)/2)); c.y=head.y
def shot(az,fn,el=5,r=1.15):
    a=math.radians(az); e=math.radians(el)
    co.location=c+Vector((math.sin(a)*r*math.cos(e),-math.cos(a)*r*math.cos(e),r*math.sin(e)))
    co.rotation_euler=(c-co.location).to_track_quat('-Z','Y').to_euler()
    sc.render.filepath=os.path.join(OUT,fn); bpy.ops.render.render(write_still=True)
keys=me.shape_keys.key_blocks
def setk(d):
    for k in keys:
        if k.name in d: k.value=d[k.name]
        elif k.name.startswith(('viseme','brow','eye','jaw','mouth','cheek','nose','tongue')): k.value=0
for az,fn in [(0,'p_front.png'),(35,'p_q.png'),(90,'p_side.png'),(180,'p_back.png')]: shot(az,fn)
for d,fn in [({'viseme_aa':1},'p_aa.png'),({'viseme_O':1},'p_O.png'),({'viseme_FF':1},'p_FF.png'),
             ({'browDownLeft':.7,'browDownRight':.7,'eyeSquintLeft':.5,'eyeSquintRight':.5,'mouthPressLeft':.4,'mouthPressRight':.4},'p_suspicious.png'),
             ({'mouthSmileLeft':.5,'mouthSmileRight':.3,'eyeSquintLeft':.3,'eyeSquintRight':.3},'p_smirk.png'),
             ({'eyeBlinkLeft':1,'eyeBlinkRight':1},'p_blink.png')]:
    setk(d); shot(25,fn)
