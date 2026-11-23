#!/usr/bin/env python3
"""The rest of the main world's fire as a part of the working content: street lamps, small open fires, stray smoke.

    python3 scripts/content/fire/rest_set.py build [--base DIR]
    python3 scripts/content/fire/rest_set.py pack --set fire-rest-1

reports/content/remaster/fire-rest. Three things, all with effects that other parts already hold:
- the 43 street lamps (the MOB FIREPLACE_NW_LAMP_01): their fire is the vob tree FIRETREE_LAMP, replaced whole by
  vobtrees.d/FIRETREE_LAMP.json with the wall torch's flame, tongues, sparks, haze and a light (torch-model-*,
  torch-haze-*);
- the small open fires that burn the original's FIRE_MEDIUM beside a fire bowl, a ground fireplace, an orc
  fireplace or a spit: the brazier's flames, tongues, sparks, smoke and haze on the same spot (brazier-*);
- every other FIRE_SMOKE of the world (burnt poles, wrecks, a cold brazier): the brazier's thin smoke.
The two wall torches whose fire holds other vobs stay the original's: their model comes with the torch's own part.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import shutil
import sys

import numpy as np

from campfire_set import ROOT, STAGING, at, base_dir, write_layer

CONTENT = ROOT / 'reports/content/remaster/fire-rest/content'
LAYER = CONTENT / 'worlds/NEWWORLD/vobs.d/fire-rest.json'
TREE = CONTENT / 'vobtrees.d/FIRETREE_LAMP.json'
HOSTS = {'OC_LOB_TORCHHOLDER_FILLED.3DS': 'bowl', 'FIREPLACE_GROUND.ASC': 'ground', 'ORC_FIREPLACE_V3.3DS': 'orc',
         'BARBQ_NW_MISC_SHEEP_01.MDS': 'spit'}
REACH = 120.0
TORCH = 'OW_MISC_WALL_TORCH_01.3DS'
LAMP = {'color': [230, 84, 15], 'near': 12}
SWAY = {'jitter_cm': 1.2, 'sway_hz': 1.3}
COALS_UNDER_FIRE = 8.0   # the fuel's top under the original effect's origin
CORE, SIDE = (44, 68), (30, 47)   # brazier_set.py
LIGHT_UP = 30.0
# The lamp's slot is the middle of the original's flame picture (40 x 60 cm); the wick of the iron basket is about
# 20 cm under it. The torch's cards: 34 x 44 and 21 x 32 cm (wall_torch_set.py).
LAMP_WICK = -20.0
LAMP_RANGE = 550
# the original's lamp hangs 85 cm under the flame, under the lantern: a light inside the cage is shadowed by its floor
# and the street under the post stays dark (round 1)
LAMP_LIGHT = -85.0


def brazier_fire(key, coals, turn_of, seed, smoke) -> list:
    """The `add` entries of the brazier's fire on a bed of coals, world or slot cm: flames, tongues, sparks, haze."""
    turn = (turn_of * 2.39996) % (2 * math.pi)
    ring = lambda r, a, y: coals + np.array([r * math.cos(turn + a), y, r * math.sin(turn + a)])
    points = [('flame', 'OWN_BRAZIER_FLAME', coals + [0, CORE[1] / 2 - 3, 0]),
              ('flame-a', 'OWN_BRAZIER_FLAME_SIDE', ring(9, 0, SIDE[1] / 2 - 2)),
              ('flame-b', 'OWN_BRAZIER_FLAME_SIDE', ring(10, math.pi + 0.4, SIDE[1] / 2 - 4)),
              ('licks', 'OWN_BRAZIER_LICKS', coals + [0, 6, 0]),
              ('sparks', 'OWN_BRAZIER_SPARKS', coals + [0, 25, 0]),
              ('haze', 'OWN_BRAZIER_HAZE', coals + [0, 36, 0])]
    if smoke:
        points.append(('smoke', 'OWN_BRAZIER_SMOKE', coals + [0, 42, 0]))
    entries = []
    for end, pfx, position in points:
        entry = {'key': f'{key}-{end}', 'kind': 'emitter', 'pfx': pfx, 'position': at(position), 'yaw_deg': 0}
        if 'flame' in end:
            entry['flicker'] = {'pulse_hz': 2.4 if end == 'flame' else 3.4, 'jitter_hz': 10, 'amount': 0.25,
                                'seed': seed + len(entries)}
        entries.append(entry)
    return entries


def torch_fire(key, wick, seed) -> list:
    """The `add` entries of the wall torch's fire on a wick, world or slot cm (the street lamp's set)."""
    flick = lambda n: {'pulse_hz': 4.5, 'jitter_hz': 12, 'amount': 0.2, 'seed': seed + n}
    wick = np.array(wick, float)
    entry = lambda end, pfx, offset: {'key': f'{key}-{end}', 'kind': 'emitter', 'pfx': pfx, 'position': at(wick + offset), 'yaw_deg': 0}
    return [{**entry('flame', 'OWN_TORCH_FLAME', [0, 18, 0]), 'flicker': flick(0)},
            {**entry('flame-a', 'OWN_TORCH_FLAME_SIDE', [5, 10, 2]), 'flicker': flick(1)},
            {**entry('flame-b', 'OWN_TORCH_FLAME_SIDE', [-4, 10, -3]), 'flicker': flick(2)},
            entry('licks', 'OWN_TORCH_LICKS', [0, -2, 0]),
            entry('sparks', 'OWN_TORCH_SPARKS', [0, 5, 0]),
            entry('haze', 'OWN_TORCH_HAZE', [0, 40, 0])]


