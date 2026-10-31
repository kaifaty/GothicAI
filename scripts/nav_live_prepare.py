#!/usr/bin/env python3
"""Prepare read-only geometry and a native market census inside one gq job."""
from __future__ import annotations

import argparse
from copy import deepcopy
import hashlib
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


def position(token, points):
    return [float(value) for value in token[3:].split(',')] if token.startswith('cm:') else points[token]


def nearest_routine(case, actors, points):
    actor = next((row for row in actors if row['instance'] == case['npc']), None)
    if actor is None:
        raise ValueError(f"No saved native actor for {case['npc']}")
    names = sorted({row['point'] for row in actor['routines'] if row['point'] in points})
    if not names:
        raise ValueError(f"No measured own routine point for {case['npc']}")
    start = position(case['start'], points)
    goal = min(names, key=lambda name: (math.dist(start, points[name]), name))
    return dict(case, goal=goal, routine_point_selection=dict(method='nearest own routine point, 3D',
        candidates=names, chosen=goal, distance_cm=math.dist(start, points[goal])))


def water_pool(base, points, excluded, forbidden_names):
    names = [name for name in sorted(points) if name.startswith('NW_CITY_')
             and (any(name.startswith('NW_CITY_HABOUR_' + part) for part in
                      ('06', '11', '12', 'SHIP_01', 'POOR_AREA_PATH'))
                  or any(part in name for part in ('WAY_TO_SHIP', 'FISCHER', 'SHIP_GUARD')))
             and name not in forbidden_names
             and not any(part in name for part in
                         ('SHIP_DECK', 'SHIP_IN', 'BED', '_IN', '_COOK', '_SIT', 'TAVERN', 'PUFF'))]
    pairs = []
    for index, start in enumerate(names):
        for goal in names[index + 1:]:
            first, last = points[start], points[goal]
            distance = math.hypot(first[0] - last[0], first[2] - last[2])
            if ((start, goal) in excluded or (goal, start) in excluded
                    or not 800 <= distance <= 18000 or abs(first[1] - last[1]) > 600):
                continue
            pairs.append((start, goal))
    if len(pairs) > 400:
        pairs = [pairs[index * len(pairs) // 400] for index in range(400)]
    return [dict(base, start=start, goal=goal, probe_label=f'water-{index:03d}')
            for index, (start, goal) in enumerate(pairs)]


def supported_route(probe):
    return (probe['detour_complete'] and probe['route_sample_count'] > 1
            and probe['route_deep_water_samples'] == 0 and probe['route_no_floor_samples'] == 0
            and all(probe[field]['supported'] and probe[field]['dry'] and probe[field]['capsule_clear'] and probe[field]['ground_known']
                    and abs(probe[field]['height_delta_cm']) <= 20
                    and probe[field]['ground_snap_cm'] <= 75 for field in ('start_support', 'goal_support')))


def qualify_probe(number, probe):
    if not supported_route(probe):
        return False
    obstruction = probe['direct_obstruction']
    if number == 4:
        material = obstruction['material_name'].lower()
        named_rock = any(token in obstruction['label'].upper() for token in ('ROCK', 'FELS', 'CLIFF'))
        return (probe['goal_visible'] and obstruction['hit'] and
                ((named_rock and probe['direct_sensor']['kind'] in ('world', 'static_vob'))
                 or (obstruction['world_hit'] and probe['direct_sensor']['kind'] == 'world' and material == 'stone')))
    if number == 5:
        material = obstruction['material_name'].lower()
        return (probe['detour_ratio'] is not None and probe['detour_ratio'] >= 2
                and probe['first_detour_goal_dot'] is not None and probe['first_detour_goal_dot'] < -.2
                and obstruction['hit'] and obstruction['normal_y'] is not None
                and abs(obstruction['normal_y']) <= .35 and material not in ('water', 'none', 'undefined', 'unknown'))
    if number == 6:
        return probe['direct_deep_water_samples'] > 0
    if number == 7:
        return (probe['route_min_edge_clearance_cm'] is not None
                and probe['route_min_edge_clearance_cm'] < 200 and probe['route_edge_clearance_censored'] is False
                and probe['probe']['edge_drop_cm'] >= 100)
    return True


def probe_groups(worker, cases, output, seed):
    measured = {}
    for npc in sorted({case['npc'] for case in cases}):
        group = [case for case in cases if case['npc'] == npc]
        text = ''.join(f"{case['probe_label']} {case['start']} {case['goal']}\n" for case in group)
        signature = hashlib.sha256(text.encode()).hexdigest()[:12]
        directory = output / 'native' / f'{npc}-{signature}'
        directory.mkdir(parents=True, exist_ok=True)
        source = directory / 'candidates.txt'
        if source.is_file() and source.read_text() != text:
            raise ValueError(f'Native probe inputs changed at {directory}')
        source.write_text(text)
        path = directory / 'live-probes.jsonl'
        expected = {case['probe_label']: case for case in group}
        try:
            probes = rows(path)
            complete = len(probes) == len(expected) and {row['label'] for row in probes} == set(expected)
        except (OSError, ValueError, KeyError):
            complete = False
        if not complete:
            # Only this incomplete native-probe group is repeated. Complete
            # groups and recorded teacher episodes remain untouched.
            path.unlink(missing_ok=True)
            worker.scenario(f'nav-live-probes-{npc}-{signature}', ['navdemos record off',
                f'navdemos live-probes {npc} {source}'], directory, seed)
        probes = rows(path)
        if len(probes) != len(expected) or {row['label'] for row in probes} != set(expected):
            raise ValueError(f'Incomplete native probe group: {directory}')
        for probe in probes:
            case = expected[probe['label']]
            if any(probe[field] != case[field] for field in ('start', 'goal', 'npc')):
                raise ValueError(f'Native probe belongs to another place: {probe["label"]}')
            measured[probe['label']] = dict(probe=probe, file=str(path))
    return measured


def qualify(args):
    from nav_live import check_space, phase_deadline, report, teacher_outcome, teachers
    suite = read_json(args.suite)
    inputs = suite['inputs']
    for name in ('save', 'roads', 'familiar', 'links'):
        if not Path(inputs[name]).is_file():
            raise ValueError(f'Missing frozen input {name}: {inputs[name]}')
    check_space(args.output)
    preparation = args.output / 'preparation'
    if not read_json(preparation / 'complete.json').get('complete'):
        raise ValueError('Complete the initial geometry and census preparation first')
    geometry = preparation / 'geometry'
    points = {row['name']: row['position_cm'] for row in rows(geometry / 'points-v3.jsonl')}
    actors = next(row['actors'] for row in rows(geometry / 'roads-events.jsonl') if row['kind'] == 'roads_actors')
    excluded = test_pairs(inputs)
    root = Path(inputs['roads']).parent
    manifest = read_json(root / 'v4-manifest.json')
    places = read_json(inputs['roads'])
    test_places = set(manifest['split_policy']['object']['groups']['test'])
    forbidden_names = {name for name, index in places['points'].items()
                       if places['places'][index]['id'] in test_places}
    train_water = rows(root / 'water-scenarios.jsonl')
    water_excluded = excluded | {(row['start'], row['goal']) for row in train_water}
    cases = suite_cases(suite)
    selected = {}
    candidates = []
    for case in cases:
        if case['number'] == 2:
            case = nearest_routine(case, actors, points)
        if case['number'] == 6:
            choices = water_pool(case, points, water_excluded, forbidden_names)
        elif case['number'] == 4:
            validation = rows(root / 'detour-val.jsonl')
            if any(row['split'] != 'validation' for row in validation):
                raise ValueError('Detour alternatives must contain validation places only')
            ratio = lambda row: row['geometry']['length_cm'] / math.dist(
                row['geometry']['start_cm'], row['geometry']['goal_cm'])
            validation.sort(key=lambda row: (-ratio(row), row['id']))
            choices = [dict(case, start=row['start'], goal=row['goal'], goal_kind=row['goal_kind'],
                probe_label=case['id'] + '--' + row['id'],
                source=dict(dataset='detour-val', id=row['id'], place_id=row.get('place_id'), detour_ratio=ratio(row),
                            file=str(root / 'detour-val.jsonl'))) for row in validation]
        else:
            alternatives = case.get('qualification_candidates') or [{}]
            choices = [dict(case, **alternative, probe_label=case['id'] + '--' + alternative.get('label', 'primary'))
                       for alternative in alternatives]
        for choice in choices:
            if not isolated(choice, excluded):
                raise ValueError(f"A live qualification candidate overlaps a protected test route: {choice['id']}")
        candidates.extend(choices)
    binary = args.runtime / 'build/opengothic/gothic-body-runner'
    output = args.output / 'qualification' / f'{sha256(args.suite)[:12]}-{sha256(binary)[:12]}'
    output.mkdir(parents=True, exist_ok=True)
    options = SimpleNamespace(output=args.runtime / 'qualification-worker', runner=binary,
        assets=Path(os.environ['GOTHIC_DIR']), save=Path(inputs['save']), cache=args.runtime / 'navmesh-cache',
        links=Path(inputs['links']), roads=Path(inputs['roads']), npc='BAU_954_MALETH',
        ini=None, seed=suite['seeds'][0], physics='cv', watchdog_seconds=900, limit_ms=180000)
    worker = Worker(options, 3, None)
    try:
        measured = probe_groups(worker, candidates, output, suite['seeds'][0])
    finally:
        worker.close()
    failures = []
    for case in cases:
        choices = [choice for choice in candidates if choice['id'] == case['id']]
        acceptable = [choice for choice in choices
                      if qualify_probe(case['number'], measured[choice['probe_label']]['probe'])]
        if not acceptable:
            probes = [measured[choice['probe_label']]['probe'] for choice in choices]
            failures.append(dict(id=case['id'], reason='no qualifying native geometry', native_facts=str(output),
                candidates_probed=len(probes), complete_detours=sum(probe['detour_complete'] for probe in probes),
                supported_routes=sum(supported_route(probe) for probe in probes)))
            continue
        if case['number'] == 6:
            acceptable.sort(key=lambda choice: (measured[choice['probe_label']]['probe']['detour_length_cm'], choice['probe_label']))
        chosen = acceptable[0]
        chosen = {key: value for key, value in chosen.items() if key not in ('variants', 'qualification_candidates', 'probe_label', 'label')}
        evidence = measured[acceptable[0]['probe_label']]
        chosen['qualification_evidence'] = evidence
        if case['number'] == 4:
            label = evidence['probe']['direct_obstruction']['label'].upper()
            chosen['qualification_evidence']['rock_semantics_confirmed'] = any(token in label for token in ('ROCK', 'FELS', 'CLIFF'))
        selected[case['id']] = chosen
    frozen = deepcopy(suite)
    for scenario in frozen['scenarios']:
        if scenario.get('variants'):
            variants = []
            for variant in scenario['variants']:
                chosen = selected.get(scenario['id'] + '-' + variant['id'])
                if chosen:
                    variant = dict(variant, goal=chosen['goal'], routine_point_selection=chosen['routine_point_selection'],
                                   qualification_evidence=chosen['qualification_evidence'])
                variants.append(variant)
            scenario['variants'] = variants
            scenario['goal'] = variants[0]['goal']
        elif scenario['id'] in selected:
            scenario.update(selected[scenario['id']])
        if scenario['id'] in selected:
            scenario.pop('qualification_candidates', None)
        if scenario['number'] == 8:
            scenario['time'] = read_json(preparation / 'market-census.json')['peak']['time']
    semantic = [case['id'] for case in selected.values() if case['number'] == 4
                and not case['qualification_evidence']['rock_semantics_confirmed']]
    frozen['qualification_status'] = 'partial'
    frozen['qualification_source'] = str(output)
    frozen['unresolved_geometry'] = failures
    frozen['semantic_confirmation_required'] = semantic
    destination = args.output / 'qualified-suite.json'
    write_json(destination, frozen)
    print(json.dumps(dict(event='geometry_qualification', suite=str(destination), native_probe_count=len(measured),
                         accepted=len(selected), unresolved_geometry=failures, semantic_confirmation_required=semantic)), flush=True)
    live_args = SimpleNamespace(suite=destination, runtime=args.runtime, output=args.output,
        binary=args.runtime / 'build/opengothic/Gothic2Notr', deadline=phase_deadline(),
        inputs=dict(inputs, cache=str(args.runtime / 'navmesh-cache')),
        input_sha256={name: sha256(inputs[name]) for name in ('save', 'roads', 'familiar', 'links')})
    frozen_cases = suite_cases(frozen)
    expected_learner_runs = (sum(len(case['seeds']) for case in frozen_cases) * len(args.weights.split(','))
                             + sum(2 * len(case['seeds']) for case in frozen_cases if case['number'] == 10))
    live_args.expected_learner_runs = expected_learner_runs
    accepted = [case for case in frozen_cases if case['id'] in selected]
    teacher_failures = teachers(live_args, accepted)
    outcomes = [teacher_outcome(live_args, case) for case in accepted]
    passed = [row['id'] for row in outcomes if row['passed']]
    complete = not failures and not teacher_failures and not semantic and len(passed) == len(frozen_cases)
    frozen['qualification_status'] = 'geometry-qualified' if complete else 'partial'
    frozen['teacher_qualification'] = dict(complete=not failures and not teacher_failures,
        binary_sha256=sha256(live_args.binary), required_placements=len(frozen_cases),
        passed_placements=len(passed), placements=outcomes)
    write_json(destination, frozen)
    report(live_args, frozen_cases, [], None)
    unresolved = [*failures, *teacher_failures,
                  *(dict(id=identifier, reason='rock semantics require confirmation') for identifier in semantic)]
    write_json(output / 'result.json', dict(geometry_qualified=not failures,
        accepted=len(selected), native_probe_count=len(measured), baseline_runs=0, expected_baseline_runs=expected_learner_runs,
        teacher_placements_passed=passed, teacher_placements=outcomes, unresolved=unresolved,
        complete=complete))
    print(json.dumps(dict(event='teacher_qualification', required=len(frozen_cases), passed=len(passed),
        geometry_rejected=len(failures), teacher_rejected=len(teacher_failures), complete=complete,
        suite=str(destination))), flush=True)
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--suite', type=Path, required=True)
    parser.add_argument('--qualify', action='store_true')
    parser.add_argument('--weights')
    args = parser.parse_args(argv)
    if args.qualify:
        if not args.weights:
            parser.error('--qualify requires --weights')
        return qualify(args)
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
    raise SystemExit(main())
