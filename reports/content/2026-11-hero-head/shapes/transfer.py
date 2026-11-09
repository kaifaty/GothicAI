# Carries the ARKit face shapes of another face (Mark of Audio2Face v3.0, or ICT-FaceKit) onto the hero's head
# (face-shapes.md, "Способ"). Runs in ~/.local/share/gothic-ai/mpfb/venv (numpy, scipy, trimesh).
#   python transfer.py mark|ict <work dir>
# <work dir>/hero.npz is dump_glb.py of the hero's head.glb; the sources live in ~/.local/share/gothic-ai/face-sources.
# Writes <work dir>/<src>.npz: absolute coordinates of every face, brow, lash, teeth and tongue shape to set, and
# <work dir>/<src>.json: what was measured (landmarks, fit, correspondence).
import json, os, sys
import numpy as np
import trimesh
from scipy.spatial import cKDTree

SRC, WD = sys.argv[1], os.path.expanduser(sys.argv[2])
FS = os.path.expanduser('~/.local/share/gothic-ai/face-sources')
REG = os.path.expanduser('~/.local/share/gothic-ai/mpfb/work/reg')
ARKIT = ("eyeBlinkLeft eyeLookDownLeft eyeLookInLeft eyeLookOutLeft eyeLookUpLeft eyeSquintLeft eyeWideLeft "
         "eyeBlinkRight eyeLookDownRight eyeLookInRight eyeLookOutRight eyeLookUpRight eyeSquintRight eyeWideRight "
         "jawForward jawLeft jawRight jawOpen mouthClose mouthFunnel mouthPucker mouthLeft mouthRight mouthSmileLeft "
         "mouthSmileRight mouthFrownLeft mouthFrownRight mouthDimpleLeft mouthDimpleRight mouthStretchLeft "
         "mouthStretchRight mouthRollLower mouthRollUpper mouthShrugLower mouthShrugUpper mouthPressLeft "
         "mouthPressRight mouthLowerDownLeft mouthLowerDownRight mouthUpperUpLeft mouthUpperUpRight browDownLeft "
         "browDownRight browInnerUp browOuterUpLeft browOuterUpRight cheekPuff cheekSquintLeft cheekSquintRight "
         "noseSneerLeft noseSneerRight tongueOut").split()
KEEP = [n for n in ARKIT if n.startswith(('eyeBlink', 'eyeLook', 'eyeSquint', 'eyeWide'))] + ['tongueOut']  # ours
JAW = ['jawOpen', 'jawLeft', 'jawRight', 'jawForward']
unit = lambda v: v/np.linalg.norm(v)


def load_hero():
    h = np.load(os.path.join(WD, 'hero.npz'))
    V, T = h['face|co'], h['face|tri']
    S = {n: h['face|sk|'+n]-V for n in ARKIT}
    # interior (mouth bag, vestibule, eye-socket lining) from the fit: the GLB is a scaled, turned copy of reg.npz
    R = np.load(os.path.join(REG, 'reg.npz'))
    s = 1.16
    A = s*np.array([[0, 0, -1], [0, 1, 0], [1, 0, 0]], float)
    P = R['co'] @ A.T
    b = np.array([V[:, 0].min()-P[:, 0].min(), 0, 0])     # the crown of the head: GLB x points down
    for _ in range(30):
        d, nn = cKDTree(P+b).query(V)
        b += np.median(V-(P[nn]+b), axis=0)
    d, nn = cKDTree(P+b).query(V)
    print('hero -> fit: %.2e m max mismatch' % d.max())
    assert d.max() < 1e-4, 'the head is not the export of the fit'
    return dict(V=V, T=T, S=S, interior=R['interior'][nn], valid=~R['interior'][nn], h=h)


def load_mark():
    d = np.load(os.path.join(FS, 'mark/bs_skin_Mark.npz'))
    V = d['neutral'].astype(float)
    return dict(V=V, T=None, S={n: d[n].astype(float) for n in ARKIT if n in d.files})   # stored as shifts


