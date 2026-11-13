"""The hair of the full model as a layer over our head: where it is, its outer surface, the tail.

  python hair_layer.py views.json params.json raw.npz uv.npz groups.npz <out dir> [--place place.json] [--ears ears.npz]

raw.npz is the generator's full model (with hair), placed onto the head (place_face.py); uv.npz and groups.npz are
the head (dump_uv.py: co, tri; body). The head is fitted to the bald model of the same person, so hair is where the
full model stands over the head's skin, and how far it stands is how thick the hair is. No colour is asked: the
thresholds that told hair from skin on hero 3a (hair_cut.py) do not hold for grey or fair hair, and they took the
shadowed hollows of the ears and a shirt of another colour for hair.
- The grid is the one of hair_cut.py and groom.py: directions round the skull's centre, poles at the ears. A cell's
  skin is the head's outermost skin in it, its hair the outer surface of the model (97 % of its points lie inside:
  a stray strand does not count). Hair is there where the model stands 3 mm over the skin, above the eyes or behind
  the ears, down to the nape; not on an ear (ear_points.py --mask): the model's ear is not the head's to the
  millimetre.
- The tail is found by its shape, not by its place: in the slab of the model 2 cm either side of the middle, seen
  from the side, the hair of the head is a band a centimetre thick and the tail is a lump hanging on it. The lump
  is what is left of the points behind the head, clear of the skin, when everything narrower than 2 cm is taken
  away; the tail is the lump from its waist (the cord) on. It is a tube round its own axis, as before, and is put
  in the middle of the head wherever the model hangs it.
Writes cap.npz for groom.py (C, D, r, mask, tail_c, tail_ax, tail_e1, tail_e2, tail_l, tail_r; and skin, thick: the
head's skin and the hair over it by cell, metres) and layer.jpg: the thickness by cell with the outline of the hair
and the ears, and the middle slab from the side with the tail.
"""
import sys, os, json
import numpy as np
from PIL import Image
from scipy import ndimage
from scipy.spatial import cKDTree

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', '..', '..', 'reports', 'content', '2026-11-hero-head', 'skin'))
import ref_model

OPT = ref_model.options(sys.argv)
EARS = None
if '--ears' in sys.argv:
    i = sys.argv.index('--ears'); EARS = np.load(sys.argv[i+1])['ear'].astype(bool); del sys.argv[i:i+2]
VJ, PJ, RAW, UV, GR, OUT = sys.argv[1:7]
V = json.load(open(VJ)); P = json.load(open(PJ)); eye = np.array(V['eye'])
fv, fc = ref_model.load(np.load(RAW), P, eye, OPT); fv = fv.astype(np.float64)-eye          # relative to the eyes
d = np.load(UV); co = d['co'].astype(np.float64)-eye; body = np.load(GR)['body'].astype(bool); tri = d['tri'][body[d['tri']].all(1)]
THICK, CLEAR, NARROW, SLAB = 0.003, 0.008, 0.010, 0.020


def samples(p, t, step=0.001):
    """Points over the triangles."""
    a, b, c = p[t[:, 0]], p[t[:, 1]], p[t[:, 2]]; n = np.ceil(np.maximum(np.linalg.norm(b-a, axis=1), np.linalg.norm(c-a, axis=1))/step).astype(int).clip(1, 40)
    out = []
    for k in np.unique(n):
        s = n == k; u, w = np.mgrid[0:k+1, 0:k+1]; ok = u+w <= k; u, w = u[ok]/k, w[ok]/k
        out.append((a[s][:, None]+(b[s]-a[s])[:, None]*u[None, :, None]+(c[s]-a[s])[:, None]*w[None, :, None]).reshape(-1, 3))
    return np.concatenate(out)


hs = samples(co, tri[co[tri][:, :, 2].mean(1) > -0.26]); tree = cKDTree(hs)

