"""The last pass of the wrap: the head's faces onto the bald model, its outline onto the concept's.

  python head_outline.py <reg dir> <params.json> <bald front.png> <bald profile.png> <out dir> [--ears ears.npz]
      [--reg reg.npz]

<reg dir>: mpfb.npz, groups.npz, views.json, ref.npz (the bald model, placed by place_ref.py), reg.npz and
reg_log.json of reg_solve.py; --reg: the head to start from when it is not <reg dir>/reg.npz (head_relief.py's).
The pictures are the whole bald views of the concept in the frame of params.json.
Writes <out dir>/reg.npz (the arrays of reg_solve.py with `co` moved), outline.json and outline.jpg.

reg_solve.py lays the head's vertices on the bald model. Two things are left to the outline.
1. Faces. The skull of the template is coarse: 44 vertices over the crown, edges of 2-3 cm. With the vertices on
   the model the flat faces between them cut under it, by a millimetre and more on the crown, and the head's
   outline is that much smaller than the model's. Here the faces are laid on the model, not the vertices: points
   over every face are measured against the model along the face's normal, and the vertices move along their
   normals so that the faces lie on the model in the mean, weighed by area.
   After head_relief.py (--reg) the faces already lie on the model and this part moves next to nothing (0.01 mm on
   the second face); it stays for a head that comes straight from reg_solve.py.
2. Outline. Where the model itself departs from the concept, the concept is right: it is what the user accepted.
   On the second face the underside of the model's chin is 3-5 mm higher than the concept's in the profile, and
   its lower lip 1-3 mm behind. The head's outline in each whole view is compared with the concept's: every pixel
   of our outline knows its signed distance to the concept's and votes for the nearest vertex near our outline.
   Those vertices are asked to move by their mean distance along the direction to the concept's outline, in the
   plane of the view; the depth along the view is not asked. The rest of the head follows by the stiffness of
   Amberg's registration (reg_solve.py): the smoothest change that satisfies the outline, so a head a little
   short from front to back is stretched as a whole, and the underside of a chin moves as a piece. The corners of
   the mouth stay where the detector's points put them in the front view; nothing moves round the eyes.
   Not asked: the brows (hair on the concept's outline), the ears (--ears: their rim is laid by ear_points.py),
   vertices over the model's cloth or within 15 mm of it (the collar is in the concept's outline, not in ours),
   and anything below 140 mm under the eyes.
"""
import json, os, sys
import numpy as np
from PIL import Image
from scipy import ndimage, sparse
from scipy.sparse.linalg import splu
from scipy.spatial import cKDTree

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, '..', '..', '..', 'reports', 'content', '2026-11-hero-head', 'skin'))
import ref_model
from concept_view import Views

EARS = ref_model.take(sys.argv, '--ears'); REG = ref_model.take(sys.argv, '--reg')
D, PJ, OUT = sys.argv[1], sys.argv[2], sys.argv[5]; PIC = {'front': sys.argv[3], 'profile': sys.argv[4]}
J = lambda n: os.path.join(D, n)
P = json.load(open(PJ)); V = json.load(open(J('views.json'))); eye = np.array(V['eye']); W = Views(P, eye, 2)
m = np.load(J('mpfb.npz')); g = np.load(J('groups.npz')); R = np.load(REG or J('reg.npz')); ref = np.load(J('ref.npz'))
names = json.load(open(J('reg_log.json')))['landmarks']
co0 = R['co'].astype(np.float64); tris, edges = m['tris'], m['edges']; body = g['body'].astype(bool)
free, follow, interior, wgt = R['free'], R['follow'], R['interior'], R['w']
rv = ref['v'].astype(np.float64); ref_ok = R['ref_ok']; tree = cKDTree(rv)
ear = np.load(EARS)['ear'] if EARS else np.zeros(len(co0), bool)
skin_tris = tris[body[tris].all(1)]
fi = np.nonzero(free)[0]; nf = len(fi); pos = -np.ones(len(co0), np.int64); pos[fi] = np.arange(nf)
LOW = -0.140                                                # nothing is asked of the outline below this, metres over the eyes
FAR = 8.0                                                   # mm: an outline further from the concept's is another thing
CM = {k: W.concept(PIC[k]) for k in PIC}; CS = {k: W.sdf(CM[k]) for k in PIC}
CG = {k: [x/(W.mpp*1000) for x in np.gradient(CS[k])] for k in PIC}          # d(mm)/d(mm): (along v, along u)
lc, rc = co0[m['leye']].mean(0), co0[m['reye']].mean(0)
HELD = np.array([not n.startswith(('ear', 'tragus', 'rim_')) for n in names]) & (np.abs(co0[R['src_l'], 0]-eye[0]) > 0.012)
d_eye = np.minimum(np.linalg.norm(co0-lc, axis=1), np.linalg.norm(co0-rc, axis=1))