def load_ict():
    D = os.path.join(FS, 'ict-facekit/FaceXModel')
    def obj(name, faces=False):
        v, f = [], []
        for line in open(os.path.join(D, name+'.obj')):
            if line.startswith('v '):
                v.append([float(x) for x in line.split()[1:4]])
            elif faces and line.startswith('f '):
                ids = [int(x.split('/')[0])-1 for x in line.split()[1:]]
                f += [[ids[0], ids[i], ids[i+1]] for i in range(1, len(ids)-1)]
        return np.array(v), np.array(f)
    V, F = obj('generic_neutral_mesh', True)
    N = 11248                                   # face and head-and-neck; sockets, teeth and eyes are other parts
    F = F[(F < N).all(1)]
    V = V[:N]
    S = {}
    side = {'_L': 'Left', '_R': 'Right'}
    for f in sorted(os.listdir(D)):
        n = f[:-4]
        if not f.endswith('.obj') or n.startswith(('identity', 'generic', 'Pupil', 'cheekRaiser')):
            continue
        d = obj(n)[0][:N]-V
        a = n[:-2]+side[n[-2:]] if n[-2:] in side else n
        if a in ('browInnerUpLeft', 'browInnerUpRight', 'cheekPuffLeft', 'cheekPuffRight'):
            a = a[:-4] if a.endswith('Left') else a[:-5]
            S[a] = S.get(a, 0)+d
        else:
            S[a] = d
    return dict(V=V, T=F, S=S)


def movers(d, frac):
    m = np.linalg.norm(d, axis=1)
    return m > frac*m.max()


def frame_and_landmarks(M):
    """Landmarks found by the shapes themselves, the same way on every face: the eyes are where the blink moves,
    the mouth corners where the smile moves most; the frame is the plane of eyes and mouth corners, and nose tip,
    lips and chin are the points that stand out of it most along the midline."""
    V = M['V']
    ok = M.get('valid', np.ones(len(V), bool))[:, None]       # the hero's mouth bag moves most in many shapes
    S = {n: d*ok for n, d in M['S'].items()}
    eL = V[movers(S['eyeBlinkLeft'], 0.3)].mean(0)
    eR = V[movers(S['eyeBlinkRight'], 0.3)].mean(0)
    u = np.linalg.norm(eL-eR)
    lm = {}
    top = lambda d, k: V[np.argsort(-np.linalg.norm(d, axis=1))[:k]].mean(0)
    for sd in ('Left', 'Right'):
        lm['lid'+sd] = V[movers(S['eyeBlink'+sd], 0.85)].mean(0)    # the middle of the upper lid's margin
        lm['mouth'+sd] = top(S['mouthSmile'+sd], 3)
    mc = (lm['mouthLeft']+lm['mouthRight'])/2
    ec = (eL+eR)/2
    left = unit(lm['mouthLeft']-lm['mouthRight']+eL-eR)
    down = mc-ec; down = unit(down-left*(down @ left))
    fwd = unit(np.cross(down, left))
    if (S['mouthPucker'][movers(S['mouthPucker'], 0.5)].mean(0)) @ fwd < 0:
        fwd = -fwd                              # a mirrored naming: its "Left" is on the other side
    hand = float(np.sign(np.cross(fwd, down) @ left))
    rel = V-mc
    mid = np.abs(rel @ left) < 0.04*u
    dn, fw = rel @ down, rel @ fwd
    pk = np.linalg.norm(S['mouthPucker'], axis=1)
    def most(sel, name, by):
        lm[name] = V[np.nonzero(sel)[0][np.argmax(by[sel])]]
    most(mid & (dn < 0) & (dn > -0.35*u), 'lipUpper', pk)          # the lips move most in a pucker
    most(mid & (dn > 0) & (dn < 0.4*u), 'lipLower', pk)
    most(mid & (dn > 0.55*u) & (dn < 1.3*u), 'chin', fw)
    # the nose tip stands out most from the straight line between the lids' middle and the upper lip
    a, b = (lm['lidLeft']+lm['lidRight'])/2, lm['lipUpper']
    ab = unit(b-a); off = (V-a)-np.outer((V-a) @ ab, ab)
    band = mid & ((V-a) @ ab > 0.25*np.linalg.norm(b-a)) & ((V-a) @ ab < 0.9*np.linalg.norm(b-a))
    most(band, 'noseTip', np.linalg.norm(off, axis=1)*np.sign(off @ fwd))
    return dict(down=down, fwd=fwd, left=left, hand=hand, u=u, eye={'Left': eL, 'Right': eR}, lm=lm, mc=mc)


