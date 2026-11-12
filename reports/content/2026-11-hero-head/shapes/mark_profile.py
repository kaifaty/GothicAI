# The face profile of a head with Mark's shapes (face-shapes.md, "Формы Mark в игре", "Профиль"): every weight of the
# MPFB template (mpfb/test-head.face.json) re-scaled so that the expression, cue, leak or brow move it makes moves the
# face's points as far on Mark's shapes as it does on MPFB's now. Mark's shapes move differently far at one weight
# (the jaw opens the mouth less, the brows go up twice as high).
#   ~/.local/share/gothic-ai/mpfb/venv/bin/python mark_profile.py <MPFB head.glb> <Mark head.glb> <reg.npz> \
#       <template face.json> <out face.json> [<table.md>]
# The points: brows (inner, middle, outer), the lids' margins, the cheeks, the nose's wings and tip, the mouth's
# corners, the lips and the chin of transfer.landmarks(); a zone's reach is the mean shift of its points. Per shape a
# factor: MPFB's reach over Mark's in the zone where the shape reaches furthest on MPFB (its zone), one for both sides.
# Then per weight set the shapes of each zone once more, until the set's reach there is MPFB's; a weight stays 0..1.
# The other zones show what Mark's shapes do besides (Mark's squint leaves the cheek, its press pulls the chin): that
# is the table's deviation, not corrected. (Matching all the points' shifts by least squares shrank the weights, the
# directions of the two sets differ.) The blink, the gaze
# and the visemes are the same shapes on both heads and keep their weights. speech_face plays Mark's weights on Mark's
# shapes: no cap and no scale.
import json
import os
import sys

import numpy as np
from scipy.spatial import cKDTree

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import head_glb  # noqa: E402
import transfer as X  # noqa: E402

ZONES = ('brows', 'lids', 'cheeks', 'nose', 'mouth', 'chin')
SAME = ('eyeBlink', 'eyeLook', 'viseme_', 'tongueOut')


def points(hero):
    """Vertex indices of the measuring points per zone, on the outer skin."""
    fr = X.frame_and_landmarks(hero)
    V, lm, u, h = hero['V'], fr['lm'], fr['u'], fr['h']
    skin = np.nonzero(hero['valid'])[0]
    tree = cKDTree(V[skin])
    at = lambda p: int(skin[tree.query(p)[1]])                     # noqa: E731
    z = {k: [] for k in ZONES}
    for sd, sg in (('Left', 1), ('Right', -1)):
        lid, low = lm['lid'+sd], lm['lidLow'+sd]
        for off in (-0.12, 0.0, 0.14):                             # brow over the lid: inner, middle, outer
            z['brows'].append(at(lid-0.2*h*fr['down']+off*u*sg*fr['left']+0.05*h*fr['fwd']))
        z['lids'] += [fr['lm_v']['lid'+sd], fr['lm_v']['lidLow'+sd]]
        z['cheeks'].append(at(low+0.22*h*fr['down']+0.05*u*sg*fr['left']+0.1*h*fr['fwd']))
        z['nose'].append(at(lm['noseTip']+0.06*h*fr['down']+0.18*u*sg*fr['left']-0.1*h*fr['fwd']))
        z['mouth'].append(fr['lm_v']['mouth'+sd])
    z['nose'].append(fr['lm_v']['noseTip'])
    z['mouth'] += [fr['lm_v'][n] for n in ('lipUpper', 'lipLower', 'lipContactUpper', 'lipContactLower')]
    z['chin'] += [fr['lm_v'][n] for n in ('chin', 'sulcus')]
    return {k: np.array(v) for k, v in z.items()}


def reach(S, W, pts):
    """Mean shift (mm) of each zone's points under the weights W."""
    d = sum(w*S[n] for n, w in W.items() if n in S)
    if np.isscalar(d):
        return {k: 0.0 for k in pts}
    return {k: float(np.linalg.norm(d[i], axis=1).mean()*1000) for k, i in pts.items()}


def is_weights(v):
    return isinstance(v, dict) and v and all(isinstance(x, (int, float)) for x in v.values()) and \
        all(k[:1].islower() and not k.endswith('_ms') for k in v)


def walk(node, path, out):
    """Every weight set of a profile section: {path: dict}."""
    if is_weights(node) and any(k in X.ARKIT for k in node):
        out[path] = node
    elif isinstance(node, dict):
        for k, v in node.items():
            walk(v, path+'/'+k, out)
    elif isinstance(node, list):
        for i, v in enumerate(node):
            walk(v, path+'/%d' % i, out)