def vnormals(p):
    fn = np.cross(p[tris[:, 1]]-p[tris[:, 0]], p[tris[:, 2]]-p[tris[:, 0]]); vn = np.zeros_like(p)
    for k in range(3): np.add.at(vn, tris[:, k], fn)
    return vn/(np.linalg.norm(vn, axis=1, keepdims=True)+1e-12)


# ---- 1. the faces onto the model
ft = tris[(free[tris] & (wgt[tris] > 0.5) & ~ear[tris]).all(1)]
K = 4; bary = np.array([(a, b, K-a-b) for a in range(K+1) for b in range(K+1-a)], float)/K
fe = np.unique(np.sort(np.concatenate([ft[:, [0, 1]], ft[:, [1, 2]], ft[:, [2, 0]]]), axis=1), axis=0)


def faces(p):
    """Points over the faces against the model: (signed distance along the face's normal, cm; weight; face normal)."""
    a, b, c = p[ft[:, 0]], p[ft[:, 1]], p[ft[:, 2]]; fn = np.cross(b-a, c-a); area = np.linalg.norm(fn, axis=1)/2
    fn /= 2*area[:, None]+1e-15
    pts = (bary[None, :, 0, None]*a[:, None]+bary[None, :, 1, None]*b[:, None]+bary[None, :, 2, None]*c[:, None])
    d, k = tree.query(pts.reshape(-1, 3), workers=-1); q = rv[k].reshape(pts.shape)
    r = ((pts-q)*fn[:, None]).sum(2)*100
    ok = (d.reshape(r.shape) < 0.003) & ref_ok[k].reshape(r.shape)
    return r, ok*(area[:, None]*1e4/len(bary)), fn


def face_pass(p, mu=2.0, lim=0.25):
    r, w, fn = faces(p); N = vnormals(p); nt, nb = r.shape
    rows = np.repeat(np.arange(nt*nb), 3); cols = np.repeat(pos[ft], nb, axis=0).ravel()
    vals = (np.repeat(bary[None], nt, 0)*np.einsum('tk,tvk->tv', fn, N[ft])[:, None, :]).ravel()         # b_k (n_face . N_k)
    sw = np.sqrt(w.ravel()); A = sparse.csr_matrix((vals*np.repeat(sw, 3), (rows, cols)), shape=(nt*nb, nf))
    ea, eb = pos[fe[:, 0]], pos[fe[:, 1]]; ne = len(fe)
    S = sparse.csr_matrix((np.r_[np.full(ne, mu), np.full(ne, -mu)], (np.r_[np.arange(ne), np.arange(ne)], np.r_[ea, eb])), shape=(ne, nf))
    M = sparse.vstack([A, S, 0.05*sparse.identity(nf)]).tocsc(); rhs = np.r_[-r.ravel()*sw, np.zeros(ne+nf)]
    t = splu((M.T@M).tocsc()).solve(M.T@rhs); used = np.zeros(nf, bool); used[pos[ft].ravel()] = True
    t = np.clip(np.where(used, t, 0), -lim, lim); out = p.copy(); out[fi] += N[fi]*t[:, None]/100
    return out, t


