#!/usr/bin/env python3
"""Verify real arrow pins from their actual mesh draw transforms."""
import argparse
import json
import math
import re
import statistics
from pathlib import Path


def multiply(a, b):
    return [sum(a[k*4+r]*b[c*4+k] for k in range(4)) for c in range(4) for r in range(4)]


def delta(a, b):
    assert len(a) == len(b) == 16 and all(math.isfinite(v) for v in a+b)
    return max(abs(x-y) for x, y in zip(a, b))


def travel(points):
    return max((math.dist(points[0], point) for point in points), default=0)


# How deep a stuck arrow's tip goes past the hit by the surface's material group (common/world/arrowdepth.cpp,
# reports/physics/2026-11-19-arrow-pin-depth): METAL 1 and STONE 2 — 2 cm, WOOD 3 — 8 cm, the rest 15 cm.
def world_depth(material):
    return {1: 2., 2: 2., 3: 8.}.get(material, 15.)


def pin_depth(matrix, tip_length, point):
    """The tip (the mesh's origin + X·(−tip_length)): its depth past the hit along the flight (−X), its distance off it."""
    x = matrix[0:3]
    scale = math.hypot(*x)
    assert scale > 1e-6, 'arrow matrix has no X axis'
    flight = [-v/scale for v in x]
    tip = [o-v*tip_length for o, v in zip(matrix[12:15], x)]
    offset = [t-p for t, p in zip(tip, point)]
    depth = sum(o*f for o, f in zip(offset, flight))
    return depth, math.dist(offset, [f*depth for f in flight])


def costs(rows):
    values = [r['physics_ms'] for r in rows]
    assert values and all(math.isfinite(v) and v >= 0 for v in values)
    return {'samples': len(values), 'mean_ms': statistics.mean(values),
            'p95_ms': sorted(values)[math.ceil(len(values)*.95)-1], 'max_ms': max(values)}


