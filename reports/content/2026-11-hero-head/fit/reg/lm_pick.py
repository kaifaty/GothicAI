# Landmark targets for a new reference: pixels picked on the concept laid into the landmark frames (concept_frame.py)
# get their depth from the reference; MPFB vertex ids come from the lm_fixed.json kept in git.
#   python lm_pick.py <reg dir> picks.json lm_fixed.json [lid_back_mm]   -> <reg dir>/lm_fixed.json
# lid_back_mm: the lid points take their depth not from the reference (a generator sculpts the eye as a bulge in front
# of the cornea) but from the MPFB lid moved back by this much - the distance from the MPFB cornea to the concept's
# cornea in the profile view.
# picks.json: {"front": {name: [u, v]}, "profile": {name: [u, v]}}; "*_r" ear points are the left picks seen from the
# right side; pronasale, subnasale, nasion, glabella lie on the midline silhouette of the profile.
import sys, os, json
import numpy as np
from scipy.spatial import cKDTree
D = sys.argv[1]; K = json.load(open(sys.argv[2])); L = json.load(open(sys.argv[3]))
V = json.load(open(os.path.join(D, 'views.json'))); r = np.load(os.path.join(D, 'ref.npz'))['v'].astype(np.float64)
N, s = V['N'], V['s']; fc, pc = V['views']['front']['loc'], V['views']['profile']['loc']
LID = float(sys.argv[4]) / 1000 if len(sys.argv) > 4 else None; m0 = np.load(os.path.join(D, 'mpfb.npz'))['co']
MID = ('pronasale', 'subnasale', 'nasion', 'glabella')
tf = cKDTree(r[:, [0, 2]]); tp = cKDTree(r[:, [1, 2]])
def depth(tree, q, axis, pick, rad=0.0008):
    i = tree.query_ball_point(q, rad)
    while not i: rad *= 1.5; i = tree.query_ball_point(q, rad)
    return pick(r[i, axis])
out = []
for lm in L['landmarks']:
    n = lm['name']; base = n[:-2] if n.endswith('_r') and n[:-2] in K['profile'] else n
    if base in K['front']:
        u, v = K['front'][base]; x = fc[0] + (u - N / 2) * s; z = fc[2] - (v - N / 2) * s
        t = [x, depth(tf, [x, z], 1, np.min), z]
        if LID is not None and n[:2] in ('l_', 'r_'): t[1] = m0[lm['mpfb_vertex']][1] + LID
    elif base in K['profile']:
        u, v = K['profile'][base]; y = pc[1] + (u - N / 2) * s; z = pc[2] - (v - N / 2) * s
        if n in MID: t = [0.0, y, z]
        elif n.endswith('_r'): t = [depth(tp, [y, z], 0, np.min), y, z]
        else: t = [depth(tp, [y, z], 0, np.max), y, z]
    else: raise SystemExit('no pick for ' + n)
    out.append({'name': n, 'mpfb_vertex': lm['mpfb_vertex'], 'target': [round(float(c), 5) for c in t]})
json.dump({'note': L['note'], 'landmarks': out}, open(os.path.join(D, 'lm_fixed.json'), 'w'), indent=1)
m = np.load(os.path.join(D, 'mpfb.npz'))['co']
for o in out: print('%-16s move mm' % o['name'], (1000 * (np.array(o['target']) - m[o['mpfb_vertex']])).round(1))
