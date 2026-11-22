# Brows as hairs painted into the skin map, laid out by the brow of the concept.
#   python brow_paint.py uv.npz groups.npz views.json concept_front.png concept_front.json skin.png out.png [seed]
# concept_front.png is the concept in the front frame of views.json (concept_frame.py), concept_front.json its face
# points (scripts/content/npc/face_points.py). A concept is 0.4 mm a pixel: projected onto the skin its brow is a
# soft dark patch without hairs. Here the concept gives only what it can: the place, the outline, the density and
# the colour of each brow. The hairs are drawn anew, in the map's own resolution:
# - the brow's region is the detector's brow outline, widened, and inside it the darkness of the concept against
#   the forehead; the centre line and the thickness along it come from that mask;
# - roots are scattered by the density; a hair's direction follows the anatomy of a brow: at the inner end hairs
#   stand up, along the body the lower ones go up and out and the upper ones lie over them, at the tail they run
#   along the brow; `BUSHY` lifts the upper hairs as in an unkempt brow;
# - under the hairs the skin keeps a soft shade of the hair's colour, so thin places do not show as lines on bare
#   skin; the old projected brow is replaced by skin filled in from around it.
# Writes the map with the brows and <out>.brows.jpg, a close-up of the two brows in the map.
import sys, os, json
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage
from scipy.interpolate import LinearNDInterpolator
import uvmap

UV, GR, VJ, CF, CJ, SKIN, OUT = sys.argv[1:8]; rng = np.random.default_rng(int(sys.argv[8]) if len(sys.argv) > 8 else 1)
PER_MM2, WIDTH, BUSHY, SS = 1.25, 0.00011, 0.6, 3          # hairs per mm2 at full density, hair width (m), 0..1, supersampling
d = np.load(UV); g = np.load(GR); V = json.load(open(VJ)); J = json.load(open(CJ))
co, no, tri, tuv = d['co'], d['no'], d['tri'], d['tuv']
eye = np.array(V['eye']); NV, S = V['N'], V['s']; fc = V['views']['front']['loc']
C = np.asarray(Image.open(CF).convert('RGB'), np.float32); P = np.array(J['points'])[:, :2]
skin = np.asarray(Image.open(SKIN).convert('RGB'), np.float32).copy(); N = skin.shape[0]

to_px = lambda x, z: ((x-fc[0])/S+NV/2, (fc[2]-z)/S+NV/2)          # world -> concept pixel (u, v)
lum = C.mean(2)
OUTLINE = {'r': ([70, 63, 105, 66, 107], [55, 65, 52, 53, 46]), 'l': ([300, 293, 334, 296, 336], [285, 295, 282, 283, 276])}

# the head's texels around the brows: the front of the face above the eyes
zc = co[tri].mean(1); faces = np.nonzero(g['body'][tri].all(1) & (np.abs(zc[:, 0]) < 0.085) & (zc[:, 2] > eye[2]-0.005) &
                                         (zc[:, 2] < eye[2]+0.05) & (zc[:, 1] < eye[1]+0.04))[0]
pos, nor, hit = uvmap.raster(co, no, tri, tuv, N, faces)
texel = np.median(np.linalg.norm(pos[:, 1:]-pos[:, :-1], axis=2)[hit[:, 1:] & hit[:, :-1]])
print('BROWS texel %.3f mm, hair %.2f texels wide' % (1000*texel, WIDTH/texel))

