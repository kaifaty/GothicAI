# The skin of a head as a material of the game: its tone against the body it sits on, the neck rim blended into the
# body's skin, a roughness map by the zones of the face and a normal map (creases and pores).
#   python skin_material.py uv.npz groups.npz views.json skin.png template_points.json <body> <cut> <out dir>
#       [--groom groom.npz]... [--under r,g,b] [--seed 1] [--relief samples.npz]
# Input:
# - uv.npz, groups.npz, views.json: the registered head (co, no, tri, tuv; body; eye), metres, Z up, the face to -Y;
# - skin.png: the colour map of the head (after beard.py: skin_shaved.png);
# - template_points.json: MediaPipe index -> vertex of the MPFB mesh (lips, nose, eyes, brows);
# - <body>: the texture of the body the head sits on (its skin texels are found by colour), or "r,g,b" (sRGB);
# - <cut>: where the head ends and the body's neck begins: the head's frame.json (`cut`: the plane of
#   export_test_head.py), or a height in metres.
# - --groom: the strands of the head's hair (groom.py). Where their roots stand, the hair painted on the map by the
#   generator's model is taken off: seen between the strands, it reads as patches of a texture. Its place takes
#   skin shaded towards --under, the colour of the hair at its roots (sRGB), by the density of the roots: a gap in
#   thick hair reads as depth, and the shade runs out towards the hairline. The beard's zone is beard.py's.
# - --relief: what the head's vertices could not take of the bald model's relief (head_relief.py: places on the
#   head's faces and the height of the model over them). It goes into the normal map: wrinkles and the fine part of
#   the folds, wider than half a millimetre (finer is the generator's grain), without what changes over
#   centimetres (that is shape, not relief), no deeper than a millimetre (a pit of the generator's) and at
#   RELIEF_GAIN of its height. Without it the only creases are the dark lines of the colour map.
# Output:
# - skin_game.png: the colour map. The head's bare skin is as light as the body's (one gain for the whole head, the
#   face keeps its own hue); in the band over the cut the colour runs into the body's, so the rim has its colour;
# - skin_orm.png: R = 1 (no baked occlusion), G = roughness, B = 0. The forehead, the nose and the lips shine, hair
#   painted on the skin does not; at the rim the skin is as matte as the body, which the game lights without a
#   highlight;
# - skin_normal.png: tangent normals as the game reads them (X in alpha, Y in green): the model's relief (--relief),
#   creases where the colour map has dark lines, and pores;
# - skin_height.png (with --relief): the model's relief alone, 16 bit: (value - 32768) * 0.5 micrometres, for the
#   bump of the Cycles check sheet (groom_render.py --height).
import sys, os, json
import numpy as np
from PIL import Image
from scipy import ndimage, sparse
from scipy.sparse.csgraph import dijkstra
from scipy.spatial import cKDTree

a = sys.argv[1:]; GROOMS = []
while '--groom' in a:
    i = a.index('--groom'); GROOMS.append(a[i+1]); del a[i:i+2]


def opt(name, default):
    if name in a:
        i = a.index(name); v = a[i+1]; del a[i:i+2]; return v
    return default


UNDER = np.array([float(v) for v in opt('--under', '58,40,26').split(',')])/255; SEED = int(opt('--seed', 1)); RELIEF = opt('--relief', None)
UV, GR, VJ, SKIN, TP, BODY, CUT, OUT = a[:8]
rng = np.random.default_rng(SEED); os.makedirs(OUT, exist_ok=True)
d = np.load(UV); g = np.load(GR); eye = np.array(json.load(open(VJ))['eye'])
co, no, tri, tuv = d['co'].astype(np.float64), d['no'].astype(np.float64), d['tri'], d['tuv'].astype(np.float64)
tp = {int(k): v['vertex'] for k, v in json.load(open(TP))['points'].items()}
img = np.asarray(Image.open(SKIN).convert('RGB')).astype(np.float32); N = img.shape[0]
cut = np.array(json.load(open(CUT))['cut'] if CUT.endswith('.json') else [0, 0, 1, float(CUT)])
step = lambda x, a, b: np.clip((x-a)/(b-a), 0, 1)**2*(3-2*np.clip((x-a)/(b-a), 0, 1))
lin = lambda c: np.where(c > 0.04045, ((c+0.055)/1.055)**2.4, c/12.92)
srgb = lambda c: np.where(c > 0.0031308, 1.055*np.maximum(c, 1e-9)**(1/2.4)-0.055, c*12.92)
LUM = np.array([0.2126, 0.7152, 0.0722])
RELIEF_GAIN = 0.5            # of the model's height in the normal map: the colour map carries the same wrinkles as dark lines (the
                             # concept's light, baked in), and at full height the two together read as cracks
