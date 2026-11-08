"""MPFB hero body, step 2: warp the man onto HUMANS.MDH, cut the head, make linen braies, export the parts.

    BLENDER_USER_RESOURCES=~/.local/share/gothic-ai/mpfb/blender \
      Blender -b <run>/body.blend -P mpfb_export.py -- <run> <HUMANS.MDH.json>

Reads <run>/landmarks.json from mpfb_body.py. Writes <run>/body.npz and <run>/braies.npz in the format of
build.py's lo.npz (co in the W frame of fit.py, cm; loop normals; weights per Gothic node), and <run>/export.json.

- The skin is the MPFB 'body' group; helpers, eyes and joint cubes are dropped.
- The head is cut along the neck ring of the original HUM_BODY_NAKED0: the Gothic head covers it from above.
- The braies are the band of MPFB's 'helper-tights' from the waist to mid-thigh, pushed off the skin; the skin
  under them is removed (MPFB's delete_verts idea), with a margin at the edges.
- Weights: MPFB's game_engine rig mapped onto the 34 Gothic nodes; the fingers but the thumb go to FINGER0.
"""
import bpy, os, sys, json
import numpy as np

argv = sys.argv[sys.argv.index('--') + 1:]
RUN, MDH = argv[0], argv[1]
sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
import fit

meta = json.load(open(os.path.join(RUN, 'landmarks.json')))
fit.S = meta['scale']
LW = {k: np.array(v) * fit.S for k, v in meta['landmarks'].items()}
skel = fit.load_skeleton(MDH)
names = skel[0]
posed = fit.posed_skeleton(skel, LW)
tz = fit.tz_of(LW)
src, dst = fit.control_points(skel, posed, LW)
tps = fit.TPS(src, dst)

BRAIES = (float(os.environ.get('BRAIES_LO', -22.0)), float(os.environ.get('BRAIES_HI', 12.0)))  # cm from the hip joint
OFFSET = float(os.environ.get('BRAIES_OFFSET', 0.45))   # cm off the skin
MARGIN = 2.0                                            # cm of skin kept under the braies' edges

bm = [o for o in bpy.data.objects if o.type == 'MESH' and 'body' in o.vertex_groups][0]
rig = [o for o in bpy.data.objects if o.type == 'ARMATURE'][0]
me = bm.data
kb = me.shape_keys.key_blocks
nv = len(me.vertices)
basis = np.empty(nv * 3); kb[0].data.foreach_get('co', basis); basis = basis.reshape(-1, 3)
co = basis.copy(); buf = np.empty(nv * 3)
for k in kb[1:]:
    if k.value != 0.0 and not k.mute:
        k.data.foreach_get('co', buf); co += k.value * (buf.reshape(-1, 3) - basis)
T = np.c_[co[:, 0], co[:, 2], -co[:, 1]] * 100           # T frame, cm
T[:, 1] -= meta.get('ground_cm', 0.0)                    # feet on the floor, as in the landmarks
Wc = tps(T * fit.S)                                       # W frame, warped onto the posed skeleton
model_y = Wc[:, 1]                                        # W.y is the height above the floor
Gz = Wc[:, 2] + tz                                        # Gothic z (forward)

def group(name, thr=0.5):
    gi = bm.vertex_groups[name].index
    out = np.zeros(nv, bool)
    for v in me.vertices:
        for g in v.groups:
            if g.group == gi and g.weight > thr: out[v.index] = True
    return out

body_v = group('body'); tights_v = group('helper-tights')

# --- weights: MPFB bones -> Gothic nodes
def gothic_node(b):
    if b in ('Root', 'pelvis'): return 'BIP01 PELVIS'
    m = {'spine_01': 'BIP01 SPINE', 'spine_02': 'BIP01 SPINE1', 'spine_03': 'BIP01 SPINE2',
         'neck_01': 'BIP01 NECK', 'head': 'BIP01 HEAD'}
    if b in m: return m[b]
    side = 'L' if b.endswith('_l') else 'R'
    base = b[:-2]
    part = {'clavicle': 'CLAVICLE', 'upperarm': 'UPPERARM', 'lowerarm': 'FOREARM', 'hand': 'HAND',
            'thigh': 'THIGH', 'calf': 'CALF', 'foot': 'FOOT', 'ball': 'TOE0'}
    if base in part: return f'BIP01 {side} {part[base]}'
    if base.startswith('thumb'): return f'BIP01 {side} HAND'
    if base.split('_')[0] in ('index', 'middle', 'ring', 'pinky'): return f'BIP01 {side} FINGER0'
    raise KeyError(b)

