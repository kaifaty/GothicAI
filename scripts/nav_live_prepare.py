#!/usr/bin/env python3
"""Prepare read-only geometry and a native market census inside one gq job."""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import shutil
from types import SimpleNamespace

from nav_live import read_json, sha256, suite_cases, write_json
from nav_teacher_v3_campaign import Worker
from nav_teacher_v3 import isolated, legs


def rows(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def test_pairs(inputs):
    # Read split metadata only to exclude test routes before any physical query.
    root = Path(inputs['roads']).parent
    manifest = read_json(root / 'v4-manifest.json')
    excluded = set()
    for cohort in manifest['splits']['test'].values():
        for case in cohort:
            excluded.update(legs(case))
    for case in rows(root / 'detour-test.jsonl'):
        excluded.update(legs(case))
    return excluded


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--suite', type=Path, required=True)
    args = parser.parse_args(argv)
    suite = read_json(args.suite)
    cases = suite_cases(suite)
    inputs = suite['inputs']
    excluded_tests = test_pairs(inputs)
    if any(not isolated(case, excluded_tests) for case in cases):
        raise ValueError('A live candidate overlaps a protected test route')
    output = args.output / 'preparation'
    output.mkdir(parents=True, exist_ok=True)
    identity = dict(suite_sha256=sha256(args.suite),
                    runner_sha256=sha256(args.runtime / 'build/opengothic/gothic-body-runner'))
    if (output / 'complete.json').is_file():
        if read_json(output / 'complete.json').get('identity') != identity:
            raise ValueError('Completed preparation belongs to different inputs')
        print('Geometry and market census are already complete.')
        return
    # Copy only immutable mesh files; this task owns every cache it writes.
    cache = args.runtime / 'navmesh-cache'
    cache.mkdir(parents=True, exist_ok=True)
    source = Path('/mnt/shared/gothic-ai/archive/nav-dagger3-20261030/hd-measure-cache/ordinary')
    for path in source.glob('*.navmesh'):
        target = cache / path.name
        if not target.exists():
            shutil.copyfile(path, target)
    options = SimpleNamespace(output=args.runtime / 'preparation-worker',
        runner=args.runtime / 'build/opengothic/gothic-body-runner',
        assets=Path(os.environ['GOTHIC_DIR']), save=Path(inputs['save']), cache=cache,
        links=Path(inputs['links']), roads=Path(inputs['roads']), npc='BAU_954_MALETH',
        ini=None, seed=suite['seeds'][0], physics='cv', watchdog_seconds=900, limit_ms=180000)
    worker = Worker(options, 3, None)
    try:
        geometry = output / 'geometry'
        if not (geometry / 'detour-plans-v3.jsonl').is_file():
            plans = [(case['id'], case['start'], case['goal']) for case in cases]
            # New starts around the shipyard wall, never a test cohort. Results
            # are candidates only; no route is frozen from geometry alone.
            for x in (700, 900, 1100, 1300, 1500):
                for z in (600, 900, 1200, 1500, 1800):
                    start, goal = f'cm:{x},-186,{z}', 'FP_PICK_CITY_01'
                    if (start, goal) not in excluded_tests and (goal, start) not in excluded_tests:
                        plans.append((f'pocket-{x}-{z}', start, goal))
            path = output / 'geometry-candidates.txt'
            path.write_text(''.join(f'{label} {start} {goal}\n' for label, start, goal in plans))
            worker.scenario('nav-live-geometry', ['navdemos record off',
                f'roads load {inputs["roads"]}', 'roads actors', 'navdemos points-v3',
                f'navdemos detour-plans-v3 {path}'], geometry, suite['seeds'][0])
        points = {row['name']: row['position_cm'] for row in rows(geometry / 'points-v3.jsonl')}
        water_dir = output / 'water'
        if not (water_dir / 'water-probes.jsonl').is_file():
            train = rows(Path('/mnt/shared/gothic-ai/nav-demos-v5/input/water-scenarios.jsonl'))
            excluded = {(row['start'], row['goal']) for row in train} | excluded_tests
            names = [name for name in sorted(points) if name.startswith('NW_CITY_HABOUR_')
                     and any(part in name for part in ('06_', 'HUT_', 'SHIP_', 'POOR_AREA', 'KASERN_'))]
            # Source is v5's water-probe geometry. Previously used train pairs
            # are excluded; dry endpoints and direct water are measured natively.
            candidates = []
            for start in names:
                for goal in names:
                    first, last = points[start], points[goal]
                    distance = math.hypot(first[0] - last[0], first[2] - last[2])
                    if (start >= goal or (start, goal) in excluded or (goal, start) in excluded
                            or not 800 <= distance <= 4500 or abs(first[1] - last[1]) > 500):
                        continue
                    candidates.append((start, goal))
            # A fixed lexical sample is independent of all teacher/policy outcomes.
            if len(candidates) > 180:
                candidates = [candidates[index * len(candidates) // 180] for index in range(180)]
            path = output / 'water-candidates.txt'
            path.write_text(''.join(f'harbour-{index:03d} {start} {goal}\n'
                                    for index, (start, goal) in enumerate(candidates)))
            worker.scenario('nav-live-water', ['navdemos record off',
                f'navdemos water-probes {path}'], water_dir, suite['seeds'][0])
        census = []
        center = points['NW_CITY_MERCHANT_PATH_20']
        for hour in range(24):
            directory = output / 'market' / f'{hour:02d}'
            if not (directory / 'roads-events.jsonl').is_file():
                worker.scenario(f'nav-live-market-{hour:02d}', [f'roads load {inputs["roads"]}', f'set time {hour} 59',
                    'goto NW_CITY_MERCHANT_PATH_20', 'wait 4000', 'roads actors',
                    'wait 10000', 'roads actors'], directory, suite['seeds'][0])
            snapshots = [row['actors'] for row in rows(directory / 'roads-events.jsonl')
                         if row['kind'] == 'roads_actors']
            if len(snapshots) != 2:
                raise ValueError(f'Market census needs exactly two snapshots: {directory}')
            before = {row['npc_id']: row for row in snapshots[0]}
            walking = []
            for row in snapshots[1]:
                previous = before.get(row['npc_id'])
                if not previous or not row['routines'] or row['instance'] == 'PC_HERO':
                    continue
                point, first = row['position_cm'], previous['position_cm']
                if (math.hypot(point[0] - center[0], point[2] - center[2]) <= 3000
                        and math.hypot(point[0] - first[0], point[2] - first[2]) >= 50):
                    walking.append(row['instance'])
            census.append(dict(time=[hour, 59], walking=len(walking), actors=walking))
        peak = max(census, key=lambda row: (row['walking'], -row['time'][0]))
        write_json(output / 'market-census.json', dict(radius_cm=3000, settle_ms=4000,
            observation_ms=10000, minimum_displacement_cm=50, hours=census, peak=peak))
        write_json(output / 'complete.json', dict(complete=True, identity=identity,
            geometry_cases=len(rows(geometry / 'detour-plans-v3.jsonl')),
            water_candidates=len(rows(water_dir / 'water-probes.jsonl')), market_hours=len(census),
            teacher_qualification=False, baseline_runs=0))
        print(json.dumps(read_json(output / 'complete.json')))
    finally:
        worker.close()


if __name__ == '__main__':
    main()
