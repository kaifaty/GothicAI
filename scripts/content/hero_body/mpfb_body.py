"""MPFB hero body, step 1: build the man, fit his proportions to HUMANS.MDH, export landmarks.

    BLENDER_USER_RESOURCES=~/.local/share/gothic-ai/mpfb/blender \
      Blender -b -P mpfb_body.py -- <out dir> <HUMANS.MDH.json>

Writes <out>/body.blend (the man with the game_engine rig, targets applied) and <out>/landmarks.json in the
T frame of fit.py (cm, y up, faces +Z, the man's left at +X, floor y=0) plus the solved scale and targets.

Gothic's skeleton is not human anatomy: the knee sits at 60.7 cm, the hip at 100 cm, the arms are long and the hips
narrow. The body must bend where the bones bend, so MPFB's own proportion targets move his joints onto the
skeleton first; the small rest is left to the TPS of fit.py.
"""
import bpy, os, sys, json
import numpy as np
from bl_ext.user_default.mpfb.services import HumanService, TargetService

argv = sys.argv[sys.argv.index('--') + 1:]
OUT, MDH = argv[0], argv[1]
os.makedirs(OUT, exist_ok=True)

# the hero (concept 3): about 33, strong and heavy like a miner, not a bodybuilder
MACRO = dict(gender=1.0, age=0.577, muscle=float(os.environ.get('MUSCLE', 0.8)), weight=float(os.environ.get('WEIGHT', 0.7)),
             proportions=float(os.environ.get('PROPORTIONS', 0.8)), height=0.5)
REG = float(os.environ.get('REG', 4.0))   # cm of misfit worth a full target: the TPS takes the rest
# build beyond the macros (round 4, the user: legs too thin, back and triceps too wide, no trapezius, thin neck):
# working legs, plain arms, a straight back instead of a bodybuilder's V; amounts are a look, girths are solved below
STYLE = {**{f'{s}-{t}': v for s in 'lr' for t, v in (('upperarm-muscle-incr', 0.2), ('lowerarm-muscle-incr', 0.5),
                                                    ('upperarm-shoulder-muscle-incr', 0.25), ('upperleg-muscle-incr', 1.0),
                                                    ('lowerleg-muscle-incr', 0.6))},
         'torso-muscle-pectoral-incr': 0.6, 'torso-muscle-dorsi-decr': 0.3, 'torso-vshape-decr': 0.2,
         'neck-scale-horiz-incr': 0.3}
STYLE = {k: v * float(os.environ.get('STYLE', 1.0)) for k, v in STYLE.items()}
STYLE['bulge-incr'] = float(os.environ.get('BULGE', 1.0))   # a man under the braies: MPFB's base groin is smooth
TARGETS = ['upperlegs-height', 'lowerlegs-height', 'measure-upperarm-length', 'measure-lowerarm-length',
           'hip-scale-horiz', 'measure-shoulder-dist', 'measure-neck-height', 'torso-scale-vert']
# girths of a fit, strong man, game cm (the classical canon: neck ~ calf ~ relaxed upper arm + 2 cm, thigh ~ 1.5 calf,
# chest ~ 1.25 waist); MPFB's measure targets reach them, measure.py checks the exported body
GIRTH = {k: float(os.environ.get(f'G_{k.upper()}', v)) for k, v in
         (('neck', 40.5), ('upperarm', 34.5), ('thigh', 58.0), ('calf', 41.0), ('waist', 84.0), ('hips', 96.0))}
# the levers: on the limbs the whole segment gets wider and deeper ('lr-' = both sides); MPFB's measure-*-circ targets
# inflate one ring for a tape measurement and blew the thighs up like balloons (round 4a), so they stay on the neck
# and the torso only
GIRTH_TARGETS = {'neck': ['measure-neck-circ'], 'upperarm': ['lr-upperarm-scale-horiz'],
                 'thigh': ['lr-upperleg-scale-horiz', 'lr-upperleg-scale-depth'], 'calf': ['lr-lowerleg-scale-horiz'],
                 'waist': ['measure-waist-circ'], 'hips': ['measure-hips-circ']}
GREG = float(os.environ.get('GREG', 2.0))  # cm of girth misfit worth a full target
TRAPS = float(os.environ.get('TRAPS', 3.0))  # cm: the upper trapezius, from the neck to the shoulder (no MPFB target)

# --- Gothic rest joints (HUMANS.MDH), heights above the floor, cm
FLOOR = 99.95
d = json.load(open(MDH))['data']['nodes']
names = [n['name'] for n in d]
loc = [np.array(n['transform'], dtype=np.float64).reshape(4, 4).T for n in d]
glob = []
for i, n in enumerate(d):
    glob.append(loc[i] if n['parent_index'] < 0 else glob[n['parent_index']] @ loc[i])