# ---- the tail: the lump behind the head in the middle slab, seen from the side
CELL = 0.001; Y0, Y1, Z0, Z1 = 0.08, 0.34, -0.30, 0.04; ny, nz = int(round((Y1-Y0)/CELL)), int(round((Z1-Z0)/CELL))
behind = (fv[:, 1] > Y0) & (fv[:, 1] < Y1) & (fv[:, 2] > Z0) & (fv[:, 2] < Z1) & (np.abs(fv[:, 0]) < 0.09)
bi = np.nonzero(behind)[0]; clear = tree.query(fv[bi], workers=-1)[0] > CLEAR; bi = bi[clear]
iy, iz = ((fv[bi, 1]-Y0)/CELL).astype(int).clip(0, ny-1), ((fv[bi, 2]-Z0)/CELL).astype(int).clip(0, nz-1)
k = int(NARROW/CELL); yy, zz = np.mgrid[-k:k+1, -k:k+1]; DISC = np.hypot(yy, zz) <= k


def find_lump(xc):
    """The lump of the slab round x = xc: (the slab's points, the lump's cells) or None."""
    m = np.abs(fv[bi, 0]-xc) < SLAB; slab = np.zeros((ny, nz), bool); slab[iy[m], iz[m]] = True
    slab = ndimage.binary_fill_holes(ndimage.binary_closing(slab, iterations=2)); lump = ndimage.binary_opening(slab, structure=DISC)
    lab, n = ndimage.label(lump)
    if not n: return None
    lump = lab == 1+np.argmax(ndimage.sum(lump, lab, range(1, n+1)))
    if lump.sum()*CELL*CELL < 4e-4: return None                             # under 4 cm2: not a tail
    # the whole lump: the opening rounds it off, and the hairs at its tip are thinner than 2 cm
    return m, ndimage.binary_propagation(lump, mask=slab & ndimage.binary_dilation(lump, iterations=int(0.006/CELL)))


found = find_lump(0.0)
if found is None: raise SystemExit('hair_layer: no tail behind the head (groom.py knows one hairstyle: combed back into a tail)')
# A generator that saw the head from the front and the side hangs the tail where it likes across the head (the second
# face: 24 mm to the left). It is measured where it hangs: the slab is moved to its middle and the lump found again.
for _ in range(2):
    m, lump = found; side_x = fv[bi[lump[iy, iz]], 0]; xc = float(np.median(side_x)); found = find_lump(xc) or found
m, lump = found; tail = lump[iy, iz]; wide = np.abs(fv[bi, 0]-xc); tail &= wide < np.percentile(wide[tail & (wide < 0.07)], 97)+0.003
ti_ = bi[tail]; tp = fv[ti_]; tcen = tp.mean(0)
ax = np.linalg.svd(tp-tcen, full_matrices=False)[2][0]; C = np.array([0.0, 0.085, 0.02]); ax *= np.sign(ax@(tcen-C))   # along the tail, away from the head
e1 = np.cross(ax, [1, 0, 0]); e1 /= np.linalg.norm(e1); e2 = np.cross(ax, e1)
tq = tp-tcen; tt = tq@ax; trr = np.stack([tq@e1, tq@e2], 1); trad = np.linalg.norm(trr, axis=1)
t0, t1 = np.percentile(tt, [0.5, 99.5]); na, nl = 20, max(8, int((t1-t0)/0.004))
li = ((tt-t0)/(t1-t0)*nl).astype(int); ai = (np.degrees(np.arctan2(trr[:, 1], trr[:, 0])) % 360/360*na).astype(int)
tg = np.full((nl, na), np.nan, np.float32)
for i in range(nl):
    for j in range(na):
        s = trad[(li == i) & (ai == j)]
        if len(s) >= 5: tg[i, j] = np.percentile(s, 90)
