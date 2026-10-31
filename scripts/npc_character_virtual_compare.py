#!/usr/bin/env python3
"""Compare native NPC CharacterVirtual OFF/ON evidence; absent data stays incomplete."""

import argparse
import csv
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path


SPOTS = ('market', 'harbour', 'onar', 'lobart')
WINDOWS = tuple(f'{spot}-{hour}' for spot in SPOTS for hour in (7, 20))
WINDOW_MS = 248300
ROUTES = ('stairs', 'doorway', 'tavern', 'seat')
BOOLEAN_COLUMNS = {'cv_enabled', 'cv_active', 'moving', 'in_air', 'interactive',
                   'captured', 'normal_clock'}
FRAME_COLUMNS = {'phase', 'frame', 't_game_ms', 'dt_ms', 'npc_id', 'x_cm', 'y_cm',
                 'z_cm', 'visual_x_cm', 'visual_y_cm', 'visual_z_cm', 'step_ms',
                 'wall_ms', 'nearby_humans', 'nearby_cv'} | BOOLEAN_COLUMNS
OPTIONAL_FRAME_COLUMNS = {'moving_humans', 'moving_cv'}
SLIDING_BOOLEAN_COLUMNS = {'diagnostics_valid', 'steep_contact', 'move_blocked'}
SLIDING_FRAME_COLUMNS = {'move_sequence', 'move_dt_ms', 'requested_x_cm', 'requested_y_cm',
                         'requested_z_cm', 'accepted_dx_cm', 'accepted_dy_cm', 'accepted_dz_cm',
                         'advancement_ratio', 'steep_normal_x', 'steep_normal_y', 'steep_normal_z'} | SLIDING_BOOLEAN_COLUMNS
OPTIONAL_FRAME_COLUMNS |= SLIDING_FRAME_COLUMNS


def require(condition, message):
    if not condition:
        raise ValueError(message)


def number(value):
    result = float(value)
    require(math.isfinite(result), f'non-finite number: {value!r}')
    return result


def flag(value):
    if value in (True, 1, '1', 'true', 'True'):
        return True
    if value in (False, 0, '0', 'false', 'False'):
        return False
    raise ValueError(f'invalid boolean: {value!r}')


def vector(value):
    if isinstance(value, dict):
        value = [value.get(key) for key in ('x', 'y', 'z')]
    require(isinstance(value, (list, tuple)) and len(value) == 3,
            f'invalid position: {value!r}')
    return [number(part) for part in value]


def combined(checks):
    if any(check is False for check in checks):
        return False
    return True if checks and all(check is True for check in checks) else None


def percentile(values, fraction=.95):
    return sorted(values)[math.ceil(fraction * len(values)) - 1] if values else None


def load_run(directory, enabled, require_frames=True):
    run = {'directory': str(directory), 'enabled': enabled, 'events': [],
           'frames': [], 'errors': [], 'missing': [], 'complete': None}
    required_files = ('events.jsonl', 'bench.log') + (('npc-character-frames.csv',) if require_frames else ())
    for filename in required_files:
        if not (directory / filename).is_file():
            run['missing'].append(filename)
    event_path = directory / 'events.jsonl'
    if event_path.is_file():
        try:
            for index, line in enumerate(event_path.read_text().splitlines(), 1):
                if not line.strip():
                    continue
                row = json.loads(line)
                require(isinstance(row, dict) and isinstance(row.get('kind'), str),
                        f'{event_path}:{index}: invalid event')
                if row['kind'].endswith('_error'):
                    run['errors'].append(f'native error: {row}')
                if row['kind'].startswith('npc_character_'):
                    if 'cv_enabled' in row:
                        require(flag(row['cv_enabled']) is enabled,
                                f'{event_path}:{index}: mode differs from directory')
                    run['events'].append(row)
        except (OSError, ValueError, TypeError) as error:
            run['errors'].append(str(error))
    frame_path = directory / 'npc-character-frames.csv'
    if frame_path.is_file():
        try:
            prior = {}
            with frame_path.open(newline='') as source:
                reader = csv.DictReader(source)
                require(FRAME_COLUMNS <= set(reader.fieldnames or []),
                        f'{frame_path}: missing columns {sorted(FRAME_COLUMNS - set(reader.fieldnames or []))}')
                for index, raw in enumerate(reader, 2):
                    row = {key: (value if key == 'phase' else flag(value) if key in BOOLEAN_COLUMNS | SLIDING_BOOLEAN_COLUMNS
                                 else number(value)) for key, value in raw.items()
                           if key in FRAME_COLUMNS | OPTIONAL_FRAME_COLUMNS}
                    require(row['phase'] and row['dt_ms'] > 0 and row['step_ms'] >= 0 and row['wall_ms'] >= 0,
                            f'{frame_path}:{index}: invalid phase or timing')
                    require(row['cv_enabled'] is enabled and (enabled or not row['cv_active']),
                            f'{frame_path}:{index}: native controller mode mismatch')
                    key = (row['phase'], row['npc_id'])
                    last = prior.get(key)
                    require(last is None or (row['frame'] > last['frame'] and row['t_game_ms'] > last['t_game_ms']),
                            f'{frame_path}:{index}: non-monotonic actor samples')
                    prior[key] = row
                    run['frames'].append(row)
        except (OSError, ValueError, TypeError) as error:
            run['errors'].append(str(error))
    log = directory / 'bench.log'
    if log.is_file():
        lines = log.read_text().splitlines()
        run['complete'] = 'done' in lines
        run['errors'] += [f'bench error: {line}' for line in lines
                          if 'failed' in line or 'unknown command' in line]
    run['pass'] = combined([False if run['errors'] else True,
                            None if run['missing'] else True,
                            True if run['complete'] is True else None])
    return run


