"""Smoke contrast of screening variants against side A (no smoke) on the same build.
usage: measure.py ROUND_DIR A_SET SET... — prints a table and writes ROUND_DIR/smoke-contrast.json, ROUND_DIR/sheet-<view>.jpg"""
import sys, json
import numpy as np
from PIL import Image, ImageDraw
root = sys.argv[1]; A = sys.argv[2]; sets = sys.argv[3:]
REG = {'huts': {'plume': (560, 180, 720, 370)},
       'huts-north': {'plume': (540, 120, 720, 330)},
       'overhead': {'h06': (650, 230, 740, 335), 'h04': (850, 230, 940, 335)}}
W = np.array([0.2126, 0.7152, 0.0722])
def load(p): return np.asarray(Image.open(p).convert('RGB'), float)
def classes(a):
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    sky = (b - r > 20) & (b > g)
    tree = ~sky & (g > r + 3) & (a @ W < 90)
    return {'sky': sky, 'tree': tree, 'roof': ~sky & ~tree}
res = {}
for view, regs in REG.items():
    for t in ('', '-night'):
        for name, (x0, y0, x1, y1) in regs.items():
            key = f'{view}{t}:{name}'
            row = {}
            for s in sets:
                vals = []
                for suf in ('', '-b'):
                    a = load(f'{root}/{A}/{view}{t}{suf}.png')[y0:y1, x0:x1]
                    b = load(f'{root}/{s}/{view}{t}{suf}.png')[y0:y1, x0:x1]
                    d = b @ W - a @ W
                    cl = classes(a)
                    v = {'mean_abs': float(np.abs(d).mean()), 'vis8': float((np.abs(d) > 8).mean()),
                         'p98': float(np.percentile(np.abs(d), 98)), 'max+': float(d.max()), 'max-': float(d.min())}
                    for c, m in cl.items():
                        if m.sum() > 50:
                            v[c] = float(d[m].mean()); v[c + '_vis'] = float((np.abs(d[m]) > 8).mean())
                    sm = np.abs(d) > 8
                    v['rgb'] = [round(float(x)) for x in b[sm].mean(0)] if sm.sum() > 20 else None
                    vals.append(v)
                row[s] = {k: (round(float(np.mean([x[k] for x in vals if k in x])), 3) if all(isinstance(x.get(k), float) for x in vals if k in x) and any(k in x for x in vals) else vals[0].get(k)) for k in vals[0]}
            res[key] = row
            print(f'\n{key}')
            for s, v in row.items():
                print(f"  {s:18} |dY| {v['mean_abs']:5.2f}  vis>8 {v['vis8']*100:5.1f}%  p98 {v['p98']:5.1f}  max {v['max+']:+5.0f}/{v['max-']:+5.0f}  "
                      + '  '.join(f"{c} {v[c]:+5.1f} ({v[c+'_vis']*100:4.1f}%)" for c in ('sky', 'tree', 'roof') if c in v) + f"  rgb {v['rgb']}")
json.dump(res, open(f'{root}/smoke-contrast.json', 'w'), indent=1)
# sheets: per view+time a row of crops A, sets...
for view, regs in REG.items():
    xs = [r[0] for r in regs.values()] + [r[2] for r in regs.values()]; ys = [r[1] for r in regs.values()] + [r[3] for r in regs.values()]
    box = (max(min(xs) - 40, 0), max(min(ys) - 40, 0), min(max(xs) + 40, 1280), min(max(ys) + 40, 720))
    cols = [A] + sets; rows = []
    for t in ('', '-night'):
        crops = [Image.open(f'{root}/{s}/{view}{t}.png').convert('RGB').crop(box) for s in cols]
        sc = 2 if crops[0].width < 300 else 1
        crops = [c.resize((c.width * sc, c.height * sc), Image.NEAREST) for c in crops]
        if t:  # lift night for my eyes only, same gain on every column
            crops = [Image.eval(c, lambda x: min(255, x * 3)) for c in crops]
        w, h = crops[0].size; r = Image.new('RGB', (len(crops) * (w + 6), h + 18), 'white'); dr = ImageDraw.Draw(r)
        for i, c in enumerate(crops):
            r.paste(c, (i * (w + 6), 18)); dr.text((i * (w + 6) + 4, 3), cols[i] + t, fill='black')
        rows.append(r)
    out = Image.new('RGB', (rows[0].width, sum(r.height for r in rows)), 'white'); y = 0
    for r in rows: out.paste(r, (0, y)); y += r.height
    out.save(f'{root}/sheet-{view}.jpg', quality=90)
