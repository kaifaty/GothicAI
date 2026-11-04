#!/usr/bin/env python3
"""Flicker of the lamps' penumbra between consecutive frames of a still camera (step-2d.md).

    uv run --with pillow --with numpy python3 flicker.py <main run> <hard run> <run> [<run> ...]

Runs of step-2d/vsm-still.bench: per view <view>.png and <view>-f1 .. -f7.png, eight consecutive frames. The penumbra
is where main's first frame differs from the 'hard' shadow's (OPENGOTHIC_VSM_LAMP=hard) by more than 2 levels in its
largest channel. Per view and run: the mean absolute difference of consecutive frames inside that mask (largest
channel, 8 bit, over the 7 pairs), and its ratio to main's.
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

VIEWS = [f'interior-{i}' for i in range(1, 5)] + [f'harbour-night-{i}' for i in range(1, 5)]
FRAMES = [''] + [f'-f{i}' for i in range(1, 8)]


def load(path: Path) -> np.ndarray:
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.int16)


def flicker(run: Path, view: str, mask: np.ndarray) -> float:
    frames = [load(run / f'{view}{s}.png') for s in FRAMES]
    diffs = [np.abs(a - b).max(axis=2)[mask].mean() for a, b in zip(frames, frames[1:])]
    return float(np.mean(diffs))


def main() -> None:
    main_run, hard_run, *runs = map(Path, sys.argv[1:])
    print('| View | penumbra, % of pixels | main | ' + ' | '.join(r.name for r in runs) + ' |')
    print('|---|---:|---:|' + '---:|' * len(runs))
    for view in VIEWS:
        mask = np.abs(load(main_run / f'{view}.png') - load(hard_run / f'{view}.png')).max(axis=2) > 2
        base = flicker(main_run, view, mask)
        cells = [f'{f:.3f} ({f / base * 100 - 100:+.0f} %)' for f in (flicker(r, view, mask) for r in runs)]
        print(f'| {view} | {mask.mean() * 100:.1f} | {base:.3f} | ' + ' | '.join(cells) + ' |')


if __name__ == '__main__':
    main()
