#!/usr/bin/env python3
"""Bond groups of an own-wood-fracture-clip or own-wood-remnant-clip run: guards and which pieces stayed joined.

    python3 clip_groups.py <bench out> <nodes dir>

A remnant-clip run has a break per case `<prop>-1h` and `<prop>-1h-remnant-<n>`; each remnant break also gets the
swings of its `destruction2_remnant_complete`.

<nodes dir> holds `<prop>.nodes` from `fracture-gltf-extract --nodes model.debris.glb`: its node order is
the piece order of model.fracture (scripts/fracture/debris.py), so piece numbers get the generator's names.
"""
import json
import sys
from pathlib import Path

PROPS = {'barrel-1h': 'mesh3d-barrel-atlas', 'crate-1h': 'rigging-crate'}


def names(path):
    return [line.split()[1] for line in path.read_text().splitlines() if line.startswith('node ')]


def groups(row, label):
    joined = {}
    for piece in row['rows']:
        joined.setdefault(piece['bond_group'], []).append(label[piece['piece']])
    return sorted(joined.values(), key=len, reverse=True)


def main():
    out, nodes = Path(sys.argv[1]), Path(sys.argv[2])
    rows = [json.loads(line) for line in (out / 'events.jsonl').read_text().splitlines()]
    result = {}
    cases = []
    for base, prop in PROPS.items():
        cases.append((base, prop))
        cases += sorted(((row['case'], prop) for row in rows if row['kind'] == 'destruction2_start' and
                         row['case'].startswith(base + '-remnant-')), key=lambda c: int(c[0].rsplit('-', 1)[1]))
    complete = {(row['case'], row['ordinal']): row for row in rows if row['kind'] == 'destruction2_remnant_complete'}
    for case, prop in dict.fromkeys(cases):
        label = names(nodes / f'{prop}.nodes')
        samples = {row['after_break_ms']: row for row in rows
                   if row['kind'] == 'destruction2_pieces' and row['case'] == case}
        if not samples:
            continue
        last = samples[15000]
        assert len(label) == last['pieces'], f'{case}: {len(label)} names for {last["pieces"]} pieces'
        keys = ['pieces', 'groups', 'bodies', 'single_pieces', 'largest_group_volume_fraction', 'fell', 'bodies_fell',
                'sleeping', 'bodies_sleeping', 'max_native_floor_penetration_cm', 'max_visible_terrain_penetration_cm',
                'max_piece_penetration_cm', 'metal_on_ground', 'metal', 'median_displacement_cm', 'max_jitter_cm']
        entry = {'15s': {key: last.get(key) for key in keys}}
        for at in (500, 2000, 15000):
            parts = groups(samples[at], label)
            entry[f'{at}ms'] = {'bodies': samples[at]['bodies'], 'largest': parts[0],
                                'joined': [part for part in parts[1:] if len(part) > 1],
                                'single': sorted(part[0] for part in parts if len(part) == 1)}
        if '-remnant-' in case:
            base, ordinal = case.split('-remnant-')
            done = complete.get((base, int(ordinal)))
            breaks = [row for row in rows if row['kind'] == 'destruction_break' and row.get('remnant')]
            entry['swings'] = done and done['swings']
            entry['break_ms'] = [row['break_ms'] for row in breaks if row['case'] == base]
        result[case] = entry
    print(json.dumps(result, ensure_ascii=False, indent=1))


if __name__ == '__main__':
    main()
