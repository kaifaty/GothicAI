#!/usr/bin/env python3
"""Differences of the bench shots of two runs, and a crop of the place where they differ most.

    uv run --with pillow --with numpy python3 frame_diff.py <run A> <run B> [--crop <dir>] [--suffix -next] [shot ...]

Per shot (<run>/<shot>.png): the mean absolute difference of the 8-bit RGB values, its 99.9th percentile, the share
of pixels whose largest channel differs by more than 8 and 16, and where a 192x192 window differs most. With --crop,
<dir>/<shot>.png holds that window: A | B | the difference x4, twice the size.
"""
import argparse
from pathlib import Path

import numpy as np
from PIL import Image

SHOTS = [f'interior-{i}' for i in range(1, 5)] + [f'harbour-night-{i}' for i in range(1, 5)]
WINDOW = 192


def load(path: Path) -> np.ndarray:
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.int16)


def worst_window(diff: np.ndarray) -> tuple[int, int]:
    h, w = diff.shape
    sat = np.pad(diff.astype(np.float64).cumsum(0).cumsum(1), ((1, 0), (1, 0)))
    sums = sat[WINDOW:, WINDOW:] - sat[:-WINDOW, WINDOW:] - sat[WINDOW:, :-WINDOW] + sat[:-WINDOW, :-WINDOW]
    y, x = np.unravel_index(int(np.argmax(sums)), sums.shape)
    return int(y), int(x)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('a', type=Path)
    parser.add_argument('b', type=Path)
    parser.add_argument('--crop', type=Path)
    parser.add_argument('--suffix', default='', help='B shot name suffix: <shot><suffix>.png in run B')
    parser.add_argument('shots', nargs='*', default=SHOTS)
    args = parser.parse_args()
    print('| Shot | mean | p99.9 | max | >8 | >16 | window |')
    print('|---|---:|---:|---:|---:|---:|---|')
    for shot in args.shots:
        a, b = load(args.a / f'{shot}.png'), load(args.b / f'{shot}{args.suffix}.png')
        d = np.abs(a - b)
        dmax = d.max(axis=2)
        y, x = worst_window(dmax)
        print(f'| {shot} | {d.mean():.3f} | {np.percentile(dmax, 99.9):.0f} | {dmax.max()} | {(dmax > 8).mean() * 100:.3f} % | '
              f'{(dmax > 16).mean() * 100:.3f} % | {x},{y} |')
        if args.crop:
            args.crop.mkdir(parents=True, exist_ok=True)
            win = (slice(y, y + WINDOW), slice(x, x + WINDOW))
            strip = np.concatenate([a[win], b[win], np.clip(d[win] * 4, 0, 255)], axis=1).astype(np.uint8)
            Image.fromarray(strip).resize((strip.shape[1] * 2, strip.shape[0] * 2), Image.NEAREST) \
                .save(args.crop / f'{shot}.png')


if __name__ == '__main__':
    main()