BAND = 0.045                 # the band along the skin from the cut where the head's colour runs into the body's, metres
ROUGH, ROUGH_BODY = 0.52, 0.90

# ---- the body's skin: what the neck under the head looks like
if os.path.exists(BODY):
    b = np.asarray(Image.open(BODY).convert('RGB')).astype(np.float32)
    sk = (b[..., 0] > 150) & (b[..., 0]-b[..., 2] > 60) & (b[..., 0]-b[..., 2] < 140) & (b[..., 1] > 0.60*b[..., 0]) & (b[..., 1] < 0.82*b[..., 0])
    assert sk.mean() > 0.005, 'no skin found in the body texture'
    body_lin = np.median(lin(b[sk]/255), 0); print('BODY skin texels %.1f%%' % (100*sk.mean()))
else:
    body_lin = lin(np.array([float(v) for v in BODY.split(',')])/255)

# ---- landmarks
LIPS = [61, 185, 40, 39, 37, 0, 267, 269, 270, 409, 291, 375, 321, 405, 314, 17, 84, 181, 91, 146]
lip = co[[tp[i] for i in LIPS if i in tp]]; lip_c = lip.mean(0)
lip_bot, nose_tip, nose_root = co[tp[17]], co[tp[1]], co[tp[168]]
eyes = [co[[tp[i] for i in ids if i in tp]].mean(0) for ids in ((33, 133, 159, 145), (263, 362, 386, 374))]
brow_z = co[[tp[i] for i in (70, 63, 105, 66, 107, 336, 296, 334, 293, 300) if i in tp]][:, 2].mean()


def in_lips(p, grow=0.0):
    q = lip[:, [0, 2]]; c = q.mean(0); q = c+(q-c)*(1+grow/np.abs(q-c).max())
    x, z = p[:, 0], p[:, 2]; ins = np.zeros(len(p), bool)
    for i in range(len(q)):
        a, bb = q[i], q[(i+1) % len(q)]
        ins ^= ((a[1] > z) != (bb[1] > z)) & (x < (bb[0]-a[0])*(z-a[1])/(bb[1]-a[1]+1e-12)+a[0])
    return ins & (p[:, 1] < lip_c[1]+0.02)


# ---- the head's island of the map: position, normal and the weight of the body's colour in every texel
over = (co @ cut[:3]-cut[3])/np.linalg.norm(cut[:3])                         # metres over the cut
uc = tuv.mean(1)
head = g['body'][tri].all(1) & (over[tri] >= 0).any(1) & (uc[:, 0] > 0.62) & (uc[:, 1] > 0.12) & (uc[:, 1] < 0.84)
# the rim: the head is cut by a plane, so the vertices of its triangles under the plane stand on the rim
# (a little under it); the band is measured along the skin from them
ht = tri[head]; ue = np.unique(np.sort(np.concatenate([ht[:, [0, 1]], ht[:, [1, 2]], ht[:, [2, 0]]]), 1), axis=0)
rim = np.unique(ht); rim = rim[over[rim] < 0]
ln = np.linalg.norm(co[ue[:, 0]]-co[ue[:, 1]], axis=1)
gr = sparse.coo_matrix((ln, (ue[:, 0], ue[:, 1])), shape=(len(co), len(co)))
dist = dijkstra(gr, directed=False, indices=rim, min_only=True)
rim_v = 1-step(np.where(np.isfinite(dist), dist, 1.0), 0.003, BAND)
F = np.concatenate([co, no, rim_v[:, None]], 1); fld = np.zeros((N, N, 7), np.float32); m = np.zeros((N, N), bool)
for k in np.nonzero(head)[0]:
    a = tuv[k]*[N, N]; a[:, 1] = N-a[:, 1]
    x0, y0 = np.floor(a.min(0)).astype(int)-1; x1, y1 = np.ceil(a.max(0)).astype(int)+1
    x0, y0, x1, y1 = max(x0, 0), max(y0, 0), min(x1, N-1), min(y1, N-1)
    X, Y = np.meshgrid(np.arange(x0, x1+1)+0.5, np.arange(y0, y1+1)+0.5)
    T = np.array([[a[0, 0]-a[2, 0], a[1, 0]-a[2, 0]], [a[0, 1]-a[2, 1], a[1, 1]-a[2, 1]]])
    if abs(np.linalg.det(T)) < 1e-9: continue
    l = np.linalg.solve(T, np.stack([X.ravel()-a[2, 0], Y.ravel()-a[2, 1]])); w = np.vstack([l, 1-l.sum(0)]).T
    ok = (w > -0.02).all(1); yy, xx = Y.ravel()[ok].astype(int), X.ravel()[ok].astype(int)
    fld[yy, xx] = np.clip(w[ok], 0, 1) @ F[tri[k]]; m[yy, xx] = True
