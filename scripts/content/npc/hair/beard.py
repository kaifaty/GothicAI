# Facial hair as strands: the beard, the moustache and the stubble of a head, grown from what the pipeline measures.
#   python beard.py uv.npz groups.npz views.json skin.png beard.npz template_points.json <out dir> [seed, 1]
# Input:
# - uv.npz, groups.npz: the registered head (co, no, tri, tuv; body);
# - skin.png: the colour baked from the generator's model (bake_ref.py). Where a texel is hair-coloured, hair grows:
#   the share of such texels around a point is the cover of facial hair there;
# - beard.npz: `disp` of beard_shell.py, how far the beard of the model stands over every skin vertex. Under 3 mm
#   it is stubble (and the slack of the registration), over 5.5 mm a beard whose hairs reach that surface;
# - template_points.json: MediaPipe index -> vertex of the MPFB mesh, the same for every head: lips and nose.
# Output:
# - beard_groom.npz: pts (hairs, 12, 3) metres, root first; rad; tint -1..1; kind 3 beard, 4 moustache, 5 stubble;
# - skin_shaved.png: the map with the painted beard taken off. Hair painted on the skin under real hairs reads as
#   dirt, so the paint is replaced by the skin colour around it; a part of its shade stays (30 % under stubble,
#   70 % under the beard, where the skin lies in shadow).
import sys, os, json
import numpy as np
from PIL import Image
from scipy import ndimage
from scipy.spatial import cKDTree

UV, GR, VJ, SKIN, BEARD, TP, OUT = sys.argv[1:8]; SEED = int(sys.argv[8]) if len(sys.argv) > 8 else 1
rng = np.random.default_rng(SEED); K = 12
d = np.load(UV); g = np.load(GR); eye = np.array(json.load(open(VJ))['eye'])
co, no, tri, tuv = d['co'].astype(np.float64), d['no'].astype(np.float64), d['tri'], d['tuv'].astype(np.float64)
th_v = np.linalg.norm(np.load(BEARD)['disp'], axis=1)
tp = {int(k): v['vertex'] for k, v in json.load(open(TP))['points'].items()}
skin = np.asarray(Image.open(SKIN).convert('RGB')).astype(np.float32); N = skin.shape[0]
DOWN, FWD = np.array([0.0, 0.0, -1.0]), np.array([0.0, -1.0, 0.0])
step = lambda x, a, b: np.clip((x-a)/(b-a), 0, 1)**2*(3-2*np.clip((x-a)/(b-a), 0, 1))

# ---- landmarks of the mesh
LIPS = [61, 185, 40, 39, 37, 0, 267, 269, 270, 409, 291, 375, 321, 405, 314, 17, 84, 181, 91, 146]
lip = co[[tp[i] for i in LIPS if i in tp]]; lip_c = lip.mean(0)
corner_l, corner_r = co[tp[61]], co[tp[291]]; half = 0.5*abs(corner_r[0]-corner_l[0])
lip_top, lip_bot, nose_base, nose_tip = co[tp[0]], co[tp[17]], co[tp[2]], co[tp[1]]
mouth_z = 0.5*(corner_l[2]+corner_r[2])


def in_lips(p, grow=0.0):
    """Inside the outline of the lips, seen from the front."""
    q = lip[:, [0, 2]]; c = q.mean(0); q = c+(q-c)*(1+grow/np.abs(q-c).max())
    x, z = p[:, 0], p[:, 2]; ins = np.zeros(len(p), bool)
    for i in range(len(q)):
        a, b = q[i], q[(i+1) % len(q)]
        cr = ((a[1] > z) != (b[1] > z)) & (x < (b[0]-a[0])*(z-a[1])/(b[1]-a[1]+1e-12)+a[0]); ins ^= cr
    return ins & (p[:, 1] < lip_c[1]+0.02)


# ---- where facial hair may grow: under the nose tip, in front of the ears, down to the top of the neck
r = co-eye
zone_v = step(nose_tip[2]+0.004-co[:, 2], 0, 0.008)*(1-step(r[:, 1], 0.058, 0.068))*step(co[:, 2]-(lip_bot[2]-0.120), 0, 0.020)*g['body']
beard_v = step(th_v, 0.003, 0.0055)
zc = co[tri].mean(1)[:, 2]; uc = tuv.mean(1)
head = g['body'][tri].all(1) & (zc > eye[2]-0.20) & (uc[:, 0] > 0.62) & (uc[:, 1] > 0.12) & (uc[:, 1] < 0.84)
ft = np.nonzero(head & (zone_v[tri] > 0).any(1))[0]

