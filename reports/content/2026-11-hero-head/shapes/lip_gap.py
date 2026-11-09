# Guard rail of face-shapes.md: how often the lower lip rides up over the upper one while the jaw is nearly closed.
# Plays the Audio2Face weights on each shape set at the midline lip contact and measures the gap along "down".
#   python lip_gap.py <work dir> <weights dir>      (in ~/.local/share/gothic-ai/mpfb/venv)
# <clip>.smpfb.csv and <clip>.sict.csv are weights from the solver given those shapes (transfer.py step 6).
import os, sys, glob
import numpy as np
WD, WT = (os.path.expanduser(a) for a in sys.argv[1:3])
ARKIT = open(os.path.join(os.path.dirname(__file__), '../a2f/render_a2f.py')).read()
ARKIT = ARKIT[ARKIT.index('SKIN = ("')+8:ARKIT.index('.split()')].replace('"', '').replace('\n', ' ').replace('(', '').replace(')', '')
ARKIT = ARKIT.split()
h = np.load(os.path.join(WD, 'hero.npz'))
V = h['face|co']
sets = {'mpfb': {n: h['face|sk|'+n]-V for n in ARKIT}}
for s in ('ict', 'mark'):
    d = np.load(os.path.join(WD, s+'.npz'))
    sets[s] = {n: d['face|'+n]-V for n in ARKIT}
# the midline contact: the closest pair of an upper and a lower lip vertex in front of the mouth bag
jaw = np.linalg.norm(sets['mpfb']['jawOpen'], axis=1)
unit = lambda v: v/np.linalg.norm(v)
cl = V[np.argsort(-np.linalg.norm(sets['mpfb']['mouthSmileLeft'], axis=1))[:3]].mean(0)
cr = V[np.argsort(-np.linalg.norm(sets['mpfb']['mouthSmileRight'], axis=1))[:3]].mean(0)
mc, left = (cl+cr)/2, unit(cl-cr)
eyes = (h['eye_L|co'].mean(0)+h['eye_R|co'].mean(0))/2
down = unit(mc-eyes); down = unit(down-left*(down @ left))
fwd = unit(np.cross(down, left))
pk = sets['mpfb']['mouthPucker'][np.linalg.norm(sets['mpfb']['mouthPucker'], axis=1) > 0.004].mean(0)
fwd = fwd if pk @ fwd > 0 else -fwd
pc = V[np.argsort(-np.linalg.norm(sets['mpfb']['mouthPucker'], axis=1))[:5]].mean(0)   # the lips' front
mid = (np.abs((V-pc) @ left) < 0.002) & (np.linalg.norm(V-pc, axis=1) < 0.015)
front = mid & ((V-pc) @ fwd > -0.006)
lo = np.nonzero(front & (jaw > 0.4*jaw[front].max()))[0]
up = np.nonzero(front & (jaw < 0.2*jaw[front].max()))[0]
iu, il = up[np.argmax(((V[up]-pc) @ down))], lo[np.argmin(((V[lo]-pc) @ down))]
print('contact: upper %d lower %d, rest gap %.2f mm' % (iu, il, ((V[il]-V[iu]) @ down)*1000))
def gaps(s, w):
    D = np.stack([sets[s][n][[iu, il]] for n in ARKIT])          # (52, 2, 3)
    G, J = [], []
    for f in sorted(glob.glob(os.path.join(WT, '*.%s.csv' % w))):
        W = np.loadtxt(f, delimiter=',')[:, 1:1+len(ARKIT)]
        P = V[[iu, il]][None]+np.einsum('fk,kpc->fpc', W, D)
        G.append((P[:, 1]-P[:, 0]) @ down); J.append(W[:, ARKIT.index('jawOpen')])
    return (np.concatenate(G), np.concatenate(J)) if G else (None, None)
# the reference is what the network itself drew: Mark's shapes with its weights. Where its lips close (an m, p, b),
# a set must close them too: a set that never rides over may just never close
ref, _ = gaps('mark', 'v3')
rows = []
for col, (s, w) in {'MPFB с поправкой': ('mpfb', 'v3fix'), 'MPFB без поправки': ('mpfb', 'v3'),
                    'ICT с поправкой': ('ict', 'v3fix'), 'ICT без поправки': ('ict', 'v3'),
                    'Mark без поправки': ('mark', 'v3'),
                    'MPFB, решатель под MPFB': ('mpfb', 'smpfb'), 'ICT, решатель под ICT': ('ict', 'sict')}.items():
    gap, jaw_w = gaps(s, w)
    if gap is None:
        continue
    c = jaw_w < 0.15
    over = (c & (gap < -0.002)).sum()/c.sum()
    shut = ref < 0.001
    open_ = (shut & (gap > 0.003)).sum()/shut.sum()
    rows.append((col, over, gap[c].min(), open_))
    print('%-24s over the upper > 2 mm: %4.1f %% of near-closed frames, worst %5.1f mm | open > 3 mm where Mark '
          'closes: %4.1f %% | gap vs Mark: mean %.1f mm' % (col, 100*over, gap[c].min()*1000, 100*open_,
                                                          np.abs(gap-ref).mean()*1000))
