# Mark's face shapes (Audio2Face v3.0) on a head of export_test_head.py, written into a copy of its GLB
# (face-shapes.md, "Формы Mark в игре"). The build step "shapes" of scripts/content/npc/build_head.py runs it.
#   ~/.local/share/gothic-ai/mpfb/venv/bin/python head_shapes.py <head.glb> <reg.npz> <out.glb> [--json <info.json>]
#       [--face <head.face.json> <out.face.json>]
# reg.npz is the fit the head was exported from (fit/reg: <work>/round/reg.npz of a build). Every ARKit shape but the
# blink, the gaze and tongueOut is replaced on the face, brows, lashes, teeth and tongue; the visemes stay. --json
# writes what was measured: the landmarks before and after the registration, the lids against the balls.
# --face writes the face profile for Mark's shapes: the head's own (MPFB) profile with its weights re-scaled by
# mark_profile.py for this head (mpfb/mark.face.json is that of the hero 3a), and reach.md next to the --json (or the
# profile): MPFB's and Mark's reach per weight set and zone. Mark's data are NVIDIA's (NVIDIA Open Model License): the head is not for git.
import json
import os
import sys

import numpy as np
from scipy.spatial import ConvexHull

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import head_glb  # noqa: E402
import transfer as X  # noqa: E402
import mark_profile  # noqa: E402

PROFILE = os.path.join(HERE, '..', 'mpfb', 'test-head.face.json')


def rot(axis, deg):
    a = np.radians(deg)
    k = axis/np.linalg.norm(axis)
    K = np.array([[0, -k[2], k[1]], [k[2], 0, -k[0]], [-k[1], k[0], 0]])
    return np.eye(3)+np.sin(a)*K+(1-np.cos(a))*K @ K


def lids_check(hero, fr, shapes, limits, lids):
    """Face vertices inside an eyeball (its convex hull, turned to the gaze) that are outside it at rest: for every
    shape at 1, the expressions that move the lids most at 1 together, and the gaze at the profile's limits with the
    lid shapes that follow it (eyes.lids) - alone and with those expressions. Returns the worst depth per case, mm."""
    V = hero['V']
    out = {}
    hulls = {}
    for sd, e in hero['balls'].items():
        hulls[sd] = e

    def inside(P, sd, R):
        e = hulls[sd]
        E = (e['co']-e['c']) @ R.T+e['c']
        h = ConvexHull(E)
        d = P @ h.equations[:, :3].T+h.equations[:, 3]             # < 0 inside every face
        return -d.max(1)                                             # depth inside, > 0 inside
    rest = {sd: inside(V, sd, np.eye(3)) for sd in hulls}
    # the skin only: the socket's lining lies against the ball behind the lids and is not seen
    near = {sd: (np.linalg.norm(V-e['c'], axis=1) < e['rmax']+0.01) & hero['valid'] for sd, e in hulls.items()}

    def depth(W, R=None):
        P = V+sum(w*shapes[n] for n, w in W.items())
        worst = 0.0
        for sd in hulls:
            dd = inside(P[near[sd]], sd, np.eye(3) if R is None else R[sd])
            new = (dd > 0) & (rest[sd][near[sd]] <= 0)
            worst = max(worst, float(dd[new].max()*1000) if new.any() else 0.0)
        return worst
    for n in X.ARKIT:
        out[n] = depth({n: 1.0})
    expr = {'squint+browDown': {'eyeSquintLeft': 1, 'eyeSquintRight': 1, 'browDownLeft': 1, 'browDownRight': 1},
            'wide+browUp': {'eyeWideLeft': 1, 'eyeWideRight': 1, 'browInnerUp': 1, 'browOuterUpLeft': 1,
                            'browOuterUpRight': 1},
            'blink+squint': {'eyeBlinkLeft': 1, 'eyeBlinkRight': 1, 'eyeSquintLeft': 1, 'eyeSquintRight': 1},
            'cheekSquint+smile': {'cheekSquintLeft': 1, 'cheekSquintRight': 1, 'mouthSmileLeft': 1,
                                  'mouthSmileRight': 1}}
    for k, W in expr.items():
        out[k] = depth(W)
    # the gaze at each limit, each eye turned so that its front goes that way (out: away from the nose), with the
    # lid shapes the profile lets follow that direction, alone and with the expressions
    dirs = {'up': -fr['down'], 'down': fr['down']}
    for name in ('up', 'down', 'in', 'out'):
        R = {}
        for sd in hulls:
            out_ax = fr['left'] if sd == 'Left' else -fr['left']
            to = dirs.get(name, out_ax if name == 'out' else -out_ax)
            ax = np.cross(fr['fwd'], to)                     # turns fwd towards "to"
            R[sd] = rot(ax, limits[name])
        W = {f['target'].replace('{side}', sd): f['max'] for f in lids.get(name, []) for sd in ('Left', 'Right')}
        out['gaze %s' % name] = depth(W, R)
        for k, E in expr.items():
            out['gaze %s + %s' % (name, k)] = depth({**W, **E}, R)
    return out


def main(argv):
    a = list(argv)
    js = face = None
    if '--json' in a:
        i = a.index('--json')
        js = a[i+1]
        del a[i:i+2]
    if '--face' in a:
        i = a.index('--face')
        face = a[i+1:i+3]
        del a[i:i+3]
    glb, reg, out = (os.path.expanduser(x) for x in a[:3])
    h = head_glb.read(glb)
    hero = X.hero_from_parts(h, reg)
    rg = X.register(hero, X.load_mark())
    X.GUARD_MOVED.clear()
    res, new, extra = X.carry(hero, rg)
    moved = max(X.GUARD_MOVED, key=lambda m: m[1], default=(0, 0.0))
    print('lid_guard moved %d vertices in all, at most %.3f mm (the transfer itself keeps the lids out of the balls '
          'when it is 0)' % (sum(m[0] for m in X.GUARD_MOVED), moved[1]*1000))
    extra['lid_guard_moved'] = {'vertices': sum(m[0] for m in X.GUARD_MOVED), 'max_mm': moved[1]*1000}
    n = head_glb.write(glb, res, out)
    print('shapes set: %d -> %s' % (n, out))
    prof = json.load(open(face[0] if face else PROFILE))
    lim = prof['eyes']['limits_deg']
    lids = lids_check(hero, rg['fh'], new, lim, prof['eyes']['lids'])
    before = lids_check(hero, rg['fh'], hero['S'], lim, prof['eyes']['lids'])
    bad = {k: round(v, 2) for k, v in lids.items() if v > 0.05}
    print('lids into the balls (mm, over 0.05): %s' % (bad or 'none'))
    if face:
        q, rows = mark_profile.profile(hero, new, face[0])
        json.dump(q, open(face[1], 'w'), indent=2, ensure_ascii=False)
        mark_profile.write_table(rows, os.path.join(os.path.dirname(js or face[1]), 'reach.md'))
        print('face profile ->', face[1])
    if js:
        json.dump(dict(head=glb, src='mark', **X.info(rg, extra), lids_into_balls_mm=lids,
                       lids_into_balls_mpfb_mm=before), open(js, 'w'), indent=1)


if __name__ == '__main__':
    main(sys.argv[1:])
