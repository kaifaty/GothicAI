"""The relief of the bald model on the wrapped head: folds into the vertices, the rest kept for the normal map.

  python head_relief.py <reg dir> <out dir> [--ears ears.npz]

<reg dir>: mpfb.npz, groups.npz, views.json, ref.npz (the bald model, placed by place_ref.py), reg.npz of
reg_solve.py. Writes <out dir>/reg.npz (the same arrays with `co` moved), samples.npz (what the vertices could not
take: places on the head's faces and the height of the model over them, for relief_map.py), relief.json and
relief.jpg (the model over the head in the front view, before and after: red outside, blue inside, 2 mm full).

The wrap (reg_solve.py) asks every vertex of the head to lie on the model. It looks from the head to the nearest
point of the model, and a fold narrower than the faces round it is not seen that way: over a fold the nearest point
of the model is its bank, so the head bridges the fold. On the second face the model's nasolabial folds, the frown
lines and the folds at the corners of the mouth lay 2-3 mm under the head, the brow ridge 1-2 mm over it, and the
wrapped face was that of a younger and calmer man.
Here the model looks at the head: every point of the model finds its place on the head's faces and its height over
it along the face's normal. Each vertex moves along its normal so that the faces take the heights in the mean: a
fold between two rows of vertices becomes a real fold, and the flat faces of the coarse skull stop cutting under
the model. What is finer than the faces is written out for the normal map.
Not moved: the lids and what lies within reach of the eyes (the model's eyes are half shut, the lids follow the
concept), the brows less than the rest (hair on the model) - both by `w` of reg_solve.py; the ears and the skin
within 12 mm of them (--ears: the back of the model's ear is not the skull); the landmarks; the mouth bag and the
sockets; the skin within 15 mm of the model's cloth and the neck lower than 14 cm under the eyes.
The concept's outline comes after this pass (head_outline.py): it moves the head smoothly, folds and all.
"""
import json, os, sys
import numpy as np
from PIL import Image
from scipy import ndimage, sparse
from scipy.sparse.linalg import splu
from scipy.spatial import cKDTree

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', '..', 'reports', 'content', '2026-11-hero-head', 'skin'))
import ref_model

EARS = ref_model.take(sys.argv, '--ears')
D, OUT = sys.argv[1:3]; J = lambda n: os.path.join(D, n)
m = np.load(J('mpfb.npz')); g = np.load(J('groups.npz')); R = np.load(J('reg.npz')); ref = np.load(J('ref.npz'))
eye = np.array(json.load(open(J('views.json')))['eye'])
co0 = R['co'].astype(np.float64); tris = m['tris']; body = g['body'].astype(bool)
free, follow, wgt = R['free'], R['follow'], R['w']; ref_ok = R['ref_ok']
ear = np.load(EARS)['ear'] if EARS else np.zeros(len(co0), bool)
rv = ref['v'].astype(np.float64)
LOW = -0.140                 # metres over the eyes: nothing moves below (head_outline.py's line)
REACH = 0.006                # a point of the model further from the head than this is another thing (cloth, an ear)
LIM = 0.004                  # no vertex moves further than this
MU = 0.35                    # smoothing of the vertices' offsets: one vertex has about one unit of data
PIN = 3.0                    # how firmly a vertex the wrap did not trust to the model (`w` = 0) stays
K = 8                        # points per edge of a face that the model's points are matched to

ft = tris[(free[tris] & ~ear[tris]).all(1)]
fi = np.unique(ft); nf = len(fi); pos = -np.ones(len(co0), np.int64); pos[fi] = np.arange(nf)
fe = np.unique(np.sort(np.concatenate([ft[:, [0, 1]], ft[:, [1, 2]], ft[:, [2, 0]]]), axis=1), axis=0)
grid = np.array([(a, b, K-a-b) for a in range(K+1) for b in range(K+1-a)], float)/K
near_ear = cKDTree(co0[ear]).query(co0[fi])[0] < 0.012 if ear.any() else np.zeros(nf, bool)
cloth = rv[~ref_ok]
near_cloth = cKDTree(cloth).query(co0[fi], workers=-1)[0] < 0.015 if len(cloth) else np.zeros(nf, bool)
low = np.clip((eye[2]+LOW+0.02-co0[fi][:, 2])/0.02, 0, 1)                    # the neck down to the collar is the wrap's
hold = np.maximum.reduce([1-wgt[fi], near_ear, near_cloth, low]); hold[pos[R['src_l']][pos[R['src_l']] >= 0]] = 1
# the neighbours of a held vertex are half held: a rim that stays next to skin that moves 3 mm is a row of ripples
nb = sparse.csr_matrix((np.ones(2*len(fe)), (np.r_[pos[fe[:, 0]], pos[fe[:, 1]]], np.r_[pos[fe[:, 1]], pos[fe[:, 0]]])), shape=(nf, nf))
hold = np.maximum(hold, 0.5*np.asarray(nb.multiply(hold[None, :]).max(1).todense()).ravel())
# the model's points that may speak: skin, near the head, not the ears
near = ref_ok.copy()
near[near] = cKDTree(co0[fi]).query(rv[near], workers=-1)[0] < 0.02
if ear.any(): near[near] = cKDTree(co0[ear]).query(rv[near], workers=-1)[0] > 0.012
q = rv[near]


