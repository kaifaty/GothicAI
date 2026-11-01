"""usage: tone_r6.py OUT.json
r6 shots (captures/clip-1, harbour-motion-4 on the E4 build) against r5-fog g75a52: dY to side A in the plume body
(body from the r5-fog black probe k52 at midnight). Same layer data, the sprite now from content/materials."""
import sys, json, numpy as np
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import measure_r5 as m
from PIL import Image
from scipy import ndimage
R = str(Path.home() / '.local/share/gothic-ai/remaster/harbour-motion/captures')
def L(c, s, n): return np.asarray(Image.open(f'{R}/{c}/{s}/{n}.png').convert('RGB'), float)
out = {}
for v in ('huts', 'huts-north'):
    roi = ~np.isnan(m.path_mask(v)); cl = m.classes(L('r5-fog', 'harbour-life-8', v))
    cs = [1 - m.lin(L('r5-fog', 'r5-k52', v + '-night' + s)) / np.maximum(m.lin(L('r5-fog', 'harbour-life-8', v + '-night' + s)), 1e-5) for s in ('', '-b')]
    body = roi & (ndimage.gaussian_filter(np.mean(cs, 0), 6) >= 0.05)
    for t in ('', '-night'):
        for name, (c, a, b) in {'r5-fog g75a52': ('r5-fog', 'harbour-life-8', 'r5-g75a52'), 'r6 clip-1': ('clip-1', 'harbour-life-8', 'harbour-motion-4')}.items():
            d = np.mean([L(c, b, v + t + s) @ m.W - L(c, a, v + t + s) @ m.W for s in ('', '-b')], 0)
            mot = np.abs(L(c, b, v + t + '-b') @ m.W - L(c, b, v + t) @ m.W)[body].mean()
            mota = np.abs(L(c, a, v + t + '-b') @ m.W - L(c, a, v + t) @ m.W)[body].mean()
            row = {k: round(float(d[body & mk].mean()), 1) for k, mk in cl.items() if (body & mk).sum() >= 200}
            row['motion'] = [round(float(mot), 2), round(float(mota), 2)]
            out[f'{v}{t} {name}'] = row
for k, r in out.items(): print(k, r)
json.dump(out, open(sys.argv[1], 'w'), indent=1)
