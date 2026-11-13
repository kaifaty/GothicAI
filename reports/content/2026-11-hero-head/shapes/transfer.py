# Carries the ARKit face shapes of another face (Mark of Audio2Face v3.0, or ICT-FaceKit) onto a head of
# export_test_head.py (face-shapes.md, "Способ" and "Формы Mark в игре"). Runs in ~/.local/share/gothic-ai/mpfb/venv
# (numpy, scipy, trimesh). As a module: hero_from_parts(), register(), carry() - head_shapes.py writes a head with them.
#   python transfer.py mark|ict <work dir> [mpfb,ict,mark]
# <work dir>/hero.npz is dump_glb.py of the hero's head.glb (its fit: --reg <reg.npz>, by default the probe's
# ~/.local/share/gothic-ai/mpfb/work/reg/reg.npz); the sources live in ~/.local/share/gothic-ai/face-sources.
# Writes <work dir>/<src>.npz: absolute coordinates of every face, brow, lash, teeth and tongue shape to set, and
# <work dir>/<src>.json: what was measured (landmarks before and after the registration, fit, correspondence).
# With the third argument, step 6: the shapes of those sets carried back onto Mark's mesh for the solver.
import json
import os
import sys

import numpy as np
import trimesh
from scipy.spatial import cKDTree

FS = os.path.expanduser('~/.local/share/gothic-ai/face-sources')
REG = os.path.expanduser('~/.local/share/gothic-ai/mpfb/work/reg/reg.npz')
ARKIT = ("eyeBlinkLeft eyeLookDownLeft eyeLookInLeft eyeLookOutLeft eyeLookUpLeft eyeSquintLeft eyeWideLeft "
         "eyeBlinkRight eyeLookDownRight eyeLookInRight eyeLookOutRight eyeLookUpRight eyeSquintRight eyeWideRight "
         "jawForward jawLeft jawRight jawOpen mouthClose mouthFunnel mouthPucker mouthLeft mouthRight mouthSmileLeft "
         "mouthSmileRight mouthFrownLeft mouthFrownRight mouthDimpleLeft mouthDimpleRight mouthStretchLeft "
         "mouthStretchRight mouthRollLower mouthRollUpper mouthShrugLower mouthShrugUpper mouthPressLeft "
         "mouthPressRight mouthLowerDownLeft mouthLowerDownRight mouthUpperUpLeft mouthUpperUpRight browDownLeft "
         "browDownRight browInnerUp browOuterUpLeft browOuterUpRight cheekPuff cheekSquintLeft cheekSquintRight "
         "noseSneerLeft noseSneerRight tongueOut").split()
# ours: the blink and the gaze were built around our eyeballs (Audio2Face's weights for the gaze are dropped anyway)
KEEP = [n for n in ARKIT if n.startswith(('eyeBlink', 'eyeLook'))] + ['tongueOut']
JAW = ['jawOpen', 'jawLeft', 'jawRight', 'jawForward']
LIDS = ('lid', 'lidLow')                       # per side: the middle of the upper and of the lower lid's margin
unit = lambda v: v/np.linalg.norm(v)           # noqa: E731


def similarity(A, B):
    """s, R, t with s R A + t ~ B (Umeyama)."""
    ma, mb = A.mean(0), B.mean(0)
    a, b = A-ma, B-mb
    U, D, Vt = np.linalg.svd(b.T @ a)
    E = np.eye(3)
    E[2, 2] = np.sign(np.linalg.det(U @ Vt))
    R = U @ E @ Vt
    s = (D*np.diag(E)).sum()/(a**2).sum()
    return s, R, mb-s*R @ ma


def movers(d, frac):
    m = np.linalg.norm(d, axis=1)
    return m > frac*m.max()


def fit_sphere(P):
    A = np.c_[2*P, np.ones(len(P))]
    x = np.linalg.lstsq(A, (P**2).sum(1), rcond=None)[0]
    return x[:3], float(np.sqrt(x[3]+x[:3] @ x[:3]))


# ---- the faces ------------------------------------------------------------------------------------------------
def hero_from_parts(h, reg=REG):
    """The head's face part with its shapes as shifts. Its interior (mouth bag, vestibule, eye-socket lining) comes
    from the fit the head was exported from: the GLB is a scaled, turned copy of reg.npz, cut at the neck (the vertices
    the cut made are outer skin)."""
    V, T = h['face|co'], h['face|tri']
    S = {n: h['face|sk|'+n]-V for n in ARKIT}
    R = np.load(reg)
    P = R['co'] @ (1.16*np.array([[0, 0, -1], [0, 1, 0], [1, 0, 0]], float)).T
    P = P+[V[:, 0].min()-P[:, 0].min(), 0, 0]              # the crown of the head: GLB x points down
    for i in range(50):
        d, nn = cKDTree(P).query(V)
        ok = d <= np.percentile(d, 95) if i >= 40 else d >= 0      # the neck cut's new vertices left out at the end
        s, Rr, t = similarity(P[nn[ok]], V[ok])
        P = P @ (s*Rr).T+t
    d, nn = cKDTree(P).query(V)
    match = d < 2e-4
    print('hero -> fit %s: %d of %d vertices within 0.2 mm (median %.1e m), the rest is the neck cut'
          % (os.path.basename(os.path.dirname(reg)), match.sum(), len(V), np.median(d)))
    assert np.median(d) < 1e-4 and match.mean() > 0.97, 'the head is not the export of this fit'
    inter = R['interior'][nn] & match
    eyes = {}
    for k, side in (('L', 'Left'), ('R', 'Right')):
        E = h['eye_%s|co' % k]
        c, r = fit_sphere(E)
        eyes[side] = dict(c=c, r=r, rmax=float(np.linalg.norm(E-c, axis=1).max()), co=E)
    # around the balls the fit's interior is more than the socket's lining: with eye_block (reg_solve.py) it holds
    # a whole core round each ball still, lids included. Here only the lining is interior: near a ball and facing it
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from head_glb import normals
    N = normals(V, T)
    c = np.array([e['c'] for e in eyes.values()])
    k = np.argmin(np.linalg.norm(V[:, None]-c[None], axis=2), axis=1)
    rel = V-c[k]
    dist = np.linalg.norm(rel, axis=1)
    core = dist < max(e['rmax'] for e in eyes.values())+0.012
    lining = core & ((N*rel).sum(1) < -0.3*dist)
    print('  around the balls: %d of %d interior vertices are lids, %d the lining'
          % ((inter & core & ~lining).sum(), (inter & core).sum(), (inter & lining).sum()))
    inter = (inter & ~core) | (inter & lining)
    return dict(V=V, T=T, S=S, interior=inter, valid=~inter, h=h, balls=eyes)


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


