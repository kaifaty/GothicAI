#!/usr/bin/env python3
"""Measure how fast a simulated flame puffs: the spectrum of its brightness and its tip height over the frames.

    OPENCV_IO_ENABLE_OPENEXR=1 uv run --no-project --with numpy --with opencv-python-headless --with matplotlib \
        python scripts/content/fire/flame_pulse.py FRAMES_DIR --fps 64 --height-m 0.6 [--target 6] [--plot PNG]

reports/content/remaster/wall-torch/flame-sim.md. A flame of a 7 cm torch head puffs at 5.5-7 Hz. Mantaflow at a
small scale may run slower, so the rate is measured and set back by the flipbook's playback rate. The frames are
the linear EXR renders of torch_flame.py (or fire_flipbook.py), FPS per second of sim. Two signals per frame:
the summed brightness and the tip height, the height under which 97 % of the emission lies (the tongue's tips
carry little of it, so the top few percent follow them). Each is detrended and windowed; the peak of its
spectrum between 1.5 and 20 Hz is the puff rate. With pairs of frames averaged into atlas cells (fire_atlas.py),
a cell lasts 2/FPS s of sim: playing the cells at TARGET/peak x FPS/2 per second puts the puff at TARGET Hz.
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
    return np.asarray(a[..., 2::-1], dtype=np.float64)


def spectrum(x: np.ndarray, fps: float) -> tuple[np.ndarray, np.ndarray]:
    t = np.arange(len(x))
    x = x - np.polyval(np.polyfit(t, x, 1), t)
    x = x * np.hanning(len(x))
    n = 8 * len(x)  # zero-padded: a finer grid for the peak, not more resolution
    power = np.abs(np.fft.rfft(x, n)) ** 2
    return np.fft.rfftfreq(n, 1 / fps), power


def peak(freq: np.ndarray, power: np.ndarray, lo: float = 1.5, hi: float = 20.0) -> float:
    band = (freq >= lo) & (freq <= hi)
    return float(freq[band][np.argmax(power[band])])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('frames_dir', type=Path)
    parser.add_argument('--fps', type=float, required=True, help='frames per second of sim')
    parser.add_argument('--height-m', type=float, required=True, help='metres the frame spans vertically')
    parser.add_argument('--target', type=float, default=6.0)
    parser.add_argument('--plot', type=Path)
    args = parser.parse_args()
    files = sorted(args.frames_dir.glob('flame_*.exr'))
    if len(files) < 32:
        raise SystemExit(f'{len(files)} frames: too few for a spectrum')
    bright, tip = [], []
    for f in files:
        lum = load(f) @ np.array([0.2126, 0.7152, 0.0722])
        rows = lum.sum(axis=1)[::-1]  # from the frame bottom up
        total = rows.sum()
        bright.append(total)
        cum = np.cumsum(rows) / max(total, 1e-9)
        tip.append(np.searchsorted(cum, 0.97) / len(rows) * args.height_m)
    bright, tip = np.array(bright), np.array(tip)
    fb, pb = spectrum(bright, args.fps)
    ft, pt = spectrum(tip, args.fps)
    f_b, f_t = peak(fb, pb), peak(ft, pt)
    rate = args.target / f_t * args.fps / 2
    print(f'{args.frames_dir}: {len(files)} frames, {len(files) / args.fps:.2f} s of sim')
    print(f'  brightness: mean {bright.mean():.1f}, swing {bright.std() / bright.mean():.1%}, peak {f_b:.2f} Hz')
    print(f'  tip height (97 %): mean {tip.mean() * 100:.1f} cm, range {tip.min() * 100:.1f}-{tip.max() * 100:.1f} cm, '
          f'peak {f_t:.2f} Hz')
    print(f'  cells at {rate:.1f} per second put the tip puff at {args.target} Hz '
          f'(64 cells: a {64 / rate:.2f} s loop)')
    if args.plot:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
        t = np.arange(len(files)) / args.fps
        fig, ax = plt.subplots(2, 2, figsize=(11, 6))
        ax[0, 0].plot(t, bright / bright.mean())
        ax[0, 0].set(title='brightness / mean', xlabel='s of sim')
        ax[1, 0].plot(t, tip * 100)
        ax[1, 0].set(title='tip height (97 %), cm', xlabel='s of sim')
        for a, (f, p, pk) in zip(ax[:, 1], ((fb, pb, f_b), (ft, pt, f_t))):
            band = f <= 25
            a.plot(f[band], p[band] / p[band].max())
            a.axvline(pk, color='r', lw=0.8)
            a.set(title=f'spectrum, peak {pk:.2f} Hz', xlabel='Hz of sim')
        fig.tight_layout()
        fig.savefig(args.plot, dpi=90)
        print(f'  plot: {args.plot}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
