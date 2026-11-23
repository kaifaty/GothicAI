#!/usr/bin/env python3
"""Flicker of the lamps' penumbra between consecutive frames of a still camera (step-2d.md).

    uv run --with pillow --with numpy python3 flicker.py <main run> <hard run> <run> [<run> ...]

Runs of step-2d/vsm-still.bench: per view <view>.png and <view>-f1 .. -f7.png, consecutive frames (the first seven). The penumbra
is where main's first frame differs from the 'hard' shadow's (OPENGOTHIC_VSM_LAMP=hard) by more than 2 levels in its
largest channel. Per view and run: the mean absolute difference of consecutive frames inside that mask (largest
channel, 8 bit, over 6 pairs), and its ratio to main's; the convergence: the run's first frame against main's
inside the mask, mean and the share above 8 levels. The reference for both is main's consecutive frames.
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

VIEWS = [f'interior-{i}' for i in range(1, 5)] + [f'harbour-night-{i}' for i in range(1, 5)]
# -f7 of runs before 2026-11-04 19:50 was taken after the next view's camera command: seven frames, six pairs
FRAMES = [''] + [f'-f{i}' for i in range(1, 7)]


def load(path: Path) -> np.ndarray:
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.int16)


def flicker(run: Path, view: str, mask: np.ndarray) -> tuple[float, float]:
    frames = [load(run / f'{view}{s}.png') for s in FRAMES]
    diffs = [np.abs(a - b).max(axis=2)[mask] for a, b in zip(frames, frames[1:])]
    return float(np.mean([d.mean() for d in diffs])), float(np.mean([(d > 8).mean() for d in diffs]) * 100)


def converge(run: Path, main_run: Path, view: str, mask: np.ndarray) -> tuple[float, float]:
    d = np.abs(load(run / f'{view}.png') - load(main_run / f'{view}.png')).max(axis=2)[mask]
    return float(d.mean()), float((d > 8).mean() * 100)


def main() -> None:
    main_run, hard_run, *runs = map(Path, sys.argv[1:])
    names = ' | '.join(f'{r.name}: vs main (>8) | {r.name}: flicker' for r in runs)
    print(f'| View | penumbra, % | main: flicker (>8) | {names} |')
    print('|---|---:|---:|' + '---:|---:|' * len(runs))
    for view in VIEWS:
        mask = np.abs(load(main_run / f'{view}.png') - load(hard_run / f'{view}.png')).max(axis=2) > 2
        base, base8 = flicker(main_run, view, mask)
        cells = []
        for r in runs:
            c, c8 = converge(r, main_run, view, mask)
            f, _ = flicker(r, view, mask)
            cells += [f'{c:.3f} ({c8:.2f} %)', f'{f:.3f} ({f / base * 100 - 100:+.0f} %)']
        print(f'| {view} | {mask.mean() * 100:.1f} | {base:.3f} ({base8:.2f} %) | ' + ' | '.join(cells) + ' |')


if __name__ == '__main__':
    main()
