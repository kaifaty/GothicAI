"""Measures of defects of a head built by build_head.py, written to <work>/measures.json and printed.

  python head_measures.py <work dir>

- skin_mm: how far the registered skin lies from the bald dense model, median and 95th percentile (reg_solve.py's
  last step), and the landmark error;
- outline: the last pass (head_outline.py): the faces against the model, the head's outline against the concept's
  before and after, how far the skin moved;
- relief: the bald model over the head's faces before and after head_relief.py (the whole and the nasolabial zone),
  how far the vertices moved;
- eyes: the opening of the lids and the iris. The concept's are read from its full front (eye_lines.py); the
  head's opening is the distance between its lid chains at the iris (the mesh itself, no picture), its iris is
  read from the front render;
- folds: skin triangles of the template turned over by the registration and the last pass, and bent by more than
  60 degrees;
- place: the scale and shift place_ref.py gave each dense model (a big one means the generator framed it oddly) and
  how far its outline stays from the concept's;
- face_mm: the face points of the detector on the concept and on our head with its strands, both front tiles of
  the likeness sheet (one camera, 0.6 mm a pixel): median and the worst of the groups of eyes, brows, lips, nose
  and jaw. None when the detector finds no face on one of them.
"""
import json
import os
import sys

import numpy as np

W = sys.argv[1]
j = lambda *p: os.path.join(W, *p)
out = {}

log = json.load(open(j('reg', 'reg_log.json')))
last = (log.get('steps') or log.get('log') or [log])[-1] if isinstance(log, dict) else log[-1]
out['skin_mm'] = {k: round(float(last[k]), 2) for k in ('skin_mm_median', 'skin_mm_p95', 'lm_mm_median', 'lm_mm_max')
                  if k in last}

if os.path.exists(j('outline', 'outline.json')):
    o = json.load(open(j('outline', 'outline.json')))
    out['outline'] = {'faces_on_model_mm': o['faces'], 'apart_from_concept_mm': o['outline_apart_mm'], 'moved_mm': o['moved_mm']}

if os.path.exists(j('relief', 'relief.json')):
    o = json.load(open(j('relief', 'relief.json')))
    out['relief'] = {'model_over_head_mm': {k: {z: o[k][z] for z in ('all', 'nasolabial', 'brows') if z in o[k]} for k in ('before', 'after')},
                     'moved_mm': o['moved_mm'], 'faces_turned_over': o['faces_turned_over']}

r, m, g = np.load(j('round', 'reg.npz')), np.load(j('reg', 'mpfb.npz')), np.load(j('reg', 'groups.npz'))
t = m['tris']; t = t[g['body'][t].all(1)]


def normals(p):
    n = np.cross(p[t[:, 1]] - p[t[:, 0]], p[t[:, 2]] - p[t[:, 0]])
    return n / (np.linalg.norm(n, axis=1, keepdims=True) + 1e-15)


c = np.einsum('ij,ij->i', normals(m['co']), normals(r['co']))
out['folds'] = {'flipped': int((c < 0).sum()), 'bent_over_60': int((c < 0.5).sum())}
out['place'] = {k: {n: v for n, v in json.load(open(j(k, 'place.json'))).items() if n in ('scale', 'shift_mm', 'apart_mm')}
                for k in ('reg', 'full') if os.path.exists(j(k, 'place.json'))}

GROUPS = ('eye_r_upper', 'eye_r_lower', 'eye_l_upper', 'eye_l_lower', 'brow_r', 'brow_l', 'lip_upper_outer',
          'lip_lower_outer', 'nose', 'jaw')
pa, pb = j('sheet', 'points', 'concept.json'), j('sheet', 'points', 'ours.json')
if os.path.exists(pa) and os.path.exists(pb):
    A, B = json.load(open(pa)), json.load(open(pb))
    a, b = (np.array(X['points'])[:, :2] - (X['box'] or [0, 0])[:2] for X in (A, B))   # each in its own tile
    d = np.linalg.norm(a - b, axis=1) * 0.6
    by = {k: round(float(np.median(d[A['groups'][k]])), 1) for k in GROUPS if k in A['groups']}
    out['face_mm'] = {'median': round(float(np.median(d)), 1), 'groups': by}
else:
    out['face_mm'] = None
# ---- the eyes
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eye_lines
P = json.load(open(j('params.json'))); V = json.load(open(j('reg', 'views.json'))); ez = V['eye'][2]
pick = lambda e: {k: e[k] for k in ('opening_mm', 'upper_over_iris_centre_mm', 'lower_under_iris_centre_mm', 'upper_by', 'lower_by')} | {
    'iris_mm': e['iris']['diameter_mm'], 'iris_srgb': e['iris']['srgb']}
eyes = {}
cp = j('views', 'points', 'full_front.json')
if os.path.exists(cp):
    eyes['concept'] = {s: pick(e) for s, e in eye_lines.read(j('views', 'full_front.png'), cp, 61.5/P['ipd_px'])[2].items()}
chains = json.load(open(j('reg', 'lid_chains.json'))); co = r['co']; ours = {}
for s in ('r', 'l'):
    up, lo = co[chains[s]['upper']], co[chains[s]['lower']]; cx = (up[:, 0].min()+up[:, 0].max())/2
    at = lambda line: float(np.interp(cx, *line[np.argsort(line[:, 0])][:, [0, 2]].T))
    ours[s] = {'opening_mm': round((at(up)-at(lo))*1000, 2), 'upper_over_eye_line_mm': round((at(up)-ez)*1000, 2), 'lower_under_eye_line_mm': round((ez-at(lo))*1000, 2)}
op = j('sheet', 'points', 'ours_front.json')
if os.path.exists(op):
    for s, e in eye_lines.read(j('sheet', 's_front.png'), op, 0.4)[2].items(): ours[s] |= {'iris_mm': e['iris']['diameter_mm'], 'iris_srgb': e['iris']['srgb']}
eyes['ours'] = ours; out['eyes'] = eyes
json.dump(out, open(j('measures.json'), 'w'), indent=1)
print(json.dumps(out, indent=1))