def select(run, kind):
    return [row for row in run['events'] if row['kind'] == kind]


def steep_sliding(run, threshold=.60, required_routes=ROUTES):
    """Measure native human CV contacts; repeated idle diagnostics add no time.

    steep_contact is the production static/kinematic, unwalkable, opposing
    contact after the stair recipe. A sliding sample also needs a grounded
    moving actor and at least 0.1 cm accepted horizontal travel. Completely
    blocked samples are reported separately, rather than called sliding.
    """
    require(0 < threshold < 1, 'advancement threshold must be between zero and one')
    routes = []
    for name in required_routes:
        frames = [row for row in run['frames'] if row['phase'] == name]
        result = {'route': name, 'frames': len(frames), 'cv_frames': sum(row['cv_active'] for row in frames),
                  'cv_move_frames': 0, 'steep_below_threshold_frames': 0, 'sliding_frames': 0,
                  'sliding_episodes': 0, 'sliding_ms': 0, 'longest_episode_ms': 0,
                  'missing_diagnostic_frames': 0, 'episodes': [], 'pass': None}
        active, prior_sequence = {}, {}
        for row in frames:
            actor = row['npc_id']
            eligible = row['cv_active'] and row['moving'] and not row['in_air'] and not row['interactive']
            if not SLIDING_FRAME_COLUMNS <= row.keys():
                if eligible:
                    result['missing_diagnostic_frames'] += 1
                active.pop(actor, None)
                continue
            valid = row['diagnostics_valid'] and row['move_sequence'] > 0 and row['move_dt_ms'] > 0
            if not valid:
                if eligible:
                    result['missing_diagnostic_frames'] += 1
                active.pop(actor, None)
                continue
            sequence = row['move_sequence']
            previous = prior_sequence.get(actor)
            prior_sequence[actor] = sequence
            if not eligible:
                active.pop(actor, None)
                continue
            # A diagnostic persists when the actor does not run its movement
            # again. It is one move, however many bench frames observe it.
            if sequence == previous:
                continue
            requested = math.hypot(row['requested_x_cm'], row['requested_z_cm'])
            accepted = math.hypot(row['accepted_dx_cm'], row['accepted_dz_cm'])
            if requested <= .1:
                active.pop(actor, None)
                continue
            result['cv_move_frames'] += 1
            steep = row['steep_contact'] and row['advancement_ratio'] < threshold
            result['steep_below_threshold_frames'] += steep
            if not steep or accepted <= .1:
                active.pop(actor, None)
                continue
            require(math.hypot(row['steep_normal_x'], row['steep_normal_z']) > .01,
                    f'{name}: steep contact has no horizontal normal')
            result['sliding_frames'] += 1
            duration = row['move_dt_ms']
            result['sliding_ms'] += duration
            episode = active.get(actor)
            if episode is None:
                episode = {'npc_id': actor, 'start_game_ms': row['t_game_ms'] - duration,
                           'end_game_ms': row['t_game_ms'], 'frames': 0, 'duration_ms': 0}
                result['episodes'].append(episode)
                active[actor] = episode
            episode['end_game_ms'] = row['t_game_ms']
            episode['frames'] += 1
            episode['duration_ms'] += duration
        result['sliding_episodes'] = len(result['episodes'])
        result['longest_episode_ms'] = max((row['duration_ms'] for row in result['episodes']), default=0)
        result['pass'] = combined([run['pass'], True if frames and result['cv_frames'] else None,
                                   None if result['missing_diagnostic_frames'] else True])
        routes.append(result)
    return {'pass': combined([row['pass'] for row in routes]), 'advancement_threshold': threshold,
            'routes': routes, 'run': run_status(run),
            'duration_clock': 'Sum of unique native move_dt_ms samples, not repeated idle diagnostics.',
            'definition': 'Grounded moving noninteractive human CV; static/kinematic unwalkable opposing contact after stairs; advancement below threshold and horizontal accepted travel >0.1 cm.'}


