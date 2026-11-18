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
- hit: one burst: a flash, the bolt's fire thrown apart as tongues, sparks on arcs, puffs of smoke that play their
  atlas once and a gust of hot air. The bolt's own flame goes at the hit.
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
BURST = {'ppsScaleKeys_S': '', 'ppsIsLooping': 0, 'ppsIsSmooth': 0, 'ppsFPS': 0.0}
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
        # hit: one burst (an effect without emission keys that is not looped gives its ppsValue particles at once)
        # the flash: the game's flare of the hit, a few large ones for a sixth of a second
        {'name': 'OWN_BOLT_HIT_FLASH', 'base': 'MFX_FIREBOLT_COLLIDE',
         'set': {'ppsValue': 6.0, 'lspPartAvg': 180.0, 'lspPartVar': 30.0, 'visSizeStart_S': '55 55', 'visSizeEndScale': 1.8,
                 'visTexColorStart_S': '255 235 190', 'visTexColorEnd_S': '255 110 25', 'velAvg': 0.01, 'velVar': 0.005,
                 'shpDim_S': '3'}},
        of_torch('OWN_TORCH_SPARKS', 'OWN_BOLT_HIT_SPARKS',
                 {**BURST, 'ppsValue': 50.0, 'shpDim_S': '4', 'velAvg': 0.42, 'velVar': 0.26, 'flyGravity_S': '0 -0.0009 0',
                 'lspPartAvg': 800.0, 'lspPartVar': 350.0, 'visSizeStart_S': '0.6 0.6',
                 'visTexColorStart_S': '255 170 60', 'visTexColorEnd_S': '200 40 0', **ALL_AROUND}, drag=1.0, minPixels=2.0),
        of_torch('OWN_TORCH_SOOT', 'OWN_BOLT_HIT_PUFF',
                 {**BURST, 'ppsValue': 3.0, 'shpType_S': 'SPHERE', 'shpIsVolume': 1, 'shpDim_S': '8',
                 'visSizeStart_S': '22 22', 'visSizeEndScale': 2.6, 'visAlphaStart': 45.0, 'lspPartAvg': 1500.0,
                 'lspPartVar': 300.0, 'velAvg': 0.03, 'velVar': 0.015, 'flyGravity_S': '0 2e-05 0', **ALL_AROUND},
                 flipbook={**SMOKE_BOOK, 'once': True}),
        haze('OWN_BOLT_HIT_HEAT', 8.0, inherit=0.0, **BURST, ppsValue=3.0, lspPartAvg=380.0, lspPartVar=60.0, shpDim_S='6',
             visSizeStart_S='55 55', visSizeEndScale=1.8),
        # the splash: the bolt's fire thrown apart, tongues that the air stops within a hand's breadth and that rise
        {'name': 'MFX_FIREBOLT_COLLIDE', 'base': 'OWN_HAND_LICKS', 'replaces': True, 'inherit': 0.0,
         'with': ['OWN_BOLT_HIT_FLASH', 'OWN_BOLT_HIT_SPARKS', 'OWN_BOLT_HIT_PUFF', 'OWN_BOLT_HIT_HEAT'], 'drag': 4.0,
         'set': {**BURST, 'ppsValue': 45.0, 'shpType_S': 'SPHERE', 'shpIsVolume': 1, 'shpDim_S': '5', 'velAvg': 0.3,
                 'velVar': 0.12, 'flyGravity_S': '0 8e-05 0', 'visSizeStart_S': '13 18', 'visSizeEndScale': 1.5,
                 'lspPartAvg': 300.0, 'lspPartVar': 90.0, **ALL_AROUND}},
    ]


