# The beard of the generator's model as its own part of the head: a shell over the skin of the lower face.
#   python beard_shell.py uv.npz groups.npz views.json params.json raw.npz <out dir> [texture size] [--place place.json] [--tone tone.json]
# The head is registered to the bald, clean-shaven model, so the beard of the full model stands above its skin
# (3..10 mm on the chin of hero 3a) and paint on the skin cannot give its outline. The shell is a copy of the skin's
# own triangles of the lower face, each vertex moved away from the middle of the jaw onto the outer surface of the
# full model:
# - the outer surface is the farthest point of the ray, 2 mm inside to 30 mm outside (a goatee hangs 2 cm under the
#   chin), that has a point of the model within 0.5 mm;
# - a vertex belongs to the beard when that point is hair-coloured (the same test as hair_cut.py); vertices that
#   look into the mouth or the nostrils (their normal's ray meets the head's own skin) take no part;
# - the texture is the model's colour at the shell, its alpha the share of hair-coloured points: lips and bare
#   cheeks stay open, the edge of the beard is cut by alpha; islands apart from the main piece are dropped.
# The shell keeps the skin's vertex ids (vid), so the export binds it to the skin and it follows every face shape.
# Writes beard.npz (v, f, uv per face corner, vid, disp = the way from every skin vertex to the beard over it, metres, 0 where none)
# and beard.png (RGBA).
import sys, os, json
import numpy as np
from PIL import Image
from scipy import ndimage
from scipy.spatial import cKDTree
import ref_model

OPT = ref_model.options(sys.argv)
UV, GR, VJ, PJ, RAW, OUT = sys.argv[1:7]; N = int(sys.argv[7]) if len(sys.argv) > 7 else 1024
d = np.load(UV); g = np.load(GR); V = json.load(open(VJ)); P = json.load(open(PJ)); raw = np.load(RAW)
co, no, tri, tuv = d['co'].astype(np.float64), d['no'].astype(np.float64), d['tri'], d['tuv']
eye = np.array(V['eye']); rv, rc = ref_model.load(raw, P, eye, OPT)
keep = (rv[:, 2] > eye[2]-0.17) & (rv[:, 2] < eye[2]) & (rv[:, 1] < eye[1]+0.12); rv, rc, r0 = rv[keep], rc[keep], ref_model.raw_colour(raw)[keep]
hairlike = (r0[:, 0]-r0[:, 2] < 50) & (r0[:, 1] > 0.70*r0[:, 0]) & (r0.max(1) < 150)
shirt = (rv[:, 2] < eye[2]-0.10) & (r0.max(1)-r0.min(1) < 30) & (r0.max(1) > 70)   # grey cloth passes the colour test; the dark underside of a beard is not cloth
hairlike &= ~shirt
tree = cKDTree(rv)

z = co[:, 2]-eye[2]; y = co[:, 1]-eye[1]
zone = g['body'] & (z < -0.022) & (z > -0.15) & (y < 0.10)
skin_tri = tri[g['body'][tri].all(1)]

# vertices that look at the head's own skin: the mouth, the nostrils, the folds of the lips
def blocked(vi, dirs, reach):
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

# The rays go out from the middle of the jaw, not along the normals: a generator that saw the head from the front
# and the side leaves the underside of a goatee open, and a normal that points down finds nothing there.
C = eye+np.array([0.0, 0.010, -0.085]); ray = co-C; ray /= np.linalg.norm(ray, axis=1, keepdims=True)
vi = np.nonzero(zone)[0]; vi = vi[~blocked(vi, no, 0.02)]; vi = vi[~blocked(vi, ray, 0.03)]   # inside the mouth: skin lies in front
ts = np.arange(-0.002, 0.030, 0.00025)
pts = co[vi][:, None]+ray[vi][:, None]*ts[None, :, None]
dd, ii = tree.query(pts.reshape(-1, 3), workers=-1); dd = dd.reshape(len(vi), len(ts)); ii = ii.reshape(len(vi), len(ts))
on = dd < 0.0005; onb = on & hairlike[ii]
def outer(m):
    return np.where(m.any(1), len(ts)-1-np.argmax(m[:, ::-1], axis=1), -1)
last, lastb = outer(on), outer(onb)
# beard: the outermost hair-coloured point of the ray, unless skin lies right over it, within 3 mm (a shadow in a
# fold). Skin farther out is another thing: under the chin the ray goes on to the neck.
W3 = 12; skin_on = on & ~hairlike[ii]; cover = np.zeros(len(vi), bool)
for k in range(1, W3+1):
    j = np.clip(lastb+k, 0, len(ts)-1); cover |= skin_on[np.arange(len(vi)), j] & (lastb+k < len(ts))
