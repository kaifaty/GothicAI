#!/usr/bin/env python3
"""Fit a ready flame sequence (EXR frames from a CC0 pack) into the torch's cell, as frames for fire_atlas.py.

    OPENCV_IO_ENABLE_OPENEXR=1 uv run --no-project --with numpy --with opencv-python-headless \
        python scripts/content/fire/flame_cells.py SRC_DIR OUT_DIR [--start 100] [--count 380] [--sx 1] [--sy 1] \
        [--base-m 0.08] [--crop-bottom 0] [--fade 8] [--ramp COLOUR_EXR TEMPERATURE_EXR] [--luma-gain G] \
        [--blue 1] [--cell 128 256] [--cell-h-m 0.6]

reports/content/remaster/wall-torch/flame-sim.md, «Готовые наборы и инструменты». SRC_DIR holds the frames of one
flame (Unity Labs' "VFX image sequences": 480 frames, the first hundred are the ignition, so --start 100). Each
frame is scaled by --sx/--sy and put into a cell of --cell pixels that spans --cell-h-m metres, centred, its
bottom row --base-m metres over the cell's bottom; its lowest --fade rows fade in, since the source is cut flat
where its burner was and the torch head hides only the middle of that edge. What sticks out of the cell is cut.
OUT_DIR gets flame_NNNN.exr, linear RGB, the names and format torch_flame.py renders, so flame_pulse.py measures
them against the torch's rails and fire_atlas.py packs them.

Colour. A "-temperature" sequence is grey; --ramp takes the pack's two flipbooks of the same flame, the coloured
one and the temperature one, and reads from them the colour the pack's authors gave each temperature. A coloured
source goes through the same ramp with --luma-gain: its brightest channel times G is taken as the temperature, so
two flames from different sequences share one colour. --blue scales the ramp's blue channel: the pack's ramp runs
to a white-hot core, a torch's sooty flame stays yellow. The pack's ramp also clips red at its top, so its hottest
filaments are pale, and scaled down to a cell they average with black into dim olive patches; the ramp's red is
therefore run on past the clip along its own slope (fire_atlas.py's shoulder does the clipping, after the scaling).
"""
from __future__ import annotations

import argparse
from pathlib import Path

import cv2
import numpy as np


def load(path: Path) -> np.ndarray:
    a = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
    if a is None:
        raise SystemExit(f'{path}: unreadable (OPENCV_IO_ENABLE_OPENEXR=1?)')
    return a.astype(np.float32)  # BGR, as OpenCV reads and writes it


def ramp(colour: Path, temperature: Path, blue: float) -> tuple[np.ndarray, np.ndarray]:
    """Temperatures and their mean BGR colour, from two flipbooks of the same cells."""
    col = load(colour).reshape(-1, 3)
    tmp = load(temperature)[..., 0].ravel()
    edges = np.linspace(0, float(tmp.max()), 257)
    idx = np.clip(np.digitize(tmp, edges) - 1, 0, 255)
    n = np.bincount(idx, minlength=256)
    mean = np.stack([np.bincount(idx, weights=col[:, c], minlength=256) / np.maximum(n, 1) for c in range(3)], axis=1)
    ok = n > 50
    temps, mean = ((edges[:-1] + edges[1:]) / 2)[ok], mean[ok]
    mean[:, 0] *= blue
    red = mean[:, 2]
    clip = int(np.argmax(red >= 0.99 * red.max()))  # the first bin at the clip
    if 8 <= clip < len(red) - 1:
        slope = np.polyfit(temps[clip // 2:clip], red[clip // 2:clip], 1)
        mean[clip:, 2] = np.maximum(red[clip:], np.polyval(slope, temps[clip:]))
    return temps, mean


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('src', type=Path)
    parser.add_argument('out', type=Path)
    parser.add_argument('--start', type=int, default=100)
    parser.add_argument('--count', type=int, default=380)
    parser.add_argument('--sx', type=float, default=1.0)
    parser.add_argument('--sy', type=float, default=1.0)
    parser.add_argument('--base-m', type=float, default=0.08)
    parser.add_argument('--crop-bottom', type=int, default=0, help='source rows cut from the bottom first')
    parser.add_argument('--fade', type=int, default=8)
    parser.add_argument('--ramp', nargs=2, type=Path, metavar=('COLOUR_EXR', 'TEMPERATURE_EXR'))
    parser.add_argument('--luma-gain', type=float)
    parser.add_argument('--blue', type=float, default=1.0)
    parser.add_argument('--cell', nargs=2, type=int, default=[128, 256], metavar=('W', 'H'))
    parser.add_argument('--cell-h-m', type=float, default=0.6)
    args = parser.parse_args()
    files = sorted(args.src.rglob('*.exr'))[args.start:args.start + args.count]
    if not files:
        raise SystemExit(f'{args.src}: no frames from {args.start}')
    if args.luma_gain and not args.ramp:
        raise SystemExit('--luma-gain: only with --ramp')
    temps, colours = ramp(*args.ramp, args.blue) if args.ramp else (None, None)
    cw, ch = args.cell
    args.out.mkdir(parents=True, exist_ok=True)
    for old in args.out.glob('flame_*.exr'):
        old.unlink()
    for i, path in enumerate(files):
        src = load(path)
        if temps is not None:
            t = src.max(axis=2) * args.luma_gain if args.luma_gain else src[..., 0]
            src = np.stack([np.interp(t, temps, colours[:, c]) for c in range(3)], axis=2).astype(np.float32)
            src[t <= 0] = 0
        if args.crop_bottom:
            src = src[:-args.crop_bottom]
        h, w = src.shape[:2]
        nw, nh = int(round(w * args.sx)), int(round(h * args.sy))
        shrink = max(args.sx, args.sy) <= 1
        img = np.maximum(cv2.resize(src, (nw, nh), interpolation=cv2.INTER_AREA if shrink else cv2.INTER_CUBIC), 0)
        edge = np.clip((nh - 0.5 - np.arange(nh)) / max(args.fade, 1), 0, 1)
        img *= (edge * edge * (3 - 2 * edge))[:, None, None]
        cell = np.zeros((ch, cw, 3), np.float32)
        top = ch - int(round(args.base_m / args.cell_h_m * ch)) - nh
        left = (cw - nw) // 2
        sy0, sx0, cy0, cx0 = max(0, -top), max(0, -left), max(0, top), max(0, left)
        hh, ww = min(nh - sy0, ch - cy0), min(nw - sx0, cw - cx0)
        cell[cy0:cy0 + hh, cx0:cx0 + ww] = img[sy0:sy0 + hh, sx0:sx0 + ww]
        cv2.imwrite(str(args.out / f'flame_{i + 1:04d}.exr'), cell)
    print(f'{args.out}: {len(files)} frames {cw}x{ch}, source {w}x{h} scaled to {nw}x{nh}, '
          f'{sy0} rows cut at the top, {sx0} columns at each side')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
