#!/usr/bin/env python3
"""Find one-frame jumps of each spruce in a fir-bough-lod recording.

    uv run --offline --no-project --with numpy python scripts/content/fir_bough_pops.py \
      <set> <bench output> <csv> --visuals NAME...

The clip is read in grey at 1/8 of its size: a jump of a level or of a shadow is a large patch, the grass and the
encoder's noise are fine detail. For each spruce the frame difference is the mean absolute change of its box on screen
(the box of fir_eye_frames.tree_bounds, as wide as 0.6 of its height, so its shadow at noon is inside). A frame's score
is that difference over the median of the 61 frames around it. A jump is a score at --ratio or more, the highest within
4 frames and above every score 8 to 60 frames away. The script prints the jumps while the spruce is in 3D, then a row
for every threshold of the hard level and shadow switches it crosses (falling 800 px and the px of its four levels,
400, 200, 100, 50 for spruce A, rising 1.1 times those): the highest score within 4 frames of the crossing and the highest one 8 to 60 frames away.
<csv> is the table of fir_bough_frames.py for the same recording; the heights and frames are its own.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import subprocess
from pathlib import Path
import numpy as np

from fir_eye_frames import FOV_DEGREES, HYSTERESIS, camera_path, tree_bounds

NAME = 'fir-bough-lod'
SCENARIO = Path(__file__).resolve().parents[1] / 'bench' / f'{NAME}.bench'
WIDTH, HEIGHT = 240, 135
SHADOW_PX = 800
NEAR, RING, WINDOW = 4, (8, 60), 30


def eyes(run: Path) -> tuple[list[tuple[float, ...]], tuple[float, ...]]:
    """The camera of every recorded frame and the point it looks at, as fir_bough_frames.py places them."""
    events = [json.loads(line) for line in (run / 'events.jsonl').read_text().splitlines()]
    events = [event for event in events if event['seg'] == NAME]
    (record,) = [event for event in events if event['kind'] == 'rec']
    positions, durations, starts = camera_path(SCENARIO, events, 'fir-bough')
    first = next(line.split() for line in SCENARIO.read_text().splitlines() if line.startswith('cam at '))
    target = tuple(map(float, first[5:8]))
    out, leg = [], -1
    eye = origin = positions[0]
    with (run / 'metrics.csv').open(newline='') as stream:
        for sample in csv.DictReader(stream):
            if sample['seg'] != NAME:
                continue
            time = record['t'] + int(sample['t'])
            if leg + 1 < len(starts) and time == starts[leg + 1]:
                origin = eye
                leg += 1
            if leg >= 0:
                progress = min(1, (time - starts[leg]) / durations[leg])
                eye = tuple(a + (b - a) * progress for a, b in zip(origin, positions[leg + 1]))
            out.append(eye)
    return out, target


def box(eye, target, middle, height) -> tuple[int, int, int, int]:
    """The spruce's box on the 1/8 frame: columns and rows, end exclusive."""
    forward = [t - e for t, e in zip(target, eye)]
    norm = math.hypot(*forward)
    forward = [f / norm for f in forward]
    right = [forward[2], 0.0, -forward[0]]  # forward x (0, 1, 0): Gothic's screen x
    norm = math.hypot(*right)
    right = [r / norm for r in right]
    up = [right[1] * forward[2] - right[2] * forward[1], right[2] * forward[0] - right[0] * forward[2],
          right[0] * forward[1] - right[1] * forward[0]]
    tan = math.tan(math.radians(FOV_DEGREES / 2))
    xs, ys = [], []
    for dx in (-0.3, 0.3):
        for dy in (-0.5, 0.5):
            for dz in (-0.3, 0.3):
                at = [m + d * height - e for m, d, e in zip(middle, (dx, dy, dz), eye)]
                depth = max(1.0, sum(a * f for a, f in zip(at, forward)))
                xs.append((0.5 + sum(a * r for a, r in zip(at, right)) / (depth * tan * WIDTH / HEIGHT) / 2) * WIDTH)
                ys.append((0.5 - sum(a * u for a, u in zip(at, up)) / (depth * tan) / 2) * HEIGHT)
    return (max(0, math.floor(min(xs))), max(0, math.floor(min(ys))),
            min(WIDTH, math.ceil(max(xs))), min(HEIGHT, math.ceil(max(ys))))