def swap_sides(S):
    out = {}
    for n, d in S.items():
        m = n.replace('Left', '#').replace('Right', 'Left').replace('#', 'Right')
        out[m] = d
    return out


def similarity(A, B):
    """s, R, t with s R A + t ~ B (Umeyama)."""
    ma, mb = A.mean(0), B.mean(0)
    a, b = A-ma, B-mb
    U, D, Vt = np.linalg.svd(b.T @ a)
    E = np.eye(3); E[2, 2] = np.sign(np.linalg.det(U @ Vt))
    R = U @ E @ Vt
    s = (D*np.diag(E)).sum()/(a**2).sum()
    return s, R, mb-s*R @ ma


hero = load_hero()
src = load_mark() if SRC == 'mark' else load_ict()
fh = frame_and_landmarks(hero)
fs = frame_and_landmarks(src)
print('handedness hero %+d %s %+d' % (fh['hand'], SRC, fs['hand']))
if fh['hand'] != fs['hand']:
    src['S'] = swap_sides(src['S'])
    fs = frame_and_landmarks(src)
    print('  sides swapped: now %+d' % fs['hand'])
names = sorted(fh['lm'])
LH = np.array([fh['lm'][n] for n in names]); LS = np.array([fs['lm'][n] for n in names])
s, R, t = similarity(LS, LH)
VS = src['V'] @ (s*R).T+t
res = np.linalg.norm(LS @ (s*R).T+t-LH, axis=1)
print('similarity: scale %.4f, landmark residual mean %.1f mm, max %.1f mm (%s)'
      % (s, res.mean()*1000, res.max()*1000, names[int(res.argmax())]))
DS = {n: d @ (s*R).T for n, d in src['S'].items()}

# 2. our outer skin onto the source's skin (Amberg); the interior follows afterwards
V, T, inter = hero['V'], hero['T'], hero['interior']
keepf = ~inter[T].any(1)
outer_v = np.unique(T[keepf])
remap = -np.ones(len(V), int); remap[outer_v] = np.arange(len(outer_v))
sub = trimesh.Trimesh(V[outer_v], remap[T[keepf]], process=False)
# the lip centres are found differently on lip fronts and lip contacts: the skin itself places the lips
reg_names = [n for n in names if not n.startswith('lip')]
lm_idx = [int(cKDTree(V[outer_v]).query(fh['lm'][n])[1]) for n in reg_names]
lm_tgt = np.array([fs['lm'][n] for n in reg_names]) @ (s*R).T+t
# solved in units of the eye distance, so stiffness and the distance threshold do not depend on metres or cm
u = fh['u']
sub_u = trimesh.Trimesh(sub.vertices/u, sub.faces, process=False)
tgt = trimesh.PointCloud(VS/u)               # dense enough for nearest points; faces would need rtree
steps = [[50, 10, 0.5, 10], [20, 5, 0.5, 10], [5, 2.5, 0.5, 10], [2, 1, 0.5, 10], [0.8, 0.5, 0.0, 10],
         [0.3, 0.2, 0.0, 10], [0.1, 0.1, 0.0, 10]]
