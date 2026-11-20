#!/usr/bin/env python3
"""Write per-frame heights, impostor joint stages and hierarchy counters for a fir-bough-lod recording.

    python3 scripts/content/fir_bough_frames.py <set> <bench output> <csv> --visuals NAME...

The heights follow the marked camera path as in fir_eye_frames.py. Stage 0 is the 3D crown alone, 1 the joint
fade with the impostor (46 to 56 px for a 49 px impostor, TreeBough::impostorJointWeight) and 2 the impostor
alone. Recorded frames are the first run of frameprofile.csv rows with outcome 3 (later single rows are the
stills after the recording). The CPU counter mesh_impostors belongs to
its own row; the GPU tree_bough_* counters arrive a few frames later, and the delay is taken where their
largest height best matches the path.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
import subprocess

from fir_eye_frames import FOV_DEGREES, camera_path, tree_bounds

NAME = 'fir-bough-lod'
SCENARIO = Path(__file__).resolve().parents[1] / 'bench' / f'{NAME}.bench'
GPU = ('tree_bough_nodes', 'tree_bough_source_cards', 'tree_bough_parent_cards', 'tree_bough_triangles',
       'tree_bough_height_mpx', 'tree_bough_active_bands', 'tree_bough_version_pairs', 'tree_bough_band_triangles')


def stage(px: float, impostor_px: float) -> int:
    scale = impostor_px / 49
    return 0 if px >= 56 * scale else 1 if px > 46 * scale else 2


def write_frames(content: Path, run: Path, output: Path, layer: str, visuals: list[str]) -> list[list[int]]:
    events = [json.loads(line) for line in (run / 'events.jsonl').read_text().splitlines()]
    events = [event for event in events if event['seg'] == NAME]
    (record,) = [event for event in events if event['kind'] == 'rec']
    (stop,) = [event for event in events if event['kind'] == 'rec_stop']
    positions, durations, starts = camera_path(SCENARIO, events, 'fir-bough')
    frames = int(subprocess.check_output([
        'ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries', 'stream=nb_frames',
        '-of', 'csv=p=0', run / f'{NAME}.mp4']))
    with (run / 'frameprofile.csv').open(newline='') as stream:
        rows = [row['outcome'] == '3' for row in csv.DictReader(stream)]
        stream.seek(0)
        first = rows.index(True)
        count = rows[first:].index(False) if False in rows[first:] else len(rows) - first
        profile = list(csv.DictReader(stream))[first:first + count]
    if not frames == stop['frames'] == len(profile):
        raise ValueError(f'{frames} video frames, {stop["frames"]} recorded, {len(profile)} profile rows')
    focal_px = 1080 / (2 * math.tan(math.radians(FOV_DEGREES / 2)))
    trees = tree_bounds(content, layer, visuals)
    fps = record['fps']
    heights, leg = [], -1
    eye = origin = positions[0]
    with (run / 'metrics.csv').open(newline='') as stream:
        for sample in csv.DictReader(stream):
            if sample['seg'] != NAME:
                continue
            time = record['t'] + int(sample['t'])
            # Commands run before the camera update, so the next fly starts at the previous frame's eye.
            if leg + 1 < len(starts) and time == starts[leg + 1]:
                origin = eye
                leg += 1
            if leg >= 0:
                progress = min(1, (time - starts[leg]) / durations[leg])
                eye = tuple(a + (b - a) * progress for a, b in zip(origin, positions[leg + 1]))
            heights.append([height * focal_px / max(1, math.dist(middle, eye)) for middle, height, _ in trees])
    if len(heights) != frames:
        raise ValueError('Metrics do not cover all recorded frames')
    stages = [[stage(px, levels[-1]) for px, (_, _, levels) in zip(row, trees)] for row in heights]
    mismatched = sum(int(profile[i]['mesh_impostors']) != sum(s > 0 for s in row) for i, row in enumerate(stages))

    def error(delay):
        return sum(abs(int(profile[i + delay]['tree_bough_height_mpx']) / 1000 - max(heights[i]))
                   for i in range(frames - delay)) / (frames - delay)
    delay = min(range(6), key=error)
    with output.open('w', newline='') as stream:
        writer = csv.writer(stream)
        names = [visual.rsplit('_', 1)[-1].lower() for visual in visuals]
        writer.writerow(('frame', 'time_s', *(f'{name}_height_px' for name in names),
                         *(f'{name}_stage' for name in names), 'mesh_impostors', *GPU))
        for i in range(frames):
            gpu = [profile[i + delay][key] for key in GPU] if i + delay < frames else [''] * len(GPU)
            writer.writerow((i, f'{i / fps:.6f}', *(f'{px:.3f}' for px in heights[i]), *stages[i],
                             profile[i]['mesh_impostors'], *gpu))
    print(f'{frames} frames at {fps} fps; GPU counters {delay} frames late, mean height error {error(delay):.3f} px; '
          f'impostor count differs from the stages on {mismatched} frames')
    for k, name in enumerate(names):
        changes = [i for i in range(1, frames) if stages[i][k] != stages[i - 1][k]]
        print(name, ' '.join(f'{stages[i][k]}@{i}' for i in changes))
    return stages


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('content', type=Path)
    parser.add_argument('run', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--layer', default='fir-bough-stagger')
    parser.add_argument('--visuals', nargs='+', required=True)
    args = parser.parse_args()
    write_frames(args.content, args.run, args.output, args.layer, args.visuals)
