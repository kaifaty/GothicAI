"""Fog over translucent surfaces, before / after: the black smoke probe of M5 r5 on two engine builds.
usage: fog_ab.py CAPTURES OUT_DIR
CAPTURES/<build>/<set>/<view>[-night][-b].png for builds 'before' and 'after' (one source tree apart), sets
harbour-life-8 (A: the harbour without the smoke), r5-k52 (the same smoke with vertex colour 0: it only absorbs,
alpha 52) and, if present, r5-g115a52 (grey 115). Frames come from the M5 smoke tooling (PR #637, capture_r5.sh).
- coverage c = 1 - Y_probe / Y_A on scene luminance (the inverse of the game's ACES), mean of a shot and its -b pair
  0.7 s later, Gaussian 3 px, inside the plume's projected path where the night probe covers c >= 0.05.
  Black absorbs alike by day and by night: only light laid over it changes c. Day/night by the background of A at noon.
- A after against A before: share of pixels that moved by more than 2 levels (only translucent effects may move).
writes OUT_DIR/fog-ab.json and OUT_DIR/fog-ab.jpg
"""
import json, math, os, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy import ndimage

CAMS = {'huts': ([2512, 0, -4106], [4018, 300, -4061]), 'huts-north': ([3000, 30, -1400], [2880, 350, -2500]),
        'hole': ([261, 1707, -2428], [2859, 507, -2300]), 'backlit': ([4805, 325, -4054], [4005, 507, -4000])}
HOLES = {'04': ((2859, 207, -2428), 30), '06': ((4005, 257, -4054), 15)}  # smoke hole, emitter radius (cm)
A, PROBE, GREY = 'harbour-life-8', 'r5-k52', 'r5-g115a52'
W = np.array([0.2126, 0.7152, 0.0722])


def project(view, p, w=1280, h=720, hfov=90.0):
    e, t = (np.array(v, float) for v in CAMS[view])
    f = (t - e) / np.linalg.norm(t - e); r = np.cross(f, [0, 1, 0]); r /= np.linalg.norm(r); u = np.cross(r, f)
    fx = (w / 2) / math.tan(math.radians(hfov / 2)); d = np.asarray(p, float) - e; z = d @ f
    return None if z <= 0 else (w / 2 - (d @ r) / z * fx, h / 2 - (d @ u) / z * fx, z)  # the game's x runs mirrored


def path_mask(view, shape=(720, 1280)):
    """pixels the plume can cover: birth 30 cm over the hole, 0.75 m/s up, +z drift 0.1 m/s^2, 10 s life"""
    roi = np.zeros(shape, bool); yy, xx = np.mgrid[0:shape[0], 0:shape[1]]
    for (x, y, z), src in HOLES.values():
        if math.dist(CAMS[view][0], (x, y + 30, z)) > 4000:
            continue  # beyond the 40 m radius of birth
        for t in np.arange(0, 11.01, 0.25):
            q = project(view, (x, y + 30 + 75 * t, z + 0.5 * 10 * t * t))
            if q is None: continue
            half = 0.5 * 70 * (1 + 4 * t / 10) * 0.65 + src + 75 * 0.17 * t
            roi |= np.hypot(xx - q[0], yy - q[1]) < half / q[2] * 640
    return roi


def lin(rgb):
    """scene luminance before the tonemap: a pixel is ACES(hdr)^(1/2.2); exposure is the same in both frames"""
    x = np.clip((rgb / 255.0) ** 2.2, 0, 0.98)
    return ((-0.59 * x + 0.03 - np.sqrt(-1.0127 * x * x + 1.3702 * x + 0.0009)) / (2.0 * (2.43 * x - 2.51))) @ W


def load(root, build, s, name):
    p = f'{root}/{build}/{s}/{name}.png'
    return np.asarray(Image.open(p).convert('RGB'), float) if os.path.exists(p) else None