def swap_sides(S):
    return {n.replace('Left', '#').replace('Right', 'Left').replace('#', 'Right'): d for n, d in S.items()}


# ---- landmarks ------------------------------------------------------------------------------------------------
def frame(M):
    """The face's axes from its shapes: the eyes are where the blink moves, the mouth corners where the smile moves
    most; down is from the eyes to the mouth, left from the right corners to the left ones."""
    V = M['V']
    ok = M.get('valid', np.ones(len(V), bool))[:, None]       # the hero's mouth bag moves most in many shapes
    S = {n: d*ok for n, d in M['S'].items()}
    eL = V[movers(S['eyeBlinkLeft'], 0.3)].mean(0)
    eR = V[movers(S['eyeBlinkRight'], 0.3)].mean(0)
    top = lambda d, k: V[np.argsort(-np.linalg.norm(d, axis=1))[:k]].mean(0)   # noqa: E731
    cL, cR = top(S['mouthSmileLeft'], 3), top(S['mouthSmileRight'], 3)
    mc, ec = (cL+cR)/2, (eL+eR)/2
    left = unit(cL-cR+eL-eR)
    down = mc-ec
    down = unit(down-left*(down @ left))
    fwd = unit(np.cross(down, left))
    if (S['mouthPucker'][movers(S['mouthPucker'], 0.5)].mean(0)) @ fwd < 0:
        fwd = -fwd                              # a mirrored naming: its "Left" is on the other side
    return dict(down=down, fwd=fwd, left=left, hand=float(np.sign(np.cross(fwd, down) @ left)),
                u=float(np.linalg.norm(eL-eR)), h=float((mc-ec) @ down), eye={'Left': eL, 'Right': eR}, mc=mc,
                corner={'Left': cL, 'Right': cR})


def profile(V, ok, fr):
    """The outer midline: per 1/90 of the eyes-to-mouth height the front-most point within a narrow band of the
    midline (a little wider than the mesh's spacing), without the points that stand far behind their neighbours
    (a bin the band missed shows the back of the head)."""
    r = V-fr['mc']
    x, y, z = r @ fr['left'], r @ fr['down'], r @ fr['fwd']
    sp = np.median(cKDTree(V[ok]).query(V[ok], k=2)[0][:, 1])
    band = max(1.6*sp, 0.012*fr['h'])
    m = (np.abs(x) < band) & ok
    step = fr['h']/90
    rows = []
    for b in np.arange(-0.85*fr['h'], 0.75*fr['h'], step):
        k = np.nonzero(m & (y >= b) & (y < b+step))[0]
        if len(k):
            k = k[np.argmax(z[k])]
            rows.append((y[k], z[k], k))
    p = np.array(rows)
    med = np.array([np.median(p[max(0, i-3):i+4, 1]) for i in range(len(p))])
    return p[p[:, 1] > med-0.08*fr['h']]