def differences(video: Path, boxes: list[list[tuple[int, int, int, int]]]) -> list[list[float]]:
    """Per frame and spruce: the mean absolute change from the previous frame over the union of both boxes."""
    stream = subprocess.Popen(['ffmpeg', '-v', 'error', '-i', video, '-vf', f'scale={WIDTH}:{HEIGHT}:flags=area',
                               '-pix_fmt', 'gray', '-f', 'rawvideo', '-'], stdout=subprocess.PIPE)
    out, previous = [], None
    for frame in boxes:
        data = stream.stdout.read(WIDTH * HEIGHT)
        if len(data) < WIDTH * HEIGHT:
            raise ValueError(f'{video} ends after {len(out)} frames')
        image = np.frombuffer(data, np.uint8).reshape(HEIGHT, WIDTH).astype(np.int16)
        row = []
        for k, (x0, y0, x1, y1) in enumerate(frame):
            if previous is None:
                row.append(0.0)
                continue
            a = boxes[len(out) - 1][k]
            x0, y0, x1, y1 = min(x0, a[0]), min(y0, a[1]), max(x1, a[2]), max(y1, a[3])
            row.append(float(np.abs(image[y0:y1, x0:x1] - previous[y0:y1, x0:x1]).mean()) if x1 > x0 and y1 > y0 else 0.0)
        out.append(row)
        previous = image
    stream.stdout.close()
    stream.wait()
    return out


def scores(values: list[float]) -> list[float]:
    medians = [float(np.median(values[max(1, i - WINDOW):i + WINDOW + 1])) for i in range(len(values))]
    return [value / median_ if median_ > 0 else 0.0 for value, median_ in zip(values, medians)]


def standout(score: list[float], frame: int, ratio: float) -> tuple[int, float, bool]:
    """The highest score within NEAR frames, the highest one RING frames away, and whether the first is a jump."""
    near = range(max(1, frame - NEAR), min(len(score), frame + NEAR + 1))
    peak = max(near, key=lambda i: score[i])
    around = max((score[i] for i in range(frame - RING[1], frame + RING[1] + 1)
                  if RING[0] <= abs(i - frame) and 0 < i < len(score)), default=0.0)
    return peak, around, score[peak] >= ratio and score[peak] > around


def crossings(heights: list[float], thresholds) -> list[tuple[str, float, int]]:
    """The frames where the hard switches of the old scheme happen: falling below a threshold on the way out, rising
    to 1.1 times it on the way back."""
    lowest = heights.index(min(heights))
    out = []
    for threshold in thresholds:
        down = next((i for i in range(1, lowest + 1) if heights[i] < threshold <= heights[i - 1]), None)
        if down is not None:
            out.append(('out', threshold, down))
        rise = threshold * HYSTERESIS
        up = next((i for i in range(lowest + 1, len(heights)) if heights[i] >= rise > heights[i - 1]), None)
        if up is not None:
            out.append(('back', rise, up))
    return out


def report(content: Path, run: Path, table: Path, layer: str, visuals: list[str], ratio: float) -> list[dict]:
    with table.open(newline='') as stream:
        rows = list(csv.DictReader(stream))
    names = [visual.rsplit('_', 1)[-1].lower() for visual in visuals]
    heights = [[float(row[f'{name}_height_px']) for row in rows] for name in names]
    stages = [[int(row[f'{name}_stage']) for row in rows] for name in names]
    cameras, target = eyes(run)
    if len(cameras) != len(rows):
        raise ValueError(f'{len(cameras)} camera samples for {len(rows)} rows of {table}')
    trees = tree_bounds(content, layer, visuals)
    boxes = [[box(eye, target, middle, height) for middle, height, _ in trees] for eye in cameras]
    diff = differences(run / f'{NAME}.mp4', boxes)
    result = []
    for k, name in enumerate(names):
        score = scores([row[k] for row in diff])
        jumps = [i for i in range(1, len(score))
                 if stages[k][i] < 2 and standout(score, i, ratio)[0]==i and standout(score, i, ratio)[2]]
        print(f'{name}: {len(jumps)} jumps while in 3D')
        for i in jumps:
            print(f'  frame {i}: {heights[k][i]:.1f} px, x{score[i]:.2f}, {diff[i][k] - diff[i][k] / score[i]:+.3f} grey')
        for way, threshold, frame in crossings(heights[k], (SHADOW_PX, *trees[k][2][:-1])):
            peak, around, jump = standout(score, frame, ratio)
            print(f'  {way} {threshold:g} px at frame {frame}: peak x{score[peak]:.2f} at {peak} '
                  f'({heights[k][peak]:.1f} px), around x{around:.2f}: {"JUMP" if jump else "none"}')
            result.append({'tree': name, 'way': way, 'threshold': threshold, 'frame': frame, 'peak': score[peak],
                           'around': around, 'jump': jump})
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('content', type=Path)
    parser.add_argument('run', type=Path)
    parser.add_argument('csv', type=Path)
    parser.add_argument('--layer', default='fir-bough-stagger')
    parser.add_argument('--visuals', nargs='+', required=True)
    parser.add_argument('--ratio', type=float, default=1.5)
    args = parser.parse_args()
    report(args.content, args.run, args.csv, args.layer, args.visuals, args.ratio)
