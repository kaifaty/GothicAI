"""Landmark targets of the wrap from the face points of the concept, without hand picks on the face.

  python auto_targets.py <reg dir> lid_chains.json concept_front.json tmpl_front.json base_lm_fixed.json out.json [lid_back_mm]

<reg dir>: views.json, mpfb.npz (template), ref.npz (the dense bald model, placed by place_ref.py).
concept_front.json: face_points.py run on the concept laid into the landmark frame (concept_frame.py), so a pixel
is a point of the front plane. tmpl_front.json: the same detector on the template rendered in that frame
(lm_mpfb_front.png).

The template side never comes from a detector: on a clay render it puts the eye corners and lips several
millimetres off and tears the mesh. It comes from the template itself:
- lids: the chains of lid-margin vertices (lid_chains.json, inner corner first). A chain vertex finds its place
  along the lid on the contour detected on the template (the contour runs 3 mm past each corner of the chain).
  Along the lid it moves by how far the concept's contour is from the template's at that place. Its height in the
  middle of the lid is the concept's lid line at that place, read from the picture (eye_lines.py), not a
  difference of two detections: the detector reads nearly the same eye whatever is drawn (9.0 mm of opening on
  the clay render of the template, whose chains are 7.6 mm apart, and 6.7 on the concept, drawn 7.0 mm open), so
  the difference shut the second face's eyes to 5.3 mm. Towards the two ends of a chain the height goes over to
  the old rule, the difference of the two contours: a chain ends where the lids meet, and the contours do not.
  Depth is the template lid moved back by lid_back_mm: generators sculpt the eye half shut and bulging, so the
  opening comes from the concept alone.
  Names start with r_/l_ (the subject's side), which is how reg_solve.py tells the lids from the rest.
  Writes <reg dir>/eyes.json and eyes.jpg (the lines on the concept, eye_lines.py).
- mouth: the four hand-picked vertices of base_lm_fixed.json (corners, seam centre, bottom of the lower lip) take the
  concept's points; depth from the dense model.
Ears and the profile midline stay as base_lm_fixed.json has them (lm_pick.py); its other front points are dropped.
With --no-hand they are dropped too: base_lm_fixed.json only names the mouth vertices, and the ears follow the
dense model as plain surface (a concept without hand picks, build_head.py).
The nose, brows and jaw get no landmarks: the dense model has them as plain surface.
"""
import json, os, sys
import numpy as np
from scipy.spatial import cKDTree

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eye_lines

NO_HAND = '--no-hand' in sys.argv
ARGV = [a for a in sys.argv if a != '--no-hand']
D, CH, CP, TP, BASE, OUT = ARGV[1:7]
LID = float(ARGV[7]) / 1000 if len(ARGV) > 7 else 0.0
V = json.load(open(os.path.join(D, 'views.json'))); N, s = V['N'], V['s']; fc = V['views']['front']['loc']
m0 = np.load(os.path.join(D, 'mpfb.npz'))['co']; r = np.load(os.path.join(D, 'ref.npz'))['v'].astype(np.float64)
P = json.load(open(CP)); pts = np.array(P['points']); G = P['groups']; chains = json.load(open(CH))
tpts = np.array(json.load(open(TP))['points'])
at = lambda p, k: np.array([fc[0] + (p[k, 0] - N / 2) * s, fc[2] - (p[k, 1] - N / 2) * s])
world = lambda k: at(pts, k)                                             # x, z of a concept point
tree = cKDTree(r[:, [0, 2]])
KEEP = ('ear', 'tragus', 'pronasale', 'subnasale', 'nasion', 'glabella')
MOUTH = {'mouth_r': 61, 'mouth_l': 291, 'lip_mid': 13, 'lip_lo': 17}


def depth(q, rad=0.0008):
    i = tree.query_ball_point(q, rad)
    while not i: rad *= 1.5; i = tree.query_ball_point(q, rad)
    return float(r[i, 1].min())


def share(p):
    d = np.r_[0, np.cumsum(np.linalg.norm(np.diff(p, axis=0), axis=1))]; return d / d[-1]


base = json.load(open(BASE))['landmarks']; out = [] if NO_HAND else [l for l in base if l['name'].startswith(KEEP)]
for l in base:
    if l['name'] in MOUTH:
        x, z = world(MOUTH[l['name']])
        out.append({'name': l['name'], 'mpfb_vertex': l['mpfb_vertex'], 'target': [round(float(c), 5) for c in (x, depth([x, z]), z)]})
