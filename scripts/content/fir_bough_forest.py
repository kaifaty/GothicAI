#!/usr/bin/env python3
"""Forecast the foliage submitted by spruce hierarchies in the four cameras of perf-trees.bench.

    python3 scripts/content/fir_bough_forest.py <forest set> <forecast.json> <hierarchy.json>...

The forest set is `fir_set.py forest --hierarchy` (1,024 spruces A, their positions, yaw and scale in the
world override). Each spruce whose bounding sphere meets the 2 560 × 1 440 frustum (vertical 67.5°) and whose
height on screen exceeds the impostor's lower joint edge submits the engine's selection
(TreeBough::selectNode): nodes measured at the nearest corner of their box, a fading parent together with the
nearest banded or original descendants. Occlusion is not modelled. The first hierarchy is the one measured in
the forest by PR 1653; its measured triangles per frame in each view (the forecast JSON of PR 1658) calibrate
the others by the same ratio.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np

SCENARIO = Path(__file__).resolve().parents[1] / 'bench' / 'perf-trees.bench'
VISUAL = 'OWN_TREE_FIR_A_4.3DS'
WIDTH, HEIGHT, FOV_DEGREES = 2560, 1440, 67.5
JOINT_LOW = 46 / 49  # of the impostor's px


def gothic(points):
    """Generator Z-up metres to the engine's mirrored-X centimetres (TreeBough point())."""
    points = np.asarray(points, dtype=np.float64)
    return np.stack((-100 * points[..., 0], 100 * points[..., 2], -100 * points[..., 1]), axis=-1)


def read_hierarchy(path: Path):
    saved = json.loads(path.read_text())
    nodes = saved['nodes']
    count = len(nodes)
    parent = np.full(count, -1)
    for node in nodes:
        for child in node['children']:
            parent[child - 1] = node['id'] - 1
    corners = gothic([node['box'] for node in nodes])
    low, high = corners.min(axis=1), corners.max(axis=1)
    triangles = np.array([node['geometry']['index_count'] // 3 if node['geometry'] else 0 for node in nodes])
    bands = np.array([(node['low_px'], node['high_px']) for node in nodes])
    leaf = np.array([not node['children'] for node in nodes])
    return dict(parent=parent, low=low, high=high, diameter=np.linalg.norm(high - low, axis=1),
                triangles=triangles, bands=bands, leaf=leaf, height_cm=float(100 * saved['source']['height_m']))


def forest(path: Path):
    records = json.loads((path / 'worlds/NEWWORLD/vobs.json').read_text())['vobs']
    (source,) = [record for record in records if record.get('visual', {}).get('name') == VISUAL]
    trees = [record for record in records if record is source
             or record.get('visual') == {'ref': source['visual']['object_id']}]
    return np.array([record['position'] for record in trees]), np.array([record['rotation'] for record in trees])


def cameras():
    lines = [line.split() for line in SCENARIO.read_text().splitlines() if line.startswith('cam at ')]
    return [np.array(line[2:8], dtype=float).reshape(2, 3) for line in lines]


def submitted(hierarchy, positions, rotations, eye, aim, impostor_px):
    forward = (aim - eye) / np.linalg.norm(aim - eye)
    focal = HEIGHT / 2 / math.tan(math.radians(FOV_DEGREES / 2))
    scale = np.linalg.norm(rotations[:, :, 1], axis=1)
    centre_local = (hierarchy['low'].min(axis=0) + hierarchy['high'].max(axis=0)) / 2
    radius = np.linalg.norm(hierarchy['high'].max(axis=0) - hierarchy['low'].min(axis=0)) / 2 * scale
    centre = positions + np.einsum('tij,j->ti', rotations, centre_local)
    relative = centre - eye
    depth = relative @ forward
    right = np.cross(forward, (0, 1, 0))
    right /= np.linalg.norm(right)
    up = np.cross(right, forward)
    half = math.tan(math.radians(FOV_DEGREES / 2))
    inside = depth + radius > 0
    for axis, tangent in ((right, half * WIDTH / HEIGHT), (up, half)):
        # Distance from the sphere centre to each side plane of the frustum.
        offset = np.abs(relative @ axis) - tangent * depth
        inside &= offset * math.cos(math.atan(tangent)) < radius
    tree_px = hierarchy['height_cm'] * scale * focal / np.linalg.norm(relative, axis=1)
    keep = inside & (tree_px > JOINT_LOW * impostor_px)
    rotations, positions, scale = rotations[keep], positions[keep], scale[keep]
    local_forward = np.einsum('tji,j->ti', rotations, forward)
    origin = (positions - eye) @ forward
    count = len(hierarchy['parent'])
    mode = np.zeros((count, len(positions)), np.int8)  # 0 traverse, 1 descendant side, 2 done
    nodes = np.zeros(len(positions))
    triangles = np.zeros(len(positions))
    low, high = hierarchy['bands'].T
    for i in range(count - 1, -1, -1):
        parent = hierarchy['parent'][i]
        incoming = mode[parent] if parent >= 0 else np.zeros(len(positions), np.int8)
        if incoming.max() == 2 and parent >= 0 and np.all(incoming == 2):
            mode[i] = 2
            continue
        live = incoming < 2
        if hierarchy['leaf'][i]:
            emit = live
            out = np.full(len(positions), 2, np.int8)
        elif high[i] > 0:
            nearest = np.where(local_forward < 0, hierarchy['high'][i], hierarchy['low'][i])
            px = hierarchy['diameter'][i] * scale * focal / np.maximum(origin + np.sum(local_forward * nearest, axis=1), 1)
            t = np.clip((px - low[i]) / (high[i] - low[i]), 0, 1)
            weight = 1 - t * t * (3 - 2 * t)
            emit = live & ((incoming == 1) | (weight > 0))
            out = np.where(incoming == 1, 2, np.where(weight >= 1, 2, np.where(weight > 0, 1, 0))).astype(np.int8)
            out[~live] = 2
        else:
            emit = np.zeros(len(positions), bool)
            out = incoming
        mode[i] = out
        nodes += emit
        triangles += emit * hierarchy['triangles'][i]
    return dict(crowns=int(keep.sum()), nodes=float(nodes.sum()), triangles=float(triangles.sum()))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('forest', type=Path)
    parser.add_argument('forecast', type=Path, help='forest-forecast.json with the measured counters')
    parser.add_argument('hierarchies', type=Path, nargs='+')
    parser.add_argument('--impostor-px', type=float, default=49.)
    args = parser.parse_args()
    positions, rotations = forest(args.forest)
    measured = [view['measured'] for view in json.loads(args.forecast.read_text())['views']]
    cold = [view['cold_frustum'] for view in json.loads(args.forecast.read_text())['views']]
    hierarchies = [read_hierarchy(path) for path in args.hierarchies]
    report = []
    for number, ((eye, aim), counted, frustum) in enumerate(zip(cameras(), measured, cold, strict=True), 1):
        rows = [submitted(h, positions, rotations, eye, aim, args.impostor_px) for h in hierarchies]
        ratio = counted['tree_bough_triangles'] / rows[0]['triangles']
        for path, row in zip(args.hierarchies, rows):
            row.update(hierarchy=path.name, forecast_triangles=row['triangles'] * ratio,
                       forecast_nodes=row['nodes'] * counted['tree_bough_nodes'] / rows[0]['nodes'])
        report.append(dict(view=number, calibration=ratio,
                           previous_cold=dict(nodes=frustum['tree_bough_nodes'],
                                              triangles=frustum['tree_bough_triangles']), rows=rows))
        print(json.dumps(report[-1]))


if __name__ == '__main__':
    main()
