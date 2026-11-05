# Test head for the engine work (steps 0-5 of ../engine-plan.md): the MPFB probe head as one GLB in the frame of
# the original HUM_HEAD_PONY on BIP01 HEAD, with every face shape (15 visemes, 52 ARKit units) on the face and
# carried over to brows, lashes, teeth and tongue. Not the hero: the probe's average face, MPFB skin.
#   BLENDER_USER_RESOURCES=~/.local/share/gothic-ai/mpfb/blender \
#     blender -b ~/.local/share/gothic-ai/mpfb/work/probe.blend -P export_test_head.py -- <content root>
# Writes <root>/characters/hero/test-head.glb, <root>/characters/characters.json (PC_HERO) and the albedo
# materials (<root>/materials/materials.json, test-head/*.dds packed by scripts/hd/texpack.cpp).
import bpy, bmesh, os, sys, json, struct, subprocess, shutil
import numpy as np
from mathutils import Matrix
from bl_ext.user_default.mpfb.services import FaceService

ROOT = os.path.abspath(sys.argv[sys.argv.index('--')+1])
DATA = os.path.expanduser('~/.local/share/gothic-ai/mpfb/blender/extensions/.user/user_default/mpfb/data')
TEXPACK = os.path.expanduser('~/.local/share/gothic-ai/hd-textures/bin/texpack')

# the original head in its bone frame (cm): x up, y across, z forward
_base = os.path.expanduser(os.environ.get('OPENGOTHIC_BASE_CACHE', '~/.cache/gothic-ai/base'))
B = max((os.path.join(_base, d) for d in os.listdir(_base) if os.path.isdir(os.path.join(_base, d, 'models'))),
        key=os.path.getmtime)
j = json.load(open(B+'/models/HUM_HEAD_PONY.MMB.json'))['data']
buf = open(B+'/models/HUM_HEAD_PONY.MMB.bin', 'rb').read()
pos = j['mesh']['positions']
G = np.array(struct.unpack_from('<%df' % (3*pos['count']), buf, pos['offset'])).reshape(-1, 3)
g_min, g_max = G.min(0), G.max(0)
print('ORIGINAL up %.1f..%.1f across %.1f..%.1f forward %.1f..%.1f' % (g_min[0], g_max[0], g_min[1], g_max[1], g_min[2], g_max[2]))

def coords(block):
    a = np.empty(len(block.data)*3, dtype=np.float64)
    block.data.foreach_get('co', a)
    return a.reshape(-1, 3)

def world(ob, c):
    m = np.array(ob.matrix_world)
    return c @ m[:3, :3].T + m[:3, 3]

human = bpy.data.objects['Human']
for o in bpy.data.objects:
    for m in getattr(o, 'modifiers', []):
        m.show_viewport = m.show_render = False
FaceService.interpolate_targets(human)

kb = human.data.shape_keys.key_blocks
FACE = [k.name for k in kb if k.name != 'Basis' and not k.name.startswith('$')]
print('FACE SHAPES', len(FACE))

# body shape of the probe (macro keys at their values) baked into the neutral face
basis = coords(kb['Basis'])
neutral = basis.copy()
for k in kb:
    if k.name.startswith('$') and k.value != 0:
        neutral += k.value*(coords(k)-coords(k.relative_key))
height = neutral[:, 2].max()-neutral[:, 2].min()
assert 1.5 < height < 2.1, 'basemesh is not in metres: %.3f' % height

helper = set()
for gname in ('HelperGeometry', 'JointCubes'):
    gi = human.vertex_groups[gname].index
    for v in human.data.vertices:
        if any(g.group == gi and g.weight > 0 for g in v.groups):
            helper.add(v.index)
body = np.array([i not in helper for i in range(len(neutral))])
nw = world(human, neutral)
top = nw[body, 2].max()
head = body & (nw[:, 2] > top-0.2)
nose = nw[head, 1].min()   # the face looks at -Y