def validate(run):
    assert 'done' in (run/'bench.log').read_text().splitlines(), 'unfinished scenario'
    data = [json.loads(line) for line in (run/'events.jsonl').read_text().splitlines()]
    assert not any(e['kind'] == 'ragdoll_error' for e in data), 'fixture error'
    setup = [e for e in data if e['kind'] == 'arrow_sticking_setup']
    assert len(setup) == 1 and setup[0]['default_enabled'], 'feature default must be ON'
    names = ('off_npc', 'off_world', 'live', 'motion', 'ko', 'corpse', 'getup',
             'death', 'dead_corpse', 'world', 'toggle_off', 'rearm', 'remove')
    expected_npc = (0, 0, 1, 1, 2, 2, 2, 3, 3, 3, 0, 1, 0)
    expected_world = (0, 0, 0, 0, 0, 1, 1, 1, 2, 3, 0, 0, 0)
    cases = [e for e in data if e['kind'] == 'arrow_sticking_case']
    assert [e['case'] for e in cases] == list(names), 'missing or reordered cases'
    frames = [e for e in data if e['kind'] == 'arrow_sticking_frame' and e['case'] == e['seg']]
    rows = {name: [r for r in frames if r['case'] == name] for name in names}
    assert all(rows.values()), 'missing recorded draw-transform samples'
    initial_bodies = rows['off_npc'][-1]['world_bodies']
    local = {}
    world_reference = {}
    maximum_error = maximum_local_change = maximum_world_drift = 0
    result = {}
    for name, npc_count, world_count in zip(names, expected_npc, expected_world):
        recorded = rows[name]
        assert all(b['t_game_ms'] > a['t_game_ms'] for a, b in zip(recorded, recorded[1:])), name
        assert recorded[-1]['npc_pin_count'] == npc_count and recorded[-1]['world_pin_count'] == world_count, (name, 'wrong final pin count')
        if name == 'rearm':
            local.clear()
            world_reference.clear()
        for row in recorded:
            assert row['npc_pin_count'] == len(row['npc_pins']) and row['world_pin_count'] == len(row['world_pins'])
            assert row['all_npc_pin_count'] == row['npc_pin_count'], (name, 'orphan owner pins')
            assert row['world_bodies'] <= initial_bodies, (name, 'arrows added collision bodies')
            assert not (row['living_parts'] and row['corpse_parts']), (name, 'duplicate native bodies')
            for pin in row['npc_pins']:
                error = delta(pin['world'], pin['expected'])
                oracle = multiply(pin['render_bone'], pin['local'])
                assert delta(oracle, pin['expected']) < .03, 'expected transform is not bone * local'
                maximum_error = max(maximum_error, error)
                assert error < .03, (name, 'actual draw node does not follow rendered bone', error)
                key = pin['index']
                if key in local:
                    change = delta(pin['local'], local[key])
                    maximum_local_change = max(maximum_local_change, change)
                    assert change < 1e-5, (name, 'attachment local frame changed')
                else:
                    local[key] = pin['local']
            for pin in row['world_pins']:
                key = pin['index']
                if key in world_reference:
                    drift = delta(pin['world'], world_reference[key])
                    maximum_world_drift = max(maximum_world_drift, drift)
                    assert drift < 1e-4, (name, 'world arrow bounced or moved after pinning', drift)
                else:
                    world_reference[key] = pin['world']
        result[name] = {'samples': len(recorded), 'npc_pins': npc_count, 'world_pins': world_count}

    for name in ('off_npc', 'off_world'):
        assert all(not r['feature'] and not r['npc_pins'] and not r['world_pins'] for r in rows[name]), name
    note = lambda name: [e.get('text', '') for e in data if e['kind'] == 'note' and e['seg'] == name]
    assert any(s.startswith('arrow hit BAU_904_BAUER:') for s in note('off_npc')), 'OFF NPC projectile never hit'
    assert any(s.startswith('arrow missed: came down') for s in note('off_world')), 'OFF world projectile never collided'
    for name in ('live', 'ko', 'death', 'rearm'):
        assert sum(s.startswith('arrow stuck npc BAU_904_BAUER bone ') for s in note(name)) == 1, (name, 'missing real NPC projectile pin')
    for name in ('corpse', 'dead_corpse'):
        assert not any(s.startswith(('arrow stuck corpse ', 'arrow stuck npc ', 'arrow hit BAU_904_BAUER:')) for s in note(name)), (name, 'new arrow intercepted by a down body')
        assert all((r['ko'] or r['dead']) and r['corpse_parts'] == 24 for r in rows[name]), (name, 'missing physical down body')
        assert sum(s.startswith('arrow stuck world: at ') for s in note(name)) == 1, (name, 'projectile did not reach the landscape behind the body')
    assert all(r['npc_pin_count'] == 2 for r in rows['getup']), 'getup lost an attached arrow'
    assert rows['getup'][-1]['living_parts'] == 24 and not rows['getup'][-1]['ko'] and not rows['getup'][-1]['dead'], 'getup did not finish'
    assert any(r['recovering'] for r in rows['getup']), 'recovery blend was not observed'
    assert rows['ko'][-1]['ko'] and rows['ko'][-1]['corpse_parts'] == 24, 'KO handoff failed'
    assert rows['death'][-1]['dead'] and rows['death'][-1]['corpse_parts'] == 24, 'lethal handoff failed'
    assert all(r['world_bodies'] == initial_bodies for name in ('live', 'motion', 'corpse', 'dead_corpse', 'world', 'rearm') for r in rows[name]), 'stable native count changed'
    moved = travel([r['root_cm'] for r in rows['motion']])
    yaw = rows['motion'][0]['yaw_deg']
    turned = max(abs((r['yaw_deg']-yaw+180)%360-180) for r in rows['motion'])
    arrow_travel = travel([r['npc_pins'][0]['world'][12:15] for r in rows['motion']])
    assert moved > 50 and turned > 25 and arrow_travel > 50, ('motion missing', moved, turned, arrow_travel)
    physical_draw_delta = max(math.dist(pin['world'][12:15], multiply(pin['animation_bone'], pin['local'])[12:15])
                              for row in rows['live'] if row['weight'] > 0 for pin in row['npc_pins'])
    assert physical_draw_delta > .05, 'pins followed ordinary query animation instead of physical rendering'
    world_notes = [s for s in note('world') if s.startswith('arrow stuck world: at ')]
    assert len(world_notes) == 1, 'missing real world collision metadata'
    match = re.match(r'arrow stuck world: at (\S+) (\S+) (\S+) normal (\S+) (\S+) (\S+) material (\d+)(?: tip (\S+))?', world_notes[0])
    assert match, 'world hit lacks point/normal/material'
    assert match[8] is not None, 'world hit note lacks the tip length: a log of the engine before the arrow pin depth'
    point = [float(match[i]) for i in (1, 2, 3)]
    material, tip_length = int(match[7]), float(match[8])
    assert tip_length > 0, ('arrow mesh has no tip length', tip_length)
    world_before = cases[names.index('world')]['world_pins_before']
    actual = next(r['world_pins'][world_before]['world'] for r in rows['world'] if len(r['world_pins']) > world_before)
    tip_depth, tip_off = pin_depth(actual, tip_length, point)
    expected_depth = world_depth(material)
    assert abs(tip_depth-expected_depth) <= 1 and tip_off < 1, \
        ('world arrow tip not at its material depth past the hit', material, tip_depth, expected_depth, tip_off)
    toggle = cases[names.index('toggle_off')]
    assert toggle['npc_pins_before'] == 3 and toggle['world_pins_before'] >= 1, 'toggle did not cover both owner types'
    assert not rows['toggle_off'][-1]['feature'] and rows['toggle_off'][-1]['all_npc_pin_count'] == 0
    removed = [e for e in data if e['kind'] == 'arrow_sticking_remove']
    assert len(removed) == 1 and removed[0]['npc_pins_before'] == 1 and removed[0]['npc_pins_after'] == 0
    assert removed[0]['bodies_before']-removed[0]['bodies_after'] == 24 and removed[0]['world_pins_before'] == removed[0]['world_pins_after'] == 0
    return {'pass': True, 'cases': result, 'actual_draw_max_matrix_error': maximum_error,
            'attachment_max_local_change': maximum_local_change, 'world_max_matrix_drift': maximum_world_drift,
            'world_tip_depth_cm': tip_depth, 'world_tip_expected_cm': expected_depth, 'world_tip_off_line_cm': tip_off, 'motion_root_travel_cm': moved,
            'motion_turn_deg': turned, 'motion_arrow_travel_cm': arrow_travel,
            'physical_render_vs_query_arrow_delta_cm': physical_draw_delta,
            'physics_step': {mode: costs([r for r in frames if r['feature'] == (mode == 'on')]) for mode in ('off', 'on')},
            'remove': removed[0], 'note': 'Actual mesh draw nodes; every pin comes from a real projectile. Existing NPC pins follow KO, getup and death; new arrows do not intercept down bodies and can stop in the landscape behind them. World pins are created only after the original terminal collision, preserving ricochets. Native contracts separately prove corpse passage and reflection. KO is bench-forced after a real hit. Pin persistence across saves is outside this fixture.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    try:
        result = validate(args.directory)
    except (AssertionError, KeyError, ValueError, OSError, TypeError) as error:
        result = {'pass': False, 'error': str(error)}
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))
    return 0 if result['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
