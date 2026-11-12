"""Where the shape of a head is lost on the way from the concept to our head: a sheet and numbers by stage.

    python shape_report.py <work dir of build_head.py> [out dir, default <work dir>/shape]

Three things are compared at the concept's cameras (front and profile, the frame of the concept views):
- the concept, its bald sheet and its full one (outlines);
- the generator's bald model as it was placed on the concept (reg/place.json), drawn with its own colours;
- our head after the registration (round/reg.npz) and its strands (colour/groom.npz and beard_groom.npz, when they
  are built); the full model as it was put on our head (full/place.json), for the hair and the beard.

Writes shape.jpg (rows front and profile; columns: the bald concept with the three outlines, the model, our head
coloured by its distance from the model) and shape.json:
- regions: the signed distance of our skin from the model by part of the head, mm (+ = ours outside), and the share
  of the part the registration pulled to the model;
- rows: by height under the eyes, the face line in profile (mm in front of the eyes) and the head's width in front, for
  the concept, the model and our head: this is what the eye reads as "another jaw" or "other ears";
- hair: by direction over the head, how thick the hair is on the concept (full outline minus bald outline), on the
  full model as it was put on our head (its outline minus our skin) and on our head (strands minus skin): the first
  pair shows what the generator lost, the second what the strands lost; and how far the outline with hair lies from
  the full concept's (model_outline, ours_outline);
- beard: by height under the eyes, how far the same three stand over the bare outline: a side in front, forward in
  profile, and down under the chin by distance behind the chin's tip.
A distance to the nearest point of the model does not see a part the model has and our head does not reach (an ear's
rim): the rows do.
"""
import sys, os, json
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage as nd
from scipy.spatial import cKDTree

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', '..', 'reports', 'content', '2026-11-hero-head', 'skin'))
import ref_model

W = os.path.abspath(sys.argv[1]); OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(W, 'shape'); os.makedirs(OUT, exist_ok=True)
j = lambda *p: os.path.join(W, *p)
P = json.load(open(j('params.json'))); V = json.load(open(j('reg', 'views.json')))
mpp = 0.0615/P['ipd_px']; eye = np.array(V['eye']); R = P['res'][0]
EYE_PX = {'front': P['front_eye'], 'profile': P['profile_eye']}
BG = np.array([122, 124, 128])


def proj(p, view):
    """World metres -> pixel of the concept view: front looks along +y, profile from +x with the face to the left."""
    q = p-eye; ex, ey = EYE_PX[view]
    return np.c_[ex+q[:, 0 if view == 'front' else 1]/mpp, ey-q[:, 2]/mpp]


def mask_image(path):
    a = np.asarray(Image.open(path).convert('RGB')).astype(float)
    bg = np.median(np.r_[a[:20, :20].reshape(-1, 3), a[:20, -20:].reshape(-1, 3)], 0)
    m = nd.binary_fill_holes(nd.binary_opening(np.abs(a-bg).sum(-1) > 25, iterations=2))
    lab, n = nd.label(m)
    return lab == (np.argmax(np.bincount(lab.ravel())[1:])+1) if n else m


def mask_points(p, view, close=2):
    uv = np.round(proj(p, view)).astype(int); ok = (uv >= 0).all(1) & (uv < R).all(1)
    m = np.zeros((R, R), bool); m[uv[ok, 1], uv[ok, 0]] = True
    return nd.binary_fill_holes(nd.binary_closing(m, iterations=close))


def mask_tris(p, tris, view):
    uv = proj(p, view); im = Image.new('L', (R, R), 0); d = ImageDraw.Draw(im)
    for t in tris: d.polygon([tuple(uv[i]) for i in t], fill=255)
    return np.asarray(im) > 0


def edge(m): return m & ~nd.binary_erosion(m, iterations=2)


