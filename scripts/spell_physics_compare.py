#!/usr/bin/env python3
"""Read real spell dispatch, native motion, field duration and freeze poses."""
import argparse
import json
import math
import re
import statistics
from pathlib import Path


def length(v):
    return math.sqrt(sum(x*x for x in v))


def travel(points):
    return max(math.dist(points[0], p) for p in points)


def cost(rows):
    values = [r['physics_ms'] for r in rows]
    assert values and all(math.isfinite(v) and v >= 0 for v in values)
    return {'mean_ms': statistics.mean(values), 'p95_ms': sorted(values)[math.ceil(len(values)*.95)-1], 'max_ms': max(values)}


def segments(data, names, ids):
    errors = [e for e in data if e['kind'] == 'ragdoll_error' and e['seg'] in names]
    assert not errors, errors
    frames = {n: [e for e in data if e['kind'] == 'spell_physics_frame' and e['case'] == n and e['seg'] == n] for n in names}
    result = {}
    for name, spell_id in zip(names, ids):
        rows = frames[name]
        stops = [e for e in data if e['kind'] == 'rec_stop' and e['seg'] == name]
        assert len(stops) == 1 and stops[0]['frames'] >= len(rows), (name, 'incomplete capture segment')
        assert rows and all(b['t_game_ms'] > a['t_game_ms'] for a, b in zip(rows, rows[1:])), name
        casts = [e for e in data if e['kind'] == 'spell_physics_cast' and e['seg'] == name]
        assert [e['id'] for e in casts] == ([71, 33] if name == 'airborne_ice' else [spell_id]), (name, 'missing real cast')
        for cast in casts:
            assert cast['begin_result'] in (2, 3), (name, 'real cast rejected')
            rune = next(e for e in data if e['kind'] == 'spell_physics_rune' and e['id'] == cast['id'])
            assert cast['active_item'] == rune['class_id'], (name, 'wrong active rune')
            assert rune['projectile'] == (cast['id'] != 71), (name, 'wrong projectile dispatch')
        assert all(not (a['living_parts'] and a['corpse_parts']) for r in rows for a in r['actors'])
        result[name] = {'samples': len(rows), 'cast_id': spell_id, 'physics_step': cost(rows)}
    return frames, result


def validate(run):
    data = [json.loads(line) for line in (run/'events.jsonl').read_text().splitlines()]
    names = ('wind_off', 'wind', 'whirlwind', 'instant_fireball', 'charge_fireball', 'ice', 'ice_off')
    ids = (26, 26, 71, 23, 30, 33, 33)
    assert [e['case'] for e in data if e['kind'] == 'spell_physics_case'][:7] == list(names)
    assert next(e for e in data if e['kind'] == 'spell_physics_setup')['default_enabled']
    frames, result = segments(data, names, ids)
    notes = lambda n: [e for e in data if e['kind'] == 'note' and e['seg'] == n]

    wind = frames['wind']
    actors = lambda rows, role: [next(a for a in r['actors'] if a['role'] == role) for r in rows]
    human, troll = actors(wind, 'human'), actors(wind, 'troll')
    assert abs(human[0]['mass_kg']-75) < .1 and abs(troll[0]['mass_kg']-600) < .1
    speed = {role: max(length(a['spell_velocity_cm_s']) for a in actors(wind, role)) for role in ('human', 'troll', 'behind', 'outside_cone')}
    assert speed['human'] > 100 and all(speed[r] < .01 for r in ('troll', 'behind', 'outside_cone')), ('wind mass/cone controls', speed)
    assert any(e['text'].startswith('spell physics impact id 26 ') for e in notes('wind'))
    assert not any(e['text'].startswith('spell physics impact ') for e in notes('wind_off'))
    assert any('spell, by PC_HERO' in e['text'] for e in notes('wind_off')), 'OFF lacked real spell damage'
    result['wind']['peak_velocity_cm_s'] = speed
    result['wind']['human_travel_cm'] = travel([a['position_cm'] for a in human])

    field = frames['whirlwind']
    starts = [e for e in notes('whirlwind') if e['text'].startswith('spell physics field start id 71 ')]
    ends = [e for e in notes('whirlwind') if e['text'].startswith('spell physics field end id 71 ')]
    assert len(starts) == len(ends) == 1
    lifetime = ends[0]['t']-starts[0]['t']
    assert 9900 <= lifetime <= 10100 and field[-1]['fields'] == 0 and max(r['fields'] for r in field) == 1
    match = re.search(r'at (\S+) (\S+) (\S+) duration', starts[0]['text']);assert match
    center = [float(match[i]) for i in (1, 2, 3)]
    active_items = [p for r in field if r['fields'] for p in r['items']]
    up = max(p['velocity_cm_s'][1] for p in active_items)
    tangent = max(abs((p['position_cm'][0]-center[0])*p['velocity_cm_s'][2]-(p['position_cm'][2]-center[2])*p['velocity_cm_s'][0]) for p in active_items)
    assert up > 50 and tangent > 100, ('whirlwind lacks real upward/tangent item motion', up, tangent)
    human_up = max(a['spell_velocity_cm_s'][1] for a in actors(field, 'human'))
    assert human_up > 50, 'whirlwind did not lift living body'
    result['whirlwind'].update({'lifetime_ms': lifetime, 'item_peak_up_cm_s': up, 'item_peak_tangent_cm2_s': tangent, 'human_peak_up_cm_s': human_up})

    for name, spell_id in (('instant_fireball', 23), ('charge_fireball', 30)):
        rows = frames[name]
        impact = [e for e in notes(name) if e['text'].startswith(f'spell physics impact id {spell_id} ')]
        assert len(impact) == 1, (name, 'missing actual projectile impact')
        impact_items = int(impact[0]['text'].rsplit('items ', 1)[1].split()[0])
        assert impact_items > 0, (name, 'impact missed every loose prop')
        assert any(p['sleeping'] for r in rows[:5] for p in r['items']), (name, 'ground props were not at rest before impact')
        peak = max(length(p['velocity_cm_s']) for r in rows for p in r['items'])
        displacement = max(travel([r['items'][i]['position_cm'] for r in rows]) for i in range(len(rows[0]['items'])))
        corpse = actors(rows, 'corpse')
        assert any(a['sleeping'] for a in corpse[:5]) and any(not a['sleeping'] for a in corpse), (name, 'sleeping ragdoll did not wake')
        corpse_travel = travel([a['position_cm'] for a in corpse])
        assert peak > 50 and displacement > 10 and corpse_travel > 1, (name, 'radial physics absent', peak, displacement, corpse_travel)
        result[name].update({'impact_items': impact_items, 'item_peak_cm_s': peak, 'item_travel_cm': displacement, 'corpse_travel_cm': corpse_travel})

    ice = frames['ice'];frozen = [r for r in ice if r['actors'][0]['frozen']]
    assert frozen and 19000 <= frozen[-1]['t_game_ms']-frozen[0]['t_game_ms'] <= 25000, 'actual freeze state duration/End failed'
    held = frozen[5:-3];assert held
    reference = held[0]['actors'][0]['bones']
    pose_error = max(abs(x-y) for r in held for b, old in zip(r['actors'][0]['bones'], reference) for x, y in zip(b['render'], old['render']))
    native_error = max(math.dist(b['native'][12:15], b['render'][12:15]) for r in held for b in r['actors'][0]['bones'])
    assert all(r['actors'][0]['kinematic'] and r['actors'][0]['state'].upper() == 'ZS_MAGICFREEZE' for r in held)
    assert pose_error < .05 and native_error < .1, ('frozen real pose/native rig drifted', pose_error, native_error)
    assert not ice[-1]['actors'][0]['frozen'] and ice[-1]['actors'][0]['state'].upper() != 'ZS_MAGICFREEZE', 'real state End did not release freeze'
    off = frames['ice_off']
    assert any(r['actors'][0]['frozen'] for r in off) and not off[-1]['actors'][0]['frozen'] and not off[-1]['feature'], 'toggle OFF did not release physics freeze'
    result['ice'].update({'frozen_ms': frozen[-1]['t_game_ms']-frozen[0]['t_game_ms'], 'render_max_matrix_drift': pose_error, 'native_position_error_cm': native_error, 'real_end': True})
    cleanups = [e for e in data if e['kind'] == 'spell_physics_cleanup']
    assert cleanups and all(e['before']-e['after'] == e['expected_parts'] for e in cleanups), 'native cleanup leaked bodies'
    return {'pass': True, 'cases': result, 'physics_step': {'off': cost(frames['wind_off']), 'on': cost(wind)}, 'cleanup_count': len(cleanups)}


