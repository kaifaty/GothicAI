"""Write the fitted, weighted Tripo hero as ARMOR_PRISONER.MDM + DDS maps into a test content root.

uv run --with numpy python -I write_mdm.py <run dir> <HUMANS.MDH.json> <landmarks.json> <ARMOR_PRISONER.MDM.json> <content root> <repo>
"""
import sys, os, json, struct, importlib.util
from pathlib import Path
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fit import load_skeleton, landmarks_work, posed_skeleton, tz_of, w2g, FLOOR

run, mdh, lmk, orig, root, repo = map(Path, sys.argv[1:7])
MESH = 'ARMOR_PRISONER.MDM'
TEXNAME = 'OWN_HERO_BODY_TRIPO.TGA'
MATDIR = 'own-hero-body-tripo'

skel = load_skeleton(mdh)
names, parent, local, rest = skel
LW = landmarks_work(lmk)
posed = posed_skeleton(skel, LW)
tz = tz_of(LW)

d = np.load(run / 'lo.npz')
co_w, tri_l, uv, ln, Wt, groups = d['co'], d['tri_l'], d['uv'], d['loop_normal'], d['weights'], list(d['groups'])
tri_v = d['tri_v']
nv = len(co_w)
assert nv <= 65535, nv
P = w2g(co_w, tz)                              # Gothic cm, model space of the bind pose
node_of = np.array([names.index(g) for g in groups])

# up to 4 influences per vertex
order = np.argsort(-Wt, axis=1)[:, :4]
w4 = np.take_along_axis(Wt, order, 1)
w4[w4 < 0.01] = 0
s = w4.sum(1, keepdims=True); assert (s > 0).all()
w4 /= s
n4 = node_of[order]

inv_bind = {n: np.linalg.inv(posed[n]) for n in set(node_of.tolist())}
Ph = np.c_[P, np.ones(nv)]
weights, rest_pos = [], np.zeros((nv, 3))
R_fix = {n: rest[n][:3, :3] @ posed[n][:3, :3].T for n in inv_bind}   # bind -> rest rotation per bone
for v in range(nv):
    ws = []
    for k in range(4):
        if w4[v, k] <= 0: continue
        n = int(n4[v, k]); loc = (inv_bind[n] @ Ph[v])[:3]
        ws.append((float(w4[v, k]), loc, n))
        rest_pos[v] += w4[v, k] * (rest[n] @ np.r_[loc, 1])[:3]
    weights.append(ws)

# loop normals -> Gothic frame -> rest pose (the engine does not rotate skin normals by bones)
nl = np.c_[-ln[:, 0], ln[:, 1], ln[:, 2]]
loop_vertex = np.zeros(len(uv), np.int64)
loop_vertex[tri_l.ravel()] = tri_v.ravel()
nr = np.zeros_like(nl)
for k in range(4):
    wk = w4[loop_vertex, k]; nk = n4[loop_vertex, k]
    for n in np.unique(nk):
        m = (nk == n) & (wk > 0)
        nr[m] += wk[m, None] * (nl[m] @ R_fix[int(n)].T)
nr /= np.linalg.norm(nr, axis=1, keepdims=True)
uv_g = np.c_[uv[:, 0], 1 - uv[:, 1]]

# wedges: unique (vertex, uv, normal)
key = np.c_[loop_vertex, np.round(uv_g * 1e5), np.round(nr * 1e4)]
_, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
inverse = inverse.ravel()
assert len(first) <= 65535, len(first)
wedge_n, wedge_uv, wedge_i = nr[first], uv_g[first], loop_vertex[first]
tris = inverse[tri_l]
# Gothic winding: cross(b-a, c-a) points against the outward normal
a, b, c = (rest_pos[wedge_i[tris[:, i]]] for i in range(3))
fn = np.cross(b - a, c - a); wn = wedge_n[tris].mean(1)
pos_frac = float(((fn * wn).sum(1) > 0).mean())
if pos_frac > 0.5:
    tris = tris[:, [0, 2, 1]]
print('winding: fraction along the normal before fix', round(pos_frac, 4))

vnorm = np.zeros((nv, 3)); np.add.at(vnorm, wedge_i, wedge_n)
vnorm /= np.maximum(np.linalg.norm(vnorm, axis=1, keepdims=True), 1e-9)
model_pos = rest_pos + np.array([0, FLOOR, 0])

# binary
bin_ = bytearray()
def block(arr, fmt_count):
    off = len(bin_); bin_.extend(arr); return {'offset': off, 'count': fmt_count}
positions = block(model_pos.astype('<f4').tobytes(), nv)
normals = block(vnorm.astype('<f4').tobytes(), nv)
triangles = block(tris.astype('<u2').tobytes(), len(tris))
wdt = np.dtype([('n', '<f4', 3), ('uv', '<f4', 2), ('i', '<u2')])
wa = np.zeros(len(first), wdt); wa['n'] = wedge_n; wa['uv'] = wedge_uv; wa['i'] = wedge_i
wedges = block(wa.tobytes(), len(first))
woff = len(bin_)
for ws in weights:
    bin_ += struct.pack('<I', len(ws))
    for wt, p, n in ws:
        bin_ += struct.pack('<4f', wt, *p) + bytes([n])
wref = {'offset': woff, 'count': nv, 'byte_length': len(bin_) - woff}

def obb(points):
    lo, hi = points.min(0), points.max(0)
    return {'center': ((lo + hi) / 2).tolist(), 'axes': [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]],
            'half_width': ((hi - lo) / 2 + 1e-4).tolist(), 'children': []}
