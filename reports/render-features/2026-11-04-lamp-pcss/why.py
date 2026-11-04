#!/usr/bin/env python3
"""Variants of the lamp shadow against the truth and main, still frames of step-2d/vsm-still.bench (step-2d-why.md).

    uv run --with pillow --with numpy python3 why.py <dir> <variant> [<variant> ...] [--crop <out> <best>]

<dir>/<variant>/<view>.png and -f1 .. -f6; <dir>/main, <dir>/hard and <dir>/truth must be there. The mask is where
main differs from 'hard' by more than 2 levels (largest channel, 8 bit). Per view and variant, inside the mask: the
distance to the truth and to main (mean, and the share above 8 levels), and the flicker, the mean difference of
consecutive frames over 6 pairs. With --crop: per interior view, a 192x192 window where main and d6 differ most,
main | d6 | <best> | truth, twice the size.
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

VIEWS = [f'interior-{i}' for i in range(1, 5)] + [f'harbour-night-{i}' for i in range(2, 5)]
FRAMES = [''] + [f'-f{i}' for i in range(1, 7)]
WINDOW = 192


def load(path: Path) -> np.ndarray:
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.int16)


def dist(a: np.ndarray, b: np.ndarray, mask: np.ndarray) -> str:
    d = np.abs(a - b).max(axis=2)[mask]
    return f'{d.mean():.2f} ({(d > 8).mean() * 100:.1f} %)'


def flicker(run: Path, view: str, mask: np.ndarray) -> float:
    f = [load(run / f'{view}{s}.png') for s in FRAMES]
    return float(np.mean([np.abs(a - b).max(axis=2)[mask].mean() for a, b in zip(f, f[1:])]))


def worst(d: np.ndarray) -> tuple[int, int]:
    sat = np.pad(d.astype(np.float64).cumsum(0).cumsum(1), ((1, 0), (1, 0)))
    s = sat[WINDOW:, WINDOW:] - sat[:-WINDOW, WINDOW:] - sat[WINDOW:, :-WINDOW] + sat[:-WINDOW, :-WINDOW]
    y, x = np.unravel_index(int(np.argmax(s)), s.shape)
    return int(y), int(x)


def main() -> None:
    args = sys.argv[1:]
    crop = None
    if '--crop' in args:
        i = args.index('--crop')
        crop = (Path(args[i + 1]), args[i + 2])
        args = args[:i]
    root, variants = Path(args[0]), args[1:]
    print('| View | mask, % | variant | to truth (>8) | to main (>8) | flicker | flicker vs main |')
    print('|---|---:|---|---:|---:|---:|---:|')
    for view in VIEWS:
        m, h, t = (load(root / r / f'{view}.png') for r in ('main', 'hard', 'truth'))
        mask = np.abs(m - h).max(axis=2) > 2
        if not mask.any():
            continue
        base = flicker(root / 'main', view, mask)
        for v in ['main'] + variants:
            a = load(root / v / f'{view}.png')
            f = flicker(root / v, view, mask)
            print(f'| {view} | {mask.mean() * 100:.1f} | {v} | {dist(a, t, mask)} | {dist(a, m, mask)} | {f:.2f} | '
                  f'{f / base * 100 - 100:+.0f} % |')
        if crop and view.startswith('interior') and view != 'interior-1':
            d6 = load(root / 'd6' / f'{view}.png')
            best = load(root / crop[1] / f'{view}.png')
            y, x = worst(np.abs(m - d6).max(axis=2))
            win = (slice(y, y + WINDOW), slice(x, x + WINDOW))
            strip = np.concatenate([m[win], d6[win], best[win], t[win]], axis=1).astype(np.uint8)
            crop[0].mkdir(parents=True, exist_ok=True)
            Image.fromarray(strip).resize((strip.shape[1] * 2, strip.shape[0] * 2), Image.NEAREST) \
                .save(crop[0] / f'{view}.png')


if __name__ == '__main__':
    main()
