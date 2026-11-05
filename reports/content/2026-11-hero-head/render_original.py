# Renders the hero's original head HUM_HEAD_PONY with his face HUM_HEAD_V18_C0 (FACE_N_PLAYER=18) from the
# imported base: front, 3/4, profile, back, behind-above, then morph frames (VISEME 7 and 15, eyes closed,
# angry, friendly). Gothic's head frame: x up, z forward, y across.
#   blender -b --factory-startup -P render_original.py -- <out dir>
import bpy, json, struct, math, sys, os
from mathutils import Vector
# the newest imported base: OPENGOTHIC_BASE_CACHE (default ~/.cache/gothic-ai/base)/<fingerprint>
_root=os.path.expanduser(os.environ.get('OPENGOTHIC_BASE_CACHE','~/.cache/gothic-ai/base'))
B=max((os.path.join(_root,d) for d in os.listdir(_root) if os.path.isdir(os.path.join(_root,d,'models'))),key=os.path.getmtime)
OUT=sys.argv[sys.argv.index('--')+1] if '--' in sys.argv else '.'
j=json.load(open(B+'/models/HUM_HEAD_PONY.MMB.json'))['data']; buf=open(B+'/models/HUM_HEAD_PONY.MMB.bin','rb').read()
m=j['mesh']
P=[struct.unpack_from('<3f',buf,m['positions']['offset']+12*i) for i in range(m['positions']['count'])]
def morph(name,frame):
    a=[x for x in j['animations'] if x['name']==name][0]
    vi=[struct.unpack_from('<I',buf,a['vertices']['offset']+4*i)[0] for i in range(a['vertices']['count'])]
    n=len(vi); base=a['samples']['offset']+12*n*frame
    Q=list(P)
    for k,v in enumerate(vi):
        d=struct.unpack_from('<3f',buf,base+12*k); Q[v]=(P[v][0]+d[0],P[v][1]+d[1],P[v][2]+d[2])
    return Q
tex={'HUM_HEAD_PONY-HEAD':'HUM_HEAD_V18_C0','HUM_HEAD_PONY-MOUTHHOLE':'HUM_MOUTH_V0','HUM_HEAD_PONY-TEETH':'HUM_TEETH_V0'}
bpy.ops.wm.read_factory_settings(use_empty=True)
sc=bpy.context.scene
def build(Q,name):
    verts=[];faces=[];uvs=[];mats=[]
    me=bpy.data.meshes.new(name)
    for si,s in enumerate(m['sub_meshes']):
        W=[struct.unpack_from('<3f2fH',buf,s['wedges']['offset']+22*i) for i in range(s['wedges']['count'])]
        b=len(verts)
        for w in W: verts.append(Q[w[5]]); uvs.append((w[3],1-w[4]))
        for t in range(s['triangles']['count']):
            a=struct.unpack_from('<3H',buf,s['triangles']['offset']+6*t)
            faces.append((b+a[0],b+a[1],b+a[2])); mats.append(si)
    # gothic: y up, z forward -> blender z up
    me.from_pydata([(v[2],v[1],v[0]) for v in verts],[],faces)
    uvl=me.uv_layers.new()
    for poly in me.polygons:
        poly.material_index=mats[poly.index]
        for li in poly.loop_indices: uvl.data[li].uv=uvs[me.loops[li].vertex_index]
    for s in m['sub_meshes']:
        mat=bpy.data.materials.new(s['mat']['name']); mat.use_nodes=True
        bsdf=mat.node_tree.nodes['Principled BSDF']; bsdf.inputs['Roughness'].default_value=0.8
        im=bpy.data.images.load(B+'/textures/'+tex[s['mat']['name']]+'.dds')
        tn=mat.node_tree.nodes.new('ShaderNodeTexImage'); tn.image=im; tn.interpolation="Linear"
        mat.node_tree.links.new(tn.outputs[0],bsdf.inputs['Base Color'])
        me.materials.append(mat)
    for p in me.polygons: p.use_smooth=True
    ob=bpy.data.objects.new(name,me); sc.collection.objects.link(ob); return ob
ob=build(P,'head')
bb=[ob.matrix_world@Vector(c) for c in ob.bound_box]
lo=Vector([min(v[i] for v in bb) for i in range(3)]); hi=Vector([max(v[i] for v in bb) for i in range(3)])
print('BBOX cm', lo, hi, hi-lo)
c=(lo+hi)/2
w=bpy.data.worlds.new('w'); sc.world=w; w.use_nodes=True; w.node_tree.nodes['Background'].inputs[0].default_value=(0.35,0.36,0.38,1); w.node_tree.nodes['Background'].inputs[1].default_value=0.6
ld=bpy.data.lights.new('k','SUN'); ld.energy=3; lo_=bpy.data.objects.new('k',ld); sc.collection.objects.link(lo_); lo_.rotation_euler=(math.radians(55),0,math.radians(-30))
cam=bpy.data.cameras.new('c'); cam.type='ORTHO'; cam.ortho_scale=(hi-lo).length*1.05
co=bpy.data.objects.new('c',cam); sc.collection.objects.link(co); sc.camera=co
sc.render.engine='BLENDER_EEVEE' if 'BLENDER_EEVEE' in [e.identifier for e in bpy.types.RenderSettings.bl_rna.properties['engine'].enum_items] else 'BLENDER_EEVEE_NEXT'
sc.render.resolution_x=sc.render.resolution_y=512
sc.view_settings.view_transform='Standard'
def shot(azim,fn,el=0):
    r=60; a=math.radians(azim); e=math.radians(el)
    co.location=c+Vector((math.sin(a)*r*math.cos(e),-math.cos(a)*r*math.cos(e),r*math.sin(e)))
    d=c-co.location; co.rotation_euler=d.to_track_quat('-Z','Y').to_euler()
    sc.render.filepath=os.path.join(OUT,fn); bpy.ops.render.render(write_still=True)
for az,n,el in [(90,'o_front.png',0),(45,'o_q.png',0),(0,'o_side.png',0),(270,'o_back.png',0),(240,'o_tp.png',25)]: shot(az,n,el)
# visemes / mood
for nm,fr,fn in [('VISEME',15,'o_viseme15.png'),('VISEME',7,'o_viseme7.png'),('S_EYESCLOSED',0,'o_closed.png'),('S_ANGRY',0,'o_angry.png'),('S_FRIENDLY',0,'o_friendly.png')]:
    bpy.data.objects.remove(ob); ob=build(morph(nm,fr),nm); shot(60,fn)
