"""Put the dense model where the concept is: one scale and a shift by the outlines of the two whole views.

  python place_ref.py <dir> <params.json> <raw.npz> <front.png> <profile.png> [--low mm]

<dir>: ref.npz from reg_export.py (the model cut to the head) and views.json. <raw.npz>: the generator's whole
model. The two pictures are the whole views of the concept in the frame of params.json.
A generator centres and scales its model by its own bounds, so a head without the ponytail lands elsewhere than
one with it. The model's outline in the front and the profile is laid on the concept's: one scale about the point
between the eyes and a shift.
- The whole views, not the frames of views.json: those end 9 cm over the eyes and 15 cm behind them, so the crown
  and the nape, where the scale shows, were outside, and the second face came out 1 % small (2 mm at the nape).
- Only above `--low` mm under the eyes (100): below are the chin's underside, which a generator shortens, the neck
  and the shirt.
- One scale, not one per axis: three scales lower the misfit further, but they follow the ears, which the
  generator makes stand out 2-3 mm more than the concept, and narrow the skull by a millimetre a side.
- Solved, not searched: every point of one outline knows its distance to the other and the direction to it (the
  signed distance field), so the scale and the shift are a least-squares step, repeated. Points far from the other
  outline weigh less and past a limit nothing (an ear, a lock of hair, a tail the generator hung elsewhere); the
  limit closes from 12 mm to 3 mm as the outlines meet.
Rewrites ref.npz (the first version stays as ref_unplaced.npz), writes place.json (scale, shift_mm, and how far the
outlines stay apart: mm by view, and by direction from the head's middle) and place_check.jpg.
"""
import json, os, sys
import numpy as np
from PIL import Image
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, '..', '..', '..', 'reports', 'content', '2026-11-hero-head', 'skin'))
import ref_model
from concept_view import Views

LOW = -float(ref_model.take(sys.argv, '--low') or 100)/1000
D, PJ, RAW = sys.argv[1:4]; PIC = {'front': sys.argv[4], 'profile': sys.argv[5]}
P = json.load(open(PJ)); V = json.load(open(os.path.join(D, 'views.json'))); eye = np.array(V['eye']); W = Views(P, eye)
pts = ref_model.load(np.load(RAW), P, eye, {'place': None, 'tone': None})[0].astype(np.float64)
CM = {k: W.concept(PIC[k]) for k in PIC}; CS = {k: W.sdf(CM[k])/(W.mpp*1000) for k in PIC}          # distances in pixels
rows = {k: W.row(k, LOW) for k in PIC}
grad = lambda f: np.gradient(f)                                         # (d/dv, d/du)


def place(s, t, p): return (p-eye)*s+eye+t


def step(s, t, lim):
    """One least-squares step for (scale change, shift u in the front, shift u in the profile, shift v)."""
    A, b, w, far = [], [], [], {}
    for n, k in enumerate(PIC):
        m = W.points(place(s, t, pts), k); ms = W.sdf(m)/(W.mpp*1000); e = W.eye_px[k]
        for sd, src, sign in ((CS[k], m, 1.0), (ms, CM[k], -1.0)):     # the model's outline on the concept's, and back
            i, j = np.nonzero(W.edge(src)[:rows[k]]); gv, gu = grad(sd); d = sd[i, j]; gu, gv = sign*gu[i, j], sign*gv[i, j]
            a = np.zeros((len(i), 4)); a[:, 0] = gu*(j-e[0])+gv*(i-e[1]); a[:, 1+n] = gu; a[:, 3] = gv
            x = np.abs(d)*W.mpp*1000/lim; ww = np.where(x < 1, (1-x*x)**2, 0.0)/max(len(i), 1)
            A.append(a); b.append(-d); w.append(ww); far.setdefault(k, []).append(np.abs(d[x < 1])*W.mpp*1000)
    A, b, w = np.concatenate(A), np.concatenate(b), np.concatenate(w)
    x = np.linalg.lstsq(A*np.sqrt(w)[:, None], b*np.sqrt(w), rcond=None)[0]
    return x, {k: float(np.mean(np.concatenate(v))) for k, v in far.items()}


s, t = 1.0, np.zeros(3); first = None
for it in range(30):
    lim = max(12.0*0.75**it, 3.0); x, far = step(s, t, lim)
    if first is None: first = far
    T = np.array([x[1], x[2], -x[3]])*W.mpp; s, t = s*(1+x[0]), t*(1+x[0])+T
    if lim == 3.0 and abs(x[0]) < 1e-4 and np.abs(T).max() < 5e-5: break
_, far = step(s, t, 3.0)
sides = {k: W.by_angle(W.points(place(s, t, pts), k), CS[k]*W.mpp*1000, k, LOW) for k in PIC}
print('PLACE scale %.4f shift mm x %+.1f y %+.1f z %+.1f after %d steps; the outlines apart, mm (mean of those within 3 mm): front %.2f profile %.2f '
      '(as generated, within 12 mm: %.2f, %.2f)' % (s, *(t*1000), it+1, far['front'], far['profile'], first['front'], first['profile']))
for k in PIC: print('PLACE %s: the model outside the concept by direction, mm (0 right, 90 up): %s' % (k, ' '.join('%d:%+.1f' % kv for kv in sides[k].items())))
json.dump({'scale': float(s), 'shift_mm': [float(x) for x in t*1000], 'apart_mm': far, 'by_direction_mm': sides, 'low_mm': LOW*1000},
          open(os.path.join(D, 'place.json'), 'w'))
src = os.path.join(D, 'ref_unplaced.npz')
if not os.path.exists(src): os.replace(os.path.join(D, 'ref.npz'), src)
r = np.load(src)
np.savez(os.path.join(D, 'ref.npz'), v=place(s, t, r['v'].astype(np.float64)).astype(np.float32), **{k: r[k] for k in r.files if k != 'v'})
tiles = []
for k in PIC:
    im = np.asarray(Image.open(PIC[k]).convert('RGB')).copy()
    for col, p in (((255, 60, 60), pts), ((60, 255, 90), place(s, t, pts))):       # red: as generated, green: placed
        m = W.points(p, k); im[m & ~ndimage.binary_erosion(m, iterations=2)] = col
    im[rows[k]-1:rows[k]+1, ::6] = (255, 255, 255); tiles.append(Image.fromarray(im))    # the dashes: nothing below counts
sh = Image.new('RGB', (2*W.res, W.res)); sh.paste(tiles[0], (0, 0)); sh.paste(tiles[1], (W.res, 0)); sh.save(os.path.join(D, 'place_check.jpg'), quality=88)
