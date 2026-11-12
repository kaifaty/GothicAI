"""The concept's frame for a head without hand picks: params.json of fit_head.py and the registration
(reports/content/2026-11-npc-pipeline/build.md, round Б).

  python auto_params.py <views dir> <points dir> <npc.json> <out params.json> [<fit dir>]

<views dir> holds the square views of build_head.py: {full,bald}_{front,profile}.png and the generator's cut-outs
{full,bald}/view00_azim000.png, view01_azim090.png. <points dir>: face_points.py on full_front and bald_front.

Without <fit dir> (first pass):
- ipd_px, front_eye: the pupils of the detector (iris centres 468 and 473), the mean of the full and the bald front.
  The scale of the whole pipeline is 61.5 mm between the pupils;
- profile_eye: at the front eye's height (the views of a sheet are drawn on one line); its depth is only a guess;
- macro, race, targets: from npc.json, or an average man and no targets (the registration makes the shape);
- ref_origin: where the dense models' centre goes. A generator scales its model to its bounds, so the centre is the
  middle of the bounds of the two cut-outs it saw, put into the world by the eye of the fit (needs <fit dir>; the
  first pass leaves the hand value of hero 3a, which only fit_head.py reads, and it does not read it).
With <fit dir> (second pass, after fit_head.py rendered the template with the first pass): the depth of the
profile eye is set so that the template's face line in the band from 5 mm under to 10 mm over the eye lies on the
bald concept's. On hero 3a a hand pick had them 0.8 mm apart there; the forehead above and the lips below differ by
the face itself (5 and 11 mm). ref_origin is computed from the fit's eye.
"""
import json
import os
import sys

import numpy as np
from PIL import Image
from scipy import ndimage

VD, PD, NPC, OUT = sys.argv[1:5]
FIT = sys.argv[5] if len(sys.argv) > 5 else None
IPD_M = 0.0615
cfg = json.load(open(NPC))


def iris(name):
    p = np.array(json.load(open(os.path.join(PD, name + '.json')))['points'])[:, :2]
    return p[468], p[473]


eyes = [iris(n) for n in ('full_front', 'bald_front')]
ipd = float(np.mean([np.linalg.norm(r - l) for r, l in eyes]))
fx, fy = np.mean([(r + l) / 2 for r, l in eyes], axis=0)
res = Image.open(os.path.join(VD, 'bald_front.png')).size
mpp = IPD_M / ipd


def concept_mask(name):
    a = np.asarray(Image.open(os.path.join(VD, name + '.png')).convert('RGB')).astype(int)
    bg = np.median(np.concatenate([a[0], a[:, 0], a[:, -1]]), axis=0)
    return ndimage.binary_fill_holes(ndimage.binary_opening(np.abs(a - bg).max(2) > 14, iterations=2))


def face_line(m, rows):
    """The first column of the silhouette per row: the face looks to the left in a left profile."""
    return np.array([np.argmax(m[r]) if m[r].any() else np.nan for r in rows], np.float64)


band = range(int(fy - 0.010 / mpp), int(fy + 0.005 / mpp))
P = {'macro': {'gender': 1.0, 'age': 0.5, 'muscle': 0.5, 'weight': 0.5, 'proportions': 0.5},
     'race': {'caucasian': 1.0, 'asian': 0.0, 'african': 0.0}, 'targets': {}, 'eyes': True, 'quarter': True}
for k in ('macro', 'race', 'targets'):
    if k in cfg: P[k] = dict(P[k], **cfg[k]) if k == 'macro' else cfg[k]
bald_p = concept_mask('bald_profile')
px = float(np.nanmedian(face_line(bald_p, band))) + 0.024 / mpp      # first guess: the eye ~24 mm behind the brow
P.update({'res': list(res), 'ipd_px': round(ipd, 2), 'front_eye': [round(fx, 2), round(fy, 2)],
          'profile_eye': [round(px, 2), round(fy, 2)], 'ref_origin': [0.0, -0.053, 1.5927]})
note = {'pass': 1}

if FIT:
    P0 = json.load(open(os.path.join(FIT, 'params.json')))           # the params fit_head.py rendered with
    a = np.asarray(Image.open(os.path.join(FIT, 'fit_profile.png')).convert('RGBA'))
    tmpl = a[..., 3] > 128
    if tmpl.shape != bald_p.shape: raise SystemExit('auto_params: the fit render and the views differ in size')
    d = face_line(bald_p, band) - face_line(tmpl, band)
    px = P0['profile_eye'][0] + float(np.nanmedian(d))
    P['profile_eye'][0] = round(px, 2)
    eye = np.array(json.load(open(os.path.join(FIT, 'fit.json')))['eye'])
    sc = res[0] / Image.open(os.path.join(VD, 'bald', 'view00_azim000.png')).size[0]

    def centre(path):
        m = np.asarray(Image.open(path).convert('RGBA'))[..., 3] > 128
        r, c = np.nonzero(m.any(1))[0], np.nonzero(m.any(0))[0]
        return (c.min() + c.max()) / 2 * sc, (r.min() + r.max()) / 2 * sc
    origins = []
    for kind in ('full', 'bald'):
        fu, fv = centre(os.path.join(VD, kind, 'view00_azim000.png'))
        pu, pv = centre(os.path.join(VD, kind, 'view01_azim090.png'))
        origins.append([eye[0] + (fu - fx) * mpp, eye[1] + (pu - px) * mpp, eye[2] - ((fv + pv) / 2 - fy) * mpp])
    P['ref_origin'] = [round(float(v), 4) for v in np.mean(origins, axis=0)]
    note = {'pass': 2, 'brow_band_shift_px': round(float(np.nanmedian(d)), 1),
            'origin_full_bald_mm': [round(1000 * float(v), 1) for v in np.subtract(*origins)]}
P['auto'] = note
json.dump(P, open(OUT, 'w'), indent=1)
print('AUTO', json.dumps({k: P[k] for k in ('ipd_px', 'front_eye', 'profile_eye', 'ref_origin', 'auto')}))