def along(poly, q):
    """Share of the polyline's length at its point nearest to q."""
    seg = np.diff(poly, axis=0); ln = np.linalg.norm(seg, axis=1); cum = np.r_[0, np.cumsum(ln)]
    u = np.clip(np.einsum('ij,ij->i', q - poly[:-1], seg) / ln ** 2, 0, 1); near = poly[:-1] + u[:, None] * seg
    k = np.linalg.norm(near - q, axis=1).argmin(); return (cum[k] + u[k] * ln[k]) / cum[-1]


pic = os.path.join(D, 'concept_front.png'); img, ipts, EY = eye_lines.read(pic, CP, s * 1000)
json.dump({'mm_per_px': s * 1000, 'eyes': EY}, open(os.path.join(D, 'eyes.json'), 'w'), indent=1)
eye_lines.sheet(img, ipts, EY, s * 1000, os.path.join(D, 'eyes.jpg'))
for side in ('r', 'l'):
    for part, key in (('upper', 'up'), ('lower', 'lo')):
        line = np.array(EY[side][part]); lz = fc[2] - (line[:, 1] - N / 2) * s; ls = share(line)   # the concept's lid line, inner corner first
        idx = G['eye_%s_%s' % (side, part)][::-1]                                  # the detector's contour, inner corner first
        c = np.array([world(k) for k in idx]); t0 = np.array([at(tpts, k) for k in idx]); tc, tt = share(c), share(t0)
        ch = chains[side][part] + (chains[side]['upper'][-1:] if part == 'lower' else [])   # the lower chain ends one vertex short of the outer corner
        cs = share(m0[ch][:, [0, 2]])
        for n, vid in enumerate(chains[side][part]):
            if part == 'lower' and n == 0: continue                               # the inner corner is in the upper chain
            ti = along(t0, m0[vid][[0, 2]])
            d = [np.interp(ti, tc, c[:, a]) - np.interp(ti, tt, t0[:, a]) for a in (0, 1)]
            mid = float(np.clip(min(cs[n], 1 - cs[n]) / 0.3, 0, 1)); mid = mid * mid * (3 - 2 * mid)   # 0 at the chain's ends, 1 over its middle
            out.append({'name': '%s_%s%02d' % (side, key, n), 'mpfb_vertex': int(vid),
                        'target': [round(float(v), 5) for v in (m0[vid][0] + d[0], m0[vid][1] + LID, mid * np.interp(ti, ls, lz) + (1 - mid) * (m0[vid][2] + d[1]))]})
json.dump({'note': 'automatic: lids by the template lid chains and the concept lid contours, mouth by four points; ' + ('no ears or profile midline' if NO_HAND else 'ears and profile midline by hand picks'),
           'landmarks': out}, open(OUT, 'w'), indent=1)
mv = np.array([np.array(o['target']) - m0[o['mpfb_vertex']] for o in out]) * 1000
for grp in ('r_', 'l_', 'mouth', 'lip'):
    sel = [i for i, o in enumerate(out) if o['name'].startswith(grp)]
    print('TARGETS %-5s n %2d  move mm: mean x %+.1f y %+.1f z %+.1f, longest %.1f' % (grp, len(sel), *mv[sel].mean(0), np.linalg.norm(mv[sel], axis=1).max()))
op = lambda p, u, l: (at(p, u)[1] - at(p, l)[1]) * 1000
print('TARGETS eye opening by the detector, mm: concept r %.1f l %.1f, template r %.1f l %.1f' % (op(pts, 159, 145), op(pts, 386, 374), op(tpts, 159, 145), op(tpts, 386, 374)))
print('TARGETS eye opening on the concept (eye_lines.py), mm: r %.1f, l %.1f; the upper lid by the %s and the %s, the lower by the %s and the %s' % (
    EY['r']['opening_mm'], EY['l']['opening_mm'], EY['r']['upper_by'], EY['l']['upper_by'], EY['r']['lower_by'], EY['l']['lower_by']))
tz = {o['name']: (o['target'][2] - V['eye'][2]) * 1000 for o in out}
for sd in ('r', 'l'):
    print('TARGETS %s lid chains over the eye line, mm: upper %s lower %s' % (sd, ' '.join('%+.1f' % v for k, v in tz.items() if k.startswith(sd + '_up')),
                                                                           ' '.join('%+.1f' % v for k, v in tz.items() if k.startswith(sd + '_lo'))))
print('TARGETS total', len(out))
