# Test head for the engine work (steps 0-5 of ../engine-plan.md): the MPFB probe head as one GLB in the frame of
# the original HUM_HEAD_PONY on BIP01 HEAD, with every face shape (15 visemes, 52 ARKit units) on the face and
# carried over to brows, lashes, teeth and tongue. Not the hero: the probe's average face, MPFB skin.
#   BLENDER_USER_RESOURCES=~/.local/share/gothic-ai/mpfb/blender \
#     blender -b ~/.local/share/gothic-ai/mpfb/work/probe.blend -P export_test_head.py -- <content root>
# Writes <root>/characters/hero/test-head.glb, <root>/characters/characters.json (PC_HERO) and the albedo
# materials (<root>/materials/materials.json, test-head/*.dds packed by scripts/hd/texpack.cpp).
# With --hero the same probe becomes the hero's head (README, "Моргание и глаза"): the registered shape of
# fit/reg (reg.npz over mpfb.npz), the rebuilt blink (blink.npz, closed frame) as eyeBlinkLeft/Right, the eyeballs
# where the blink was built around them and the grey-blue iris; written as hero/head.glb, OWN_HERO_HEAD_*.
#     ... -P export_test_head.py -- <content root> --hero <round dir> <reg work dir> <eye texture png>
# --skin <png> and --brows <png> replace MPFB's skin and brow maps (skin/project.py; a transparent brow map hides
# the brow cards when the brows are painted on the skin).
# --hair <npz> <png> and --beard <npz> <png> add the parts cut from the generator's model (skin/hair_cut.py,
# skin/beard_shell.py; metres, the frame of the registered head). The hair is rigid; the beard is bound to the skin
# and follows every face shape; its map is cut by alpha.
import bpy, bmesh, os, sys, json, struct, subprocess, shutil
import numpy as np
from mathutils import Matrix
from bl_ext.user_default.mpfb.services import FaceService

ARGS = sys.argv[sys.argv.index('--')+1:]
ROOT = os.path.abspath(ARGS[0])
HERO = ARGS[ARGS.index('--hero')+1:ARGS.index('--hero')+4] if '--hero' in ARGS else None
NAME, MATS = ('head', 'HERO_HEAD') if HERO else ('test-head', 'TEST_HEAD')
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
kb = human.data.shape_keys.key_blocks

# body shape of the probe (macro keys at their values) baked into the neutral face
basis = coords(kb['Basis'])
neutral = basis.copy()
for k in kb:
    if k.name.startswith('$') and k.value != 0:
        neutral += k.value*(coords(k)-coords(k.relative_key))

if HERO:
    from bl_ext.user_default.mpfb.services import ClothesService
    RD, WD, EYE_PNG = (os.path.expanduser(a) for a in HERO)
    R = np.load(os.path.join(RD, 'reg.npz')); M = np.load(os.path.join(WD, 'mpfb.npz'))
    BL = np.load(os.path.join(RD, 'blink.npz'))
    assert np.abs(M['co'][neutral[:, 2] < 1.0]-neutral[neutral[:, 2] < 1.0]).max() < 1e-5, 'the fit is not on this probe'
    # the registered head as one more macro-like key, so the proxies (brows, lashes, teeth, tongue) refit onto it
    hk = human.shape_key_add(name='$hero', from_mix=False)
    hk.data.foreach_set('co', (basis+R['co']-neutral).astype(np.float32).ravel()); hk.value = 1.0
    probe_neutral, neutral = neutral, R['co'].astype(np.float64)
    # the rebuilt blink, closed frame, as the ARKit blink of its side (MPFB l = +x)
    for side in ('l', 'r'):
        d = BL[side][list(BL['t']).index(1.0)]
        sx = np.sign(neutral[np.linalg.norm(d, axis=1) > 1e-4, 0].mean())
        for name in ('eyeBlinkLeft', 'eyeBlinkRight'):
            m0 = coords(kb[name])-basis
            if np.sign(neutral[np.linalg.norm(m0, axis=1) > 1e-4, 0].mean()) == sx:
                kb[name].data.foreach_set('co', (basis+d).astype(np.float32).ravel())
                print('BLINK', side, '->', name, 'max mm %.1f' % (1000*np.linalg.norm(d, axis=1).max()))
    # teeth and tongue refit by their MPFB binding; brows and lashes are bound to helper geometry the registration
    # did not shape, so they are rebound to the skin itself after the face shapes (rebind_to_skin below)
    for o in bpy.data.objects:
        if o.parent == human and o.type == 'MESH' and ('teeth' in o.name or 'tongue' in o.name):
            ClothesService.fit_clothes_to_human(o, human, set_parent=False)
    # the eyeballs of the fit moved with their helpers, as blink_build.py placed them for the lid clearance
    eo = [o for o in bpy.data.objects if o.parent == human and 'high-poly' in o.name][0]
    ec = M['eye_obj_co'].astype(np.float64).copy()
    for side, mask, sg in (('l', M['leye'], 1), ('r', M['reye'], -1)):
        sel = ec[:, 0]*sg > 0
        ec[sel] += R['co'][mask].mean(0)-M['co'][mask].mean(0)
    inv = np.array(eo.matrix_world.inverted())
    eo.data.vertices.foreach_set('co', (ec @ inv[:3, :3].T + inv[:3, 3]).astype(np.float32).ravel())

