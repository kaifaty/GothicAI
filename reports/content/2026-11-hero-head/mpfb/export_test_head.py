# Test head for the engine work (steps 0-5 of ../engine-plan.md): the MPFB probe head as one GLB in the frame of
# the original HUM_HEAD_PONY on BIP01 HEAD, with every face shape (15 visemes, 52 ARKit units) on the face and
# carried over to brows, lashes, teeth and tongue. Not the hero: the probe's average face, MPFB skin.
#   BLENDER_USER_RESOURCES=~/.local/share/gothic-ai/mpfb/blender \
#     blender -b ~/.local/share/gothic-ai/mpfb/work/probe.blend -P export_test_head.py -- <content root>
# Writes <root>/characters/hero/test-head.glb, test-head.frame.json, <root>/characters/characters.json (PC_HERO) and the albedo
# materials (<root>/materials/materials.json, test-head/*.dds packed by scripts/hd/texpack.cpp).
# With --hero the same probe becomes the hero's head (README, "Моргание и глаза"): the registered shape of
# fit/reg (reg.npz over mpfb.npz), the rebuilt blink (blink.npz, closed frame) as eyeBlinkLeft/Right, the eyeballs
# where the blink was built around them and the grey-blue iris; written as hero/head.glb, OWN_HERO_HEAD_*.
#     ... -P export_test_head.py -- <content root> --hero <round dir> <reg work dir> <eye texture png>
# --skin <png> and --brows <png> replace MPFB's skin and brow maps (skin/project.py; a transparent brow map hides
# the brow cards when the brows are painted on the skin).
# --skin-normal <png> and --skin-orm <png> are the skin's normal and roughness maps (scripts/content/npc/
# skin_material.py): they go into the skin material's `normal` and `orm` slots.
# --eye-orm <png> is the eyes' roughness map (scripts/content/npc/eye_material.py, with its eye_game.png as the eye
# texture): it goes into the eye material's `orm` slot and the material gets `eye: true`, so the engine lights the
# eyes with its eye shader (reports/content/2026-11-hero-head/eye-shader.md), and the face profile gets the lids' shade
# (eyes.lid_shade, LID_SHADE below) the face rig computes for that shader. Without it the eye and the face are as before.
# --cornea <eye_material.json> raises each iris onto the white and gives the front of the ball the normals of a corneal
# dome (scripts/content/npc/eye_cornea.py); with eye_material.py --depth the iris then lies that deep under it for the
# shader's refraction. --tear-line <eye_material.json> adds the part `tears`: a wet line along each lower lid that the
# face shapes move with the lid (material OWN_<head>_TEARS, MASK, its own maps). Without them the head is as before.
# --hair <npz> <png> and --beard <npz> <png> add the parts cut from the generator's model (skin/hair_cut.py,
# skin/beard_shell.py; metres, the frame of the registered head). The hair is rigid; the beard is bound to the skin
# and follows every face shape; its map is cut by alpha. A hair part with normals in its npz is hair cards
# (scripts/content/npc/hair/hair_cards.py): the normals are kept and its map is cut by alpha too.
# --hair-map <png> is the hair map of strand_atlas.py: it goes into the material's `orm` slot and the material gets
# `hair: true`, so the engine lights the cards with its hair BRDF.
import bpy, bmesh, os, sys, json, struct, subprocess, shutil
import numpy as np
from mathutils import Matrix, Vector
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
# Where the head ends. The original head has no neck: its underside rises from the chin to the nape, and the body's
# neck goes up into it. Ours is cut by the plane of that underside, up = CUT_A + CUT_B*forward (cm): a line through
# the low corners of the original's side view (its lower convex hull, within 5 cm of the lowest point). What hangs
# behind our skull (a tail of the original) is not its underside. A level cut at the chin leaves a collar of our
# neck round the body's.
_e = to_export(nw[head]); _lo = -100*_e[:, 1].max()+1.5                     # forward of the back of our skull, cm
_q = G[G[:, 2] > _lo][:, [2, 0]]; _q = _q[np.lexsort((_q[:, 1], _q[:, 0]))]; _h = []
for _p in _q:                                                               # the lower hull, left to right
    while len(_h) > 1 and (_h[-1][0]-_h[-2][0])*(_p[1]-_h[-2][1]) - (_h[-1][1]-_h[-2][1])*(_p[0]-_h[-2][0]) <= 0:
        _h.pop()
    _h.append(_p)