Q = trimesh.registration.nricp_amberg(sub_u, tgt, source_landmarks=lm_idx, target_positions=lm_tgt/u,
                                      steps=steps, distance_threshold=0.3, use_faces=False,
                                      use_vertex_normals=True)*u
dist = cKDTree(VS).query(Q)[0]
face_zone = np.linalg.norm(V[outer_v]-(fh['mc']+(fh['eye']['Left']+fh['eye']['Right'])/2)/2, axis=1) < 1.2*fh['u']
pos_o = -np.ones(len(V), int); pos_o[outer_v] = np.arange(len(outer_v))
lm_after = {n: float(np.linalg.norm(Q[pos_o[int(cKDTree(V).query(fh['lm'][n])[1])]]-(fs['lm'][n] @ (s*R).T+t))*1000)
            for n in names}
print('landmarks after the registration, mm:', {n: round(v, 1) for n, v in lm_after.items()})
print('registered: distance to source skin median %.2f mm, 95%% %.2f mm; face %.2f / %.2f mm'
      % (np.median(dist)*1000, np.percentile(dist, 95)*1000, np.median(dist[face_zone])*1000,
         np.percentile(dist[face_zone], 95)*1000))

# 3. correspondence: k nearest source points; mouth and lids match only within their own half
def labels(S, Vx, chin):
    """Lower half: what goes with the chin in jawOpen (over 40 % of its shift; the upper lip's edge moves too, less).
    Upper lids: what the blink moves."""
    m = np.linalg.norm(S['jawOpen'], axis=1)
    ci = int(np.argmin(np.linalg.norm(Vx-chin, axis=1)))
    return m > 0.4*m[ci], movers(S['eyeBlinkLeft'], 0.15) | movers(S['eyeBlinkRight'], 0.15)
jh, lh = labels(hero['S'], V, fh['lm']['chin'])
js, ls = labels(DS, VS, fs['lm']['chin'] @ (s*R).T+t)
Vo = V[outer_v]
near_mouth = np.linalg.norm(Vo-fh['mc'], axis=1) < 0.7*fh['u']
near_eye = np.minimum(*[np.linalg.norm(Vo-fh['eye'][k], axis=1) for k in ('Left', 'Right')]) < 0.45*fh['u']
K = 4
idx = np.zeros((len(Vo), K), int); wts = np.zeros((len(Vo), K))
groups = [(~near_mouth & ~near_eye, np.ones(len(VS), bool))]
for zone, mh, ms in ((near_mouth, jh[outer_v], js), (near_eye & ~near_mouth, lh[outer_v], ls)):
    groups += [(zone & mh, ms), (zone & ~mh, ~ms)]
for sel, ms in groups:
    if not sel.any():
        continue
    pool = np.nonzero(ms)[0]
    d, j = cKDTree(VS[pool]).query(Q[sel], k=K)
    w = 1/np.maximum(d, 1e-4)**2
    idx[sel], wts[sel] = pool[j], w/w.sum(1, keepdims=True)
corr = cKDTree(VS).query(Q)[0]

