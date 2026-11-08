"""MPFB hero body, step 3: write HUM_BODY_NAKED0.MDM (skin + braies) and its maps into a test content root.

    uv run --with numpy --with pillow python -I mpfb_write.py <run> <HUMANS.MDH.json> <HUM_BODY_NAKED0.MDM.json> \
        <skin diffuse png> <content root> <repo> [neck colour #rrggbb]

Reads <run>/body.npz, braies.npz (mpfb_export.py) and landmarks.json (mpfb_body.py). The record keeps the
original's name, so every unarmoured man wears this body in that root. The Gothic head stays: its neck covers
the cut. The skin is MPFB's CC0 diffuse, scaled in linear light so its mean matches the neck of the hero's
original face texture (the seam), as the content rules take brightness from the replaced original.
"""
import sys, os, json, struct, importlib.util
from pathlib import Path
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fit
import rig_parts

run, mdh, orig, skin_png, root, repo = map(Path, sys.argv[1:7])
NECK = sys.argv[7] if len(sys.argv) > 7 else '#8a604a'
AO_STRENGTH = float(os.environ.get('AO_STRENGTH', 0.6))
MESH = 'HUM_BODY_NAKED0.MDM'
meta = json.load(open(run / 'landmarks.json'))
fit.S = meta['scale']
LW = {k: np.array(v) * fit.S for k, v in meta['landmarks'].items()}
skel = fit.load_skeleton(mdh)
names, parent, local, rest = skel
posed = fit.posed_skeleton(skel, LW)
tz = fit.tz_of(LW)
# RIG mode (mpfb_body.py / mpfb_export.py RIG=1): the body is skinned to its own skeleton, MPFB's game_engine rig in
# HUMANS.MDH's model space, and render-rigs.json tells the engine how that skeleton follows Gothic's pose
# (reports/engine/2026-11-modern-skeleton). Gothic's own skeleton stays the source.
RIG = meta.get('rig', False)
g_names, g_glob = names, rest
if RIG:
    rig = json.load(open(run / 'rig.json'))
    names = [b['name'] for b in rig['bones']]
    rig_bind = [np.array(b['bind']) for b in rig['bones']]
    posed = rest = rig_bind                      # a true bind pose: local positions and normals need no fix

def to_lin(c): c = np.asarray(c, float); return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
def to_srgb(c): c = np.clip(c, 0, 1); return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)

bin_ = bytearray()
def block(arr, count):
    off = len(bin_); bin_.extend(arr); return {'offset': off, 'count': count}
def obb(points):
    lo, hi = points.min(0), points.max(0)
    return {'center': ((lo + hi) / 2).tolist(), 'axes': [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]],
            'half_width': ((hi - lo) / 2 + 1e-4).tolist(), 'children': []}

src = json.load(open(orig))
template = dict(src['data']['meshes'][0]['mesh']['materials'][0])

