# Colour of the head from the concept's two views, projected onto the MPFB skin UV of the registered head.
#   python project.py uv.npz groups.npz views.json params.json front.png profile.png base_skin.png out_dir [size] [r g b]
# r g b: gain on the concept's colour, found from game frames: the concept is darker and redder than the original's
# face under the game's light.
# Every texel of the head island gets its point and normal on the registered head; the point is looked up in the
# front view and in the profile (mirrored for the right side) and the two are mixed by how the normal faces each
# camera. Texels that see the concept's background, the shirt or the open eye are filled from their neighbours;
# at the shirt line the colour fades into the MPFB skin, brought to the colour of the projected forehead.
# Writes skin.png (the whole MPFB map with the head replaced), head_mask.png and proj_raw.png.
import sys, os, json
import numpy as np
from PIL import Image
from scipy import ndimage

UV, GR, VJ, PJ, FRONT, PROF, BASE, OUT = sys.argv[1:9]
N = int(sys.argv[9]) if len(sys.argv) > 9 else 4096
d = np.load(UV); g = np.load(GR); V = json.load(open(VJ)); P = json.load(open(PJ))
co, no, tri, tuv = d['co'], d['no'], d['tri'], d['tuv']
eye = np.array(V['eye']); mpp = 0.0615/P['ipd_px']; (fx, fy), (px, py) = P['front_eye'], P['profile_eye']
F = np.asarray(Image.open(FRONT).convert('RGB'), np.float32); S = np.asarray(Image.open(PROF).convert('RGB'), np.float32)
BG = np.median(np.concatenate([F[0], F[-1], F[:, 0]]), axis=0)
GAIN = np.array([float(v) for v in sys.argv[10:13]], np.float32) if len(sys.argv) > 12 else np.ones(3, np.float32)

# the head island: skin triangles above the collar whose UVs lie in the island on the right of the map
zc = co[tri].mean(1)[:, 2]; uc = tuv.mean(1)
sel = g['body'][tri].all(1) & (zc > eye[2]-0.20) & (uc[:, 0] > 0.62) & (uc[:, 1] > 0.12) & (uc[:, 1] < 0.84)
pos = np.zeros((N, N, 3), np.float32); nor = np.zeros((N, N, 3), np.float32); hit = np.zeros((N, N), bool)
for k in np.nonzero(sel)[0]:
    a = tuv[k]*[N, N]; a[:, 1] = N-a[:, 1]
    x0, y0 = np.floor(a.min(0)).astype(int)-1; x1, y1 = np.ceil(a.max(0)).astype(int)+1
    x0, y0, x1, y1 = max(x0, 0), max(y0, 0), min(x1, N-1), min(y1, N-1)
    X, Y = np.meshgrid(np.arange(x0, x1+1)+0.5, np.arange(y0, y1+1)+0.5)
    T = np.array([[a[0, 0]-a[2, 0], a[1, 0]-a[2, 0]], [a[0, 1]-a[2, 1], a[1, 1]-a[2, 1]]])
    if abs(np.linalg.det(T)) < 1e-9: continue
    l = np.linalg.solve(T, np.stack([X.ravel()-a[2, 0], Y.ravel()-a[2, 1]])); w = np.vstack([l, 1-l.sum(0)]).T
    ok = (w > -0.02).all(1)                                    # a small overdraw so neighbours leave no gaps
    yy, xx = (Y.ravel()[ok]).astype(int), (X.ravel()[ok]).astype(int)
    pos[yy, xx] = w[ok] @ co[tri[k]]; nor[yy, xx] = w[ok] @ no[tri[k]]; hit[yy, xx] = True
nor /= np.linalg.norm(nor, axis=2, keepdims=True)+1e-9

def sample(img, u, v):
    h, w = img.shape[:2]; u = np.clip(u, 0, w-1.001); v = np.clip(v, 0, h-1.001)
    i, j = u.astype(int), v.astype(int); a, b = (u-i)[..., None], (v-j)[..., None]
    return img[j, i]*(1-a)*(1-b)+img[j, i+1]*a*(1-b)+img[j+1, i]*(1-a)*b+img[j+1, i+1]*a*b

x, y, z = pos[..., 0], pos[..., 1], pos[..., 2]
cf = sample(F, fx+x/mpp, fy-(z-eye[2])/mpp); cs = sample(S, px+(y-eye[1])/mpp, py-(z-eye[2])/mpp)
wf = np.clip(-nor[..., 1], 0, 1)**2; ws = np.clip(np.abs(nor[..., 0]), 0, 1)**2+0.02
wf *= np.abs(cf-BG).max(2) > 14; ws *= np.abs(cs-BG).max(2) > 14           # the concept's background says nothing
# the eye opening of the concept (white and iris between the lids) must not land on the lids
lum = cf.mean(2); sat = cf.max(2)-cf.min(2)
eye_px = (np.abs(np.abs(x)-0.031) < 0.022) & (np.abs(z-eye[2]-0.001) < 0.011) & ((sat < 38) | (cf[..., 2] > cf[..., 0]))
wf *= ~eye_px; ws *= ~((np.abs(z-eye[2]-0.001) < 0.011) & (y < eye[1]+0.01) & ((cs.max(2)-cs.min(2) < 38) | (cs[..., 2] > cs[..., 0])))
low = np.where(y < eye[1]+0.03, 0.135, np.clip(0.135+(y-eye[1]-0.03)*0.5, 0.135, 0.16))   # the shirt: 13 cm under the eyes in front, 16 behind
fade = np.clip((z-(eye[2]-low))/0.015, 0, 1)
wsum = wf+ws; col = (cf*wf[..., None]+cs*ws[..., None])/np.maximum(wsum, 1e-6)[..., None]
col = np.clip(col*GAIN, 0, 255)
known = hit & (wsum > 0.03) & (fade > 0)
Image.fromarray(np.where(known[..., None], col, 0).clip(0, 255).astype(np.uint8)).save(os.path.join(OUT, 'proj_raw.png'))

# fill what is unknown inside the island, and pad the island outwards, from the nearest known texel; then soften
idx = ndimage.distance_transform_edt(~known, return_distances=False, return_indices=True)
filled = col[idx[0], idx[1]]
soft = ndimage.gaussian_filter(filled, (6, 6, 0)); dist = ndimage.distance_transform_edt(~known)
t = np.clip(dist/8, 0, 1)[..., None]; filled = filled*(1-t)+soft*t

base = np.asarray(Image.open(BASE).convert('RGB').resize((N, N), Image.LANCZOS), np.float32)
neck = known & (z > eye[2]+0.025) & (z < eye[2]+0.045) & (wf > 0.5) & (np.abs(x) < 0.03)   # the base takes the colour of the forehead
gain = np.median(col[neck], axis=0)/np.median(base[neck], axis=0); base_m = np.clip(base*gain, 0, 255)
pad = ndimage.binary_dilation(hit, iterations=12)
a = np.where(hit, fade, 1.0)[..., None]*pad[..., None]
out = filled*a+base_m*(1-a)
Image.fromarray(out.clip(0, 255).astype(np.uint8)).save(os.path.join(OUT, 'skin.png'))
Image.fromarray((hit*255).astype(np.uint8)).save(os.path.join(OUT, 'head_mask.png'))
print('PROJECT texels %d known %.1f%% gain %s' % (hit.sum(), 100*known.sum()/hit.sum(), gain.round(3)))