def unique_event(run, kind, key=None, value=None):
    rows = [row for row in select(run, kind) if key is None or row.get(key) == value]
    require(len(rows) <= 1, f"{run['directory']}: duplicate {kind} {value or ''}")
    return rows[0] if rows else None


def activity(frames, enabled):
    moving = [row for row in frames if row['moving'] and not row['interactive']]
    active = sum(row['cv_active'] for row in moving)
    return {'frames': len(frames), 'moving_frames': len(moving), 'cv_active_moving_frames': active,
            'cv_active_frames': sum(row['cv_active'] for row in frames),
            'pass': (active > 0 if enabled else active == 0) if moving else None}


def route_result(run, name):
    end = unique_event(run, 'npc_character_route_end', 'route', name)
    frames = [row for row in run['frames'] if row['phase'] == name]
    result = {'success': None, 'elapsed_ms': None, 'stalls': None,
              'max_visual_jump_cm': None, 'max_physical_jump_cm': None,
              'max_above_floor_cm': None, 'native_activity': activity(frames, run['enabled']),
              'pass': None}
    if end is None:
        result['reason'] = 'Missing native route end.'
        return result
    for key in ('elapsed_ms', 'window_ms', 'stalls', 'max_visual_jump_cm',
                'max_physical_jump_cm', 'max_above_floor_cm'):
        if key in end:
            result[key] = number(end[key])
            require(result[key] >= 0, f'{name}: negative {key}')
    if 'success' in end:
        result['success'] = flag(end['success'])
    for key in ('sat', 'standing', 'mug_native_contact'):
        if key in end:
            result[key] = flag(end[key])
    for key in ('mug_mass_kg', 'mug_max_displacement_cm'):
        if key in end:
            result[key] = number(end[key])
    for key in ('start_cm', 'goal_cm', 'end_cm'):
        if key in end:
            result[key] = vector(end[key])
    by_actor = defaultdict(list)
    for row in frames:
        by_actor[row['npc_id']].append(row)
    deltas = [abs(b['visual_y_cm'] - a['visual_y_cm']) for rows in by_actor.values()
              for a, b in zip(rows, rows[1:])]
    result['csv_max_visual_jump_cm'] = max(deltas, default=None)
    result['native_event'] = end
    if name == 'seat':
        active = result['native_activity']['cv_active_frames']
        result['native_activity']['pass'] = (active > 0 if run['enabled'] else active == 0) if frames else None
        result['seating_pass'] = combined([result.get('sat'), result.get('standing')])
    if name == 'tavern':
        result['mug_push_pass'] = (result['mug_native_contact'] and result['mug_max_displacement_cm'] > 1 and
                                   0 < result['mug_mass_kg'] < 10) if all(key in result for key in
                                   ('mug_native_contact', 'mug_max_displacement_cm', 'mug_mass_kg')) else None
    required = ('success', 'elapsed_ms', 'window_ms', 'stalls', 'max_visual_jump_cm',
                'max_physical_jump_cm', 'start_cm', 'goal_cm', 'end_cm')
    checks = [run['pass'], True if all(result.get(key) is not None for key in required) else None,
              True if frames else None, result['native_activity']['pass']]
    if name == 'seat':
        checks.append(result['seating_pass'])
    if name == 'tavern' and run['enabled']:
        checks.append(result['mug_push_pass'])
    result['pass'] = combined(checks)
    return result


def compare_routes(off, on, required_routes=None):
    names = sorted(set(required_routes or ROUTES) | {row['route'] for run in (off, on)
                   for row in select(run, 'npc_character_route_end')})
    result = []
    for name in names:
        before, after = route_result(off, name), route_result(on, name)
        matched = None
        if all(all(key in row for key in ('start_cm', 'goal_cm', 'window_ms')) for row in (before, after)):
            matched = (math.dist(before['start_cm'], after['start_cm']) <= 1 and
                       math.dist(before['goal_cm'], after['goal_cm']) <= 1 and
                       before['window_ms'] == after['window_ms'])
        progress = None
        if all(row['success'] is not None and row['stalls'] is not None for row in (before, after)):
            progress = after['success'] and after['stalls'] <= before['stalls']
        result.append({'route': name, 'off': before, 'on': after,
                       'matched_fixture': matched, 'movement_pass': progress,
                       'pass': combined([before['pass'], after['pass'], matched, progress])})
    return {'routes': result, 'pass': combined([row['pass'] for row in result])}