def write_part(npz, matname, texname, size):
    d = np.load(npz)
    co_w, tri_l, tri_v, uv, ln, Wt = d['co'], d['tri_l'], d['tri_v'], d['uv'], d['loop_normal'], d['weights']
    groups = list(d['groups'])
    nv = len(co_w); assert nv <= 65535, nv
    P = fit.w2g(co_w, tz)
    node_of = np.array([names.index(g) for g in groups])
    order = np.argsort(-Wt, axis=1)[:, :4]
    w4 = np.take_along_axis(Wt, order, 1); w4[w4 < 0.01] = 0
    s = w4.sum(1, keepdims=True); assert (s > 0).all(); w4 /= s
    n4 = node_of[order]
    inv_bind = {n: np.linalg.inv(posed[n]) for n in set(n4[w4 > 0].tolist())}
    R_fix = {n: rest[n][:3, :3] @ posed[n][:3, :3].T for n in inv_bind}
    Ph = np.c_[P, np.ones(nv)]
    weights, rest_pos = [], np.zeros((nv, 3))
    for v in range(nv):
        ws = []
        for k in range(4):
            if w4[v, k] <= 0: continue
            n = int(n4[v, k]); loc = (inv_bind[n] @ Ph[v])[:3]
            ws.append((float(w4[v, k]), loc, n)); rest_pos[v] += w4[v, k] * (rest[n] @ np.r_[loc, 1])[:3]
        weights.append(ws)
    nl = np.c_[-ln[:, 0], ln[:, 1], ln[:, 2]]
    loop_vertex = np.zeros(len(uv), np.int64); loop_vertex[tri_l.ravel()] = tri_v.ravel()
    nr = np.zeros_like(nl)
    for k in range(4):
        wk = w4[loop_vertex, k]; nk = n4[loop_vertex, k]
        for n in np.unique(nk[wk > 0]):
            m = (nk == n) & (wk > 0); nr[m] += wk[m, None] * (nl[m] @ R_fix[int(n)].T)
    nr /= np.linalg.norm(nr, axis=1, keepdims=True)
    uv_g = np.c_[uv[:, 0], 1 - uv[:, 1]]
    key = np.c_[loop_vertex, np.round(uv_g * 1e5), np.round(nr * 1e4)]
    _, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
    inverse = inverse.ravel(); assert len(first) <= 65535, len(first)
    wedge_n, wedge_uv, wedge_i = nr[first], uv_g[first], loop_vertex[first]
    tris = inverse[tri_l]
    a, b, c = (rest_pos[wedge_i[tris[:, i]]] for i in range(3))
    pos_frac = float(((np.cross(b - a, c - a) * wedge_n[tris].mean(1)).sum(1) > 0).mean())
    if pos_frac > 0.5: tris = tris[:, [0, 2, 1]]
    vnorm = np.zeros((nv, 3)); np.add.at(vnorm, wedge_i, wedge_n)
    vnorm /= np.maximum(np.linalg.norm(vnorm, axis=1, keepdims=True), 1e-9)
    model_pos = rest_pos + np.array([0, fit.FLOOR, 0])
    positions = block(model_pos.astype('<f4').tobytes(), nv)
    normals = block(vnorm.astype('<f4').tobytes(), nv)
    triangles = block(tris.astype('<u2').tobytes(), len(tris))
    wdt = np.dtype([('n', '<f4', 3), ('uv', '<f4', 2), ('i', '<u2')])
    wa = np.zeros(len(first), wdt); wa['n'] = wedge_n; wa['uv'] = wedge_uv; wa['i'] = wedge_i
    wedges = block(wa.tobytes(), len(first))
    woff = len(bin_)
    for ws in weights:
        bin_.extend(struct.pack('<I', len(ws)))
        for wt, p, n in ws: bin_.extend(struct.pack('<4f', wt, *p) + bytes([n]))
    wref = {'offset': woff, 'count': nv, 'byte_length': len(bin_) - woff}
    used = sorted({n for ws in weights for _, _, n in ws})
    bboxes = [obb(np.array([p for ws in weights for _, p, n in ws if n == u])) for u in used]
    mat = dict(template); mat.update(name=matname, texture=texname, color=[128, 128, 128, 255], texture_scale=[float(size)] * 2)
    mesh = {'positions': positions, 'normals': normals, 'sub_meshes': [{'mat': mat, 'triangles': triangles, 'wedges': wedges}],
            'materials': [mat], 'alpha_test': 0, 'bbox': [model_pos.min(0).tolist(), model_pos.max(0).tolist()], 'obbox': obb(model_pos)}
    return {'mesh': mesh, 'bboxes': bboxes, 'weights': wref}, {
        'name': matname, 'positions': nv, 'wedges': len(first), 'triangles': len(tris), 'winding_flip': pos_frac > 0.5,
        'max_influences': max(len(w) for w in weights), 'rest_bbox': [model_pos.min(0).round(1).tolist(), model_pos.max(0).round(1).tolist()]}

parts, report = [], []
OUTFIT = json.load(open(run / 'export.json')).get('outfit') or []   # outfit_build.py pieces instead of the braies
plan = [(run / 'body.npz', 'OWN_HERO_BODY_MPFB', 'OWN_HERO_BODY_MPFB.TGA', 2048)]
if OUTFIT:
    plan += [(run / f'{p["name"]}.npz', f'OWN_HERO_{p["name"].upper()}', f'OWN_HERO_{p["name"].upper()}.TGA', 64)
             for p in OUTFIT]