# 4. carry every shape over. Interior vertices (mouth bag, vestibule, the lips' inner faces) keep their own MPFB
# motion plus the correction of the outer vertices that moved like them in MPFB: near in place and in MPFB shift,
# so the inside of the upper lip follows the upper lip even where the lips touch. The lids stay ours, like the blink
# and the gaze: around each eyeball the carried shift fades into our own shift of the same shape.
inner = np.nonzero(~np.isin(np.arange(len(V)), outer_v))[0]
balls = [h['eye_%s|co' % k].mean(0) for k in ('L', 'R')] if (h := hero['h']) is not None else []
rad = np.mean([np.linalg.norm(h['eye_%s|co' % k]-c, axis=1).max() for k, c in zip('LR', balls)])
de = np.min([np.linalg.norm(V-c, axis=1) for c in balls], axis=0)
x = np.clip((de-(rad+0.003))/0.010, 0, 1)
w_eye = (x*x*(3-2*x))[:, None]                 # 0 up to 3 mm off the ball, 1 from 13 mm
new = {}
for n in ARKIT:
    if n in KEEP or n not in DS:
        new[n] = hero['S'][n]
        continue
    d = hero['S'][n].copy()
    d[outer_v] = (DS[n][idx]*wts[..., None]).sum(1)
    corr = d[outer_v]-hero['S'][n][outer_v]
    feat = lambda i: np.hstack([V[i], 3*hero['S'][n][i]])
    dk, jk = cKDTree(feat(outer_v)).query(feat(inner), k=4)
    wk = 1/np.maximum(dk, 1e-4); wk /= wk.sum(1, keepdims=True)
    d[inner] = hero['S'][n][inner]+(corr[jk]*wk[..., None]).sum(1)
    new[n] = w_eye*d+(1-w_eye)*hero['S'][n]

# 5. brows and lashes ride on the skin triangle under them; lower teeth and tongue follow the chin
h = hero['h']
res_out = {'face|'+n: V+new[n] for n in ARKIT}
tc = V[T].mean(1)
def ride(P0):
    _, ti = cKDTree(tc).query(P0)
    a, b, c = (V[T[ti, k]] for k in range(3))
    def fr(a, b, c):
        e1 = b-a; n = np.cross(e1, c-a); n /= np.linalg.norm(n, axis=1, keepdims=True)
        e1 /= np.linalg.norm(e1, axis=1, keepdims=True); e2 = np.cross(n, e1)
        return a, e1, e2, n
    o, e1, e2, nn = fr(a, b, c)
    loc = np.stack([((P0-o)*e).sum(1) for e in (e1, e2, nn)], 1)
    def build(Vd):
        o, e1, e2, nn = fr(Vd[T[ti, 0]], Vd[T[ti, 1]], Vd[T[ti, 2]])
        return o+loc[:, :1]*e1+loc[:, 1:2]*e2+loc[:, 2:]*nn
    return build
for part in ('brows', 'lashes'):
    P0 = h[part+'|co']; build = ride(P0); base = build(V)
    for n in ARKIT:
        k = part+'|sk|'+n
        if k in h.files:
            res_out[part+'|'+n] = h[k] if n in KEEP else P0+build(V+new[n])-base
chin_i = int(cKDTree(V).query(fh['lm']['chin'])[1])
ratio = {}
for n in JAW:
    old = np.linalg.norm(hero['S'][n][chin_i]); nw = np.linalg.norm(new[n][chin_i])
    ratio[n] = float(nw/old) if old > 1e-6 else 1.0
for part in ('teeth', 'tongue'):
    P0 = h[part+'|co']
    for n in ARKIT:
        k = part+'|sk|'+n
        if k in h.files:
            res_out[part+'|'+n] = P0+(h[k]-P0)*ratio.get(n, 1.0)
np.savez_compressed(os.path.join(WD, SRC+'.npz'), **res_out)

mag = {n: [float(np.linalg.norm(hero['S'][n], axis=1).max()*1000), float(np.linalg.norm(new[n], axis=1).max()*1000)]
       for n in ARKIT if n not in KEEP}
info = dict(src=SRC, scale=float(s), landmarks=names, landmark_residual_mm=(res*1000).round(2).tolist(),
            landmark_after_registration_mm=lm_after,
            registered_median_mm=float(np.median(dist)*1000), registered_p95_mm=float(np.percentile(dist, 95)*1000),
            correspondence_p95_mm=float(np.percentile(corr, 95)*1000), jaw_teeth_ratio=ratio,
            max_shift_mm_hero_vs_new=mag, swapped=bool(fh['hand'] != frame_and_landmarks(src)['hand']))
