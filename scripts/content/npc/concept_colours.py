"""The colours of the hair, the beard and the eyes, taken from the concept (build_head.py, step colour).

  python concept_colours.py <views dir> <params.json> <views.json> groom.npz beard_groom.npz eye.png <out dir>

<views dir> is build_head.py's: {full,bald}_{front,profile}.png at the scale of params.json and points/{full,bald}_front.json
of face_points.py. groom.npz and beard_groom.npz are groom.py's and beard.py's strands in the head's frame (metres),
eye.png the template's eye map. Writes into <out dir>:
- groom.npz, beard_groom.npz: the same strands with a new tint. A strand's colour is looked up in one ramp by its tint
  (groom_export.py for the game, groom_render.py for Cycles), so the tint now says where the strand's colour lies in
  the concept's hair: every strand is projected onto the concept's front and profile at three points along it, the
  colour seen there is averaged with its neighbours within 6 mm, and the tint is the rank of its brightness among all
  strands, plus a little of the old tint for the locks. Grey temples and a grey chin stay where the concept has them;
  hair and beard share the ramp;
- hair_ramp.png (256 x 8): the colours of that rank, the game's strand albedo: the colour seen in the concept times
  GAIN (below), since the game shades the strands again;
- hair_pigment.png: the same ramp for Cycles' principled hair, whose light scattered inside the hair makes a pigment
  look lighter. The brightness is converted by a fit to hero 3a's two hand ramps, the hue kept;
- eye.png: the template's eye map with the iris recoloured to the concept's: the iris of the face_points detector
  (centre 468/473, ring 469-472/474-477) on the full and the bald front, the median of its middle ring under the lid,
  without highlights. The map keeps the template's iris detail as brightness over that colour. The iris is also
  brought to the concept's size: its diameter there (eye_lines.py, both fronts) over IRIS_MM, the diameter the same
  reading gives on a render of the template's map; the pupil keeps its share of the iris;
- colour.json: the numbers; colour.jpg: the concept next to the strands drawn in their new colours, the ramp and the iris.
Hair in a view: on the front, where the full sheet differs from the bald one and the pixel looks like hair; on the
profile, whose bald sheet is another generation and does not lie on the full one, where it looks like hair. "Looks
like" is the vote of the 15 nearest front pixels in brightness, chromaticity and texture. A strand point off the hair
takes the nearest hair pixel within 4 mm (the groom's hairline lies a few mm inside the concept's); a strand with no
sample takes its nearest neighbour's.
"""
import json
import os
import sys

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage
from scipy.spatial import cKDTree

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eye_lines

IRIS_MM = 12.45        # the template's iris as eye_lines.py reads it on a front render of our head (the second face, 0.4 mm a pixel)
VD, PJ, VJ, GROOM, BEARD, EYE, OUT = sys.argv[1:8]
os.makedirs(OUT, exist_ok=True)
P = json.load(open(PJ)); eye = np.array(json.load(open(VJ))['eye'])
mpp = 0.0615/P['ipd_px']; (fx, fy), (px, py) = P['front_eye'], P['profile_eye']
lin = lambda c: np.where(c/255 > 0.04045, ((c/255+0.055)/1.055)**2.4, c/255/12.92)
srgb = lambda c: np.where(c > 0.0031308, 1.055*np.clip(c, 0.0031308, None)**(1/2.4)-0.055, 12.92*c)*255
lum = lambda c: c@np.array([0.2126, 0.7152, 0.0722])
img = lambda name: np.asarray(Image.open(os.path.join(VD, name+'.png')).convert('RGB')).astype(np.float64)


def backdrop(a):
    bg = np.median(np.concatenate([a[0], a[:, 0], a[:, -1]]), axis=0)
    return ndimage.binary_dilation(np.abs(a-bg).max(2) <= 14, iterations=2)


# ---- the views: colour blurred to about a millimetre, linear; where a view says nothing
front, prof, bald = img('full_front'), img('full_profile'), img('bald_front')
blur = lambda a: lin(ndimage.gaussian_filter(a, (2.5, 2.5, 0)))
F, PR = blur(front), blur(prof)
diff = np.abs(ndimage.gaussian_filter(front, (3, 3, 0))-ndimage.gaussian_filter(bald, (3, 3, 0))).max(2)
HAIR_DIFF = 22                                                  # 8-bit; skin of two generations differs by ~10
hair_f = ndimage.binary_opening(diff > HAIR_DIFF, iterations=2) & ~backdrop(front)