def body_fire() -> list:
    """The fire on a burning body (body-fire.md, concept A: the body burns like a wick).

    The game's VOB_MAGICBURN is seven emitters of one effect on bones and one of smoke on the root. The seven take the
    tongues born on the body's skinned mesh, a seventh of the whole each; the one on the root takes what there is one
    of: the plume over the shoulders, the smoke, the embers and the hot air.
    """
    up = {'shpFOR_S': 'WORLD'}
    # the game runs the effect once, not in a loop: it burns for as long as its emission keys last (4.3 s, as the
    # game's own); the keys are the stages: the fire spreads in half a second, holds and dies down
    burn = {'ppsScaleKeys_S': '0.3 0.7 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 0.9 0.7 0.5 0.3', 'ppsFPS': 6.0,
            'ppsIsLooping': 0, 'ppsIsSmooth': 1}
    return [
        # tall tongues: born on the mesh as the short ones, they rise higher and join over the shoulders into the plume
        {'name': 'OWN_BODY_TALL', 'base': 'OWN_HAND_LICKS', 'inherit': 0.85,
         'set': {**burn, 'ppsValue': 9.0, 'shpType_S': 'MESH', 'shpIsVolume': 0, 'shpDim_S': '0 0 0',
                 'shpOffsetVec_S': '0 0 0', 'visSizeStart_S': '34 60', 'visSizeEndScale': 0.75, 'lspPartAvg': 560.0,
                 'lspPartVar': 140.0, 'velAvg': 0.05, 'velVar': 0.02, 'flyGravity_S': '0 0.00012 0'}},
        of_torch('OWN_TORCH_SOOT', 'OWN_BODY_SMOKE',
                 {**burn, 'ppsValue': 1.6, 'shpDim_S': '8 0 8', **up, 'shpOffsetVec_S': '0 45 0',
                 'visSizeStart_S': '26 26', 'visSizeEndScale': 3.2, 'visAlphaStart': 110.0, 'lspPartAvg': 2200.0,
                 'lspPartVar': 500.0}, inherit=0.1),
        of_torch('OWN_TORCH_SPARKS', 'OWN_BODY_EMBERS',
                 {**burn, 'ppsValue': 2.5, 'shpDim_S': '12', **up, 'shpOffsetVec_S': '0 20 0', 'velAvg': 0.12,
                 'velVar': 0.07, 'flyGravity_S': '0 0.00012 0', 'lspPartAvg': 1100.0, 'lspPartVar': 450.0,
                 'visSizeStart_S': '0.6 0.6'}, inherit=0.2, minPixels=2.0),
        haze('OWN_BODY_HEAT', 5.0, inherit=0.3, **burn, ppsValue=1.0, lspPartAvg=600.0, lspPartVar=120.0, shpDim_S='10',
             **up, shpOffsetVec_S='0 50 0', visSizeStart_S='60 60', visSizeEndScale=1.5, flyGravity_S='0 0.0001 0'),
        # the short tongues on the clothes: born on the body's mesh, they rise and go out. Each of the seven emitters
        # brings a seventh of the smoke, the embers and the hot air, over its own bone
        {'name': 'MFX_MAGICFIRE_HUMANBURN', 'base': 'OWN_HAND_LICKS', 'replaces': True, 'inherit': 0.92,
         'with': ['OWN_BODY_TALL', 'OWN_BODY_SMOKE', 'OWN_BODY_EMBERS', 'OWN_BODY_HEAT'],
         'set': {**burn, 'ppsValue': 30.0, 'shpType_S': 'MESH', 'shpIsVolume': 0, 'shpDim_S': '0 0 0', 'shpOffsetVec_S': '0 0 0',
                 'visSizeStart_S': '24 40', 'visSizeEndScale': 0.7, 'lspPartAvg': 380.0, 'lspPartVar': 100.0,
                 'velAvg': 0.02, 'velVar': 0.01}},
    ]


def cmd_build(_args) -> int:
    PARTICLES.parent.mkdir(parents=True, exist_ok=True)
    PARTICLES.write_text(json.dumps({'format': 1, 'effects': firebolt() + body_fire()}, indent=1) + '\n')
    print(f'{PARTICLES.relative_to(ROOT)}: {len(firebolt()) + len(body_fire())} effects')
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