bones = list(rig.data.bones.keys())
groups = sorted({gothic_node(b) for b in bones}, key=names.index)
gidx = {g: i for i, g in enumerate(groups)}
vg_to_g = {bm.vertex_groups[b].index: gidx[gothic_node(b)] for b in bones if b in bm.vertex_groups}
Wt = np.zeros((nv, len(groups)), np.float32)
for v in me.vertices:
    for g in v.groups:
        if g.group in vg_to_g: Wt[v.index, vg_to_g[g.group]] += g.weight
s = Wt.sum(1)

# --- neck cut along the original body's top ring: y = 167.2 - 0.47 (z + 1), model space
neck_cut = 167.2 - 0.47 * (Gz + 1.0)
hip_y = float(posed[names.index('BIP01 L THIGH')][1, 3] + fit.FLOOR)   # model y of the hip joints after the warp
lo, hi = hip_y + BRAIES[0], hip_y + BRAIES[1]
arm_nodes = [gidx[n] for n in groups if any(p in n for p in ('UPPERARM', 'FOREARM', 'HAND', 'FINGER'))]
armish = Wt[:, arm_nodes].sum(1) > 0.05

polys = [(p.index, list(p.vertices), list(p.loop_indices)) for p in me.polygons]
def faces_where(pred):
    return [p for p in polys if pred(np.array(p[1]))]
braies_f = faces_where(lambda vs: tights_v[vs].all() and lo - 1.5 <= model_y[vs].mean() <= hi + 1.5 and not armish[vs].any())
covered = (model_y > lo + MARGIN) & (model_y < hi - MARGIN) & ~armish
body_f = faces_where(lambda vs: body_v[vs].all() and (model_y[vs] < neck_cut[vs]).all() and not covered[vs].all())

uv_all = np.empty(len(me.loops) * 2); me.uv_layers['UVMap'].data.foreach_get('uv', uv_all); uv_all = uv_all.reshape(-1, 2)

def smooth_normals(P, tris):
    a, b, c = P[tris[:, 0]], P[tris[:, 1]], P[tris[:, 2]]
    fn = np.cross(b - a, c - a)
    n = np.zeros_like(P)
    for i in range(3): np.add.at(n, tris[:, i], fn)
    return n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-12)

def edge_verts(faces):
    """Vertices on the open border of a face set."""
    count = {}
    for _, vs, _ in faces:
        for a, b in zip(vs, vs[1:] + vs[:1]):
            e = (min(a, b), max(a, b)); count[e] = count.get(e, 0) + 1
    return {v for e, c in count.items() if c == 1 for v in e}

def part(faces, push=0.0, name='', snap=None):
    used = sorted({v for _, vs, _ in faces for v in vs})
    remap = {v: i for i, v in enumerate(used)}
    tri_v, tri_l, loops = [], [], []
    for _, vs, ls in faces:                      # fan triangulation, quads and n-gons
        for k in range(1, len(vs) - 1):
            idx = [0, k, k + 1]
            tri_v.append([remap[vs[i]] for i in idx])
            tri_l.append([len(loops) + i for i in idx])
        loops.extend(ls)
    tri_v = np.array(tri_v); tri_l = np.array(tri_l)
    P = Wc[used].copy()
    if snap:                                     # a straight hem: border vertices onto the band's limit
        lo_, hi_ = snap; mid = (lo_ + hi_) / 2
        for v in edge_verts(faces):
            i = remap[v]; P[i, 1] = lo_ if P[i, 1] < mid else hi_
    n = smooth_normals(P, tri_v)
    if push:
        P += n * push; n = smooth_normals(P, tri_v)
    w = Wt[used]
    w = w / np.maximum(w.sum(1, keepdims=True), 1e-9)
    uv = uv_all[loops]
    loop_vertex = np.zeros(len(loops), np.int64); loop_vertex[tri_l.ravel()] = tri_v.ravel()
    # the T frame is right-handed like glTF; write_mdm mirrors x itself
    np.savez(os.path.join(RUN, f'{name}.npz'), co=P, tri_v=tri_v, tri_l=tri_l, uv=uv,
             loop_normal=n[loop_vertex], weights=w, groups=np.array(groups))
    return {'verts': len(used), 'tris': len(tri_v), 'height': [float(P[:, 1].min()), float(P[:, 1].max())]}

info = {'body': part(body_f, 0.0, 'body'), 'braies': part(braies_f, OFFSET, 'braies', snap=(lo, hi)),
        'groups': groups, 'braies_band_model_y': [lo, hi], 'hip_y': hip_y, 'tz': tz,
        'tps_max_move_cm': float(np.linalg.norm(dst - src, axis=1).max()),
        'unweighted_body_verts': int(((s <= 0) & body_v).sum())}
json.dump(info, open(os.path.join(RUN, 'export.json'), 'w'), indent=1)
print('EXPORT', json.dumps(info))
