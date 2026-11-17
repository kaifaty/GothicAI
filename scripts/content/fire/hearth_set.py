#!/usr/bin/env python3
"""The own flame in the hearths of the city's houses: the effects, the layer over the base's fires and the staging part.

    python3 scripts/content/fire/hearth_set.py effects
    python3 scripts/content/fire/hearth_set.py layer [--base DIR]
    python3 scripts/content/fire/hearth_set.py pack --set hearth-1

reports/content/remaster/hearth/README.md. The third step of the flame's rollout after the campfire and the
brazier (campfire_set.py, brazier_set.py), with the same atlases. Only the fire changes: the original's four
fireplace models stay. Every FIRE.PFX that burns in a fireplace (NW_CITY_FIREPLACE_*) is replaced where it stands
by three flames, licks, sparks and a wisp of smoke that fades inside the hood; a wide fireplace keeps its row of
fires. The original lights the room with nothing of its own (a lamp of range 15 cm and a static lamp turned off):
every fireplace gets one lamp with the flame's colour, flicker and slow sway.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import shutil
import sys

import numpy as np

from campfire_set import ROOT, SMOKE, STAGING, TONGUE, at, base_dir, card, haze, write_layer

CONTENT = ROOT / 'reports/content/remaster/hearth/content'
PARTICLES = CONTENT / 'particles.d/hearth.json'
LAYER = CONTENT / 'worlds/NEWWORLD/vobs.d/hearth.json'
FIRE = 'FIRE.PFX'
# the model's floor slab, cm in its frame; the firebox opens to -z, 1.2-1.3 m high and from 1.1 m wide
FLOOR = {'NW_CITY_FIREPLACE_NORMAL_01.3DS': -150.0, 'NW_CITY_FIREPLACE_LARGE_01.3DS': -182.0,
         'NW_CITY_FIREPLACE_LARGE_011.3DS': -182.0, 'NW_CITY_FIREPLACE_LARGE_02.3DS': -182.0,
         'NW_CITY_FIREPLACE_LARGE_021.3DS': -182.0}
REACH = 260.0        # a fire farther from every fireplace is not a hearth's
BED = 5.0            # the ash over the slab
CORE = (64, 99)      # the cell; the flame in it reaches about 0.7 m: half the opening and more (the original's fills it)
SIDE = (46, 71)
LAMP = {'color': [230, 84, 15], 'near': 12}
SWAY = {'jitter_cm': 1.0, 'sway_hz': 1.2}
LIGHT_UP = 45.0
LIGHT_OUT = 20.0     # towards the room, out of the firebox's back
LIGHT_RANGE = 500


def effects() -> dict:
    licks = card('OWN_HEARTH_LICKS', TONGUE, (22, 34), 52)
    licks['flipbook'].pop('steady')
    licks['set'].update({'ppsValue': 16.0, 'shpType_S': 'CIRCLE', 'shpDim_S': '22 0 22', 'dirMode_S': 'DIR',
                         'velAvg': 0.07, 'velVar': 0.02, 'lspPartAvg': 480.0, 'lspPartVar': 140.0,
                         'visSizeEndScale': 0.55, 'visAlphaEnd': 0.0})
    sparks = {'name': 'OWN_HEARTH_SPARKS', 'base': 'FIRE_SPARKS',
              'set': {'ppsValue': 4.0, 'shpType_S': 'SPHERE', 'shpDim_S': '16', 'shpOffsetVec_S': '0 0 0',
                      'dirMode_S': 'DIR', 'velAvg': 0.1, 'velVar': 0.05, 'lspPartAvg': 700.0, 'lspPartVar': 300.0,
                      'flyGravity_S': '0 0.0001 0', 'visSizeStart_S': '0.3 0.3', 'visSizeEndScale': 0.6,
                      'visAlphaStart': 255.0, 'visAlphaEnd': 40.0, 'visAlphaFunc_S': 'ADD'},
              'curl': {'strength': 2.0, 'frequency': 2.5, 'speed': 0.3}, 'drag': 1.4, 'stretch': 0.025, 'minPixels': 1.5}
    # the draught takes the smoke into the flue: a thin wisp that is gone half a metre over the flames, under the hood
    smoke = {'name': 'OWN_HEARTH_SMOKE', 'base': 'LIGHTSMOKE',
             'flipbook': {'cols': 8, 'rows': 8, 'frames': 64, 'fps': 10},
             'set': {'visName_S': SMOKE, 'visAlphaFunc_S': 'BLEND', 'visAlphaStart': 90.0, 'visAlphaEnd': 0.0,
                     'visTexColorStart_S': '255 255 255', 'visTexColorEnd_S': '255 255 255',
                     'visSizeStart_S': '20 20', 'visSizeEndScale': 2.2, 'lspPartAvg': 1100.0, 'lspPartVar': 250.0,
                     'ppsValue': 4.0, 'shpType_S': 'CIRCLE', 'shpDim_S': '6 0 6', 'shpOffsetVec_S': '0 0 0',
                     'dirMode_S': 'DIR', 'velAvg': 0.05, 'velVar': 0.012, 'flyGravity_S': '0 0 0'},
             'curl': {'strength': 0.3, 'frequency': 0.9, 'speed': 0.25}, 'drag': 0.2}
    return {'format': 1, 'effects': [card('OWN_HEARTH_FLAME', TONGUE, CORE, 38), card('OWN_HEARTH_FLAME_SIDE', TONGUE, SIDE, 46),
                                     licks, sparks, smoke,
                                     # the hot air goes up the flue: the shimmer fills the opening over the flames
                                     # and is gone at the lintel
                                     haze('OWN_HEARTH_HAZE', 60, 16, 800)]}


def census(vobs_json) -> tuple[list, list]:
    """The base's FIRE.PFX vobs with their parents, and the fireplace models."""
    fires, places = [], []

    def walk(vob, parent):
        name = str(vob.get('visual_name', '')).upper()
        if name == FIRE:
            fires.append((vob, parent))
        elif name in FLOOR:
            places.append(vob)
        for child in vob.get('children', []):
            walk(child, vob)

    for vob in json.loads(Path(vobs_json).read_text())['vobs']:
        walk(vob, None)
    return sorted(fires, key=lambda pair: pair[0]['id']), places