def cadence(directory, enabled):
    metadata = directory / 'appcadence.csv.json'
    csv_path = directory / 'appcadence.csv'
    if not metadata.is_file() or not csv_path.is_file():
        return {'pass': None, 'reason': 'Missing native application-frame cadence CSV or metadata.'}
    meta = json.loads(metadata.read_text())
    require(meta.get('clock') == 'normal' and meta.get('complete') is True and meta.get('overflow') == 0,
            f'{metadata}: incomplete normal-clock cadence')
    require(meta.get('mode') == ('on' if enabled else 'off'), f'{metadata}: cadence mode mismatch')
    start, window_end = int(meta['start_ns']), int(meta['window_end_ns'])
    require(window_end - start == 30_000_000_000, f'{metadata}: expected 30-second window')
    with csv_path.open(newline='') as source:
        rows = list(csv.DictReader(source))
    require(rows and len(rows) == int(meta['frames']), f'{csv_path}: sample count mismatch')
    prior = start
    for index, row in enumerate(rows):
        end, interval = int(row['end_ns']), int(row['interval_ns'])
        require(int(row['frame']) == index and end > prior and interval == end - prior,
                f'{csv_path}: non-monotonic cadence')
        require(index + 1 == len(rows) or end < window_end, f'{csv_path}: extra frame after window')
        prior = end
    require(prior == int(meta['final_end_ns']) and prior >= window_end, f'{metadata}: missing final frame tail')
    return {'pass': True, 'app_frames': len(rows), 'app_window_ms': (prior - start) / 1e6,
            'mean_fps': 1e9 * len(rows) / (prior - start)}


def performance(run, minimum_nearby):
    all_frames = [row for row in run['frames'] if row['normal_clock'] and not row['captured']]
    result = {'pass': None, 'native_activity': activity(all_frames, run['enabled'])}
    if not all_frames:
        result['reason'] = 'No uncaptured normal-clock physics samples.'
        return result
    unique = {}
    same = ('step_ms', 'wall_ms', 'nearby_humans', 'nearby_cv', 'moving_humans', 'moving_cv')
    for row in all_frames:
        key = (row['frame'], row['t_game_ms'])
        require(key not in unique or all(unique[key].get(field) == row.get(field) for field in same),
                f"{run['directory']}: per-actor copies disagree on world-frame measurement")
        unique[key] = row
    frames = list(unique.values())
    if all('moving_cv' in row and 'moving_humans' in row for row in frames):
        moving = sum(row['moving_humans'] for row in frames)
        active = sum(row['moving_cv'] for row in frames)
        require(all(0 <= row['moving_cv'] <= row['moving_humans'] and
                    row['moving_cv'] <= row['nearby_cv'] for row in frames),
                f"{run['directory']}: invalid native moving-actor counts")
        result['native_activity'].update(moving_actor_frames=moving, cv_active_moving_actor_frames=active,
                                         **{'pass': (active > 0 if run['enabled'] else active == 0)
                                            if moving else None})
    near = [row['nearby_humans'] for row in frames]
    enough = sum(count >= minimum_nearby for count in near) / len(near)
    costs = [row['step_ms'] for row in frames]
    app = cadence(Path(run['directory']), run['enabled'])
    fixture_end = unique_event(run, 'npc_character_measure_end')
    fixture_keys = ('fixture_actors', 'fixture_min_nearby', 'fixture_max_nearby', 'fixture_max_distance_cm')
    fixture = {key: number(fixture_end[key]) for key in fixture_keys
               if fixture_end is not None and key in fixture_end}
    fixture['pass'] = None
    if all(key in fixture for key in fixture_keys):
        require(all(fixture[key] >= 0 for key in fixture_keys),
                f"{run['directory']}: negative native performance fixture metric")
        fixture['pass'] = (fixture['fixture_actors'] == 20 and fixture['fixture_min_nearby'] == 20 and
                           fixture['fixture_max_nearby'] == 20 and fixture['fixture_max_distance_cm'] < 2000)
    result['native_fixture'] = fixture
    result.update(samples=len(frames), physics_p95_ms=percentile(costs), physics_mean_ms=statistics.mean(costs),
                  nearby_humans_min=min(near), nearby_humans_mean=statistics.mean(near), nearby_humans_max=max(near),
                  nearby_humans_minimum=minimum_nearby, nearby_sufficient_sample_fraction=enough,
                  nearby_cv_max=max(row['nearby_cv'] for row in frames), app_cadence=app)
    if 'mean_fps' in app:
        result['mean_fps'] = app['mean_fps']
    result['pass'] = combined([run['pass'], app['pass'], result['native_activity']['pass'],
                               fixture['pass'], enough >= .95])
    return result