G = {n: glob[i][:3, 3] for i, n in enumerate(names)}
dist = lambda a, b: float(np.linalg.norm(G[a] - G[b]))
GOAL = {
    'h_hip': G['BIP01 L THIGH'][1] + FLOOR, 'h_knee': G['BIP01 L CALF'][1] + FLOOR,
    'h_shoulder': G['BIP01 L UPPERARM'][1] + FLOOR, 'h_neck': G['BIP01 NECK'][1] + FLOOR,
    'h_head': G['BIP01 HEAD'][1] + FLOOR,
    'w_hip': dist('BIP01 L THIGH', 'BIP01 R THIGH'), 'w_shoulder': dist('BIP01 L UPPERARM', 'BIP01 R UPPERARM'),
    'l_upperarm': dist('BIP01 L UPPERARM', 'BIP01 L FOREARM'), 'l_forearm': dist('BIP01 L FOREARM', 'BIP01 L HAND'),
}
KEYS = list(GOAL)

# --- the man
for o in list(bpy.data.objects):
    bpy.data.objects.remove(o)
macro = TargetService.get_default_macro_info_dict()
macro.update(MACRO)
macro['race'] = {'caucasian': 1.0, 'asian': 0.0, 'african': 0.0}
bm = HumanService.create_human(macro_detail_dict=macro)
for t, v in STYLE.items():
    TargetService.load_target(bm, TargetService.target_full_path(t), weight=v)
def sides(t):
    return [f'{x}-{t[3:]}' for x in 'lr'] if t.startswith('lr-') else [t]

for t in TARGETS + [t for ts in GIRTH_TARGETS.values() for t in ts]:
    for one in sides(t):
        for s in ('incr', 'decr'):
            TargetService.load_target(bm, TargetService.target_full_path(f'{one}-{s}'), weight=0.0)
kb = bm.data.shape_keys.key_blocks
nv = len(bm.data.vertices)

def coords():
    """Mesh coordinates with the current shape key values (helpers included), metres."""
    basis = np.empty(nv * 3); kb[0].data.foreach_get('co', basis); basis = basis.reshape(-1, 3)
    out = basis.copy(); buf = np.empty(nv * 3)
    for k in kb[1:]:
        if k.value != 0.0 and not k.mute:
            k.data.foreach_get('co', buf); out += k.value * (buf.reshape(-1, 3) - basis)
    return out

def group_verts(name):
    gi = bm.vertex_groups[name].index
    return np.array([v.index for v in bm.data.vertices if any(g.group == gi and g.weight > 0.5 for g in v.groups)])

JG = {k: group_verts(f'joint-{k}') for k in
      ('l-upper-leg', 'r-upper-leg', 'l-knee', 'r-knee', 'l-ankle', 'r-ankle', 'l-shoulder', 'r-shoulder',
       'l-elbow', 'r-elbow', 'l-hand', 'r-hand', 'l-finger-3-4', 'r-finger-3-4', 'neck', 'head', 'pelvis')}

def joints(co):
    return {k: co[v].mean(0) * 100 for k, v in JG.items()}   # Blender frame, cm

BODY = None

def measures(co):
    j = joints(co)
    f = co[BODY, 2].min() * 100                   # heights above his soles, not above MPFB's origin
    n = lambda a, b: float(np.linalg.norm(j[a] - j[b]))
    return np.array([j['l-upper-leg'][2] - f, j['l-knee'][2] - f, j['l-shoulder'][2] - f, j['neck'][2] - f, j['head'][2] - f,
                     n('l-upper-leg', 'r-upper-leg'), n('l-shoulder', 'r-shoulder'),
                     n('l-shoulder', 'l-elbow'), n('l-elbow', 'l-hand')])

def set_targets(names, w):
    for t, x in zip(names, w):
        for one in sides(t):
            kb[f'{one}-incr'].value = max(x, 0.0)
            kb[f'{one}-decr'].value = max(-x, 0.0)

def set_w(w):
    set_targets(TARGETS, w)

BODY = group_verts('body')
in_body = np.zeros(nv, bool); in_body[BODY] = True
TRI = np.array([(p.vertices[0], p.vertices[k], p.vertices[k + 1]) for p in bm.data.polygons
                if in_body[list(p.vertices)].all() for k in range(1, len(p.vertices) - 1)])

