# The hair of the generator's model as its own part of the head: a light shell over the skull and a tube for the
# tail, with the model's colour baked onto them.
#   python hair_cut.py views.json params.json raw.npz <out dir> [texture size] [--place place.json] [--tone tone.json]
# The model's hair is a torn, layered shell wound both ways: cut out as it is, it does not decimate below 775 000
# triangles, and a voxel remesh loses 40 % of it (the model is not closed). So the surface is rebuilt from the
# model's points instead:
# - hair is told from skin by colour (measured on the model: hair r-b 12..38 and g/r 0.72..0.79, skin r-b 65..87
#   and g/r 0.61..0.68); the cap is hair above the eyes or behind the ears, down to the nape; the beard and the
#   sideburns below the eyes stay on the skin map;
# - the cap is a grid of directions around the skull's centre, poles at the ears; each cell takes the outer radius
#   of the hair points in it. One more ring of cells dives 1 cm under the rim, so no gap opens over the scalp;
# - the tail (the cord is leather, so it is taken by place) is a tube around its own axis, closed at the tip.
# Writes hair.npz (v, f, uv per face corner; metres, the head's frame) and hair.png.
import sys, os, json
import numpy as np
from PIL import Image
from scipy import ndimage
from scipy.spatial import cKDTree
import ref_model

OPT = ref_model.options(sys.argv)
VJ, PJ, RAW, OUT = sys.argv[1:5]; N = int(sys.argv[5]) if len(sys.argv) > 5 else 2048
V = json.load(open(VJ)); P = json.load(open(PJ)); raw = np.load(RAW)
eye = np.array(V['eye']); rv, rc = ref_model.load(raw, P, eye, OPT); rv = rv-eye       # relative to the eyes
x, y, z = rv.T
r0 = ref_model.raw_colour(raw); hairlike = (r0[:, 0]-r0[:, 2] < 50) & (r0[:, 1] > 0.70*r0[:, 0])
tailbox = (y > 0.150) & (z > -0.165) & (z < -0.045) & (np.abs(x) < 0.04)
cap = hairlike & ((y > 0.11) | (z > -0.005)) & (z > -0.085) & ~tailbox      # the shirt is grey like hair: nothing below the nape
cap &= ~((np.abs(x) > 0.078) & (z < 0.015))                             # the shadowed hollows of the ears are dark like hair

# ---- the cap: directions around the skull's centre, the pole axis through the ears
C = np.array([0.0, 0.085, 0.02]); D = 2.0; nt, nf = int(160/D), int(360/D)   # theta 10..170 from +x, phi around x
q = rv[cap]-C; rad = np.linalg.norm(q, axis=1)
th = np.degrees(np.arccos(np.clip(q[:, 0]/rad, -1, 1))); ph = np.degrees(np.arctan2(q[:, 1], -q[:, 2])) % 360   # phi 0 = down
ti = ((th-10)/D).astype(int); fi = (ph/D).astype(int); ok = (ti >= 0) & (ti < nt)
order = np.lexsort((rad[ok], fi[ok], ti[ok])); key = (ti[ok]*nf+fi[ok])[order]; r_sorted = rad[ok][order]
start = np.r_[0, np.nonzero(np.diff(key))[0]+1]; cnt = np.diff(np.r_[start, len(key)])
r90 = r_sorted[start+(cnt*0.9).astype(int).clip(0, cnt-1)]                  # the outer layer, not a stray strand
grid = np.full(nt*nf, np.nan, np.float32); grid[key[start][cnt >= 8]] = r90[cnt >= 8]; grid = grid.reshape(nt, nf)
mask = ndimage.binary_opening(ndimage.binary_closing(~np.isnan(grid), iterations=2), iterations=1)
lab, n = ndimage.label(mask); mask = lab == (1+np.argmax(ndimage.sum(mask, lab, range(1, n+1))))
g0 = np.where(np.isnan(grid), 0, grid); w0 = (~np.isnan(grid)).astype(np.float32)
rg = ndimage.gaussian_filter(g0, 0.8)/np.maximum(ndimage.gaussian_filter(w0, 0.8), 1e-6)   # smoothed, holes filled
rim = ndimage.binary_dilation(mask) & ~mask
rin = ndimage.gaussian_filter(np.where(mask, rg, 0), 1.0)/np.maximum(ndimage.gaussian_filter(mask.astype(np.float32), 1.0), 1e-6)
rcap = np.where(mask, rg, rin-0.010); mcap = mask | rim
tc = np.radians(10+(np.arange(nt)+0.5)*D)[:, None]; fc = np.radians((np.arange(nf)+0.5)*D)[None, :]
dirs = np.stack([np.cos(tc)+0*fc, np.sin(tc)*np.sin(fc), -np.sin(tc)*np.cos(fc)], -1)
pcap = C+dirs*rcap[..., None]

verts, faces, uvs = [], [], []
def add_grid(pg, mg, u0, u1, wrap):
    """Quads between neighbouring cells of a grid of points; uv = the cell's place in the strip u0..u1."""
    a, b = mg.shape; idx = -np.ones((a, b), int); idx[mg] = len(verts)+np.arange(mg.sum())
    verts.extend(pg[mg]); uvg = np.stack(np.meshgrid((np.arange(b)+0.5)/b*(u1-u0)+u0, 1-(np.arange(a)+0.5)/a), -1)
    for i in range(a-1):
        for j in range(b if wrap else b-1):
            j2 = (j+1) % b; c = [idx[i, j], idx[i, j2], idx[i+1, j2], idx[i+1, j]]
            if min(c) < 0: continue
            uq = [uvg[i, j], uvg[i, j2]+([u1-u0, 0] if j2 < j else 0), uvg[i+1, j2]+([u1-u0, 0] if j2 < j else 0), uvg[i+1, j]]
            for t in ((0, 1, 2), (0, 2, 3)):
                faces.append([c[k] for k in t]); uvs.append([uq[k] for k in t])
    return idx
