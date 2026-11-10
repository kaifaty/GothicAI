"""Put the dense model where the concept is: scale and shift by the outlines of the two views.

  python place_ref.py <reg dir> [v0 v1]

<reg dir>: ref.npz from reg_export.py, views.json, concept_front.png and concept_profile.png (concept_frame.py).
A generator centres and scales its model by its own bounds, so a head without the ponytail lands elsewhere than
one with it. The model's outline in the front and profile frames is fitted to the concept's outline between
rows v0 and v1 (above the shirt; 80..800 by default): one scale about the eye point and a shift.
Rewrites ref.npz (the first version stays as ref_unplaced.npz), writes place.json and place_check.jpg.
"""
import json, os, sys
import numpy as np
from PIL import Image
from scipy import ndimage
from scipy.optimize import minimize

D = sys.argv[1]; V0, V1 = (int(sys.argv[2]), int(sys.argv[3])) if len(sys.argv) > 3 else (80, 800)
V = json.load(open(os.path.join(D, 'views.json'))); N, S = V['N'], V['s']; eye = np.array(V['eye'])
fc, pc = V['views']['front']['loc'], V['views']['profile']['loc']
src = os.path.join(D, 'ref_unplaced.npz')
if not os.path.exists(src): os.replace(os.path.join(D, 'ref.npz'), src)
r = np.load(src); v = r['v'].astype(np.float64)
C = 4; n = N // C                                                    # outlines are compared on 4 px cells
sub = v[np.random.default_rng(1).choice(len(v), min(len(v), 600000), replace=False)]


def concept(name):
    a = np.asarray(Image.open(os.path.join(D, f'concept_{name}.png')).convert('RGB')).astype(int)
    m = np.abs(a - np.array([122, 124, 128])).max(2) > 14
    return ndimage.binary_opening(m, iterations=2)[::C, ::C]


def outline(p, name):
    if name == 'front': u = (p[:, 0] - fc[0]) / S + N / 2
    else: u = (p[:, 1] - pc[1]) / S + N / 2
    w = (pc[2] if name == 'profile' else fc[2]); vv = (w - p[:, 2]) / S + N / 2
    m = np.zeros((n, n), bool); i, j = (vv / C).astype(int), (u / C).astype(int)
    ok = (i >= 0) & (i < n) & (j >= 0) & (j < n); m[i[ok], j[ok]] = True
    return ndimage.binary_fill_holes(ndimage.binary_closing(m, iterations=2))


CM = {k: concept(k) for k in ('front', 'profile')}
rows = slice(V0 // C, V1 // C)
# the profile frame is cut behind the ear by the head box of reg_export.py: compare only what the model covers there
umax = int(((v[:, 1].max() - pc[1]) / S + N / 2) / C) - 6


def place(q, p):
    s, tx, ty, tz = q
    return (p - eye) * s + eye + np.array([tx, ty, tz]) / 1000


def loss(q):
    p = place(q, sub); e = 0.0
    for k in ('front', 'profile'):
        a, b = outline(p, k)[rows], CM[k][rows]
        if k == 'profile': a, b = a[:, :umax], b[:, :umax]
        e += 1 - (a & b).sum() / max((a | b).sum(), 1)
    return e


best = None
for s0 in (0.97, 1.0, 1.03):                                           # a few starts: the loss is piecewise constant
    for tx0 in (-12, 0, 12):
        o = minimize(loss, [s0, tx0, 0, 0], method='Nelder-Mead',
                     options={'initial_simplex': [[s0, tx0, 0, 0], [s0 + .02, tx0, 0, 0], [s0, tx0 + 6, 0, 0], [s0, tx0, 6, 0], [s0, tx0, 0, 6]], 'xatol': 0.05, 'fatol': 1e-4})
        if best is None or o.fun < best.fun: best = o
q = best.x; print('PLACE scale %.4f shift mm x %+.1f y %+.1f z %+.1f  loss %.4f (unplaced %.4f)' % (*q, best.fun, loss([1, 0, 0, 0])))
np.savez(os.path.join(D, 'ref.npz'), v=place(q, v).astype(np.float32), f=r['f'], n=r['n'])
json.dump({'scale': float(q[0]), 'shift_mm': [float(x) for x in q[1:]], 'loss': float(best.fun), 'rows': [V0, V1]}, open(os.path.join(D, 'place.json'), 'w'))
tiles = []
for k in ('front', 'profile'):
    im = np.asarray(Image.open(os.path.join(D, f'concept_{k}.png')).convert('RGB')).copy()
    for col, p in (((255, 60, 60), sub), ((60, 255, 90), place(q, sub))):       # red: as generated, green: placed
        m = np.kron(outline(p, k), np.ones((C, C), bool)); edge = m & ~ndimage.binary_erosion(m, iterations=2)
        im[edge] = col
    tiles.append(Image.fromarray(im).resize((600, 600)))
s = Image.new('RGB', (1200, 600)); s.paste(tiles[0], (0, 0)); s.paste(tiles[1], (600, 0)); s.save(os.path.join(D, 'place_check.jpg'), quality=88)