# ---- the fields in the map: zone, beard, position (for the lips)
zone_t = np.zeros((N, N), np.float32); beard_t = np.zeros((N, N), np.float32); pos_t = np.zeros((N, N, 3), np.float32); nor_t = np.zeros((N, N, 3), np.float32)
for k in ft:
    a = tuv[k]*[N, N]; a[:, 1] = N-a[:, 1]
    x0, y0 = np.floor(a.min(0)).astype(int)-1; x1, y1 = np.ceil(a.max(0)).astype(int)+1
    x0, y0, x1, y1 = max(x0, 0), max(y0, 0), min(x1, N-1), min(y1, N-1)
    X, Y = np.meshgrid(np.arange(x0, x1+1)+0.5, np.arange(y0, y1+1)+0.5)
    T = np.array([[a[0, 0]-a[2, 0], a[1, 0]-a[2, 0]], [a[0, 1]-a[2, 1], a[1, 1]-a[2, 1]]])
    if abs(np.linalg.det(T)) < 1e-9: continue
    l = np.linalg.solve(T, np.stack([X.ravel()-a[2, 0], Y.ravel()-a[2, 1]])); w = np.vstack([l, 1-l.sum(0)]).T
    ok = (w > -0.02).all(1); yy, xx = Y.ravel()[ok].astype(int), X.ravel()[ok].astype(int); w = np.clip(w[ok], 0, 1)
    zone_t[yy, xx] = w @ zone_v[tri[k]]; beard_t[yy, xx] = w @ beard_v[tri[k]]; pos_t[yy, xx] = w @ co[tri[k]]; nor_t[yy, xx] = w @ no[tri[k]]
e3 = np.linalg.norm(co[tri[ft][:, 1]]-co[tri[ft][:, 0]], axis=1); e2 = np.linalg.norm(tuv[ft][:, 1]-tuv[ft][:, 0], axis=1)*N
MM = 0.001/np.median(e3/np.maximum(e2, 1e-9))                               # texels in a millimetre
hairy = (skin[..., 0]-skin[..., 2] < 50) & (skin[..., 1] > 0.70*skin[..., 0]) & (skin.max(2) < 150)
lips_t = np.zeros((N, N), bool); m = zone_t > 0; lips_t[m] = in_lips(pos_t[m], 0.001)
# The skin as points with normals: what a hair must not go under, and what stands in a root's way.
ht = tri[head]; ar = np.linalg.norm(np.cross(co[ht[:, 1]]-co[ht[:, 0]], co[ht[:, 2]]-co[ht[:, 0]]), axis=1)/2
low = co[ht].mean(1)[:, 2] < nose_tip[2]+0.03; ht, ar, huv = ht[low], ar[low], tuv[head][low]


def scatter(count):
    k = rng.choice(len(ht), count, p=ar/ar.sum()); u = rng.random((count, 2)); f = u.sum(1) > 1; u[f] = 1-u[f]
    w = np.c_[1-u.sum(1), u]
    return k, w, (co[ht[k]]*w[..., None]).sum(1), (no[ht[k]]*w[..., None]).sum(1), (huv[k]*w[..., None]).sum(1)


_, _, sp, sn, _ = scatter(int(ar.sum()*2.0e6)); sn /= np.linalg.norm(sn, axis=1, keepdims=True)
stree = cKDTree(sp)

# A nostril and the mouth are dark too, but skin stands right in front of them: no hair, and nothing to take off.
nl = np.linalg.norm(nor_t[m], axis=1, keepdims=True); hole_t = np.zeros((N, N), bool)
hole_t[m] = stree.query(pos_t[m]+nor_t[m]/np.maximum(nl, 1e-9)*0.004, workers=-1)[0] < 0.0026
hole_t = ndimage.binary_dilation(hole_t, iterations=int(0.8*MM))
# How much hair a texel holds: skin is red (r-b about 80 on hero 3a), the beard is brown (about 37), stubble lies
# between them. Bare skin is measured on the cheeks and the nose over the zone, the beard where the offsets say beard.
rb = skin[..., 0]-skin[..., 2]
up = head & (zc > nose_tip[2]+0.008) & (zc < eye[2]-0.012); ua = tuv[up].mean(1)
RB0 = float(np.median(rb[np.clip(((1-ua[:, 1])*N).astype(int), 0, N-1), np.clip((ua[:, 0]*N).astype(int), 0, N-1)]))
RB1 = float(np.median(rb[hairy & (beard_t > 0.5)])) if (hairy & (beard_t > 0.5)).sum() > 100 else RB0-45
src = np.clip((RB0-4-rb)/max(RB0-4-RB1, 1), 0, 1)*~lips_t*~hole_t*zone_t
cover = ndimage.gaussian_filter(src, 1.2*MM)                                 # the share of hair within a millimetre or so
cover = np.where(lips_t, 0, cover)

