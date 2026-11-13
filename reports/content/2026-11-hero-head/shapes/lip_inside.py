# The inner faces of the lips against their outer rims (face-shapes.md, "Формы Mark в игре", round 3): seen from the
# front, along the face's "down", the inside of the lower lip must not stand above the lower lip's outer rim, the
# inside of the upper lip not below the upper lip's rim. Measured for jawOpen 0, 0.3, 0.5, 0.7, 1 alone and with the
# funnel, the pucker, mouthClose 0.5, the smile and the roll.
#   ~/.local/share/gothic-ai/mpfb/venv/bin/python lip_inside.py <MPFB head.glb> <reg.npz> [<label>=<head.glb>] ...
# The lips and their halves come from the MPFB head (all heads share its mesh): the outer face of each lip is what faces
# forward at rest, its inside what faces into the mouth within 8 mm of it (the lower half moves with the chin in
# jawOpen). Per 2 mm
# across the mouth (between the corners) the rim is the highest outer vertex of the lower lip (the lowest of the upper
# one), the inside the highest inner vertex of the lower lip (the lowest of the upper one).
import os
import sys

import numpy as np
from scipy.spatial import cKDTree

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import head_glb  # noqa: E402
import transfer as X  # noqa: E402

LIMIT = 0.0005
CORNER = 0.2      # a fifth of the mouth's width at each corner is left out: there the inside of one lip turns into the other
JAW = (0.0, 0.3, 0.5, 0.7, 1.0)
WITH = {'': {}, 'mouthFunnel': {'mouthFunnel': 1}, 'mouthPucker': {'mouthPucker': 1}, 'mouthClose 0.5': {'mouthClose': 0.5},
        'smile': {'mouthSmileLeft': 1, 'mouthSmileRight': 1}, 'roll': {'mouthRollLower': 1, 'mouthRollUpper': 1}}


def lips(hero):
    """Vertex sets of the MPFB head: outer and inner of the upper and the lower lip, and the frame. A lip is the
    mouth's skin and interior near the mouth between the nose and the chin, the lower one what moves with the chin in
    jawOpen. Its outer face is what faces forward at rest, its inside what faces into the mouth: the fit's interior
    is not that - the free edge of the hero's lips belongs to it."""
    fr = X.frame_and_landmarks(hero)
    V = hero['V']
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from head_glb import normals
    N = normals(V, hero['T'])
    jaw, _ = X.halves(hero['S'], V, fr['lm'], fr)
    r = V-fr['mc']
    near = np.linalg.norm(r, axis=1) < 0.32*fr['u']
    top = (fr['lm']['subnasale']-fr['mc']) @ fr['down']
    bottom = (fr['lm']['sulcus']-fr['mc']) @ fr['down']
    band = near & (r @ fr['down'] > top) & (r @ fr['down'] < bottom)
    facing = N @ fr['fwd']
    out = {}
    for name, half in (('lower', jaw), ('upper', ~jaw)):
        outer = np.nonzero(band & half & (facing > 0.3))[0]
        inner = np.nonzero(near & half & (facing < -0.3))[0]
        inner = inner[cKDTree(V[outer]).query(V[inner])[0] < 0.008]
        out[name] = (outer, inner)
    return out, fr


def rims(P, sets, fr, lo, hi):
    """Per lip and inner vertex: how far it stands over its lip's rim (m; > 0 above the lower rim, below the upper)
    between the corners lo..hi (nan outside). The rim is the envelope of the outer face seen from the front: its
    outer vertices that are the highest of the lower lip (the lowest of the upper) within 3 mm across, joined by
    straight lines (a strip with no vertex of the rim made a deeper vertex the rim)."""
    res = {}
    for name, (outer, inner) in sets.items():
        sg = 1 if name == 'lower' else -1                   # in "up" for the lower lip, "down" for the upper
        x_o, y_o = (P[outer]-fr['mc']) @ fr['left'], -sg*((P[outer]-fr['mc']) @ fr['down'])
        x_i, y_i = (P[inner]-fr['mc']) @ fr['left'], -sg*((P[inner]-fr['mc']) @ fr['down'])
        env = [k for k in range(len(outer)) if y_o[k] >= y_o[np.abs(x_o-x_o[k]) < 0.003].max()-1e-9]
        env = sorted(env, key=lambda k: x_o[k])
        rim = np.interp(x_i, x_o[env], y_o[env])
        over = y_i-rim
        over[(x_i < lo) | (x_i > hi)] = np.nan
        res[name] = over
    return res