def layer(fires, places) -> tuple[dict, list]:
    out = {'format': 1, 'remove': [], 'light': [], 'add': []}
    left, lit = [], {}
    for vob, parent in fires:
        spot = np.array(vob['position'], float)
        place = min(places, key=lambda p: float(np.linalg.norm(spot - np.array(p['position']))))
        origin, turn = np.array(place['position'], float), np.array(place['rotation'], float)
        if np.linalg.norm(spot - origin) > REACH:
            if not (parent and str(parent.get('visual_name', '')).upper() == FIRE):
                left.append((vob['id'], 'no fireplace within reach'))
            continue
        local = turn.T @ (spot - origin)
        local[1] = FLOOR[place['visual_name'].upper()] + BED
        bed = origin + turn @ local
        wide = turn @ np.array([1.0, 0.0, 0.0])
        key = f'hearth-{vob["id"]}'
        out['remove'].append(vob['id'])
        siblings = parent.get('children', []) if parent and parent['type'] == 'zCVobLight' else []
        out['remove'] += [c['id'] for c in siblings if str(c.get('visual_name', '')).upper() == 'FIRE_SMOKE.PFX']
        points = [('flame', 'OWN_HEARTH_FLAME', bed + [0, CORE[1] / 2 - 3, 0]),
                  ('flame-a', 'OWN_HEARTH_FLAME_SIDE', bed + wide * 24 + [0, SIDE[1] / 2 - 2, 0]),
                  ('flame-b', 'OWN_HEARTH_FLAME_SIDE', bed - wide * 25 + [0, SIDE[1] / 2 - 4, 0]),
                  ('licks', 'OWN_HEARTH_LICKS', bed + [0, 5, 0]),
                  ('sparks', 'OWN_HEARTH_SPARKS', bed + [0, 30, 0]),
                  ('smoke', 'OWN_HEARTH_SMOKE', bed + [0, 82, 0]),
                  ('haze', 'OWN_HEARTH_HAZE', bed + [0, 50, 0])]
        for end, pfx, position in points:
            entry = {'key': f'{key}-{end}', 'kind': 'emitter', 'pfx': pfx, 'position': at(position), 'yaw_deg': 0}
            if 'flame' in end:
                entry['flicker'] = {'pulse_hz': 2.0 if end == 'flame' else 3.0, 'jitter_hz': 10, 'amount': 0.25,
                                    'seed': 23 + vob['id'] * 3 + len(out['add'])}
            out['add'].append(entry)
        lit.setdefault(place['id'], []).append((key, bed, turn))
    for place_id, units in sorted(lit.items()):
        # one lamp a fireplace, over the middle of its fires, flickering with the fire nearest to the middle
        middle = np.mean([bed for _key, bed, _turn in units], axis=0)
        key, _bed, turn = min(units, key=lambda unit: float(np.linalg.norm(unit[1] - middle)))
        position = middle + [0, LIGHT_UP, 0] + turn @ np.array([0.0, 0.0, -LIGHT_OUT])
        out['add'].append({'key': f'hearth-{place_id}-light', 'kind': 'light', 'position': at(position), 'yaw_deg': 0,
                           'range': LIGHT_RANGE, 'color': LAMP['color'], 'near': LAMP['near'],
                           'flicker_from': f'{key}-flame', **SWAY})
    return out, left


def cmd_effects(_args) -> int:
    PARTICLES.parent.mkdir(parents=True, exist_ok=True)
    PARTICLES.write_text(json.dumps(effects()) + '\n')
    print(f'{PARTICLES.relative_to(ROOT)}: {len(effects()["effects"])} effects')
    return 0


def cmd_layer(args) -> int:
    fires, places = census(base_dir(args.base) / 'worlds/NEWWORLD/vobs.json')
    data, left = layer(fires, places)
    write_layer(LAYER, data)
    lights = sum(e['kind'] == 'light' for e in data['add'])
    flames = sum(e['key'].endswith('-flame') for e in data['add'])
    print(f'{LAYER.relative_to(ROOT)}: {flames} fires in {lights} fireplaces of {len(places)}, {len(data["remove"])} removes, '
          f'{lights} lights added')
    for vob_id, why in left:
        print(f'  left as it is: {vob_id}: {why}')
    return 0


def cmd_pack(args) -> int:
    output = STAGING / args.set
    if output.exists():
        sys.exit(f'{output}: the staging set exists; choose a new name')
    for source in (PARTICLES, LAYER):
        target = output / source.relative_to(CONTENT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    print(f'{output}: the hearths\' fire as a part; it needs {TONGUE} of torch-model-* and {SMOKE} of smoke-*')
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('effects').set_defaults(func=cmd_effects)
    one = sub.add_parser('layer')
    one.add_argument('--base')
    one.set_defaults(func=cmd_layer)
    pack = sub.add_parser('pack')
    pack.add_argument('--set', required=True)
    pack.set_defaults(func=cmd_pack)
    args = parser.parse_args()
    return args.func(args)


if __name__ == '__main__':
    raise SystemExit(main())
