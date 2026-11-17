#!/usr/bin/env python3
"""The own flame in the braziers of the world: the effects, the layer over the base's fires and the staging part.

    python3 scripts/content/fire/brazier_set.py effects
    python3 scripts/content/fire/brazier_set.py layer [--base DIR]
    python3 scripts/content/fire/brazier_set.py pack --set brazier-1

reports/content/remaster/brazier/README.md. Only the fire changes: the original's model (OC_LOB_LIGHTER_HUGE3, a
pole with a basket 46 cm wide, its rim 3.2-3.4 m over the foot) stays. The second step of the flame's rollout
after the campfire (campfire_set.py), with the same atlases: OWN_TORCH_FLAME.TGA of the part torch-model-* and
OWN_SMOKE_WISPY.TGA of the part smoke-*. A brazier loses FIRE_MEDIUM, its sparks, smoke and the glow sprite and
gets three flames, licks, sparks and smoke; its own lamp takes the flame's colour, flicker and slow sway (a
brazier without a lamp gets one). Many braziers lean up to 16 degrees: the fire stands in the tilted basket,
upright.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import shutil
import sys

from fire_wind import with_wind

import numpy as np

from campfire_set import BASE_CACHE, HDR, ROOT, SMOKE, STAGING, TONGUE, at, base_dir, card, haze, write_layer

CONTENT = ROOT / 'reports/content/remaster/brazier/content'
PARTICLES = CONTENT / 'particles.d/brazier.json'
LAYER = CONTENT / 'worlds/NEWWORLD/vobs.d/brazier.json'
MESH = 'OC_LOB_LIGHTER_HUGE3.3DS'
FIRE = 'FIRE_MEDIUM.PFX'
GONE = {FIRE, 'FIRE_SPARKS.PFX', 'FIRE_SMOKE.PFX', 'ZFLARE6.TGA'}

# cm in the model's frame: the basket's lower rim at 317, the upper at 335, the top ring at 340; it is 46 cm wide
# a bed of coals 0.4 m wide: a flame about half a metre high that puffs at 1.5 / sqrt(0.4) = 2.4 Hz
BED = 324.0          # the coals' top
CORE = (44, 68)      # the middle flame: the torch's flame twice as large
SIDE = (30, 47)      # two flames beside it fill the basket's width
LAMP = {'color': [230, 84, 15], 'near': 12}
SWAY = {'jitter_cm': 1.2, 'sway_hz': 1.3}
LIGHT_UP = 30.0      # over the coals
LIGHT_RANGE = 700


def effects() -> dict:
    licks = card('OWN_BRAZIER_LICKS', TONGUE, (17, 26), 56)
    licks['flipbook'].pop('steady')
    licks['set'].update({'ppsValue': 14.0, 'shpType_S': 'CIRCLE', 'shpDim_S': '13 0 13', 'dirMode_S': 'DIR',
                         'velAvg': 0.06, 'velVar': 0.02, 'lspPartAvg': 520.0, 'lspPartVar': 150.0,
                         'visSizeEndScale': 0.55, 'visAlphaEnd': 0.0})
    sparks = {'name': 'OWN_BRAZIER_SPARKS', 'base': 'FIRE_SPARKS',
              'set': {'ppsValue': 4.0, 'shpType_S': 'SPHERE', 'shpDim_S': '10', 'shpOffsetVec_S': '0 0 0',
                      'dirMode_S': 'DIR', 'velAvg': 0.16, 'velVar': 0.07, 'lspPartAvg': 1200.0, 'lspPartVar': 600.0,
                      'flyGravity_S': '0 0.00015 0', 'visSizeStart_S': '0.35 0.35', 'visSizeEndScale': 0.6,
                      'visAlphaStart': 255.0, 'visAlphaEnd': 40.0, 'visAlphaFunc_S': 'ADD'},
              'curl': {'strength': 2.0, 'frequency': 2.5, 'speed': 0.3}, 'drag': 1.4, 'stretch': 0.025, 'minPixels': 1.5}
    # coals and dry wood in an open basket: the campfire's thin grey veil at a third of its size (campfire_set.py:
    # grey and faint, many overlapping puffs; the first round's 3 white puffs a second read as cotton balls)
    smoke = {'name': 'OWN_BRAZIER_SMOKE', 'base': 'LIGHTSMOKE',
             'flipbook': {'cols': 8, 'rows': 8, 'frames': 64, 'fps': 9},
             'set': {'visName_S': SMOKE, 'visAlphaFunc_S': 'BLEND', 'visAlphaStart': 38.0, 'visAlphaEnd': 0.0,
                     'visTexColorStart_S': '128 130 136', 'visTexColorEnd_S': '150 152 158',
                     'visSizeStart_S': '36 36', 'visSizeEndScale': 3.4, 'lspPartAvg': 4500.0, 'lspPartVar': 1000.0,
                     'ppsValue': 9.0, 'shpType_S': 'CIRCLE', 'shpDim_S': '8 0 8', 'shpOffsetVec_S': '0 0 0',
                     'dirMode_S': 'DIR', 'velAvg': 0.1, 'velVar': 0.025, 'flyGravity_S': '0 0 0'},
             'curl': {'strength': 0.7, 'frequency': 0.9, 'speed': 0.25}, 'drag': 0.35}
    return {'format': 1, 'effects': [card('OWN_BRAZIER_FLAME', TONGUE, CORE, 44), card('OWN_BRAZIER_FLAME_SIDE', TONGUE, SIDE, 52),
                                     licks, sparks, smoke,
                                     # the campfire's shimmer at the basket's size: gone 1.3 m over the coals
                                     haze('OWN_BRAZIER_HAZE', 48, 10, 1500)]}


def braziers(vobs_json) -> list:
    found = []

    def walk(vob):
        if str(vob.get('visual_name', '')).upper() == MESH:
            found.append(vob)
        for child in vob.get('children', []):
            walk(child)

    for vob in json.loads(Path(vobs_json).read_text())['vobs']:
        walk(vob)
    return sorted(found, key=lambda vob: vob['id'])


def fire(vob) -> dict:
    """The fire of one brazier, world cm: the coals follow the model's tilt, the flames stand upright on them."""
    coals = np.array(vob['position'], float) + np.array(vob['rotation'], float) @ np.array([0.0, BED, 0.0])
    turn = (vob['id'] * 2.39996) % (2 * math.pi)
    ring = lambda r, a, y: coals + np.array([r * math.cos(turn + a), y, r * math.sin(turn + a)])
    points = [('flame', 'OWN_BRAZIER_FLAME', coals + [0, CORE[1] / 2 - 3, 0]),
              ('flame-a', 'OWN_BRAZIER_FLAME_SIDE', ring(9, 0, SIDE[1] / 2 - 2)),
              ('flame-b', 'OWN_BRAZIER_FLAME_SIDE', ring(10, math.pi + 0.4, SIDE[1] / 2 - 4)),
              ('licks', 'OWN_BRAZIER_LICKS', coals + [0, 6, 0]),
              ('sparks', 'OWN_BRAZIER_SPARKS', coals + [0, 25, 0]),
              ('smoke', 'OWN_BRAZIER_SMOKE', coals + [0, 42, 0]),
              ('haze', 'OWN_BRAZIER_HAZE', coals + [0, 36, 0])]
    return dict(emitters=points, light=coals + [0, LIGHT_UP, 0])