def compare_performance(off, on, minimum_nearby):
    before, after = performance(off, minimum_nearby), performance(on, minimum_nearby)
    result = {'off': before, 'on': after, 'physics_p95_limit_delta_ms': .5,
              'fps_drop_limit_percent': 5, 'pass': None}
    if all('mean_fps' in row and 'physics_p95_ms' in row for row in (before, after)):
        delta = after['physics_p95_ms'] - before['physics_p95_ms']
        loss = 100 * (1 - after['mean_fps'] / before['mean_fps'])
        result.update(physics_p95_delta_ms=delta, fps_drop_percent=loss,
                      physics_pass=delta <= .5, fps_pass=loss <= 5)
        result['pass'] = combined([before['pass'], after['pass'], delta <= .5, loss <= 5])
    return result


def stuck_observation(directory, label):
    summaries = sorted(directory.glob('*stuck*.summary.json'))
    if not summaries:
        summaries = sorted(directory.glob('*.summary.json'))
    observations = []
    for path in summaries:
        summary = json.loads(path.read_text())
        observations += [(path, row) for row in summary.get('observations', []) if row.get('sample') == label]
    require(len(observations) <= 1, f'{directory}: duplicate stuck observation {label}')
    if not observations:
        return {'pass': None, 'reason': f'Missing same-sample stuck summary for {label}.'}
    path, row = observations[0]
    logfile = Path(str(path)[:-len('.summary.json')])
    if not logfile.is_file():
        return {'pass': None, 'summary': row, 'reason': f'Missing stuck event log {logfile.name}.'}
    events = []
    for line in logfile.read_text().splitlines():
        if line.strip():
            event = json.loads(line)
            if event.get('type', event.get('kind')) == 'npc_stuck' and event.get('sample') == label:
                events.append(event)
    count = int(row['stuck_events'])
    censored = int(row['censored_events'])
    require(count >= 0 and 0 <= censored <= count, f'{path}: invalid stuck counts')
    require(len(events) == count and sum(flag(event['censored']) for event in events) == censored,
            f'{path}: stuck summary/event counts differ')
    require(row['end_reason'] == 'observation_end' and number(row['simulation_duration_ms']) > 0,
            f'{path}: observation did not finish normally')
    return {'pass': True, 'summary': row, 'events': events, 'summary_file': str(path),
            'stuck_events': count, 'censored_events': censored, 'unique_walkers': int(row['unique_walkers'])}


def window_result(directory, label, enabled, require_runner_summary=False):
    run = load_run(directory, enabled, require_frames=False)
    stuck = stuck_observation(directory, label)
    begin = unique_event(run, 'npc_character_observe_begin', 'window', label)
    observe = unique_event(run, 'npc_character_observe_end', 'window', label)
    summary_path = directory / 'runner-summary.json'
    runner_summary = json.loads(summary_path.read_text()) if summary_path.is_file() else None
    runner_complete = None if require_runner_summary else True
    if runner_summary is not None:
        runner_complete = (runner_summary.get('done') is True and runner_summary.get('loaded') is True and
                           runner_summary.get('mode') == 'lockstep' and
                           runner_summary.get('graphics_device_created') is False and
                           runner_summary.get('window_created') is False and
                           runner_summary.get('audio_device_created') is False and
                           number(runner_summary.get('simulated_ms', 0)) >= WINDOW_MS)
    result = {'directory': str(directory), 'run': run_status(run), 'stuck': stuck,
              'native_observe': observe, 'unique_reached_humans': None,
              'reached_human_ids': None, 'arrival_event_count': None,
              'arrival_event_count_matches': None, 'observation_complete': None,
              'runner_summary': runner_summary, 'runner_complete': runner_complete, 'pass': None}
    if observe is not None:
        required = ('window_ms', 'unique_walkers', 'reached_goals', 'frames', 'cv_actor_frames', 'max_nearby_humans')
        require(all(number(observe[key]) >= 0 for key in required), f'{directory}: negative observe metric')
        arrivals = [row for row in select(run, 'npc_character_goal_arrive') if row.get('window') == label]
        reached_ids = set()
        for row in arrivals:
            npc_id = number(row['npc_id'])
            require(npc_id >= 0 and npc_id.is_integer(), f'{directory}: invalid arrival NPC id')
            reached_ids.add(int(npc_id))
        observation_complete = None
        if begin is not None:
            observation_complete = (abs(number(observe['window_ms']) - WINDOW_MS) <= 100 and
                                    number(observe['t']) - number(begin['t']) == number(observe['window_ms']) and
                                    all(number(begin['t']) <= number(row['t']) <= number(observe['t']) for row in arrivals) and
                                    number(begin['radius_cm']) == number(observe['radius_cm']) and
                                    flag(begin['humans_only']))
        result.update(unique_reached_humans=len(reached_ids), reached_human_ids=sorted(reached_ids),
                      arrival_event_count=len(arrivals),
                      arrival_event_count_matches=len(arrivals) == number(observe['reached_goals']),
                      observation_complete=observation_complete)
        native = number(observe['frames']) > 0 and (number(observe['cv_actor_frames']) > 0 if enabled
                                                   else number(observe['cv_actor_frames']) == 0)
        summary = stuck.get('summary')
        same_sample = None
        if summary is not None:
            # The same name is required above; permit at most one native tick at each edge.
            same_sample = (abs(number(observe['window_ms']) - number(summary['simulation_duration_ms'])) <= 500 and
                           number(observe['radius_cm']) == number(summary['radius_cm']))
        result['same_sample'] = same_sample
        result['pass'] = combined([run['pass'], stuck['pass'], native, same_sample,
                                   result['arrival_event_count_matches'],
                                   flag(observe['humans_only']), observation_complete, runner_complete])
    return result