# ---- the skin without the painted beard
S = 4; small = skin[::S, ::S]; n = small.shape[0]
wide = ndimage.gaussian_filter(src, 3*MM)[::S, ::S]
bare = (wide < 0.10) & ~ndimage.binary_dilation(lips_t[::S, ::S], iterations=int(3*MM/S)) & (np.abs(small-small[0, 0]).sum(2) > 6)   # skin of the head, not the empty map
bare[:int(0.16*n)] = False; bare[int(0.88*n):] = False; bare[:, :int(0.62*n)] = False   # the head's island of the map
fill = np.zeros_like(small); got = np.zeros((n, n), bool)
for sg in (4, 10, 25, 60):                                                    # the nearest bare skin wins, far skin fills the middle of the beard
    wgt = ndimage.gaussian_filter(bare.astype(np.float32), sg*MM/S)
    col = np.stack([ndimage.gaussian_filter(small[..., c]*bare, sg*MM/S) for c in range(3)], -1)/np.maximum(wgt, 1e-6)[..., None]
    use = ~got & (wgt > 0.08); fill[use] = col[use]; got |= use
fill = np.stack([ndimage.zoom(ndimage.gaussian_filter(fill[..., c], 2), S, order=1) for c in range(3)], -1)[:N, :N]
amount = step(ndimage.gaussian_filter(src, 2*MM), 0.08, 0.30)*np.repeat(np.repeat(got, S, 0), S, 1)[:N, :N]
keep = 0.30+0.40*ndimage.gaussian_filter(beard_t, 2*MM)
shaved = skin+(amount*(1-keep))[..., None]*(fill-skin)
Image.fromarray(shaved.clip(0, 255).astype(np.uint8)).save(os.path.join(OUT, 'skin_shaved.png'))

# ---- roots: by the cover, denser in the beard (cheek 20-80, chin 70-120, upper lip 80-150 hairs on a cm2)
DMAX = 150.0
k, w, p0, n0, q = scatter(int(ar.sum()*DMAX*1e4)); n0 /= np.linalg.norm(n0, axis=1, keepdims=True)
tx = lambda F: F[np.clip(((1-q[:, 1])*N).astype(int), 0, N-1), np.clip((q[:, 0]*N).astype(int), 0, N-1)]
cv = tx(cover); bd = (beard_v[ht[k]]*w).sum(1); th = (th_v[ht[k]]*w).sum(1)
mous = step(p0[:, 2]-mouth_z, -0.002, 0.004)*step(nose_base[2]+0.004-p0[:, 2], 0, 0.004)*(1-step(np.abs(p0[:, 0]), half+0.002, half+0.010))*(p0[:, 1] < lip_c[1]+0.03)
bd = np.maximum(bd, mous*step(cv, 0.45, 0.75))                                   # the moustache is a beard whatever the offsets say
dens = step(cv, 0.15, 0.40)*(28+55*cv+(45+30*mous)*bd)
way = stree.query(p0+n0*0.004)[0]                                              # skin right in front of the root: a nostril, the mouth
ok = (rng.random(len(p0)) < dens/DMAX) & ~in_lips(p0, 0.0005) & (way > 0.0026)
p0, n0, cv, bd, th, mous = p0[ok], n0[ok], cv[ok], bd[ok], th[ok], mous[ok]; M = len(p0)

# ---- the way a hair grows: down and a little forward; the moustache from the middle to the corners of the mouth
side = np.sign(p0[:, 0])*step(np.abs(p0[:, 0]), 0.001, 0.006)
grow = DOWN+0.30*FWD+np.zeros((M, 3))
mdir = np.c_[0.95*side, -0.10*np.ones(M), -0.45-0.5*(1-np.abs(side))]
grow = grow+(mdir-grow)*mous[:, None]
grow += rng.normal(0, 0.22, (M, 3)); grow /= np.linalg.norm(grow, axis=1, keepdims=True)
gt = grow-n0*(grow*n0).sum(1, keepdims=True)                                   # along the skin; short where the skin faces the way of growth
elev = np.radians(rng.uniform(35, 65, M)*(1-bd)+rng.uniform(22, 42, M)*bd)
d0 = np.sin(elev)[:, None]*n0+np.cos(elev)[:, None]*gt; d0 /= np.linalg.norm(d0, axis=1, keepdims=True)
L_st = 0.0019*rng.uniform(0.55, 1.35, M)*(0.6+0.4*cv)
L_bd = np.clip(1.8*th, 0.006, 0.024)*rng.uniform(0.65, 1.12, M)
L_bd = np.where(mous > 0.5, 0.0085*rng.uniform(0.7, 1.15, M), L_bd)
L = L_st+(L_bd-L_st)*bd
hang = DOWN+0.15*FWD+np.zeros((M, 3)); hang = hang+(mdir*[1, 1, 0.8]-hang)*mous[:, None]; hang /= np.linalg.norm(hang, axis=1, keepdims=True)
bend = 0.16*bd*np.clip(L/0.012, 0.3, 1.5)

