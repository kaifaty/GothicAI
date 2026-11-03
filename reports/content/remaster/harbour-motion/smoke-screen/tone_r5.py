"""usage: tone_r5.py CAPTURES_DIR OUT.json
r5 tone table: dY of each variant to side A in the plume body. The body is where the black probe of the same
alpha covers >= 0.05 at midnight (Gaussian 6 px): at noon the fog pass hides most of the probe (see the README)."""
import sys, json, numpy as np, measure_r5 as m
from PIL import Image
from scipy import ndimage
C = sys.argv[1]
def L(s, n): return np.asarray(Image.open(f'{C}/{s}/{n}.png').convert('RGB'), float)
def cov(k, n):
    cs = [1 - m.lin(L(k, n + s)) / np.maximum(m.lin(L('harbour-life-8', n + s)), 1e-5) for s in ('', '-b')]
    return ndimage.gaussian_filter(np.mean(cs, 0), 6)
out = {}
for v in m.VIEWS:
    roi = ~np.isnan(m.path_mask(v)); cl = m.classes(L('harbour-life-8', v))
    for k in ('29', '52'):
        body = roi & (cov(f'r5-k{k}', v + '-night') >= 0.05)
        for t in ('', '-night'):
            a = [L('harbour-life-8', v + t + s) for s in ('', '-b')]
            for g in (75, 95, 115):
                s_ = f'r5-g{g}a{k}'
                d = np.mean([L(s_, v + t + s) @ m.W - aa @ m.W for s, aa in zip(('', '-b'), a)], 0)
                mot = np.abs(L(s_, v + t + '-b') @ m.W - L(s_, v + t) @ m.W)[body].mean()
                mota = np.abs(a[1] @ m.W - a[0] @ m.W)[body].mean()
                row = {c: round(float(d[body & mk].mean()), 1) for c, mk in cl.items() if (body & mk).sum() >= 200}
                row['motion'] = [round(float(mot), 2), round(float(mota), 2)]
                out.setdefault(s_, {})[v + t] = row
json.dump(out, open(sys.argv[2], 'w'), indent=1)
for s_, r in out.items():
    print(s_, ' | '.join(f"{v}: " + ' '.join(f"{c} {x:+.1f}" for c, x in row.items() if c != 'motion') + f" mot {row['motion'][0]}/{row['motion'][1]}" for v, row in r.items()))