FaceService.interpolate_targets(human)
FACE = [k.name for k in kb if k.name != 'Basis' and not k.name.startswith('$')]
print('FACE SHAPES', len(FACE))

def rebind_to_skin(ob, rest_skin, skin):
    """Wrap ob onto the skin: each vertex keeps its nearest skin triangle, barycentric point and offset in the
    triangle's frame, measured on rest_skin (where ob was fitted) and rebuilt on `skin` and on every face shape."""
    from mathutils.bvhtree import BVHTree
    from mathutils.geometry import barycentric_transform
    from mathutils import Vector
    human.data.calc_loop_triangles()
    tris = np.array([t.vertices[:] for t in human.data.loop_triangles])
    tris = tris[body[tris].all(1)]
    bvh = BVHTree.FromPolygons([Vector(v) for v in rest_skin], tris.tolist())
    pv = world(ob, coords_mesh(ob))
    def frame(P, t):
        a, b, c = P[t[:, 0]], P[t[:, 1]], P[t[:, 2]]
        e1 = (b-a)/np.linalg.norm(b-a, axis=1, keepdims=True)
        n = np.cross(b-a, c-a); n /= np.linalg.norm(n, axis=1, keepdims=True)
        return a, b, c, e1, np.cross(n, e1), n
    hit = [bvh.find_nearest(Vector(v)) for v in pv]
    ti = np.array([h[2] for h in hit]); t = tris[ti]
    a, b, c, e1, e2, n = frame(rest_skin, t)
    near = np.array([h[0] for h in hit])
    # barycentric of the nearest point
    v0, v1, v2 = b-a, c-a, near-a
    d00, d01, d11 = (v0*v0).sum(1), (v0*v1).sum(1), (v1*v1).sum(1)
    d20, d21 = (v2*v0).sum(1), (v2*v1).sum(1)
    den = d00*d11-d01*d01
    wb = (d11*d20-d01*d21)/den; wc = (d00*d21-d01*d20)/den; wa = 1-wb-wc
    o = pv-near
    off = np.stack([(o*e1).sum(1), (o*e2).sum(1), (o*n).sum(1)], 1)
    def place(P):
        a, b, c, e1, e2, n = frame(P, t)
        return wa[:, None]*a+wb[:, None]*b+wc[:, None]*c + off[:, :1]*e1+off[:, 1:2]*e2+off[:, 2:]*n
    inv = np.array(ob.matrix_world.inverted()); loc = lambda P: P @ inv[:3, :3].T + inv[:3, 3]
    if ob.data.shape_keys:
        ob.shape_key_clear()
    rest = place(skin)
    ob.data.vertices.foreach_set('co', loc(rest).astype(np.float32).ravel())
    ob.shape_key_add(name='Basis', from_mix=False)
    kept = 0
    for name in FACE:
        P = place(skin+coords(kb[name])-basis)
        if np.linalg.norm(P-rest, axis=1).max() > 1e-4:
            ob.shape_key_add(name=name, from_mix=False).data.foreach_set('co', loc(P).astype(np.float32).ravel()); kept += 1
    print('REBIND %s: offset mm median %.2f max %.2f, %d shapes' % (ob.name, 1000*np.median(np.abs(off[:, 2])),
          1000*np.abs(off[:, 2]).max(), kept))
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

if HERO:
    for o in bpy.data.objects:
        if o.parent == human and o.type == 'MESH' and ('eyebrow' in o.name or 'eyelashes' in o.name):
            rebind_to_skin(o, world(human, probe_neutral), world(human, neutral))
def npz_part(path, name):
    """A part of hair_cut.py or beard_shell.py as a mesh object in the human's frame."""
    h = np.load(os.path.expanduser(path)); me = bpy.data.meshes.new(name+'_src')
    me.from_pydata(h['v'].tolist(), [], h['f'].tolist()); me.update()
    me.uv_layers.new(name='UVMap').data.foreach_set('uv', h['uv'].reshape(-1).astype(np.float32))
    ob = bpy.data.objects.new(name+'_src', me); bpy.context.scene.collection.objects.link(ob)
    ob.matrix_world = human.matrix_world.copy()
    return ob

EXTRA = {}
for part in ('hair', 'beard'):
    if '--'+part in ARGS:
        i = ARGS.index('--'+part); EXTRA[part] = (npz_part(ARGS[i+1], part), os.path.abspath(os.path.expanduser(ARGS[i+2])))
if 'beard' in EXTRA:
    rebind_to_skin(EXTRA['beard'][0], world(human, neutral), world(human, neutral))
