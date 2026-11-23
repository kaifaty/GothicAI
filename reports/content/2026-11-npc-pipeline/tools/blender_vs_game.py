# blender_vs_game.py <points dir> <reference> <name>=<image> ... : how a head in the game's frames differs from its
# Cycles tile of the likeness sheet (../likeness.md, "Blender и игра"). <points dir> holds the <name>.json of
# scripts/content/npc/face_points.py for every name; <reference> is the name of the Cycles tile.
# Prints, per picture:
# - the face points against the reference's after a similarity fit (shift, turn, scale), in mm at 61.5 mm between
#   the irises: the median and by groups; and the width of the face over its height (points 234-454 over 10-152): a
#   short lens narrows it;
# - luminance (linear) against the cheeks': the eye socket between the upper lid and the brow, the beard on the chin,
#   the moustache; and the cheeks' own green and blue over red. Patches of a twentieth of the distance between the
#   irises, medians.
import sys, os, json
import numpy as np
from PIL import Image

DIR, REF = sys.argv[1:3]; PICS = [s.split('=', 1) for s in sys.argv[3:]]
G = {'eyes': [33, 133, 263, 362, 159, 145, 386, 374], 'brows': [70, 63, 105, 66, 107, 300, 293, 334, 296, 336],
     'nose': [1, 2, 4, 5, 6, 168, 98, 327, 129, 358],
     'lips': [61, 185, 40, 39, 37, 0, 267, 269, 270, 409, 291, 146, 91, 181, 84, 17, 314, 405, 321, 375],
     'jaw': [234, 93, 132, 58, 172, 136, 150, 149, 176, 148, 152, 377, 400, 378, 379, 365, 397, 288, 361, 323, 454]}
lin = lambda c: np.where(c > 0.04045, ((c+0.055)/1.055)**2.4, c/12.92)
LUM = np.array([0.2126, 0.7152, 0.0722])


def points(name):
    d = json.load(open(os.path.join(DIR, name+'.json')))
    return np.array(d['points'] if isinstance(d, dict) else d)[:, :2].astype(float)


def fit(a, b):
    """b laid on a by a shift, a turn and a scale."""
    ca, cb = a.mean(0), b.mean(0); A, B = a-ca, b-cb; U, s, Vt = np.linalg.svd(B.T @ A); R = U @ Vt
    if np.linalg.det(R) < 0:
        U[:, -1] *= -1; R = U @ Vt
    return (B @ R)*(s.sum()/(B**2).sum())+ca


ipd = lambda p: np.linalg.norm(p[468:473].mean(0)-p[473:478].mean(0))
ref = points(REF); mm = 61.5/ipd(ref)
print('%-14s | median mm | ' % 'points' + ' | '.join('%-5s' % k for k in G) + ' | width/height || socket | chin beard | moustache  (over the cheeks) || cheeks g/r b/r')
for name, path in PICS:
    p = points(name); d = np.linalg.norm(fit(ref[:468], p[:468])-ref[:468], axis=1)*mm
    im = Image.open(path).convert('RGBA'); bg = Image.new('RGBA', im.size, (128, 128, 128, 255)); bg.alpha_composite(im)
    a = np.asarray(bg.convert('RGB')).astype(float); r = max(2, int(ipd(p)*0.05))

    def col(c):
        x, y = int(round(c[0])), int(round(c[1])); return lin(np.median(a[y-r:y+r+1, x-r:x+r+1].reshape(-1, 3), 0)/255)
    lum = lambda c: col(c) @ LUM
    cheek = (lum(p[50])+lum(p[280]))/2; cc = (col(p[50])+col(p[280]))/2
    socket = (lum((p[159]+p[105])/2)+lum((p[386]+p[334])/2))/2
    moustache = (lum((p[37]+p[97])/2)+lum((p[267]+p[326])/2))/2
    print('%-14s | %8.1f  | ' % (name, np.median(d)) + ' | '.join('%5.1f' % np.median(d[G[k]]) for k in G)
          + ' | %.3f        || %.2f   | %.2f       | %.2f                         || %.2f %.2f' % (
              np.linalg.norm(p[234]-p[454])/np.linalg.norm(p[10]-p[152]), socket/cheek, lum(p[175])/cheek, moustache/cheek, cc[1]/cc[0], cc[2]/cc[0]))