def feat(a):
    """Per pixel: brightness, chromaticity and the local texture, the things that tell hair from skin and cloth."""
    b = ndimage.gaussian_filter(a, (1.5, 1.5, 0)); s = b.sum(2)+1e-3; L = b@[0.2126, 0.7152, 0.0722]
    tex = np.sqrt(np.maximum(ndimage.gaussian_filter(L**2, 3)-ndimage.gaussian_filter(L, 3)**2, 0))
    return np.stack([np.log(L+5), b[..., 0]/s*12, b[..., 1]/s*12, np.log(tex+2)*0.7], -1)


# the bald profile is another generation and does not lie on the full one, so the profile's hair is told by
# its look: 15 nearest pixels of the front, where the full and the bald fronts said hair or not
rng = np.random.default_rng(0); Ff = feat(front)
skin_f = ndimage.binary_erosion(diff < 10, iterations=3) & ~backdrop(front)
pick = lambda m: (lambda i: i[rng.choice(len(i), min(15000, len(i)), replace=False)])(np.argwhere(m))
hi, si = pick(hair_f), pick(skin_f)
X = np.concatenate([Ff[hi[:, 0], hi[:, 1]], Ff[si[:, 0], si[:, 1]]]); lab = np.r_[np.ones(len(hi)), np.zeros(len(si))]
mu, sd = X.mean(0), X.std(0); tree = cKDTree((X-mu)/sd)
looks_hair = lambda a: lab[tree.query((feat(a).reshape(-1, 4)-mu)/sd, k=15, workers=-1)[1]].mean(1).reshape(a.shape[:2])
# the front: both, since the faces of two generations differ in width and lips by a few pixels
f_bad = ~(hair_f & (looks_hair(front) >= 0.7))
p_bad = backdrop(prof) | (looks_hair(prof) < 0.9)
# the colour is blurred by 2.5 px: the hair's last pixels hold the skin next to them, and the strands that reach
# for hair from the skin take exactly those
f_bad, p_bad = ndimage.binary_dilation(f_bad, iterations=4), ndimage.binary_dilation(p_bad, iterations=4)
REACH = 0.004/mpp       # a strand a few millimetres inside the concept's hairline takes the hair next to it


def look(a, bad, u, v):
    """Colours at pixels (u, v), from the nearest hair pixel within REACH, and whether there was one."""
    h, w = bad.shape; ok = (u >= 0) & (u < w-1) & (v >= 0) & (v < h-1)
    ui, vi = np.clip(u.round().astype(int), 0, w-1), np.clip(v.round().astype(int), 0, h-1)
    dist, (ny, nx) = ndimage.distance_transform_edt(bad, return_indices=True)
    ok &= dist[vi, ui] <= REACH
    yy, xx = ny[vi, ui]+(v-vi), nx[vi, ui]+(u-ui)
    c = np.stack([ndimage.map_coordinates(a[..., k], [np.clip(yy, 0, h-1), np.clip(xx, 0, w-1)], order=1) for k in range(3)], 1)
    return c, ok


# ---- strands: three points along each, seen by the front where they face it and by the profile from the side
G, B = np.load(GROOM), np.load(BEARD)
nh = len(G['pts'])
pts = np.concatenate([G['pts'][:, [int(f*(G['pts'].shape[1]-1)) for f in (0.15, 0.4, 0.65)]],
                      B['pts'][:, [int(f*(B['pts'].shape[1]-1)) for f in (0.15, 0.4, 0.65)]]])          # (n, 3, 3)