def compare_windows(directory, spots=None, require_runner_summary=False):
    rows = []
    labels = [f'{spot}-{hour}' for spot in dict.fromkeys(spots or SPOTS) for hour in (7, 20)]
    for label in labels:
        before = window_result(directory / 'off' / label, label, False, require_runner_summary)
        after = window_result(directory / 'on' / label, label, True, require_runner_summary)
        matched = progress = None
        a, b = before['stuck'].get('summary'), after['stuck'].get('summary')
        if a is not None and b is not None:
            matched = (a['sample'] == b['sample'] and a['world'] == b['world'] and
                       math.dist(vector(a['center']), vector(b['center'])) <= 1 and
                       number(a['radius_cm']) == number(b['radius_cm']) and
                       abs(number(a['start_game_time_ms']) - number(b['start_game_time_ms'])) <= 1000 and
                       abs(number(a['end_game_time_ms']) - number(b['end_game_time_ms'])) <= 1000 and
                       abs(number(a['simulation_duration_ms']) - number(b['simulation_duration_ms'])) <= 500)
        native_off, native_on = before['native_observe'], after['native_observe']
        if native_off is not None and native_on is not None and a is not None and b is not None:
            progress = (number(b['stuck_events']) <= number(a['stuck_events']) and
                        number(native_on['reached_goals']) >= number(native_off['reached_goals']) and
                        after['unique_reached_humans'] >= before['unique_reached_humans'])
        rows.append({'window': label, 'off': before, 'on': after, 'matched_window': matched,
                     'progress_pass': progress,
                     'pass': combined([before['pass'], after['pass'], matched, progress])})
    return {'windows': rows, 'selected_windows': labels, 'full_suite': set(labels) == set(WINDOWS),
            'expected_runs': len(labels) * 2, 'pass': combined([row['pass'] for row in rows])}


def run_status(run):
    return {key: run[key] for key in ('directory', 'complete', 'missing', 'errors', 'pass')}


def display(value):
    if value is None:
        return '—'
    if isinstance(value, bool):
        return 'да' if value else 'нет'
    if isinstance(value, float):
        return f'{value:.3f}'.rstrip('0').rstrip('.')
    return str(value).replace('|', '\\|')


def state(value):
    return {True: 'PASS', False: 'FAIL', None: 'НЕПОЛНО'}[value]


