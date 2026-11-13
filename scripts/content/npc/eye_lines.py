"""The eyes of a front picture as they are drawn: the lid lines, the iris, its colour.

  python eye_lines.py <picture> <points.json> <out.json> <mm per pixel> [--jpg check.jpg] [--box x0,y0,x1,y1]

<picture>: a front view (the concept, or a render of our head). <points.json>: face_points.py's points of it.
--box: the picture is this part of the file the points were found in.
Writes for each eye (r, l: the subject's side) the upper and the lower lid line from the inner corner to the outer
one, the iris' circle and median colour, the opening at the iris; --jpg: both eyes six times larger with the
lines (green), the detector's lid contour (red) and the iris (blue). auto_targets.py, concept_colours.py and
head_measures.py call read() themselves.

Why not the detector's contour alone. Its lid points come from a face model more than from the picture: it reads
nearly the same eye whatever is drawn. On the second face it read 6.7 and 6.0 mm of opening on the concept (drawn:
7.0), 9.0 on a clay render of the template (the mesh: 7.6) and 6.2 on a render of our head (the mesh: 5.3).
So the lines are read from the picture where the picture can be read.
- The iris: the detector's centre and radius, corrected by the edge between the iris and the white on its two sides.
- The upper lid. Over the white on both sides of the iris: the top of what is paler and less warm than the skin
  round the eye (both measured in this picture). Over the iris it cannot be read: the dark iris runs into the dark
  of the lashes. There the line is the curve through what was read on the two sides. Towards the corners it runs
  to the detector's corners.
- The lower lid: the detector's contour moved to where, under the iris, the picture gets lighter than the iris (the
  lid's rim), in full under the iris and less and less towards the corners.
What could not be read stays the detector's; upper_by and lower_by say which it was.
The opening is measured at the iris' centre, between the two lines.
"""
import json, sys
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

LIDS = {'r': ([133, 173, 157, 158, 159, 160, 161, 246, 33], [133, 155, 154, 153, 145, 144, 163, 7, 33], 468, range(469, 473)),
        'l': ([362, 398, 384, 385, 386, 387, 388, 466, 263], [362, 382, 381, 380, 374, 373, 390, 249, 263], 473, range(474, 478))}
LUM = np.array([0.2126, 0.7152, 0.0722])


def poly_mask(shape, pts):
    im = Image.new('1', (shape[1], shape[0]), 0); ImageDraw.Draw(im).polygon([tuple(float(v) for v in p) for p in pts], fill=1)
    return np.asarray(im)


