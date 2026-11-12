"""The beard of the full model as a layer over our head: how far the model stands over the skin of the lower face.

  python beard_layer.py uv.npz groups.npz views.json params.json raw.npz <out dir> [--place place.json] [--tone tone.json]

uv.npz and groups.npz are the head (dump_uv.py: co, no, tri; body), raw.npz the generator's full model, placed onto
the head (place_face.py). The head is fitted to the bald, shaven model of the same person, so over the lower face the
full model stands above the skin by the beard's thickness. No colour is asked (beard_shell.py told beard from skin by
the colours of hero 3a): where the model stands over the skin, the offset is there, and whether hair grows on that
skin is the skin map's matter (beard.py).
- Rays go out from the middle of the jaw through the skin's vertices, not along the normals: a generator that saw the
  head from the front and the side leaves the underside of a beard open, and a normal that points down finds nothing.
- The model is a shell, so along a ray it is met at the beard's outer surface, 2 mm inside the skin to 30 mm outside.
  The surface is the far end of the first piece of the model on the ray; a piece that begins 5 mm further out is
  another thing (under the chin the ray goes on to the neck and the collar).
- Vertices that look at the head's own skin (the mouth, the nostrils, the folds of the lips) take no part; holes
  take their neighbours' offset, a single strand does not stick out (the offsets are smoothed over the mesh).
Writes beard.npz: disp, the way from every skin vertex to the model's surface over it, metres (0 where the model is
not over the skin or the vertex is not of the lower face). bake_ref.py takes the colour there, beard.py the beard's
thickness. With the full model put on by the face the slack is a millimetre: the two models are two generations.
"""
import sys, os, json
import numpy as np
from scipy.spatial import cKDTree

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', '..', '..', 'reports', 'content', '2026-11-hero-head', 'skin'))
import ref_model

OPT = ref_model.options(sys.argv)
UV, GR, VJ, PJ, RAW, OUT = sys.argv[1:7]
d = np.load(UV); g = np.load(GR); V = json.load(open(VJ)); P = json.load(open(PJ))
co, no, tri = d['co'].astype(np.float64), d['no'].astype(np.float64), d['tri']
eye = np.array(V['eye']); rv, _ = ref_model.load(np.load(RAW), P, eye, OPT)
rv = rv[(rv[:, 2] > eye[2]-0.19) & (rv[:, 2] < eye[2]) & (rv[:, 1] < eye[1]+0.13)]; tree = cKDTree(rv)
body = g['body'].astype(bool); z = co[:, 2]-eye[2]; y = co[:, 1]-eye[1]
zone = body & (z < -0.022) & (z > -0.15) & (y < 0.10)
skin_tri = tri[body[tri].all(1)]
GAP, NEAR = 0.005, 0.0005


def blocked(vi, dirs, reach):
    """Vertices whose ray meets the head's own skin within `reach`."""
    t0 = co[skin_tri[:, 0]]; e1 = co[skin_tri[:, 1]]-t0; e2 = co[skin_tri[:, 2]]-t0
    near = (np.abs(co[skin_tri].mean(1)-co[vi].mean(0)) < 0.12).all(1); t0, e1, e2, st = t0[near], e1[near], e2[near], skin_tri[near]
    out = np.zeros(len(vi), bool)
    for s in range(0, len(vi), 64):
        i = vi[s:s+64]; dr = dirs[i]; o = co[i]+dr*0.0004
        pv = np.cross(dr[:, None], e2[None]); det = (e1[None]*pv).sum(2); det = np.where(np.abs(det) < 1e-12, 1e-12, det)
        tv = o[:, None]-t0[None]; u = (tv*pv).sum(2)/det
        qv = np.cross(tv, e1[None]); w = (dr[:, None]*qv).sum(2)/det; t = (e2[None]*qv).sum(2)/det
        own = (st[None] == i[:, None, None]).any(2)
        out[s:s+64] = ((u > 0) & (w > 0) & (u+w < 1) & (t > 1e-5) & (t < reach) & ~own).any(1)
    return out


