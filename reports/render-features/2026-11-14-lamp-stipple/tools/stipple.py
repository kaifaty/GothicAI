# stipple.py <runs dir> <out dir> : the net of dots of a lamp's shadow on the hero's nape (../README.md).
# <runs dir> holds the frames of lamp-stipple.bench, one folder per variant: clean-<variant>/{p120,b60,p120_class}.png
# (base, still, own, truth, hard, nohair, notaa, lamptaa, novsm, sides1), 3840x2160.
# Prints, per variant, on the skin of the nape in p120: the grain (the mean of |pixel - mean of its 3x3| over the
# mean) and the mean, luminance, linear; and how much of the two frames differs between still, own and truth.
# Writes the sheets 1-cause.jpg and 2-own.jpg into <out dir>.
import sys, os
import numpy as np
from PIL import Image, ImageDraw, ImageFont

RUNS, OUT = sys.argv[1:3]
SKIN = (1966, 1066, 2022, 1116)   # the nape's skin under the tail, p120
NAPE = (1900, 940, 2100, 1140)    # the ear and the nape, p120: a tile at 3x
BACK = (1650, 900, 1950, 1200)    # behind the ear, b60: a tile at 2x
FONT = ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc', 30)
lin = lambda c: np.where(c > 0.04045, ((c+0.055)/1.055)**2.4, c/12.92)
frame = lambda v, n: Image.open(os.path.join(RUNS, 'clean-'+v, n+'.png')).convert('RGB')


def grain(im, box):
    a = lin(np.asarray(im.crop(box)).astype(float)/255) @ [0.2126, 0.7152, 0.0722]
    p = np.pad(a, 1, mode='edge'); m = sum(p[i:i+a.shape[0], j:j+a.shape[1]] for i in range(3) for j in range(3))/9
    return np.abs(a-m).mean()/max(a.mean(), 1e-6), a.mean()


def tile(im, box, k, label):
    t = im.crop(box).resize(((box[2]-box[0])*k, (box[3]-box[1])*k), Image.NEAREST); d = ImageDraw.Draw(t)
    d.rectangle((0, 0, 18+d.textlength(label, font=FONT), 46), fill=(0, 0, 0)); d.text((9, 7), label, fill=(255, 255, 255), font=FONT)
    return t


def sheet(name, rows):
    w, h = rows[0][0].size; s = Image.new('RGB', (len(rows[0])*(w+6)-6, len(rows)*(h+6)-6), (20, 20, 20))
    for j, row in enumerate(rows):
        for i, t in enumerate(row): s.paste(t, (i*(w+6), j*(h+6)))
    s.save(os.path.join(OUT, name), quality=90); print(name, s.size)


print('%-8s | grain | mean' % 'variant')
for v in ['base', 'still', 'notaa', 'lamptaa', 'sides1', 'hard', 'truth', 'own', 'nohair', 'novsm']:
    print('%-8s | %.3f | %.4f' % ((v,)+grain(frame(v, 'p120'), SKIN)))
for n in ['p120', 'b60']:
    g = {v: np.asarray(frame(v, n).convert('L')).astype(int) for v in ['base', 'still', 'own', 'truth']}
    for a, b in [('base', 'still'), ('still', 'own'), ('own', 'truth'), ('still', 'truth')]:
        d = np.abs(g[a]-g[b]); print('%s %-5s -> %-5s: pixels that differ by more than 8 levels %.4f%%, by more than 24 %.4f%%' % (n, a, b, 100*(d > 8).mean(), 100*(d > 24).mean()))

sheet('1-cause.jpg', [[tile(frame('base', 'p120'), NAPE, 3, 'Игра'), tile(frame('base', 'p120_class'), NAPE, 3, 'Ответ тени ламп'),
                       tile(frame('novsm', 'p120'), NAPE, 3, 'Без теней ламп')],
                      [tile(frame('nohair', 'p120'), NAPE, 3, 'Голова без волос'), tile(frame('hard', 'p120'), NAPE, 3, 'Жёсткая тень: 1 выборка'),
                       tile(frame('truth', 'p120'), NAPE, 3, 'Эталон: 32 + 64 выборки')]])
sheet('2-own.jpg', [[tile(frame('base', 'p120'), NAPE, 3, 'Игра, 120 см'), tile(frame('own', 'p120'), NAPE, 3, 'Опыт: своя точка в поиске'),
                     tile(frame('truth', 'p120'), NAPE, 3, 'Эталон')],
                    [tile(frame('base', 'b60'), BACK, 2, 'Игра, 60 см'), tile(frame('own', 'b60'), BACK, 2, 'Опыт: своя точка в поиске'),
                     tile(frame('truth', 'b60'), BACK, 2, 'Эталон')]])
