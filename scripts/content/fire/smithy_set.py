#!/usr/bin/env python3
"""The smithy's own fire: the forge's fire under the bellows and the scale thrown off the anvil, by the game's names.

    python3 scripts/content/fire/smithy_set.py build
    python3 scripts/content/fire/smithy_set.py pack --set smithy-1

reports/content/remaster/smithy-fire. The objects' animations ask for two particle effects by name (`*eventPFX`):
FIRE_HOT on the forge's bone PFX_FIRE while the smith works the bellows (BSFIRE_OC, T_S0_2_S1 to T_S1_2_S0) and
ANVIL_SPARKS on the anvil's PFX_SPARK at every blow (BSANVIL_OC, S_S1). The part declares own effects under those
names (`replaces`). In the working content no vob of the worlds burns FIRE_HOT any more (the fire parts removed it or
swapped it for their own tongues), so the forge is its only user. The file sorts after the brazier's effects,
which the forge is based on (the anvil's on the game's FIRE_SPARKS: no wind, no plume, no curl for falling scale); the set needs torch-model-*, brazier-* and smoke-*.

The forge (a bed of charcoal about 45 cm wide under a hood): low tongues licking out of the coals, one steady small
flame, a few sparks of burning fines and a thin smoke into the hood. The tongues and sparks swell at every push of the
bellows and settle between them: the loop S_S1 is 62 frames at 25 a second, 2.48 s; the fire starts at frame 11 of
T_S0_2_S1, 26 frames before the loop, and the push (its sound FORGE_FIRE) comes 7 frames into the loop, so the first
push is 1.32 s after the fire starts. No column of flame: the fire stays in the coals. No light of its own yet: an
effect's light (particles.d "light") stands at the emitter, here inside the forge's base, and plays its keys once; the
smithy's lamp of the world lights the place.

The anvil (a blow on iron at forging heat): a handful of flakes of scale, dark orange, thrown sideways and down at a
few metres a second; they fall, land on the floor (particles.d "land") and glow out there. No shower of bright sparks:
that is iron at welding heat.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import shutil
import sys

from campfire_set import ROOT, STAGING, TONGUE, card, haze

CONTENT = ROOT / 'reports/content/remaster/smithy-fire/content'
PARTICLES = CONTENT / 'particles.d/zz-smithy.json'

LOOP_S = 62 / 25.0           # the bellows' loop S_S1
FIRST_PUSH_S = (26 + 7) / 25.0
KEYS = 31                    # keys over one loop
FPS = KEYS / LOOP_S          # 12.5 keys a second: the keys repeat with the bellows
BED = 22                     # cm: the radius of the burning part of the bed
# cm in the forge's bone PFX_FIRE (it stands 10 cm over the floor inside the forge's base; its X points where the smith
# stands, BSFIRE_OC.MDH). The up-facing stones of the bowl (BSFIRE_OC.MDM, MOSTSTONES02) have their centre at
# (-8, 77, 1) in it: the fire stands there, its tongues born just under the bowl's mean surface. The game's FIRE_HOT
# reaches the coals from the bone with tongues of 60 cm thrown up at 2 m/s.
COALS = 74.0
FRONT = -8.0


def over(height: float) -> str:
    """An offset in the bone's frame: so on every forge, however it is turned."""
    return f'{FRONT:g} {height:g} 0'


def pulse(low: float, high: float, rise_s: float = 0.25, fall_s: float = 1.1) -> list[float]:
    """One loop of keys: `low` between the pushes, up to `high` in `rise_s` after a push and back down over `fall_s`."""
    out = []
    for k in range(KEYS):
        t = (k / FPS - FIRST_PUSH_S) % LOOP_S
        if t < rise_s:
            v = t / rise_s
        else:
            v = math.exp(-(t - rise_s) / (fall_s / 3))
        out.append(round(low + (high - low) * v, 3))
    return out


def keys(values) -> str:
    return ' '.join(f'{v:g}' for v in values)


