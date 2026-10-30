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
              f"navdemos live-target {'player' if case.get('target') == 'PC_HERO' else 'npc'}",
              f"navdemos live-guide {'player' if case.get('live_guide') == 'player' else 'off'}"]
    result.extend(case.get('prepare_commands', []))
    # Keep native routines alive. The observer starts beside the actor so normal AI
    # stays active through the market; the navigation stand owns only this actor.
    if case['start'].startswith('cm:'):
        result.append('goto pos ' + case['start'][3:].replace(',', ' '))
    else:
        result.append(f"goto {case['start']}")
    result += [f"watch {case['npc']}", 'cam follow 650 260 90', 'overlay off']
    if recorded:
        result.append('rec source')
    result += [episode_command(case, model, seed), 'navdemos wait']
    if recorded:
        result.append('stop')
    return result


def behavioral_case(case):
    fields = ('id', 'npc', 'start', 'goal', 'goal_kind', 'time', 'limit_ms', 'stranger',
              'route', 'target', 'scenario_seed', 'live_guide', 'prepare_commands')
    return {field: case[field] for field in fields if field in case}


def actual_inputs(args, case, model, seed, recorded=True):
    if not hasattr(args, 'binary_sha256'):
        args.binary_sha256 = sha256(args.binary)
    return dict(case=behavioral_case(case), model_sha256=model['sha256'] if model else None, seed=seed,
                binary_sha256=args.binary_sha256, input_sha256=args.input_sha256, recorded=recorded)


def teacher_directory(args, case):
    key = actual_inputs(args, case, None, case['seeds'][0])
    signature = hashlib.sha256(json.dumps(key, sort_keys=True).encode()).hexdigest()[:12]
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
        environment = dict(os.environ, BENCH_HOST='local', NO_BUILD='1', GAME_BIN=str(args.binary),
            SAVE=str(args.inputs['save']), GOTHIC_AI='0', TEMPEST_WINDOW_SIZE='640x360',
            OPENGOTHIC_BODY_TRACE='0', OPENGOTHIC_NAV_LIVE='1', OPENGOTHIC_TEXTURES='', OPENGOTHIC_CONTENT='', MOVABLE_VOBS='0',
            OPENGOTHIC_BENCH_ENCODER='-c:v libx264 -preset veryfast -crf 30 -maxrate 800k -bufsize 1600k -vf fps=15',
            OMP_NUM_THREADS='1', OPENBLAS_NUM_THREADS='1',
            TIMEOUT=str(min(600, int(remaining) - 20)))
        for key in ('OPENGOTHIC_NAV_JOURNEY_MEMORY', 'OPENGOTHIC_NAV_FAMILIAR_MAP', 'OPENGOTHIC_BENCH_REALTIME'):
            environment.pop(key, None)
        with (directory / 'launcher.log').open('w') as log:
            subprocess.run(['bash', str(ROOT / 'scripts/bench.sh'), str(scenario), str(directory)],
                           env=environment, check=True, stdout=log, stderr=subprocess.STDOUT,
                           timeout=min(620, remaining - 5))
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


def teachers(args, cases):
    failures = []
    # Qualify the newly selected coast before spending time on the fixed places.
    for case in sorted(cases, key=lambda row: (row['number'] != 6, row['number'], row['id'])):
        directory = teacher_directory(args, case)
        episode = run_episode(args, case, None, case['seeds'][0], directory)
        summary = read_json(directory / 'summary.json')
        unsafe = summary.get('deep_water_entries') not in (0, None) or (case['number'] == 7 and summary.get('falls') != 0)
        if not episode.get('success') or unsafe:
            failure = dict(
                id=case['id'], elapsed_ms=episode.get('elapsed_ms'), failure=episode.get('failure'),
                deep_water_entries=summary.get('deep_water_entries'), falls=summary.get('falls'),
                output=str(directory))
            failures.append(failure)
            print(json.dumps(dict(event='teacher_place_rejected', **failure)), flush=True)
    return failures


