#!/usr/bin/env python3
"""Write the per-frame mesh-level selection for a completed fir-eye-threshold recording."""
from __future__ import annotations

import argparse
import csv
from fractions import Fraction
import json
import math
from pathlib import Path
import subprocess

from world.chunk_glb import read_glb

NAME = 'fir-eye-threshold'
SCENARIO = Path(__file__).resolve().parents[1] / 'bench' / f'{NAME}.bench'
TREES = ('control', 'k4', 'k8', 'k16')
VISUALS = tuple(f'OWN_TREE_FIR_A_EYE_{suffix}' for suffix in ('CONTROL', '4', '8', '16'))
FOV_DEGREES = 67.5
HYSTERESIS = 1.1


def camera_path(scenario: Path, events: list[dict]) -> tuple[list[tuple[float, ...]], list[int], list[int]]:
    """Use the bench's game-time marks to match each linear cam fly to recorded frames."""
    commands = [line.split() for line in scenario.read_text().splitlines()
                if line.startswith(('cam at ', 'cam fly '))]
    positions = [tuple(map(float, command[2:5])) for command in commands]
    durations = [int(command[8]) for command in commands[1:]]
    starts = [event['t'] for event in events if event['kind'] == 'mark'
              and event['text'] == 'fir-eye-leg']
    returned = [event['t'] for event in events if event['kind'] == 'mark'
                and event['text'] == 'fir-eye-returned']
    if not starts or len(starts) != len(durations) or returned != [starts[-1] + durations[-1]]:
        raise ValueError('Recording did not complete the marked round trip')
    if positions != positions[::-1]:
        raise ValueError('Return camera path differs from the outbound path')
    return positions, durations, starts


def tree_bounds(content: Path) -> list[tuple[tuple, float, list[float]]]:
    placements = json.loads((content / 'worlds/NEWWORLD/vobs.d' / f'{NAME}.json').read_text())
    by_visual = {tree['visual']: tree for tree in placements['add']}
    mesh_dir = content / 'meshes'
    records = json.loads((mesh_dir / 'meshes.json').read_text())
    bounds = []
    for visual in VISUALS:
        tree, record = by_visual[visual], records[visual]
        if tree['yaw_deg'] != 0:
            raise ValueError(f'{visual}: this camera fixture uses yaw 0')
        # The own-mesh loader mirrors glTF X and converts meters to Gothic centimeters.
        document, _ = read_glb(mesh_dir / record['glb'])
        boxes = [document['accessors'][primitive['attributes']['POSITION']]
                 for primitive in document['meshes'][0]['primitives']]
        low = [min(box['min'][axis] for box in boxes) for axis in range(3)]
        high = [max(box['max'][axis] for box in boxes) for axis in range(3)]
        middle = tuple(root + unit * (lo + hi) * .5 for root, unit, lo, hi
                       in zip(tree['position'], (-100, 100, 100), low, high))
        height = 100 * (high[1] - low[1])
        levels = record['levels']
        if len(levels) != 5 or 'impostor' not in levels[-1]:
            raise ValueError(f'{visual}: expected four GLB levels and the impostor')
        bounds.append((middle, height, [level['px'] for level in levels]))
    return bounds


def write_frames(content: Path, run: Path, output: Path) -> None:
    events = [json.loads(line) for line in (run / 'events.jsonl').read_text().splitlines()]
    events = [event for event in events if event['seg'] == NAME]
    (record,) = [event for event in events if event['kind'] == 'rec']
    (stop,) = [event for event in events if event['kind'] == 'rec_stop']
    positions, durations, starts = camera_path(SCENARIO, events)
    video = json.loads(subprocess.check_output([
        'ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
        'stream=width,height,avg_frame_rate,nb_frames', '-of', 'json', run / f'{NAME}.mp4',
    ]))['streams'][0]
    fps = record['fps']
    if ((video['width'], video['height']) != (1920, 1080) or Fraction(video['avg_frame_rate']) != fps
            or int(video['nb_frames']) != stop['frames']):
        raise ValueError('Video dimensions, frame rate or frame count disagree with the recording')
    focal_px = video['height'] / (2 * math.tan(math.radians(FOV_DEGREES / 2)))
    trees = tree_bounds(content)
    stages = [0] * len(trees)
    switches = [[] for _ in trees]
    rows = []
    leg = -1
    eye = origin = positions[0]
    with (run / 'metrics.csv').open(newline='') as stream:
        for sample in csv.DictReader(stream):
            if sample['seg'] != NAME:
                continue
            frame, time = int(sample['frame']), record['t'] + int(sample['t'])
            if frame != len(rows):
                raise ValueError(f'Missing or repeated frame in metrics.csv at {frame}')
            # Commands run before the camera update, so the next fly starts at the previous frame's eye.
            if leg + 1 < len(starts) and time == starts[leg + 1]:
                origin = eye
                leg += 1
            if leg >= 0:
                progress = min(1, (time - starts[leg]) / durations[leg])
                eye = tuple(a + (b - a) * progress for a, b in zip(origin, positions[leg + 1]))
            heights = []
            for k, (middle, height, thresholds) in enumerate(trees):
                px = height * focal_px / max(1, math.dist(middle, eye))
                heights.append(f'{px:.4f}')
                # MeshLevels::pick enters below the threshold and returns at 1.1 times the threshold.
                stage = stages[k]
                while stage < len(thresholds) and px < thresholds[stage]:
                    stage += 1
                while stage > 0 and px >= HYSTERESIS * thresholds[stage - 1]:
                    stage -= 1
                if stage != stages[k]:
                    switches[k].append((stage, frame))
                    stages[k] = stage
            rows.append((frame, f'{frame / fps:.6f}', *heights, *stages))
    if len(rows) != stop['frames'] or leg != len(starts) - 1:
        raise ValueError('Metrics do not cover all recorded frames or camera marks')
    for name, changes in zip(TREES, switches):
        if [stage for stage, _ in changes] != [1, 2, 3, 4, 5, 4, 3, 2, 1, 0]:
            raise ValueError(f'{name}: expected a passage through every stage in both directions: {changes}')
        smallest_gap = min(b - a for (_, a), (_, b) in zip(changes, changes[1:]))
        if smallest_gap < 2 * fps:
            raise ValueError(f'{name}: only {smallest_gap} frames between level changes')
    with output.open('w', newline='') as stream:
        writer = csv.writer(stream)
        writer.writerow(('frame', 'time_s', *(f'{name}_height_px' for name in TREES),
                         *(f'{name}_level' for name in TREES)))
        writer.writerows(rows)
    print(f'{len(rows)} frames at {fps} fps, levels 0–4 and impostor 5')
    for name, changes in zip(TREES, switches):
        print(name, ' '.join(f'{stage}@{frame}' for stage, frame in changes))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('content', type=Path)
    parser.add_argument('run', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    write_frames(args.content, args.run, args.output)