def vnormals(p):
    fn = np.cross(p[tris[:, 1]]-p[tris[:, 0]], p[tris[:, 2]]-p[tris[:, 0]]); vn = np.zeros_like(p)
    for k in range(3): np.add.at(vn, tris[:, k], fn)
    return vn/(np.linalg.norm(vn, axis=1, keepdims=True)+1e-12)


def heights(p):
    """The model over the head: (face, weights of its corners, height in mm, trust, face normals, foot on the face)."""
    a, b, c = p[ft[:, 0]], p[ft[:, 1]], p[ft[:, 2]]; fn = np.cross(b-a, c-a); fn /= np.linalg.norm(fn, axis=1, keepdims=True)+1e-15
    pts = (grid[None, :, 0, None]*a[:, None]+grid[None, :, 1, None]*b[:, None]+grid[None, :, 2, None]*c[:, None]).reshape(-1, 3)
    d, k = cKDTree(pts).query(q, workers=-1); t = k//len(grid)
    off = q-pts[k]; h = (off*fn[t]).sum(1); foot = q-h[:, None]*fn[t]
    # the foot's own weights in its face: the grid's point is only the nearest of 45
    e1, e2, ef = b[t]-a[t], c[t]-a[t], foot-a[t]
    d11, d12, d22 = (e1*e1).sum(1), (e1*e2).sum(1), (e2*e2).sum(1); f1, f2 = (ef*e1).sum(1), (ef*e2).sum(1)
    det = d11*d22-d12*d12+1e-30; v = (d22*f1-d12*f2)/det; u = (d11*f2-d12*f1)/det; bw = np.c_[1-v-u, v, u]
    ok = (d < REACH) & (bw.min(1) > -0.15)                    # beside the head, not over it: the rim of the faces that speak
    bw = np.clip(bw[ok], 0, 1); bw /= bw.sum(1, keepdims=True); t = t[ok]
    return t, bw, h[ok]*1000, (wgt[ft[t]]*bw).sum(1), fn, foot[ok]


def fit(t, b, h, w, fn, N):
    """Offsets of the vertices along their normals, mm, whose faces take the heights in the mean."""
    n = len(h); rows = np.repeat(np.arange(n), 3); cols = pos[ft[t]].ravel()
    vals = (b*np.einsum('tk,tvk->tv', fn[t], N[ft[t]])).ravel()
    sw = np.sqrt(w/max(w.sum()/nf, 1e-9))
    A = sparse.csr_matrix((vals*np.repeat(sw, 3), (rows, cols)), shape=(n, nf))
    ea, eb = pos[fe[:, 0]], pos[fe[:, 1]]; ne = len(fe)
    S = sparse.csr_matrix((np.r_[np.full(ne, MU), np.full(ne, -MU)], (np.r_[np.arange(ne), np.arange(ne)], np.r_[ea, eb])), shape=(ne, nf))
    M = sparse.vstack([A, S, sparse.diags(0.03+PIN*hold)]).tocsc()
    return splu((M.T@M).tocsc()).solve(M.T@np.r_[h*sw, np.zeros(ne+nf)])


def bands(foot, h, w):
    z = (foot[:, 2]-eye[2])*1000; x = np.abs(foot[:, 0]-eye[0])*1000; f = foot[:, 1] < eye[1]+0.06; out = {}
    for name, s in (('skull', (z > 30) & ~f), ('forehead', (z > 25) & f), ('brows', (z > 8) & (z <= 25) & f), ('nose and cheeks', (z > -50) & (z <= -15) & f),
                    ('nasolabial', (z > -70) & (z <= -35) & (x > 15) & (x < 40) & f), ('mouth and chin', (z <= -50) & (z > -110) & f), ('all', z > -110)):
        s = s & (w > 0.5)
        if s.sum() > 100: out[name] = {'mean_mm': round(float(h[s].mean()), 2), 'abs_mm': round(float(np.abs(h[s]).mean()), 2),
                                       'p95_mm': round(float(np.percentile(np.abs(h[s]), 95)), 2)}
    return out