pos_t, nor_t, rim_t = fld[..., :3], fld[..., 3:6], fld[..., 6]
nor_t /= np.maximum(np.linalg.norm(nor_t, axis=2, keepdims=True), 1e-9)
hk = np.nonzero(head)[0]
e3 = np.linalg.norm(co[tri[hk][:, 1]]-co[tri[hk][:, 0]], axis=1); e2 = np.linalg.norm(tuv[hk][:, 1]-tuv[hk][:, 0], axis=1)*N
MM = 0.001/np.median(e3/np.maximum(e2, 1e-9))                                # texels in a millimetre

# ---- zones in the map
P = pos_t[m]
lips_t = np.zeros((N, N), bool); lips_t[m] = in_lips(P, 0.0005)
eye_t = np.zeros((N, N), np.float32); eye_t[m] = np.max([1-step(np.linalg.norm(P-e, axis=1), 0.013, 0.019) for e in eyes], 0)
rb = img[..., 0]-img[..., 2]
front = m & (nor_t[..., 1] < -0.3) & (pos_t[..., 2] > nose_tip[2]-0.01) & (pos_t[..., 2] < brow_z-0.012) & (eye_t < 0.01) & (np.abs(pos_t[..., 0]) > 0.02)
RB0 = float(np.median(rb[front]))                                            # bare skin: the cheeks beside the nose
hair_t = np.clip(np.maximum((RB0-4-rb)/45, (95-img.max(2))/40), 0, 1)*m       # hair painted on the skin: brown or dark
bare = front & (hair_t < 0.15)
img_lin = lin(img/255)
head_lin = np.median(img_lin[bare], 0)

# ---- the skin under the hair of the head: the painted hair off, skin in its place; `cover` is 1 where roots stand
cover = np.zeros((N, N), np.float32)
if GROOMS:
    roots = cKDTree(np.concatenate([np.load(gp)['pts'][:, 0].astype(np.float64) for gp in GROOMS]))
    sub = np.zeros((N, N), bool); sub[::2, ::2] = True; ys, xs = np.nonzero(m & sub)
    cover[ys, xs] = 1-step(roots.query(pos_t[ys, xs], workers=-1)[0], 0.0015, 0.004)   # a root within 1.5 mm: under hair
    cover = ndimage.grey_closing(ndimage.maximum_filter(cover, 2), size=int(3*MM))   # thin gaps along the edges of large triangles
    cover = ndimage.gaussian_filter(cover, 1.5*MM)*m                          # (the map's texels are not even in size, so not a count of roots)
    near = step(ndimage.gaussian_filter((cover > 0.05).astype(np.float32), 6*MM), 0.02, 0.2)
    off = np.maximum(np.clip(cover*1.5, 0, 1), ndimage.gaussian_filter(hair_t, 0.8*MM)*near)*m   # where the paint goes
    S2 = 4; small = img_lin[::S2, ::S2]; n2 = small.shape[0]
    src = (bare | (m & (hair_t < 0.15) & (eye_t < 0.01) & ~lips_t & (off < 0.02)))[::S2, ::S2]
    fill = np.zeros_like(small); got = np.zeros((n2, n2), bool)
    for sg in (6, 15, 40, 100):                                               # the nearest bare skin wins, far skin fills the crown
        wg = ndimage.gaussian_filter(src.astype(np.float32), sg*MM/S2)
        col = np.stack([ndimage.gaussian_filter(small[..., c]*src, sg*MM/S2) for c in range(3)], -1)/np.maximum(wg, 1e-6)[..., None]
        use = ~got & (wg > 0.08); fill[use] = col[use]; got |= use
    fill[~got] = head_lin
    fill = np.stack([ndimage.zoom(fill[..., c], S2, order=1) for c in range(3)], -1)[:N, :N]
    img_lin = img_lin*(1-off[..., None])+fill*off[..., None]
    hair_t = np.maximum(hair_t*(1-off), cover)                                # matte and flat under the strands