_pt = np.array(_h); _pt = _pt[_pt[:, 1] < g_min[0]+5.0]
CUT_B, CUT_A = np.polyfit(_pt[:, 0], _pt[:, 1], 1)
print('CUT up = %.2f %+.3f*forward cm (%d points of the original)' % (CUT_A, CUT_B, len(_pt)))
_ex = to_export(nw); over_cut = -100*_ex[:, 0]-(CUT_A+CUT_B*(-100*_ex[:, 1]))  # cm over the cut, every vertex of the human
# the same plane in the human's frame: z + CUT_B*y >= CUT_C
CUT_C = top + CUT_B*nose + (CUT_A + CUT_B*g_max[2] - (g_max[0]-1.0))/(100*S)

bpy.ops.object.select_all(action='DESELECT')
parts = []

def make_part(src, name, keep, shapes, cut=False):
    """Copy of src with only the vertices in `keep`, Basis = neutral and one key per face shape, export axes.
    cut: the part is cut by the plane of the head's underside (CUT_A, CUT_B) and ends there in a straight rim;
    `keep` then leaves a margin under the plane. Dropping whole triangles instead leaves a saw-toothed rim."""
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
    if cut:                                                            # export axes: x = -up/100, y = -forward/100; the face shapes are cut with the mesh
        bmesh.ops.bisect_plane(bm, geom=bm.verts[:]+bm.edges[:]+bm.faces[:], dist=1e-6, plane_co=(-CUT_A/100, 0, 0),
                               plane_no=Vector((1, -CUT_B, 0)).normalized(), clear_outer=True)
    bm.to_mesh(ob.data); bm.free()
    for p in ob.data.polygons:
        p.use_smooth = True
        p.material_index = 0
    ob.data.materials.clear()
    if src.name in CUSTOM_NORMALS:                                     # the axes of to_export: (x, y, z) -> (-z, y, x)
        n = CUSTOM_NORMALS[src.name]
        ob.data.normals_split_custom_set_from_vertices(np.stack([-n[:, 2], n[:, 1], n[:, 0]], 1).tolist())
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
CUSTOM_NORMALS = {}
ob_name = lambda part: part+'_src'

def npz_part(path, name):
    """A part of hair_cut.py or beard_shell.py as a mesh object in the human's frame."""
    h = np.load(os.path.expanduser(path)); me = bpy.data.meshes.new(name+'_src')
    me.from_pydata(h['v'].tolist(), [], h['f'].tolist()); me.update()
    me.uv_layers.new(name='UVMap').data.foreach_set('uv', h['uv'].reshape(-1).astype(np.float32))
    if 'n' in h.files:                                                 # hair cards carry the normals of the hair's surface
        CUSTOM_NORMALS[ob_name(name)] = h['n']
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
make_part(human, 'face', lambda i: body[i] and over_cut[i] >= -3.0, FACE, cut=True)
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
    if m['name'].endswith(('BROWS', 'LASHES', 'BEARD')+(('HAIR',) if CUSTOM_NORMALS else ())):   # hair cards are cut by alpha, a hair shell is not
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
# the cornea and the tear line (task 2 of the eye shader), on the GLB as written
EYE_JSON = {flag: os.path.abspath(os.path.expanduser(ARGS[ARGS.index(flag)+1])) for flag in ('--cornea', '--tear-line')
            if flag in ARGS}
TEAR_MAPS = None
if EYE_JSON:
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), *(['..']*4), 'scripts', 'content', 'npc'))
    import eye_cornea
    assert len(set(EYE_JSON.values())) == 1, '--cornea and --tear-line take the same eye_material.json'
    eye_cornea.apply(glb, next(iter(EYE_JSON.values())), glb, cornea='--cornea' in EYE_JSON, tears='--tear-line' in EYE_JSON)
    if '--tear-line' in EYE_JSON:
        TEAR_MAPS = eye_cornea.tear_maps()

# the face profile: the old face animation names as weights of these shapes (authored by hand, next to this script)
FACE = os.path.join(ROOT, 'characters', 'hero', NAME+'.face.json')
shutil.copyfile(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'test-head.face.json'), FACE)
# the lids' shade over the balls (eye-shader.md), only for a head with the eye shader: the share of the light the lids
# leave to a point of a ball is upper_edge at the upper lid's edge and under it and all of it upper_cm below the edge (cm
# on the ball, which is ~1.5 times a real one), the same for the lower lid, which hangs over the ball far less
LID_SHADE = {'about': "The lids' shade over the balls for the eye shader (reports/content/2026-11-hero-head/eye-shader.md).",
             'part': 'face', 'upper_cm': 0.3, 'lower_cm': 0.08, 'upper_edge': 0.25, 'lower_edge': 0.8}