else:
    plan += [(run / 'braies.npz', 'OWN_HERO_BRAIES', 'OWN_HERO_BRAIES.TGA', 64)]
for npz, mat, tex, size in plan:
    p, r = write_part(npz, mat, tex, size); parts.append(p); report.append(r)
record = {'format': 'gothic-model', 'version': 1, 'kind': 'mesh', 'buffer': MESH + '.bin', 'byte_length': len(bin_),
          'attachment_order': [], 'data': {'meshes': parts, 'attachments': []}}
models = root / 'models'; models.mkdir(parents=True, exist_ok=True)
(models / (MESH + '.bin')).write_bytes(bytes(bin_))
(models / (MESH + '.json')).write_text(json.dumps(record))
files = {MESH: MESH + '.json'}
if RIG:
    RIGNAME = 'HUM_RIG_MPFB'
    gi = {n: i for i, n in enumerate(g_names)}
    G = lambda n: g_glob[gi[n]]
    hum = json.load(open(mdh))['data']
    # the engine moves HUMANS.MDH's root by its root translation in the root's own frame: ours sits in that space
    T0 = np.eye(4); T0[:3, 3] = G('BIP01')[:3, :3] @ np.array(hum['root_translation'])
    nodes, ridx = [], {n: i for i, n in enumerate(names)}
    for b, M in zip(rig['bones'], rig_bind):
        par = ridx[b['parent']] if b['parent'] else -1
        local = (T0 @ M) if par < 0 else np.linalg.inv(rig_bind[par]) @ M
        nodes.append({'parent_index': par, 'name': b['name'], 'transform': local.T.reshape(-1).tolist()})
    mdh_rec = {'format': 'gothic-model', 'version': 1, 'kind': 'hierarchy', 'buffer': RIGNAME + '.MDH.bin', 'byte_length': 0,
               'data': {'nodes': nodes, 'bbox': hum['bbox'], 'collision_bbox': hum['collision_bbox'],
                        'root_translation': [0.0, 0.0, 0.0], 'checksum': 0, 'source_date': hum['source_date'],
                        'source_path': 'scripts/content/hero_body (MPFB 2 game_engine rig)'}}
    (models / (RIGNAME + '.MDH.bin')).write_bytes(b'')
    (models / (RIGNAME + '.MDH.json')).write_text(json.dumps(mdh_rec))
    files[RIGNAME + '.MDH'] = RIGNAME + '.MDH.json'

    def rot_between(a, b):
        a = a / np.linalg.norm(a); b = b / np.linalg.norm(b); v = np.cross(a, b); c = float(a @ b)
        if np.linalg.norm(v) < 1e-9: return np.eye(3)
        vx = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
        return np.eye(3) + vx + vx @ vx / (1 + c)
    def quat(R):                                  # (w, x, y, z) of a rotation matrix
        w = np.sqrt(max(0.0, 1 + R[0, 0] + R[1, 1] + R[2, 2])) / 2
        x = np.copysign(np.sqrt(max(0.0, 1 + R[0, 0] - R[1, 1] - R[2, 2])) / 2, R[2, 1] - R[1, 2])
        y = np.copysign(np.sqrt(max(0.0, 1 - R[0, 0] + R[1, 1] - R[2, 2])) / 2, R[0, 2] - R[2, 0])
        z = np.copysign(np.sqrt(max(0.0, 1 - R[0, 0] - R[1, 1] + R[2, 2])) / 2, R[1, 0] - R[0, 1])
        q = np.array([w, x, y, z]); return (q / np.linalg.norm(q)).tolist()
    ANKLE = np.array([0.0, -5.0, 9.0])            # Gothic's foot node sits 5 cm above and 9 cm behind a real ankle
    def ankle_world(S): return G(f'BIP01 {S} FOOT')[:3, 3] + ANKLE
    MAP = {'pelvis': 'PELVIS', 'spine_01': 'SPINE', 'spine_02': 'SPINE1', 'spine_03': 'SPINE2', 'neck_01': 'NECK', 'head': 'HEAD'}
    DIRS = {}
    for sd, S in (('l', 'L'), ('r', 'R')):
        MAP.update({f'clavicle_{sd}': f'{S} CLAVICLE', f'upperarm_{sd}': f'{S} UPPERARM', f'lowerarm_{sd}': f'{S} FOREARM',
                    f'hand_{sd}': f'{S} HAND', f'thigh_{sd}': f'{S} THIGH', f'calf_{sd}': f'{S} CALF',
                    f'foot_{sd}': f'{S} FOOT', f'ball_{sd}': f'{S} TOE0'})
        DIRS.update({f'clavicle_{sd}': (G(f'BIP01 {S} UPPERARM')[:3, 3] - G(f'BIP01 {S} CLAVICLE')[:3, 3]),
                     f'upperarm_{sd}': (G(f'BIP01 {S} FOREARM')[:3, 3] - G(f'BIP01 {S} UPPERARM')[:3, 3]),
                     f'lowerarm_{sd}': (G(f'BIP01 {S} HAND')[:3, 3] - G(f'BIP01 {S} FOREARM')[:3, 3]),
                     f'hand_{sd}': (G(f'BIP01 {S} FINGER0')[:3, 3] - G(f'BIP01 {S} HAND')[:3, 3]),
                     f'thigh_{sd}': (G(f'BIP01 {S} CALF')[:3, 3] - G(f'BIP01 {S} THIGH')[:3, 3]),
                     f'calf_{sd}': (ankle_world(S) - G(f'BIP01 {S} CALF')[:3, 3]),
                     f'foot_{sd}': (G(f'BIP01 {S} TOE0')[:3, 3] - ankle_world(S))})
    # the spine and the neck as Gothic's bind holds them: MPFB's own curve (the upper back 17 degrees and the neck 8.5
    # further forward than Gothic's) bent our hero over in every pose Gothic leans in (reports/engine/
    # 2026-11-modern-skeleton, «Аудит»); the head keeps its own turn (where the face looks is Gothic's)
    DIRS.update({'spine_01': G('BIP01 SPINE1')[:3, 3] - G('BIP01 SPINE')[:3, 3],
                 'spine_02': G('BIP01 SPINE2')[:3, 3] - G('BIP01 SPINE1')[:3, 3],
                 'spine_03': G('BIP01 NECK')[:3, 3] - G('BIP01 SPINE2')[:3, 3],
                 'neck_01': G('BIP01 HEAD')[:3, 3] - G('BIP01 NECK')[:3, 3]})
    tails = {b['name']: np.array(b['tail']) for b in rig['bones']}
    # the spine's segments joint to joint, as Gothic's (a bone's tail in MPFB need not sit on the next joint)
    CHAIN = {'spine_01': 'spine_02', 'spine_02': 'spine_03', 'spine_03': 'neck_01', 'neck_01': 'head'}
    align, bones_out = {}, []
    for b, M in zip(rig['bones'], rig_bind):
        n = b['name']; e = {'name': n}
        if n in MAP:
            e['source'] = 'BIP01 ' + MAP[n]
            ours = (rig_bind[ridx[CHAIN[n]]][:3, 3] if n in CHAIN else tails[n]) - M[:3, 3]
            A = rot_between(ours, DIRS[n]) if n in DIRS else np.eye(3)   # the pelvis, head stay
            align[n] = A; e['align'] = quat(A)
        bones_out.append(e)
    # the fist (mpfb_export.py): a phalanx takes the turn of Gothic's FINGER0, the thumb the hand's, on top of the
    # hand's align and its own curl from the bind pose: R = dR(source) * A_hand * (R_fist R_bind^T) * R_bind
    by_name = {e['name']: e for e in bones_out}
    for b, M in zip(rig['bones'], rig_bind):
        if 'fist' not in b:
            continue
        n = b['name']; sd = n[-1]; S = sd.upper()
        A = align[f'hand_{sd}'] @ np.array(b['fist']) @ M[:3, :3].T
        by_name[n]['source'] = f'BIP01 {S} HAND' if n.startswith('thumb') else f'BIP01 {S} FINGER0'
        by_name[n]['align'] = quat(A)
    B = lambda n: rig_bind[ridx[n]]
    def socket(src, bone, shift=np.zeros(3)):
        Mg = G(src).copy(); Mg[:3, 3] += shift
        Mb = B(bone).copy(); A = align.get(bone, np.eye(3)); Mb[:3, :3] = A @ Mb[:3, :3]
        if bone.startswith('hand_'):              # the slot keeps its offset from the hand, as on Gothic's hand
            Mb[:3, 3] = G('BIP01 ' + MAP[bone])[:3, 3]
        off = np.linalg.inv(Mb) @ Mg
        return {'bone': bone, 'offset': off.T.reshape(-1).tolist()}
    head_shift = np.array([0.0, 0.0, rig['head_dz_cm']])
    # Gothic's clips have legs of their own lengths (the shin 44.9 or 49.1 cm), ours are fitted to the bind: the knees
    # bend as Gothic's, the pelvis takes the height that needs, a foot on the ground pivots about Gothic's ball (root
    # knees); the IK eases the last SOFT cm (reports/engine/2026-11-modern-skeleton, «Ноги», «Аудит»). Twist bones and
    # the shoulder rhythm - «Верх тела»; the arms - «Аудит» (rig_parts.py); the twist bones are in the rig only when
    # mpfb_export.py made them
    SOFT = rig_parts.SOFT
    twists, shoulders = rig_parts.table()
    twists = [t for t in twists if t['bone'] in ridx]
    table = {'format': 'gothic-render-rigs', 'version': 1, 'rigs': {RIGNAME: {
        'skeleton': RIGNAME + '.MDH', 'source': 'HUMANS.MDH', 'bones': bones_out,
        'root': {'bone': 'pelvis', 'source': 'BIP01 PELVIS', 'knees': rig_parts.KNEES},
        'legs': [{'thigh': f'thigh_{sd}', 'calf': f'calf_{sd}', 'foot': f'foot_{sd}', 'ball': f'ball_{sd}',
                  'source_foot': f'BIP01 {S} FOOT', 'source_knee': f'BIP01 {S} CALF',
                  'ankle': (np.linalg.inv(G(f'BIP01 {S} FOOT')) @ np.r_[ankle_world(S), 1.0])[:3].tolist(),
                  'soft': SOFT}
                 for sd, S in (('l', 'L'), ('r', 'R'))],
        'twists': twists, 'shoulders': shoulders, 'arms': rig_parts.arms(),
        'sockets': {'BIP01 HEAD': socket('BIP01 HEAD', 'head', head_shift),
                    'ZS_RIGHTHAND': socket('ZS_RIGHTHAND', 'hand_r'), 'ZS_LEFTHAND': socket('ZS_LEFTHAND', 'hand_l')}}},
        'meshes': {MESH: RIGNAME}}
    (models / 'render-rigs.json').write_text(json.dumps(table, indent=1) + '\n')