# ---- the three things
M = np.load(j('reg', 'mpfb.npz')); rg = np.load(j('round', 'reg.npz')); co = rg['co'].astype(float)
# the skin only: the template carries joint markers that are neither helpers nor skin (one stands 10 mm over the crown)
tris = M['tris'][np.load(j('reg', 'groups.npz'))['body'].astype(bool)[M['tris']].all(1)]
raw = np.load(j('mesh', 'bald', 'raw.npz'))
mv, mc = ref_model.load(raw, P, eye, {'place': json.load(open(j('reg', 'place.json'))), 'tone': None})   # the whole model
ref = np.load(j('reg', 'ref.npz')); rv, rn = ref['v'].astype(float), ref['n'].astype(float)               # its head, with normals
strands = [np.load(j('colour', n))['pts'].reshape(-1, 3) for n in ('groom.npz', 'beard_groom.npz') if os.path.exists(j('colour', n))]
hair = np.vstack(strands) if strands else None
fullm = None                                           # the full model as it was put on our head
if os.path.exists(j('full', 'place.json')):
    fullm = ref_model.load(np.load(j('mesh', 'full', 'raw.npz')), P, eye, {'place': json.load(open(j('full', 'place.json'))), 'tone': None})[0]

# ---- regions: our skin against the model
used = np.unique(tris); d, k = cKDTree(rv).query(co[used], workers=-1)
sd = np.zeros(len(co)); sd[used] = np.sign(((co[used]-rv[k])*rn[k]).sum(1))*d*1000
x, y, z = ((co-eye)*1000).T
inner = rg['interior'].astype(bool) | rg['socket'].astype(bool) | rg['cav'].astype(bool)
head = np.zeros(len(co), bool); head[used] = True; head &= (z > -150) & ~inner
REGIONS = {
    'forehead and skull top': head & (z > 15) & (y < 110),
    'face front': head & (z <= 15) & (z > -95) & (np.abs(x) < 35) & (y < 40),
    'cheeks': head & (z <= 15) & (z > -60) & (np.abs(x) >= 35) & (y < 60),
    'jaw line and angles': head & (z <= -60) & (z > -120) & (np.abs(x) >= 30) & (y < 110),
    'chin and under it': head & (z <= -95) & (np.abs(x) < 30) & (y < 60),
    'ears and behind them': head & (z <= 15) & (z > -60) & (y >= 60) & (y < 125) & (np.abs(x) > 55),
    'back of the head': head & (y >= 110) & (z > -60),
    'neck': head & (z <= -60) & (y >= 60),
}
out = {'regions': {}, 'rows': {}, 'hair': {}, 'beard': {}}
print(f"{'our skin from the model, mm':<30}{'median':>8}{'|median|':>9}{'p90':>7}{'pulled':>8}")
for name, m in REGIONS.items():
    a = sd[m]; r = {'median': round(float(np.median(a)), 1), 'abs_median': round(float(np.median(np.abs(a))), 1),
                    'abs_p90': round(float(np.percentile(np.abs(a), 90)), 1), 'pulled': round(float((rg['w'][m] > 0.5).mean()), 2)}
    out['regions'][name] = r; print(f"{name:<30}{r['median']:8.1f}{r['abs_median']:9.1f}{r['abs_p90']:7.1f}{r['pulled']:8.2f}")

# ---- outlines, the model's picture, the rows
def model_picture(view):
    uv = np.round(proj(mv, view)).astype(int); q = mv-eye
    dep, look = (q[:, 1], 1) if view == 'front' else (-q[:, 0], 0)
    n = np.zeros_like(mv); f = raw['faces']; fn = np.cross(mv[f[:, 1]]-mv[f[:, 0]], mv[f[:, 2]]-mv[f[:, 0]])
    for c in range(3): np.add.at(n, f[:, c], fn)
    n /= np.linalg.norm(n, axis=1, keepdims=True)+1e-12
    ok = (uv >= 1).all(1) & (uv < R-1).all(1)
    D = np.full((R, R), 9, np.float32); np.minimum.at(D, (uv[ok, 1], uv[ok, 0]), dep[ok]); D = nd.minimum_filter(D, 5)
    ok &= dep < D[np.clip(uv[:, 1], 0, R-1), np.clip(uv[:, 0], 0, R-1)]+0.003
    i = np.nonzero(ok)[0]; i = i[np.argsort(-dep[i])]; img = np.zeros((R, R, 3), np.float32)+BG
    sh = (0.72+0.28*np.abs(n[i, look]))[:, None]
    for du in (-1, 0, 1):
        for dv in (-1, 0, 1): img[uv[i, 1]+dv, uv[i, 0]+du] = mc[i]*sh
    return img.clip(0, 255).astype(np.uint8)


def colour(v):                                        # white within 1 mm, blue: ours inside the model, red: outside
    t = 0 if abs(v) < 1 else min(abs(v)/8, 1)
    return tuple(int(255*(1-t)+c*t) for c in ((220, 40, 30) if v > 0 else (30, 80, 230)))