GAIN = float(np.clip((body_lin@LUM)/(head_lin@LUM), 1.0, 2.5))

# ---- the colour: one gain for the head; over the cut the colour within a centimetre is replaced by the body's
S = 4; mm_s = MM/S; ms = m[::S, ::S].astype(np.float32)
wgt = ndimage.gaussian_filter(ms, 6*mm_s)
loc = np.stack([ndimage.gaussian_filter(img_lin[::S, ::S, c]*ms, 6*mm_s) for c in range(3)], -1)/np.maximum(wgt, 1e-6)[..., None]
loc = np.stack([ndimage.zoom(loc[..., c], S, order=1) for c in range(3)], -1)[:N, :N]
rim_s = ndimage.gaussian_filter(rim_t, 1.0*MM)
to_body = body_lin/np.maximum(loc*GAIN, 1e-4)
out = img_lin*GAIN*(1+rim_s[..., None]*(to_body-1))
out = np.where(out > 0.70, 0.70+0.25*(1-np.exp(-(out-0.70)/0.25)), out)       # a soft knee: no burnt highlights of the map
out = out+(lin(UNDER)-out)*(0.85*cover[..., None])                            # under thick hair the skin is in its shade
edge = ndimage.gaussian_filter(m.astype(np.float32), 0.5*MM)                 # the island's border: no step against the rest of the map
game = np.where(m[..., None], out, img_lin*(1+(GAIN-1)*np.clip(edge*2, 0, 1)[..., None]))
Image.fromarray(np.clip(srgb(game)*255+0.5, 0, 255).astype(np.uint8)).save(os.path.join(OUT, 'skin_game.png'))

