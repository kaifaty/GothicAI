# own_pairs.py <game run> <reference run> <out dir> [view ...] : the lamps' first search with the point's own texels
# against the search before it (../README.md, "Правка в игре"). Both runs are lamp-own.bench: <view>-old, -new, -old2 of
# one camera; the reference run is the same under OPENGOTHIC_VSM_LAMP=truth (32 search and 64 filter taps, no early
# exits). The world moves between the shots of a view (flames, smoke, the hero's breath), but two runs repeat each
# other shot for shot, so a shot is compared with the reference's shot of the same moment. Particles (own fire and
# smoke) do not repeat: there "before again" tells how much of "before" is them.
# Prints per view, luminance of the frame, levels 0..255: the pixels farther than 8 from the reference before (old),
# after (new) and before again (old2), and how much lighter than the reference the old search's wrong pixels are.
# Writes 3-own-tower.jpg (crops of the nape: old | new | reference) when no view is named, else 4-own-views.jpg (a
# named view: the frame, where the old search is off the reference (blue lighter, red darker), and the place of the
# largest such spot: old | new | reference).
import sys, os
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy import ndimage

GAME, REF, OUT = sys.argv[1:4]; NAMED = sys.argv[4:]
FONT = ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc', 24)
VIEWS = ['tower-nape', 'tower-ear'] + ['interior-%d' % i for i in range(1, 5)] + ['harbour-brazier'] + ['harbour-night-%d' % i for i in range(1, 5)]
rgb = lambda d, n: Image.open(os.path.join(d, n+'.png')).convert('RGB')
lum = lambda im: np.asarray(im.convert('L')).astype(int)
far = lambda a, b: 100*(np.abs(a-b) > 8).mean()


def label(t, text):
    d = ImageDraw.Draw(t); d.rectangle((0, 0, 14+d.textlength(text, font=FONT), 36), fill=(0, 0, 0)); d.text((7, 5), text, fill=(255, 255, 255), font=FONT)
    return t


def sheet(name, rows):
    w, h = rows[0][0].size; s = Image.new('RGB', (len(rows[0])*(w+6)-6, len(rows)*(h+6)-6), (20, 20, 20))
    for j, row in enumerate(rows):
        for i, t in enumerate(row): s.paste(t, (i*(w+6), j*(h+6)))
    s.save(os.path.join(OUT, name), quality=88); print(name, s.size)


print('%-16s | far from the reference, %% of pixels: before | after | before again || the old search\'s wrong pixels over the reference, levels' % 'view')
rows = []
for v in VIEWS:
    o, n, o2 = [rgb(GAME, v+s) for s in ('-old', '-new', '-old2')]; to, tn, to2 = [rgb(REF, v+s) for s in ('-old', '-new', '-old2')]
    d = lum(o)-lum(to); wrong = np.abs(d) > 8
    print('%-16s | %.3f | %.3f | %.3f || %+.1f' % (v, far(lum(o), lum(to)), far(lum(n), lum(tn)), far(lum(o2), lum(to2)), d[wrong].mean() if wrong.any() else 0))
    if v not in NAMED or not wrong.any():
        continue
    W, H = o.size; small = lambda im: im.resize((W//4, H//4), Image.LANCZOS)
    g = lum(to)//3; m = Image.fromarray(np.clip(np.stack([g+np.clip(-d*8, 0, 255), g, g+np.clip(d*8, 0, 255)], -1), 0, 255).astype(np.uint8))
    lab, k = ndimage.label(ndimage.binary_dilation(wrong, iterations=10)); size = ndimage.sum(wrong, lab, range(1, k+1)); c = int(np.argmax(size))+1
    cy, cx = ndimage.center_of_mass(wrong & (lab == c)); cw, chh = W//8, H//8
    x0 = int(min(max(cx-cw/2, 0), W-cw)); y0 = int(min(max(cy-chh/2, 0), H-chh)); box = (x0, y0, x0+cw, y0+chh)
    f = small(n); ImageDraw.Draw(f).rectangle((x0//4, y0//4, (x0+cw)//4, (y0+chh)//4), outline=(255, 255, 0), width=2)
    crop = lambda im: im.crop(box).resize((W//4, H//4), Image.NEAREST)
    rows.append([label(f, v), label(small(m), 'прежний поиск против эталона'), label(crop(o), 'до'), label(crop(n), 'после'), label(crop(tn), 'эталон')])

if NAMED:
    sheet('4-own-views.jpg', rows); sys.exit()
W, H = rgb(GAME, 'tower-nape-new').size; k = W/3840   # the crops of stipple.py, given for 3840x2160
crop = lambda im, box: im.crop(tuple(int(round(c*k)) for c in box)).resize((600, 600), Image.NEAREST)
NAPE, BACK = (1900, 940, 2100, 1140), (1650, 900, 1950, 1200)
sheet('3-own-tower.jpg', [[label(crop(rgb(GAME, v+'-old'), b), d+': до'), label(crop(rgb(GAME, v+'-new'), b), 'после'),
                           label(crop(rgb(REF, v+'-new'), b), 'эталон')] for v, b, d in [('tower-nape', NAPE, '120 см'), ('tower-ear', BACK, '60 см')]])