def lamp_tree() -> dict:
    add = torch_fire('lamp', [0, LAMP_WICK, 0], 41)
    add.append({'key': 'lamp-light', 'kind': 'light', 'position': [0, LAMP_LIGHT, 0], 'yaw_deg': 0, 'range': LAMP_RANGE,
                'color': LAMP['color'], 'near': 8, 'flicker_from': 'lamp-flame', 'jitter_cm': 0.6, 'sway_hz': 1.5})
    return {'format': 1, 'add': add}


def layer(vobs_json, taken) -> tuple[dict, dict]:
    everything = []

    def walk(vob, parent):
        everything.append((vob, parent))
        for child in vob.get('children', []):
            walk(child, vob)

    for vob in json.loads(Path(vobs_json).read_text())['vobs']:
        walk(vob, None)
    name = lambda vob: str(vob.get('visual_name', '')).upper()
    hosts = [vob for vob, _parent in everything if name(vob) in HOSTS]
    out = {'format': 1, 'remove': [], 'light': [], 'add': []}
    count = {kind: 0 for kind in HOSTS.values()} | {'smoke': 0}
    for vob, parent in everything:
        if name(vob) != 'FIRE_MEDIUM.PFX' or vob['id'] in taken or (parent and name(parent) == TORCH):
            continue
        spot = np.array(vob['position'], float)
        host = min(hosts, key=lambda h: math.dist(spot, h['position']))
        if math.dist(spot, host['position']) > REACH:
            continue
        kind = HOSTS[name(host)]
        key = f'{kind}-{vob["id"]}'
        coals = spot - [0, COALS_UNDER_FIRE, 0]
        siblings = parent.get('children', []) if parent and parent['type'] == 'zCVobLight' else []
        smoke = [c for c in siblings if name(c) == 'FIRE_SMOKE.PFX']
        out['remove'] += [vob['id']] + [c['id'] for c in smoke]
        taken |= {c['id'] for c in smoke}
        out['add'] += brazier_fire(key, coals, vob['id'], 53 + vob['id'] * 3 + len(out['add']), bool(smoke))
        light = coals + [0, LIGHT_UP, 0]
        signal = {'flicker_from': f'{key}-flame', **SWAY}
        if parent and parent['type'] == 'zCVobLight' and not parent['is_static']:
            out['light'].append({'id': parent['id'], 'color': LAMP['color'], 'near': LAMP['near'], **signal,
                                 'offset_cm': at(light - np.array(parent['position']))})
        else:
            out['add'].append({'key': f'{key}-light', 'kind': 'light', 'position': at(light), 'yaw_deg': 0, 'range': 700,
                               'color': LAMP['color'], 'near': LAMP['near'], **signal})
        count[kind] += 1
    for vob, _parent in everything:
        if name(vob) == 'FIRE_SMOKE.PFX' and vob['id'] not in taken:
            out['remove'].append(vob['id'])
            out['add'].append({'key': f'smoke-{vob["id"]}', 'kind': 'emitter', 'pfx': 'OWN_BRAZIER_SMOKE',
                               'position': at(vob['position']), 'yaw_deg': 0})
            count['smoke'] += 1
    return out, count


def cmd_build(args) -> int:
    base = base_dir(args.base)
    taken = set()
    for path in sorted((ROOT / 'reports/content/remaster').glob('*/content/worlds/NEWWORLD/vobs.d/*.json')):
        if path != LAYER:
            taken |= set(json.loads(path.read_text()).get('remove', []))
    data, count = layer(base / 'worlds/NEWWORLD/vobs.json', taken)
    write_layer(LAYER, data)
    TREE.parent.mkdir(parents=True, exist_ok=True)
    TREE.write_text(json.dumps(lamp_tree(), indent=1) + '\n')
    print(f'{LAYER.relative_to(ROOT)}: {count}, {len(data["remove"])} removes, {len(data["light"])} lamps of the base')
    print(f'{TREE.relative_to(ROOT)}: {len(lamp_tree()["add"])} entries')
    return 0


def cmd_pack(args) -> int:
    output = STAGING / args.set
    if output.exists():
        sys.exit(f'{output}: the staging set exists; choose a new name')
    for source in (LAYER, TREE):
        target = output / source.relative_to(CONTENT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    print(f'{output}: the rest of the fire as a part; it needs the effects of torch-model-*, torch-haze-* and brazier-*')
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    one = sub.add_parser('build')
    one.add_argument('--base')
    one.set_defaults(func=cmd_build)
    pack = sub.add_parser('pack')
    pack.add_argument('--set', required=True)
    pack.set_defaults(func=cmd_pack)
    args = parser.parse_args()
    return args.func(args)


if __name__ == '__main__':
    raise SystemExit(main())
