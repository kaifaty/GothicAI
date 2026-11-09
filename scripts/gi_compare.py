#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow>=11,<13", "numpy>=1.26"]
# ///
"""GI shots against the path-traced reference, cell by cell (step 2 of the ray-tracing revisit).

uv run scripts/gi_compare.py ROOT [--variants off,gi1,gi2] SHOT...
uv run scripts/gi_compare.py --response CLIP.mp4
uv run scripts/gi_compare.py --turn CLIP.mp4


ROOT holds one directory per variant with <SHOT>.png from scripts/bench/gi-clips.bench, and pt/ from
gi-reference.bench. Each frame is split into an 8x4 grid of cells. The two middle columns are left out, because
the hero stands there and the reference does not show him. So are cells the reference leaves nearly black,
whose ratio is noise. A cell's value is the mean display luminance (sRGB decoded, after the tone mapper): the
same tone mapper and sky-driven exposure serve every variant, so the ratios compare like with like. They are
smaller than the ratios of scene radiance. The particle flames of fires are drawn only in the rasterised frame.

Prints a Markdown table per shot (median ratio to the reference, its range, cells off by more than 15%) and a
summary over all shots.

--response takes a clip of gi-clips.bench (hut-response: 22:00, then 12:00 in the same view 1 s later, 30 frames
per second) and prints how long each cell takes to come within 10% of its new brightness, counted from the frame
the time changes in. Direct light changes at once, so the slowest cells show the lag of the reflected light.

--turn takes the clip hut-turn of scripts/bench/gi-turn.bench (the camera stands 1 s, turns 180 degrees at once, stands
3 s, turns back, stands 2 s; 30 frames per second) and compares the frame before the turn with the first frame after
the return, cell by cell, as above: the light of probes that live on off the screen comes back as it was.
"""
import argparse
import subprocess
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


def clip_cells(path, w=320, h=180):
    raw = subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', str(path), '-vf', f'scale={w}:{h}', '-f', 'rawvideo',
                          '-pix_fmt', 'rgb24', '-'], capture_output=True, check=True).stdout
    c = np.frombuffer(raw, np.uint8).reshape(-1, h, w, 3) / 255.0
    lum = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4) @ LUMA
    return np.array([cells(f) for f in lum])


def response(path, at=1.0, fps=30, settle=0.1):
    # The exposure follows the sky at once, so after the change it stays put: a cell's brightness then moves only
    # with its light. The reflected light of a shaded cell starts from about nothing (the night's light is tiny
    # at the day's exposure), so the time to come within 10% of the new brightness is its lag; lit cells settle
    # at once.
    c = clip_cells(path)
    mean = np.log(c.mean(axis=1) + 1e-6)
    near = range(max(1, round(at * fps) - 3), min(len(c), round(at * fps) + 4))
    t0 = max(near, key=lambda t: abs(mean[t] - mean[t - 1]))  # the first frame after the change
    after = c[-8:].mean(axis=0)
    noise = c[-20:].std(axis=0)  # the encoder and the TAA jitter: a cell this close to its end has settled
    keep = after > 0.005
    frames = []
    for i in np.flatnonzero(keep):
        late = np.flatnonzero(np.abs(c[t0:, i] - after[i]) > max(settle * after[i], 3 * noise[i]))
        frames.append(int(late[-1]) + 2 if len(late) else 1)  # frames since the change, the settled one included
    frames = np.array(frames)
    print(f'{path}: {len(c)} frames, change at frame {t0}, {len(frames)} of {c.shape[1]} cells lit enough')
    first = c[t0, keep] / after[keep]
    print(f'- first frame after the change: {np.median(first):.0%} of the new brightness (median of the cells), '
          f'lowest {first.min():.0%}')
    if len(frames):
        print(f'- within {settle:.0%} of the new brightness: median {np.median(frames) * 1000 / fps:.0f} ms, '
              f'slowest cell {frames.max() * 1000 / fps:.0f} ms ({frames.max()} frames at {fps} per second)')


def turn(path, at=1.0, back=4.0, fps=30, window=0.5):
    # The two cuts are the largest jumps of the frame near their times (the view changes whole); the light of the
    # probes settling after the return moves the cells far less.
    c = clip_cells(path)
    jump = np.median(np.abs(np.diff(np.log(c + 1e-4), axis=0)), axis=1)  # jump[t]: frame t+1 against frame t

    def cut(t):
        lo, hi = max(0, round((t - window) * fps)), min(len(jump), round((t + window) * fps))
        return lo + int(np.argmax(jump[lo:hi])) + 1  # the first frame after the cut

    t0, t1 = cut(at), cut(back)
    before, after = c[t0 - 1], c[t1]
    keep = before > 0.002
    change = np.abs(after[keep] / before[keep] - 1)
    print(f'{path}: {len(c)} frames, turn at frame {t0}, back at frame {t1}, {keep.sum()} of {c.shape[1]} cells lit enough')
    settle = next((k for k in range(t1, len(c)) if np.abs(c[k][keep] / before[keep] - 1).max() <= 0.05), None)
    if settle is not None:
        print(f'- every cell within 5% of the frame before the turn from frame {settle - t1 + 1} after the return')
    print(f'- turn: first frame after the return, max cell change {change.max() * 100:.1f}%, '
          f'median {np.median(change) * 100:.1f}%')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('root', type=Path, nargs='?')
    ap.add_argument('shots', nargs='*')
    ap.add_argument('--variants', default='off,gi1,gi2')
    ap.add_argument('--response', type=Path, help='a clip with a sharp change of light: the time to settle')
    ap.add_argument('--turn', type=Path, help='a clip of a turn and back (gi-turn.bench): the change it leaves')
    args = ap.parse_args()
    if args.response:
        response(args.response)
        return
    if args.turn:
        turn(args.turn)
        return
    if args.root is None or not args.shots:
        ap.error('ROOT and SHOT... are required without --response')
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