def excess(P, sets, fr, cl, cr, rest):
    """The worst excess (m) and the number of inner vertices over the rim by more than LIMIT, per lip, for the
    positions P; a vertex counts by how much further it gets over than at rest (rest: rims() of the neutral face)."""
    lo, hi = sorted([(cl-fr['mc']) @ fr['left'], (cr-fr['mc']) @ fr['left']])
    lo, hi = lo+(hi-lo)*CORNER, hi-(hi-lo)*CORNER
    now = rims(P, sets, fr, lo, hi)
    res = {}
    for name in sets:
        over = now[name]-np.maximum(np.nan_to_num(rest[name], nan=0.0), 0)
        over = over[~np.isnan(over)]
        res[name] = (int((over > LIMIT).sum()), float(over.max()) if len(over) else 0.0)
    return res


def measure(hero, S, sets, fr):
    """{(jaw, with): {'lower': (vertices, mm), 'upper': (...)}} for the shifts S."""
    V = hero['V']
    c0l, c0r = V[fr['lm_v']['mouthLeft']], V[fr['lm_v']['mouthRight']]
    lo, hi = sorted([(c0l-fr['mc']) @ fr['left'], (c0r-fr['mc']) @ fr['left']])
    lo, hi = lo+(hi-lo)*CORNER, hi-(hi-lo)*CORNER
    rest = rims(V, sets, fr, lo, hi)
    out = {}
    for j in JAW:
        for k, extra in WITH.items():
            W = {'jawOpen': j, **extra}
            if not any(W.values()):
                continue
            P = hero['V']+sum(w*S[n] for n, w in W.items())
            cl = P[fr['lm_v']['mouthLeft']]
            cr = P[fr['lm_v']['mouthRight']]
            out[(j, k)] = {n: (c, w*1000) for n, (c, w) in excess(P, sets, fr, cl, cr, rest).items()}
    return out


def summary(res):
    """(cases over the limit, worst mm, vertices in the worst case) per lip."""
    s = {}
    for lip in ('lower', 'upper'):
        bad = [(v[lip][1], v[lip][0], k) for k, v in res.items() if v[lip][1] > LIMIT*1000]
        worst = max(res.items(), key=lambda kv: kv[1][lip][1])
        s[lip] = (len(bad), worst[1][lip][1], worst[1][lip][0], worst[0])
    return s


def against(res, ref):
    """Per lip: the cases where res stands over the limit and further than ref (MPFB) by more than the limit, and the
    worst such difference, mm."""
    out = {}
    for lip in ('lower', 'upper'):
        d = {k: v[lip][1]-max(ref[k][lip][1], 0.0) for k, v in res.items()}
        bad = {k: v for k, v in d.items() if v > LIMIT*1000 and res[k][lip][1] > LIMIT*1000}
        out[lip] = (len(bad), max(d.values()), max(d, key=d.get))
    return out


def main(argv):
    mp, reg = argv[0], argv[1]
    hero = X.hero_from_parts(head_glb.read(mp), reg)
    sets, fr = lips(hero)
    print('lips: lower outer %d inner %d, upper outer %d inner %d vertices' % (
        len(sets['lower'][0]), len(sets['lower'][1]), len(sets['upper'][0]), len(sets['upper'][1])))
    heads = [('MPFB', hero['S'])]
    for a in argv[2:]:
        label, path = a.split('=', 1)
        d = head_glb.read(path)
        heads.append((label, {n: d['face|sk|'+n]-hero['V'] for n in X.ARKIT}))
    ref = None
    for label, S in heads:
        res = measure(hero, S, sets, fr)
        ref = ref or res
        s = summary(res)
        print('%-22s' % label + ' | '.join(
            '%s: %d of %d cases over %.1f mm, worst %.1f mm (%d vertices, jaw %.1f%s)' % (
                lip, n, len(res), LIMIT*1000, w, c, k[0], ' + '+k[1] if k[1] else '') for lip, (n, w, c, k) in s.items()))
        if res is not ref:
            print('    over MPFB by more than %.1f mm: ' % (LIMIT*1000) + ' | '.join('%s %d of %d cases, worst %+.1f mm (jaw %.1f%s)' % (
                lip, n, len(res), w, k[0], ' + '+k[1] if k[1] else '') for lip, (n, w, k) in against(res, ref).items()))
        for j in JAW:
            print('    jaw %.1f: ' % j + '  '.join('%s %s' % (k or 'alone', '/'.join(
                '%.1f' % res[(j, k)][lip][1] for lip in ('lower', 'upper'))) for k in WITH if (j, k) in res))


if __name__ == '__main__':
    main(sys.argv[1:])
