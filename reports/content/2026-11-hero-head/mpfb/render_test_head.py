# Checks the test head GLB of export_test_head.py: front and profile against the original HUM_HEAD_PONY (red wire,
# same bone frame), then shapes on all parts at once - jaw open, blink, smile, brows, U, look up.
#   blender -b --factory-startup -P render_test_head.py -- <test-head.glb> <out dir> [<face profile .json>]
# With a face profile (test-head.face.json) it renders its old names instead: each anim's frames as the engine
# shows them, VISEME at frames 0, 5, 10 and 15.
import bpy, json, struct, os, sys, math
from mathutils import Vector

ARGS = sys.argv[sys.argv.index('--')+1:]
GLB, OUT = ARGS[:2]
PROFILE = ARGS[2] if len(ARGS) > 2 else None
os.makedirs(OUT, exist_ok=True)
bpy.ops.wm.read_factory_settings(use_empty=True)
sc = bpy.context.scene

bpy.ops.import_scene.gltf(filepath=GLB)
parts = [o for o in sc.objects if o.type == 'MESH']
pivot = bpy.data.objects.new('pivot', None); sc.collection.objects.link(pivot)
for o in parts:
    o.parent = pivot
pivot.rotation_euler = (0, math.radians(90), 0)   # export axes (-up, -forward, across) -> Z up, face to -Y

# the original in the same frame: x up, y across, z forward (cm)
_base = os.path.expanduser(os.environ.get('OPENGOTHIC_BASE_CACHE', '~/.cache/gothic-ai/base'))
B = max((os.path.join(_base, d) for d in os.listdir(_base) if os.path.isdir(os.path.join(_base, d, 'models'))),
        key=os.path.getmtime)
j = json.load(open(B+'/models/HUM_HEAD_PONY.MMB.json'))['data']; buf = open(B+'/models/HUM_HEAD_PONY.MMB.bin', 'rb').read()
m = j['mesh']
P = [struct.unpack_from('<3f', buf, m['positions']['offset']+12*i) for i in range(m['positions']['count'])]
verts, faces = [], []
for s in m['sub_meshes']:
    W = [struct.unpack_from('<3f2fH', buf, s['wedges']['offset']+22*i) for i in range(s['wedges']['count'])]
    b = len(verts)
    for w in W:
        p = P[w[5]]; verts.append((p[1]/100, -p[2]/100, p[0]/100))
    for t in range(s['triangles']['count']):
        a = struct.unpack_from('<3H', buf, s['triangles']['offset']+6*t); faces.append((b+a[0], b+a[1], b+a[2]))
me = bpy.data.meshes.new('orig'); me.from_pydata(verts, [], faces)
orig = bpy.data.objects.new('orig', me); sc.collection.objects.link(orig)
wf = orig.modifiers.new('w', 'WIREFRAME'); wf.thickness = 0.0012
orig.color = (0.9, 0.1, 0.1, 1)

sc.render.engine = 'BLENDER_WORKBENCH'
sc.display.shading.light = 'STUDIO'; sc.display.shading.color_type = 'OBJECT'
sc.render.resolution_x = sc.render.resolution_y = 600
sc.render.film_transparent = False
sc.world = bpy.data.worlds.new('w'); sc.world.color = (0.25, 0.25, 0.25)
for o in parts:
    o.color = (0.85, 0.72, 0.62, 1) if o.name in ('face',) else (0.95, 0.95, 0.95, 1)
cam = bpy.data.objects.new('cam', bpy.data.cameras.new('cam')); sc.collection.objects.link(cam); sc.camera = cam
cam.data.type = 'ORTHO'
bpy.context.view_layer.update()
pts = [o.matrix_world @ Vector(c) for o in parts for c in o.bound_box]
ctr = sum(pts, Vector())/len(pts)

def shot(name, azim, scale, z=None):
    a = math.radians(azim); zc = ctr.z if z is None else z
    cam.data.ortho_scale = scale
    cam.location = (ctr.x+math.sin(a)*2, ctr.y-math.cos(a)*2, zc)
    cam.rotation_euler = (math.radians(90), 0, a)
    sc.render.filepath = os.path.join(OUT, name+'.png'); bpy.ops.render.render(write_still=True)

def pose(**w):
    for o in parts:
        if o.data.shape_keys:
            for k in o.data.shape_keys.key_blocks[1:]:
                k.value = w.get(k.name, 0.0)

orig.hide_render = PROFILE is not None
zc = ctr.z-0.015
if PROFILE:
    anims = json.load(open(PROFILE))['anims']
    for name, a in anims.items():
        frames = a['frames']
        picks = [0, 5, 10, 15] if name == 'VISEME' else range(len(frames))
        for f in picks:
            pose(**frames[f]); shot('%s_%02d' % (name, f), 25, 0.24, zc)
    sys.exit(0)
pose(); shot('a_front', 0, 0.36, 0.115); shot('b_profile', 90, 0.36, 0.115)
orig.hide_render = True
for name, w in [('c_neutral', {}), ('d_jaw', {'jawOpen': 1}), ('e_blink', {'eyeBlinkLeft': 1, 'eyeBlinkRight': 1}),
                ('f_smile', {'mouthSmileLeft': 1, 'mouthSmileRight': 1}),
                ('g_brows', {'browInnerUp': 1, 'browOuterUpLeft': 1}), ('h_U', {'viseme_U': 1}),
                ('i_lookup', {'eyeLookUpLeft': 1, 'eyeLookUpRight': 1}),
                ('j_distrust', {'browDownLeft': .8, 'browDownRight': .8, 'eyeSquintLeft': .7, 'eyeSquintRight': .7, 'mouthPressLeft': .6, 'mouthPressRight': .6})]:
    pose(**w); shot(name, 25, 0.24, zc)