# MPFB (metres, Z up, face to -Y, left +X) -> the original's bone frame (cm) -> the GLB axes the engine reads back:
# gothic = (-100 x, 100 y, 100 z) of glTF, glTF = (x, z, -y) of Blender; so Blender = (-up, -forward, across)/100
# Gothic heads are larger than life: the original is ~1.18x a real head in height and width (chin, mouth and ears
# against the render of render_test_head.py), so the test head fills its volume and meets the body's neck seam.
S = 1.16
def to_export(c):
    up = (c[:, 2]-top)*100*S + g_max[0]-1.0      # 1 cm of hair over the skull of the original
    fwd = (nose-c[:, 1])*100*S + g_max[2]
    across = c[:, 0]*100*S + (g_min[1]+g_max[1])/2
    return np.stack([-up, -fwd, across], 1)/100
cut_z = top + (g_min[0]-(g_max[0]-1.0))/(100*S)  # the neck seam of the original

bpy.ops.object.select_all(action='DESELECT')
parts = []

def make_part(src, name, keep, shapes):
    """Copy of src with only the vertices in `keep`, Basis = neutral and one key per face shape, export axes."""
    ob = src.copy(); ob.data = src.data.copy(); ob.name = ob.data.name = name
    bpy.context.scene.collection.objects.link(ob)
    ob.parent = None
    sk = ob.data.shape_keys
    if sk is None or not shapes:
        if sk is not None:
            ob.shape_key_clear()
        base = world(src, neutral if src == human else coords_mesh(src))
        set_coords(ob.data.vertices, to_export(base))
    else:
        own = neutral if src == human else coords(sk.key_blocks['Basis'])
        rest = to_export(world(src, own))
        for k in list(sk.key_blocks):
            if k.name in shapes:
                d = coords(src.data.shape_keys.key_blocks[k.name])-coords(src.data.shape_keys.key_blocks[k.name].relative_key)
                set_coords(k.data, to_export(world(src, own+d)))
            elif k.name != 'Basis':
                ob.shape_key_remove(k)
        set_coords(sk.key_blocks['Basis'].data, rest)
        set_coords(ob.data.vertices, rest)
    ob.matrix_world = Matrix.Identity(4)
    bm = bmesh.new(); bm.from_mesh(ob.data); bm.verts.ensure_lookup_table()
    drop = [v for v in bm.verts if not keep(v.index)]
    bmesh.ops.delete(bm, geom=drop, context='VERTS')
    bm.to_mesh(ob.data); bm.free()
    for p in ob.data.polygons:
        p.use_smooth = True
        p.material_index = 0
    ob.data.materials.clear()
    parts.append(ob)
    return ob

def coords_mesh(o):
    a = np.empty(len(o.data.vertices)*3); o.data.vertices.foreach_get('co', a); return a.reshape(-1, 3)

def set_coords(seq, c):
    seq.foreach_set('co', c.astype(np.float32).ravel())

nzc = nw[:, 2]
make_part(human, 'face', lambda i: body[i] and nzc[i] >= cut_z, FACE)
proxy = {o.name.split('.', 1)[1]: o for o in bpy.data.objects if o.parent == human and o.type == 'MESH'}
make_part(proxy['eyebrow006'], 'brows', lambda i: True, FACE)
make_part(proxy['eyelashes01'], 'lashes', lambda i: True, FACE)
make_part(proxy['teeth_base'], 'teeth', lambda i: True, FACE)
make_part(proxy['tongue01'], 'tongue', lambda i: True, FACE)
eyes = proxy['high-poly']; ex = world(eyes, coords_mesh(eyes))[:, 0]
make_part(eyes, 'eye_L', lambda i: ex[i] > 0, [])
make_part(eyes, 'eye_R', lambda i: ex[i] < 0, [])