pts = np.zeros((M, K, 3)); pts[:, 0] = p0-n0*0.0003; dk = d0.copy(); ds = (L/(K-1))[:, None]
for j in range(1, K):
    pts[:, j] = pts[:, j-1]+dk*ds
    dk = dk+bend[:, None]*(hang-dk); dk /= np.linalg.norm(dk, axis=1, keepdims=True)

# ---- wisps: a beard hair leans to the leader of its wisp, more to the tip
isb = bd > 0.5; bi = np.nonzero(isb)[0]; tint = rng.normal(0, 0.30, M)
if len(bi) > 20:
    lead = rng.choice(bi, max(len(bi)//7, 1), replace=False); of = lead[cKDTree(p0[lead]).query(p0[bi])[1]]
    wsp = rng.uniform(0.25, 0.6, len(bi))[:, None, None]*(np.linspace(0, 1, K)**1.5)[None, :, None]
    pts[bi] += wsp*((pts[of]-p0[of][:, None])-(pts[bi]-p0[bi][:, None]))*1.0+wsp*0.55*(p0[of]-p0[bi])[:, None]
    tw = rng.normal(0, 0.45, M); tint[bi] += tw[of]

# ---- a beard hair is wiry: it curls
s = np.linspace(0, 1, K)[None]*L[:, None]; ax = pts[:, -1]-pts[:, 0]; ax /= np.linalg.norm(ax, axis=1, keepdims=True)+1e-12
e1 = np.cross(ax, rng.normal(0, 1, (M, 3))); e1 /= np.linalg.norm(e1, axis=1, keepdims=True)+1e-12; e2 = np.cross(ax, e1)
amp = (rng.uniform(0.0004, 0.0010, M)*bd)[:, None]*np.clip(s/0.004, 0, 1); lam = rng.uniform(0.005, 0.010, M)[:, None]; ph = rng.uniform(0, 6.283, M)[:, None]
pts += amp[..., None]*(np.cos(6.283*s/lam+ph)[..., None]*e1[:, None]+np.sin(6.283*s/lam+ph)[..., None]*e2[:, None])

# ---- never under the skin, never over the lips
for _ in range(2):
    P = pts[:, 1:].reshape(-1, 3); ii = stree.query(P, workers=-1)[1]; h = ((P-sp[ii])*sn[ii]).sum(1)
    hmin = np.minimum(0.0003+0.10*s[:, 1:].reshape(-1), 0.0012)
    P += sn[ii]*np.maximum(hmin-h, 0)[:, None]; pts[:, 1:] = P.reshape(M, K-1, 3)
inl = in_lips(pts.reshape(-1, 3), -0.0012).reshape(M, K); cut = np.where(inl.any(1), inl.argmax(1), K).astype(float)
f = np.clip((cut-0.5)/(K-1), 0.15, 1.0)[:, None]*np.arange(K)[None]            # a hair that reaches the lips ends at their edge
i0 = np.floor(f).astype(int); i1 = np.minimum(i0+1, K-1); fr = (f-i0)[..., None]; ix = np.arange(M)[:, None]
pts = pts[ix, i0]*(1-fr)+pts[ix, i1]*fr

kind = np.where(bd > 0.5, np.where(mous > 0.5, 4, 3), 5)
rad = np.where(kind == 5, 0.000045, 0.000055)*rng.uniform(0.85, 1.15, M)
tint = np.clip(tint+np.where(kind == 5, -0.45, -0.35), -1, 1)
np.savez(os.path.join(OUT, 'beard_groom.npz'), pts=pts.astype(np.float32), rad=rad.astype(np.float32), tint=tint.astype(np.float32), kind=kind)
area = lambda mask: 1e4*ar.sum()*mask.mean()
print('skin r-b %.0f, beard r-b %.0f' % (RB0, RB1))
print('BEARD GROOM hairs %d: beard %d, moustache %d, stubble %d; length mm beard median %.1f max %.1f, stubble median %.1f; %.1f texels/mm' % (
    M, (kind == 3).sum(), (kind == 4).sum(), (kind == 5).sum(), 1000*np.median(L[kind != 5]) if (kind != 5).any() else 0,
    1000*L.max(), 1000*np.median(L[kind == 5]) if (kind == 5).any() else 0, MM))
