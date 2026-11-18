#!/usr/bin/env python3
"""The candles' and the pentagram chandeliers' own flame as a part of the working content.

    python3 scripts/content/fire/candle_set.py effects
    python3 scripts/content/fire/candle_set.py layer [--base DIR]
    python3 scripts/content/fire/candle_set.py pack --set candle-1 --atlas candle-flame.dds

reports/content/remaster/candle. The original's flame is a flat animated picture (the decal FIRE_COMPLETE_A0.TGA,
20 x 50 cm) over NW_CITY_CANDLE_01 and over the five bowls of DT_LIGHTER_PENTAGRAMM. Each goes; a laminar flame of
candle_flame.py stands on the wick instead: a teardrop that breathes and hardly moves. The models and the lamps stay.
`pack` takes the flame's atlas (candle_flame.py, outside git) as OWN_CANDLE_FLAME.TGA.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import shutil
import sys

from campfire_set import HDR, ROOT, STAGING, at, base_dir, write_layer

CONTENT = ROOT / 'reports/content/remaster/candle/content'
PARTICLES = CONTENT / 'particles.d/candle.json'
LAYER = CONTENT / 'worlds/NEWWORLD/vobs.d/candle.json'
DECAL = 'FIRE_COMPLETE_A0.TGA'
CANDLE, STAND = 'NW_CITY_CANDLE_01.3DS', 'DT_LIGHTER_PENTAGRAMM.3DS'
FLAME = ('OWN_CANDLE_FLAME.TGA', 'own-candle-flame/albedo.dds')
REACH = 160.0        # a decal farther from every candle and stand is not theirs
# candle_flame.py: a cell of 4 x 8 cm, the wick's top 1.4 cm over its bottom, the flame 3.4 cm from wick to tip.
# The candle is a pillar 12 cm thick with a thick wick: a flame of about 6 cm. A chandelier's bowl burns oil on a
# thicker wick: about 10 cm.
CELL, WICK = (4.0, 8.0), 1.4
SCALE = {'candle': 1.8, 'bowl': 3.0}
# the decal's centre is 33.3 cm over the candle's origin, its picture starts 8 cm over its lower edge;
# the candle's top is 16.1 cm over the origin and the wick stands 1 cm out of it
WICK_UNDER_DECAL = {'candle': 33.3 - 17.1, 'bowl': 28.0}  # the bowl's: by the frames, the oil just under the rim


def card(name, scale, fps) -> dict:
    size = (CELL[0] * scale, CELL[1] * scale)
    return {'name': name, 'base': 'FIRE_HOT', 'flipbook': {'cols': 8, 'rows': 4, 'frames': 32, 'fps': fps, 'yaw': True, 'steady': True},
            'set': {'ppsValue': 1.0, 'ppsScaleKeys_S': '', 'ppsIsLooping': 0, 'ppsIsSmooth': 0, 'ppsFPS': 0.0,
                    'shpType_S': 'POINT', 'shpIsVolume': 0, 'shpDim_S': '0 0 0', 'shpOffsetVec_S': '0 0 0',
                    'dirMode_S': 'NONE', 'velAvg': 0.0, 'velVar': 0.0, 'flyGravity_S': '0 0 0',
                    'lspPartAvg': 1000.0, 'lspPartVar': 0.0, 'visName_S': FLAME[0], 'visOrientation_S': 'NONE',
                    'visTexIsQuadPoly': 1, 'visTexAniFPS': 0.0, 'visTexAniIsLooping': 0,
                    'visTexColorStart_S': '255 255 255', 'visTexColorEnd_S': '255 255 255',
                    'visSizeStart_S': f'{size[0]:g} {size[1]:g}', 'visSizeEndScale': 1.0,
                    'visAlphaFunc_S': 'ADD', 'visAlphaStart': 255.0, 'visAlphaEnd': 255.0},
            'hdr': HDR}


def effects() -> dict:
    # the atlas loops in 2 s at 16 cells a second; the bowl's larger flame breathes a little slower
    return {'format': 1, 'effects': [card('OWN_CANDLE_FLAME', SCALE['candle'], 16), card('OWN_BOWL_FLAME', SCALE['bowl'], 13)]}


def census(vobs_json) -> tuple[list, list]:
    decals, hosts = [], []

    def walk(vob):
        name = str(vob.get('visual_name', '')).upper()
        if name == DECAL:
            decals.append(vob)
        elif name in (CANDLE, STAND):
            hosts.append(vob)
        for child in vob.get('children', []):
            walk(child)

    for vob in json.loads(Path(vobs_json).read_text())['vobs']:
        walk(vob)
    return sorted(decals, key=lambda vob: vob['id']), hosts


def layer(decals, hosts) -> tuple[dict, list, dict]:
    out = {'format': 1, 'remove': [], 'light': [], 'add': []}
    left, count = [], {'candle': 0, 'bowl': 0}
    for vob in decals:
        host = min(hosts, key=lambda h: math.dist(vob['position'], h['position']))
        if math.dist(vob['position'], host['position']) > REACH:
            left.append((vob['id'], 'no candle or chandelier within reach'))
            continue
        kind = 'candle' if str(host['visual_name']).upper() == CANDLE else 'bowl'
        x, y, z = vob['position']
        size = CELL[1] * SCALE[kind]
        centre = y - WICK_UNDER_DECAL[kind] + size / 2 - WICK * SCALE[kind]
        out['remove'].append(vob['id'])
        out['add'].append({'key': f'{kind}-{vob["id"]}-flame', 'kind': 'emitter',
                           'pfx': 'OWN_CANDLE_FLAME' if kind == 'candle' else 'OWN_BOWL_FLAME',
                           'position': at([x, centre, z]), 'yaw_deg': 0,
                           # a still room's flame: a slow shallow breath
                           'flicker': {'pulse_hz': 1.3 if kind == 'candle' else 1.8, 'jitter_hz': 8,
                                       'amount': 0.08 if kind == 'candle' else 0.14, 'seed': 31 + vob['id'] * 3}})
        count[kind] += 1
    return out, left, count


def cmd_effects(_args) -> int:
    PARTICLES.parent.mkdir(parents=True, exist_ok=True)
    PARTICLES.write_text(json.dumps(effects()) + '\n')
    print(f'{PARTICLES.relative_to(ROOT)}: {len(effects()["effects"])} effects')
    return 0


def cmd_layer(args) -> int:
    decals, hosts = census(base_dir(args.base) / 'worlds/NEWWORLD/vobs.json')
    data, left, count = layer(decals, hosts)
    write_layer(LAYER, data)
    print(f'{LAYER.relative_to(ROOT)}: {count["candle"]} candles, {count["bowl"]} bowls, {len(data["remove"])} removes')
    for vob_id, why in left:
        print(f'  left as it is: {vob_id}: {why}')
    return 0


def cmd_pack(args) -> int:
    output = STAGING / args.set
    if output.exists():
        sys.exit(f'{output}: the staging set exists; choose a new name')
    if not args.atlas.is_file():
        sys.exit(f'{args.atlas}: the flame atlas is missing')
    for source in (PARTICLES, LAYER):
        target = output / source.relative_to(CONTENT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    flame = output / 'materials' / FLAME[1]
    flame.parent.mkdir(parents=True)
    shutil.copyfile(args.atlas, flame)
    (output / 'materials/materials.json').write_text(json.dumps({FLAME[0]: {'albedo': FLAME[1]}}, indent=2) + '\n')
    print(f'{output}: the candles\' flame as a part with its atlas {FLAME[0]}')
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
    pack.add_argument('--atlas', type=Path, required=True)
    pack.set_defaults(func=cmd_pack)
    args = parser.parse_args()
    return args.func(args)


if __name__ == '__main__':
    raise SystemExit(main())