(models / 'models.json').write_text(json.dumps({'format': 'gothic-models', 'version': 1, 'files': files}, indent=1) + '\n')

# maps through the project's packer
spec = importlib.util.spec_from_file_location('material_fixture', repo / 'scripts/content/test-material.py')
sys.path.insert(0, str(repo / 'scripts/content'))
mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
skin = np.asarray(Image.open(skin_png).convert('RGB')).astype(np.float64) / 255

def pad_islands(img, npz, steps=24):
    """Grow the UV islands' colour outward, so mips and bilinear taps at a seam never reach the background."""
    from PIL import ImageDraw
    d = np.load(npz); uv = d['uv']; H, W = img.shape[:2]
    m = Image.new('L', (W, H), 0); dr = ImageDraw.Draw(m)
    for t in d['tri_l']:
        dr.polygon([(uv[i, 0] * W, (1 - uv[i, 1]) * H) for i in t], fill=255)
    mask = np.asarray(m) > 0
    img = img.copy(); img[~mask] = 0; w = mask.astype(np.float64)
    for _ in range(steps):
        acc = np.zeros_like(img); cnt = np.zeros_like(w)
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            acc += np.roll(img * w[..., None], (dy, dx), (0, 1)); cnt += np.roll(w, (dy, dx), (0, 1))
        grow = (w == 0) & (cnt > 0)
        img[grow] = acc[grow] / cnt[grow, None]; w = np.where(grow, 1.0, w)
    img[w == 0] = img[mask].mean(0)            # far from any island: the mean skin, so small mips stay skin
    return img, mask

