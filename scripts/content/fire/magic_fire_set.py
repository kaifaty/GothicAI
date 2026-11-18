#!/usr/bin/env python3
"""The fire of spells as a part of the working content: own effects that take the place of the game's by name.

    python3 scripts/content/fire/magic_fire_set.py build
    python3 scripts/content/fire/magic_fire_set.py pack --set magic-fire-1

reports/content/remaster/magic-fire. A spell's VisualFx asks for one particle effect by name in each of its states
(in the hand, flying, hit). The part declares own effects under those names (`replaces`), each bringing its layers
along (`with`): the flame card of the torch, its tongues, sparks and the smoke puff. The file sorts after the torch's
and the hand torch's effects, which it is based on; the set needs torch-model-*, fire-worlds-* and smoke-*.

The fire bolt (round 2, firebolt-design.md: one body on one axis):
- in the hand: a small flame standing on the palm, leaning as the hand moves, and its tongues;
- flying at 10 m/s: a white-hot head, one flame pressed to the way by the air behind it (the torch's flame card lying
  along the way, a short bright one inside it as its root), embers left behind that cool and sink, the air shimmers around the flame
  and in a wake behind it. No standing tongues, no second tail, no smoke;
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


def haze(name, shift, inherit, **change) -> dict:
    """Hot air (particles.d "haze"): a sprite without a colour of its own that shows the frame behind it shifted."""
    return {'name': name, 'base': 'LIGHTSMOKE', 'haze': {'shift': shift, 'cell': 4.0}, 'inherit': inherit, 'drag': 0.3,
            'set': {'visName_S': 'OWN_SMOKE_WISPY.TGA', 'visAlphaFunc_S': 'BLEND', 'visAlphaStart': 255.0, 'visAlphaEnd': 0.0,
                    'visTexColorStart_S': '255 255 255', 'visTexColorEnd_S': '255 255 255', 'shpType_S': 'SPHERE',
                    'shpIsVolume': 1, 'shpOffsetVec_S': '0 0 0', 'velAvg': 0.01, 'velVar': 0.005, 'flyGravity_S': '0 0 0',
                    'visOrientation_S': 'NONE', **ALL_AROUND, **change}}


def firebolt() -> list:
    return [
        # in the hand
        {'name': 'OWN_BOLT_HAND_LICKS', 'base': 'OWN_HAND_LICKS', 'inherit': 0.9,
         'set': {'ppsValue': 16.0, 'shpDim_S': '2.5 0 2.5', 'visSizeStart_S': '6 9', 'lspPartAvg': 300.0, 'lspPartVar': 80.0}},
        {'name': 'MFX_FIREBOLT_INIT', 'base': 'OWN_HAND_FLAME', 'replaces': True, 'with': ['OWN_BOLT_HAND_LICKS'],
         'set': {'visSizeStart_S': '12 16', **foot(16)}, 'lean': {'rise': 1.4, 'max_deg': 70, 'response': 0.1}},
        # flying: one body on one axis (firebolt-design.md, concept A with the embers of C)
        # the head: a small white-hot core that goes with the bolt, the only place of full brightness
        # (the game's glow of the bolt without its ribbons and its offset from the axis, in the emitter's frame)
        {'name': 'OWN_BOLT_HEAD', 'base': 'MFX_FIREBOLT_CAST',
         'set': {'ppsValue': 80.0, 'lspPartAvg': 100.0, 'lspPartVar': 20.0, 'visSizeStart_S': '25 25', 'visSizeEndScale': 0.5,
                 'visTexColorStart_S': '255 240 205', 'visTexColorEnd_S': '255 170 70', 'shpOffsetVec_S': '0 0 0',
                 'shpDim_S': '2', 'trlWidth': 0.0, 'trlFadeSpeed': 0.0, 'visOrientation_S': 'NONE'}},
        # the hot root of the flame: a short bright card inside the body
        {'name': 'OWN_BOLT_ROOT', 'base': 'OWN_HAND_FLAME', 'set': {'visSizeStart_S': '36 50', **foot(50)},
         'lean': {'rise': 1.8, 'max_deg': 87, 'response': 0.03, 'along': 0.2, 'grow': 0.3, 'grow_from': 0.25}, 'hdr': {'from': 1, 'to': 16, 'edge': 0.06}},
        # embers: left behind on the way, they drift, cool from orange to dull red and sink
        of_torch('OWN_TORCH_SPARKS', 'OWN_BOLT_EMBERS',
                 {'ppsValue': 110.0, 'shpDim_S': '5', 'velAvg': 0.07, 'velVar': 0.04, 'flyGravity_S': '0 -0.00025 0',
                 'lspPartAvg': 900.0, 'lspPartVar': 400.0, 'visSizeStart_S': '0.7 0.7', 'visSizeEndScale': 0.6,
                 'visTexColorStart_S': '255 150 40', 'visTexColorEnd_S': '170 25 0', 'visAlphaEnd': 0.0, **ALL_AROUND},
                 inherit=0.12, minPixels=2.2),
        # the heat: the air shimmers around the flame and in a wake behind it that widens and calms down
        haze('OWN_BOLT_HEAT', 4.0, inherit=1.0, ppsValue=24.0, lspPartAvg=170.0, lspPartVar=40.0, shpDim_S='6',
             visSizeStart_S='60 60', visSizeEndScale=1.2),
        haze('OWN_BOLT_WAKE', 7.0, inherit=0.1, ppsValue=45.0, lspPartAvg=420.0, lspPartVar=90.0, shpDim_S='5',
             visSizeStart_S='34 34', visSizeEndScale=2.0),
        # the body: the flame pressed to the way by the air, its foot at the head, widest behind it, torn at the tip
        {'name': 'MFX_FIREBOLT_CAST', 'base': 'OWN_HAND_FLAME', 'replaces': True,
         'with': ['OWN_BOLT_HEAD', 'OWN_BOLT_ROOT', 'OWN_BOLT_EMBERS', 'OWN_BOLT_HEAT', 'OWN_BOLT_WAKE'],
         'set': {'visSizeStart_S': '40 100', **foot(100)},
         'lean': {'rise': 1.8, 'max_deg': 87, 'response': 0.03, 'along': 0.2, 'grow': 0.3, 'grow_from': 0.16}},
        # hit
        of_torch('OWN_TORCH_SPARKS', 'OWN_BOLT_HIT_SPARKS',
                 {'ppsValue': 30.0, 'shpDim_S': '4', 'velAvg': 0.3, 'velVar': 0.15, 'flyGravity_S': '0 -0.0007 0',
                 'lspPartAvg': 700.0, 'lspPartVar': 300.0, **ALL_AROUND}),
        of_torch('OWN_TORCH_SOOT', 'OWN_BOLT_HIT_PUFF',
                 {'ppsValue': 3.0, 'ppsIsSmooth': 0, 'ppsScaleKeys_S': '', 'ppsFPS': 0.0, 'shpType_S': 'SPHERE', 'shpIsVolume': 1, 'shpDim_S': '8',
                 'visSizeStart_S': '22 22', 'visSizeEndScale': 2.6, 'visAlphaStart': 45.0, 'lspPartAvg': 1500.0,
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