nzc = nw[:, 2]
make_part(human, 'face', lambda i: body[i] and nzc[i] >= cut_z, FACE)
proxy = {o.name.split('.', 1)[1]: o for o in bpy.data.objects if o.parent == human and o.type == 'MESH'}
make_part(proxy['eyebrow006'], 'brows', lambda i: True, FACE)
make_part(proxy['eyelashes01'], 'lashes', lambda i: True, FACE)
make_part(proxy['teeth_base'], 'teeth', lambda i: True, FACE)
make_part(proxy['tongue01'], 'tongue', lambda i: True, FACE)
eyes = proxy['high-poly']; ex = world(eyes, coords_mesh(eyes))[:, 0]
# The eye of MPFB is a ball and a cornea shell over it, plus four stray quads. The shell and the quads map to the
# transparent part of the texture (black with alpha 0): drawn opaque, they hid the iris behind black.
# The ball alone is kept; the cornea comes back as a shader (plan step 7). The two eyes' UVs are mirrored,
# so the test is the texture's alpha under each vertex, not a UV range.
eye_img = bpy.data.images.load(EYE_PNG if HERO else os.path.join(DATA, 'eyes/materials/grey_eye.png'))
ew, eh = eye_img.size
eye_alpha = np.array(eye_img.pixels[:]).reshape(eh, ew, 4)[..., 3]
eye_clear = np.zeros(len(eyes.data.vertices), bool)
for l, d in zip(eyes.data.loops, eyes.data.uv_layers.active.data):
    u, v = d.uv
    if eye_alpha[min(eh-1, max(0, int(v*eh))), min(ew-1, max(0, int(u*ew)))] < 0.5:
        eye_clear[l.vertex_index] = True
make_part(eyes, 'eye_L', lambda i: ex[i] > 0 and not eye_clear[i], [])
make_part(eyes, 'eye_R', lambda i: ex[i] < 0 and not eye_clear[i], [])
if 'hair' in EXTRA:
    make_part(EXTRA['hair'][0], 'hair', lambda i: True, [])
if 'beard' in EXTRA:
    make_part(EXTRA['beard'][0], 'beard', lambda i: True, FACE)

MAT = {'face': 'SKIN', 'brows': 'BROWS', 'lashes': 'LASHES', 'teeth': 'TEETH', 'tongue': 'TONGUE', 'eye_L': 'EYE', 'eye_R': 'EYE'}
TEX = {'SKIN': 'skins/middleage_caucasian_male/middleage_lightskinned_male_diffuse.png',
       'BROWS': 'eyebrows/eyebrow006/eyebrow006.png', 'LASHES': 'eyelashes/eyelashes01/eyelashes01.png',
       'TEETH': 'teeth/teeth_base/teeth.png', 'TONGUE': 'tongue/tongue01/tongue01_diffuse.png',
       'EYE': EYE_PNG if HERO else 'eyes/materials/grey_eye.png'}
for key, flag in (('SKIN', '--skin'), ('BROWS', '--brows')):          # own maps instead of MPFB's: absolute PNG paths
    if flag in ARGS:
        TEX[key] = os.path.abspath(os.path.expanduser(ARGS[ARGS.index(flag)+1]))
for part, (_, tex) in EXTRA.items():
    MAT[part] = part.upper(); TEX[part.upper()] = tex
mats = {}
for ob in parts:
    key = MAT[ob.name]
    if key not in mats:
        mats[key] = bpy.data.materials.new('OWN_%s_%s' % (MATS, key))
    ob.data.materials.append(mats[key])
    ob.select_set(True)
    print('PART %-7s verts %5d tris %5d shapes %d' % (ob.name, len(ob.data.vertices),
          sum(len(p.vertices)-2 for p in ob.data.polygons), len(ob.data.shape_keys.key_blocks)-1 if ob.data.shape_keys else 0))

glb = os.path.join(ROOT, 'characters', 'hero', NAME+'.glb')
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
    if m['name'].endswith(('BROWS', 'LASHES', 'BEARD')):
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

# the face profile: the old face animation names as weights of these shapes (authored by hand, next to this script)
shutil.copyfile(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'test-head.face.json'),
                os.path.join(ROOT, 'characters', 'hero', NAME+'.face.json'))
json.dump({'PC_HERO': {'head': 'hero/%s.glb' % NAME, 'face': 'hero/%s.face.json' % NAME}},
          open(os.path.join(ROOT, 'characters', 'characters.json'), 'w'), indent=2)

# albedo maps: MPFB PNGs packed as the game's own (DXT1/DXT5 with mips)
png = os.path.join(ROOT, 'materials', NAME+'-png'); dds = os.path.join(ROOT, 'materials', NAME)
os.makedirs(png, exist_ok=True); os.makedirs(dds, exist_ok=True)
for key, src in TEX.items():
    shutil.copyfile(os.path.join(DATA, src), os.path.join(png, key.lower()+'.png'))
subprocess.run([TEXPACK, png, dds], check=True)
shutil.rmtree(png)
mj = os.path.join(ROOT, 'materials', 'materials.json')
table = json.load(open(mj)) if os.path.exists(mj) else {}
for key in TEX:
    table['OWN_%s_%s.TGA' % (MATS, key)] = {'albedo': '%s/%s.dds' % (NAME, key.lower())}
json.dump(table, open(mj, 'w'), indent=2)
print('DONE')
