#!/usr/bin/env python3
"""The own flame on the campfires of the world: the effects, the layer over the base's fires and the staging part.

    python3 scripts/content/fire/campfire_set.py effects
    python3 scripts/content/fire/campfire_set.py layer [--base DIR]
    python3 scripts/content/fire/campfire_set.py pack --set campfire-1 --smoke wispy03b.dds

reports/content/remaster/campfire/README.md. Only the fire changes: the original's model (NW_MISC_FIREPLACE_01, a
ring of stones 1.8 m wide with a tipi of logs 45 cm over its centre) stays. `effects` writes
particles.d/campfire.json, `layer` reads the base's vobs and writes worlds/NEWWORLD/vobs.d/campfire.json: every
campfire loses FIRE_HOT with the smoke under it and gets the own flames, sparks and smoke; its own lamp takes the
flame's colour, flicker and slow sway (a fire without a lamp gets one). `pack` makes the staging part: the two
files. Every flame is the torch's atlas OWN_TORCH_FLAME.TGA of the part torch-model-* at another size and speed
(the wider Flame03 of the same set fills a third of its cell: at a campfire's size its texel is over 1 cm); the
smoke is the wispy puff of smoke_atlas.py (Unity Labs WispySmoke03b, CC0) as OWN_SMOKE_WISPY.TGA, in the part.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import shutil
import sys

import numpy as np

ROOT = Path(__file__).resolve().parents[3]
CONTENT = ROOT / 'reports/content/remaster/campfire/content'
PARTICLES = CONTENT / 'particles.d/campfire.json'
LAYER = CONTENT / 'worlds/NEWWORLD/vobs.d/campfire.json'
STAGING = Path.home() / '.local/share/gothic-ai/content-staging'
BASE_CACHE = Path.home() / '.cache/gothic-ai/base'
MESH = 'NW_MISC_FIREPLACE_01.3DS'
GONE = {'FIRE_HOT.PFX', 'FIRE_SPARKS.PFX'}
TONGUE = 'OWN_TORCH_FLAME.TGA'
SMOKE = ('OWN_SMOKE_WISPY.TGA', 'own-smoke-wispy/albedo.dds')

# cm over the mesh's origin: the ash mound's top is at +4, the logs' tips at +45, the ground at about -45
# the fuel bed is the tipi's foot, about 0.9 m wide: a fire that size stands over a metre high
CORE = (80, 124)     # the middle flame through the tipi: the torch's flame 3.7 times as large, tips 1.1 m over the ash
MID = (62, 96)       # two flames beside it widen the body to the tipi's width
SIDE = (42, 65)      # flames between the logs
BED = -2.0           # where the flames stand
LAMP = {'color': [230, 84, 15], 'near': 12}
SWAY = {'jitter_cm': 2.0, 'sway_hz': 1.0}
LIGHT_UP = 55.0
LIGHT_RANGE = 500
HDR = {'from': 1, 'to': 10, 'edge': 0.06}


def card(name, texture, size, fps) -> dict:
    return {'name': name, 'base': 'FIRE_HOT', 'flipbook': {'cols': 8, 'rows': 8, 'frames': 64, 'fps': fps, 'yaw': True, 'steady': True},
            'set': {'ppsValue': 1.0, 'ppsScaleKeys_S': '', 'ppsIsLooping': 0, 'ppsIsSmooth': 0, 'ppsFPS': 0.0,
                    'shpType_S': 'POINT', 'shpIsVolume': 0, 'shpDim_S': '0 0 0', 'shpOffsetVec_S': '0 0 0',
                    'dirMode_S': 'NONE', 'velAvg': 0.0, 'velVar': 0.0, 'flyGravity_S': '0 0 0',
                    'lspPartAvg': 1000.0, 'lspPartVar': 0.0, 'visName_S': texture, 'visOrientation_S': 'NONE',
                    'visTexIsQuadPoly': 1, 'visTexAniFPS': 0.0, 'visTexAniIsLooping': 0,
                    'visTexColorStart_S': '255 255 255', 'visTexColorEnd_S': '255 255 255',
                    'visSizeStart_S': f'{size[0]} {size[1]}', 'visSizeEndScale': 1.0,
                    'visAlphaFunc_S': 'ADD', 'visAlphaStart': 255.0, 'visAlphaEnd': 255.0},
            'hdr': HDR}


def effects() -> dict:
    """A fire 0.9 m wide puffs at about 1.5 / sqrt(0.9) = 1.6 Hz and its sheets move slower than a torch's: the
    flames play at 34-46 cells a second, each at its own speed (the torch's at 65)."""
    licks = card('OWN_CAMP_LICKS', TONGUE, (30, 46), 50)
    licks['flipbook'].pop('steady')
    licks['set'].update({'ppsValue': 26.0, 'shpType_S': 'CIRCLE', 'shpDim_S': '40 0 40', 'dirMode_S': 'DIR',
                         'velAvg': 0.08, 'velVar': 0.03, 'lspPartAvg': 620.0, 'lspPartVar': 180.0,
                         'visSizeEndScale': 0.55, 'visAlphaEnd': 0.0})
    sparks = {'name': 'OWN_CAMP_SPARKS', 'base': 'FIRE_SPARKS',
              'set': {'ppsValue': 10.0, 'shpType_S': 'SPHERE', 'shpDim_S': '25', 'shpOffsetVec_S': '0 0 0',
                      'dirMode_S': 'DIR', 'velAvg': 0.2, 'velVar': 0.09, 'lspPartAvg': 1500.0, 'lspPartVar': 700.0,
                      'flyGravity_S': '0 0.00015 0', 'visSizeStart_S': '0.4 0.4', 'visSizeEndScale': 0.6,
                      'visAlphaStart': 255.0, 'visAlphaEnd': 40.0, 'visAlphaFunc_S': 'ADD'},
              'curl': {'strength': 2.0, 'frequency': 2.5, 'speed': 0.3}, 'drag': 1.4, 'stretch': 0.025, 'minPixels': 1.5}
    # the smoke of dry wood: thin wisps that turn and unwind (a simulated puff's 64 frames, each particle from its
    # own frame), leave the flame's tips fast and slow down, widen and drift; lit by the sun, the sky and the fire
    smoke = {'name': 'OWN_CAMP_SMOKE', 'base': 'LIGHTSMOKE',
             'flipbook': {'cols': 8, 'rows': 8, 'frames': 64, 'fps': 9},
             'set': {'visName_S': SMOKE[0], 'visAlphaFunc_S': 'BLEND', 'visAlphaStart': 150.0, 'visAlphaEnd': 0.0,
                     'visTexColorStart_S': '255 255 255', 'visTexColorEnd_S': '255 255 255',
                     'visSizeStart_S': '45 45', 'visSizeEndScale': 4.5, 'lspPartAvg': 6000.0, 'lspPartVar': 1200.0,
                     'ppsValue': 4.0, 'shpType_S': 'CIRCLE', 'shpDim_S': '14 0 14', 'shpOffsetVec_S': '0 0 0',
                     'dirMode_S': 'DIR', 'velAvg': 0.11, 'velVar': 0.03, 'flyGravity_S': '0 0 0'},
             'curl': {'strength': 0.7, 'frequency': 0.8, 'speed': 0.25}, 'drag': 0.35}
    return {'format': 1, 'effects': [card('OWN_CAMP_FLAME', TONGUE, CORE, 34), card('OWN_CAMP_FLAME_MID', TONGUE, MID, 38),
                                     card('OWN_CAMP_FLAME_SIDE', TONGUE, SIDE, 46), licks, sparks, smoke]}


def base_dir(given) -> Path:
    if given:
        return Path(given)
    found = sorted(p for p in BASE_CACHE.iterdir() if (p / 'worlds/NEWWORLD/vobs.json').is_file())
    if len(found) != 1:
        sys.exit(f'{BASE_CACHE}: {len(found)} bases; name one with --base')
    return found[0]


def campfires(vobs_json) -> list:
    found = []

    def walk(vob):
        if str(vob.get('visual_name', '')).upper() == MESH:
            found.append(vob)
        for child in vob.get('children', []):
            walk(child)

    for vob in json.loads(Path(vobs_json).read_text())['vobs']:
        walk(vob)
    return sorted(found, key=lambda vob: vob['id'])


def at(point) -> list:
    return [round(float(v), 1) for v in point]


def fire(vob) -> dict:
    """The fire of one campfire, world cm. The flames stand upright on the ash whatever the model's tilt."""
    origin = np.array(vob['position'], float)
    turn = (vob['id'] * 2.39996) % (2 * math.pi)  # every fire turned its own way
    ring = lambda r, a, y: origin + np.array([r * math.cos(turn + a), y, r * math.sin(turn + a)])
    third = 2 * math.pi / 3
    points = [('flame', 'OWN_CAMP_FLAME', origin + [0, BED + CORE[1] / 2 - 4, 0]),
              ('flame-m', 'OWN_CAMP_FLAME_MID', ring(20, 0.5, BED + MID[1] / 2 - 3)),
              ('flame-n', 'OWN_CAMP_FLAME_MID', ring(22, 0.5 + math.pi, BED + MID[1] / 2 + 2)),
              ('flame-a', 'OWN_CAMP_FLAME_SIDE', ring(38, 0, BED + SIDE[1] / 2 - 4)),
              ('flame-b', 'OWN_CAMP_FLAME_SIDE', ring(34, third, BED + SIDE[1] / 2 + 3)),
              ('flame-c', 'OWN_CAMP_FLAME_SIDE', ring(40, 2 * third, BED + SIDE[1] / 2 - 6)),
              ('licks', 'OWN_CAMP_LICKS', origin + [0, BED + 10, 0]),
              ('sparks', 'OWN_CAMP_SPARKS', origin + [0, BED + 50, 0]),
              ('smoke', 'OWN_CAMP_SMOKE', origin + [0, BED + 115, 0])]
    return dict(emitters=points, light=origin + [0, LIGHT_UP, 0])


def layer(fires) -> tuple[dict, list]:
    out = {'format': 1, 'remove': [], 'light': [], 'add': []}
    left = []
    for vob in fires:
        children = vob.get('children', [])
        gone = [c for c in children if str(c.get('visual_name', '')).upper() in GONE]
        if not any(str(c.get('visual_name', '')).upper() == 'FIRE_HOT.PFX' for c in gone):
            left.append((vob['id'], 'no fire in the base: a cold fireplace'))
            continue
        key = f'campfire-{vob["id"]}'
        spot = fire(vob)
        out['remove'] += [c['id'] for c in gone]
        for end, pfx, position in spot['emitters']:
            entry = {'key': f'{key}-{end}', 'kind': 'emitter', 'pfx': pfx, 'position': at(position), 'yaw_deg': 0}
            if 'flame' in end:
                entry['flicker'] = {'pulse_hz': 1.6 if end == 'flame' else 2.6, 'jitter_hz': 9, 'amount': 0.25,
                                    'seed': 11 + vob['id'] * 3 + len(out['add'])}
            out['add'].append(entry)
        signal = {'flicker_from': f'{key}-flame', **SWAY}
        lamps = [c for c in children if c['type'] == 'zCVobLight' and not c['is_static'] and c.get('on', True)]
        near = sorted(lamps, key=lambda c: float(np.linalg.norm(spot['light'] - np.array(c['position']))))
        offset = at(spot['light'] - np.array(near[0]['position'])) if near else None
        if near and max(abs(v) for v in offset) <= 100:
            out['light'].append({'id': near[0]['id'], 'color': LAMP['color'], 'near': LAMP['near'], **signal, 'offset_cm': offset})
        else:
            out['add'].append({'key': f'{key}-light', 'kind': 'light', 'position': at(spot['light']), 'yaw_deg': 0,
                               'range': LIGHT_RANGE, 'color': LAMP['color'], 'near': LAMP['near'], **signal})
    return out, left


def write_layer(path, data) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    row = lambda entries: [json.dumps(e) + (',' if i + 1 < len(entries) else '') for i, e in enumerate(entries)]
    lines = ['{', ' "format": 1,', f' "remove": {json.dumps(data["remove"])},', ' "light": [',
             *('  ' + r for r in row(data['light'])), ' ],', ' "add": [', *('  ' + r for r in row(data['add'])), ' ]', '}']
    path.write_text('\n'.join(lines) + '\n')


def cmd_effects(_args) -> int:
    PARTICLES.parent.mkdir(parents=True, exist_ok=True)
    PARTICLES.write_text(json.dumps(effects()) + '\n')
    print(f'{PARTICLES.relative_to(ROOT)}: {len(effects()["effects"])} effects')
    return 0


def cmd_layer(args) -> int:
    fires = campfires(base_dir(args.base) / 'worlds/NEWWORLD/vobs.json')
    data, left = layer(fires)
    write_layer(LAYER, data)
    lights = sum(e['kind'] == 'light' for e in data['add'])
    print(f'{LAYER.relative_to(ROOT)}: {len(fires) - len(left)} of {len(fires)} campfires, {len(data["remove"])} removes, '
          f'{len(data["light"])} lamps of the base, {lights} lights added')
    for vob_id, why in left:
        print(f'  left as it is: {vob_id}: {why}')
    return 0


def cmd_pack(args) -> int:
    output = STAGING / args.set
    if output.exists():
        sys.exit(f'{output}: the staging set exists; choose a new name')
    if not args.smoke.is_file():
        sys.exit(f'{args.smoke}: the smoke atlas is missing')
    for source in (PARTICLES, LAYER):
        target = output / source.relative_to(CONTENT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    smoke = output / 'materials' / SMOKE[1]
    smoke.parent.mkdir(parents=True)
    shutil.copyfile(args.smoke, smoke)
    (output / 'materials/materials.json').write_text(json.dumps({SMOKE[0]: {'albedo': SMOKE[1]}}, indent=2) + '\n')
    print(f'{output}: the campfires\' fire as a part; it needs {TONGUE} of torch-model-*')
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
    pack.add_argument('--smoke', type=Path, required=True)
    pack.set_defaults(func=cmd_pack)
    args = parser.parse_args()
    return args.func(args)


if __name__ == '__main__':
    raise SystemExit(main())
