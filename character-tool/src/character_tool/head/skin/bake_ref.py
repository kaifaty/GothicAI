# Colour of the head taken from the generator's own model (raw.npz: vertices, faces, linear vertex colour), baked
# onto the MPFB skin UV of the registered head. The model is coloured from every side, so nothing is guessed from
# two drawings: hair, beard, brows and lips come with it.
#   python bake_ref.py uv.npz groups.npz views.json params.json raw.npz base_skin.png out_dir [size]
#       [--place place.json] [--tone tone.json] [--offsets beard.npz] [--front front_ref.png lid_chains.json]
# Every texel of the head island gets its point and normal on the registered head and takes the colour of the
# nearest points of the model whose normals agree with it. Texels that land on the model's eyeballs or shirt take
# nothing and are filled from skin around them; at the collar the colour fades into the MPFB skin, brought to the
# colour of the model's neck.
# --place, --tone: the model is put on the concept and brought to its tone (ref_model.py).
# --offsets: where the model stands above the skin (the beard over a shaven chin, beard_shell.py), the colour is
#   looked up at the beard's surface, so the skin under the beard is beard-coloured.
# --front: the band of the eyes and brows is taken from the concept's front view instead. A generator sculpts the
#   eyes half shut, and the head's lids are registered to the concept's lid lines, so only the concept fits them.
# Writes skin.png (the whole MPFB map with the head replaced), head_mask.png and bake_raw.png.
import sys, os, json
import numpy as np
from PIL import Image
from scipy import ndimage
from scipy.spatial import cKDTree
import ref_model

OPT = ref_model.options(sys.argv); OFFS = ref_model.take(sys.argv, '--offsets')
FRONT = None
if '--front' in sys.argv:
    i = sys.argv.index('--front'); FRONT = sys.argv[i+1:i+3]; del sys.argv[i:i+3]
UV, GR, VJ, PJ, RAW, BASE, OUT = sys.argv[1:8]
N = int(sys.argv[8]) if len(sys.argv) > 8 else 4096
d = np.load(UV); g = np.load(GR); V = json.load(open(VJ)); P = json.load(open(PJ)); raw = np.load(RAW)
co, no, tri, tuv = d['co'], d['no'], d['tri'], d['tuv']
eye = np.array(V['eye'])

# the model in the head's frame, as reg_export.py places it
rv, rc = ref_model.load(raw, P, eye, OPT); rf = raw['faces']
rn = np.zeros_like(rv); fn = np.cross(rv[rf[:, 1]]-rv[rf[:, 0]], rv[rf[:, 2]]-rv[rf[:, 0]])
for k in range(3): np.add.at(rn, rf[:, k], fn)
rn /= np.linalg.norm(rn, axis=1, keepdims=True)+1e-12
r0 = ref_model.raw_colour(raw); sat = r0.max(1)-r0.min(1); dz = rv[:, 2]-eye[2]
eyeball = (np.abs(np.abs(rv[:, 0])-0.031) < 0.018) & (np.abs(dz+0.003) < 0.007) & (rv[:, 1] < eye[1]+0.02) & ((sat < 50) | (r0[:, 2] > r0[:, 0]*0.9))   # white, iris, pupil; the brow lies higher
shirt = (dz < -0.10) & (sat < 30) & (r0.max(1) > 70)                 # grey cloth; the dark underside of a beard is not cloth
keep = dz > -0.24
rv, rc, rn, bad = rv[keep], rc[keep], rn[keep], (eyeball | shirt)[keep]
tree = cKDTree(rv)

# the head island: skin triangles above the collar whose UVs lie in the island on the right of the map
zc = co[tri].mean(1)[:, 2]; uc = tuv.mean(1)
sel = g['body'][tri].all(1) & (zc > eye[2]-0.20) & (uc[:, 0] > 0.62) & (uc[:, 1] > 0.12) & (uc[:, 1] < 0.84)
pos = np.zeros((N, N, 3), np.float32); nor = np.zeros((N, N, 3), np.float32); hit = np.zeros((N, N), bool)
vdisp = np.load(OFFS)['disp'] if OFFS else np.zeros((len(co), 3))
ofs = np.zeros((N, N, 3), np.float32)
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
    pos[yy, xx] = w[ok] @ co[tri[k]]; nor[yy, xx] = w[ok] @ no[tri[k]]; ofs[yy, xx] = w[ok] @ vdisp[tri[k]]; hit[yy, xx] = True
nor /= np.linalg.norm(nor, axis=2, keepdims=True)+1e-9

K = 8; p = pos[hit]; n = nor[hit]
dist, idx = tree.query(p+ofs[hit], k=K, distance_upper_bound=0.045, workers=-1)
ok = np.isfinite(dist); idx = np.where(ok, idx, 0)
# under the hair the scalp lies up to 4.5 cm below the model's surface; elsewhere a far point is another feature
reach = np.where(p[:, 2] > eye[2]+0.03, 0.045, 0.007)[:, None]
ok &= (dist < reach) & (dist < dist[:, :1]*1.5+0.0007) & ~bad[idx]
ok &= ~bad[idx[:, :1]]                                          # nearest is an eyeball or the shirt: nothing to take
agree = np.einsum('kj,knj->kn', n, rn[idx])
w = ok*agree**2/(dist.clip(1e-4, 1)+0.0005)**2             # the model is a thin shell wound both ways: the sign says nothing
w = np.where(ok, w, 0); ws = w.sum(1)
c = (rc[idx]*w[..., None]).sum(1)/np.maximum(ws, 1e-9)[:, None]