def face_stats(p):
    r, w, _ = faces(p); ok = w > 0; z = (p[ft].mean(1)[:, 2]-eye[2])[:, None]+0*r; out = {}
    for name, s in (('skull', ok & (z > 0.03)), ('face and neck', ok & (z <= 0.03))):
        out[name] = {'mean_mm': round(float(np.average(r[s], weights=w[s])*10), 2), 'abs_mm': round(float(np.average(np.abs(r[s]), weights=w[s])*10), 2)}
    return out


# ---- 2. the outline onto the concept's
cloth = rv[~ref_ok]
near_cloth = cKDTree(cloth).query(co0, workers=-1)[0] < 0.015 if len(cloth) else np.zeros(len(co0), bool)
on_model = tree.query(co0, workers=-1)[0] < 0.006
# wgt: where reg_solve.py was told not to trust the model (the brows are hair on it), the concept's outline is hair too.
# The ears are the registration's matter (ear_points.py): an ear is a thin flap whose outline turns with it, and on
# hero 3a, whose ears are 9 mm short of the concept's, the outline pulled one ear 5 mm out and left the other alone.
may = free & on_model & ~near_cloth & (co0[:, 2] > eye[2]+LOW) & (d_eye > 0.022) & (wgt > 0.9) & ~ear


def outline(p, view):
    m_ = W.tris(p, skin_tris, view); return m_, W.sdf(m_)


def rims(p, smooth=6.0):
    """The vertices near our outline that the concept's outline moves: (index among the free, direction, mm to move)."""
    idx, dirs, dist = [], [], []
    for view in PIC:
        m_, so = outline(p, view); u, v = W.proj(p[fi], view); gv, gu = CG[view]
        own = W.at(so, u, v); gr = np.c_[W.at(gu, u, v), W.at(gv, u, v)]; gn = np.linalg.norm(gr, axis=1)
        rim = np.nonzero((own > -1.0) & (gn > 0.8))[0]
        if len(rim) < 3: continue
        # every pixel of our outline votes for the vertex nearest to it, allowed or not: the hair of a brow on the
        # concept's outline must not be heard by the vertices next to the brow
        ei, ek = np.nonzero(W.edge(m_)); keep = ei < W.row(view, LOW); ei, ek = ei[keep], ek[keep]; dp = CS[view][ei, ek]
        uv = np.c_[u[rim], v[rim]]; dd, kk = cKDTree(uv).query(np.c_[ek, ei]); near = (dd*W.mpp*1000 < 8) & (np.abs(dp) < FAR)
        n = np.bincount(kk[near], minlength=len(rim)); s = np.bincount(kk[near], dp[near], minlength=len(rim))
        has = (n >= 3) & may[fi][rim]; d = s[has]/n[has]; rim = rim[has]; uv = uv[has]
        if len(rim) < 3: continue
        # one vertex's mean is noisy by half a millimetre, and a head that follows it wrinkles: the distances are
        # smoothed along the outline
        g2 = np.exp(-((uv[:, None]-uv[None])**2).sum(2)*(W.mpp*1000)**2/(2*smooth**2)); d = (g2@d)/g2.sum(1)
        gr = gr[rim]/gn[rim, None]
        d3 = np.c_[gr[:, 0], 0*d, -gr[:, 1]] if view == 'front' else np.c_[0*d, gr[:, 0], -gr[:, 1]]
        idx.append(rim); dirs.append(d3); dist.append(-d)
    return np.concatenate(idx), np.concatenate(dirs), np.concatenate(dist)


def by_angle(p):
    return {v: W.by_angle(outline(p, v)[0], CS[v], v, LOW) for v in PIC}


def apart(p):
    out = {}
    for v in PIC:
        ei, ek = np.nonzero(W.edge(outline(p, v)[0])); keep = ei < W.row(v, LOW); d = CS[v][ei[keep], ek[keep]]; d = d[np.abs(d) < FAR]
        out[v] = {'mean_mm': round(float(d.mean()), 2), 'abs_mm': round(float(np.abs(d).mean()), 2)}
    return out