def markdown(result):
    lines = [f"NPC CharacterVirtual ON/OFF: **{state(result['pass'])}**. «—» означает отсутствие данных."]
    if 'error' in result:
        return '\n'.join(lines + ['', result['error']]) + '\n'
    lines += ['', '| Маршрут | Дошёл OFF / ON | Время, мс OFF / ON | Застревания OFF / ON | Макс. ΔY модели за кадр, см OFF / ON | Кадры CV ON всего / в движении | Итог |',
              '|---|---|---:|---:|---:|---:|---|']
    for row in result['routes']['routes']:
        before, after = row['off'], row['on']
        pair = lambda key: f'{display(before.get(key))} / {display(after.get(key))}'
        native = after['native_activity']
        lines.append(f"| {display(row['route'])} | {pair('success')} | {pair('elapsed_ms')} | {pair('stalls')} | {pair('max_visual_jump_cm')} | {display(native['cv_active_frames'])} / {display(native['cv_active_moving_frames'])} | {state(row['pass'])} |")
    tavern = next((row for row in result['routes']['routes'] if row['route'] == 'tavern'), None)
    if tavern is not None:
        lines += ['', '| Кружка в таверне | OFF | ON |', '|---|---:|---:|']
        for label, key in (('Масса, кг', 'mug_mass_kg'), ('Макс. смещение, см', 'mug_max_displacement_cm'),
                           ('Нативный контакт NPC', 'mug_native_contact'), ('Толкнул', 'mug_push_pass'),
                           ('Макс. высота корня над полом, см', 'max_above_floor_cm')):
            lines.append(f"| {label} | {display(tavern['off'].get(key))} | {display(tavern['on'].get(key))} |")
        lines += ['', 'Высота над полом приведена как измерение; она сама по себе не доказывает отсутствие опоры на кружку.']
    selected = result['windows']['selected_windows']
    coverage = 'полный набор' if result['windows']['full_suite'] else 'выбранная часть набора'
    lines += ['', f"Окна: {len(selected)}/8 пар OFF/ON ({coverage}); требуются все {result['windows']['expected_runs']} запусков выбранных пар по {WINDOW_MS} мс наблюдения.",
              '', '| Окно | Застревания OFF / ON | Цензурированные OFF / ON | Уникальные идущие (детектор) OFF / ON | Уникальные идущие (native observe) OFF / ON | Людей дошло OFF / ON | Достигнутые цели OFF / ON | CV actor-frames ON | Та же выборка | Итог |',
              '|---|---:|---:|---:|---:|---:|---:|---:|---|---|']
    for row in result['windows']['windows']:
        before, after = row['off'], row['on']
        stuck_pair = lambda key: f"{display(before['stuck'].get(key))} / {display(after['stuck'].get(key))}"
        native_pair = lambda key: f"{display((before['native_observe'] or {}).get(key))} / {display((after['native_observe'] or {}).get(key))}"
        unique_pair = f"{display(before['unique_reached_humans'])} / {display(after['unique_reached_humans'])}"
        lines.append(f"| {row['window']} | {stuck_pair('stuck_events')} | {stuck_pair('censored_events')} | {stuck_pair('unique_walkers')} | {native_pair('unique_walkers')} | {unique_pair} | {native_pair('reached_goals')} | {display((after['native_observe'] or {}).get('cv_actor_frames'))} | {display(row['matched_window'])} | {state(row['pass'])} |")
    perf = result['performance']
    a, b = perf['off'], perf['on']
    lines += ['', '| Производительность | OFF | ON | Изменение | Лимит |', '|---|---:|---:|---:|---:|',
              f"| Шаг физики p95, мс | {display(a.get('physics_p95_ms'))} | {display(b.get('physics_p95_ms'))} | {display(perf.get('physics_p95_delta_ms'))} | +0,5 мс |",
              f"| Средний FPS приложения | {display(a.get('mean_fps'))} | {display(b.get('mean_fps'))} | {display(perf.get('fps_drop_percent'))}% потери | 5% |",
              f"| NPC-люди рядом, мин / среднее / макс | {' / '.join(display(a.get('nearby_humans_' + key)) for key in ('min', 'mean', 'max'))} | {' / '.join(display(b.get('nearby_humans_' + key)) for key in ('min', 'mean', 'max'))} | — | ≥{result['minimum_nearby_humans']} в ≥95% кадров |",
              f"| Акторы стенда в 20 м, мин / макс | {' / '.join(display(a.get('native_fixture', {}).get('fixture_' + key + '_nearby')) for key in ('min', 'max'))} | {' / '.join(display(b.get('native_fixture', {}).get('fixture_' + key + '_nearby')) for key in ('min', 'max'))} | — | 20 / 20 во всех тиках |",
              f"| Макс. расстояние актора стенда, см | {display(a.get('native_fixture', {}).get('fixture_max_distance_cm'))} | {display(b.get('native_fixture', {}).get('fixture_max_distance_cm'))} | — | <2000 |",
              '', 'FPS — завершённые кадры приложения за обычное 30-секундное окно; физика — только кадры без видеозаписи и с обычными часами. Флаг ON сам по себе не доказывает работу контроллера.',
              '', 'Детектор учитывает всех подходящих NPC, native observe — людей; счётчики уникальных идущих независимы и не означают число дошедших. «Людей дошло» — уникальные npc_id из событий достижения цели того же окна. Число событий сверяется с native reached_goals; один человек может достичь нескольких целей. Цензурированные застревания включены в общее число.']
    for section in ('routes', 'performance'):
        for mode in ('off', 'on'):
            run = result['runs'][section][mode]
            if run['missing'] or run['errors'] or run['complete'] is not True:
                lines += ['', f"{section} {mode.upper()}: {state(run['pass'])}; завершён: {display(run['complete'])}; отсутствуют: {', '.join(run['missing']) or '—'}; ошибки: {'; '.join(run['errors']) or '—'}."]
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path, nargs='?', default=Path('.bench/npc-character-virtual'))
    parser.add_argument('--routes-off', type=Path)
    parser.add_argument('--routes-on', type=Path)
    parser.add_argument('--performance-off', '--perf-off', type=Path)
    parser.add_argument('--performance-on', '--perf-on', type=Path)
    parser.add_argument('--windows', type=Path)
    parser.add_argument('--window-spot', action='append', choices=SPOTS,
                        help='Repeat to compare selected spots (both hours and both modes); default all four')
    parser.add_argument('--check-window', type=Path, help='Validate one completed observation and exit')
    parser.add_argument('--window-label', choices=WINDOWS)
    parser.add_argument('--window-mode', choices=('off', 'on'))
    parser.add_argument('--require-runner-summary', action='store_true', help='Require completed CPU-runner metadata for every selected window')
    parser.add_argument('--required-route', action='append', default=[], help='Repeat to require a fixture even when no route end was recorded')
    parser.add_argument('--steep-sliding', action='store_true',
                        help='Emit human steep-contact sliding JSON for one ON route run (directory or --routes-on)')
    parser.add_argument('--advancement-threshold', type=float, default=.60,
                        help='Advancement ratio below which a steep contact is counted (default 0.60)')
    parser.add_argument('--min-nearby-humans', type=int, default=18)
    parser.add_argument('--output', type=Path, help='Markdown output and JSON beside it; .json selects the JSON destination')
    args = parser.parse_args()
    if args.steep_sliding:
        try:
            run = load_run(args.routes_on or args.directory, True)
            result = steep_sliding(run, args.advancement_threshold, args.required_route or ROUTES)
        except (OSError, ValueError, KeyError, TypeError) as error:
            result = {'pass': False, 'error': str(error)}
        print(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False))
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False) + '\n')
        return 0 if result['pass'] is True else 1 if result['pass'] is False else 2
    if args.check_window is not None:
        if args.window_label is None or args.window_mode is None:
            parser.error('--check-window requires --window-label and --window-mode')
        try:
            checked = window_result(args.check_window, args.window_label, args.window_mode == 'on',
                                    args.require_runner_summary)
            result = {key: checked[key] for key in ('pass', 'unique_reached_humans', 'arrival_event_count',
                       'arrival_event_count_matches', 'observation_complete', 'runner_complete')}
            result.update(window=args.window_label, mode=args.window_mode, run=checked['run'],
                          stuck_pass=checked['stuck']['pass'], same_sample=checked.get('same_sample'))
        except (OSError, ValueError, KeyError, TypeError) as error:
            result = {'pass': False, 'error': str(error)}
        print(json.dumps(result, ensure_ascii=False, allow_nan=False))
        return 0 if result['pass'] is True else 1 if result['pass'] is False else 2
    try:
        require(args.min_nearby_humans > 0, 'minimum nearby humans must be positive')
        root = args.directory
        routes_off = load_run(args.routes_off or root / 'routes-off', False)
        routes_on = load_run(args.routes_on or root / 'routes-on', True)
        perf_off = load_run(args.performance_off or root / 'performance-off', False)
        perf_on = load_run(args.performance_on or root / 'performance-on', True)
        routes = compare_routes(routes_off, routes_on, args.required_route)
        perf = compare_performance(perf_off, perf_on, args.min_nearby_humans)
        windows = compare_windows(args.windows or root / 'windows', args.window_spot, args.require_runner_summary)
        result = {'pass': combined([routes['pass'], perf['pass'], windows['pass']]),
                  'minimum_nearby_humans': args.min_nearby_humans, 'routes': routes,
                  'performance': perf, 'windows': windows,
                  'runs': {'routes': {'off': run_status(routes_off), 'on': run_status(routes_on)},
                           'performance': {'off': run_status(perf_off), 'on': run_status(perf_on)}},
                  'notes': ['null means insufficient evidence, never a pass.',
                            'This analyzer covers route, natural-window and performance evidence. Other native fixtures, save/reload, m5b and follower e2e require their own checks.']}
    except (OSError, ValueError, KeyError, TypeError) as error:
        result = {'pass': False, 'error': str(error)}
    report = markdown(result)
    print(report, end='')
    if args.output:
        md_path = args.output.with_suffix('.md') if args.output.suffix == '.json' else args.output
        json_path = args.output if args.output.suffix == '.json' else args.output.with_suffix('.json')
        md_path.parent.mkdir(parents=True, exist_ok=True)
        md_path.write_text(report)
        json_path.write_text(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False) + '\n')
    return 0 if result['pass'] is True else 1 if result['pass'] is False else 2


if __name__ == '__main__':
    raise SystemExit(main())
