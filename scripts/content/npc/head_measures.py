"""Measures of defects of a head built by build_head.py, written to <work>/measures.json and printed.

  python head_measures.py <work dir>

- skin_mm: how far the registered skin lies from the bald dense model, median and 95th percentile (reg_solve.py's
  last step), and the landmark error;
- folds: skin triangles of the template turned over by the registration, and bent by more than 60 degrees;
- place: the scale and shift place_ref.py gave each dense model (a big one means the generator framed it oddly);
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

r, m, g = np.load(j('reg', 'reg.npz')), np.load(j('reg', 'mpfb.npz')), np.load(j('reg', 'groups.npz'))
t = m['tris']; t = t[g['body'][t].all(1)]


def normals(p):
    n = np.cross(p[t[:, 1]] - p[t[:, 0]], p[t[:, 2]] - p[t[:, 0]])
    return n / (np.linalg.norm(n, axis=1, keepdims=True) + 1e-15)


c = np.einsum('ij,ij->i', normals(m['co']), normals(r['co']))
out['folds'] = {'flipped': int((c < 0).sum()), 'bent_over_60': int((c < 0.5).sum())}
out['place'] = {k: json.load(open(j(k, 'place.json'))) for k in ('reg', 'full') if os.path.exists(j(k, 'place.json'))}

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
json.dump(out, open(j('measures.json'), 'w'), indent=1)
print(json.dumps(out, indent=1))