def coverage(root, build, s, name, night):
    fa = [load(root, build, A, name + b) for b in ('', '-b')]
    fo = [load(root, build, s, name + b) for b in ('', '-b')]
    c = ndimage.gaussian_filter(np.clip(np.mean([1 - lin(o) / np.maximum(lin(a), 1e-4) for o, a in zip(fo, fa)], 0), -1, 1), 3)
    c[lin(fa[0]) < (0.002 if night else 0.01)] = np.nan  # too dark for a ratio
    return c


def classes(a):
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    sky = (b - r > 20) & (b > g); crown = ~sky & (g > r + 3) & (a @ W < 90)
    return {'sky': sky, 'crown': crown, 'other': ~sky & ~crown}


def measure(root):
    out = {}
    for view in CAMS:
        roi = path_mask(view); row = {}
        for build in ('before', 'after'):
            day, night = coverage(root, build, PROBE, view, False), coverage(root, build, PROBE, view + '-night', True)
            body = roi & (np.nan_to_num(night) >= 0.05) & ~np.isnan(day)
            res = {}
            for name, m in classes(load(root, 'before', A, view)).items():
                m = m & body
                if m.sum() < 40: continue
                d, n = float(np.median(day[m])), float(np.median(night[m]))
                res[name] = dict(day=round(d, 3), night=round(n, 3), ratio=round(d / n, 2), px=int(m.sum()))
            row[build] = res
        for t in ('', '-night'):
            a0, a1 = load(root, 'before', A, view + t), load(root, 'after', A, view + t)
            dy = np.abs(a1 @ W - a0 @ W)
            row['A' + t] = dict(moved=round(float((dy > 2).mean()), 4), max=round(float(dy.max()), 1))
        out[view] = row
    return out


def sheet(root, path):
    font = ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial.ttf', 15)
    cols = [('before', A, 'без дыма'), ('before', PROBE, 'до: чёрный зонд'), ('after', PROBE, 'после: чёрный зонд'),
            ('before', GREY, 'до: серый 115'), ('after', GREY, 'после: серый 115')]
    cols = [c for c in cols if os.path.isdir(f'{root}/{c[0]}/{c[1]}')]
    rows = []
    for view in CAMS:
        ys, xs = np.nonzero(path_mask(view))
        box = (max(int(xs.min()) - 40, 0), max(int(ys.min()) - 40, 0), min(int(xs.max()) + 40, 1280), min(int(ys.max()) + 40, 720))
        box = (box[0], box[1], min(box[2], box[0] + int(1.4 * (box[3] - box[1]))), box[3])  # the plume, not the whole sky
        crops = []  # noon only: at midnight the haze is next to nothing and the fix changes little
        for build, s, _ in cols:
            c = Image.fromarray(np.uint8(load(root, build, s, view))).crop(box)
            crops.append(c.resize((c.width * 240 // c.height, 240)))
        w, h = crops[0].size; r = Image.new('RGB', (len(crops) * (w + 6), h + 22), 'white'); dr = ImageDraw.Draw(r)
        for i, c in enumerate(crops):
            r.paste(c, (i * (w + 6), 22)); dr.text((i * (w + 6) + 3, 2), f'{view}: {cols[i][2]}', fill='black', font=font)
        rows.append(r)
    width = max(r.width for r in rows); im = Image.new('RGB', (width, sum(r.height for r in rows)), 'white'); y = 0
    for r in rows: im.paste(r, (0, y)); y += r.height
    im.save(path, quality=88)


if __name__ == '__main__':
    root, out = sys.argv[1], sys.argv[2]; os.makedirs(out, exist_ok=True)
    res = measure(root)
    json.dump(res, open(f'{out}/fog-ab.json', 'w'), indent=1, ensure_ascii=False)
    for view, row in res.items():
        print(view, json.dumps(row, ensure_ascii=False))
    sheet(root, f'{out}/fog-ab.jpg')