sheet = []
for side, (up, lo) in OUTLINE.items():
    # ---- the brow of the concept: region, density, colour
    poly = Image.new('L', (NV, NV), 0); ImageDraw.Draw(poly).polygon([tuple(P[i]) for i in up+lo], fill=255)
    reg = ndimage.binary_dilation(np.asarray(poly) > 0, iterations=int(0.0025/S))
    up_reg = reg.copy()
    for k in range(1, int(0.004/S)+1): up_reg[:-k] |= reg[k:]                                 # hairs stand up over the outline
    reg = up_reg
    ys, xs = np.nonzero(reg); band = lum[max(ys.min()-int(0.012/S), 0):ys.min(), xs.min():xs.max()]
    ref = np.median(band)                                                                     # the forehead over the brow
    dark = np.clip((ref-lum)/(0.45*ref), 0, 1)*reg
    D = ndimage.gaussian_filter(dark, 1.5); M = D > 0.5      # the shade of the brow ridge under the brow is weaker than the hair
    lab, n = ndimage.label(M); M = lab == (1+np.argmax(ndimage.sum(M, lab, range(1, n+1))))
    wide = ndimage.gaussian_filter((D > 0.25)*ndimage.binary_dilation(M, iterations=int(0.004/S))*1.0, 2.0)   # all that the projected brow darkened
    D = D*ndimage.binary_dilation(M, iterations=4)
    core = M & (D > np.percentile(D[M], 60))
    dark_col, hair_col, light_col = (np.percentile(C[core], p, axis=0) for p in (20, 60, 93))   # shade between hairs, hair, lit hair
    # ---- the centre line and the thickness, as functions of the column
    cols = np.nonzero(M.any(0))[0]; rows = np.arange(NV)[:, None]
    cen = (D*M*rows).sum(0)[cols]/np.maximum((D*M).sum(0)[cols], 1e-9); thick = M.sum(0)[cols]*S
    pz = np.polyfit(cols, cen, 3); pt = np.polyfit(cols, thick, 3)
    inner_first = abs(cols[0]-NV/2) < abs(cols[-1]-NV/2); c0, c1 = (cols[0], cols[-1]) if inner_first else (cols[-1], cols[0])
    out_sign = np.sign(c1-c0)                                                                 # the way to the temple, in columns
    # ---- roots by density
    area = M.sum()*S*S*1e6; count = int(area*PER_MM2)
    w = (D*M).ravel(); idx = rng.choice(w.size, count, p=w/w.sum()); rv_, ru = np.divmod(idx, NV)
    ru = ru+rng.uniform(-.5, .5, count); rv_ = rv_+rng.uniform(-.5, .5, count)
    s = np.clip((ru-c0)/(c1-c0), 0, 1)                                                        # 0 at the nose, 1 at the temple
    slope = np.polyval(np.polyder(pz), ru); tang = np.stack([np.ones(count)*out_sign, slope*out_sign], 1)
    tang /= np.linalg.norm(tang, axis=1, keepdims=True); upv = np.stack([tang[:, 1]*out_sign, -tang[:, 0]*out_sign], 1)   # up = towards smaller rows
    doff = np.clip((np.polyval(pz, ru)-rv_)*S/np.maximum(np.polyval(pt, ru)/2, 0.002), -1.2, 1.2)   # +1 at the top edge
    sm = lambda a, b, x: np.clip((x-a)/(b-a), 0, 1)**2*(3-2*np.clip((x-a)/(b-a), 0, 1))
    w_head = 1-sm(0.05, 0.30, s); w_tail = sm(0.60, 0.92, s)
    th_body = -28*doff*(1-BUSHY)+8+24*BUSHY; th_tail = -10*doff+4
    th = np.radians(w_head*78+(1-w_head)*((1-w_tail)*th_body+w_tail*th_tail)+rng.normal(0, 15, count))
    length = rng.uniform(0.0055, 0.0095, count)*(1-0.35*w_tail)*(0.65+0.35*D[rv_.astype(int).clip(0, NV-1), ru.astype(int).clip(0, NV-1)])
    # ---- the hairs as polylines in concept pixels, then in the map
    K = 9; t = np.linspace(0, 1, K)[None, :]
    ang = th[:, None]*(1-0.55*t)+rng.normal(0, 0.12, (count, 1))*t                           # a hair lies down towards its tip
    step = (length/S/(K-1))[:, None, None]*(np.cos(ang)[..., None]*tang[:, None]+np.sin(ang)[..., None]*upv[:, None])
    pts = np.stack([ru, rv_], 1)[:, None]+np.concatenate([np.zeros((count, 1, 2)), np.cumsum(step[:, :-1], 1)], 1)
    # the map under this brow: texels whose surface faces the camera, by their place in the concept
    # (one piece of the map only: lids, sockets and lashes have islands of their own with the same place in the picture)
    pu, pv = to_px(pos[..., 0], pos[..., 2]); m = int(0.012/S)
    near = hit & (nor[..., 1] < -0.2) & (pu > xs.min()-m) & (pu < xs.max()+m) & (pv > ys.min()-m) & (pv < ys.max()+m)
    lab, n = ndimage.label(near); inside = M[pv.astype(int).clip(0, NV-1), pu.astype(int).clip(0, NV-1)] & near
    near = lab == (1+np.argmax(ndimage.sum(inside, lab, range(1, n+1))))
    ty, tx = np.nonzero(near); tu, tv = pu[ty, tx], pv[ty, tx]; sub = slice(None, None, 3)
    f = LinearNDInterpolator(np.stack([tu, tv], 1)[sub], np.stack([tx, ty], 1)[sub].astype(float))
    mp_ = f(pts.reshape(-1, 2)).reshape(count, K, 2); ok = np.isfinite(mp_).all((1, 2)); mp_ = mp_[ok]
    # ---- the skin under the brow: the projected brow gives way to skin from around it, shaded by the density
    Dt = np.zeros((N, N), np.float32); Dt[ty, tx] = ndimage.map_coordinates(D, [tv, tu], order=1)
    x0, x1, y0, y1 = int(np.nanmin(mp_[..., 0]))-12, int(np.nanmax(mp_[..., 0]))+12, int(np.nanmin(mp_[..., 1]))-12, int(np.nanmax(mp_[..., 1]))+12
    Wt = np.zeros((N, N), np.float32); Wt[ty, tx] = ndimage.map_coordinates(wide, [tv, tu], order=1)
    crop = skin[y0:y1, x0:x1]; dc = ndimage.gaussian_filter(Dt[y0:y1, x0:x1], 2.0); wc = ndimage.gaussian_filter(Wt[y0:y1, x0:x1], 2.0)
    old = wc > 0.04; ix = ndimage.distance_transform_edt(old, return_distances=False, return_indices=True)
    bare = ndimage.gaussian_filter(crop[ix[0], ix[1]], (5, 5, 0)); a = np.clip(wc/0.3, 0, 1)[..., None]
    under = crop*(1-a)+bare*a
    shade = (0.38*np.clip(dc, 0, 1))[..., None]; under = under*(1-shade)+dark_col*shade
    # ---- draw: every hair on its own patch, root wide and dense, tip thin and light
    h, wd = under.shape[:2]; acc = np.zeros((h*SS, wd*SS, 3), np.float32); cov = np.zeros((h*SS, wd*SS), np.float32)
    q = (mp_-[x0, y0])*SS; wpx = WIDTH/texel*SS
    for i in rng.permutation(len(q)):
        f_ = rng.uniform(-0.5, 1.0); col = hair_col+(light_col-hair_col)*f_ if f_ > 0 else hair_col+(hair_col-dark_col)*f_; bx0, by0 = np.floor(q[i].min(0)).astype(int)-3; bx1, by1 = np.ceil(q[i].max(0)).astype(int)+3
        bx0, by0, bx1, by1 = max(bx0, 0), max(by0, 0), min(bx1, wd*SS), min(by1, h*SS)
        if bx1 <= bx0 or by1 <= by0: continue
        patch = Image.new('L', (bx1-bx0, by1-by0), 0); dr = ImageDraw.Draw(patch)
        for k in range(K-1):
            fr = k/(K-1); dr.line([tuple(q[i, k]-[bx0, by0]), tuple(q[i, k+1]-[bx0, by0])], fill=int(255*(0.95-0.5*fr)), width=max(1, int(round(wpx*(1-0.6*fr)))))
        al = np.asarray(patch, np.float32)/255
        sl = (slice(by0, by1), slice(bx0, bx1))
        acc[sl] = acc[sl]*(1-al[..., None])+col*al[..., None]; cov[sl] = cov[sl]*(1-al)+al
    acc = acc.reshape(h, SS, wd, SS, 3).mean((1, 3)); cov = cov.reshape(h, SS, wd, SS).mean((1, 3))[..., None]
    skin[y0:y1, x0:x1] = under*(1-cov)+acc
    sheet.append(Image.fromarray(skin[y0:y1, x0:x1].clip(0, 255).astype(np.uint8)))
    print('BROW %s: %.0f mm2, %d hairs (%d on the map), colour %s, forehead lum %.0f' % (side, area, count, len(mp_), hair_col.round(0), ref))
Image.fromarray(skin.clip(0, 255).astype(np.uint8)).save(OUT)
hh = max(im.size[1] for im in sheet); sh = Image.new('RGB', (sum(im.size[0] for im in sheet)+10, hh), (40, 40, 40)); x = 0
for im in sheet: sh.paste(im, (x, 0)); x += im.size[0]+10
sh.resize((sh.size[0]*2, sh.size[1]*2), Image.LANCZOS).save(os.path.splitext(OUT)[0]+'.brows.jpg', quality=90)