skin, island = pad_islands(skin, run / 'body.npz')
coverage = float(island.mean())
target = to_lin(np.array([int(NECK[i:i + 2], 16) for i in (1, 3, 5)]) / 255)
lin = to_lin(skin); gain = target / lin[island].mean(0)   # the mean over the islands, the skin itself
skin = to_srgb(lin * gain)
mats = {}
def flat(v, n=4): return np.tile(np.asarray(v, float), (n, n, 1))
# relief baked by mpfb_bake.py: tangent normals (OpenGL +Y, rows top-down) and AO, padded like the skin
body_n, body_orm = flat([0.0, 0.0, 1.0]), flat([1.0, 0.55, 0.0])
if (run / 'normal.npy').exists():
    nrm = np.load(run / 'normal.npy').astype(np.float64) * 2 - 1
    nrm, _ = pad_islands(nrm, run / 'body.npz')
    nrm[..., 1] *= -1                       # OpenGL +Y up -> the engine's bitangent follows texture V down
    nrm[..., 2] = np.maximum(nrm[..., 2], 0.05); nrm /= np.linalg.norm(nrm, axis=2, keepdims=True)
    ao = np.load(run / 'ao.npy').astype(np.float64)[..., 0]
    ao = 1 - AO_STRENGTH * (1 - ao)         # a soft AO: the engine has its own ambient occlusion on top
    ao, _ = pad_islands(np.repeat(ao[..., None], 3, 2), run / 'body.npz'); ao = ao[..., 0]
    body_n = nrm; body_orm = np.stack([ao, np.full_like(ao, 0.55), np.zeros_like(ao)], 2)
