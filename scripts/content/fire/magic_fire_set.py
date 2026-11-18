#!/usr/bin/env python3
"""The fire of spells as a part of the working content: own effects that take the place of the game's by name.

    python3 scripts/content/fire/magic_fire_set.py build
    python3 scripts/content/fire/magic_fire_set.py pack --set magic-fire-1

reports/content/remaster/magic-fire. A spell's VisualFx asks for one particle effect by name in each of its states
(in the hand, flying, hit). The part declares own effects under those names (`replaces`), each bringing its layers
along (`with`): the flame card of the torch, its tongues, sparks and the smoke puff. The file sorts after the torch's
and the hand torch's effects, which it is based on; the set needs torch-model-*, fire-worlds-* and smoke-*.

The fire bolt (round 1):
- in the hand: a small flame standing on the palm, leaning as the hand moves, and its tongues;
- flying at 10 m/s: the flame card lies along the way behind the head (lean of the air past it), a dense head of
  tongues that goes with the bolt, tongues, sparks and thin smoke left behind as the tail;
- hit: the game keeps the effect on for as long as the target burns, so it is a small fire, not a burst: tongues,
  sparks on arcs and puffs of smoke that play their atlas once.
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys

from campfire_set import ROOT, STAGING

CONTENT = ROOT / 'reports/content/remaster/magic-fire/content'
PARTICLES = CONTENT / 'particles.d/zz-magic-fire.json'
SMOKE_BOOK = {'cols': 8, 'rows': 8, 'frames': 64, 'fps': 10}
ALL_AROUND = {'dirMode_S': 'DIR', 'dirFOR_S': 'WORLD', 'dirAngleHead': 0.0, 'dirAngleHeadVar': 179.0, 'dirAngleElev': 0.0,
              'dirAngleElevVar': 89.0}
TORCH = ROOT / 'reports/content/remaster/wall-torch-model/content/particles.d/wall-torch-fire.json'


def of_torch(source, name, change, **own) -> dict:
    """A torch's effect under a new name, without the world's wind and the plume over a standing flame."""
    entry = next(e for e in json.loads(TORCH.read_text())['effects'] if e['name'] == source)
    entry = {key: value for key, value in entry.items() if key not in ('wind', 'plume')}
    return {**entry, **own, 'name': name, 'set': {**entry['set'], **change}}


def foot(height) -> dict:
    """A leaning card stands half its height under its emitter (pfx.vert): lift it, so its foot is at the emitter."""
    return {'shpFOR_S': 'WORLD', 'shpOffsetVec_S': f'0 {height / 2:g} 0'}


def firebolt() -> list:
    return [
        # in the hand
        {'name': 'OWN_BOLT_HAND_LICKS', 'base': 'OWN_HAND_LICKS', 'inherit': 0.9,
         'set': {'ppsValue': 16.0, 'shpDim_S': '2.5 0 2.5', 'visSizeStart_S': '6 9', 'lspPartAvg': 300.0, 'lspPartVar': 80.0}},
        {'name': 'MFX_FIREBOLT_INIT', 'base': 'OWN_HAND_FLAME', 'replaces': True, 'with': ['OWN_BOLT_HAND_LICKS'],
         'set': {'visSizeStart_S': '12 16', **foot(16)}, 'lean': {'rise': 1.4, 'max_deg': 70, 'response': 0.1}},
        # flying
        # the bright core: the game's own glow of the bolt (declared before its name is replaced), dense and short-lived,
        # dropped along the way with 0.6 of the bolt's speed: a comet 60-70 cm long that tapers and reddens
        {'name': 'OWN_BOLT_GLOW', 'base': 'MFX_FIREBOLT_CAST', 'inherit': 0.6,
         'set': {'ppsValue': 220.0, 'lspPartAvg': 160.0, 'lspPartVar': 30.0, 'visSizeStart_S': '15 15', 'visSizeEndScale': 0.3,
                 'visTexColorStart_S': '255 225 170', 'visTexColorEnd_S': '255 90 10'}},
        {'name': 'OWN_BOLT_HEAD', 'base': 'OWN_HAND_LICKS', 'inherit': 1.0,
         'set': {'ppsValue': 70.0, 'shpType_S': 'SPHERE', 'shpIsVolume': 1, 'shpDim_S': '3', 'velAvg': 0.01, 'velVar': 0.005,
                 'flyGravity_S': '0 0 0', 'visSizeStart_S': '20 20', 'visSizeEndScale': 0.7, 'lspPartAvg': 130.0,
                 'lspPartVar': 30.0, **ALL_AROUND}},
        {'name': 'OWN_BOLT_TAIL', 'base': 'OWN_HAND_LICKS', 'inherit': 0.35,
         'set': {'ppsValue': 220.0, 'shpType_S': 'SPHERE', 'shpIsVolume': 1, 'shpDim_S': '4', 'velAvg': 0.02, 'velVar': 0.01,
                 'visSizeStart_S': '13 16', 'visSizeEndScale': 0.3, 'lspPartAvg': 240.0, 'lspPartVar': 70.0, **ALL_AROUND}},
        of_torch('OWN_TORCH_SPARKS', 'OWN_BOLT_SPARKS',
                 {'ppsValue': 35.0, 'shpDim_S': '3', 'velAvg': 0.06, 'velVar': 0.04, 'flyGravity_S': '0 -0.0003 0',
                 'lspPartAvg': 600.0, 'lspPartVar': 250.0, **ALL_AROUND}, inherit=0.5),
        of_torch('OWN_TORCH_SOOT', 'OWN_BOLT_SMOKE',
                 {'ppsValue': 22.0, 'ppsIsSmooth': 0, 'ppsScaleKeys_S': '', 'ppsFPS': 0.0, 'shpDim_S': '2 0 2',
                 'visSizeStart_S': '7 7', 'visSizeEndScale': 3.5, 'visAlphaStart': 70.0, 'lspPartAvg': 900.0,
                 'lspPartVar': 250.0, 'velAvg': 0.01, 'velVar': 0.005}, inherit=0.05),
        {'name': 'MFX_FIREBOLT_CAST', 'base': 'OWN_HAND_FLAME', 'replaces': True,
         'with': ['OWN_BOLT_GLOW', 'OWN_BOLT_HEAD', 'OWN_BOLT_TAIL', 'OWN_BOLT_SPARKS', 'OWN_BOLT_SMOKE'],
         'set': {'visSizeStart_S': '28 90', **foot(90)}, 'lean': {'rise': 1.8, 'max_deg': 86, 'response': 0.04}},
        # hit
        of_torch('OWN_TORCH_SPARKS', 'OWN_BOLT_HIT_SPARKS',
                 {'ppsValue': 30.0, 'shpDim_S': '4', 'velAvg': 0.3, 'velVar': 0.15, 'flyGravity_S': '0 -0.0007 0',
                 'lspPartAvg': 700.0, 'lspPartVar': 300.0, **ALL_AROUND}),
        of_torch('OWN_TORCH_SOOT', 'OWN_BOLT_HIT_PUFF',
                 {'ppsValue': 3.0, 'ppsIsSmooth': 0, 'ppsScaleKeys_S': '', 'ppsFPS': 0.0, 'shpType_S': 'SPHERE', 'shpIsVolume': 1, 'shpDim_S': '8',
                 'visSizeStart_S': '22 22', 'visSizeEndScale': 2.6, 'visAlphaStart': 90.0, 'lspPartAvg': 1500.0,
                 'lspPartVar': 300.0, 'velAvg': 0.03, 'velVar': 0.015, 'flyGravity_S': '0 2e-05 0', **ALL_AROUND}, flipbook={**SMOKE_BOOK, 'once': True}),
        {'name': 'MFX_FIREBOLT_COLLIDE', 'base': 'OWN_HAND_LICKS', 'replaces': True, 'inherit': 0.0,
         'with': ['OWN_BOLT_HIT_SPARKS', 'OWN_BOLT_HIT_PUFF'], 'drag': 5.0,
         'set': {'ppsValue': 70.0, 'shpType_S': 'SPHERE', 'shpIsVolume': 1, 'shpDim_S': '5', 'velAvg': 0.08,
                 'velVar': 0.04, 'flyGravity_S': '0 5e-05 0', 'visSizeStart_S': '10 15', 'visSizeEndScale': 1.2,
                 'lspPartAvg': 300.0, 'lspPartVar': 90.0, **ALL_AROUND}},
    ]


def cmd_build(_args) -> int:
    PARTICLES.parent.mkdir(parents=True, exist_ok=True)
    PARTICLES.write_text(json.dumps({'format': 1, 'effects': firebolt()}, indent=1) + '\n')
    print(f'{PARTICLES.relative_to(ROOT)}: {len(firebolt())} effects')
    return 0


def cmd_pack(args) -> int:
    output = STAGING / args.set
    if output.exists():
        sys.exit(f'{output}: the staging set exists; choose a new name')
    target = output / PARTICLES.relative_to(CONTENT)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(PARTICLES, target)
    print(f'{output}: the spells\' fire as a part; it needs the effects of torch-model-* and fire-worlds-* and smoke-*')
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