def looping(values) -> dict:
    return {'ppsScaleKeys_S': keys(values), 'ppsIsLooping': 1, 'ppsIsSmooth': 1, 'ppsFPS': round(FPS, 4)}


def forge() -> list:
    bed = {'shpType_S': 'CIRCLE', 'shpFOR_S': 'OBJECT', 'shpIsVolume': 1, 'shpDim_S': f'{BED} 0 {BED}',
           'shpOffsetVec_S': over(COALS)}
    up = {'dirMode_S': 'DIR', 'dirFOR_S': 'WORLD', 'dirAngleHead': 0.0, 'dirAngleHeadVar': 179.0, 'dirAngleElev': 90.0}
    flame = card('OWN_FORGE_FLAME', TONGUE, (16, 25), 50)
    heat = haze('OWN_FORGE_HAZE', 30, 14, 1100)
    flame['set'].update({'shpFOR_S': 'OBJECT', 'shpOffsetVec_S': over(COALS + 11)})
    return [
        flame,
        # glowing bits of coal on the bed: still points that glow up and out, more of them at a push; they tie the
        # tongues to the coals (round 2: the tongues alone hung over a dark bowl)
        {'name': 'OWN_FORGE_COALS', 'base': 'FIRE_SPARKS',
         'set': {**bed, **looping(pulse(0.6, 1.5)), 'shpOffsetVec_S': over(COALS - 2), 'ppsValue': 30.0,
                 'dirMode_S': 'NONE', 'velAvg': 0.0, 'velVar': 0.0, 'flyGravity_S': '0 0 0', 'lspPartAvg': 1300.0,
                 'lspPartVar': 500.0, 'visOrientation_S': 'NONE', 'visSizeStart_S': '2.5 2.5', 'visSizeEndScale': 0.7,
                 'visTexColorStart_S': '255 130 35', 'visTexColorEnd_S': '170 35 0', 'visAlphaStart': 210.0,
                 'visAlphaEnd': 0.0, 'visAlphaFunc_S': 'ADD'},
         'minPixels': 1.5},
        {'name': 'OWN_FORGE_SPARKS', 'base': 'OWN_BRAZIER_SPARKS',
         'set': {**bed, **up, **looping(pulse(0.3, 1.6)), 'ppsValue': 6.0, 'dirAngleElevVar': 25.0, 'velAvg': 0.2,
                 'velVar': 0.08, 'lspPartAvg': 800.0, 'lspPartVar': 350.0}},
        {'name': 'OWN_FORGE_SMOKE', 'base': 'OWN_BRAZIER_SMOKE',
         'set': {**bed, 'shpDim_S': '14 0 14', 'shpOffsetVec_S': over(COALS + 15), 'ppsValue': 4.0, 'visAlphaStart': 26.0,
                 'visSizeStart_S': '28 28', 'visSizeEndScale': 3.0, 'velAvg': 0.12, 'lspPartAvg': 3500.0}},
        {**heat, 'set': {**heat['set'], 'shpFOR_S': 'OBJECT', 'shpOffsetVec_S': over(COALS + 20)}},
        # the tongues out of the coals: the game's name, so every forge asks for this
        {'name': 'FIRE_HOT', 'base': 'OWN_BRAZIER_LICKS', 'replaces': True,
         'with': ['OWN_FORGE_FLAME', 'OWN_FORGE_COALS', 'OWN_FORGE_SPARKS', 'OWN_FORGE_SMOKE', 'OWN_FORGE_HAZE'],
         'set': {**bed, **up, **looping(pulse(0.55, 1.4)), 'ppsValue': 55.0, 'dirAngleElevVar': 12.0,
                 'visSizeStart_S': '15 25', 'visSizeEndScale': 0.5, 'velAvg': 0.035, 'velVar': 0.015,
                 'lspPartAvg': 420.0, 'lspPartVar': 120.0}},
    ]