# --- girths, as a tape lies over the skin: the convex hull of a cross-section (Blender frame, cm)
def section(P, point, normal):
    d = (P - point) @ normal
    out = []
    for a, b in ((0, 1), (1, 2), (2, 0)):
        da, db = d[TRI[:, a]], d[TRI[:, b]]
        m = da * db < 0
        t = da[m] / (da[m] - db[m])
        out.append(P[TRI[m, a]] + (P[TRI[m, b]] - P[TRI[m, a]]) * t[:, None])
    return np.vstack(out)

def hull_len(q):
    """Perimeter of the convex hull of 2D points (monotone chain)."""
    pts = sorted(set(map(tuple, np.round(q, 4))))
    if len(pts) < 3:
        return 0.0
    turn = lambda o, a, b: (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])
    def chain(ps):
        h = []
        for p in ps:
            while len(h) >= 2 and turn(h[-2], h[-1], p) <= 0:
                h.pop()
            h.append(p)
        return h[:-1]
    v = np.array(chain(pts) + chain(pts[::-1]))
    return float(np.linalg.norm(v - np.roll(v, 1, 0), axis=1).sum())

def limb_girth(P, a, b, frac, radius, left_only=False):
    axis = (b - a) / np.linalg.norm(b - a); point = a + (b - a) * frac
    q = section(P, point, axis)
    r = q - point; r -= np.outer(r @ axis, axis)
    keep = np.linalg.norm(r, axis=1) < radius
    if left_only:
        keep &= q[:, 0] > 0
    q = q[keep] - point
    e1 = np.cross(axis, [0, 1.0, 0]); e1 /= np.linalg.norm(e1); e2 = np.cross(axis, e1)
    return hull_len(np.c_[q @ e1, q @ e2])

def torso_girth(P, z, xmax):
    q = section(P, np.array([0, 0, z]), np.array([0, 0, 1.0]))
    return hull_len(q[np.abs(q[:, 0]) < xmax][:, :2])

def girths(co):
    P = co * 100; j = joints(co); pz = j['pelvis'][2]; xmax = j['l-shoulder'][0] + 2.0
    return np.array([
        limb_girth(P, j['neck'], j['head'], 0.3, 9.0),
        limb_girth(P, j['l-shoulder'], j['l-elbow'], 0.55, 9.0),
        limb_girth(P, j['l-upper-leg'], j['l-knee'], 0.3, 11.0, left_only=True),
        limb_girth(P, j['l-knee'], j['l-ankle'], 0.3, 9.0, left_only=True),
        min(torso_girth(P, z, xmax) for z in np.arange(pz + 5, pz + 28, 1.0)),
        max(torso_girth(P, z, xmax) for z in np.arange(pz - 6, pz + 5, 1.0)),    # the buttocks, above the thighs' split
    ])

def gauss_newton(names, measure, goal, reg, w, iters, fixed_scale=None):
    """(scale,) target weights clamped to [-1, 1] so that scale * measure hits the goal"""
    s = fixed_scale
    for it in range(iters):
        set_targets(names, w); m0 = measure(coords())
        if fixed_scale is None:
            s = float(m0 @ goal / (m0 @ m0))
        J = np.zeros((len(goal), len(names)))
        for i in range(len(names)):
            e = w.copy(); e[i] += 0.1 if w[i] < 0.9 else -0.1
            set_targets(names, e); J[:, i] = (measure(coords()) - m0) / (e[i] - w[i])
        r = goal - s * m0
        A = np.vstack([s * J, reg * np.eye(len(names))]); b = np.r_[r, -reg * w]
        dw, *_ = np.linalg.lstsq(A, b, rcond=None)
        w = np.clip(w + dw, -1.0, 1.0)
        print('ITER', names[0], it, 'scale', round(s, 4), 'rms cm', round(float(np.sqrt((r ** 2).mean())), 2))
    set_targets(names, w)
    return w, s

goal = np.array([GOAL[k] for k in KEYS])
w = np.zeros(len(TARGETS))
w, s = gauss_newton(TARGETS, measures, goal, REG, w, 4)            # joints onto the skeleton
GNAMES = [t for ts in GIRTH_TARGETS.values() for t in ts]; ggoal = np.array([GIRTH[k] for k in GIRTH_TARGETS])
gw, _ = gauss_newton(GNAMES, girths, ggoal, GREG, np.zeros(len(GNAMES)), 6, fixed_scale=s)   # then the girths
w, s = gauss_newton(TARGETS, measures, goal, REG, w, 2)            # the girths moved the joints a little
co = coords(); m = measures(co); g = girths(co)
print('TARGETS', dict(zip(TARGETS, np.round(w, 3).tolist())))
print('GIRTH TARGETS', dict(zip(GNAMES, np.round(gw, 3).tolist())))
print('FIT', {k: (round(float(s * a), 1), round(float(gl), 1)) for k, a, gl in zip(KEYS, m, goal)})
print('GIRTHS', {k: (round(float(s * a), 1), round(float(gl), 1)) for k, a, gl in zip(GIRTH_TARGETS, g, ggoal)})

