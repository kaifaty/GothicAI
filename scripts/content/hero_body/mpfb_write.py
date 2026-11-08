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
for npz, mat, tex, size in ((run / 'body.npz', 'OWN_HERO_BODY_MPFB', 'OWN_HERO_BODY_MPFB.TGA', 2048),
                            (run / 'braies.npz', 'OWN_HERO_BRAIES', 'OWN_HERO_BRAIES.TGA', 64)):
    p, r = write_part(npz, mat, tex, size); parts.append(p); report.append(r)
record = {'format': 'gothic-model', 'version': 1, 'kind': 'mesh', 'buffer': MESH + '.bin', 'byte_length': len(bin_),
          'attachment_order': [], 'data': {'meshes': parts, 'attachments': []}}
models = root / 'models'; models.mkdir(parents=True, exist_ok=True)
(models / (MESH + '.bin')).write_bytes(bytes(bin_))
(models / (MESH + '.json')).write_text(json.dumps(record))
(models / 'models.json').write_text(json.dumps({'format': 'gothic-models', 'version': 1, 'files': {MESH: MESH + '.json'}}, indent=1) + '\n')

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
for name, alb, nmap, orm in (('own-hero-body-mpfb', skin, body_n, body_orm),
                             ('own-hero-braies', flat(to_srgb(to_lin(np.array([0x7d, 0x70, 0x5e]) / 255))), flat([0.0, 0.0, 1.0]), flat([1.0, 0.9, 0.0]))):
    d = root / 'materials' / name; d.mkdir(parents=True, exist_ok=True)
    mod.write_dds(d / 'albedo.dds', np.clip(alb, 0, 1))
    mod.write_dds(d / 'normal.dds', nmap, normal=True)
    mod.write_dds(d / 'orm.dds', np.clip(orm, 0, 1))
mats['OWN_HERO_BODY_MPFB.TGA'] = 'own-hero-body-mpfb'; mats['OWN_HERO_BRAIES.TGA'] = 'own-hero-braies'
(root / 'materials' / 'materials.json').write_text(json.dumps({t: {
    'albedo': f'{d}/albedo.dds', 'normal': f'{d}/normal.dds', 'orm': f'{d}/orm.dds',
    'scale': 1.0, 'antitile': 'off', 'antitile_cell': 1.0} for t, d in mats.items()}, indent=2) + '\n')
print(json.dumps({'parts': report, 'bin_bytes': len(bin_), 'skin_gain_linear': gain.round(3).tolist(), 'uv_coverage': round(coverage, 3)}, indent=1))
