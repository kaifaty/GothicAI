"""usage: tone_r7.py OUT.json
r7 (denser r6) against side A in captures/clip-1, all on one build: dY in the plume body (body from the r5-fog black
probe k52 at midnight), its motion over 0.7 s, the share of the body that changed by more than 3 levels, and the
coverage scaled from r6 (0.13 by day at the same tone: dY of a fixed grey grows with the coverage)."""
import sys, json, numpy as np
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import measure_r5 as m
from PIL import Image
from scipy import ndimage
R = str(Path.home() / '.local/share/gothic-ai/remaster/harbour-motion/captures')
def L(c, s, n): return np.asarray(Image.open(f'{R}/{c}/{s}/{n}.png').convert('RGB'), float)
SETS = ('harbour-motion-4', 'r7-d2', 'r7-d3', 'r7-d4')
out = {}
for v in ('huts', 'huts-north'):
    roi = ~np.isnan(m.path_mask(v)); cl = m.classes(L('r5-fog', 'harbour-life-8', v))
    cs = [1 - m.lin(L('r5-fog', 'r5-k52', v + '-night' + s)) / np.maximum(m.lin(L('r5-fog', 'harbour-life-8', v + '-night' + s)), 1e-5) for s in ('', '-b')]
    body = roi & (ndimage.gaussian_filter(np.mean(cs, 0), 6) >= 0.05)
    for t in ('', '-night'):
        for s_ in SETS:
            d = np.mean([L('clip-1', s_, v + t + s) @ m.W - L('clip-1', 'harbour-life-8', v + t + s) @ m.W for s in ('', '-b')], 0)
            mot = np.abs(L('clip-1', s_, v + t + '-b') @ m.W - L('clip-1', s_, v + t) @ m.W)[body].mean()
            row = {k: round(float(d[body & mk].mean()), 1) for k, mk in cl.items() if (body & mk).sum() >= 200}
            row['seen>3'] = round(float((np.abs(d[body]) > 3).mean()), 3)
            row['motion'] = round(float(mot), 2)
            out[f'{v}{t} {s_}'] = row
ref = {v: out[f'{v} harbour-motion-4']['sky'] for v in ('huts', 'huts-north')}
for k, r in out.items():
    v = k.split()[0]
    if v in ref:
        r['coverage~'] = round(0.13 * r['sky'] / ref[v], 2)
    print(k, r)
json.dump(out, open(sys.argv[1], 'w'), indent=1)