def picture(s, h, w):
    S, n = 0.0002, 900; u = ((s[:, 0]-eye[0])/S+n/2).astype(int); v = (n/2-(s[:, 2]-(eye[2]-0.035))/S).astype(int)
    ok = (u >= 0) & (u < n) & (v >= 0) & (v < n) & (s[:, 1] < eye[1]+0.05) & (w > 0.05)
    acc = np.zeros((n, n)); cnt = np.zeros((n, n)); np.add.at(acc, (v[ok], u[ok]), h[ok]); np.add.at(cnt, (v[ok], u[ok]), 1)
    has = cnt > 0; a = ndimage.gaussian_filter(acc, 1.0)/np.maximum(ndimage.gaussian_filter(cnt, 1.0), 1e-6); x = np.clip(a/2.0, -1, 1)
    img = np.dstack([np.where(x > 0, 255, 255*(1+x)), 255*(1-np.abs(x)), np.where(x < 0, 255, 255*(1-x))])
    img[~ndimage.binary_dilation(has, iterations=2)] = 40
    return Image.fromarray(img.astype(np.uint8))


p = co0.copy(); stat = {}; tiles = []
for it in range(4):
    t, b, h, w, fn, foot = heights(p)
    if it == 0: stat['before'] = bands(foot, h, w); tiles.append(picture(foot, h, w))
    if it == 3: break
    x = fit(t, b, h, w, fn, vnormals(p))
    total = p[fi]+vnormals(p)[fi]*x[:, None]/1000-co0[fi]; ln = np.linalg.norm(total, axis=1)
    p[fi] = co0[fi]+total*np.minimum(1, LIM/np.maximum(ln, 1e-12))[:, None]
    print('RELIEF pass %d: the model over the head, mean |mm| %.2f; asked of the vertices mean %.2f max %.2f' % (it, np.abs(h[w > 0.5]).mean(), np.abs(x).mean(), np.abs(x).max()), flush=True)
stat['after'] = bands(foot, h, w); tiles.append(picture(foot, h, w))
# the vertices that are not free skin follow the skin round them, as in reg_solve.py; the eyes stay
du = p-co0; mvd = np.zeros(len(co0), bool); mvd[fi] = True; hk = np.nonzero(follow & ~mvd)[0]
if len(hk):
    dd, kk = cKDTree(co0[fi]).query(co0[hk], k=32); ww = 1/(dd*100+0.05)**4; ww /= ww.sum(1, keepdims=True)
    du[hk] = (du[fi][kk]*ww[:, :, None]).sum(1)
new = co0+du; mv = np.linalg.norm(du[fi], axis=1)*1000; st = tris[body[tris].all(1)]
fl = (np.cross(new[st[:, 1]]-new[st[:, 0]], new[st[:, 2]]-new[st[:, 0]])*np.cross(co0[st[:, 1]]-co0[st[:, 0]], co0[st[:, 2]]-co0[st[:, 0]])).sum(1) < 0
stat['moved_mm'] = {'median': round(float(np.median(mv)), 2), 'p95': round(float(np.percentile(mv, 95)), 2), 'max': round(float(mv.max()), 2),
                    'max_at_mm': [round(float(v), 0) for v in (co0[fi][np.argmax(mv)]-eye)*1000]}
stat['faces_turned_over'] = int(fl.sum()); stat['points_of_the_model'] = int(len(h))
os.makedirs(OUT, exist_ok=True)
np.savez(os.path.join(OUT, 'reg.npz'), **{k: (new if k == 'co' else R[k]) for k in R.files})
np.savez(os.path.join(OUT, 'samples.npz'), tri=ft[t].astype(np.int32), bary=b.astype(np.float32), h=(h/1000).astype(np.float32), w=w.astype(np.float32))
json.dump(stat, open(os.path.join(OUT, 'relief.json'), 'w'), indent=1)
sh = Image.new('RGB', (2*tiles[0].width, tiles[0].height)); sh.paste(tiles[0], (0, 0)); sh.paste(tiles[1], (tiles[0].width, 0))
sh.save(os.path.join(OUT, 'relief.jpg'), quality=88)
print('RELIEF the model over the head, mm:', json.dumps({k: stat[k] for k in ('before', 'after')}))
print('RELIEF moved, mm:', json.dumps(stat['moved_mm']), 'faces turned over', stat['faces_turned_over'])
