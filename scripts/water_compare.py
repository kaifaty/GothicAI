#!/usr/bin/env python3
"""Check the water bench's native measurements and fresh-process save restore."""
import argparse
import csv
import json
import math
from pathlib import Path


# Stock script materials of the four genuine world Items in prepareWaterFixture.
# Keep this expectation independent of mass / measured buoyancy-envelope volume:
# a construction bug must not turn a floating sword into a passing test.
ITEM_DENSITY_KG_M3 = {'arrow': 700, 'wood_item': 700, 'sword': 7800, 'stone': 2200}
# Wooden Vobs keep their native push masses. Their water displacement instead
# follows the fixture class, and pieces use the independent solid-wood target.
VOB_SUBMERGED_FRACTION = {'barrel': 1 / 3, 'crate': .5, 'table': .9}
WOOD_FRAGMENT_SUBMERGED_FRACTION = .6


def require(condition, message):
    if not condition:
        raise ValueError(message)


def events(directory):
    require('done' in (directory / 'bench.log').read_text().splitlines(),
            f'{directory}: scenario did not finish')
    rows = [json.loads(line) for line in (directory / 'events.jsonl').read_text().splitlines()]
    require(not any(row['kind'].endswith('_error') for row in rows),
            f'{directory}: fixture preparation failed')
    return rows


def checkpoint(rows, label):
    found = [row for row in rows if row['kind'] == 'water_checkpoint' and row['label'] == label]
    require(len(found) == 1, f'expected one water checkpoint {label}, got {len(found)}')
    return found[0]


def one(rows, kind, **fields):
    found = [row for row in rows if row['kind'] == kind and
             all(row.get(key) == value for key, value in fields.items())]
    require(len(found) == 1, f'expected one {kind} {fields}, got {len(found)}')
    return found[0]


def fixture_body(row, label):
    found = [body for body in row['bodies'] if body['label'] == label]
    require(len(found) == 1, f'expected one fixture {label}, got {len(found)}')
    return found[0]


def swimming(rows):
    begin = one(rows, 'water_swim_begin')
    swum = checkpoint(rows, 'swum')
    require(swum['hero_saw_swim'], 'hero never entered the swimming state')
    direction = [begin['direction'][0], begin['direction'][2]]
    length = math.hypot(*direction)
    require(math.isfinite(length) and length > 0, 'invalid swimming direction')
    direction = [value / length for value in direction]

    def projection(position):
        return sum((position[axis] - begin['barrel_cm'][axis]) * direction[index]
                   for index, axis in enumerate((0, 2)))

    before, after = projection(begin['hero_cm']), projection(swum['hero_cm'])
    require(before < 0 < after,
            f'hero did not cross the original barrel along the swim direction: '
            f'{before:.3f} -> {after:.3f} cm')
    original = fixture_body(one(rows, 'water_setup'), 'barrel')
    settled = fixture_body(checkpoint(rows, 'settled'), 'barrel')
    barrel = fixture_body(swum, 'barrel')
    require(barrel['category'] == 10 and
            barrel['zen_id'] == settled['zen_id'] == original['zen_id'],
            'swimming checkpoint lost the intact original barrel')
    maximum_step = swum['hero_max_step_cm']
    require(math.isfinite(maximum_step) and maximum_step >= 0, 'invalid maximum hero step')
    return {'hero_saw_swim': True, 'projection_before_cm': before,
            'projection_after_cm': after, 'hero_max_step_cm': maximum_step,
            'barrel_displacement_cm': math.dist(settled['position_cm'], barrel['position_cm'])}


def fragments(rows):
    crate = fixture_body(one(rows, 'water_setup'), 'crate')
    broken = checkpoint(rows, 'fragments')
    pieces = [body for body in broken['bodies'] if body['category'] in (13, 15) and
              body['zen_id'] == crate['zen_id']]
    require(pieces, 'fireball checkpoint has no fragments of the original crate')
    for piece in pieces:
        density = piece['density_kg_m3']
        require(math.isfinite(density) and 0 < density < 1000,
                f'crate fragment {piece["piece"]}: invalid wooden density {density}')
        bottom, surface, top = (piece['world_bottom_cm'], piece['water_y_cm'],
                                piece['world_top_cm'])
        require(all(value is not None and math.isfinite(value)
                    for value in (bottom, surface, top)) and bottom < surface < top,
                f'crate fragment {piece["piece"]}: wooden shell is not floating across the surface')
        fraction = piece['submerged_fraction']
        require(math.isfinite(fraction) and 0 < fraction < 1,
                f'crate fragment {piece["piece"]}: invalid floating submerged fraction {fraction}')
        require(abs(fraction - WOOD_FRAGMENT_SUBMERGED_FRACTION) < .06,
                f'crate fragment {piece["piece"]}: displaced volume does not match wooden fragment target')
        require(piece['sleeping'], f'crate fragment {piece["piece"]}: still moving after settling')
    return {'crate_zen_id': crate['zen_id'], 'count': len(pieces),
            'sleeping_count': sum(piece['sleeping'] for piece in pieces),
            'wet_count': sum(piece['submerged_fraction'] > 0 for piece in pieces)}