json.dump(info, open(os.path.join(WD, SRC+'.json'), 'w'), indent=1)
for n in ('jawOpen', 'mouthClose', 'mouthRollLower', 'mouthPucker', 'mouthSmileLeft', 'browInnerUp'):
    print('  %-16s max shift: hero %5.1f mm, %s %5.1f mm' % (n, mag[n][0], SRC, mag[n][1]))
print('teeth ratio', {k: round(v, 2) for k, v in ratio.items()})

# 6. the other way (face-shapes.md, "Решатель под наши формы"): the shapes a head plays, carried onto Mark's mesh, so
# that Audio2Face's own solver finds the weights for them. Every Mark point takes the shift of the nearest registered
# skin points of the hero (mouth and lids only within their own half), turned back into Mark's frame and centimetres.
#   python transfer.py mark <work dir> mpfb,ict,mark   ->  <work dir>/solver/bs_skin_<set>.npz
# mpfb is the head as built; ict and mark are <set>.npz written by earlier runs (mark there is the round-trip check).
if len(sys.argv) > 3:
    assert SRC == 'mark', 'the solver works on Mark'
    mk = np.load(os.path.join(FS, 'mark/bs_skin_Mark.npz'))
    nm_s = np.linalg.norm(VS-fh['mc'], axis=1) < 0.7*fh['u']
    ne_s = np.minimum(*[np.linalg.norm(VS-fh['eye'][k], axis=1) for k in ('Left', 'Right')]) < 0.45*fh['u']
    ridx = np.zeros((len(VS), K), int); rw = np.zeros((len(VS), K))
    rgroups = [(~nm_s & ~ne_s, np.ones(len(Q), bool))]
    for zone, ms, mh in ((nm_s, js, jh[outer_v]), (ne_s & ~nm_s, ls, lh[outer_v])):
        rgroups += [(zone & ms, mh), (zone & ~ms, ~mh)]
    for sel, mh in rgroups:
        if not sel.any():
            continue
        pool = np.nonzero(mh)[0]
        dq, jq = cKDTree(Q[pool]).query(VS[sel], k=K)
        wq = 1/np.maximum(dq, 1e-4)**2
        ridx[sel], rw[sel] = pool[jq], wq/wq.sum(1, keepdims=True)
    gap = cKDTree(Q).query(VS[mk['frontalMask']])[0]*1000
    print('solver mask (%d Mark points) to the registered hero skin: median %.2f mm, 95%% %.2f mm, max %.1f mm'
          % (len(gap), np.median(gap), np.percentile(gap, 95), gap.max()))
    os.makedirs(os.path.join(WD, 'solver'), exist_ok=True)
    np.savez(os.path.join(WD, 'solver', 'reg.npz'), Q=Q, outer_v=outer_v, VS=VS, s=s, R=R, t=t)   # for checks
    for name in sys.argv[3].split(','):
        if name == 'mpfb':
            SH = hero['S']
        else:
            e = np.load(os.path.join(WD, name+'.npz'))
            SH = {n: e['face|'+n]-V for n in ARKIT}
        out = {k: mk[k] for k in mk.files}
        for n in ARKIT:
            if n != 'tongueOut':                # the solver leaves it out (bsSolveActivePoses); Mark's own stays
                dn = (SH[n][outer_v][ridx]*rw[..., None]).sum(1)
                out[n] = (dn @ R/s).astype(mk[n].dtype)
        np.savez(os.path.join(WD, 'solver', 'bs_skin_%s.npz' % name), **out)   # uncompressed, like NVIDIA's
        print('solver shapes %s: jawOpen %.2f cm, mouthClose %.2f cm (Mark %.2f / %.2f)' % (
            name, np.linalg.norm(out['jawOpen'], axis=1).max(), np.linalg.norm(out['mouthClose'], axis=1).max(),
            np.linalg.norm(mk['jawOpen'], axis=1).max(), np.linalg.norm(mk['mouthClose'], axis=1).max()))