used = sorted({n for ws in weights for _, _, n in ws})
bboxes = [obb(np.array([p for ws in weights for _, p, n in ws if n == u])) for u in used]

src = json.load(open(orig))
mat = dict(src['data']['meshes'][1]['mesh']['materials'][0])
mat.update(name='OWN_HERO_BODY_TRIPO', texture=TEXNAME, color=[128, 128, 128, 255], texture_scale=[2048.0, 2048.0])
mesh = {'positions': positions, 'normals': normals,
        'sub_meshes': [{'mat': mat, 'triangles': triangles, 'wedges': wedges}],
        'materials': [mat], 'alpha_test': 0,
        'bbox': [model_pos.min(0).tolist(), model_pos.max(0).tolist()], 'obbox': obb(model_pos)}
record = {'format': 'gothic-model', 'version': 1, 'kind': 'mesh', 'buffer': MESH + '.bin', 'byte_length': len(bin_),
          'attachment_order': [], 'data': {'meshes': [{'mesh': mesh, 'bboxes': bboxes, 'weights': wref}], 'attachments': []}}
models = root / 'models'; models.mkdir(parents=True, exist_ok=True)
(models / (MESH + '.bin')).write_bytes(bytes(bin_))
(models / (MESH + '.json')).write_text(json.dumps(record))
(models / 'models.json').write_text(json.dumps({'format': 'gothic-models', 'version': 1, 'files': {MESH: MESH + '.json'}}, indent=1) + '\n')

# maps -> DDS through the project's packer
spec = importlib.util.spec_from_file_location('material_fixture', repo / 'scripts/content/test-material.py')
sys.path.insert(0, str(repo / 'scripts/content'))
mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
mdir = root / 'materials' / MATDIR; mdir.mkdir(parents=True, exist_ok=True)
L = lambda k: np.load(run / f'{k}.npy').astype(np.float32)
alb = L('albedo')[..., :3]
nrm = L('normal')[..., :3] * 2 - 1
nrm[..., 1] *= -1                       # OpenGL +Y up -> engine bitangent follows texture V down
nrm[..., 2] = np.maximum(nrm[..., 2], 0.05)
nrm /= np.linalg.norm(nrm, axis=2, keepdims=True)
orm = np.stack([L('ao')[..., 0], L('roughness')[..., 0], L('metallic')[..., 0]], 2)
mod.write_dds(mdir / 'albedo.dds', np.clip(alb, 0, 1))
mod.write_dds(mdir / 'normal.dds', nrm, normal=True)
mod.write_dds(mdir / 'orm.dds', np.clip(orm, 0, 1))
(root / 'materials' / 'materials.json').write_text(json.dumps({TEXNAME: {
    'albedo': f'{MATDIR}/albedo.dds', 'normal': f'{MATDIR}/normal.dds', 'orm': f'{MATDIR}/orm.dds',
    'scale': 1.0, 'antitile': 'off', 'antitile_cell': 1.0}}, indent=2) + '\n')

# the model has its own head: replace the hero's head with one hidden 1 mm triangle
def stub_glb(path):
    pos = np.array([[0, 0, 0], [0.001, 0, 0], [0, 0.001, 0]], '<f4')
    nor = np.array([[0, 0, 1]] * 3, '<f4'); tex = np.zeros((3, 2), '<f4'); idx = np.array([0, 1, 2], '<u2')
    blob = pos.tobytes() + nor.tobytes() + tex.tobytes() + idx.tobytes() + b'\0\0'
    gl = {'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [{'mesh': 0, 'name': 'face'}],
          'meshes': [{'primitives': [{'attributes': {'POSITION': 0, 'NORMAL': 1, 'TEXCOORD_0': 2}, 'indices': 3, 'material': 0}]}],
          'materials': [{'name': 'OWN_HERO_BODY_TRIPO'}],
          'buffers': [{'byteLength': len(blob)}],
          'bufferViews': [{'buffer': 0, 'byteOffset': 0, 'byteLength': 36}, {'buffer': 0, 'byteOffset': 36, 'byteLength': 36},
                          {'buffer': 0, 'byteOffset': 72, 'byteLength': 24}, {'buffer': 0, 'byteOffset': 96, 'byteLength': 6}],
          'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': 3, 'type': 'VEC3', 'min': [0, 0, 0], 'max': [0.001, 0.001, 0]},
                        {'bufferView': 1, 'componentType': 5126, 'count': 3, 'type': 'VEC3'},
                        {'bufferView': 2, 'componentType': 5126, 'count': 3, 'type': 'VEC2'},
                        {'bufferView': 3, 'componentType': 5123, 'count': 3, 'type': 'SCALAR'}]}
    js = json.dumps(gl).encode(); js += b' ' * (-len(js) % 4)
    data = struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(blob))
    data += struct.pack('<II', len(js), 0x4E4F534A) + js + struct.pack('<II', len(blob), 0x004E4942) + blob
    path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)
stub_glb(root / 'characters' / 'hero' / 'no-head.glb')
(root / 'characters' / 'characters.json').write_text(json.dumps({'PC_HERO': {'head': 'hero/no-head.glb'}}, indent=1) + '\n')

print(json.dumps({'positions': nv, 'wedges': len(first), 'triangles': len(tris), 'bin_bytes': len(bin_),
                  'max_influences': max(len(w) for w in weights), 'nodes_used': [names[u] for u in used],
                  'height_cm': float(model_pos[:, 1].max() - model_pos[:, 1].min()),
                  'rest_bbox': [model_pos.min(0).round(1).tolist(), model_pos.max(0).round(1).tolist()]}, indent=1))