kind = np.concatenate([G['kind'], B['kind']]); old = np.concatenate([G['tint'], B['tint']])
axis_y = float(np.median(G['pts'][:, 0, 1]))                    # the head's vertical axis: the middle of the roots
q = pts.reshape(-1, 3); x, y, z = q.T
d = np.stack([x, y-axis_y], 1); d /= np.linalg.norm(d, axis=1, keepdims=True)+1e-9
wf = np.clip(-d[:, 1], 0, None)**2; wp = 1-wf                   # the front sees what faces it; the side the rest
cf, okf = look(F, f_bad, fx+(x-eye[0])/mpp, fy-(z-eye[2])/mpp)
cp, okp = look(PR, p_bad, px+(y-eye[1])/mpp, py-(z-eye[2])/mpp)
wf, wp = wf*okf, wp*okp
w = wf+wp; col = (cf*wf[:, None]+cp*wp[:, None])/np.maximum(w, 1e-9)[:, None]
w, col = w.reshape(-1, 3), col.reshape(-1, 3, 3)
seen = w.sum(1) > 0.05
col = (col*w[..., None]).sum(1)/np.maximum(w.sum(1), 1e-9)[:, None]
# a strand whose colour is this concept's skin rather than its hair took an ear or a neck: the groom's hairline
# behind the ear lies on the concept's neck. The vote by colour alone, of the same front pixels
# (the hair of the front where both tests agree: the difference alone takes in the edges of the face and the lips)
s8 = np.clip(srgb(col), 0, 255); sm = s8.sum(1)+1e-3
fc = np.stack([np.log(s8@[0.2126, 0.7152, 0.0722]+5), s8[:, 0]/sm*12, s8[:, 1]/sm*12], 1)    # feat() without texture
h2 = pick(~f_bad); X2 = np.concatenate([Ff[h2[:, 0], h2[:, 1], :3], Ff[si[:, 0], si[:, 1], :3]])
lab2 = np.r_[np.ones(len(h2)), np.zeros(len(si))]
_, nc = cKDTree((X2-mu[:3])/sd[:3]).query((fc-mu[:3])/sd[:3], k=15, workers=-1)
skinlike = lab2[nc].mean(1) < 0.4
seen &= ~skinlike
# the short hairs at the hairline (groom.py's kind 2) and the stubble (beard.py's kind 5) lie on skin by design:
# they take the colour of the nearest hair or beard
seen &= ~np.isin(kind, (2, 5))
mid = pts[:, 1]
tree = cKDTree(mid[seen]); _, nn = tree.query(mid[~seen], workers=-1)
col[~seen] = col[seen][nn]
nb = cKDTree(mid).query_ball_point(mid, 0.006, workers=-1)       # the neighbours within 6 mm: a lock's colour
col = np.array([np.median(col[i], 0) for i in nb])

# ---- the ramp: strands ranked by brightness; the colour of each rank
L = lum(col); order = np.argsort(L); rank = np.empty(len(L)); rank[order] = (np.arange(len(L))+0.5)/len(L)
xs = (np.arange(256)+0.5)/256; srt = col[order]
ramp = np.stack([np.median(srt[int(max(0, a-0.02)*len(L)):int(min(1, a+0.02)*len(L))+1], 0) for a in xs])
ramp = ndimage.gaussian_filter1d(ramp, 3, axis=0, mode='nearest')
# What a view shows is the hair's colour times its light and the shade of the hairs over it; the game and Cycles
# light and shade the strands again. The gain is set so that Cycles' render of the second face (groom_render.py at
# the concept's camera, this ramp through the pigment fit below) has the concept's brightness over the concept's
# head hair: 0.91 of it at gain 1.0, 1.16 at 1.6. Hero 3a's hand ramp is 2.4 times what is measured here, and its
# accepted render is 1.5 times brighter than its concept
GAIN = 1.2
ramp = np.clip(ramp*GAIN, 0, 1)
jit = old-np.where(kind >= 3, old[kind >= 3].mean(), old[kind < 3].mean())
tint = np.clip(2*np.clip(rank+0.10*jit, 0, 1)-1, -1, 1).astype(np.float32)
np.savez(os.path.join(OUT, 'groom.npz'), **{k: G[k] for k in G.files if k != 'tint'}, tint=tint[:nh])
np.savez(os.path.join(OUT, 'beard_groom.npz'), **{k: B[k] for k in B.files if k != 'tint'}, tint=tint[nh:])
strip = lambda r: Image.fromarray(np.repeat(srgb(r)[None], 8, 0).round().clip(0, 255).astype(np.uint8))
strip(ramp).save(os.path.join(OUT, 'hair_ramp.png'))
# hero 3a by hand: (seen in the concept, the pigment Cycles shows like it), sRGB. Their brightness is fitted in
# log-linear and the colour scaled by it: a fit per channel would carry 3a's golden hue into every head's grey
SEEN, PIG = np.array([(30, 21, 14), (84, 56, 34), (175, 125, 70)]), np.array([(20, 13, 8), (56, 33, 17), (150, 92, 40)])
fit = np.polyfit(np.log(lum(lin(SEEN))), np.log(lum(lin(PIG))), 1)
rl = np.maximum(lum(ramp), 1e-5); pig = ramp*(np.exp(np.polyval(fit, np.log(rl)))/rl)[:, None]
strip(np.clip(pig, 0, 1)).save(os.path.join(OUT, 'hair_pigment.png'))