def anvil() -> list:
    return [
        # a flake on the floor: it lies where it landed and glows out
        {'name': 'OWN_ANVIL_SCALE_FLOOR', 'base': 'FIRE_SPARKS',
         'set': {'ppsValue': 20.0, 'ppsScaleKeys_S': '1', 'ppsIsLooping': 0, 'ppsIsSmooth': 0, 'ppsFPS': 20.0,
                 'shpType_S': 'POINT', 'shpIsVolume': 0, 'shpDim_S': '0', 'shpOffsetVec_S': '0 0 0',
                 'dirMode_S': 'NONE', 'velAvg': 0.0, 'velVar': 0.0, 'flyGravity_S': '0 0 0',
                 'lspPartAvg': 800.0, 'lspPartVar': 350.0, 'visOrientation_S': 'NONE', 'visSizeStart_S': '0.6 0.6',
                 'visSizeEndScale': 0.8, 'visTexColorStart_S': '230 90 20', 'visTexColorEnd_S': '120 20 0',
                 'visAlphaStart': 220.0, 'visAlphaEnd': 0.0, 'visAlphaFunc_S': 'ADD'},
         'minPixels': 1.5},
        # the blow: the game's burst (20 keys a second, one key: 50 ms) of flakes of scale, thrown all around and
        # down from the anvil's face at 1.5-3.5 m/s, falling under gravity
        {'name': 'ANVIL_SPARKS', 'base': 'FIRE_SPARKS', 'replaces': True, 'land': 'OWN_ANVIL_SCALE_FLOOR',
         'set': {'ppsValue': 170.0, 'ppsScaleKeys_S': '1', 'ppsIsLooping': 0, 'ppsIsSmooth': 0, 'ppsFPS': 20.0,
                 'useEmittersFOR': 0, 'shpType_S': 'SPHERE', 'shpFOR_S': 'WORLD', 'shpIsVolume': 1, 'shpDim_S': '3',
                 'shpOffsetVec_S': '0 0 0', 'dirMode_S': 'DIR', 'dirFOR_S': 'WORLD', 'dirAngleHead': 0.0,
                 'dirAngleHeadVar': 179.0, 'dirAngleElev': 15.0, 'dirAngleElevVar': 35.0, 'velAvg': 0.25,
                 'velVar': 0.1, 'flyGravity_S': '0 -0.00098 0', 'lspPartAvg': 1100.0, 'lspPartVar': 400.0,
                 'visOrientation_S': 'VELO', 'visSizeStart_S': '0.7 0.7', 'visSizeEndScale': 0.7,
                 'visTexColorStart_S': '255 150 50', 'visTexColorEnd_S': '210 50 5', 'visAlphaStart': 255.0,
                 'visAlphaEnd': 90.0, 'visAlphaFunc_S': 'ADD'},
         'drag': 0.4, 'stretch': 0.02, 'minPixels': 1.5},
    ]


def cmd_build(_args) -> int:
    PARTICLES.parent.mkdir(parents=True, exist_ok=True)
    effects = forge() + anvil()
    PARTICLES.write_text(json.dumps({'format': 1, 'effects': effects}, indent=1) + '\n')
    print(f'{PARTICLES.relative_to(ROOT)}: {len(effects)} effects')
    return 0


def cmd_pack(args) -> int:
    output = STAGING / args.set
    if output.exists():
        sys.exit(f'{output}: the staging set exists; choose a new name')
    target = output / PARTICLES.relative_to(CONTENT)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(PARTICLES, target)
    print(f'{output}: the smithy\'s fire as a part; it needs the effects of torch-model-*, brazier-* and smoke-*')
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('build').set_defaults(func=cmd_build)
    pack = sub.add_parser('pack')
    pack.add_argument('--set', required=True)
    pack.set_defaults(func=cmd_pack)
    args = parser.parse_args()
    return args.func(args)


if __name__ == '__main__':
    raise SystemExit(main())