def layer(found) -> tuple[dict, list]:
    out = {'format': 1, 'remove': [], 'light': [], 'add': []}
    left = []
    for vob in found:
        children = vob.get('children', [])
        gone = [c for c in children if str(c.get('visual_name', '')).upper() in GONE]
        if not any(str(c.get('visual_name', '')).upper() == FIRE for c in gone):
            left.append((vob['id'], 'no fire in the base'))
            continue
        key = f'brazier-{vob["id"]}'
        spot = fire(vob)
        out['remove'] += [c['id'] for c in gone]
        smoked = any(str(c.get('visual_name', '')).upper() == 'FIRE_SMOKE.PFX' for c in gone)
        for end, pfx, position in spot['emitters']:
            if end == 'smoke' and not smoked:
                continue  # under a roof (the temple) the original has no smoke
            entry = {'key': f'{key}-{end}', 'kind': 'emitter', 'pfx': pfx, 'position': at(position), 'yaw_deg': 0}
            if 'flame' in end:
                entry['flicker'] = {'pulse_hz': 2.4 if end == 'flame' else 3.4, 'jitter_hz': 10, 'amount': 0.25,
                                    'seed': 17 + vob['id'] * 3 + len(out['add'])}
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


def cmd_effects(_args) -> int:
    PARTICLES.parent.mkdir(parents=True, exist_ok=True)
    PARTICLES.write_text(json.dumps(with_wind(effects())) + '\n')
    print(f'{PARTICLES.relative_to(ROOT)}: {len(effects()["effects"])} effects')
    return 0


def cmd_layer(args) -> int:
    found = braziers(base_dir(args.base) / 'worlds/NEWWORLD/vobs.json')
    data, left = layer(found)
    write_layer(LAYER, data)
    lights = sum(e['kind'] == 'light' for e in data['add'])
    print(f'{LAYER.relative_to(ROOT)}: {len(found) - len(left)} of {len(found)} braziers, {len(data["remove"])} removes, '
          f'{len(data["light"])} lamps of the base, {lights} lights added')
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
    print(f'{output}: the braziers\' fire as a part; it needs {TONGUE} of torch-model-* and {SMOKE} of smoke-*')
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