add_grid(pcap, mcap, 0.0, 0.74, False)
ncap = len(faces)

# ---- the tail: a tube around its own axis
tp = rv[tailbox & ((y > 0.160) | (z < -0.07))]; tcen = tp.mean(0)
ax = np.linalg.svd(tp-tcen, full_matrices=False)[2][0]; ax *= -np.sign(ax[2])           # along the tail, downwards
e1 = np.cross(ax, [1, 0, 0]); e1 /= np.linalg.norm(e1); e2 = np.cross(ax, e1)
tq = rv[tailbox]-tcen; tt = tq@ax; tr = np.stack([tq@e1, tq@e2], 1); trad = np.linalg.norm(tr, axis=1)
near = trad < 0.035; t0, t1 = np.percentile(tt[near], [0.5, 99.5]); na, nl = 20, max(8, int((t1-t0)/0.004))
li = ((tt-t0)/(t1-t0)*nl).astype(int); ai = (np.degrees(np.arctan2(tr[:, 1], tr[:, 0])) % 360/360*na).astype(int)
tg = np.full((nl, na), np.nan, np.float32)
for i in range(nl):
    for j in range(na):
        s = trad[near & (li == i) & (ai == j)]
        if len(s) >= 5: tg[i, j] = np.percentile(s, 90)
tw = (~np.isnan(tg)).astype(np.float32); tg0 = np.where(np.isnan(tg), 0, tg)
tg = ndimage.gaussian_filter(tg0, 0.8, mode='wrap')/np.maximum(ndimage.gaussian_filter(tw, 0.8, mode='wrap'), 1e-6)
lc = t0+(np.arange(nl)+0.5)/nl*(t1-t0); ac = np.radians((np.arange(na)+0.5)/na*360)
ptail = tcen+lc[:, None, None]*ax+tg[..., None]*(np.cos(ac)[None, :, None]*e1+np.sin(ac)[None, :, None]*e2)
tidx = add_grid(ptail, np.ones((nl, na), bool), 0.76, 0.99, True)
tip = len(verts); verts.append(tcen+(t1+0.004)*ax)
for j in range(na):                                                                # the tip closed
    faces.append([tidx[-1, j], tidx[-1, (j+1) % na], tip]); uvs.append([[0.76+(j+0.5)/na*0.23, 0.5/nl]]*3)

v = np.array(verts)+eye; f = np.array(faces); uv = np.array(uvs)
# every face turned away from its own centre line: the skull's centre for the cap, the axis for the tail
fcen = v[f].mean(1)-eye; fn = np.cross(v[f[:, 1]]-v[f[:, 0]], v[f[:, 2]]-v[f[:, 0]])
ref = np.where((np.arange(len(f)) < ncap)[:, None], fcen-C, fcen-tcen-((fcen-tcen)@ax)[:, None]*ax)
ref[len(f)-na:] = ax
flip = (fn*ref).sum(1) < 0
f[flip] = f[flip][:, ::-1]; uv[flip] = uv[flip][:, ::-1]
np.savez(os.path.join(OUT, 'hair.npz'), v=v, f=f, uv=uv)

# ---- the colour: every texel's point on the part takes the model's hair colour nearest to it
src = cap | tailbox; tree = cKDTree(rv[src]+eye); csrc = rc[src]
pos = np.zeros((N, N, 3), np.float32); hit = np.zeros((N, N), bool)
for k in range(len(f)):
    a = uv[k]*[N, N]; a[:, 1] = N-a[:, 1]
    x0, y0 = np.floor(a.min(0)).astype(int)-1; x1, y1 = np.ceil(a.max(0)).astype(int)+1
    x0, y0, x1, y1 = max(x0, 0), max(y0, 0), min(x1, N-1), min(y1, N-1)
    X, Y = np.meshgrid(np.arange(x0, x1+1)+0.5, np.arange(y0, y1+1)+0.5)
    T = np.array([[a[0, 0]-a[2, 0], a[1, 0]-a[2, 0]], [a[0, 1]-a[2, 1], a[1, 1]-a[2, 1]]])
    if abs(np.linalg.det(T)) < 1e-9: continue
    l = np.linalg.solve(T, np.stack([X.ravel()-a[2, 0], Y.ravel()-a[2, 1]])); w = np.vstack([l, 1-l.sum(0)]).T
    okk = (w > -0.02).all(1); yy, xx = (Y.ravel()[okk]).astype(int), (X.ravel()[okk]).astype(int)
    pos[yy, xx] = w[okk] @ v[f[k]]; hit[yy, xx] = True
d, i = tree.query(pos[hit], k=3, workers=-1)
w = 1/(d+0.0002)**2; c = (csrc[i]*w[..., None]).sum(1)/w.sum(1)[:, None]
img = np.zeros((N, N, 3), np.float32); img[hit] = c
ix = ndimage.distance_transform_edt(~hit, return_distances=False, return_indices=True)       # pad the islands
Image.fromarray(img[ix[0], ix[1]].clip(0, 255).astype(np.uint8)).save(os.path.join(OUT, 'hair.png'))
print('HAIR cap cells %d (+%d rim), tail %d x %d, length %.0f mm; verts %d tris %d; texel dist median %.2f mm, 95%% %.1f mm' % (
    mask.sum(), rim.sum(), nl, na, 1000*(t1-t0), len(v), len(f), 1000*np.median(d[:, 0]), 1000*np.percentile(d[:, 0], 95)))
