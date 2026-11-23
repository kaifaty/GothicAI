#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["pillow>=11,<13", "numpy>=1.26"]
# ///
"""Census views against the path tracer: where a variant darkens a block against a base, is the tracer dark there too?

verify.py RUN OUT BASE VARIANT...
RUN holds <view>-<BASE>.png, <view>-<VARIANT>.png for each variant (`vsm sides` of the shot) and <view>-pt.png.
The metric is that of reports/render-features/2026-11-10-lamp-one-sided/tools/verify.py with a base of one's
choice: blocks of 16x16; a block is darker when it loses 6/255 and 25 %; k is the median pt/base of blocks lit in the
base and unchanged in every variant; 'reference dark' pt < 0.5*k*base (the leak was real), 'reference lit'
pt > 0.8*k*base (a false shadow). Brightened blocks are counted the same way, with 'reference dark' among them.
Writes OUT/summary.json and prints the totals and the views with the most 'reference lit' blocks.

The reference of 2026-11-11 is the path tracer with direct light alone (one bounce) and shadow rays that stop at both
sides of a triangle, alpha-tested ones too: a full path tracer lights the places a lamp does not reach by reflected
light (reports/render-features/2026-11-11-lamp-objects-two-sided).
"""
import json, sys, pathlib
import numpy as np
from PIL import Image

run, out = map(pathlib.Path, sys.argv[1:3])
base, variants = sys.argv[3], sys.argv[4:]
out.mkdir(parents=True, exist_ok=True)
W = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def blocks(p, k=16):
    l = np.asarray(Image.open(p).convert('RGB'), dtype=np.float32) @ W
    h, w = (l.shape[0] // k) * k, (l.shape[1] // k) * k
    return l[:h, :w].reshape(h // k, k, w // k, k).mean(axis=(1, 3))


rows = []
for pb in sorted(run.glob(f'*-{base}.png')):
    v = pb.name[:-len(base) - 5]
    paths = {t: run / f'{v}-{t}.png' for t in [base, *variants, 'pt']}
    if not all(p.exists() for p in paths.values()):
        continue
    B = {t: blocks(p) for t, p in paths.items()}
    a, pt = B[base], B['pt']
    lit = (a > 20) & np.all([np.abs(a - B[t]) < 3 for t in variants], axis=0)
    k = float(np.median(pt[lit] / a[lit])) if lit.sum() > 20 else float('nan')
    row = dict(view=v, k=k)
    for t in variants:
        d = a - B[t]
        dark = (d > 6) & (d > 0.25 * a)
        br = (-d > 6) & (-d > 0.25 * a)
        row[t] = dict(blocks=int(dark.sum()), truth_dark=int((dark & (pt < 0.5 * k * a)).sum()),
                      truth_lit=int((dark & (pt > 0.8 * k * a)).sum()), brighter=int(br.sum()),
                      brighter_truth_dark=int((br & (pt < 0.5 * k * B[t])).sum()))
    rows.append(row)

json.dump(rows, open(out / 'summary.json', 'w'), indent=1)
print(f'base {base}, {len(rows)} views')
for t in variants:
    s = {f: sum(r[t][f] for r in rows) for f in rows[0][t]}
    n = max(s['blocks'], 1)
    print(f"  {t}: darkened {s['blocks']}, reference dark {s['truth_dark']} ({s['truth_dark'] / n * 100:.0f}%), "
          f"reference lit {s['truth_lit']} ({s['truth_lit'] / n * 100:.1f}%); brightened {s['brighter']}, "
          f"reference dark there {s['brighter_truth_dark']}")
    top = sorted(rows, key=lambda r: -r[t]['truth_lit'])[:10]
    print('    most reference-lit:', ', '.join(f"{r['view']} {r[t]['truth_lit']}/{r[t]['blocks']} (k {r['k']:.2f})"
                                          for r in top if r[t]['truth_lit']))