def pickup(rows):
    row = one(rows, 'water_pickup')
    gained = row['inventory_after'] - row['inventory_before']
    if row['accepted']:
        require(gained == 1, 'accepted shore pickup did not add exactly one inventory item')
        rule = 'picked up'
    else:
        require(row['swimming'] and gained == 0,
                'shore pickup failed without the swimming restriction')
        rule = 'pickup blocked while swimming'
    return {'accepted': row['accepted'], 'swimming': row['swimming'],
            'inventory_before': row['inventory_before'],
            'inventory_after': row['inventory_after'], 'rule': rule}


def wind(rows):
    ready = one(rows, 'water_spell_ready', spell_kind='wind')
    releases = [row for row in rows if row['kind'] == 'spell_physics_release' and
                row['id'] == ready['spell_id'] and row['t'] >= ready['t']]
    require(releases, 'wind fixture never released the prepared wind spell')
    release = releases[0]
    placed = checkpoint(rows, 'wind-placed')
    resting = checkpoint(rows, 'wind-resting')
    require(ready['t'] <= placed['t'] < resting['t'] < release['t'],
            'wind barrel placement and resting checkpoints must precede the real release')
    placed_barrel = fixture_body(placed, 'barrel')
    resting_barrel = fixture_body(resting, 'barrel')
    for body in (placed_barrel, resting_barrel):
        require(body['zen_id'] == ready['zen_id'] and body['category'] == 10,
                'wind preparation lost the intact targeted barrel')
        require(body['submerged_fraction'] == 0,
                'wind barrel entered the water before the real release')
    drift = math.dist(placed_barrel['position_cm'], resting_barrel['position_cm'])
    horizontal_drift = math.hypot(*(resting_barrel['position_cm'][axis] -
                                   placed_barrel['position_cm'][axis] for axis in (0, 2)))
    require(horizontal_drift <= 1,
            f'wind barrel did not stand on its own before release: XZ drift {horizontal_drift:.6f} cm')
    precast_speed = math.hypot(*resting_barrel['velocity_cm_s'])
    require(math.isfinite(precast_speed) and precast_speed < 1,
            f'wind barrel still moved before release: {precast_speed:.6f} cm/s')
    casts = [row for row in rows if row['kind'] == 'spell_physics_cast' and
             row['id'] == ready['spell_id'] and ready['t'] <= row['t'] <= release['t']]
    require(casts, 'wind release has no actual matching spell cast')
    require(not casts[0]['target_in_air'],
            'wind projectile target was unsupported at the actual cast')
    barrel = fixture_body(checkpoint(rows, 'pushed'), 'barrel')
    original = fixture_body(one(rows, 'water_setup'), 'barrel')
    require(barrel['category'] == 10 and barrel['zen_id'] == original['zen_id'] == ready['zen_id'],
            'wind checkpoint lost the intact targeted barrel')
    require(barrel['submerged_fraction'] > 0, 'wind-pushed barrel did not enter the water')
    postcast_speed = math.hypot(*barrel['velocity_cm_s'])
    require(barrel['sleeping'] and postcast_speed == 0,
            'wind-pushed floating barrel did not settle and sleep')
    heard = {}
    for row in rows:
        if row['kind'] == 'physics_noise_heard' and row['callback_dispatched']:
            heard.setdefault((row['body'], row['t_game_ms']), []).append(row['npc_id'])
    splashes = [row for row in rows if row['kind'] == 'physics_noise' and
                row['body'] == barrel['id'] and row['surface_material'] == 5 and
                row['sound'] == 'CS_INTRO_WATERSPLASH.WAV' and row['played'] and
                row['t'] >= release['t'] and (row['body'], row['t_game_ms']) in heard]
    require(splashes, 'no audible stock water splash of the pushed barrel dispatched an NPC callback')
    splash = max(splashes, key=lambda row: row['impulse_ns'])
    require(math.isfinite(splash['impulse_ns']) and splash['impulse_ns'] > 0 and
            math.isfinite(splash['volume']) and 0 < splash['volume'] <= 1 and
            math.isfinite(splash['radius_cm']) and splash['radius_cm'] > 0,
            'invalid wind barrel splash measurements')
    return {'barrel_id': barrel['id'], 'barrel_zen_id': barrel['zen_id'],
            'precast_drift_cm': drift, 'precast_horizontal_drift_cm': horizontal_drift,
            'precast_speed_cm_s': precast_speed,
            'postcast_sleeping': barrel['sleeping'], 'postcast_speed_cm_s': postcast_speed,
            'submerged_fraction': barrel['submerged_fraction'],
            'splash_t_game_ms': splash['t_game_ms'], 'impulse_ns': splash['impulse_ns'],
            'volume': splash['volume'], 'radius_cm': splash['radius_cm'],
            'heard_npc_ids': sorted(set(heard[(splash['body'], splash['t_game_ms'])]))}