if FRONT:
    # The band of the eyes and brows as the concept's front view shows it. The open eye of the concept (white and
    # iris between the lids) and the folds that do not face the camera take nothing and are filled from around.
    F = np.asarray(Image.open(FRONT[0]).convert('RGB'), np.float32); LC = json.load(open(FRONT[1]))
    BG = np.median(np.concatenate([F[0], F[-1], F[:, 0]]), axis=0); mpp = 0.0615/P['ipd_px']; fx, fy = P['front_eye']
    u = np.clip(fx+p[:, 0]/mpp, 0, F.shape[1]-1.001); v = np.clip(fy-(p[:, 2]-eye[2])/mpp, 0, F.shape[0]-1.001)
    i, j = u.astype(int), v.astype(int); a, b = (u-i)[:, None], (v-j)[:, None]
    cz = F[j, i]*(1-a)*(1-b)+F[j, i+1]*a*(1-b)+F[j+1, i]*(1-a)*b+F[j+1, i+1]*a*b
    facing = (n[:, 1] < -0.15) & (p[:, 1] < eye[1]+0.03)
    box = np.zeros(len(p)); opening = np.zeros(len(p), bool); sat = cz.max(1)-cz.min(1)
    for side in LC.values():
        cx, _, cv = co[sorted(set(side['upper']+side['lower']))].mean(0)        # the centre of this eye's lid line
        dx = np.abs(p[:, 0]-cx); dv = p[:, 2]-cv
        box = np.maximum(box, np.clip(np.minimum((0.028-dx)/0.005, np.minimum((0.026-dv)/0.005, (dv+0.017)/0.005)), 0, 1))
        opening |= (dx < 0.019) & (np.abs(dv) < 0.008) & ((sat < 38) | (cz[:, 2] > cz[:, 0]))
    box *= facing
    kz = (np.abs(cz-BG).max(1) > 14) & ~opening
    mix = np.where(ws > 0, box, (box > 0)*1.0)[:, None]
    c = np.where(kz[:, None], cz*mix+c*(1-mix), c)
    ws = np.where(kz & (box > 0), np.maximum(ws, 1), np.where(box >= 1, 0, ws))
    inband = np.zeros(len(p), bool)
    for side in LC.values():
        cx, _, cv = co[sorted(set(side['upper']+side['lower']))].mean(0)
        inband |= (np.abs(p[:, 0]-cx) < 0.023) & (np.abs(p[:, 2]-cv) < 0.012)
    ws = np.where(inband & ~facing & (p[:, 1] < eye[1]+0.03), 0, ws)   # folds of the lids and the socket: nothing to take
    print('EYE BAND texels %d, from the concept %d' % ((box > 0).sum(), (kz & (box > 0)).sum()))
col = np.zeros((N, N, 3), np.float32); col[hit] = c
near = np.zeros((N, N), np.float32); near[hit] = np.where(ok[:, 0], dist[:, 0], 1)
z = pos[..., 2]; y = pos[..., 1]
low = np.where(y < eye[1]+0.03, 0.135, np.clip(0.135+(y-eye[1]-0.03)*0.5, 0.135, 0.16))   # the collar: 13 cm under the eyes in front, 16 behind
fade = np.clip((z-(eye[2]-low))/0.015, 0, 1)
known = np.zeros((N, N), bool); known[hit] = ws > 0; known &= fade > 0
Image.fromarray(np.where(known[..., None], col, 0).clip(0, 255).astype(np.uint8)).save(os.path.join(OUT, 'bake_raw.png'))

# fill what is unknown inside the island, and pad the island outwards, from the nearest known texel; then soften
ix = ndimage.distance_transform_edt(~known, return_distances=False, return_indices=True)
filled = col[ix[0], ix[1]]
soft = ndimage.gaussian_filter(filled, (6, 6, 0)); far = ndimage.distance_transform_edt(~known)
t = np.clip(far/8, 0, 1)[..., None]; filled = filled*(1-t)+soft*t

base = np.asarray(Image.open(BASE).convert('RGB').resize((N, N), Image.LANCZOS), np.float32)
skinlike = known & (col.max(2)-col.min(2) > 40) & (col.max(2) > 130)
neck = skinlike & (z < eye[2]-0.09) & (np.abs(pos[..., 0]) > 0.035)       # the base takes the colour of the neck's sides
if neck.sum() < 500: neck = skinlike
gain = np.median(col[neck], axis=0)/np.median(base[neck], axis=0); base_m = np.clip(base*gain, 0, 255)
pad = ndimage.binary_dilation(hit, iterations=12)
a = np.where(hit, fade, 1.0)[..., None]*pad[..., None]
out = filled*a+base_m*(1-a)
Image.fromarray(out.clip(0, 255).astype(np.uint8)).save(os.path.join(OUT, 'skin.png'))
Image.fromarray((hit*255).astype(np.uint8)).save(os.path.join(OUT, 'head_mask.png'))
print('BAKE texels %d known %.1f%% median dist %.1f mm, >5 mm %.1f%%, gain %s' % (
    hit.sum(), 100*known.sum()/hit.sum(), 1000*np.median(dist[np.isfinite(dist[:, 0]), 0]), 100*(dist[np.isfinite(dist[:, 0]), 0] > 0.005).mean(), gain.round(3)))
