"""Put the full model onto our head by the bare face: the last step of placing it.

  python place_face.py <full dir> <params.json> <raw.npz of the full model> <reg dir> <reg.npz of the head> <views dir>

<full dir>: place.json and ref.npz of place_ref.py, views.json. <reg dir>: mpfb.npz and groups.npz of the template.
<views dir>: full_front.png and full_profile.png, the whole views of the concept in the frame of params.json.

place_ref.py puts a model on the concept by the outlines of the two views. For the full model that is not enough:
its outline is mostly hair (a tail or a lock the generator hung elsewhere pulls it), and a shift along the view
does not move an outline at all. On the second face the full model stood 4 mm in front of the head and 3 mm under
it, so the hair came out that much thinner on the crown and the nape, and the colours of the face landed that far
from their places.
The head is already there (fitted to the bald model), and the two models are the same person: where both are bare
skin, they must coincide. That is the face from the brows' height to the base of the nose, in front of the ears:
no hair, no beard. The model's points there are pulled to the head's skin along its normals; points further than
4 mm from the skin (a lock on the forehead) do not count, and the further a point is, the less it weighs (a brow, a
lash: with equal weights they pushed the model 0.8 mm back). No turn: the views fix it.
The face gives the shift. It does not hold the scale: 2 % either way change the face's misfit by 0.05 mm and move the
hair of the nape by 4 mm (on hero 3a the face alone asked for +3 %). The scale is taken where it shows: for every
scale the shift is fitted by the face, and the scale is kept whose outline lies best on the concept's in both whole
views. It is the outline place_ref.py lays the bald model by: above 100 mm under the eyes, both ways, and a point
further than 4 mm from the other outline counts as one at 4 mm (a tail the generator hung elsewhere, a lock, a
quiff it flattened). The hair over the eyes alone is not enough: a generator's hair is wider at the sides and
flatter at the back than the concept's, and with only that to go by the second face's model came out 1 % smaller
than with the sides of its beard counted; the layer of the beard on the cheeks went from 3.4 to 2.0 mm, and the
beard turned to stubble there.
Rewrites place.json (scale, shift_mm: the whole placing; outline: what place_ref.py found; face: this step and its
residuals) and ref.npz.
"""
import json, os, sys
import numpy as np
from scipy.spatial import cKDTree

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, '..', '..', '..', 'reports', 'content', '2026-11-hero-head', 'skin'))
import ref_model
from concept_view import Views

