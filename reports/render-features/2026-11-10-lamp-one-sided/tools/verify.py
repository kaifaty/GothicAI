#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["pillow>=11,<13", "numpy>=1.26"]
# ///
"""Census views against the path tracer: where two-sided faces darken a block, is the tracer dark there too?

verify.py RUN OUT [SIDES...]
RUN holds <view>-1.png (vsm sides 1, before), <view>-<s>.png for each other SIDES (default 2 3) and <view>-pt.png (the
path tracer; for this question with two-sided shadow rays, see the report). Blocks of 16x16. k: the median pt/a of blocks lit in a and unchanged in b. A block a
variant darkens (by 6/255 and 25 %) is 'truth dark' when pt < 0.5*k*a (the leak is real: the variant is right),
'truth lit' when pt > 0.8*k*a (a false shadow). Writes summary.json and sheets a | r | b | pt.
"""
import json, sys, pathlib
import numpy as np
from PIL import Image, ImageDraw

run, out = map(pathlib.Path, sys.argv[1:3])
out.mkdir(parents=True, exist_ok=True)
W = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def load(p):
    im = np.asarray(Image.open(p).convert('RGB'), dtype=np.float32)
    return im, im @ W


def blocks(l, k=16):
    h, w = (l.shape[0] // k) * k, (l.shape[1] // k) * k
    return l[:h, :w].reshape(h // k, k, w // k, k).mean(axis=(1, 3))


rows = []
variants = [a for a in sys.argv[3:]] or ['2', '3']
for pa in sorted(run.glob('*-1.png')):
    v = pa.name[:-6]
    paths = {t: run / f'{v}-{t}.png' for t in ['1', *variants, 'pt']}
    if not all(p.exists() for p in paths.values()):
        continue
    ims = {t: load(p) for t, p in paths.items()}
    B = {t: blocks(ims[t][1]) for t in ims}
    a, pt = B['1'], B['pt']
    lit = (a > 20) & np.all([np.abs(a - B[t]) < 3 for t in variants], axis=0)
    k = float(np.median(pt[lit] / a[lit])) if lit.sum() > 20 else float('nan')
    row = dict(view=v, k=k)
    for t in variants:
        d = a - B[t]
        dark = (d > 6) & (d > 0.25 * a)
        tdark = dark & (pt < 0.5 * k * a)
        tlit = dark & (pt > 0.8 * k * a)
        row[t] = dict(blocks=int(dark.sum()), truth_dark=int(tdark.sum()), truth_lit=int(tlit.sum()))
    rows.append(row)
    sheet = np.concatenate([ims[t][0] for t in ['1', *variants, 'pt']], axis=1).astype(np.uint8)
    img = Image.fromarray(sheet).resize((sheet.shape[1] // 2, sheet.shape[0] // 2))
    dr = ImageDraw.Draw(img)
    for i, t in enumerate(['sides 1', *[f'sides {t}' for t in variants], 'path tracer']):
        dr.text((i * 480 + 4, 2), f'{v} {t}', fill='white')
    img.save(out / f'{v}.jpg', quality=88)

json.dump(rows, open(out / 'summary.json', 'w'), indent=1)
for t in variants:
    n = sum(r[t]['blocks'] for r in rows)
    td = sum(r[t]['truth_dark'] for r in rows)
    tl = sum(r[t]['truth_lit'] for r in rows)
    print(f'{t}: darkened blocks {n}, tracer dark there {td} ({td / max(n, 1) * 100:.0f}%), tracer lit {tl} '
          f'({tl / max(n, 1) * 100:.0f}%)')
rows.sort(key=lambda r: -max(r[t]['blocks'] for t in variants))
for r in rows[:50]:
    print(f"{r['view']:12s} k {r['k']:.2f}" + ''.join(
        f"   sides {t}: {r[t]['blocks']:4d} dark {r[t]['truth_dark']:4d} lit {r[t]['truth_lit']:4d}" for t in variants))
