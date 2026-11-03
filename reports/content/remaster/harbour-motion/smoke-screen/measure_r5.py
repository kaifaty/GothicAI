"""r5 smoke numbers by the research card, before any reviewer.
usage: measure_r5.py CAPTURES_DIR [--json OUT] [--sheets DIR]
Sets: harbour-life-8 (side A, no smoke), black probes r5-k29 / r5-k52 (vertex colour 0: pure absorption),
variants r5-g{75,95,115}a{29,52}. Every number is averaged over a shot and its -b pair (0.7 s later).
- coverage (probe of the variant's alpha): c = 1 - Y_probe / Y_A on scene luminance (inverse of the game's ACES), Gaussian 3 px;
  only where the plume can be: within the projected path of the particles (birth 30 cm over the hole,
  0.75 m/s up, +z drift 0.1 m/s^2), half-width = sprite half-size at that age + the source radius;
  by height over the hole: throat 0-3 m, 3-6 m, 6-10 m; p90 (the body) and p99 (nowhere above 0.5)
- tone: dY = luma(B) - luma(A), 0-255 sRGB, mean over the body (probe c >= 0.05) by the background of A at noon
  (sky / crown / other, as r3's measure.py); own = dY / c, the smoke's luma step over its background
- motion: mean |dY| between a shot and its -b pair in the body, B and A
"""
import json, math, os, sys
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

from pathlib import Path
ROOT = str(Path(__file__).resolve().parents[5])
CAMS = {c['name']: c for c in json.load(open(f'{ROOT}/reports/content/remaster/harbour-motion/cameras.json'))['cameras']}
HOLES = {'04': (2859, 207, -2428), '06': (4005, 257, -4054)}
SOURCE = {'04': 30, '06': 15}
VIEWS = ['huts', 'huts-north', 'hole', 'backlit']
A = 'harbour-life-8'
VARIANTS = [f'r5-g{g}a{a}' for g in (75, 95, 115) for a in (29, 52)]
W = np.array([0.2126, 0.7152, 0.0722])
BANDS = [(0, 300, 'throat 0-3 m'), (300, 600, '3-6 m'), (600, 1000, '6-10 m')]


def project(cam, pts, w=1280, h=720, hfov=90.0):
    e, t = np.array(cam['eye'], float), np.array(cam['target'], float)
    f = (t - e) / np.linalg.norm(t - e); r = np.cross(f, [0, 1, 0]); r /= np.linalg.norm(r); u = np.cross(r, f)
    fx = (w / 2) / math.tan(math.radians(hfov / 2)); out = []
    for p in pts:
        d = np.asarray(p, float) - e; z = d @ f
        out.append(None if z <= 0 else (w / 2 - (d @ r) / z * fx, h / 2 - (d @ u) / z * fx, z))  # the game's x runs mirrored
    return out


def path_mask(view, shape=(720, 1280)):
    """height over the hole (cm) of every pixel the plume can cover, else nan; nearest path point wins"""
    hgt = np.full(shape, np.nan); best = np.full(shape, np.inf)
    yy, xx = np.mgrid[0:shape[0], 0:shape[1]]
    cam = CAMS[view]
    for hut, (x, y, z) in HOLES.items():
        if math.dist(cam['eye'], (x, y + 30, z)) > 4000:
            continue  # beyond the radius of birth: no particles from this hut
        for t in np.arange(0, 11.01, 0.25):
            up = 30 + 75 * t; p = (x, y + up, z + 0.5 * 10 * t * t)
            q = project(cam, [p])[0]
            if q is None: continue
            half = 0.5 * 70 * (1 + 4 * t / 10) * 0.65 + SOURCE[hut] + 75 * 0.17 * t  # sprite footprint, source, cone ±10°
            rad = half / q[2] * (640 / math.tan(math.radians(45)))
            d = np.hypot(xx - q[0], yy - q[1])
            sel = (d < rad) & (d < best)
            best[sel] = d[sel]; hgt[sel] = up
    return hgt


def lin(rgb):
    """scene luminance before the tonemap: a pixel is ACES(hdr)^(1/2.2) (lighting/tonemapping.glsl gameTonemap,
    brightness 0, contrast 1 at the default video settings); exposure is the same in both frames and cancels"""
    x = np.clip((rgb / 255.0) ** 2.2, 0, 0.98)
    hdr = (-0.59 * x + 0.03 - np.sqrt(-1.0127 * x * x + 1.3702 * x + 0.0009)) / (2.0 * (2.43 * x - 2.51))
    return hdr @ W
def luma(rgb): return rgb @ W
def load(root, s, name):
    p = f'{root}/{s}/{name}.png'
    return np.asarray(Image.open(p).convert('RGB'), float) if os.path.exists(p) else None


def classes(a):
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    sky = (b - r > 20) & (b > g)
    crown = ~sky & (g > r + 3) & (luma(a) < 90)
    return {'sky': sky, 'crown': crown, 'other': ~sky & ~crown}