isb = (lastb >= 0) & ~cover; found = isb | ((last >= 0) & (ts[last] < 0.006))
off = np.full(len(co), np.nan); off[vi[found]] = np.where(isb, ts[lastb], ts[last])[found]
hw = np.zeros(len(co)); hw[vi] = isb

# smooth over the mesh: holes take their neighbours' offset, single strands do not stick out
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
offs, ok = smooth(off, np.isfinite(off), 3); hws, _ = smooth(hw, inz, 2)
offs = np.clip(np.where(ok, offs, 0), 0, 0.030)
beard_v = inz & ok & (hws > 0.3)

# the shell: skin triangles of the zone that touch the beard; the rim sits just over the skin and is cut by alpha
ft = np.nonzero(inz[tri].all(1) & beard_v[tri].any(1))[0]
ids = np.unique(tri[ft]); new = -np.ones(len(co), int); new[ids] = np.arange(len(ids))
lift = np.where(beard_v[ids], np.maximum(offs[ids], 0.0012), 0.0006)
v = co[ids]+ray[ids]*lift[:, None]; f = new[tri[ft]]
uv0 = tuv[ft]; lo = uv0.reshape(-1, 2).min(0); span = (uv0.reshape(-1, 2).max(0)-lo).max()
uv = (uv0-lo)/span*0.98+0.01

# the texture: the model's colour at the shell, alpha = the share of hair-coloured points there
pos = np.zeros((N, N, 3), np.float32); hit = np.zeros((N, N), bool); fid = -np.ones((N, N), int)
for k in range(len(f)):
    a = uv[k]*[N, N]; a[:, 1] = N-a[:, 1]
    x0, y0 = np.floor(a.min(0)).astype(int)-1; x1, y1 = np.ceil(a.max(0)).astype(int)+1
    x0, y0, x1, y1 = max(x0, 0), max(y0, 0), min(x1, N-1), min(y1, N-1)
    X, Y = np.meshgrid(np.arange(x0, x1+1)+0.5, np.arange(y0, y1+1)+0.5)
    T = np.array([[a[0, 0]-a[2, 0], a[1, 0]-a[2, 0]], [a[0, 1]-a[2, 1], a[1, 1]-a[2, 1]]])
    if abs(np.linalg.det(T)) < 1e-9: continue
    l = np.linalg.solve(T, np.stack([X.ravel()-a[2, 0], Y.ravel()-a[2, 1]])); w = np.vstack([l, 1-l.sum(0)]).T
    okk = (w > -0.02).all(1); yy, xx = (Y.ravel()[okk]).astype(int), (X.ravel()[okk]).astype(int)
    pos[yy, xx] = w[okk] @ v[f[k]]; hit[yy, xx] = True; fid[yy, xx] = k
dq, iq = tree.query(pos[hit], k=6, workers=-1)
w = 1/(dq+0.0002)**2; c = (rc[iq]*w[..., None]).sum(1)/w.sum(1)[:, None]
al = (hairlike[iq]*w).sum(1)/w.sum(1)*(dq[:, 0] < 0.008)       # the shell is coarse and cuts inside the hollow model
img = np.zeros((N, N, 4), np.float32); img[hit, :3] = c
alpha = np.zeros((N, N), np.float32); alpha[hit] = al
alpha = ndimage.gaussian_filter(alpha, 1.0)                                   # a soft edge, cut by the alpha test at one half
solid = alpha > 0.5
# the beard is one piece: islands (dark hollows under the jaw, single tufts of stubble) are dropped, and with them
# the triangles that carry nothing solid
lab, nl = ndimage.label(solid); area = ndimage.sum(solid, lab, range(1, nl+1))
solid = np.isin(lab, 1+np.nonzero(area >= 0.25*area.max())[0]); alpha = np.where(solid, alpha, np.minimum(alpha, 0.2))
kf = np.unique(fid[solid]); used = np.unique(f[kf]); renum = -np.ones(len(v), int); renum[used] = np.arange(len(used))
np.savez(os.path.join(OUT, 'beard.npz'), v=v[used], f=renum[f[kf]], uv=uv[kf], vid=ids[used], disp=ray*np.where(beard_v, offs, 0.0)[:, None])
ix = ndimage.distance_transform_edt(~solid, return_distances=False, return_indices=True)   # beard colour under the cut-off: no pale rim
img[..., :3] = img[ix[0], ix[1], :3]; img[..., 3] = alpha*255
Image.fromarray(img.clip(0, 255).astype(np.uint8), 'RGBA').save(os.path.join(OUT, 'beard.png'))
bo = offs[beard_v]
print('BEARD zone verts %d, on the model %d, beard %d; offset mm median %.1f, 90%% %.1f, max %.1f; shell verts %d tris %d; solid texels %.0f%%' % (
    len(vi), found.sum(), beard_v.sum(), 1000*np.median(bo), 1000*np.percentile(bo, 90), 1000*bo.max(), len(used), len(kf), 100*solid[hit].mean()))