def outline_pass(p0, alphas=(30.0, 15.0), beta=2.0, lam=5.0, stay=0.1, pin=5.0, rounds=2, smooth=6.0):
    """Amberg's system round the wrapped head p0: an affine map per free vertex, in cm round the eyes."""
    c0 = (p0-eye)*100; vh = np.hstack([c0[fi], np.ones((nf, 1))])
    Dm = sparse.csr_matrix((vh.ravel(), (np.repeat(np.arange(nf), 4), (4*np.arange(nf)[:, None]+np.arange(4)).ravel())), shape=(nf, 4*nf))
    I3 = sparse.identity(3, format='csr'); D3 = sparse.kron(Dm, I3, format='csr')                     # unknown (4 i + d) 3 + c
    e = edges[free[edges].any(1) & ~interior[edges].any(1)]; both = free[e].all(1); ff, fx = e[both], e[~both]
    one = np.where(free[fx[:, 0]], fx[:, 0], fx[:, 1]); n1, n2 = len(ff), len(fx); k4 = np.arange(4)
    r1 = (4*np.arange(n1)[:, None]+k4).ravel(); r2 = (4*(n1+np.arange(n2))[:, None]+k4).ravel()
    M = sparse.csr_matrix((np.r_[np.ones(4*n1), -np.ones(4*n1), np.ones(4*n2)],
                           (np.r_[r1, r1, r2], np.r_[(4*pos[ff[:, 0]][:, None]+k4).ravel(), (4*pos[ff[:, 1]][:, None]+k4).ravel(), (4*pos[one][:, None]+k4).ravel()])),
                          shape=(4*(n1+n2), 4*nf))
    M3 = sparse.kron(M, I3, format='csr'); Mb = np.zeros((4*(n1+n2), 3)); Mb[4*n1:] = np.tile(np.vstack([np.eye(3), np.zeros((1, 3))]), (n2, 1))
    # the detector's points put the corners of the mouth where they are in the front view: they stay there, and only
    # their depth is free. Not held: the points of the midline, which is the profile's outline (the concept's lower lip
    # stood 3 mm in front of the model's, and a lip held at its middle got a dent there), and the ears' points, which
    # lie on the model's ears.
    keep = HELD & (pos[R['src_l']] >= 0)
    ls = pos[R['src_l'][keep]]; lt = c0[fi][ls][:, [0, 2]]
    L = D3[(3*ls[:, None]+np.array([0, 2])).ravel()]
    # nothing moves round the eyes, and nothing below the lowest height the outline is asked at
    sw = stay+pin*np.maximum(np.clip((0.035-d_eye[fi])/0.013, 0, 1), np.clip((eye[2]+LOW+0.01-p0[fi][:, 2])/0.02, 0, 1))
    Sw = sparse.diags(np.repeat(sw, 3))
    cur = c0.copy(); log = []
    for alpha in alphas:
        for _ in range(rounds):
            p = cur/100+eye; ri, rd, rmm = rims(p, smooth); wr = beta*(1-(rmm/FAR)**2)**2
            Sel = sparse.csr_matrix(((rd*wr[:, None]).ravel(), (np.repeat(np.arange(len(ri)), 3), (3*ri[:, None]+np.arange(3)).ravel())), shape=(len(ri), 3*nf))
            A = sparse.vstack([alpha*M3, Sel@D3, lam*L, Sw@D3]).tocsc()
            b = np.r_[alpha*Mb.ravel(), wr*((rd*cur[fi][ri]).sum(1)+rmm/10), lam*lt.ravel(), np.repeat(sw, 3)*c0[fi].ravel()]
            X = splu((A.T@A).tocsc()).solve(A.T@b); cur[fi] = (D3@X).reshape(nf, 3)
            log.append({'alpha': alpha, 'rim': int(len(ri)), 'asked_mm_abs': round(float(np.abs(rmm).mean()), 2), 'asked_mm_max': round(float(np.abs(rmm).max()), 2)})
            print('OUTLINE step', log[-1], flush=True)
    return cur/100+eye, log