def fit_iris(L, c0, r0):
    """(centre x, centre y, radius) of the iris: the strongest step from dark to light across a circle's two sides."""
    ang = np.radians(np.r_[np.arange(-30, 31, 4), np.arange(150, 211, 4)]); ca, sa = np.cos(ang), np.sin(ang); d = max(0.08*r0, 0.7)
    best = (-1e9, c0[0], c0[1], r0)
    for r in np.arange(0.72*r0, 1.3*r0, 0.02*r0):
        for dx in np.arange(-0.15*r0, 0.151*r0, 0.03*r0):
            for dy in np.arange(-0.15*r0, 0.151*r0, 0.05*r0):
                x, y = c0[0]+dx, c0[1]+dy
                out = ndimage.map_coordinates(L, [y+(r+d)*sa, x+(r+d)*ca], order=1); inn = ndimage.map_coordinates(L, [y+(r-d)*sa, x+(r-d)*ca], order=1)
                s = np.sort(out-inn)[len(ang)//4:].mean()-0.02*abs(dy)      # a quarter of the arc may lie under a lid
                if s > best[0]: best = (s, x, y, r)
    return best[1:]


def eye(img, pts, side, mm):
    """One eye of the picture: lines in pixels (inner corner first), the iris, how it was read."""
    up_i, lo_i, ci, ring = LIDS[side]; up, lo = pts[up_i], pts[lo_i]; c0 = pts[ci]; r0 = float(np.linalg.norm(pts[list(ring)]-c0, axis=1).mean())
    L = ndimage.gaussian_filter(img@LUM, 0.12/mm); warm = ndimage.gaussian_filter((img[..., 0]-img[..., 2])/(img.sum(-1)+1e-6), 0.12/mm)
    cx, cy, r = fit_iris(L, c0, r0); H, W = L.shape; yy, xx = np.mgrid[:H, :W]; rr = np.hypot(xx-cx, yy-cy)
    ring_m = (rr > 0.45*r) & (rr < 0.85*r) & (yy > cy) & (L < 1.6*np.median(L[(rr > 0.45*r) & (rr < 0.85*r) & (yy > cy)]))
    out = {'iris': {'centre': [float(cx), float(cy)], 'radius_px': float(r), 'diameter_mm': round(2*r*mm, 2),
                    'srgb': [int(v) for v in np.median(img[ring_m], 0).round()]}}
    inside = poly_mask(L.shape, np.r_[up, lo[::-1][1:-1]]); px = lambda v: max(int(round(v/mm)), 1)
    core = ndimage.binary_erosion(inside, iterations=px(0.4)) & (rr > 1.08*r)
    skin = ndimage.binary_dilation(inside, iterations=px(5)) & ~ndimage.binary_dilation(inside, iterations=px(2.5))
    x0, x1 = sorted((up[0][0], up[-1][0])); span = x1-x0; xs = np.linspace(x0, x1, 21)
    det = lambda line, x: np.interp(x, *(line[np.argsort(line[:, 0])].T))
    yu, yl = det(up, xs), det(lo, xs); out['upper_by'] = out['lower_by'] = 'detector'
    pale = None
    if core.sum() > 12 and skin.sum() > 50:
        white = core & (L >= np.median(L[core])); ws, wk = np.median(warm[white]), np.median(warm[skin])
        if wk-ws > 0.03: pale = (warm < ws+0.45*(wk-ws)) & (L > 0.35*np.median(L[white]))
    # the upper lid over the white: the top of what is pale and joined to the eye, on both sides of the iris; over the
    # iris, the curve through the two sides
    if pale is not None:
        E = ndimage.binary_opening(pale & ndimage.binary_dilation(inside, iterations=px(2.5)) & (rr > 0.92*r), iterations=px(0.25))
        lab, n = ndimage.label(E); E = np.isin(lab, np.unique(lab[core & E])) & (lab > 0)
        cols = np.arange(int(x0+0.08*span), int(x1-0.08*span)+1); has = E[:, cols].any(0) & (np.abs(cols-cx) > r); top = np.argmax(E[:, cols], 0)-0.5
        if min((has & (cols < cx)).sum(), (has & (cols > cx)).sum()) >= px(0.8):
            k = np.polyfit(cols[has], top[has], 2); a, b = cols[has].min(), cols[has].max(); ya, yb = np.polyval(k, [a, b])
            arc = np.maximum(np.polyval(k, xs), cy-0.98*r)                            # never over the iris' own top
            yu = np.where(xs < a, yu[0]+(ya-yu[0])*(xs-x0)/max(a-x0, 1e-6), np.where(xs > b, yu[-1]+(yb-yu[-1])*(x1-xs)/max(x1-b, 1e-6), arc))
            out['upper_by'] = 'white'
    # the lower lid under the iris: where the column first gets lighter than the iris by a third of the way to the lid
    # under it. What is pale under that is the lid's wet rim, not the white, unless the whole iris is in view.
    # The detector's contour is moved there, in full under the iris and less and less towards the corners.
    Li = np.median(L[(rr < 0.8*r) & (rr > 0.3*r) & (yy > cy)])
    under = (np.abs(xx-cx) < 0.6*r) & (yy > cy+1.2*r) & (yy < cy+1.7*r); Ld = np.median(L[under]) if under.any() else Li
    ys = np.arange(int(cy+0.25*r), min(int(cy+1.3*r)+1, H-1)); cols = np.arange(int(cx-0.8*r), int(cx+0.8*r)+1)
    if Ld-Li > 15 and len(ys) and cols.min() >= 0 and cols.max() < W:
        bot = []
        for c in cols:
            lit = np.nonzero(L[ys, c] > Li+0.35*(Ld-Li))[0]; y = (ys[lit[0]] if len(lit) else ys[-1])-0.5
            if pale is not None and y-cy > 0.93*np.sqrt(max(r*r-(c-cx)**2, 0)):      # the iris' own edge: the white shows under it
                pal = np.nonzero(pale[:, c] & (yy[:, c] > y) & (yy[:, c] < cy+1.6*r))[0]
                if len(pal): y = pal[np.argmax(np.diff(np.r_[pal, 10**6]) > 1)]+0.5  # the end of the first pale run
            bot.append(y)
        shift = float(np.median(np.array(bot)-det(lo, cols))); a, b = cols.min(), cols.max()
        yl = yl+shift*np.where(xs < a, (xs-x0)/max(a-x0, 1e-6), np.where(xs > b, (x1-xs)/max(x1-b, 1e-6), 1.0))
        out['lower_by'] = 'iris'; out['lower_shift_mm'] = round(shift*mm, 2)
    o = slice(None, None, -1) if up[0][0] > up[-1][0] else slice(None)
    up, lo = np.c_[xs, yu][o], np.c_[xs, np.maximum(yl, yu)][o]
    at = lambda line, x: float(np.interp(x, *(line[np.argsort(line[:, 0])].T)))
    out.update({'upper': up.round(2).tolist(), 'lower': lo.round(2).tolist(),
                'opening_mm': round((at(lo, cx)-at(up, cx))*mm, 2), 'width_mm': round(abs(up[-1][0]-up[0][0])*mm, 2),
                'upper_over_iris_centre_mm': round((cy-at(up, cx))*mm, 2), 'lower_under_iris_centre_mm': round((at(lo, cx)-cy)*mm, 2)})
    return out


def read(path, points, mm, box=None):
    img = np.asarray(Image.open(path).convert('RGB')).astype(np.float64); pts = np.array(json.load(open(points))['points'])[:, :2]
    if box: img = img[box[1]:box[3], box[0]:box[2]]; pts = pts-box[:2]
    return img, pts, {s: eye(img, pts, s, mm) for s in ('r', 'l')}


def sheet(img, pts, eyes, mm, path, k=6):
    tiles = []
    for s in ('r', 'l'):
        e = eyes[s]; cx, cy = e['iris']['centre']; w, h = int(20/mm), int(11/mm); x0, y0 = int(cx-w), int(cy-h)
        t = Image.fromarray(img[max(y0, 0):y0+2*h, max(x0, 0):x0+2*w].astype(np.uint8)).resize((2*w*k, 2*h*k), Image.BICUBIC); d = ImageDraw.Draw(t)
        f = lambda p: [((x-x0+0.5)*k, (y-y0+0.5)*k) for x, y in p]
        d.line(f(pts[LIDS[s][0]]), fill=(255, 60, 60), width=1); d.line(f(pts[LIDS[s][1]]), fill=(255, 60, 60), width=1)
        d.line(f(e['upper']), fill=(60, 255, 90), width=2); d.line(f(e['lower']), fill=(60, 255, 90), width=2)
        r = e['iris']['radius_px']; (a, b), = f([(cx, cy)]); d.ellipse([a-r*k, b-r*k, a+r*k, b+r*k], outline=(80, 160, 255))
        tiles.append(t)
    sh = Image.new('RGB', (tiles[0].width+tiles[1].width+10, max(t.height for t in tiles)), (30, 30, 30))
    sh.paste(tiles[0], (0, 0)); sh.paste(tiles[1], (tiles[0].width+10, 0)); sh.save(path, quality=90)


if __name__ == '__main__':
    a = sys.argv[1:]
    def opt(name):
        if name not in a: return None
        i = a.index(name); v = a[i+1]; del a[i:i+2]; return v
    jpg = opt('--jpg'); box = opt('--box'); box = [int(v) for v in box.split(',')] if box else None
    mm = float(a[3]); img, pts, eyes = read(a[0], a[1], mm, box)
    json.dump({'mm_per_px': mm, 'box': box, 'eyes': eyes}, open(a[2], 'w'), indent=1)
    if jpg: sheet(img, pts, eyes, mm, jpg)
    for s in ('r', 'l'):
        e = eyes[s]
        print('EYE %s (upper lid by the %s, lower by the %s): opening %.1f mm (upper %+.1f, lower %+.1f of the iris centre), width %.1f, iris %.1f mm, colour %s'
              % (s, e['upper_by'], e['lower_by'], e['opening_mm'], e['upper_over_iris_centre_mm'], -e['lower_under_iris_centre_mm'], e['width_mm'], e['iris']['diameter_mm'], e['iris']['srgb']))