def disabled(rows):
    setup = one(rows, 'water_setup')
    expected = {'crate', 'barrel', 'table', *ITEM_DENSITY_KG_M3}
    require(len(setup['bodies']) == 7 and
            {body['label'] for body in setup['bodies']} == expected,
            'disabled fixture needs the same seven genuine objects')
    row = checkpoint(rows, 'disabled')
    require(not row['buoyancy_enabled'], 'disabled checkpoint still has buoyancy enabled')
    require(row['bodies'] and all(body['submerged_fraction'] >= .99 for body in row['bodies']),
            'disabled fixture contains a body which did not sink')
    return {'setup_bodies': len(setup['bodies']), 'submerged_bodies': len(row['bodies']),
            'sleeping_bodies': sum(body['sleeping'] for body in row['bodies'])}


def resting(row):
    bodies = row['bodies']
    require(len(bodies) >= 7, 'water fixture needs crate, barrel, table and four genuine Items')
    result = []
    for body in bodies:
        density = body['density_kg_m3']
        fraction = body['submerged_fraction']
        require(math.isfinite(density) and density > 0 and 0 <= fraction <= 1.001,
                f'invalid water measurement: {body}')
        expected_density = ITEM_DENSITY_KG_M3.get(body['label'], density)
        require(abs(density - expected_density) <= max(.1, expected_density * .001),
                f'{body["label"]}: measured density {density:.6f} kg/m3 differs from '
                f'its expected script-material density {expected_density} kg/m3')
        expected_fraction = VOB_SUBMERGED_FRACTION.get(body['label'], expected_density / 1000)
        floating = expected_fraction < 1
        if floating and row['case'] == 'deep':
            require(body['world_bottom_cm'] < body['water_y_cm'] < body['world_top_cm'],
                    f'{body["label"]}: wooden shell is not floating across the surface')
            require(abs(fraction - expected_fraction) < .06,
                    f'{body["label"]}: displaced volume does not match its target '
                    f'{expected_fraction:.6f}')
        elif not floating and row['case'] == 'deep':
            require(fraction >= .99, f'{body["label"]}: dense shell did not sink')
        require(body['sleeping'], f'{body["label"]}: still moving after settling')
        result.append({'what': body['label'], 'density_kg_m3': density,
                       'behaviour': 'floats' if floating else 'sinks',
                       'draft_cm': (max(0, body['water_y_cm'] - body['world_bottom_cm'])
                                    if body['water_y_cm'] is not None else 0),
                       'submerged_fraction': fraction,
                       'sleep_ms': body['first_sleep_ms']})
    return result


def identity(body):
    if body['category'] == 6:
        return ('item', body['visual'])
    return (body['category'], body['zen_id'], body['piece'])


def restored(saved, loaded):
    before = {identity(body): body for body in saved['bodies']}
    after = {identity(body): body for body in loaded['bodies']}
    require(len(before) == len(saved['bodies']), 'save fixture has ambiguous body identities')
    require(before.keys() <= after.keys(), 'saved water body missing in fresh process')
    maximum = 0
    for key, body in before.items():
        delta = math.dist(body['position_cm'], after[key]['position_cm'])
        maximum = max(maximum, delta)
        require(delta <= 1, f'{key}: fresh-process displacement {delta:.6f} cm exceeds 1 cm')
        require(after[key]['sleeping'], f'{key}: restored sleeper woke up')
    return {'bodies': len(before), 'maximum_displacement_cm': maximum}