def measure(root):
    out = {}
    for view in VIEWS:
        hgt = path_mask(view); roi = ~np.isnan(hgt)
        a_day = load(root, A, view)
        if a_day is None: continue
        cl = classes(a_day)
        for t in ('', '-night'):
            name = f'{view}{t}'
            fa = [load(root, A, name + s) for s in ('', '-b')]
            if fa[0] is None: continue
            cov = {}
            for k in ('29', '52'):
                fo = [load(root, f'r5-k{k}', name + s) for s in ('', '-b')]
                cs = [1 - lin(o) / np.maximum(lin(a), 1e-4) for o, a in zip(fo, fa) if o is not None]
                c = ndimage.gaussian_filter(np.clip(np.mean(cs, 0), -1, 1), 3)
                c[lin(fa[0]) < (0.002 if t else 0.01)] = np.nan  # too dark for a ratio
                cov[k] = c
            noise = ndimage.gaussian_filter(1 - lin(fa[1]) / np.maximum(lin(fa[0]), 1e-4), 3) if fa[1] is not None else None
            row = {'roi_px': int(roi.sum())}
            for k, c in cov.items():
                bands = {}
                for lo, hi, label in BANDS:
                    m = roi & (hgt >= lo) & (hgt < hi) & ~np.isnan(c)
                    if m.sum() < 30: bands[label] = None; continue
                    v = c[m]
                    bands[label] = dict(p90=round(float(np.percentile(v, 90)), 3), p99=round(float(np.percentile(v, 99)), 3),
                                        px=int(m.sum()))
                m = roi & ~np.isnan(c)
                bands['all'] = dict(p90=round(float(np.percentile(c[m], 90)), 3), p99=round(float(np.percentile(c[m], 99)), 3))
                if noise is not None:
                    bands['noise_A_pair_p90'] = round(float(np.percentile(np.abs(noise[m]), 90)), 3)
                row[f'coverage_a{k}'] = bands
            for v in VARIANTS:
                fb = [load(root, v, name + s) for s in ('', '-b')]
                if fb[0] is None: continue
                k = v[-2:]; c = np.nan_to_num(cov[k], nan=0)
                body = roi & (c >= 0.05)
                d = np.mean([luma(b) - luma(a) for b, a in zip(fb, fa) if b is not None and a is not None], 0)
                tone = {}
                for cname, cm in cl.items():
                    m = body & cm
                    if m.sum() < 40: continue
                    tone[cname] = dict(dY=round(float(d[m].mean()), 1), own=round(float((d[m] / np.maximum(c[m], 0.05)).mean()), 1),
                                       px=int(m.sum()))
                # throat tone on the lowest band that shows the plume
                mot = None
                if fb[1] is not None and fa[1] is not None and body.sum() > 40:
                    mot = dict(B=round(float(np.abs(luma(fb[1]) - luma(fb[0]))[body].mean()), 2),
                               A=round(float(np.abs(luma(fa[1]) - luma(fa[0]))[body].mean()), 2))
                row[v] = dict(tone=tone, motion=mot, body_px=int(body.sum()))
            out[name] = row
    return out


def sheets(root, outdir):
    os.makedirs(outdir, exist_ok=True)
    for view in VIEWS:
        hgt = path_mask(view); ys, xs = np.nonzero(~np.isnan(hgt))
        if not len(xs): continue
        box = (max(int(xs.min()) - 30, 0), max(int(ys.min()) - 30, 0), min(int(xs.max()) + 30, 1280), min(int(ys.max()) + 30, 720))
        cols = [A, 'r5-k29', 'r5-k52'] + VARIANTS; rows = []
        for t in ('', '-night'):
            crops = []
            for s in cols:
                im = load(root, s, view + t)
                crops.append(Image.fromarray(np.uint8(im)).crop(box) if im is not None else Image.new('RGB', (box[2] - box[0], box[3] - box[1])))
            sc = max(1, min(3, 300 // max(1, crops[0].width)))
            crops = [c.resize((c.width * sc, c.height * sc), Image.NEAREST) for c in crops]
            if t: crops = [Image.eval(c, lambda x: min(255, x * 3)) for c in crops]
            w, h = crops[0].size; r = Image.new('RGB', (len(crops) * (w + 6), h + 18), 'white'); dr = ImageDraw.Draw(r)
            for i, c in enumerate(crops):
                r.paste(c, (i * (w + 6), 18)); dr.text((i * (w + 6) + 4, 3), cols[i][3:] if cols[i].startswith('r5-') else 'A' + t, fill='black')
            rows.append(r)
        sheet = Image.new('RGB', (rows[0].width, sum(r.height for r in rows)), 'white'); y = 0
        for r in rows: sheet.paste(r, (0, y)); y += r.height
        sheet.save(f'{outdir}/sheet-{view}.jpg', quality=90)


if __name__ == '__main__':
    root = sys.argv[1]; res = measure(root)
    if '--json' in sys.argv: json.dump(res, open(sys.argv[sys.argv.index('--json') + 1], 'w'), indent=1, ensure_ascii=False)
    if '--sheets' in sys.argv: sheets(root, sys.argv[sys.argv.index('--sheets') + 1])
    print(json.dumps(res, indent=1, ensure_ascii=False)[:20000])