def our_picture(view):
    uv = proj(co, view); look = np.array([0, 1, 0]) if view == 'front' else np.array([-1, 0, 0])
    n = np.cross(co[tris[:, 1]]-co[tris[:, 0]], co[tris[:, 2]]-co[tris[:, 0]]); n /= np.linalg.norm(n, axis=1, keepdims=True)+1e-12
    dep = (co-eye)@look; im = Image.new('RGB', (R, R), (60, 60, 64)); dr = ImageDraw.Draw(im)
    for t in np.argsort(-dep[tris].mean(1)):
        if n[t]@look >= 0: continue
        s = 0.55+0.45*abs(n[t]@look); dr.polygon([tuple(uv[i]) for i in tris[t]], fill=tuple(int(q*s) for q in colour(sd[tris[t]].mean())))
    return im


ANG = {'front': list(range(-75, 76, 15)), 'profile': list(range(-45, 106, 15))}    # 0 = up; + = to the right / to the back
tiles = []
for view in ('front', 'profile'):
    ms = {'concept': mask_image(j('views', f'bald_{view}.png')), 'full': mask_image(j('views', f'full_{view}.png')),
          'model': mask_points(mv, view), 'ours': mask_tris(co, tris, view)}
    if hair is not None: ms['hair'] = mask_points(hair, view, 3) | ms['ours']
    if fullm is not None: ms['full model'] = mask_points(fullm, view)
    ex, ey = EYE_PX[view]; rows = list(range(20, 121, 10)); line = {}

    def at_row(m, dz):
        """Profile: the face line, mm from the eyes (- = forward). Front: the width of the run through the middle
        (lower down the row also crosses the shoulders)."""
        row = m[int(round(ey+dz/1000/mpp))]; c = np.where(row)[0]
        if not len(c): return None
        if view == 'profile': return float((c[0]-ex)*mpp*1000)
        mid = int(ex)
        if not row[mid]: return None
        gaps = np.where(~row)[0]; lo = gaps[gaps < mid].max()+1 if (gaps < mid).any() else 0; hi = gaps[gaps > mid].min()-1 if (gaps > mid).any() else R-1
        return float((hi-lo)*mpp*1000)
    for name in ('concept', 'model', 'ours'):
        line[name] = [None if at_row(ms[name], dz) is None else round(at_row(ms[name], dz), 1) for dz in rows]
    out['rows'][view] = {'mm_under_the_eyes': rows, 'what': 'face line, mm from the eyes (- = forward)' if view == 'profile' else 'width, mm', **line}
    print(view, out['rows'][view]['what']); print(f"{'under the eyes, mm':>20}", ' '.join(f'{r:>6}' for r in rows))
    for name in ('concept', 'model', 'ours'): print(f'{name:>20}', ' '.join(f'{v:6.1f}' if v is not None else '     -' for v in line[name]))
    # hair: thickness along rays from the middle of the skull at eye level
    if view == 'front': cx = ex
    else: c = np.where(ms['concept'][:int(ey)].any(0))[0]; cx = (c[0]+c[-1])/2
    def reach(m, a):
        t = np.radians(a); s = np.arange(0, 420, 0.5); u = (cx+np.sin(t)*s).astype(int); v = (ey-np.cos(t)*s).astype(int)
        ok = (u >= 0) & (u < R) & (v >= 0) & (v < R); hit = np.nonzero(m[v[ok], u[ok]])[0]
        return s[ok][hit[-1]]*mpp*1000 if len(hit) else 0.0
    h = {'angle_deg': ANG[view], 'concept': [round(reach(ms['full'], a)-reach(ms['concept'], a), 1) for a in ANG[view]]}
    if fullm is not None: h['model'] = [round(reach(ms['full model'], a)-reach(ms['ours'], a), 1) for a in ANG[view]]
    if hair is not None: h['ours'] = [round(reach(ms['hair'], a)-reach(ms['ours'], a), 1) for a in ANG[view]]
    # and the outline itself: our skin is not the bald concept's to the millimetre, so a layer of the right thickness
    # may still end beside the concept's outline
    if fullm is not None: h['model_outline'] = [round(reach(ms['full model'], a)-reach(ms['full'], a), 1) for a in ANG[view]]
    if hair is not None: h['ours_outline'] = [round(reach(ms['hair'], a)-reach(ms['full'], a), 1) for a in ANG[view]]
    out['hair'][view] = h
    print(view, 'hair over the skull, mm, by direction (0 = up):', ' '.join(f'{a:>6}' for a in ANG[view]))
    for name in ('concept', 'model', 'ours'):
        if name in h: print(f'{name:>20}', ' '.join(f'{v:6.1f}' for v in h[name]))
    print(view, 'the outline with hair against the full concept\'s, mm (+ = outside):')
    for name in ('model_outline', 'ours_outline'):
        if name in h: print(f'{name:>20}', ' '.join(f'{v:6.1f}' for v in h[name]))
    # the beard: how far the outline with it stands over the bare one, by height under the eyes
    brow = list(range(50, 131, 10)); pairs = {'concept': ('full', 'concept'), 'model': ('full model', 'ours'), 'ours': ('hair', 'ours')}
    side = 0.5 if view == 'front' else -1.0                                # a side in front; forward in profile

    def over(a, b, dz):
        p, q = at_row(ms[a], dz), at_row(ms[b], dz)
        return None if p is None or q is None else round(side*(p-q), 1)
    b_ = {'mm_under_the_eyes': brow, 'what': 'the beard over the bare outline, mm: ' + ('a side' if view == 'front' else 'forward')}
    for name, (a, b) in pairs.items():
        if a in ms: b_[name] = [over(a, b, dz) for dz in brow]
    if view == 'profile':                                                  # and down, under the chin
        tip = np.where(ms['concept'][int(round(ey+0.10/mpp))])[0]; cols = list(range(10, 51, 10))

        def under(m, u):
            v = int(round(ey+0.075/mpp))
            if not m[v, u]: return None
            while v < R-1 and m[v, u]: v += 1
            return (v-ey)*mpp*1000
        if len(tip):
            b_['mm_behind_the_chin_tip'] = cols
            for name, (a, b) in pairs.items():
                if a not in ms: continue
                d = [(under(ms[a], int(tip[0]+c/1000/mpp)), under(ms[b], int(tip[0]+c/1000/mpp))) for c in cols]
                b_[name+'_under_the_chin'] = [None if p is None or q is None else round(p-q, 1) for p, q in d]
    out['beard'][view] = b_
    print(view, b_['what'] + '; under the eyes, mm:', ' '.join(f'{r:>6}' for r in brow))
    for name in pairs:
        if name in b_: print(f'{name:>20}', ' '.join(f'{v:6.1f}' if v is not None else '     -' for v in b_[name]))
    if 'mm_behind_the_chin_tip' in b_:
        print(view, 'the beard under the chin, mm down; behind the chin tip, mm:', ' '.join(f'{c:>6}' for c in b_['mm_behind_the_chin_tip']))
        for name in pairs:
            if name+'_under_the_chin' in b_: print(f'{name:>20}', ' '.join(f'{v:6.1f}' if v is not None else '     -' for v in b_[name+'_under_the_chin']))
    a = np.asarray(Image.open(j('views', f'bald_{view}.png')).convert('RGB')).copy()
    for name, c in (('full', (90, 160, 255)), ('concept', (255, 255, 255)), ('model', (255, 200, 0)), ('full model', (0, 220, 120)), ('hair', (255, 0, 255)), ('ours', (255, 40, 40))):
        if name in ms: a[edge(ms[name])] = c
    tiles.append([Image.fromarray(a), Image.fromarray(model_picture(view)), our_picture(view)])

T = 600; sheet = Image.new('RGB', (3*T, 2*T+40), (60, 60, 64)); dr = ImageDraw.Draw(sheet)
for r, row in enumerate(tiles):
    for c, t in enumerate(row): sheet.paste(t.resize((T, T), Image.LANCZOS), (c*T, r*T))
dr.text((8, 2*T+4), 'outlines on the bald concept: white concept, blue full concept, yellow bald model, green full model, red our skin, magenta our strands', fill=(230, 230, 230))
dr.text((8, 2*T+20), 'right: our skin by its distance from the model; blue inside, red outside, full colour at 8 mm, white within 1 mm', fill=(230, 230, 230))
sheet.save(os.path.join(OUT, 'shape.jpg'), quality=90)
json.dump(out, open(os.path.join(OUT, 'shape.json'), 'w'), indent=1)
print('SHAPE', os.path.join(OUT, 'shape.jpg'))