maps = [('own-hero-body-mpfb', skin, body_n, body_orm)]
if OUTFIT:                                  # flat colours until the pieces get their texture pass
    maps += [(f'own-hero-{p["name"]}', flat(to_srgb(np.array(p['colour_linear']))), flat([0.0, 0.0, 1.0]),
              flat([1.0, 0.85, 0.0])) for p in OUTFIT]
else:
    maps += [('own-hero-braies', flat(to_srgb(to_lin(np.array([0x7d, 0x70, 0x5e]) / 255))), flat([0.0, 0.0, 1.0]), flat([1.0, 0.9, 0.0]))]
for name, alb, nmap, orm in maps:
    d = root / 'materials' / name; d.mkdir(parents=True, exist_ok=True)
    mod.write_dds(d / 'albedo.dds', np.clip(alb, 0, 1))
    mod.write_dds(d / 'normal.dds', nmap, normal=True)
    mod.write_dds(d / 'orm.dds', np.clip(orm, 0, 1))
mats['OWN_HERO_BODY_MPFB.TGA'] = 'own-hero-body-mpfb'
for p in OUTFIT: mats[f'OWN_HERO_{p["name"].upper()}.TGA'] = f'own-hero-{p["name"]}'
if not OUTFIT: mats['OWN_HERO_BRAIES.TGA'] = 'own-hero-braies'
(root / 'materials' / 'materials.json').write_text(json.dumps({t: {
    'albedo': f'{d}/albedo.dds', 'normal': f'{d}/normal.dds', 'orm': f'{d}/orm.dds',
    'scale': 1.0, 'antitile': 'off', 'antitile_cell': 1.0} for t, d in mats.items()}, indent=2) + '\n')
print(json.dumps({'parts': report, 'bin_bytes': len(bin_), 'skin_gain_linear': gain.round(3).tolist(), 'uv_coverage': round(coverage, 3)}, indent=1))