# ---- the iris: the detector's centre and ring on both fronts
irises = []
for name, im in (('full_front', front), ('bald_front', bald)):
    pt = np.array(json.load(open(os.path.join(VD, 'points', name+'.json')))['points'])[:, :2]
    for c, ring in ((468, range(469, 473)), (473, range(474, 478))):
        r = np.linalg.norm(pt[list(ring)]-pt[c], axis=1).mean()
        yy, xx = np.mgrid[:im.shape[0], :im.shape[1]]; dx, dy = xx-pt[c][0], yy-pt[c][1]; rr = np.hypot(dx, dy)
        m = (rr > 0.35*r) & (rr < 0.85*r) & (dy > -0.3*r)        # the middle of the iris, under the upper lid
        cl = lin(im[m]); lm = lum(cl); cl = cl[lm < 1.6*np.median(lm)]   # no highlights
        irises.append(np.median(cl, 0))
iris = np.median(irises, 0)
a = np.asarray(Image.open(EYE).convert('RGBA')).astype(np.float64); e = lin(a[..., :3])
pup = ndimage.label(a[..., :3].mean(2) < 25)[0]; out = e.copy(); spots = []
for i in range(1, pup.max()+1):
    m = pup == i
    if not 1000 < m.sum() < 8000: continue                       # the two pupils; the big disc of the map is not an eye
    cy, cx = ndimage.center_of_mass(m); r0 = np.sqrt(m.sum()/np.pi)
    yy, xx = np.mgrid[:a.shape[0], :a.shape[1]]; rr = np.hypot(xx-cx, yy-cy)
    ir = np.arange(int(r0), int(5*r0)); prof_l = np.array([lum(e[(rr >= k) & (rr < k+1)]).mean() for k in ir])
    edge = ir[np.argmax(prof_l > 2.2*np.median(prof_l[:len(ir)//2]))]   # the sclera begins: the iris's edge
    body = (rr > 1.2*r0) & (rr < 0.85*edge); med = np.median(lum(e[body]))
    wgt = np.clip((edge+2-rr)/6, 0, 1)[..., None]
    out = out*(1-wgt)+(iris*(lum(e)/med)[..., None])*wgt
    spots.append([round(cx), round(cy), round(r0, 1), int(edge)])
a[..., :3] = srgb(np.clip(out, 0, 1))
# the iris' size: the map is pulled towards each pupil so that the iris' edge comes to the concept's share of the eye
sizes = [e['iris']['diameter_mm'] for name in ('full_front', 'bald_front')
         for e in eye_lines.read(os.path.join(VD, name+'.png'), os.path.join(VD, 'points', name+'.json'), mpp*1000)[2].values()]
iris_mm = float(np.median(sizes)); scale = float(np.clip(iris_mm/IRIS_MM, 0.75, 1.15))
for sp in spots:
    cx, cy, r0, edge = sp; far = 1.8*edge; b0 = int(far)+2
    yy, xx = np.mgrid[int(cy)-b0:int(cy)+b0+1, int(cx)-b0:int(cx)+b0+1].astype(np.float64); dx, dy = xx-cx, yy-cy; rr = np.hypot(dx, dy)
    src = np.where(rr <= scale*edge, rr/scale, np.where(rr < far, edge+(rr-scale*edge)*(far-edge)/(far-scale*edge), rr))   # where a texel comes from
    k = src/np.maximum(rr, 1e-9)
    for ch in range(4):
        a[int(cy)-b0:int(cy)+b0+1, int(cx)-b0:int(cx)+b0+1, ch] = ndimage.map_coordinates(a[..., ch], [cy+dy*k, cx+dx*k], order=1, mode='nearest')
    sp[2], sp[3] = round(r0*scale, 1), int(round(edge*scale))
Image.fromarray(a.round().astype(np.uint8)).save(os.path.join(OUT, 'eye.png'))

# ---- the record and the picture
rec = {'hair_diff': HAIR_DIFF, 'gain': GAIN, 'strands': len(col), 'seen': round(float(seen.mean()), 3),
       'skin_like': round(float(skinlike.mean()), 3),
       'ramp_srgb': {f'{p:.2f}': [int(v) for v in srgb(ramp[int(p*255)]).round()] for p in (0.05, 0.25, 0.5, 0.75, 0.95)},
       'hair_median': [int(v) for v in srgb(np.median(col[kind < 3], 0)).round()],
       'beard_median': [int(v) for v in srgb(np.median(col[(kind == 3) | (kind == 4)], 0)).round()],
       'iris_srgb': [int(v) for v in srgb(iris).round()], 'irises_srgb': [[int(v) for v in srgb(c).round()] for c in irises],
       'eye_map_irises': spots, 'iris_mm': round(iris_mm, 2), 'irises_mm': sizes, 'iris_scale': round(scale, 3)}
json.dump(rec, open(os.path.join(OUT, 'colour.json'), 'w'), indent=1)
print('COLOUR', json.dumps(rec))
N = front.shape[0]; sheet = Image.new('RGB', (4*N, N+80), (122, 124, 128))
for i, (im, ang) in enumerate(((front, 0), (prof, 90))):
    sheet.paste(Image.fromarray(im.astype(np.uint8)), (2*i*N, 0))
    canvas = np.zeros((N, N, 3)) + 122; zb = np.full((N, N), 9.0)
    p3 = np.concatenate([G['pts'][:, ::4].reshape(-1, 3), B['pts'][:, ::2].reshape(-1, 3)])
    cc = np.concatenate([np.repeat(col[:nh], len(range(0, G['pts'].shape[1], 4)), 0),
                         np.repeat(col[nh:], len(range(0, B['pts'].shape[1], 2)), 0)])*GAIN
    if ang == 0: u, v, dep = fx+(p3[:, 0]-eye[0])/mpp, fy-(p3[:, 2]-eye[2])/mpp, p3[:, 1]
    else: u, v, dep = px+(p3[:, 1]-eye[1])/mpp, py-(p3[:, 2]-eye[2])/mpp, -p3[:, 0]
    ui, vi = u.round().astype(int), v.round().astype(int); ok = (ui >= 0) & (ui < N) & (vi >= 0) & (vi < N)
    ok &= (p3[:, 1] < axis_y) if ang == 0 else (p3[:, 0] > -0.01)    # no head here to hide the far side
    o = np.nonzero(ok)[0]; o = o[np.argsort(-dep[o])]
    canvas[vi[o], ui[o]] = srgb(cc[o])
    sheet.paste(Image.fromarray(canvas.clip(0, 255).astype(np.uint8)), ((2*i+1)*N, 0))
sheet.paste(Image.open(os.path.join(OUT, 'hair_ramp.png')).resize((2*N, 40)), (0, N))
sheet.paste(Image.open(os.path.join(OUT, 'hair_pigment.png')).resize((2*N, 40)), (0, N+40))
dr = ImageDraw.Draw(sheet)
dr.rectangle([2*N+10, N+5, 2*N+80, N+75], fill=tuple(int(v) for v in srgb(iris).round()))
eyeim = Image.open(os.path.join(OUT, 'eye.png')).convert('RGB').resize((N//2, N//2))
sheet.paste(eyeim.crop((0, 0, N//2, 80)).resize((N//2, 80)), (2*N+100, N))
sheet.resize((sheet.width//2, sheet.height//2), Image.LANCZOS).save(os.path.join(OUT, 'colour.jpg'), quality=88)
