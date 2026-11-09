#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow>=11,<13", "numpy>=1.26"]
# ///
"""GI shots against the path-traced reference, cell by cell (step 2 of the ray-tracing revisit).

uv run scripts/gi_compare.py ROOT [--variants off,gi1,gi2] SHOT...

ROOT holds one directory per variant with <SHOT>.png from scripts/bench/gi-clips.bench, and pt/ from
gi-reference.bench. Each frame is split into an 8x4 grid of cells. The two middle columns are left out, because
the hero stands there and the reference does not show him. So are cells the reference leaves nearly black,
whose ratio is noise. A cell's value is the mean display luminance (sRGB decoded, after the tone mapper): the
same tone mapper and sky-driven exposure serve every variant, so the ratios compare like with like. They are
smaller than the ratios of scene radiance. The particle flames of fires are drawn only in the rasterised frame.

Prints a Markdown table per shot (median ratio to the reference, its range, cells off by more than 15%) and a
summary over all shots.
"""
import argparse
from pathlib import Path

import numpy as np
from PIL import Image

LUMA = np.array([0.2126, 0.7152, 0.0722])
TOLERANCE = np.log(1.15)


def luminance(path):
    c = np.asarray(Image.open(path).convert('RGB'), dtype=np.float64) / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4) @ LUMA


def cells(img, cols=8, rows=4, skip=(3, 4)):
    h, w = img.shape
    return np.array([img[r * h // rows:(r + 1) * h // rows, c * w // cols:(c + 1) * w // cols].mean()
                     for r in range(rows) for c in range(cols) if c not in skip])


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('root', type=Path)
    ap.add_argument('shots', nargs='+')
    ap.add_argument('--variants', default='off,gi1,gi2')
    args = ap.parse_args()
    names = args.variants.split(',')

    print('| shot | ' + ' | '.join(f'{n}: median (range)' for n in names) + ' | '
          + ' | '.join(f'{n}: cells off >15%' for n in names) + ' |')
    print('|---|' + '---|' * (2 * len(names)))
    logs = {n: [] for n in names}
    for shot in args.shots:
        ref = cells(luminance(args.root / 'pt' / f'{shot}.png'))
        keep = ref > 0.002
        ratio, off = [], []
        for n in names:
            r = cells(luminance(args.root / n / f'{shot}.png'))[keep] / ref[keep]
            logs[n].extend(np.log(r))
            ratio.append(f'{np.median(r):.2f} ({r.min():.2f}…{r.max():.2f})')
            off.append(f'{int((np.abs(np.log(r)) > TOLERANCE).sum())}/{len(r)}')
        print(f'| {shot} | ' + ' | '.join(ratio) + ' | ' + ' | '.join(off) + ' |')
    print()
    for n in names:
        a = np.array(logs[n])
        print(f'- {n}: median {np.exp(np.median(a)):.2f} of the reference, mean deviation '
              f'{np.exp(np.abs(a).mean()):.2f}x, {np.mean(np.abs(a) <= TOLERANCE) * 100:.0f}% of {len(a)} cells within ±15%')


if __name__ == '__main__':
    main()