if '--eye-orm' in ARGS:
    profile = json.load(open(FACE)); profile['eyes']['lid_shade'] = LID_SHADE
    json.dump(profile, open(FACE, 'w'), indent=2, ensure_ascii=False)
json.dump({'PC_HERO': {'head': 'hero/%s.glb' % NAME, 'face': 'hero/%s.face.json' % NAME}},
          open(os.path.join(ROOT, 'characters', 'characters.json'), 'w'), indent=2)
# the map of to_export as a matrix, for parts that are not meshes (strands, scripts/content/npc/hair/groom_export.py):
# the human's frame (metres) -> the head's frame in the engine (cm: up, across, forward), engine = M[:, :3] @ p + M[:, 3]
_m = np.array(human.matrix_world); _p = np.vstack([np.zeros(3), np.eye(3)])
_e = to_export(_p @ _m[:3, :3].T + _m[:3, 3]); _g = np.stack([-100*_e[:, 0], 100*_e[:, 2], -100*_e[:, 1]], 1)
json.dump({'matrix': np.column_stack([(_g[1:]-_g[0]).T, _g[0]]).tolist(), 'cut': [0.0, float(CUT_B), 1.0, float(CUT_C)]},   # the head is the human where x*c0 + y*c1 + z*c2 >= c3
          open(os.path.join(ROOT, 'characters', 'hero', NAME+'.frame.json'), 'w'))

# albedo maps: MPFB PNGs packed as the game's own (DXT1/DXT5 with mips)
png = os.path.join(ROOT, 'materials', NAME+'-png'); dds = os.path.join(ROOT, 'materials', NAME)
os.makedirs(png, exist_ok=True); os.makedirs(dds, exist_ok=True)
for key, src in TEX.items():
    shutil.copyfile(os.path.join(DATA, src), os.path.join(png, key.lower()+'.png'))
HAIR_MAP = os.path.abspath(os.path.expanduser(ARGS[ARGS.index('--hair-map')+1])) if '--hair-map' in ARGS else None
if HAIR_MAP:
    shutil.copyfile(HAIR_MAP, os.path.join(png, 'hair_map.png'))
SKIN_MAPS = {slot: os.path.abspath(os.path.expanduser(ARGS[ARGS.index(flag)+1]))
             for slot, flag in (('normal', '--skin-normal'), ('orm', '--skin-orm')) if flag in ARGS}
for slot, src in SKIN_MAPS.items():
    shutil.copyfile(src, os.path.join(png, 'skin_%s.png' % slot))
if TEAR_MAPS:
    eye_cornea.write_png(os.path.join(png, 'tears.png'), TEAR_MAPS[0])
    eye_cornea.write_png(os.path.join(png, 'tears_orm.png'), TEAR_MAPS[1])
EYE_ORM = os.path.abspath(os.path.expanduser(ARGS[ARGS.index('--eye-orm')+1])) if '--eye-orm' in ARGS else None
if EYE_ORM:
    shutil.copyfile(EYE_ORM, os.path.join(png, 'eye_orm.png'))
subprocess.run([TEXPACK, png, dds], check=True)
shutil.rmtree(png)
mj = os.path.join(ROOT, 'materials', 'materials.json')
table = json.load(open(mj)) if os.path.exists(mj) else {}
for key in TEX:
    table['OWN_%s_%s.TGA' % (MATS, key)] = {'albedo': '%s/%s.dds' % (NAME, key.lower())}
for slot in SKIN_MAPS:
    table['OWN_%s_SKIN.TGA' % MATS][slot] = '%s/skin_%s.dds' % (NAME, slot)
if EYE_ORM:
    table['OWN_%s_EYE.TGA' % MATS].update({'orm': '%s/eye_orm.dds' % NAME, 'eye': True})
if TEAR_MAPS:
    table['OWN_%s_TEARS.TGA' % MATS] = {'albedo': '%s/tears.dds' % NAME, 'orm': '%s/tears_orm.dds' % NAME}
if HAIR_MAP and 'HAIR' in TEX:
    table['OWN_%s_HAIR.TGA' % MATS].update({'orm': '%s/hair_map.dds' % NAME, 'hair': True})
json.dump(table, open(mj, 'w'), indent=2)
print('DONE')
