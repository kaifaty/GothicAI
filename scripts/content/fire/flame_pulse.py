#!/usr/bin/env python3
"""Measure a simulated flame against the torch's guard rails: puff rate, swing, flame zones and rise speed.

    OPENCV_IO_ENABLE_OPENEXR=1 uv run --no-project --with numpy --with opencv-python-headless --with matplotlib \
        python scripts/content/fire/flame_pulse.py FRAMES_DIR --fps 64 --height-m 0.6 [--target 6] [--step 2] \
        [--head-top 0.12] [--speedup 1] [--plot PNG]

reports/content/remaster/wall-torch/flame-sim.md. A flame of a 7 cm torch head puffs at 5.5-7 Hz. Mantaflow at a
small scale may run slower, so the rate is measured and set back by the flipbook's playback rate. The frames are
the linear EXR renders of torch_flame.py (or fire_flipbook.py), FPS per second of sim. Two signals per frame:
the summed brightness and the tip height, the height under which 97 % of the emission lies (the tongue's tips
carry little of it, so the top few percent follow them). Each is detrended and windowed; the peak of its
spectrum between 1.5 and 20 Hz is the puff rate. With a cell per STEP frames (fire_atlas.py --step, pairs by
default), a cell lasts STEP/FPS s of sim: playing the cells at TARGET/peak x FPS/STEP per second puts the puff at
TARGET Hz. It prints that rate for STEP and for its neighbours, so the step can be picked for a loop of about 2 s.

Zones (McCaffrey): a real flame is a continuous body only low down; above it tongues and blobs are there part of
the time. In a centre band 1/10 of the frame wide, the share of frames in which each height (over --head-top, in
the same metres as --height-m) is lit above 5 % and 20 % of the flame's p99 brightness: where it drops below
95 %, 50 % and 10 %. Rise speed: the lag that best correlates the brightness of rows 5 cm apart, in game time
when the sim is played --speedup times faster than it ran. The torch's rails are in flame-sim.md, «Приёмка».
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


def zones(bands: np.ndarray, rows: np.ndarray, args: argparse.Namespace) -> None:
    """Flame zones and rise speed: bands and rows are frames x image rows, from the frame top down."""
    n, h = bands.shape
    metres = args.height_m / h
    z = (h - np.arange(h) - 0.5) * metres - args.head_top  # over the head top
    p99 = np.percentile(bands[bands > bands.max() * 1e-3], 99)
    for thr in (0.05, 0.2):
        lit = (bands > thr * p99).mean(axis=0)

        def below(share: float) -> str:
            idx = np.where((z > 0) & (lit < share))[0]
            return f'{z[idx].min() * 100:.0f}' if len(idx) else '-'
        print(f'  lit above {thr:.0%} of p99, cm over the head top: continuous (95 %) to {below(0.95)}, '
              f'half the time to {below(0.5)}, 10 % to {below(0.1)}')
    speeds = []
    for z0 in (0.03, 0.10, 0.18):
        r0, r1 = (h - 1 - int((zz + args.head_top) / metres) for zz in (z0, z0 + 0.05))
        if r1 < 0:
            continue
        a, b = rows[:, r0] - rows[:, r0].mean(), rows[:, r1] - rows[:, r1].mean()
        corr = [float(np.dot(a[:n - k], b[k:]) / (np.linalg.norm(a[:n - k]) * np.linalg.norm(b[k:]) + 1e-12))
                for k in range(12)]
        k = int(np.argmax(corr))
        lag = float(k)
        if 0 < k < len(corr) - 1:  # parabola through the peak
            lag += 0.5 * (corr[k - 1] - corr[k + 1]) / (corr[k - 1] - 2 * corr[k] + corr[k + 1])
        dt = lag / args.fps / args.speedup
        speeds.append(f'{z0 * 100:.0f}-{z0 * 100 + 5:.0f} cm {0.05 / dt if dt > 0 else float("inf"):.1f} m/s')
    print(f'  rise in game time (x{args.speedup:g}): ' + ', '.join(speeds))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('frames_dir', type=Path)
    parser.add_argument('--fps', type=float, required=True, help='frames per second of sim')
    parser.add_argument('--height-m', type=float, required=True, help='metres the frame spans vertically')
    parser.add_argument('--target', type=float, default=6.0)
    parser.add_argument('--step', type=int, default=2, help='sim frames per atlas cell')
    parser.add_argument('--head-top', type=float, default=0.12, help='metres from the frame bottom to the head top')
    parser.add_argument('--speedup', type=float, default=1.0, help='game playback speed over the sim')
    parser.add_argument('--plot', type=Path)
    args = parser.parse_args()
    files = sorted(args.frames_dir.glob('flame_*.exr'))
    if len(files) < 32:
        raise SystemExit(f'{len(files)} frames: too few for a spectrum')
    bright, tip, bands, row_sums = [], [], [], []
    for f in files:
        lum = load(f) @ np.array([0.2126, 0.7152, 0.0722])
        h, w = lum.shape
        bands.append(lum[:, w // 2 - w // 20:w // 2 + w // 20 + 1].max(axis=1))
        rows = lum.sum(axis=1)
        row_sums.append(rows)
        rows = rows[::-1]  # from the frame bottom up
        total = rows.sum()
        bright.append(total)
        cum = np.cumsum(rows) / max(total, 1e-9)
        tip.append(np.searchsorted(cum, 0.97) / len(rows) * args.height_m)
    bright, tip = np.array(bright), np.array(tip)
    fb, pb = spectrum(bright, args.fps)
    ft, pt = spectrum(tip, args.fps)
    f_b, f_t = peak(fb, pb), peak(ft, pt)
    print(f'{args.frames_dir}: {len(files)} frames, {len(files) / args.fps:.2f} s of sim')
    print(f'  brightness: mean {bright.mean():.1f}, swing {bright.std() / bright.mean():.1%}, peak {f_b:.2f} Hz')
    print(f'  tip height (97 %): mean {tip.mean() * 100:.1f} cm, range {tip.min() * 100:.1f}-{tip.max() * 100:.1f} cm, '
          f'peak {f_t:.2f} Hz')
    for f_name, f in (('brightness', f_b), ('tip', f_t)):
        for step in sorted({max(1, args.step - 1), args.step, args.step + 1}):
            rate = args.target / f * args.fps / step
            print(f'  step {step}: cells at {rate:.1f} per second put the {f_name} puff at {args.target} Hz '
                  f'(64 cells: a {64 / rate:.2f} s loop, {rate / args.target:.1f} cells a puff)')
    zones(np.array(bands), np.array(row_sums), args)
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