def landmarks(M, fr):
    """Landmarks found the same way on every face (face-shapes.md, "Формы Mark в игре", "Опорные точки"), as
    vertices of the face: on the outer midline the nose tip, the deepest point under the nose, the most prominent
    points of the upper and lower lip, the lips' contact (the lowest point of the upper lip and the highest of the
    lower: the halves of the jaw), the deepest point over the chin and the chin's most prominent point; the mouth
    corners where the smile moves most; per eye the middle of the upper lid's margin (where the blink moves most) and
    of the lower one (where the blink closes the upper)."""
    V = M['V']
    ok = M.get('valid', np.ones(len(V), bool))
    S = {n: d*ok[:, None] for n, d in M['S'].items()}
    p = profile(V, ok, fr)
    y, z, k = p[:, 0], p[:, 1], p[:, 2].astype(int)
    jaw = np.linalg.norm(S['jawOpen'], axis=1)
    jl = jaw[k] > 0.4*jaw[k].max()
    lm = {}

    def chord(i, j, sign):                       # the point between i and j furthest in front (+1) or behind (-1)
        a, b = np.array([y[i], z[i]]), np.array([y[j], z[j]])
        n = unit(np.array([-(b-a)[1], (b-a)[0]]))
        n = n if n[1] > 0 else -n
        d = (np.c_[y[i:j+1], z[i:j+1]]-a) @ n*sign
        return i+int(np.argmax(d))
    lowf = (y > 0.15*fr['h']) & jl                             # the chin's front, then where it turns under
    ment = int(np.nonzero(lowf & (z > z[lowf].max()-0.1*fr['h']))[0].max())
    up = np.nonzero(~jl[:ment] & (y[:ment] < 0.2*fr['h']))[0]
    su = int(up.max())                                         # the upper lip's lowest point
    sl = su+1+int(np.nonzero(jl[su+1:ment+1])[0][0])           # the lower lip's highest point
    sel = (y > -0.75*fr['h']) & (y < -0.15*fr['h'])
    nose = int(np.nonzero(sel)[0][np.argmax(z[sel])])
    sub = chord(nose, su, -1)
    lu = chord(sub, su, 1)
    sulcus = chord(sl, ment, -1)
    ll = chord(sl, sulcus, 1)
    chin = chord(sulcus, ment, 1)
    for n, i in (('noseTip', nose), ('subnasale', sub), ('lipUpper', lu), ('lipContactUpper', su),
                 ('lipContactLower', sl), ('lipLower', ll), ('sulcus', sulcus), ('chin', chin)):
        lm[n] = int(k[i])
    for sd in ('Left', 'Right'):
        # the upper lid: what goes down in the blink (Mark's lower lid goes up, the hero's fold over the lid too);
        # its margin is its lowest row. The hero's margin rolls in towards the ball, and its fit counts that as the
        # socket's lining (interior): the blink is taken whole, the landmark is the outer vertex nearest the margin.
        b = M['S']['eyeBlink'+sd]
        sgn = 1 if sd == 'Left' else -1
        mb = np.linalg.norm(b, axis=1)*(((V-fr['mc']) @ fr['left'])*sgn > 0.1*fr['u'])   # this side's eye only
        band = np.nonzero((mb > 0.35*mb.max()) & (b @ fr['down'] > 0.3*mb))[0]
        x = (V[band]-fr['mc']) @ fr['left']
        mid = band[np.argsort(np.abs(x-(np.percentile(x, 3)+np.percentile(x, 97))/2))[:max(4, len(band)//12)]]
        top = mid[np.argmax((V[mid]-fr['mc']) @ fr['down'])]
        shut = V[top]+b[top]                                     # where the closed upper lid's middle lies
        low = np.nonzero(ok & (mb < 0.3*mb.max()))[0]
        lm['lid'+sd] = int(top)
        lm['lidLow'+sd] = int(low[np.argmin(np.linalg.norm(V[low]-shut, axis=1))])
        if 'balls' in M:
            # the hero's margins roll in towards the ball, and its fit counts them as the socket's lining
            # (interior): there the margins are the outer skin that lies on the ball, straight over and under
            # the middle of the iris
            e = M['balls'][sd]
            r = V-e['c']
            on = ok & (np.abs(np.linalg.norm(r, axis=1)-e['rmax']) < 0.15*e['rmax']) & (r @ fr['fwd'] > 0.6*e['r'])
            side = np.abs(r @ fr['left']) < 0.2*e['r']
            dn = r @ fr['down']
            bl = np.linalg.norm(b, axis=1)
            up_ = np.nonzero(on & side & (bl > 0.4*bl.max()))[0]
            lm['lid'+sd] = int(up_[np.argmax(dn[up_])])
            lo_ = np.nonzero(on & side & (bl < 0.2*bl.max()) & (dn > dn[lm['lid'+sd]]))[0]
            lm['lidLow'+sd] = int(lo_[np.argmin(dn[lo_])])
        cs = np.argsort(-np.linalg.norm(S['mouthSmile'+sd], axis=1))[:3]
        lm['mouth'+sd] = int(cs[np.argmin(np.linalg.norm(V[cs]-V[cs].mean(0), axis=1))])
    return lm


def frame_and_landmarks(M, like=None, sim=None):
    """The frame and landmarks of M. With like (the hero's frame) and sim (s, R, t: M onto the hero), M's
    landmarks are found in the hero's axes: faces lean differently against the plane of their eyes and mouth
    corners, and the chin and the lips stand out of a profile only seen from one side."""
    fr = frame(M)
    if like is not None:
        s, R, t = sim
        fr = dict(fr, down=R.T @ like['down'], fwd=R.T @ like['fwd'], left=R.T @ like['left'],
                  mc=R.T @ (like['mc']-t)/s, u=like['u']/s, h=like['h']/s)
    fr['lm_v'] = landmarks(M, fr)
    fr['lm'] = {n: M['V'][i] for n, i in fr['lm_v'].items()}
    return fr


def old_landmarks(M, fr):
    """The landmarks of the first transfer (2026-11-09), kept to show what they did: the lips where the pucker moves
    most (on Mark that is the inner face of the lips), the lids where the blink moves most, the chin the front-most
    point of a band under the mouth."""
    V = M['V']
    ok = M.get('valid', np.ones(len(V), bool))[:, None]
    S = {n: d*ok for n, d in M['S'].items()}
    u, lm = fr['u'], {}
    for sd in ('Left', 'Right'):
        lm['lid'+sd] = V[movers(S['eyeBlink'+sd], 0.85)].mean(0)
        lm['mouth'+sd] = V[np.argsort(-np.linalg.norm(S['mouthSmile'+sd], axis=1))[:3]].mean(0)
    rel = V-fr['mc']
    mid = np.abs(rel @ fr['left']) < 0.04*u
    dn, fw = rel @ fr['down'], rel @ fr['fwd']
    pk = np.linalg.norm(S['mouthPucker'], axis=1)

    def most(sel, name, by):
        lm[name] = V[np.nonzero(sel)[0][np.argmax(by[sel])]]
    most(mid & (dn < 0) & (dn > -0.35*u), 'lipUpper', pk)
    most(mid & (dn > 0) & (dn < 0.4*u), 'lipLower', pk)
    most(mid & (dn > 0.55*u) & (dn < 1.3*u), 'chin', fw)
    a, b = (lm['lidLeft']+lm['lidRight'])/2, lm['lipUpper']
    ab = unit(b-a)
    off = (V-a)-np.outer((V-a) @ ab, ab)
    band = mid & ((V-a) @ ab > 0.25*np.linalg.norm(b-a)) & ((V-a) @ ab < 0.9*np.linalg.norm(b-a))
    most(band, 'noseTip', np.linalg.norm(off, axis=1)*np.sign(off @ fr['fwd']))
    return lm


# ---- registration ---------------------------------------------------------------------------------------------
def tps(P, D, X, smooth=0.0):
    """The thin-plate (biharmonic in 3D: phi(r) = r) map that moves the points P by D, evaluated at X."""
    n = len(P)
    K = np.linalg.norm(P[:, None]-P[None], axis=2)+smooth*np.eye(n)
    A = np.c_[np.ones(n), P]
    L = np.block([[K, A], [A.T, np.zeros((4, 4))]])
    w = np.linalg.solve(L, np.vstack([D, np.zeros((4, 3))]))
    return np.linalg.norm(X[:, None]-P[None], axis=2) @ w[:n]+np.c_[np.ones(len(X)), X] @ w[n:]


def local_rbf(P, D, X, radius):
    """A map that moves the points P by D and fades to nothing radius away from them (Wendland's C2 kernel)."""
    def k(A, B):
        r = np.minimum(np.linalg.norm(A[:, None]-B[None], axis=2)/radius, 1)
        return (1-r)**4*(4*r+1)
    return k(X, P) @ np.linalg.solve(k(P, P)+1e-6*np.eye(len(P)), D)


SMOOTH_LIDS, SLIDE, GUARD, INNER_LIPS = True, True, True, True   # switches for checks
SMOOTH_STEPS = 50
LIP_EDGE = 0.3                   # interior near the mouth facing forward more than this is the lips' free edge
GUARD_MOVED = []                 # (vertices, largest push, m) per shape lid_guard() had to move
# Amberg's schedule: stiffness, landmark weight, normal weight, iterations (trimesh.registration.nricp_amberg)
# (the landmarks held at 3 to the end: the skin still slides into place, local_rbf() closes the rest)
STEPS = [[20, 3, 0.5, 10], [5, 3, 0.5, 10], [2, 3, 0.5, 10], [0.8, 3, 0.0, 10], [0.3, 3, 0.0, 10], [0.1, 3, 0.0, 10],
         [0.05, 3, 0.0, 10], [0.02, 3, 0.0, 10], [0.01, 3, 0.0, 10]]


def register(hero, src, log=print):
    """The source onto the hero by a similarity of the landmarks, then the hero's outer skin onto the source's: a
    thin-plate map that takes the hero's landmarks onto the source's, then Amberg's non-rigid ICP that keeps holding
    them. Returns what carry() needs and the landmark distances before and after."""
    fh = frame_and_landmarks(hero)
    if fh['hand'] != frame(src)['hand']:
        src = dict(src, S=swap_sides(src['S']))
    # the source's landmarks in the hero's axes: a start by the landmarks of the first transfer, then twice by these
    o_h, o_s = old_landmarks(hero, fh), old_landmarks(src, frame(src))
    s, R, t = similarity(np.array([o_s[n] for n in sorted(o_h)]), np.array([o_h[n] for n in sorted(o_h)]))
    names = sorted(fh['lm'])
    LH = np.array([fh['lm'][n] for n in names])
    for _ in range(3):
        fs = frame_and_landmarks(src, like=fh, sim=(s, R, t))
        LS = np.array([fs['lm'][n] for n in names])
        s, R, t = similarity(LS, LH)
    VS = src['V'] @ (s*R).T+t
    DS = {n: d @ (s*R).T for n, d in src['S'].items()}
    LT = LS @ (s*R).T+t                                          # the source's landmarks in the hero's frame
    res = np.linalg.norm(LT-LH, axis=1)
    log('similarity: scale %.4f, landmark residual mean %.1f mm, max %.1f mm (%s)'
        % (s, res.mean()*1000, res.max()*1000, names[int(res.argmax())]))

    V, T = hero['V'], hero['T']
    keepf = ~hero['interior'][T].any(1)
    outer_v = np.unique(T[keepf])
    pos_o = -np.ones(len(V), int)
    pos_o[outer_v] = np.arange(len(outer_v))
    u = fh['u']                                                  # solved in units of the eye distance
    lm_o = np.array([pos_o[fh['lm_v'][n]] for n in names])
    assert (lm_o >= 0).all(), 'a landmark of the hero is not on its outer skin'
    W0 = V[outer_v]+tps(LH/u, (LT-LH)/u, V[outer_v]/u, smooth=1e-3)*u
    sub_u = trimesh.Trimesh(W0/u, pos_o[T[keepf]], process=False)
    steps = STEPS
    Q = trimesh.registration.nricp_amberg(sub_u, trimesh.PointCloud(VS/u), source_landmarks=lm_o,
                                          target_positions=LT/u, steps=steps, distance_threshold=0.3,
                                          use_faces=False, use_vertex_normals=True)*u
    # what the ICP left at the landmarks (a few mm where the source's skin pulls against them) closed by one more
    # smooth map: the skin round them follows, it does not leave the surface by more than that
    icp = {n: float(np.linalg.norm(Q[lm_o[i]]-LT[i])*1000) for i, n in enumerate(names)}
    log('landmarks after the ICP, mm:', {n: round(v, 1) for n, v in icp.items()})
    Q = Q+local_rbf(Q[lm_o], LT-Q[lm_o], Q, 0.25*u)
    dist = cKDTree(VS).query(Q)[0]
    face_zone = np.linalg.norm(V[outer_v]-(fh['mc']+(fh['eye']['Left']+fh['eye']['Right'])/2)/2, axis=1) < 1.2*u
    if 'balls' in hero:                          # the lids were interior in the first transfer: its 1.9 mm left them out
        lids = np.min([np.linalg.norm(V[outer_v]-e['c'], axis=1)-e['rmax'] for e in hero['balls'].values()], 0) < 0.012
        log('  face without the lids: median %.2f mm' % (np.median(cKDTree(VS).query(Q[face_zone & ~lids])[0])*1000))
    after = {n: float(np.linalg.norm(Q[lm_o[i]]-LT[i])*1000) for i, n in enumerate(names)}
    # the same distances for the landmarks of the first transfer and its registration, for the table of the report
    log('landmarks after the registration, mm:', {n: round(v, 1) for n, v in after.items()})
    log('registered: distance to the source skin median %.2f mm, 95%% %.2f mm; face %.2f / %.2f mm'
        % (np.median(dist)*1000, np.percentile(dist, 95)*1000, np.median(dist[face_zone])*1000,
           np.percentile(dist[face_zone], 95)*1000))
    return dict(fh=fh, fs=fs, src=src, s=s, R=R, t=t, VS=VS, DS=DS, Q=Q, outer_v=outer_v, names=names,
                landmark_after_mm=after, landmark_icp_mm=icp, landmark_similarity_mm=dict(zip(names, (res*1000).tolist())),
                registered_median_mm=float(np.median(dist)*1000), registered_p95_mm=float(np.percentile(dist, 95)*1000),
                face_median_mm=float(np.median(dist[face_zone])*1000),
                face_p95_mm=float(np.percentile(dist[face_zone], 95)*1000))


def halves(S, Vx, lm, fr):
    """Lower half of the mouth: what goes with the chin in jawOpen (over 40 % of its shift; the upper lip's edge moves
    too, less). Upper lids: what lies above the line through the middle of the eye's opening (between the margins'
    landmarks; by the blink, the fold over the hero's lid went with the lid and tore from its neighbours)."""
    m = np.linalg.norm(S['jawOpen'], axis=1)
    jaw = m > 0.4*m[int(np.argmin(np.linalg.norm(Vx-lm['chin'], axis=1)))]
    r = Vx-fr['mc']
    up = np.zeros(len(Vx), bool)
    for sd, sg in (('Left', 1), ('Right', -1)):
        mid = ((lm['lid'+sd]+lm['lidLow'+sd])/2-fr['mc']) @ fr['down']
        up |= ((r @ fr['left'])*sg > 0) & (r @ fr['down'] < mid)
    return jaw, up


def correspondence(hero, rg, K=4):
    """For every outer hero vertex the K nearest source points to its registered place; mouth and lids match only
    within their own half."""
    fh, Q, VS, outer_v = rg['fh'], rg['Q'], rg['VS'], rg['outer_v']
    jh, lh = halves(hero['S'], hero['V'], fh['lm'], fh)
    js, ls = halves(rg['DS'], VS, {n: VS[i] for n, i in rg['fs']['lm_v'].items()}, fh)
    Vo = hero['V'][outer_v]
    near_mouth = np.linalg.norm(Vo-fh['mc'], axis=1) < 0.7*fh['u']
    near_eye = np.minimum(*[np.linalg.norm(Vo-fh['eye'][k], axis=1) for k in ('Left', 'Right')]) < 0.45*fh['u']
    idx = np.zeros((len(Vo), K), int)
    wts = np.zeros((len(Vo), K))
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
    return idx, wts, (jh, lh, js, ls)


def ball_depth(e, P, R=None):
    """How deep the points are inside an eyeball (its convex hull, turned by R about its centre), m; < 0 outside."""
    from scipy.spatial import ConvexHull
    E = e['co'] if R is None else (e['co']-e['c']) @ R.T+e['c']
    q = ConvexHull(E).equations
    return -(P @ q[:, :3].T+q[:, 3]).max(1)


def lid_guard(hero, d, gap=0.0003):
    """No skin into the eyeballs (the socket's lining behind the lids is not seen and not held): a vertex of the
    skin that is outside a ball at rest and that a shape would bring
    nearer to it than gap (or, if it lies nearer at rest, nearer than at rest) is moved back out along the ray from the
    ball's centre. A safety net: what it moves is counted in GUARD_MOVED (head_shapes.py reports it)."""
    V = hero['V']
    P = V+d
    for e in hero['balls'].values():
        near = np.nonzero(hero['valid'] & (np.linalg.norm(V-e['c'], axis=1) < e['rmax']+0.02))[0]   # the skin
        d0 = ball_depth(e, V[near])
        for _ in range(4):
            d1 = ball_depth(e, P[near])
            bad = (d1 > np.maximum(-gap, d0)) & (d0 < 0)
            if not bad.any():
                break
            i = near[bad]
            ray = P[i]-e['c']
            ray /= np.linalg.norm(ray, axis=1, keepdims=True)
            P[i] += ray*(d1[bad]-np.maximum(-gap, d0[bad]))[:, None]
    moved = np.linalg.norm(P-V-d, axis=1)
    GUARD_MOVED.append((int((moved > 1e-6).sum()), float(moved.max())))
    return P-V


def lid_margins(hero, fr):
    """The margins of the lids: per eye and per 1 mm across it, the lowest skin vertex of the upper lid and the
    highest of the lower one that lie on the ball (within 3.5 mm of it, in front of its centre), split at the middle
    of the opening (the lid landmarks). The fold over the hero's upper lid is a layer of its own above them."""
    V = hero['V']
    out = []
    for sd, e in hero['balls'].items():
        r = V-e['c']
        cand = np.nonzero(hero['valid'] & (-ball_depth(e, V) < 0.0035) & (r @ fr['fwd'] > 0.5*e['r']))[0]
        x, y = r[cand] @ fr['left'], r[cand] @ fr['down']
        mid = ((V[fr['lm_v']['lid'+sd]]+V[fr['lm_v']['lidLow'+sd]])/2-e['c']) @ fr['down']
        for b in np.unique(np.floor(x/0.001)):
            col = (np.floor(x/0.001) == b)
            up, lo = col & (y < mid), col & (y >= mid)
            if up.any():
                out.append(cand[up][np.argmax(y[up])])
            if lo.any():
                out.append(cand[lo][np.argmin(y[lo])])
    return np.array(out)


def smooth_lids(hero, d, w, nbr, margin, steps=None, hold=(0.001, 0.003)):
    """The carried shift smoothed over the mesh around the eyes (weight w). Mark's lid has its margin and the
    margin's back face next to each other moving apart, and neighbouring vertices of ours that found one or the other
    tore the lid. The margins of the lids (lid_margins()) are smoothed only along themselves and then held - fully up
    to hold[0] from them, less and less to hold[1] - so the lids open and close as far as Mark's (averaged with the
    skin over them, eyeWide opened the eye half as far); the rest round the eyes is averaged towards them, and the lid
    moves as one sheet. Holding all the skin that lies on the ball held the hero's lid under its fold too, and the
    fold opened a slit when the brows rose."""
    steps = SMOOTH_STEPS if steps is None else steps
    V = hero['V']
    dist = cKDTree(V[margin]).query(V)[0]
    x = np.clip((dist-hold[0])/(hold[1]-hold[0]), 0, 1)
    h = np.where(hero['valid'], 1-x*x*(3-2*x), 0.0)[:, None]
    strip = h[:, 0] > 0

    def average(s, edges, n):
        cnt = np.maximum(np.bincount(edges[:, 0], minlength=len(s)).astype(float)[:, None], 1)
        for _ in range(n):
            m = np.zeros_like(s)
            np.add.at(m, edges[:, 0], s[edges[:, 1]])
            s = 0.5*s+0.5*m/cnt
        return s
    held = average(d.copy(), nbr[strip[nbr[:, 0]] & strip[nbr[:, 1]]], 10)    # along the margins only
    s = h*held+(1-h)*d
    cnt = np.maximum(np.bincount(nbr[:, 0], minlength=len(d)).astype(float)[:, None], 1)
    for _ in range(steps):
        m = np.zeros_like(s)
        np.add.at(m, nbr[:, 0], s[nbr[:, 1]])
        s = h*held+(1-h)*(0.5*s+0.5*m/cnt)
    return d+w[:, None]*(s-d)


def slide_on_balls(hero, d, near=0.0015, far=0.004):
    """The lid that lies on a ball slides over it: a skin vertex up to `near` off a ball at rest keeps its distance to
    the ball in every shape, fading out by `far` (Mark's lids lie on Mark's balls, carried onto ours they would sink
    into them or lift off where the two balls' curves differ)."""
    V = hero['V']
    P = V+d
    for e in hero['balls'].values():
        idx = np.nonzero(hero['valid'] & (np.linalg.norm(V-e['c'], axis=1) < e['rmax']+far+0.005))[0]
        d0 = ball_depth(e, V[idx])
        x = np.clip((-d0-near)/(far-near), 0, 1)
        w = np.where(d0 < 0, 1-x*x*(3-2*x), 0.0)
        sel = w > 0
        idx, d0, w = idx[sel], d0[sel], w[sel]
        for _ in range(3):
            d1 = ball_depth(e, P[idx])
            ray = P[idx]-e['c']
            ray /= np.linalg.norm(ray, axis=1, keepdims=True)
            P[idx] += ray*(w*(d1-d0))[:, None]
    return P-V


def carry(hero, rg, log=print):
    """Every shape of the source on the hero's face: (4) the outer skin takes the source's shift at its corresponding
    points; interior vertices (mouth bag, vestibule, the lips' inner faces) keep their own motion plus the correction
    of the outer vertices that moved like them in the hero's shape (near in place and in shift), so the inside of
    the upper lip follows the upper lip even where the lips touch, and near the mouth the lips' insides go with the
    nearest outer skin of their own lip; the lids take the source's shift like the rest of the face (the brows, the squint and
    the wide eyes from one source: no seam on the upper lid), smoothed round the eyes with their margins held
    (smooth_lids()), sliding over the balls (slide_on_balls()), lid_guard() a safety net. Blink and gaze stay the
    hero's. (5) Brows and lashes follow the skin under them, lower
    teeth and tongue follow the chin."""
    V, h = hero['V'], hero['h']
    outer_v = rg['outer_v']
    idx, wts, _ = correspondence(hero, rg)
    inner = np.nonzero(~np.isin(np.arange(len(V)), outer_v))[0]
    sock = np.min([np.linalg.norm(V[inner]-e['c'], axis=1)-e['rmax'] for e in hero['balls'].values()], axis=0) < 0.012
    off = np.min([np.linalg.norm(V-e['c'], axis=1)-e['rmax'] for e in hero['balls'].values()], axis=0)
    x = np.clip((0.02-off)/0.01, 0, 1)
    lid_w = x*x*(3-2*x)                          # 1 up to 10 mm off a ball, 0 from 20 mm
    E = np.vstack([hero['T'][:, [0, 1]], hero['T'][:, [1, 2]], hero['T'][:, [2, 0]]])
    nbr = np.unique(np.vstack([E, E[:, ::-1]]), axis=0)
    margin = lid_margins(hero, rg['fh'])
    lips = inner[np.linalg.norm(V[inner]-rg['fh']['mc'], axis=1) < 0.35*rg['fh']['u']]
    # the lips' free edge is interior in the fit too, but it faces forward and is seen: it keeps the rule above (by
    # the MPFB shift), which closes the lips where Mark closes them; the rule by place is for the inside that faces
    # into the mouth
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from head_glb import normals
    lips = lips[(normals(V, hero['T'])[lips] @ rg['fh']['fwd']) < LIP_EDGE]
    # the lips' insides: the nearest outer skin of the same lip half (what goes with the chin in jawOpen)
    jaw = halves(hero['S'], V, rg['fh']['lm'], rg['fh'])[0]
    lip_k, lip_w = np.zeros((len(lips), 4), int), np.zeros((len(lips), 4))
    for half in (True, False):
        pool = np.nonzero(jaw[outer_v] == half)[0]          # positions in outer_v
        sel = jaw[lips] == half
        dk, jk = cKDTree(V[outer_v[pool]]).query(V[lips[sel]], k=4)
        wk = 1/np.maximum(dk, 1e-4)**2
        lip_k[sel], lip_w[sel] = pool[jk], wk/wk.sum(1, keepdims=True)
    x = np.clip((np.linalg.norm(V[lips]-rg['fh']['mc'], axis=1)-0.25*rg['fh']['u'])/(0.1*rg['fh']['u']), 0, 1)
    lip_b = 1-x*x*(3-2*x)                                     # 1 up to 0.25 eye distances off the mouth, 0 by 0.35
    # round the eyes the smoothing also joins vertices near in space: the hero's fold over the upper lid is a layer
    # of its own, joined to the lid under it only deep in the crease, and the two moved apart and opened a slit
    near_eye = np.nonzero(lid_w > 0)[0]
    pairs = cKDTree(V[near_eye]).query_pairs(0.0025, output_type='ndarray')
    pairs = near_eye[pairs]
    nbr = np.unique(np.vstack([nbr, pairs, pairs[:, ::-1]]), axis=0)
    new = {}
    for n in ARKIT:
        if n in KEEP or n not in rg['DS']:
            new[n] = hero['S'][n]
            continue
        d = hero['S'][n].copy()
        d[outer_v] = (rg['DS'][n][idx]*wts[..., None]).sum(1)
        corr = d[outer_v]-hero['S'][n][outer_v]
        feat = lambda i: np.hstack([V[i], 3*hero['S'][n][i]])   # noqa: E731
        dk, jk = cKDTree(feat(outer_v)).query(feat(inner), k=4)
        wk = 1/np.maximum(dk, 1e-4)
        wk /= wk.sum(1, keepdims=True)
        d[inner] = hero['S'][n][inner]+(corr[jk]*wk[..., None]).sum(1)
        # the socket's lining is the back of the lids: it goes with the nearest skin, the lid's margin, else a lid
        # that rises leaves it standing in the opening
        dk, jk = cKDTree(V[outer_v]).query(V[inner[sock]], k=4)
        wk = 1/np.maximum(dk, 1e-4)**2
        wk /= wk.sum(1, keepdims=True)
        d[inner[sock]] = hero['S'][n][inner[sock]]+(corr[jk]*wk[..., None]).sum(1)
        if INNER_LIPS:
            # the inside of each lip goes with the nearest outer skin of its own lip: its own MPFB shift plus their
            # correction, by place (not by the MPFB shift as above), blended in over the mouth. By the MPFB shift the
            # correction differed from vertex to vertex and stood up as teeth in the funnel; smoothing it (round 2)
            # flattened the jaw's fall-off from the lip to the deeper vestibule - the inside of the lower lip went
            # down three quarters as far as its lip and stood up in front of the teeth - and let the deep mouth's
            # correction thin out the lip's (mouthClose: MPFB's lifts the lower lip 37 mm, Mark's 1.3; the inside
            # stayed 9 mm over the lip)
            c = (corr[lip_k]*lip_w[..., None]).sum(1)
            d[lips] = hero['S'][n][lips]+lip_b[:, None]*c+(1-lip_b[:, None])*(d[lips]-hero['S'][n][lips])
        d = smooth_lids(hero, d, lid_w, nbr, margin) if SMOOTH_LIDS else d
        d = slide_on_balls(hero, d) if SLIDE else d
        new[n] = lid_guard(hero, d) if GUARD else d
    out = {'face|'+n: V+new[n] for n in ARKIT}
    # brows and lashes keep their own shape of the hero plus the change of the skin under them: the mean change of
    # the nearest skin vertices, a shift without a turn (riding on one skin triangle turned the lashes about their
    # roots when a lid rose with the brows, and they stood up over the eye)
    skin = np.nonzero(hero['valid'])[0]
    for part in ('brows', 'lashes'):
        P0 = h[part+'|co']
        dk, jk = cKDTree(V[skin]).query(P0, k=6)
        wk = 1/np.maximum(dk, 1e-4)**2
        wk /= wk.sum(1, keepdims=True)
        for n in ARKIT:
            k = part+'|sk|'+n
            if k in h:
                ch = (new[n]-hero['S'][n])[skin]
                out[part+'|'+n] = h[k] if n in KEEP else h[k]+(ch[jk]*wk[..., None]).sum(1)
    chin_i = rg['fh']['lm_v']['chin']
    ratio = {}
    for n in JAW:
        old = np.linalg.norm(hero['S'][n][chin_i])
        ratio[n] = float(np.linalg.norm(new[n][chin_i])/old) if old > 1e-6 else 1.0
    for part in ('teeth', 'tongue'):
        P0 = h[part+'|co']
        for n in ARKIT:
            k = part+'|sk|'+n
            if k in h:
                out[part+'|'+n] = P0+(h[k]-P0)*ratio.get(n, 1.0)
    mag = {n: [float(np.linalg.norm(hero['S'][n], axis=1).max()*1000), float(np.linalg.norm(new[n], axis=1).max()*1000)]
           for n in ARKIT if n not in KEEP}
    for n in ('jawOpen', 'mouthClose', 'mouthRollLower', 'mouthPucker', 'mouthSmileLeft', 'browInnerUp'):
        log('  %-16s max shift: hero %5.1f mm, carried %5.1f mm' % (n, *mag[n]))
    log('teeth ratio', {k: round(v, 2) for k, v in ratio.items()})
    return out, new, dict(jaw_teeth_ratio=ratio, max_shift_mm_hero_vs_new=mag)


def info(rg, extra):
    return dict(scale=float(rg['s']), landmarks=rg['names'], landmark_similarity_mm=rg['landmark_similarity_mm'],
                landmark_after_icp_mm=rg['landmark_icp_mm'], landmark_after_registration_mm=rg['landmark_after_mm'], registered_median_mm=rg['registered_median_mm'],
                registered_p95_mm=rg['registered_p95_mm'], face_median_mm=rg['face_median_mm'],
                face_p95_mm=rg['face_p95_mm'], **extra)


# ---- 6. the solver's shapes -----------------------------------------------------------------------------------
def solver_files(hero, rg, WD, sets, K=4):
    """face-shapes.md, "Решатель под наши формы": the shapes a head plays, carried onto Mark's mesh, so that
    Audio2Face's own solver finds the weights for them. Every Mark point takes the shift of the nearest registered
    skin points of the hero (mouth and lids only within their own half), turned back into Mark's frame and cm.
    mpfb is the head as built; ict and mark are <set>.npz written by earlier runs (mark there is the round-trip)."""
    fh, Q, VS, outer_v, s, R = rg['fh'], rg['Q'], rg['VS'], rg['outer_v'], rg['s'], rg['R']
    V = hero['V']
    _, _, (jh, lh, js, ls) = correspondence(hero, rg, K)
    mk = np.load(os.path.join(FS, 'mark/bs_skin_Mark.npz'))
    nm_s = np.linalg.norm(VS-fh['mc'], axis=1) < 0.7*fh['u']
    ne_s = np.minimum(*[np.linalg.norm(VS-fh['eye'][k], axis=1) for k in ('Left', 'Right')]) < 0.45*fh['u']
    ridx = np.zeros((len(VS), K), int)
    rw = np.zeros((len(VS), K))
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
    np.savez(os.path.join(WD, 'solver', 'reg.npz'), Q=Q, outer_v=outer_v, VS=VS, s=s, R=R, t=rg['t'])
    for name in sets:
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


def main(argv):
    a = list(argv)
    reg = REG
    if '--reg' in a:
        i = a.index('--reg')
        reg = os.path.expanduser(a[i+1])
        del a[i:i+2]
    SRC, WD = a[0], os.path.expanduser(a[1])
    hero = hero_from_parts(np.load(os.path.join(WD, 'hero.npz')), reg)
    src = load_mark() if SRC == 'mark' else load_ict()
    rg = register(hero, src)
    out, new, extra = carry(hero, rg)
    np.savez_compressed(os.path.join(WD, SRC+'.npz'), **out)
    json.dump(dict(src=SRC, **info(rg, extra)), open(os.path.join(WD, SRC+'.json'), 'w'), indent=1)
    if len(a) > 2:
        assert SRC == 'mark', 'the solver works on Mark'
        solver_files(hero, rg, WD, a[2].split(','))


if __name__ == '__main__':
    main(sys.argv[1:])