# --- the upper trapezius: MPFB has no target for it. A smooth bulge along each neck-to-shoulder line, on the top and
# back of the slope only (not over the collarbones), pushed along the skin normal, as its own shape key
if TRAPS > 0:
    P = co * 100; j = joints(co)
    n = np.zeros_like(P)
    a, b, c = P[TRI[:, 0]], P[TRI[:, 1]], P[TRI[:, 2]]
    fn = np.cross(b - a, c - a)
    for k in range(3):
        np.add.at(n, TRI[:, k], fn)
    n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
    up, back = np.array([0, 0, 1.0]), np.array([0, 1.0, 0])     # Blender: Z up, front -Y
    disp = np.zeros_like(P)
    for side in ('l', 'r'):
        N, S = j['neck'], j[f'{side}-shoulder']
        L = float(np.linalg.norm(S - N)); u = (S - N) / L
        C = N + (S - N) * 0.45 + up * 4.0 + back * 2.0
        d = P - C
        du = d @ u; dv = np.linalg.norm(d - np.outer(du, u), axis=1)
        bump = np.exp(-(du / (0.38 * L)) ** 2 - (dv / 4.5) ** 2)
        facing = np.clip(n @ (up * 0.8 + back * 0.6), 0, 1)
        disp += (TRAPS * bump * facing)[:, None] * n
    disp[~in_body] = 0
    basis = np.empty(nv * 3); kb[0].data.foreach_get('co', basis)
    key = bm.shape_key_add(name='trapezius', from_mix=False)
    key.data.foreach_set('co', (basis.reshape(-1, 3) + disp / 100).ravel()); key.value = 1.0
    kb = bm.data.shape_keys.key_blocks
    co = coords()
    print('TRAPEZIUS max cm', round(float(np.linalg.norm(disp, axis=1).max()), 2))

# --- landmarks in the T frame (cm, y up, faces +Z, left at +X); feet stay where MPFB puts them
body = group_verts('body')
ground = float(co[body, 2].min() * 100)          # the targets lift the feet off the floor: stand him back on it
T = lambda p: [float(p[0]), float(p[2] - ground), float(-p[1])]
j = joints(co)
cb = co[body] * 100; cb[:, 2] -= ground
L = {'pelvis': T(j['pelvis']), 'neck': T(j['neck']), 'head': T(j['head'])}
top = cb[np.argmax(cb[:, 2])]; L['top'] = T(top)
for side, sx in (('L', 'l'), ('R', 'r')):
    L[f'hip_{side}'] = T(j[f'{sx}-upper-leg']); L[f'knee_{side}'] = T(j[f'{sx}-knee'])
    L[f'ankle_{side}'] = T(j[f'{sx}-ankle']); L[f'shoulder_{side}'] = T(j[f'{sx}-shoulder'])
    L[f'elbow_{side}'] = T(j[f'{sx}-elbow']); L[f'wrist_{side}'] = T(j[f'{sx}-hand'])
    L[f'fingertip_{side}'] = T(j[f'{sx}-finger-3-4'])
    foot = cb[((cb[:, 0] > 0) if side == 'L' else (cb[:, 0] < 0)) & (cb[:, 2] < 6)]
    L[f'toe_{side}'] = T(foot[np.argmin(foot[:, 1])]); L[f'heel_{side}'] = T(foot[np.argmax(foot[:, 1])])
json.dump({'landmarks': L, 'scale': s, 'ground_cm': ground, 'style': STYLE, 'targets': dict(zip(TARGETS, w.tolist())),
           'girth_targets': dict(zip(GNAMES, gw.tolist())), 'girths_cm': dict(zip(GIRTH_TARGETS, (s * g).tolist())),
           'trapezius_cm': TRAPS, 'macro': MACRO},
          open(os.path.join(OUT, 'landmarks.json'), 'w'), indent=1)

# --- rig after the targets, so the bones and weights follow the new joints
rig = HumanService.add_builtin_rig(bm, 'game_engine')
bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT, 'body.blend'))
print('SAVED', OUT, 'height cm', round(float(cb[:, 2].max() * s), 1))