def smoke(args, cases, policies, unresolved=None):
    failures = teachers(args, cases)
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
        recording_actions_identical=equal_recording, result=line, unresolved=unresolved,
        teacher_placements_passed=[row['id'] for row in passed],
        recorded_runs=[directory.relative_to(args.output).as_posix() for directory in directories[:2]]))
    if repeatable and equal_recording is False:
        raise ValueError('Recording changed actions in a repeatable scenario')
    return complete


def runs(cases, policies, bc4):
    for case in cases:
        for model in policies:
            for seed in case['seeds']:
                yield case, model, seed
        if case['number'] == 10:
            for stranger in (False, True):
                controlled = dict(case, stranger=stranger, id=case['id'] + ('-stranger' if stranger else '-local'))
                for seed in case['seeds']:
                    yield controlled, bc4, seed


def run_directory(output, case, model, seed):
    return output / model['name'] / case['id'] / f'seed-{seed}'


def completed_run(args, case, model, seed):
    directory = run_directory(args.output, case, model, seed)
    try:
        return (processed(directory) and raw_complete(directory) and read_json(directory / 'identity.json').get('actual')
                == actual_inputs(args, case, model, seed))
    except (OSError, ValueError):
        return False


def report(args, cases, policies, bc4):
    all_runs = list(runs(cases, policies, bc4))
    columns = [model['name'] for model in policies] + ['bc4 local', 'bc4 stranger']
    lines = ['# Navigation live suite — ' + args.output.name, '',
             'Facts from the fixed evaluation-only suite. Seeds and places are never training inputs.', '',
             '| Scenario | ' + ' | '.join(columns) + ' |',
             '|---|' + '---|' * len(columns)]
    complete = True
    for number in range(1, 11):
        cells = []
        scenario = next(case for case in cases if case['number'] == number)
        for column in columns:
            selected = [(case, model, seed) for case, model, seed in all_runs
                        if case['number'] == number and (model['name'] == column or
                        (model['name'] == 'bc4' and column == ('bc4 stranger' if case['stranger'] else 'bc4 local')))]
            if not selected:
                cells.append('—')
                continue
            rows = []
            for case, model, seed in selected:
                directory = run_directory(args.output, case, model, seed)
                if not completed_run(args, case, model, seed):
                    complete = False
                    continue
                value = read_json(directory / 'summary.json')
                episode = read_json(directory / 'episode.json')
                rows.append((bool(episode.get('success')), float(episode.get('elapsed_ms', 0)) / 1000,
                             seed, case['id'], directory, value))
            if not rows:
                cells.append(f'0/{len(selected)} recorded')
                continue
            # Failures rank worse than arrivals, then longer elapsed time. Each
            # fixed variant remains listed so scenario 2 does not hide a start.
            rows.sort(key=lambda row: (not row[0], row[1], row[2], row[3]))
            median, worst = rows[len(rows) // 2], rows[-1]
            links = []
            for label, row in (('median', median), ('worst', worst)):
                relative = row[4].relative_to(args.output).as_posix()
                links.append(f"{label} [{row[2]} / {row[3]}]({relative}/topdown.mp4), "
                             f"[video]({relative}/source.mp4), [timeline]({relative}/timeline.png), "
                             f"[summary]({relative}/summary.json)")
            cells.append(f'{sum(row[0] for row in rows)}/{len(selected)} arrived; ' + '; '.join(links))
        references = []
        for case in (item for item in cases if item['number'] == number):
            directory = teacher_directory(args, case)
            if processed(directory):
                relative = directory.relative_to(args.output).as_posix()
                references.append(f"[teacher {case['id']}]({relative}/topdown.mp4), [actions]({relative}/actions.txt), "
                                  f"[Detour]({relative}/live-format.json)")
            else:
                references.append(f"teacher {case['id']}: not recorded")
        lines.append(f"| {number}. {scenario['name']}<br>" + '; '.join(references) + ' | ' + ' | '.join(cells) + ' |')
    lines += ['', 'Models appear in the requested comparison order; each previous model is in the adjacent column.', '']
    gate = args.output / 'smoke.json'
    if gate.is_file():
        comparison = read_json(gate)
        lines += [comparison['result'], '']
        for relative in comparison.get('recorded_runs', []):
            lines += [f"Smoke: [video]({relative}/source.mp4), [topdown]({relative}/topdown.mp4), "
                      f"[timeline]({relative}/timeline.png), [summary]({relative}/summary.json).", '']
        if comparison.get('unresolved'):
            lines += ['Unresolved placements: ' + ', '.join(value['id'] for value in comparison['unresolved']) + '.', '']
    lines += [f'Completed runs: {sum(completed_run(args, c, m, s) for c, m, s in all_runs)}/{len(all_runs)}.',
              f'Storage: {check_space(args.output)} bytes; limit {MAX_BYTES} bytes.', '']
    temporary = args.output / f'README.md.{os.getpid()}.tmp'
    temporary.write_text('\n'.join(lines))
    temporary.replace(args.output / 'README.md')
    if not complete:
        (args.output / 'complete.json').unlink(missing_ok=True)
    return complete


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=('smoke', 'baseline', 'report'))
    parser.add_argument('--suite', type=Path, required=True)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--weights', required=True)
    parser.add_argument('--shard', type=int, choices=range(3), default=0)
    args = parser.parse_args(argv)
    if any(not path.is_absolute() for path in (args.suite, args.runtime, args.output)):
        parser.error('Suite, runtime and output paths must be absolute')
    args.output.mkdir(parents=True, exist_ok=True)
    args.binary = args.runtime / 'build/opengothic/Gothic2Notr'
    args.deadline = phase_deadline()
    suite = read_json(args.suite)
    cases = suite_cases(suite)
    if args.stage in ('smoke', 'baseline') and suite.get('qualification_status') != 'geometry-qualified':
        raise ValueError('Freeze geometry-qualified places before smoke or baseline')
    if args.stage == 'baseline' and suite.get('semantic_confirmation_required'):
        raise ValueError('Confirm the scene type before baseline: ' + ', '.join(suite['semantic_confirmation_required']))
    policies, bc4 = models(args.weights, suite)
    args.inputs = dict(suite['inputs'], cache=str(args.runtime / 'navmesh-cache'))
    for name in ('save', 'roads', 'familiar', 'links'):
        if not Path(args.inputs[name]).is_file():
            raise ValueError(f'Missing frozen input {name}: {args.inputs[name]}')
    args.input_sha256 = {name: sha256(args.inputs[name]) for name in ('save', 'roads', 'familiar', 'links')}
    check_space(args.output)
    smoke_complete = True
    if args.stage == 'smoke':
        unresolved = [dict(id=identifier, reason='scene type requires confirmation')
                      for identifier in suite.get('semantic_confirmation_required', [])]
        smoke_complete = smoke(args, cases, policies, unresolved)
    elif args.stage == 'baseline':
        gate = read_json(args.output / 'smoke.json')
        if not gate.get('complete') or gate['suite_sha256'] != sha256(args.suite) or gate['binary_sha256'] != sha256(args.binary):
            raise ValueError('Complete smoke and teacher qualification on these exact inputs first')
        for index, (case, model, seed) in enumerate(runs(cases, policies, bc4)):
            if index % 3 != args.shard:
                continue
            # Scenario variants have an independent teacher path.
            base_id = case['id'].removesuffix('-local').removesuffix('-stranger') if model['name'] == 'bc4' else case['id']
            reference_case = next(row for row in cases if row['id'] == base_id)
            reference = teacher_directory(args, reference_case)
            run_episode(args, case, model, seed, run_directory(args.output, case, model, seed), reference)
    complete = report(args, cases, policies, bc4)
    if complete and not suite.get('semantic_confirmation_required') and read_json(args.output / 'smoke.json').get('complete'):
        write_json(args.output / 'complete.json', dict(schema_version=1, complete=True,
            suite_sha256=sha256(args.suite), weights=[model['sha256'] for model in policies],
            bc4_sha256=bc4['sha256']))
    return 0 if smoke_complete else 2


if __name__ == '__main__':
    raise SystemExit(main())