tw = (~np.isnan(tg)).astype(np.float32); tg0 = np.where(np.isnan(tg), 0, tg)
tg = ndimage.gaussian_filter(tg0, 0.8, mode='wrap')/np.maximum(ndimage.gaussian_filter(tw, 0.8, mode='wrap'), 1e-6)
lc = t0+(np.arange(nl)+0.5)/nl*(t1-t0)
# The lump is the tail and, over the cord, the knot of hair it hangs on (as thick as the tail). The cord is the waist
# between the two: the ring that lies deepest under the widest rings on both its sides. No waist: the lump is the tail.
rm = np.median(tg, axis=1); waist = np.array([min(rm[:i].max(), rm[i+1:].max())-rm[i] if 0 < i < nl-1 else 0 for i in range(nl)])
ic = int(np.argmax(waist)) if waist.max() > 0.15*rm[int(np.argmax(waist))] else 0
knot_mm = 1000*(lc[ic]-t0); ti_ = ti_[tt >= lc[ic]-0.002]; lc, tg, t0 = lc[ic:], tg[ic:], lc[ic]-0.002
# the tail is put in the middle of the head, its axis in the middle plane
side_mm = 1000*tcen[0]; tcen_put = tcen*[0, 1, 1]; ax_put = ax*[0, 1, 1]; ax_put /= np.linalg.norm(ax_put)
e1_put = np.cross(ax_put, [1, 0, 0]); e1_put /= np.linalg.norm(e1_put); e2_put = np.cross(ax_put, e1_put)

# ---- the layer: the model's outer surface over the head's outermost skin, by cell
D = 2.0; nt, nf = int(160/D), int(360/D)                                    # theta 10..170 from +x, phi round x, 0 = down


def cells(p):
    q = p-C; r = np.linalg.norm(q, axis=1); th = np.degrees(np.arccos(np.clip(q[:, 0]/r, -1, 1))); ph = np.degrees(np.arctan2(q[:, 1], -q[:, 2])) % 360
    ti = ((th-10)/D).astype(int); fi = (ph/D).astype(int) % nf; ok = (ti >= 0) & (ti < nt)
    return (ti*nf+fi)[ok], r[ok]


def outer(p, share, least):
    """By cell: the radius that `share` of the points lie inside; nan where there are under `least` points."""
    key, r = cells(p); o = np.lexsort((r, key)); key, r = key[o], r[o]
    st = np.r_[0, np.nonzero(np.diff(key))[0]+1]; cnt = np.diff(np.r_[st, len(key)])
    g = np.full(nt*nf, np.nan); val = r[st+(cnt*share).astype(int).clip(0, cnt-1)]; g[key[st][cnt >= least]] = val[cnt >= least]
    return g.reshape(nt, nf)


head = np.ones(len(fv), bool); head[ti_] = False
skin = outer(hs, 1.0, 1); hair = outer(fv[head], 0.97, 8); thick = hair-skin
tc = np.radians(10+(np.arange(nt)+0.5)*D)[:, None]; pc = np.radians((np.arange(nf)+0.5)*D)[None, :]
dirs = np.stack([np.cos(tc)+0*pc, np.sin(tc)*np.sin(pc), -np.sin(tc)*np.cos(pc)], -1); sp = C+dirs*np.nan_to_num(hair)[..., None]
place = ((sp[..., 1] > 0.11) | (sp[..., 2] > -0.005)) & (sp[..., 2] > -0.085)   # above the eyes or behind the ears, down to the nape
ear = np.zeros((nt, nf), bool)
if EARS is not None and EARS.any():
    ear = ndimage.binary_dilation(~np.isnan(outer(samples(co, tri[EARS[tri].any(1)]), 1.0, 1)), iterations=1)
