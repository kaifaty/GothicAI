#!/usr/bin/env python3
"""How far apart the frames of two runs of scripts/bench/perf-trees.bench are (hiz-occluders.md).

    uv run --no-project --with numpy --with pillow python \
        reports/engine/2026-11-16-modern-trees/hiz-occluders/frames.py <folder A> <folder B> [<folder C> <folder D>]

A folder holds the shots of one run (inside.png, near-30.png, far-100.png, far-300.png, pass.png). For each shot:
the mean of |A - B| over the pixels and channels (0..255), the share of pixels whose largest channel differs by more
than 8 and by more than 32, and the mean brightness of each frame (a dark dropout of the sun's shadow pages, the
tree-shadow-flicker task, shows as a frame much darker than its pair). A second pair of folders, two runs of one
code, prints alongside as the noise to judge the first by.
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
from PIL import Image

SHOTS = ('inside', 'near-30', 'far-100', 'far-300', 'pass')


def load(folder: Path, shot: str) -> np.ndarray:
    return np.asarray(Image.open(folder / f'{shot}.png').convert('RGB'), dtype=np.int16)


def compare(a: Path, b: Path, shot: str) -> dict:
    x, y = load(a, shot), load(b, shot)
    d = np.abs(x - y)
    big = d.max(axis=2)
    luma = lambda f: float((f @ np.array([0.2126, 0.7152, 0.0722])).mean())
    return dict(mad=float(d.mean()), over8=float((big > 8).mean()), over32=float((big > 32).mean()),
                luma=(luma(x), luma(y)))


def main() -> int:
    folders = [Path(p) for p in sys.argv[1:]]
    if len(folders) not in (2, 4):
        raise SystemExit(__doc__)
    pairs = [folders[:2]] + ([folders[2:]] if len(folders) == 4 else [])
    head = '| Кадр | ' + ' | '.join(f'{name}: разница, доля >8, доля >32, яркость' for name in ('пара', 'шум')[:len(pairs)]) + ' |'
    print(head)
    print('|---' * (1 + len(pairs)) + '|')
    for shot in SHOTS:
        cells = []
        for a, b in pairs:
            r = compare(a, b, shot)
            cells.append(f"{r['mad']:.2f}; {r['over8']*100:.1f} %; {r['over32']*100:.2f} %; "
                         f"{r['luma'][0]:.1f} / {r['luma'][1]:.1f}")
        print(f'| {shot} | ' + ' | '.join(cells) + ' |')
    return 0


if __name__ == '__main__':
    sys.exit(main())