def percentile(values):
    require(values and all(math.isfinite(value) and value >= 0 for value in values),
            'missing or invalid timing samples')
    return sorted(values)[math.ceil(.95 * len(values)) - 1]


def measured(directory):
    rows = events(directory)
    setup = [row for row in rows if row['kind'] == 'water_setup']
    require(len(setup) == 1 and setup[0]['case'] == 'perf' and len(setup[0]['bodies']) == 30,
            'performance requires thirty native fixture bodies')
    settled = checkpoint(rows, 'settled')
    require(len(settled['bodies']) == 30, 'performance fixture lost a barrel before the timing window')
    require(not settled['buoyancy_enabled'] or all(body['submerged_fraction'] > 0 for body in settled['bodies']),
            'enabled performance fixture is not floating in the real water')
    meta = json.loads((directory / 'appcadence.csv.json').read_text())
    require(meta['clock'] == 'normal' and meta['complete'] and meta['overflow'] == 0,
            'performance requires complete normal-game cadence')
    require(meta['window_end_ns'] - meta['start_ns'] == 30_000_000_000,
            'performance window must be thirty seconds')
    with (directory / 'appcadence.csv').open() as source:
        frames = list(csv.DictReader(source))
    require(len(frames) == meta['frames'] and frames, 'incomplete app-frame samples')
    with (directory / 'movable-physics.csv').open() as source:
        physics = list(csv.DictReader(source))
    return {'fps': len(frames) / ((meta['final_end_ns'] - meta['start_ns']) / 1e9),
            'frame_p95_ms': percentile([int(frame['interval_ns']) / 1e6 for frame in frames]),
            'physics_p95_ms': percentile([float(row['step_ms']) for row in physics]),
            'physics_samples': len(physics), 'frames': len(frames),
            'fixture_bodies': len(settled['bodies']),
            'sleeping_bodies': sum(body['sleeping'] for body in settled['bodies'])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('functional', type=Path, nargs='?')
    parser.add_argument('--shallow', type=Path)
    parser.add_argument('--wind', type=Path)
    parser.add_argument('--disabled', type=Path)
    parser.add_argument('--deep-label', default='settled')
    parser.add_argument('--shallow-label', default='settled')
    parser.add_argument('--reload', type=Path)
    parser.add_argument('--saved-label', default='saved')
    parser.add_argument('--reload-label', default='continued')
    parser.add_argument('--perf-on', type=Path)
    parser.add_argument('--perf-off', type=Path)
    args = parser.parse_args()
    result = {}
    if args.functional:
        rows = events(args.functional)
        result['deep'] = resting(checkpoint(rows, args.deep_label))
        result['swimming'] = swimming(rows)
        result['deep_fragments'] = fragments(rows)
        if args.reload:
            loaded = events(args.reload)
            saved = checkpoint(rows, args.saved_label)
            result['restore'] = {'initial': restored(saved, checkpoint(loaded, 'restored')),
                                 'after_30_seconds': restored(saved, checkpoint(loaded, args.reload_label))}
    if args.shallow:
        rows = events(args.shallow)
        result['shallow'] = resting(checkpoint(rows, args.shallow_label))
        result['pickup'] = pickup(rows)
        result['shallow_fragments'] = fragments(rows)
    if args.wind:
        result['wind'] = wind(events(args.wind))
    if args.disabled:
        result['disabled'] = disabled(events(args.disabled))
    if args.perf_on or args.perf_off:
        require(args.perf_on and args.perf_off, 'performance needs both ON and OFF')
        on, off = measured(args.perf_on), measured(args.perf_off)
        result['performance'] = {'on': on, 'off': off,
                                 'fps_change_percent': (on['fps'] / off['fps'] - 1) * 100,
                                 'physics_p95_delta_ms': on['physics_p95_ms'] - off['physics_p95_ms']}
        require(on['physics_p95_ms'] <= 2, 'water physics p95 exceeds the two millisecond budget')
    require(result, 'choose a functional, shallow, wind, disabled or performance measurement')
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