stat = {'faces': {'before': face_stats(co0)}, 'outline_by_direction_mm': {'before': by_angle(co0)}, 'outline_apart_mm': {'before': apart(co0)}}
p = co0
for _ in range(2): p, _ = face_pass(p)
t = np.linalg.norm(p-co0, axis=1)[fi]*1000
stat['faces']['after'] = face_stats(p); stat['faces']['moved_mm'] = {'median': round(float(np.median(t)), 2), 'max': round(float(t.max()), 2)}
stat['outline_by_direction_mm']['faces'] = by_angle(p); stat['outline_apart_mm']['faces'] = apart(p)
print('FACES against the model, mm (mean signed, mean |.|):', json.dumps(stat['faces']), flush=True)
p, log = outline_pass(p)
stat['outline_by_direction_mm']['after'] = by_angle(p); stat['outline_apart_mm']['after'] = apart(p); stat['steps'] = log
stat['faces']['after_outline'] = face_stats(p)
# the vertices that are not free skin follow the skin round them, as in reg_solve.py; the eyes stay
du = p-co0; hk = np.nonzero(follow)[0]
if len(hk):
    dd, kk = cKDTree(co0[fi]).query(co0[hk], k=32); ww = 1/(dd*100+0.05)**4; ww /= ww.sum(1, keepdims=True)
    du[hk] = (du[fi][kk]*ww[:, :, None]).sum(1)
new = co0+du; mv = np.linalg.norm(du[fi], axis=1)*1000; q = (co0[fi]-eye)*1000
stat['moved_mm'] = {'median': round(float(np.median(mv)), 2), 'p95': round(float(np.percentile(mv, 95)), 2), 'max': round(float(mv.max()), 2),
                    'max_at_mm': [round(float(x), 0) for x in q[np.argmax(mv)]]}
lm = pos[R['src_l']]; ok = (lm >= 0) & HELD
stat['landmarks_moved_mm'] = {'in_the_front_view': round(float(np.linalg.norm(du[fi][lm[ok]][:, [0, 2]], axis=1).max()*1000), 2),
                              'in_depth': round(float(np.abs(du[fi][lm[ok]][:, 1]).max()*1000), 2)}
fl = lambda x: (np.cross(x[skin_tris[:, 1]]-x[skin_tris[:, 0]], x[skin_tris[:, 2]]-x[skin_tris[:, 0]])*np.cross(co0[skin_tris[:, 1]]-co0[skin_tris[:, 0]], co0[skin_tris[:, 2]]-co0[skin_tris[:, 0]])).sum(1) < 0
stat['faces_turned_over'] = int(fl(new).sum())
os.makedirs(OUT, exist_ok=True)
np.savez(os.path.join(OUT, 'reg.npz'), **{k: (new if k == 'co' else R[k]) for k in R.files})
json.dump(stat, open(os.path.join(OUT, 'outline.json'), 'w'), indent=1)
for v in PIC:
    for k in ('before', 'faces', 'after'):
        print('OUTLINE %-7s %-6s ours outside the concept by direction, mm: %s' % (v, k, ' '.join('%d:%+.1f' % kv for kv in stat['outline_by_direction_mm'][k][v].items())))
print('OUTLINE apart from the concept, mm:', json.dumps(stat['outline_apart_mm']))
print('OUTLINE moved, mm:', json.dumps(stat['moved_mm']), 'landmarks', stat['landmarks_moved_mm'], 'faces turned over', stat['faces_turned_over'])
tiles = []
for v in PIC:
    im = np.asarray(Image.open(PIC[v]).convert('RGB').resize((W.res, W.res), Image.BICUBIC)).copy()
    for col, x in (((255, 255, 255), None), ((255, 60, 60), co0), ((60, 255, 90), new)):          # white: the concept, red: before, green: after
        m_ = CM[v] if x is None else outline(x, v)[0]; im[m_ & ~ndimage.binary_erosion(m_, iterations=2)] = col
    tiles.append(Image.fromarray(im))
sh = Image.new('RGB', (2*W.res, W.res)); sh.paste(tiles[0], (0, 0)); sh.paste(tiles[1], (W.res, 0)); sh.save(os.path.join(OUT, 'outline.jpg'), quality=88)
