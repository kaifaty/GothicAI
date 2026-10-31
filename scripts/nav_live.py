#!/usr/bin/env python3
"""Run the fixed navigation viewing suite inside an already admitted gq job."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time


ROOT = Path(__file__).resolve().parent.parent
MAX_BYTES = 5_000_000_000
MIN_FREE = 20 * 1024**3
ACTION_SEEDS = [18102027, 18102028, 18102029]


def read_json(path):
    return json.loads(Path(path).read_text())


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')
    temporary.replace(path)


def sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def phase_deadline():
    remaining = float(os.environ.get('NAVLIVE_DEADLINE', time.time() + 3300)) - time.time()
    return time.monotonic() + min(3300, max(0, remaining))


def suite_cases(suite):
    if suite.get('schema_version') != 1 or suite.get('training_eligible') is not False:
        raise ValueError('nav-live requires a version 1 evaluation-only suite')
    scenarios = suite['scenarios']
    if sorted(row['number'] for row in scenarios) != list(range(1, 11)):
        raise ValueError('nav-live requires the ten fixed scenario numbers')
    cases = []
    for scenario in scenarios:
        if scenario.get('training_eligible') is not False or scenario.get('split') not in ('validation', 'new', 'control'):
            raise ValueError(f"Scenario is not held out: {scenario['id']}")
        variants = scenario.get('variants') or [{}]
        for variant in variants:
            case = {**scenario, **variant}
            case['id'] = scenario['id'] + ('-' + variant['id'] if variant else '')
            case['scenario_id'] = scenario['id']
            case['seeds'] = variant.get('seeds', scenario.get('seeds', suite.get('seeds', ACTION_SEEDS)))
            if len(case['seeds']) not in (1, 3) or len(set(case['seeds'])) != len(case['seeds']):
                raise ValueError('Each live case needs its fixed seed or three distinct seeds')
            if case.get('goal_kind', 'point') not in ('point', 'object', 'follow', 'approach'):
                raise ValueError('Unsupported live goal kind')
            if case.get('teacher_qualification_required') is False and case['number'] != 3:
                raise ValueError('Only the fixed follow comparison may use an unqualified teacher reference')
            for field in ('id', 'npc', 'start', 'goal'):
                if not isinstance(case[field], str) or not case[field] or any(char.isspace() for char in case[field]):
                    raise ValueError(f'Invalid scenario token: {field}')
            if not isinstance(case.get('source'), (str, dict)) or not case['source']:
                raise ValueError('Scenario requires its place source')
            if not 0 < case.get('limit_ms', 90000) <= 180000:
                raise ValueError('Scenario duration exceeds the live suite budget')
            cases.append(case)
    return cases


def models(weights, suite):
    result = []
    for index, item in enumerate(weights.split(',')):
        path = Path(item)
        if not path.is_absolute() or not path.is_file():
            raise ValueError(f'Missing absolute policy path: {path}')
        name = path.parent.name
        if name == 'update':
            name = path.parent.parent.name
        if path.as_posix().endswith('/hd-run/policy.navbc'):
            name = 'HD'
        elif 'iter-000035' in path.parts:
            name = 'it35'
        name = re.sub(r'[^A-Za-z0-9_.-]', '-', name)
        if any(model['name'] == name for model in result):
            raise ValueError(f'Duplicate model name: {name}')
        result.append(dict(name=name, weights=str(path), sha256=sha256(path), order=index))
    bc4 = Path(suite.get('bc4_weights', '/mnt/shared/gothic-ai/nav-bc4/bc4/policy.navbc'))
    if not bc4.is_file():
        raise ValueError(f'Missing familiar-channel control: {bc4}')
    return result, dict(name='bc4', weights=str(bc4), sha256=sha256(bc4))


def check_space(output):
    free = shutil.disk_usage(Path.home()).free
    if free < MIN_FREE:
        raise RuntimeError(f'Local disk reserve exhausted: {free} bytes free, need {MIN_FREE}')
    size = sum(path.stat().st_size for path in output.rglob('*') if path.is_file())
    if size >= MAX_BYTES:
        raise RuntimeError(f'Round storage limit reached: {size} bytes >= {MAX_BYTES}')
    return size


def raw_complete(directory, recorded=True):
    names = ['episode.json', 'actions.txt', 'trajectory.txt']
    if recorded:
        names += ['live-format.json', 'live-decisions.jsonl', 'live-observations.bin.gz', 'source.mp4']
    if not all((directory / name).is_file() and (directory / name).stat().st_size for name in names):
        return False
    try:
        episode = read_json(directory / 'episode.json')
    except (OSError, ValueError):
        return False
    if episode.get('decisions', 0) <= 0 or episode.get('elapsed_ms', 0) <= 0:
        return False
    if recorded:
        try:
            rows = [json.loads(line) for line in (directory / 'live-decisions.jsonl').read_text().splitlines()]
            if len(rows) != episode['decisions'] or not rows or episode['elapsed_ms'] - rows[-1]['elapsed_ms'] > 100:
                return False
            probe = subprocess.run(['ffprobe', '-v', 'error', '-show_entries', 'format=duration',
                                    '-of', 'default=nw=1:nk=1', str(directory / 'source.mp4')],
                                   capture_output=True, text=True, check=True)
            if abs(float(probe.stdout) - episode['elapsed_ms'] / 1000) > .2:
                return False
        except (ValueError, KeyError, subprocess.CalledProcessError):
            return False
    return True


def processed(directory):
    if not all((directory / name).is_file() and (directory / name).stat().st_size
               for name in ('summary.json', 'topdown.mp4', 'timeline.png')):
        return False
    return read_json(directory / 'summary.json').get('schema_version', read_json(directory / 'summary.json').get('schema')) == 1


def setup_rejection(directory):
    path = directory / 'episode.json'
    if not path.is_file():
        return None
    try:
        episode = read_json(path)
    except (OSError, ValueError):
        return None
    if ((episode.get('setup_rejected') is True or episode.get('failure') == 'dagger_start_not_supported')
            and episode.get('success') is False
            and episode.get('elapsed_ms') == 0 and episode.get('decisions') == 0 and episode.get('failure')):
        return dict(episode, setup_rejected=True)
    return None


def episode_command(case, model, seed):
    npc, start, goal = case['npc'], case['start'], case['goal']
    kind = case.get('goal_kind', 'point')
    limit = case.get('limit_ms', 90000)
    if model is None:
        if kind in ('follow', 'approach'):
            return (f"navdemos moving-v3 3 {npc} {start} {case['target']} {','.join(case['route'])} "
                    f"{kind} {limit} {case.get('scenario_seed', seed)}")
        command = 'object-v3 3' if kind == 'object' else 'start-v3'
        return f'navdemos {command} {npc} {start} {goal} {limit}'
    suffix = f"{limit} {model['weights']} 0 {seed} sample"
    if kind in ('follow', 'approach'):
        return (f"navdemos drive-moving {npc} {start} {case['target']} {','.join(case['route'])} "
                f"{kind} {limit} {case.get('scenario_seed', seed)} {model['weights']} 0 {seed} sample")
    command = 'drive-object' if kind == 'object' else 'drive'
    return f'navdemos {command} {npc} {start} {goal} {suffix}'


def commands(case, model, seed, inputs, recorded=True):
    hour, minute = case['time']
    result = [f'seed {seed}', 'cheat god', f'set time {hour} {minute}',
              'navdemos physics cv', f"navmesh build {inputs['cache']} {inputs['links']}",
              f"roads load {inputs['roads']}", f"roads familiar-load {inputs['familiar']}",
              f"roads {'stranger' if case.get('stranger') else 'on'} {case['npc']}",
              'navdemos hero keep', 'navdemos record off',
              'navdemos oracle off', 'navdemos perception-mode gaze', 'navdemos perception-monitor on',
              f"navdemos live {'on' if recorded else 'off'}",
              f"navdemos live-target {case.get('live_target', 'player' if case.get('target') == 'PC_HERO' else 'npc')}",
              f"navdemos live-guide {'player' if case.get('live_guide') == 'player' else 'off'}"]
    # Keep native routines alive. The observer starts beside the actor so normal AI
    # stays active through the market; the navigation stand owns only this actor.
    if case['start'].startswith('cm:'):
        result.append('goto pos ' + case['start'][3:].replace(',', ' '))
    else:
        result.append(f"goto {case['start']}")
    result.extend(case.get('prepare_commands', []))
    result += [f"watch {case['npc']}", 'cam follow 650 260 90', 'overlay off']
    if recorded:
        result.append('rec source')
    result += [episode_command(case, model, seed), 'navdemos wait']
    if case.get('target') == 'PC_HERO':
        result.append('hp PC_HERO')
    if recorded:
        result.append('stop')
    return result


def behavioral_case(case):
    fields = ('id', 'npc', 'start', 'goal', 'goal_kind', 'time', 'limit_ms', 'stranger',
              'route', 'target', 'scenario_seed', 'live_guide', 'prepare_commands', 'live_target')
    return {field: case[field] for field in fields if field in case}


def case_binary(args, case):
    pin = case.get('evaluation_binary')
    path = Path(pin['path']) if pin else args.binary
    if not path.is_absolute():
        raise ValueError('An evaluation binary must have an absolute path')
    if not hasattr(args, 'binary_hashes'):
        args.binary_hashes = {}
    if path not in args.binary_hashes:
        args.binary_hashes[path] = sha256(path)
    digest = args.binary_hashes[path]
    if pin and digest != pin['sha256']:
        raise ValueError('The case evaluation binary differs from its qualified reference')
    return path, digest


def actual_inputs(args, case, model, seed, recorded=True):
    _, digest = case_binary(args, case)
    return dict(case=behavioral_case(case), model_sha256=model['sha256'] if model else None, seed=seed,
                binary_sha256=digest, input_sha256=args.input_sha256, recorded=recorded)


def teacher_directory(args, case):
    key = actual_inputs(args, case, None, case['seeds'][0])
    signature = hashlib.sha256(json.dumps(key, sort_keys=True).encode()).hexdigest()[:12]
    if case.get('teacher_reference'):
        reference = Path(case['teacher_reference'])
        if not reference.is_absolute() or not reference.resolve().is_relative_to((args.output / 'teacher').resolve()):
            raise ValueError('A teacher reference must belong to this round')
        identity = reference / 'identity.json'
        if not identity.is_file() or read_json(identity).get('actual') == key:
            return reference
    return args.output / 'teacher' / f"{case['id']}-{signature}"


def run_episode(args, case, model, seed, directory, teacher=None, recorded=True):
    directory = Path(directory)
    case_identity = {key: value for key, value in case.items() if key != 'variants'}
    actual = actual_inputs(args, case, model, seed, recorded)
    identity = dict(case=case_identity, model=model, seed=seed, actual=actual,
                    suite_sha256=sha256(args.suite), binary_sha256=actual['binary_sha256'], recorded=recorded)
    if (directory / 'identity.json').is_file():
        previous = read_json(directory / 'identity.json')
        if previous.get('actual') != actual:
            raise ValueError(f'Refusing different inputs at {directory}')
    if directory.exists() and any(directory.iterdir()) and not (directory / 'identity.json').is_file():
        raise ValueError(f'Refusing an unowned existing run directory: {directory}')
    directory.mkdir(parents=True, exist_ok=True)
    if not (directory / 'identity.json').is_file():
        write_json(directory / 'identity.json', identity)
    rejected = setup_rejection(directory)
    if rejected:
        return rejected
    if not raw_complete(directory, recorded):
        # This identity proves that the directory belongs to this exact attempt.
        # A stopped process may have left an old video next to a new trace.
        for path in directory.iterdir():
            if path.name == 'identity.json':
                continue
            if path.is_dir() and not path.is_symlink():
                shutil.rmtree(path)
            else:
                path.unlink()
        used = check_space(args.output)
        if used + 128 * 1024**2 >= MAX_BYTES:
            raise RuntimeError(f'Not enough round storage for another run: {used} bytes used')
        remaining = args.deadline - time.monotonic()
        if remaining < 120:
            raise TimeoutError('Job budget reached between episodes; run the same command to continue')
        scenario = directory / 'scenario.bench'
        scenario.write_text('\n'.join(commands(case, model, seed, args.inputs, recorded)) + '\n')
        environment = dict(os.environ, BENCH_HOST='local', NO_BUILD='1', GAME_BIN=str(case_binary(args, case)[0]),
            SAVE=str(args.inputs['save']), GOTHIC_AI='0', TEMPEST_WINDOW_SIZE='640x360',
            OPENGOTHIC_BODY_TRACE='0', OPENGOTHIC_NAV_LIVE='1', OPENGOTHIC_TEXTURES='', OPENGOTHIC_CONTENT='', MOVABLE_VOBS='0',
            OPENGOTHIC_BENCH_ENCODER='-c:v libx264 -preset veryfast -crf 30 -maxrate 800k -bufsize 1600k -vf fps=15',
            OMP_NUM_THREADS='1', OPENBLAS_NUM_THREADS='1',
            TIMEOUT=str(min(600, int(remaining) - 20)))
        for key in ('OPENGOTHIC_NAV_JOURNEY_MEMORY', 'OPENGOTHIC_NAV_FAMILIAR_MAP', 'OPENGOTHIC_BENCH_REALTIME'):
            environment.pop(key, None)
        with (directory / 'launcher.log').open('w') as log:
            result = subprocess.run(['bash', str(ROOT / 'scripts/bench.sh'), str(scenario), str(directory)],
                                    env=environment, stdout=log, stderr=subprocess.STDOUT,
                                    timeout=min(620, remaining - 5))
        rejected = setup_rejection(directory)
        if rejected:
            return rejected
        result.check_returncode()
        if not raw_complete(directory, recorded):
            raise ValueError(f'Incomplete game run: {directory}; see launcher.log and game.log')
        # The save is input, not a per-seed output. Keep only the small bench ini.
        saved = directory / 'run/save_slot_1.sav'
        saved.unlink(missing_ok=True)
        (directory / 'navmesh-polygons.jsonl').unlink(missing_ok=True)
    if recorded and not processed(directory):
        from nav_live_render import render_run
        if model is not None and (teacher is None or not raw_complete(Path(teacher))):
            raise ValueError(f'Missing complete teacher reference for {directory}')
        render_run(directory, teacher_directory=directory if model is None else teacher, scenario=case)
    check_space(args.output)
    return read_json(directory / 'episode.json')


def teacher_outcome(args, case):
    directory = teacher_directory(args, case)
    episode = setup_rejection(directory) or read_json(directory / 'episode.json')
    summary = {} if episode.get('setup_rejected') else read_json(directory / 'summary.json')
    deep_water = summary.get('deep_water_entries') not in (0, None)
    unsafe_fall = summary.get('falls') != 0
    unsafe_target_route = case.get('target_route_evidence', {}).get('complete') is False
    minimum_edge = summary.get('minimum_edge_distance_cm')
    edge_requirement = case.get('teacher_minimum_edge_cm')
    unsafe_edge = edge_requirement is not None and (minimum_edge is None or minimum_edge < edge_requirement)
    passed = bool(episode.get('success')) and not deep_water and not unsafe_fall and not unsafe_target_route and not unsafe_edge
    failure = episode.get('failure') or ('teacher_entered_deep_water' if deep_water else
        'teacher_fell' if summary.get('falls', 0) else 'teacher_fall_measurement_missing' if unsafe_fall else
        'target_route_geometry_incomplete' if unsafe_target_route else
        'teacher_edge_clearance_below_threshold' if unsafe_edge else
        'teacher_did_not_arrive' if not passed else None)
    return dict(id=case['id'], passed=passed, elapsed_ms=episode.get('elapsed_ms'), failure=failure,
        decisions=episode.get('decisions'), setup_rejected=bool(episode.get('setup_rejected')),
        deep_water_entries=summary.get('deep_water_entries'), falls=summary.get('falls'),
        minimum_edge_distance_cm=minimum_edge, output=str(directory))


def teachers(args, cases):
    failures = []
    # Qualify the newly selected coast before spending time on the fixed places.
    for case in sorted(cases, key=lambda row: (row['number'] != 6, row['number'], row['id'])):
        directory = teacher_directory(args, case)
        run_episode(args, case, None, case['seeds'][0], directory)
        outcome = teacher_outcome(args, case)
        if not outcome['passed']:
            failures.append(outcome)
        print(json.dumps(dict(event='teacher_place_passed' if outcome['passed'] else 'teacher_place_rejected', **outcome)), flush=True)
    return failures


def eligible_cases(suite, cases):
    placements = suite.get('teacher_qualification', {}).get('placements', [])
    passed = {row['id'] for row in placements
              if row.get('passed') and row.get('falls') == 0}
    references = {row['id'] for row in placements if row.get('elapsed_ms', 0) > 0 and row.get('decisions', 0) > 0}
    passed.update(case['id'] for case in cases if case.get('teacher_qualification_required') is False and case['id'] in references)
    excluded = set(suite.get('semantic_confirmation_required', []))
    excluded.update(row['id'] for row in suite.get('unresolved_geometry', []))
    return [case for case in cases if case['id'] in passed - excluded and case.get('qualification_evidence')]


def smoke(args, cases, policies, unresolved=None):
    failures = teachers(args, [case for case in cases if case.get('teacher_qualification_required', True)])
    unresolved = [*(unresolved or []), *failures]
    rejected_ids = {value['id'] for value in failures}
    passed = [case for case in cases if case['id'] not in rejected_ids]
    if not passed:
        write_json(args.output / 'smoke.json', dict(schema_version=1, complete=False,
            suite_sha256=sha256(args.suite), binary_sha256=sha256(args.binary), unresolved=unresolved,
            result='Same-seed repetition and recording comparison not run: no accepted teacher placement.'))
        return False
    case, model = passed[0], policies[0]
    seed = case['seeds'][0]
    reference = teacher_directory(args, case)
    directories = [args.output / 'smoke' / reference.name / name for name in ('recorded', 'repeat', 'without-recording')]
    for directory in directories[:2]:
        run_episode(args, case, model, seed, directory, reference)
    run_episode(args, case, model, seed, directories[2], recorded=False)
    actions = [(directory / 'actions.txt').read_bytes() for directory in directories]
    repeatable = actions[0] == actions[1]
    equal_recording = actions[0] == actions[2] if repeatable else None
    line = (f"Same-seed repetition: {'identical' if repeatable else 'different'} actions; "
            f"recording on/off: {'identical' if equal_recording else 'different' if equal_recording is False else 'not deterministic; comparison inconclusive'}.")
    complete = not unresolved and equal_recording is not False
    write_json(args.output / 'smoke.json', dict(schema_version=1, complete=complete,
        suite_sha256=sha256(args.suite), binary_sha256=sha256(args.binary), repeatable=repeatable,
        model_sha256=model['sha256'],
        recording_actions_identical=equal_recording, result=line, unresolved=unresolved,
        teacher_placements_passed=[row['id'] for row in passed if row.get('teacher_qualification_required', True)],
        unqualified_reference_placements=[row['id'] for row in passed if row.get('teacher_qualification_required') is False],
        excluded_placements=[case['id'] for case in suite_cases(read_json(args.suite))
                             if case['id'] not in {row['id'] for row in passed}],
        recorded_runs=[directory.relative_to(args.output).as_posix() for directory in directories[:2]]))
    if repeatable and equal_recording is False:
        raise ValueError('Recording changed actions in a repeatable scenario')
    return complete


def smoke_gate(args, model):
    try:
        gate = read_json(args.output / 'smoke.json')
    except (OSError, ValueError):
        return None
    if (gate.get('suite_sha256') != sha256(args.suite) or gate.get('binary_sha256') != sha256(args.binary)
            or gate.get('model_sha256') != model['sha256']):
        return None
    return gate.get('complete') is True


def runs(cases, policies, bc4):
    for case in cases:
        for model in policies:
            for seed in case['seeds']:
                yield case, model, seed
        if case['number'] == 10 and bc4 is not None:
            for stranger in (False, True):
                controlled = dict(case, stranger=stranger, id=case['id'] + ('-stranger' if stranger else '-local'))
                for seed in case['seeds']:
                    yield controlled, bc4, seed
        elif case['number'] in (3, 4) and case.get('bc4_compare') and bc4 is not None:
            for seed in case['seeds']:
                yield case, bc4, seed


def run_directory(output, case, model, seed):
    return output / model['name'] / case['id'] / f'seed-{seed}'


def completed_run(args, case, model, seed):
    directory = run_directory(args.output, case, model, seed)
    try:
        return (processed(directory) and raw_complete(directory) and read_json(directory / 'identity.json').get('actual')
                == actual_inputs(args, case, model, seed))
    except (OSError, ValueError):
        return False


def follow_statistics(directory):
    value = read_json(Path(directory) / 'summary.json')
    if 'follow_mean_distance_m' not in value:
        from nav_live_render import refresh_follow_summary
        value = refresh_follow_summary(directory)
    band = value.get('follow_band_fraction')
    distance = value.get('follow_mean_distance_m')
    standing = value.get('standing_seconds')
    return ('band 2–5m ' + (f'{100 * band:.4f}%' if band is not None else 'unavailable')
            + '; mean hero distance ' + (f'{distance:.6f}m' if distance is not None else 'unavailable')
            + '; standing ' + (f'{standing:.2f}s' if standing is not None else 'unavailable'))


def report(args, cases, policies, bc4):
    suite = read_json(args.suite)
    eligible = eligible_cases(suite, cases)
    eligible_ids = {case['id'] for case in eligible}
    all_runs = list(runs(eligible, policies, bc4))
    columns = [model['name'] for model in policies] + (['bc4', 'bc4 local', 'bc4 stranger'] if bc4 is not None else [])
    lines = ['# Navigation live suite — ' + args.output.name, '',
             'Facts from the fixed evaluation-only suite. Seeds and places are never training inputs.', '',
             '| Scenario | v3b |' + (' ' + ' | '.join(columns) + ' |' if columns else ''),
             '|---|---|' + '---|' * len(columns)]
    unresolved = {row['id']: row for row in suite.get('unresolved_geometry', [])}
    teacher_passed = 0
    complete = bool(policies) and bool(eligible)
    for number in range(1, 11):
        cells = []
        scenario = next(case for case in cases if case['number'] == number)
        for column in columns:
            selected = [(case, model, seed) for case, model, seed in all_runs
                        if case['number'] == number and column == (model['name'] if model['name'] != 'bc4' else
                        ('bc4 stranger' if case['stranger'] else 'bc4 local') if case['number'] == 10 else 'bc4')]
            if not selected:
                cells.append('excluded' if not any(case['number'] == number for case in eligible) else '—')
                continue
            rows = []
            for case, model, seed in selected:
                directory = run_directory(args.output, case, model, seed)
                if not completed_run(args, case, model, seed):
                    complete = False
                    continue
                value = read_json(directory / 'summary.json')
                if number == 3:
                    follow_statistics(directory)
                    value = read_json(directory / 'summary.json')
                episode = read_json(directory / 'episode.json')
                rows.append((bool(episode.get('success')), float(episode.get('elapsed_ms', 0)) / 1000,
                             seed, case['id'], directory, value))
            if not rows:
                cells.append(f'0/{len(selected)} recorded')
                continue
            # Failures rank worse than arrivals, then longer elapsed time. Each
            # fixed variant remains listed so scenario 2 does not hide a start.
            if number == 3:
                rows.sort(key=lambda row: (-(row[5].get('follow_band_fraction') if row[5].get('follow_band_fraction') is not None else -1),
                    row[5].get('follow_mean_distance_m') if row[5].get('follow_mean_distance_m') is not None else float('inf'),
                    row[5].get('standing_seconds') if row[5].get('standing_seconds') is not None else float('inf'), row[2]))
            else:
                rows.sort(key=lambda row: (not row[0], row[1], row[2], row[3]))
            median, worst = rows[len(rows) // 2], rows[-1]
            links = []
            for label, row in (('median', median), ('worst', worst)):
                relative = row[4].relative_to(args.output).as_posix()
                links.append(f"{label} [{row[2]} / {row[3]}]({relative}/topdown.mp4), "
                             f"[video]({relative}/source.mp4), [timeline]({relative}/timeline.png), "
                             f"[summary]({relative}/summary.json)")
                if number == 3:
                    links[-1] += '; ' + follow_statistics(row[4])
            count = f'{sum(row[0] for row in rows)}/{len(selected)} met follow threshold' if number == 3 else f'{sum(row[0] for row in rows)}/{len(selected)} arrived'
            cells.append(count + '; ' + '; '.join(links))
        references = []
        for case in (item for item in cases if item['number'] == number):
            failure = unresolved.get(case['id'])
            if failure and case.get('teacher_qualification_required', True):
                counts = (f"; {failure['candidates_probed']} candidates, {failure['supported_routes']} supported routes"
                          if 'candidates_probed' in failure else '')
                facts = f"{case['id']}: unqualified; {failure['reason']}{counts}"
                if failure.get('topdown_frame'):
                    frame = os.path.relpath(failure['topdown_frame'], args.output)
                    facts += f'; [reference frame]({frame})'
                if failure.get('start_cm') and failure.get('goal_cm'):
                    facts += f"; start {failure['start_cm']}; goal {failure['goal_cm']}"
                references.append(facts)
                continue
            directory = teacher_directory(args, case)
            rejected = setup_rejection(directory)
            if rejected or (processed(directory) and raw_complete(directory)):
                outcome = teacher_outcome(args, case)
                if case['id'] in eligible_ids and ((not outcome['passed'] and case.get('teacher_qualification_required', True)) or
                        read_json(directory / 'identity.json').get('actual') != actual_inputs(args, case, None, case['seeds'][0])):
                    complete = False
                teacher_passed += int(outcome['passed'])
                relative = directory.relative_to(args.output).as_posix()
                facts = (f"{case['id']}: {'passed' if outcome['passed'] else 'unqualified'}; "
                         f"{outcome['elapsed_ms'] / 1000:.3f}s, {outcome['decisions']} decisions; "
                         f"deep water {outcome['deep_water_entries']}, falls {outcome['falls']}")
                if outcome['failure']:
                    facts += '; ' + outcome['failure']
                if case.get('teacher_qualification_required') is False:
                    facts += '; unqualified teacher reference; comparison explicitly included'
                if number == 3:
                    facts += '; ' + follow_statistics(directory)
                if outcome['passed'] and case['id'] not in eligible_ids:
                    facts += '; excluded: scene type unconfirmed'
                if case.get('target_route_evidence'):
                    legs = case['target_route_evidence']['legs']
                    facts += f"; target route: {sum(row['supported_route'] for row in legs)}/{len(legs)} supported native legs"
                if rejected:
                    references.append(facts + f"; [episode]({relative}/episode.json), [diagnostics]({relative}/live-guide-start.json)")
                else:
                    references.append(facts + f"; [video]({relative}/source.mp4), [topdown]({relative}/topdown.mp4), "
                        f"[timeline]({relative}/timeline.png), [summary]({relative}/summary.json), "
                        f"[actions]({relative}/actions.txt), [Detour]({relative}/live-format.json)")
            else:
                if case['id'] in eligible_ids:
                    complete = False
                references.append(f"{case['id']}: not recorded")
        lines.append(f"| {number}. {scenario['name']} | " + '<br>'.join(references) + ' |'
                     + (' ' + ' | '.join(cells) + ' |' if cells else ''))
    lines += ['', 'Models appear in the requested comparison order; each previous model is in the adjacent column.', '']
    lines += [f"Qualification status: {suite.get('qualification_status', 'pending')}.", '']
    excluded = [case['id'] for case in cases if case['id'] not in eligible_ids]
    if excluded:
        lines += ['Excluded placements: ' + ', '.join(excluded) + '.', '']
    if suite.get('semantic_confirmation_required'):
        lines += ['Scene type unconfirmed: ' + ', '.join(suite['semantic_confirmation_required']) + '.', '']
    gate = args.output / 'smoke.json'
    if gate.is_file() and read_json(gate).get('suite_sha256') == sha256(args.suite):
        comparison = read_json(gate)
        lines += [comparison['result'], '']
        for relative in comparison.get('recorded_runs', []):
            lines += [f"Smoke: [video]({relative}/source.mp4), [topdown]({relative}/topdown.mp4), "
                      f"[timeline]({relative}/timeline.png), [summary]({relative}/summary.json).", '']
        if comparison.get('unresolved'):
            lines += ['Unresolved placements: ' + ', '.join(value['id'] for value in comparison['unresolved']) + '.', '']
    complete = complete and smoke_gate(args, policies[0]) is True
    lines += [f'Teacher placements passed: {teacher_passed}/{len(cases)}.',
              f'Eligible placements: {len(eligible)}/{len(cases)}; excluded placements are omitted from learner runs.',
              f'Qualified subset complete: {str(complete).lower()}; full suite qualified: {str(len(eligible) == len(cases) and all(case.get("teacher_qualification_required", True) for case in cases)).lower()}.',
              f'Completed learner runs: {sum(completed_run(args, c, m, s) for c, m, s in all_runs)}/{len(all_runs) or getattr(args, "expected_learner_runs", 0)}.',
              f'Storage: {check_space(args.output)} bytes; limit {MAX_BYTES} bytes.', '']
    temporary = args.output / f'README.md.{os.getpid()}.tmp'
    temporary.write_text('\n'.join(lines))
    temporary.replace(args.output / 'README.md')
    if not complete:
        (args.output / 'complete.json').unlink(missing_ok=True)
    return complete


def cleanup(runtime, output, suite_path=None):
    runtime, output = Path(runtime), Path(output)
    marker_path = output / 'complete.json'
    if not marker_path.is_file():
        print('Local cleanup skipped: accepted runs are incomplete.', flush=True)
        return False
    marker = read_json(marker_path)
    suite_path = Path(suite_path or marker['suite_path'])
    gate = read_json(output / 'smoke.json')
    suite = read_json(suite_path)
    digest = sha256(suite_path)
    if (not marker.get('complete') or not marker.get('qualified_subset_complete')
            or marker.get('completed') != marker.get('expected') or not marker.get('expected')
            or marker.get('suite_sha256') != digest or gate.get('suite_sha256') != digest
            or not gate.get('complete') or gate.get('model_sha256') != marker['weights'][0]
            or gate.get('binary_sha256') != marker.get('evaluation_binary_sha256')
            or suite['evaluation_binary']['sha256'] != marker.get('evaluation_binary_sha256')):
        print('Local cleanup skipped: completion and smoke identities differ.', flush=True)
        return False
    if marker.get('local_cleanup', {}).get('status') == 'complete':
        return True
    pins = [suite['evaluation_binary']]
    pins += [case['evaluation_binary'] for case in eligible_cases(suite, suite_cases(suite)) if case.get('evaluation_binary')]
    started = marker.get('local_cleanup', {}).get('status') == 'started'
    if any(sha256(pin['path']) != pin['sha256'] for pin in pins if Path(pin['path']).exists() or not started):
        raise ValueError('Local cleanup refused: an evaluation binary differs from its snapshot')
    names = ('build', 'binaries', 'base-cache', '.venv', 'uv-cache', 'navmesh-cache',
             'requalification-worker', 'qualification-worker', 'preparation-worker', 'extension-worker', 'extension-binaries')
    marker['local_cleanup'] = dict(status='started', paths=[str(runtime / name) for name in names])
    write_json(marker_path, marker)
    for name in names:
        path = runtime / name
        if path.is_symlink():
            path.unlink()
        elif path.exists():
            shutil.rmtree(path)
    for path in [*runtime.glob('build-passed-*'), runtime / 'build-snapshot']:
        path.unlink(missing_ok=True)
    marker['local_cleanup']['status'] = 'complete'
    write_json(marker_path, marker)
    print('Local build, binaries and task caches removed; source snapshots and NAS outputs retained.', flush=True)
    return True


def completed_round(runtime, output, weights):
    output = Path(output)
    path = output / 'complete.json'
    if not path.is_file():
        return False
    marker = read_json(path)
    if not marker.get('qualified_subset_complete') or marker.get('local_cleanup', {}).get('status') not in ('started', 'complete'):
        return False
    if sha256(output / 'qualified-suite.json') != marker.get('qualified_suite_sha256'):
        return False
    suite = read_json(marker['suite_path'])
    policies, bc4 = models(weights, suite)
    if [model['sha256'] for model in policies] != marker['weights'] or bc4['sha256'] != marker['bc4_sha256']:
        raise ValueError('Policy weights changed after cleanup; use a new round label')
    if any(sha256(suite['inputs'][name]) != digest for name, digest in marker['input_sha256'].items()):
        raise ValueError('Frozen inputs changed after cleanup; use a new round label')
    planned = list(runs(eligible_cases(suite, suite_cases(suite)), policies, bc4))
    names = ('source.mp4', 'episode.json', 'actions.txt', 'trajectory.txt',
             'live-format.json', 'live-decisions.jsonl', 'live-observations.bin.gz')
    if len(planned) != marker['expected'] or any(not processed(directory) or
            not all((directory / name).is_file() and (directory / name).stat().st_size for name in names)
            for case, model, seed in planned for directory in [run_directory(output, case, model, seed)]):
        return False
    return cleanup(Path(runtime), output, Path(marker['suite_path']))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=('teacher', 'smoke', 'baseline', 'report'))
    parser.add_argument('--suite', type=Path, required=True)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--weights', required=True)
    parser.add_argument('--shard', type=int, choices=(-1, 0, 1, 2), default=-1)
    args = parser.parse_args(argv)
    if any(not path.is_absolute() for path in (args.suite, args.runtime, args.output)):
        parser.error('Suite, runtime and output paths must be absolute')
    args.output.mkdir(parents=True, exist_ok=True)
    if args.stage in ('teacher', 'smoke', 'report') and (args.output / 'qualified-suite.json').is_file():
        args.suite = args.output / 'qualified-suite.json'
    args.binary = args.runtime / 'build/opengothic/Gothic2Notr'
    args.deadline = phase_deadline()
    suite = read_json(args.suite)
    if suite.get('evaluation_binary'):
        evaluation = suite['evaluation_binary']
        args.binary = Path(evaluation['path'])
        if not args.binary.is_absolute() or sha256(args.binary) != evaluation['sha256']:
            raise ValueError('The pinned evaluation binary differs from the suite snapshot')
        args.binary_sha256 = evaluation['sha256']
    cases = suite_cases(suite)
    args.inputs = dict(suite['inputs'], cache=str(args.runtime / 'navmesh-cache'))
    for name in ('save', 'roads', 'familiar', 'links'):
        if not Path(args.inputs[name]).is_file():
            raise ValueError(f'Missing frozen input {name}: {args.inputs[name]}')
    args.input_sha256 = {name: sha256(args.inputs[name]) for name in ('save', 'roads', 'familiar', 'links')}
    check_space(args.output)
    eligible = eligible_cases(suite, cases)
    if args.stage in ('smoke', 'baseline'):
        if not eligible:
            args.expected_learner_runs = (sum(len(case['seeds']) for case in eligible) * len(args.weights.split(','))
                + sum(2 * len(case['seeds']) for case in eligible if case['number'] == 10))
            report(args, cases, [], None)
            print(json.dumps(dict(event='no_qualified_placements', eligible=0,
                                  excluded=[case['id'] for case in cases if case not in eligible])), flush=True)
            return 2
        qualification = suite.get('teacher_qualification', {})
        references = {row['id']: row for row in qualification.get('placements', [])}
        if qualification.get('binary_sha256') != sha256(args.binary):
            raise ValueError('Complete v3b qualification with this binary first')
        for case in eligible:
            directory = teacher_directory(args, case)
            if (references[case['id']]['output'] != str(directory) or not raw_complete(directory) or not processed(directory)
                    or read_json(directory / 'identity.json').get('actual') != actual_inputs(args, case, None, case['seeds'][0])
                    or (case.get('teacher_qualification_required', True) and not teacher_outcome(args, case)['passed'])):
                raise ValueError(f"Missing qualified v3b reference for current inputs: {case['id']}")
    if args.stage == 'teacher':
        case = next(row for row in cases if row['number'] == 1)
        rejected = {row['id'] for row in suite.get('unresolved_geometry', [])}
        if case['id'] in rejected or not case.get('qualification_evidence'):
            raise ValueError('The saved suite has no accepted Maleth fence placement')
        directory = teacher_directory(args, case)
        episode = run_episode(args, case, None, case['seeds'][0], directory)
        summary = {} if episode.get('setup_rejected') else read_json(directory / 'summary.json')
        validated = not episode.get('setup_rejected') and raw_complete(directory) and processed(directory)
        print(json.dumps(dict(stage='teacher', id=case['id'], output=str(directory), requested_seed=case['seeds'][0],
            success=bool(episode['success']), elapsed_ms=episode['elapsed_ms'],
            decisions=episode['decisions'], failure=episode.get('failure'),
            setup_rejected=bool(episode.get('setup_rejected')), deep_water_entries=summary.get('deep_water_entries'),
            falls=summary.get('falls'), validated=validated)), flush=True)
        return 0 if validated and episode['success'] and summary.get('deep_water_entries') in (0, None) else 2
    policies, bc4 = models(args.weights, suite)
    smoke_complete = True
    reused_runs = newly_completed_runs = 0
    if args.stage == 'smoke':
        smoke_complete = smoke(args, eligible, policies)
    elif args.stage == 'baseline':
        gate = smoke_gate(args, policies[0])
        if gate is None:
            gate = smoke(args, eligible, policies)
        if not gate:
            report(args, cases, policies, bc4)
            return 2
        for index, (case, model, seed) in enumerate(runs(eligible, policies, bc4)):
            if args.shard >= 0 and index % 3 != args.shard:
                continue
            if completed_run(args, case, model, seed):
                reused_runs += 1
                continue
            # Scenario variants have an independent teacher path.
            base_id = case['id'].rsplit('-', 1)[0] if model['name'] == 'bc4' and case['number'] == 10 else case['id']
            reference_case = next(row for row in cases if row['id'] == base_id)
            reference = teacher_directory(args, reference_case)
            run_episode(args, case, model, seed, run_directory(args.output, case, model, seed), reference)
            newly_completed_runs += 1
    complete = report(args, cases, policies, bc4)
    if complete:
        expected = len(list(runs(eligible, policies, bc4)))
        write_json(args.output / 'complete.json', dict(schema_version=1, complete=True, qualified_subset_complete=True,
            full_suite_qualified=len(eligible) == len(cases) and all(case.get('teacher_qualification_required', True) for case in cases),
            excluded_placements=[case['id'] for case in cases if case not in eligible], completed=expected, expected=expected,
            reused_runs=reused_runs, newly_completed_runs=newly_completed_runs,
            suite_path=str(args.suite), suite_sha256=sha256(args.suite), evaluation_binary_sha256=sha256(args.binary),
            qualified_suite_sha256=suite.get('qualified_suite_sha256', sha256(args.suite)), input_sha256=args.input_sha256,
            weights=[model['sha256'] for model in policies],
            bc4_sha256=bc4['sha256']))
    return 0 if smoke_complete else 2


if __name__ == '__main__':
    raise SystemExit(main())