D, PJ, RAW, RD, REG, VD = sys.argv[1:7]
P = json.load(open(PJ)); V = json.load(open(os.path.join(D, 'views.json'))); eye = np.array(V['eye'])
pl = json.load(open(os.path.join(D, 'place.json'))); o = pl.get('outline', pl)       # a second run starts from the outlines again
s0, t0 = float(o['scale']), np.array(o['shift_mm'])/1000
M = np.load(os.path.join(RD, 'mpfb.npz')); body = np.load(os.path.join(RD, 'groups.npz'))['body'].astype(bool)
co = np.load(REG)['co'].astype(np.float64); tris = M['tris'][body[M['tris']].all(1)]
fv, _ = ref_model.load(np.load(RAW), P, eye, {'place': {'scale': s0, 'shift_mm': list(t0*1000)}, 'tone': None})
q = fv-eye; face = (q[:, 1] < 0.02) & (q[:, 2] > -0.045) & (q[:, 2] < 0.045) & (np.abs(q[:, 0]) < 0.07)
p = fv[face].astype(np.float64); p = p[::max(1, len(p)//200000)]


def samples(v, t, step=0.0008):
    """Points over the triangles with their normals."""
    a, b, c = v[t[:, 0]], v[t[:, 1]], v[t[:, 2]]; n = np.ceil(np.maximum(np.linalg.norm(b-a, axis=1), np.linalg.norm(c-a, axis=1))/step).astype(int).clip(1, 40)
    nr = np.cross(b-a, c-a); nr /= np.linalg.norm(nr, axis=1, keepdims=True)+1e-12; out, on = [], []
    for k in np.unique(n):
        s = n == k; u, w = np.mgrid[0:k+1, 0:k+1]; ok = u+w <= k; u, w = u[ok]/k, w[ok]/k
        out.append((a[s][:, None]+(b[s]-a[s])[:, None]*u[None, :, None]+(c[s]-a[s])[:, None]*w[None, :, None]).reshape(-1, 3))
        on.append(np.repeat(nr[s], len(u), axis=0))
    return np.concatenate(out), np.concatenate(on)


near = (np.abs(co[tris].mean(1)-eye) < [0.09, 0.07, 0.07]).all(1) & ((co[tris].mean(1)-eye)[:, 1] < 0.04)
hs, hn = samples(co, tris[near]); tree = cKDTree(hs)


def off(pp, lim):
    d, k = tree.query(pp, workers=-1); return ((pp-hs[k])*hn[k]).sum(1), hn[k], d < lim


def shift_for(s, t, steps):
    """The shift that lays the face on the head's skin at scale s; returns it with (median |offset|, median, 90 %, share
    within 4 mm), the first two before the first step."""
    for it in range(steps+1):
        pp = (p-eye)*s+eye+t; res, n, ok = off(pp, 0.008 if it < 3 and steps > 6 else 0.004)
        if it == 0: was = (1000*float(np.median(np.abs(res[ok]))), 1000*float(np.median(res[ok])))
        if it == steps: break
        # a brow or a lash stands a millimetre or two over the bald skin: past the first steps such points weigh little
        w = np.ones(ok.sum()) if it < 3 and steps > 6 else np.clip(1-(res[ok]/0.0025)**2, 0, 1)
        t = t+np.linalg.lstsq(n[ok]*w[:, None], -res[ok]*w, rcond=None)[0]
    return t, was, (1000*float(np.median(np.abs(res[ok]))), 1000*float(np.percentile(np.abs(res[ok]), 90)), float(ok.mean()))


# ---- the scale: the model's outline against the concept's in both whole views
W = Views(P, eye); allp = fv.astype(np.float64); LOW, FAR = -0.100, 4.0
CM = {k: W.concept(os.path.join(VD, f'full_{k}.png')) for k in ('front', 'profile')}; CS = {k: W.sdf(CM[k]) for k in CM}


def misfit(s, t):
    """How far the outlines stay apart, both ways, as a bounded loss: 0 where they coincide, 1 where nothing is within FAR."""
    q = (allp-eye)*s+eye+t; e = 0.0
    for k in CM:
        m = W.points(q, k); r = W.row(k, LOW)
        for d in (CS[k][:r][W.edge(m)[:r]], W.sdf(m)[:r][W.edge(CM[k])[:r]]):
            x = np.clip(np.abs(d)/FAR, 0, 1); e += float((1-(1-x*x)**3).mean())/4
    return e


t, before, _ = shift_for(1.0, np.zeros(3), 12); scan = {}
for sc in np.arange(0.95, 1.0501, 0.01):
    t, _, _ = shift_for(sc, t, 5); scan[round(float(sc), 4)] = (misfit(sc, t), t)
best = min(scan, key=lambda k: scan[k][0]); t = scan[best][1]
for sc in np.arange(best-0.0075, best+0.0076, 0.0025):
    t, _, _ = shift_for(sc, t, 4); scan[round(float(sc), 4)] = (misfit(sc, t), t)
s = min(scan, key=lambda k: scan[k][0]); t, _, after = shift_for(s, scan[s][1], 8)
print('PLACE FACE scale %.4f (the outlines: misfit %.4f, %.4f at scale 1) shift mm x %+.1f y %+.1f z %+.1f; the face over the head\'s skin, mm: '
      'median %.2f (%+.2f signed) -> %.2f, 90%% %.2f, %.0f%% of %d points within 4 mm' % (s, scan[s][0], scan[1.0][0], *(t*1000), *before, after[0], after[1], 100*after[2], len(p)))
if not (np.abs(t).max() < 0.015 and after[2] > 0.6):
    raise SystemExit('place_face: the face of the full model does not land on the head; look at place_check.jpg')
json.dump({'scale': s0*s, 'shift_mm': [float(x) for x in (t0*s+t)*1000],
           'outline': {'scale': s0, 'shift_mm': [float(x) for x in t0*1000]},
           'face': {'scale': float(s), 'shift_mm': [float(x) for x in t*1000], 'before_mm': round(before[0], 2), 'after_mm': round(after[0], 2),
                    'outline_misfit': {str(k): round(v[0], 4) for k, v in sorted(scan.items())}}},
          open(os.path.join(D, 'place.json'), 'w'))
src = os.path.join(D, 'ref_unplaced.npz')
if os.path.exists(src):                                       # the cut model next to place.json stays the placed one
    r = np.load(src); v = (r['v'].astype(np.float64)-eye)*s0*s+eye+t0*s+t
    np.savez(os.path.join(D, 'ref.npz'), v=v.astype(np.float32), **{k: r[k] for k in r.files if k != 'v'})