MAT = {'face': 'SKIN', 'brows': 'BROWS', 'lashes': 'LASHES', 'teeth': 'TEETH', 'tongue': 'TONGUE', 'eye_L': 'EYE', 'eye_R': 'EYE'}
TEX = {'SKIN': 'skins/middleage_caucasian_male/middleage_lightskinned_male_diffuse.png',
       'BROWS': 'eyebrows/eyebrow006/eyebrow006.png', 'LASHES': 'eyelashes/eyelashes01/eyelashes01.png',
       'TEETH': 'teeth/teeth_base/teeth.png', 'TONGUE': 'tongue/tongue01/tongue01_diffuse.png',
       'EYE': 'eyes/materials/grey_eye.png'}
mats = {}
for ob in parts:
    key = MAT[ob.name]
    if key not in mats:
        mats[key] = bpy.data.materials.new('OWN_TEST_HEAD_'+key)
    ob.data.materials.append(mats[key])
    ob.select_set(True)
    print('PART %-7s verts %5d tris %5d shapes %d' % (ob.name, len(ob.data.vertices),
          sum(len(p.vertices)-2 for p in ob.data.polygons), len(ob.data.shape_keys.key_blocks)-1 if ob.data.shape_keys else 0))

glb = os.path.join(ROOT, 'characters', 'hero', 'test-head.glb')
os.makedirs(os.path.dirname(glb), exist_ok=True)
bpy.ops.export_scene.gltf(filepath=glb, export_format='GLB', use_selection=True, export_yup=True,
                          export_apply=False, export_morph=True, export_morph_normal=True, export_morph_tangent=False,
                          export_skins=False, export_animations=False, export_materials='EXPORT',
                          export_image_format='NONE', export_extras=False)

# alpha cards and no stray images in the GLB JSON
data = open(glb, 'rb').read()
jl = struct.unpack_from('<I', data, 12)[0]
gj = json.loads(data[20:20+jl])
for m in gj.get('materials', []):
    m.pop('pbrMetallicRoughness', None)
    if m['name'].endswith(('BROWS', 'LASHES')):
        m['alphaMode'] = 'MASK'
for k in ('images', 'textures', 'samplers'):
    gj.pop(k, None)
names = {mesh['name']: mesh.get('extras', {}).get('targetNames', []) for mesh in gj['meshes']}
for n in gj['nodes']:
    assert n['name'] in MAT, n['name']
txt = json.dumps(gj, separators=(',', ':')).encode()
txt += b' '*((4-len(txt) % 4) % 4)
rest = data[20+jl:]
out = struct.pack('<III', 0x46546C67, 2, 12+8+len(txt)+len(rest)) + struct.pack('<II', len(txt), 0x4E4F534A) + txt + rest
open(glb, 'wb').write(out)
print('GLB', glb, len(out), 'bytes; targets per mesh', {k: len(v) for k, v in names.items()})

json.dump({'PC_HERO': {'head': 'hero/test-head.glb'}}, open(os.path.join(ROOT, 'characters', 'characters.json'), 'w'), indent=2)

# albedo maps: MPFB PNGs packed as the game's own (DXT1/DXT5 with mips)
png = os.path.join(ROOT, 'materials', 'test-head-png'); dds = os.path.join(ROOT, 'materials', 'test-head')
os.makedirs(png, exist_ok=True); os.makedirs(dds, exist_ok=True)
for key, src in TEX.items():
    shutil.copyfile(os.path.join(DATA, src), os.path.join(png, key.lower()+'.png'))
subprocess.run([TEXPACK, png, dds], check=True)
shutil.rmtree(png)
mj = os.path.join(ROOT, 'materials', 'materials.json')
table = json.load(open(mj)) if os.path.exists(mj) else {}
for key in TEX:
    table['OWN_TEST_HEAD_%s.TGA' % key] = {'albedo': 'test-head/%s.dds' % key.lower()}
json.dump(table, open(mj, 'w'), indent=2)
print('DONE')