def profile(hm, SK, tmpl, log=print):
    """The profile of tmpl for the hero hm (transfer.hero_from_parts) whose shapes become SK (shifts): the profile
    and the rows of the table (set, MPFB's reach per zone, Mark's)."""
    SM = hm['S']
    pts = points(hm)
    # 1. per shape; a face is near symmetric: one factor and one zone for both sides
    k, dom, A = {}, {}, {}
    for n in X.ARKIT:
        if n.startswith(SAME):
            continue
        a, b = reach(SM, {n: 1}, pts), reach(SK, {n: 1}, pts)
        A[n] = (a, b)
    for n, (a, b) in A.items():
        pair = [m for m in (n.replace('Left', 'Right'), n.replace('Right', 'Left')) if m in A]
        aa = {z: np.mean([A[m][0][z] for m in pair]) for z in ZONES}
        bb = {z: np.mean([A[m][1][z] for m in pair]) for z in ZONES}
        dom[n] = max(aa, key=aa.get)
        k[n] = float(np.clip(aa[dom[n]]/max(bb[dom[n]], 1e-6), 0.25, 4))
    prof = json.load(open(tmpl))
    sets = {}
    for sec in ('anims', 'cues', 'leaks', 'speech_brows'):
        if sec in prof:                          # an older head's profile may lack the later sections
            walk(prof[sec], sec, sets)
    rows = []
    for path, W in sets.items():
        if path.startswith('anims/VISEME'):
            continue
        W0 = dict(W)
        new = {n: (min(1.0, w*k[n]) if n in k else w) for n, w in W0.items()}
        # the set's reach in the zone each of its shapes works in, back to MPFB's (the weights stay 0..1)
        a = reach(SM, W0, pts)
        for _ in range(4):
            b = reach(SK, new, pts)
            for n in new:
                if n in k and a[dom[n]] > 0.3 and b[dom[n]] > 1e-6:
                    new[n] = float(min(1.0, new[n]*a[dom[n]]/b[dom[n]]))
        b = reach(SK, new, pts)
        for n in W:
            W[n] = round(new[n], 3)
        rows.append((path, a, b))
    if 'speech_face' in prof:
        sf = prof['speech_face']
        sf.pop('cap', None)
        sf.pop('scale', None)
        sf['about'] = ("The face from the voice (engine-plan.md, step 10) on a head with Mark's shapes (face-shapes.md, "
                       "'Формы Mark в игре'): a track of Audio2Face v3.0 weights, solved for Mark's shapes, plays as it "
                       "is - no cap, no scale; eyeLook* dropped (the gaze is Eyes'). The track's eyeBlink* is the level "
                       "of the lids, the blinks go over it. A track made with an emotion has the expression: the mood "
                       "faces of the line give way to it while it shows and come back after it.")
    prof['about'] = ("Face profile of a head with Mark's shapes (Audio2Face v3.0, carried by shapes/head_shapes.py): the "
                     "MPFB template's (test-head.face.json) weights re-scaled by shapes/mark_profile.py so that every "
                     "expression, cue, leak and brow move reaches as far as on MPFB's shapes. Do not edit by hand: "
                     "change the template and run mark_profile.py.")
    log('per-shape factors:', {n: round(v, 2) for n, v in k.items()})
    return prof, rows


def main(argv):
    mp_glb, mk_glb, reg, tmpl, out = argv[:5]
    table = argv[5] if len(argv) > 5 else None
    hm = X.hero_from_parts(head_glb.read(mp_glb), reg)
    mk = head_glb.read(mk_glb)
    prof, rows = profile(hm, {n: mk['face|sk|'+n]-hm['V'] for n in X.ARKIT}, tmpl)
    json.dump(prof, open(out, 'w'), indent=2, ensure_ascii=False)
    if table:
        write_table(rows, table)
        print('table ->', table)


def write_table(rows, table):
    """Per weight set and zone: MPFB's reach -> Mark's, mm (a dash where MPFB moves the zone under 0.3 mm)."""
    with open(table, 'w') as f:
        f.write('| Набор весов | ' + ' | '.join(ZONES) + ' |\n|---|' + '---|'*len(ZONES) + '\n')
        for path, a, b in rows:
            cells = ['—' if a[z] < 0.3 else '%.1f → %.1f (%+d %%)' % (a[z], b[z], round(100*(b[z]/a[z]-1)))
                     for z in ZONES]
            f.write('| %s | %s |\n' % (path, ' | '.join(cells)))


if __name__ == '__main__':
    main(sys.argv[1:])