def validate_airborne(run):
    assert 'done' in (run/'bench.log').read_text().splitlines(), 'unfinished airborne scenario'
    data = [json.loads(line) for line in (run/'events.jsonl').read_text().splitlines()]
    errors = [e for e in data if e['kind'] == 'ragdoll_error']
    assert not errors, errors
    frames, result = segments(data, ('airborne_ice',), (71,))
    air = frames['airborne_ice']
    air_cast = next(e for e in data if e['kind'] == 'spell_physics_cast' and e['seg'] == 'airborne_ice' and e['id'] == 33)
    assert air_cast['target_in_air'], 'Icecube was not cast at an airborne target'
    air_frozen = [r for r in air if r['actors'][0]['frozen']]
    assert air_frozen and air_frozen[0]['actors'][0]['in_air'] and all(r['actors'][0]['kinematic'] for r in air_frozen), 'real airborne freeze was not admitted/held'
    assert not air[-1]['actors'][0]['frozen'] and air[-1]['actors'][0]['state'].upper() != 'ZS_MAGICFREEZE', 'airborne freeze real End missing'
    result['airborne_ice'].update({'target_in_air_at_cast': True, 'frozen_entry_in_air': True, 'frozen_ms': air_frozen[-1]['t_game_ms']-air_frozen[0]['t_game_ms'], 'real_end': True})
    cleanups = [e for e in data if e['kind'] == 'spell_physics_cleanup']
    assert cleanups and all(e['before']-e['after'] == e['expected_parts'] for e in cleanups), 'native cleanup leaked bodies'
    result['airborne_ice']['cleanup_count'] = len(cleanups)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__);parser.add_argument('directory', type=Path);parser.add_argument('--core', action='store_true', help='Validate the seven completed required segments (default)');parser.add_argument('--air', '--airborne', dest='airborne', type=Path, help='Optional separate airborne scenario');parser.add_argument('--output', type=Path, required=True);args=parser.parse_args()
    try:
        result=validate(args.directory)
        if args.airborne:
            result['cases'].update(validate_airborne(args.airborne))
    except (AssertionError, KeyError, ValueError, OSError, TypeError, StopIteration) as error: result={'pass':False,'error':str(error)}
    args.output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2));return 0 if result['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
