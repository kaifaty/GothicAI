# The tone of the generator's model against the concept it was made from.
#   python tone_ref.py views.json params.json raw.npz front_ref.png tone.json [--place place.json]
# A generator's vertex colour is darker and more saturated than its input (hero 3a: the forehead 166/109/85
# against 184/126/104 on the concept). The model is drawn as the front camera of the concept sees it, and per
# channel concept = gain * model^gamma is fitted over the pixels both have, above the collar: medians of the
# concept's value in 24 bins of the model's value, so stray pixels at outlines do not pull the fit.
import sys, json
import numpy as np
from PIL import Image
from scipy import ndimage
import ref_model

OPT = ref_model.options(sys.argv); OPT['tone'] = None
VJ, PJ, RAW, FRONT, OUT = sys.argv[1:6]
V = json.load(open(VJ)); P = json.load(open(PJ)); eye = np.array(V['eye'])
rv, rc = ref_model.load(np.load(RAW), P, eye, OPT)
F = np.asarray(Image.open(FRONT).convert('RGB'), np.float32); h, w = F.shape[:2]
BG = np.median(np.concatenate([F[0], F[-1], F[:, 0]]), axis=0); mpp = 0.0615/P['ipd_px']; fx, fy = P['front_eye']
u = (fx+rv[:, 0]/mpp).astype(int); v = (fy-(rv[:, 2]-eye[2])/mpp).astype(int); ok = (u >= 0) & (u < w) & (v >= 0) & (v < h)
D = np.full((h, w), 9, np.float32); np.minimum.at(D, (v[ok], u[ok]), rv[ok, 1])
ok &= rv[:, 1] < D[np.clip(v, 0, h-1), np.clip(u, 0, w-1)]+0.001                 # the surface nearest to the camera
acc = np.zeros((h, w, 3)); cnt = np.zeros((h, w)); np.add.at(acc, (v[ok], u[ok]), rc[ok]); np.add.at(cnt, (v[ok], u[ok]), 1)
M = acc/np.maximum(cnt, 1)[..., None]
rows = np.arange(h)[:, None] < fy+0.12/mpp                                         # above the collar
use = ndimage.binary_erosion((cnt > 0) & (np.abs(F-BG).max(2) > 14) & rows, iterations=3)
Fs = ndimage.gaussian_filter(F, (2, 2, 0)); Ms = ndimage.gaussian_filter(M, (2, 2, 0))   # a pixel of misfit does not matter
gain, gamma = [], []
for ch in range(3):
    x, y = Ms[use][:, ch], Fs[use][:, ch]; edges = np.percentile(x, np.linspace(1, 99, 25))
    bx, by = [], []
    for a, b in zip(edges[:-1], edges[1:]):
        m = (x >= a) & (x < b)
        if m.sum() > 50: bx.append(np.median(x[m])); by.append(np.median(y[m]))
    g, k = np.polyfit(np.log(np.array(bx)/255), np.log(np.array(by)/255), 1); gamma.append(float(g)); gain.append(float(np.exp(k)))
json.dump({'gain': gain, 'gamma': gamma}, open(OUT, 'w'))
print('TONE pixels %d gain %s gamma %s' % (use.sum(), np.round(gain, 3), np.round(gamma, 3)))