mask = np.nan_to_num(thick, nan=0) > THICK; mask &= place & ~ear
mask = ndimage.binary_opening(ndimage.binary_closing(mask, iterations=2), iterations=1) & ~ear
lab, n = ndimage.label(mask); mask = lab == (1+np.argmax(ndimage.sum(mask, lab, range(1, n+1))))
mask = ndimage.binary_fill_holes(mask) & ~ear                               # under the tail the model has nothing: hair all round is hair
got = mask & ~np.isnan(hair); g0 = np.where(got, hair, 0)                    # smoothed inside the hair, holes filled from it
rg = ndimage.gaussian_filter(g0, 0.8, mode=('nearest', 'wrap'))/np.maximum(ndimage.gaussian_filter(got.astype(float), 0.8, mode=('nearest', 'wrap')), 1e-6)
ix = ndimage.distance_transform_edt(~got, return_distances=False, return_indices=True); rg = np.where(ndimage.binary_dilation(got, iterations=3), rg, hair[ix[0], ix[1]])
os.makedirs(OUT, exist_ok=True)
np.savez(os.path.join(OUT, 'cap.npz'), C=C+eye, D=D, r=rg.astype(np.float32), mask=mask, tail_c=tcen_put+eye, tail_ax=ax_put, tail_e1=e1_put, tail_e2=e2_put,
         tail_l=lc, tail_r=tg, skin=skin.astype(np.float32), thick=thick.astype(np.float32))

# ---- the check picture
K = 5; t_ = np.clip(np.nan_to_num(thick, nan=0)/0.04, 0, 1); img = np.stack([t_*255]*3, -1).astype(np.uint8); img[np.isnan(thick)] = (40, 40, 90)
img = np.kron(img, np.ones((K, K, 1), np.uint8))
for mk, c in ((mask, (255, 200, 0)), (ear, (255, 60, 60))):
    e = np.kron(mk, np.ones((K, K), bool)); img[e & ~ndimage.binary_erosion(e)] = c
side = np.zeros((nz, ny, 3), np.uint8)+30                                    # y to the right, z up
side[(nz-1-iz[m]), iy[m]] = fc[bi[m]].astype(np.uint8)
e = lump & ~ndimage.binary_erosion(lump, iterations=2); side[nz-1-np.nonzero(e)[1], np.nonzero(e)[0]] = (255, 200, 0)
mid = hs[np.abs(hs[:, 0]) < 0.003]; a, b = ((mid[:, 1]-Y0)/CELL).astype(int), ((mid[:, 2]-Z0)/CELL).astype(int); ok = (a >= 0) & (a < ny) & (b >= 0) & (b < nz)
side[nz-1-b[ok], a[ok]] = (255, 255, 255)
for l in np.linspace(t0, t1, 200):                                           # the tail's axis, from the cord to the tip
    q = tcen+l*ax; a, b = int((q[1]-Y0)/CELL), int((q[2]-Z0)/CELL)
    if 1 <= a < ny-1 and 1 <= b < nz-1: side[nz-2-b:nz+1-b, a-1:a+2] = (255, 60, 60)
side = np.asarray(Image.fromarray(side).resize((ny*img.shape[0]//nz, img.shape[0])))
Image.fromarray(np.hstack([img, side])).save(os.path.join(OUT, 'layer.jpg'), quality=90)
th = thick[mask & ~np.isnan(thick)]*1000
print('LAYER hair cells %d of %d with the model 3 mm over the skin (ears %d cells); thickness mm: median %.1f, 10%% %.1f, 90%% %.1f, max %.1f' % (
    mask.sum(), (np.nan_to_num(thick, nan=0) > THICK).sum(), ear.sum(), np.median(th), np.percentile(th, 10), np.percentile(th, 90), th.max()))
print('LAYER tail: %d points, %.0f mm long from the cord (%.0f mm of the lump are the knot over it) at y %+.0f z %+.0f mm to the tip at y %+.0f z %+.0f; radius mm: median %.1f, widest %.1f; it hangs %+.0f mm off the middle (+ = left) and is put in the middle' % (
    len(ti_), 1000*(t1-t0), knot_mm, *(1000*(tcen+t0*ax)[1:]), *(1000*(tcen+t1*ax)[1:]), 1000*np.median(tg), 1000*tg.max(), side_mm))