# ---- roughness
hair_b = ndimage.gaussian_filter(hair_t, 0.8*MM)
r = np.full((N, N), ROUGH, np.float32)
ab = nose_root-nose_tip; t = np.clip(((P-nose_tip) @ ab)/(ab @ ab), 0, 1); dn = np.linalg.norm(P-(nose_tip+t[:, None]*ab), axis=1)
zone = np.zeros(len(P), np.float32)
zone += 0.07*np.exp(-(dn/0.013)**2)                                          # the nose
fz = brow_z+0.032; zone += 0.05*np.exp(-(P[:, 0]/0.040)**2-((P[:, 2]-fz)/0.022)**2)*(nor_t[m][:, 1] < -0.4)   # the forehead
ch = lip_bot+np.array([0, 0, -0.028]); zone += 0.04*np.exp(-(np.linalg.norm((P-ch)[:, [0, 2]], axis=1)/0.016)**2)*(nor_t[m][:, 1] < -0.3)   # the chin
r[m] -= zone
r = np.where(ndimage.gaussian_filter(lips_t.astype(np.float32), 0.5*MM) > 0.5, 0.38, r)
r += 0.30*hair_b
r += 0.02*ndimage.gaussian_filter(rng.standard_normal((N, N)).astype(np.float32), 1.5*MM)*1.5*MM*3.5
r = r+(ROUGH_BODY-r)*rim_s
r = np.where(m, np.clip(r, 0.30, 0.95), ROUGH_BODY)
SO = 4; orm = np.zeros((N//SO, N//SO, 3), np.uint8); orm[..., 0] = 255
orm[..., 1] = np.clip(ndimage.zoom(r, 1/SO, order=1)*255+0.5, 0, 255).astype(np.uint8)
Image.fromarray(orm).save(os.path.join(OUT, 'skin_orm.png'))

# ---- the normal map: creases from the dark lines of the colour map, pores from noise; metres of height
ll = np.log(np.maximum(img_lin@LUM, 1e-3))                                    # the map without the painted hair
band = ndimage.gaussian_filter(ll, 0.35*MM)-ndimage.gaussian_filter(ll, 2.0*MM)
crease = np.clip(band*0.0005, -0.00015, 0.00008)                             # 20 % darker = 0.1 mm deeper
n0 = rng.standard_normal((N, N)).astype(np.float32)
pores = ndimage.gaussian_filter(n0, 0.11*MM)-ndimage.gaussian_filter(n0, 0.30*MM)
pores *= 0.000008/max(float(pores.std()), 1e-9)                              # 0.008 mm of relief at 0.1..0.3 mm: more of it glitters under a low sun
keep = m*(1-np.clip(hair_b*1.5, 0, 1))*(1-eye_t)*(1-rim_s)
keep = ndimage.gaussian_filter(keep.astype(np.float32), 0.4*MM)
relief = np.zeros((N, N), np.float32)
if RELIEF:
    # the model over the head's faces, laid into the map: every place finds its texel by where it is on the head now
    sm = np.load(RELIEF); assert sm['tri'].max() < len(co), 'the relief is of another mesh'
    pts = (co[sm['tri']]*sm['bary'][:, :, None]).sum(1); ys, xs = np.nonzero(m)
    dd, kk = cKDTree(pos_t[ys, xs]).query(pts, workers=-1); ok = dd < 0.0008
    acc = np.zeros((N, N), np.float32); cnt = np.zeros((N, N), np.float32)
    np.add.at(acc, (ys[kk[ok]], xs[kk[ok]]), (sm['h']*sm['w'])[ok]); np.add.at(cnt, (ys[kk[ok]], xs[kk[ok]]), 1)
    blur = lambda x, sg: ndimage.gaussian_filter(x, sg)
    cb = blur(cnt, 0.5*MM); fine = blur(acc, 0.5*MM)/np.maximum(cb, 1e-3); known = np.clip(cb/0.1, 0, 1)*m
    slow = blur(fine*known, 8*MM)/np.maximum(blur(known, 8*MM), 1e-3)
    inner = ndimage.gaussian_filter(ndimage.binary_erosion(m, iterations=int(2*MM)).astype(np.float32), 0.7*MM)   # no step at the island's seams
    relief = RELIEF_GAIN*np.clip((fine-slow)*known, -0.001, 0.001)*keep*inner*(1-ndimage.gaussian_filter(lips_t.astype(np.float32), 0.5*MM))
    Image.fromarray(np.clip(32768+relief/5e-7, 0, 65535).astype(np.uint16)).save(os.path.join(OUT, 'skin_height.png'))
    print('RELIEF %d places, in the map %.0f%%; height mean |.| %.2f mm, 95%% %.2f mm' % (
        len(pts), 100*ok.mean(), 1000*float(np.abs(relief[m & (keep > 0.9)]).mean()), 1000*float(np.percentile(np.abs(relief[m & (keep > 0.9)]), 95))))
h = (crease*(0.5 if RELIEF else 1.0)+pores*np.where(lips_t, 0.3, 1.0))*keep+relief
tm = MM*1000                                                                 # texels in a metre
du = (np.roll(h, -1, 1)-np.roll(h, 1, 1))*0.5*tm; dv = (np.roll(h, -1, 0)-np.roll(h, 1, 0))*0.5*tm
nz = 1/np.sqrt(du*du+dv*dv+1); nx, ny = -du*nz, -dv*nz
yb = np.clip((ny*0.5+0.5)*255+0.5, 0, 255).astype(np.uint8); xb = np.clip((nx*0.5+0.5)*255+0.5, 0, 255).astype(np.uint8)
Image.fromarray(np.dstack([yb, yb, yb, np.minimum(xb, 254)])).save(os.path.join(OUT, 'skin_normal.png'))   # alpha under 255: packed as DXT5
tilt = np.degrees(np.arctan(np.sqrt(du*du+dv*dv)))[m & (keep > 0.9)]

# ---- a sheet to look at: before, after, roughness, relief
ys, xs = np.nonzero(m); y0, y1, x0, x1 = ys.min(), ys.max(), xs.min(), xs.max(); c = lambda a: a[y0:y1:3, x0:x1:3]
relief = np.clip(0.5+0.5*(nx*0.6+ny*0.6)*6, 0, 1)
sheet = np.concatenate([c(img/255), c(srgb(game)), np.repeat(c(r)[..., None], 3, 2), np.repeat(c(relief)[..., None], 3, 2)], 1)
Image.fromarray(np.clip(sheet*255, 0, 255).astype(np.uint8)).save(os.path.join(OUT, 'skin_material.jpg'), quality=88)
f8 = lambda v: tuple(int(x) for x in np.clip(srgb(v)*255+0.5, 0, 255))
print('SKIN head %s body %s gain %.2f; hair roots over %.0f cm2; %d rim vertices, %.1f texels/mm; roughness %.2f..%.2f, median %.2f; tilt median %.1f deg, 95%% %.1f deg'
      % (f8(head_lin), f8(body_lin), GAIN, float((cover > 0.5).sum())/MM/MM/100, len(rim), MM, r[m].min(), r[m].max(), float(np.median(r[bare])), float(np.median(tilt)), float(np.percentile(tilt, 95))))