C = eye+np.array([0.0, 0.010, -0.085]); ray = co-C; ray /= np.linalg.norm(ray, axis=1, keepdims=True)
vi = np.nonzero(zone)[0]; vi = vi[~blocked(vi, no, 0.02)]; vi = vi[~blocked(vi, ray, 0.03)]
ts = np.arange(-0.002, 0.030, 0.00025); pts = co[vi][:, None]+ray[vi][:, None]*ts[None, :, None]
on = (tree.query(pts.reshape(-1, 3), workers=-1)[0] < NEAR).reshape(len(vi), len(ts))
# the far end of the first piece: the last sample met before GAP of nothing
gap = int(GAP/0.00025); first = np.where(on.any(1), on.argmax(1), -1); last = first.copy(); idle = np.zeros(len(vi), int); live = first >= 0
for k in range(len(ts)):
    after = live & (k > first); hit = after & on[:, k]; last[hit] = k; idle[hit] = 0
    idle[after & ~on[:, k]] += 1; live &= idle < gap
found = first >= 0; off = np.full(len(co), np.nan); off[vi[found]] = ts[last[found]]

edges = np.unique(np.sort(np.concatenate([skin_tri[:, [0, 1]], skin_tri[:, [1, 2]], skin_tri[:, [2, 0]]]), axis=1), axis=0)
inz = np.zeros(len(co), bool); inz[vi] = True; edges = edges[inz[edges].all(1)]


def smooth(val, known, it):
    val = np.where(known, val, 0.0); wgt = known.astype(float)
    for _ in range(it):
        s = val*wgt; c = wgt.copy()
        np.add.at(s, edges[:, 0], (val*wgt)[edges[:, 1]]); np.add.at(s, edges[:, 1], (val*wgt)[edges[:, 0]])
        np.add.at(c, edges[:, 0], wgt[edges[:, 1]]); np.add.at(c, edges[:, 1], wgt[edges[:, 0]])
        val = np.where(c > 0, s/np.maximum(c, 1e-9), val); wgt = np.where(inz, np.minimum(c, 1), 0)
    return val, wgt > 0


offs, ok = smooth(off, np.isfinite(off), 3); offs = np.clip(np.where(ok & inz, offs, 0), 0, 0.030)
os.makedirs(OUT, exist_ok=True)
np.savez(os.path.join(OUT, 'beard.npz'), disp=ray*offs[:, None])
o = offs[inz]*1000; q = co-eye
part = lambda m: 'median %.1f, 90%% %.1f' % (np.median(offs[inz & m])*1000, np.percentile(offs[inz & m], 90)*1000) if (inz & m).any() else '-'
print('BEARD LAYER zone verts %d, the model met over %d; the model over the skin, mm: median %.1f, 90%% %.1f, max %.1f; over 1.5 mm: %d verts' % (
    len(vi), found.sum(), np.median(o), np.percentile(o, 90), o.max(), (o > 1.5).sum()))
print('BEARD LAYER by part, mm: upper lip %s; chin front %s; under the chin %s; cheeks and jaw %s' % (
    part((np.abs(q[:, 0]) < 0.025) & (q[:, 2] > -0.068) & (q[:, 2] < -0.045) & (q[:, 1] < 0)),
    part((np.abs(q[:, 0]) < 0.025) & (q[:, 2] > -0.115) & (q[:, 2] < -0.080) & (q[:, 1] < 0.005)),
    part((np.abs(q[:, 0]) < 0.03) & (q[:, 2] < -0.105) & (q[:, 1] > 0.0) & (q[:, 1] < 0.05)),
    part((np.abs(q[:, 0]) > 0.04) & (q[:, 2] < -0.05) & (q[:, 2] > -0.11) & (q[:, 1] < 0.06))))
