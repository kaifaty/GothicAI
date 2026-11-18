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
from pathlib import Path
import shutil
import sys

from campfire_set import ROOT, STAGING

CONTENT = ROOT / 'reports/content/remaster/magic-fire/content'
PARTICLES = CONTENT / 'particles.d/zz-magic-fire.json'
SMOKE_BOOK = {'cols': 8, 'rows': 8, 'frames': 64, 'fps': 10}
ALL_AROUND = {'dirMode_S': 'DIR', 'dirFOR_S': 'WORLD', 'dirAngleHead': 0.0, 'dirAngleHeadVar': 179.0, 'dirAngleElev': 0.0,
              'dirAngleElevVar': 89.0}
BURST = {'ppsScaleKeys_S': '', 'ppsIsLooping': 0, 'ppsIsSmooth': 0, 'ppsFPS': 0.0}
# the spells' own atlases (explosion_atlas.py, Unity Labs CC0): texture name -> (place in the part, built file)
ATLASES = {'OWN_EXPLOSION_FIRE.TGA': ('own-explosion-fire/albedo.dds', 'explosion-fire.dds'),
           'OWN_EXPLOSION_SMOKE.TGA': ('own-explosion-smoke/albedo.dds', 'explosion-smoke.dds'),
           'OWN_FIREBALL.TGA': ('own-fireball/albedo.dds', 'fireball-loop.dds'),
           'OWN_FIRE_TRAIL.TGA': ('own-fire-trail/albedo.dds', 'fire-trail.dds')}
BLAST_BOOK = {'cols': 8, 'rows': 4, 'frames': 25, 'fps': 16, 'once': True}
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


def sz(k, *values) -> str:
    """Sizes in cm scaled by k, as a C_PARTICLEFX string."""
    return ' '.join(f'{round(v * k, 2):g}' for v in values)


def blast(name, tag, k) -> list:
    """A fire ball's explosion k times the plain one's size (fireball.md), one burst: a flash, the simulated burst
    (explosion_atlas.py) played once by two particles at one place, the fire added and the cloud blended, tongues
    thrown out and stopped by the air, sparks on arcs, smoke that stays, hot air, and its own light."""
    return [
        {'name': f'{tag}_FLASH', 'base': 'OWN_BOLT_HIT_FLASH',
         'set': {'ppsValue': 3.0, 'lspPartAvg': 130.0, 'lspPartVar': 20.0, 'visSizeStart_S': sz(k, 120, 120),
                 'shpDim_S': sz(k, 6)}},
        {'name': f'{tag}_BLAST', 'base': 'OWN_HAND_LICKS', 'inherit': 0.0, 'flipbook': BLAST_BOOK,
         'hdr': {'from': 1, 'to': 5, 'edge': 0.15},
         'set': {**BURST, 'ppsValue': 1.0, 'shpType_S': 'POINT', 'shpDim_S': '0', 'shpOffsetVec_S': '0 0 0',
                 'visName_S': 'OWN_EXPLOSION_FIRE.TGA', 'velAvg': 0.0, 'velVar': 0.0, 'flyGravity_S': '0 3e-05 0',
                 'visSizeStart_S': sz(k, 160, 160), 'visSizeEndScale': 2.1, 'visAlphaStart': 255.0, 'visAlphaEnd': 255.0,
                 'lspPartAvg': 1500.0, 'lspPartVar': 0.0}},
        {'name': f'{tag}_CLOUD', 'base': 'OWN_BOLT_HIT_PUFF', 'flipbook': BLAST_BOOK,
         'set': {'ppsValue': 1.0, 'shpType_S': 'POINT', 'shpIsVolume': 0, 'shpDim_S': '0', 'shpOffsetVec_S': '0 0 0',
                 'visName_S': 'OWN_EXPLOSION_SMOKE.TGA', 'velAvg': 0.0, 'velVar': 0.0, 'flyGravity_S': '0 3e-05 0',
                 'visSizeStart_S': sz(k, 160, 160), 'visSizeEndScale': 2.1, 'visAlphaStart': 255.0, 'visAlphaEnd': 130.0,
                 'lspPartAvg': 1500.0, 'lspPartVar': 0.0}},
        {'name': f'{tag}_SPARKS', 'base': 'OWN_BOLT_HIT_SPARKS',
         'set': {'ppsValue': round(180.0 * k), 'shpDim_S': sz(k, 12), 'velAvg': round(0.75 * k ** 0.5, 3),
                 'velVar': round(0.45 * k ** 0.5, 3), 'lspPartAvg': 1300.0, 'lspPartVar': 500.0,
                 'visSizeStart_S': '0.8 0.8'}},
        {'name': f'{tag}_SMOKE', 'base': 'OWN_BOLT_HIT_PUFF',
         'set': {'ppsValue': round(9.0 * k), 'shpDim_S': sz(k, 45), 'visSizeStart_S': sz(k, 70, 70), 'visSizeEndScale': 2.8,
                 'visAlphaStart': 80.0, 'lspPartAvg': 2800.0, 'lspPartVar': 500.0, 'velAvg': 0.05, 'velVar': 0.02,
                 'flyGravity_S': '0 4e-05 0'}},
        haze(f'{tag}_HEAT', 9.0, inherit=0.0, **BURST, ppsValue=5.0, lspPartAvg=600.0, lspPartVar=100.0,
             shpDim_S=sz(k, 25), visSizeStart_S=sz(k, 160, 160), visSizeEndScale=1.8),
        {'name': name, 'base': 'OWN_HAND_LICKS', 'replaces': True, 'inherit': 0.0, 'drag': 3.5,
         'with': [f'{tag}_CLOUD', f'{tag}_BLAST', f'{tag}_FLASH', f'{tag}_SPARKS', f'{tag}_SMOKE', f'{tag}_HEAT'],
         'light': {'range': min(round(800 * k), 2000), 'color': [255, 170, 80], 'fps': 8.0,
                   'keys': [1, 1, 0.85, 0.6, 0.4, 0.22, 0.1, 0]},
         'set': {**BURST, 'ppsValue': round(60.0 * k), 'shpType_S': 'SPHERE', 'shpIsVolume': 1, 'shpDim_S': sz(k, 14),
                 'velAvg': round(0.8 * k ** 0.5, 3), 'velVar': 0.3, 'flyGravity_S': '0 0.0001 0',
                 'visSizeStart_S': sz(k, 30, 42), 'visSizeEndScale': 1.9, 'lspPartAvg': 480.0, 'lspPartVar': 130.0,
                 **ALL_AROUND}},
    ]


def charge_ball(name, tag, k) -> list:
    """The charged fire ball at one level of its charge, k times the plain ball's size. The game shows one effect in
    the hand while the charge grows and in flight, so it has no standing flame card: a core, a ball of the looping
    fire, tongues that stay behind as its tail (and rise as a flame in a still hand), embers and hot air."""
    return [
        {'name': f'{tag}_CORE', 'base': 'OWN_BOLT_HEAD',
         'set': {'ppsValue': 40.0, 'lspPartAvg': 120.0, 'lspPartVar': 25.0, 'visSizeStart_S': sz(k, 30, 30),
                 'shpDim_S': sz(k, 3)}},
        {'name': f'{tag}_FLAMES', 'base': 'OWN_BALL_FLAMES',
         'set': {'shpDim_S': sz(k, 6), 'visSizeStart_S': sz(k, 62, 62)}},
        {'name': f'{tag}_LICKS', 'base': 'OWN_BALL_LICKS',
         'set': {'shpDim_S': sz(k, 16), 'visSizeStart_S': sz(k, 20, 28)}},
        {'name': f'{tag}_EMBERS', 'base': 'OWN_BALL_EMBERS', 'set': {'ppsValue': round(160.0 * k), 'shpDim_S': sz(k, 12)}},
        haze(f'{tag}_HEAT', 5.0, inherit=1.0, ppsValue=24.0, lspPartAvg=170.0, lspPartVar=40.0, shpDim_S=sz(k, 10),
             visSizeStart_S=sz(k, 120, 120), visSizeEndScale=1.2),
        {'name': name, 'base': 'OWN_HAND_LICKS', 'replaces': True, 'inherit': 0.3,
         'with': [f'{tag}_CORE', f'{tag}_FLAMES', f'{tag}_LICKS', f'{tag}_EMBERS', f'{tag}_HEAT'],
         'set': {'ppsValue': 70.0, 'shpType_S': 'SPHERE', 'shpIsVolume': 1, 'shpDim_S': sz(k, 12),
                 'visSizeStart_S': sz(k, 34, 48), 'visSizeEndScale': 0.5, 'lspPartAvg': 330.0, 'lspPartVar': 90.0,
                 'velAvg': 0.02, 'velVar': 0.01}},
    ]


def charged_fireball() -> list:
    """The charged fire ball (CHARGEFIREBALL): the ball and its explosion grow with each of the five levels."""
    levels = [('MFX_CHARGEFB_CAST', 1, 0.8), ('MFX_CHARGEFB_CAST_L2', 2, 1.0), ('MFX_CHARGEFB_CAST_L3', 3, 1.25),
              ('MFX_CHARGEFB_CAST_L4', 4, 1.5), ('MFX_CHARGEFB_CAST_L5', 5, 1.8)]
    blasts = [('MFX_CHARGEFB_COLLIDE', 'OWN_CB_HIT0', 1.0), ('MFX_FIREBALL_COLLIDE2', 'OWN_CB_HIT2', 1.3),
              ('MFX_FIREBALL_COLLIDE3', 'OWN_CB_HIT3', 1.6), ('MFX_FIREBALL_COLLIDE4', 'OWN_CB_HIT4', 2.0)]
    effects = []
    for name, level, k in levels:
        effects += charge_ball(name, f'OWN_CB{level}', k)
    for name, tag, k in blasts:
        effects += blast(name, tag, k)
    # a level of the charge gained: a short flash and a ring of sparks at the hand
    effects += [
        {'name': 'OWN_CB_STEP_SPARKS', 'base': 'OWN_BOLT_HIT_SPARKS', 'set': {'ppsValue': 40.0, 'velAvg': 0.3, 'velVar': 0.15}},
        {'name': 'MFX_CHARGEFB_INVESTBLAST', 'base': 'OWN_BOLT_HIT_FLASH', 'replaces': True, 'with': ['OWN_CB_STEP_SPARKS'],
         'set': {'ppsValue': 3.0, 'visSizeStart_S': '45 45'}},
    ]
    return effects


def fireball() -> list:
    """The fire ball (INSTANTFIREBALL; fireball.md): the bolt's fire, larger and round, and a real explosion."""
    return [
        # in the hand: a round white-hot core inside a flame twice the bolt's, so it reads as a ball and not a candle
        {'name': 'OWN_BALL_HAND_CORE', 'base': 'OWN_BOLT_HEAD',
         'set': {'ppsValue': 40.0, 'lspPartAvg': 160.0, 'lspPartVar': 30.0, 'visSizeStart_S': '20 20', 'visSizeEndScale': 0.7,
                 'shpDim_S': '1.5'}},
        {'name': 'OWN_BALL_HAND_LICKS', 'base': 'OWN_HAND_LICKS', 'inherit': 0.9,
         'set': {'ppsValue': 30.0, 'shpType_S': 'SPHERE', 'shpIsVolume': 0, 'shpDim_S': '7', 'visSizeStart_S': '9 13',
                 'lspPartAvg': 320.0, 'lspPartVar': 80.0}},
        {'name': 'MFX_FIREBALL_INIT', 'base': 'OWN_HAND_FLAME', 'replaces': True,
         'with': ['OWN_BALL_HAND_CORE', 'OWN_BALL_HAND_LICKS'],
         'set': {'visSizeStart_S': '20 26', **foot(20)}, 'lean': {'rise': 1.4, 'max_deg': 70, 'response': 0.1}},
        # flying: the bolt's one body on one axis, twice as thick, with a ball of tongues at its head
        {'name': 'OWN_BALL_HEAD', 'base': 'OWN_BOLT_HEAD',
         'set': {'ppsValue': 40.0, 'lspPartAvg': 120.0, 'lspPartVar': 25.0, 'visSizeStart_S': '30 30', 'shpDim_S': '3'}},
        {'name': 'OWN_BALL_FLAMES', 'base': 'OWN_HAND_LICKS', 'inherit': 0.9,
         'flipbook': {'cols': 8, 'rows': 8, 'frames': 64, 'fps': 30}, 'hdr': {'from': 1, 'to': 3, 'edge': 0.3},
         'set': {'ppsValue': 22.0, 'visAlphaStart': 170.0, 'shpType_S': 'SPHERE', 'shpIsVolume': 1, 'shpDim_S': '6', 'visName_S': 'OWN_FIREBALL.TGA',
                 'visSizeStart_S': '62 62', 'visSizeEndScale': 0.75, 'lspPartAvg': 200.0, 'lspPartVar': 40.0,
                 'velAvg': 0.0, 'velVar': 0.0, 'flyGravity_S': '0 0 0'}},
        {'name': 'OWN_BALL_LICKS', 'base': 'OWN_HAND_LICKS', 'inherit': 0.8,
         'set': {'ppsValue': 90.0, 'shpType_S': 'SPHERE', 'shpIsVolume': 0, 'shpDim_S': '16', 'visSizeStart_S': '20 28',
                 'visSizeEndScale': 0.6, 'lspPartAvg': 260.0, 'lspPartVar': 70.0, 'velAvg': 0.02, 'velVar': 0.01}},
        {'name': 'OWN_BALL_ROOT', 'base': 'OWN_BOLT_ROOT', 'set': {'visSizeStart_S': '70 90', **foot(90)}},
        {'name': 'OWN_BALL_EMBERS', 'base': 'OWN_BOLT_EMBERS',
         'set': {'ppsValue': 220.0, 'shpDim_S': '12', 'lspPartAvg': 1100.0, 'lspPartVar': 450.0}},
        haze('OWN_BALL_HEAT', 5.0, inherit=1.0, ppsValue=24.0, lspPartAvg=170.0, lspPartVar=40.0, shpDim_S='10',
             visSizeStart_S='120 120', visSizeEndScale=1.2),
        haze('OWN_BALL_WAKE', 8.0, inherit=0.1, ppsValue=45.0, lspPartAvg=520.0, lspPartVar=100.0, shpDim_S='10',
             visSizeStart_S='70 70', visSizeEndScale=2.0),
        {'name': 'MFX_IFB_PFXTRAIL', 'base': 'OWN_HAND_FLAME', 'replaces': True,
         'with': ['OWN_BALL_HEAD', 'OWN_BALL_FLAMES', 'OWN_BALL_LICKS', 'OWN_BALL_ROOT', 'OWN_BALL_EMBERS', 'OWN_BALL_HEAT', 'OWN_BALL_WAKE'],
         'set': {'visSizeStart_S': '80 170', **foot(170)},
         'lean': {'rise': 1.8, 'max_deg': 87, 'response': 0.03, 'along': 0.3, 'grow': 0.3, 'grow_from': 0.25}},
        # the game's second trail (clouds of fire along the way to the target) would stand beside ours as another kind
        # of fire: it becomes a thin second scatter of embers
        {'name': 'MFX_IFB_CAST', 'base': 'OWN_BOLT_EMBERS', 'replaces': True,
         'set': {'ppsValue': 40.0, 'shpDim_S': '14', 'lspPartAvg': 1300.0, 'lspPartVar': 500.0}},
        *blast('MFX_FIREBALL_COLLIDE1', 'OWN_BALL_HIT', 1.0),
    ]


def flight(name, tag, k) -> list:
    """The fire ball's flight (MFX_IFB_PFXTRAIL) k times its size."""
    return [
        {'name': f'{tag}_HEAD', 'base': 'OWN_BALL_HEAD', 'set': {'visSizeStart_S': sz(k, 30, 30), 'shpDim_S': sz(k, 3)}},
        {'name': f'{tag}_FLAMES', 'base': 'OWN_BALL_FLAMES', 'set': {'shpDim_S': sz(k, 6), 'visSizeStart_S': sz(k, 62, 62)}},
        {'name': f'{tag}_LICKS', 'base': 'OWN_BALL_LICKS', 'set': {'shpDim_S': sz(k, 16), 'visSizeStart_S': sz(k, 20, 28)}},
        {'name': f'{tag}_ROOT', 'base': 'OWN_BALL_ROOT', 'set': {'visSizeStart_S': sz(k, 70, 90), **foot(90 * k)}},
        {'name': f'{tag}_EMBERS', 'base': 'OWN_BALL_EMBERS', 'set': {'ppsValue': round(220.0 * k), 'shpDim_S': sz(k, 12)}},
        haze(f'{tag}_HEAT', 5.0, inherit=1.0, ppsValue=24.0, lspPartAvg=170.0, lspPartVar=40.0, shpDim_S=sz(k, 10),
             visSizeStart_S=sz(k, 120, 120), visSizeEndScale=1.2),
        haze(f'{tag}_WAKE', 8.0, inherit=0.1, ppsValue=45.0, lspPartAvg=520.0, lspPartVar=100.0, shpDim_S=sz(k, 10),
             visSizeStart_S=sz(k, 70, 70), visSizeEndScale=2.0),
        {'name': name, 'base': 'OWN_HAND_FLAME', 'replaces': True,
         'with': [f'{tag}_{part}' for part in ('HEAD', 'FLAMES', 'LICKS', 'ROOT', 'EMBERS', 'HEAT', 'WAKE')],
         'set': {'visSizeStart_S': sz(k, 80, 170), **foot(170 * k)},
         'lean': {'rise': 1.8, 'max_deg': 87, 'response': 0.03, 'along': 0.3, 'grow': 0.3, 'grow_from': 0.25}},
    ]


def spread(name, k) -> list:
    """The storm's splash at the hit (MFX_FIRESTORM_SPREAD*, storm.md): gobs of the ball's fire thrown out on arcs,
    each a ball of the looping fire that burns down with a streak of fire behind it. The engine's zone of the
    spread's damage is the emitter's sphere (CollisionZone), so the record keeps the game's 10 cm and its one key."""
    return [
        {'name': name, 'base': 'OWN_BALL_FLAMES', 'replaces': True, 'inherit': 0.0, 'drag': 0.9,
         'set': {**BURST, 'ppsValue': round(28.0 * k), 'shpType_S': 'SPHERE', 'shpIsVolume': 1, 'shpDim_S': '10',
                 'shpScaleKeys_S': '1', 'shpOffsetVec_S': '0 0 0', 'dirMode_S': 'DIR', 'dirFOR_S': 'WORLD',
                 'dirAngleHead': 0.0, 'dirAngleHeadVar': 179.0, 'dirAngleElev': 25.0, 'dirAngleElevVar': 60.0,
                 'velAvg': round(0.5 * k ** 0.5, 3), 'velVar': 0.15, 'flyGravity_S': '0 -0.0005 0',
                 'visSizeStart_S': sz(k, 36, 36), 'visSizeEndScale': 0.6, 'visAlphaStart': 230.0,
                 'visTexColorStart_S': '255 175 95', 'visTexColorEnd_S': '200 60 15',
                 'lspPartAvg': 1000.0, 'lspPartVar': 250.0,
                 'trlTexture_S': 'OWN_FIRE_TRAIL.TGA', 'trlWidth': round(13.0 * k, 2), 'trlFadeSpeed': 0.45}},
    ]


def firestorm() -> list:
    """The fire storm and pyrokinesis (FIRESTORM, PYROKINESIS; storm.md): the two spells share the ball in the hand,
    its four flashes of the charge and its flight; the hit is the charged ball's explosion at level 3
    (MFX_FIREBALL_COLLIDE3) and a splash of burning gobs, larger for pyrokinesis."""
    effects = charge_ball('MFX_FIRESTORM_INIT', 'OWN_FS_HAND', 1.1)
    for level, k in ((1, 1.0), (2, 1.2), (3, 1.45), (4, 1.7)):
        effects += [
            {'name': f'OWN_FS_STEP{level}_SPARKS', 'base': 'OWN_BOLT_HIT_SPARKS',
             'set': {'ppsValue': round(40.0 * k), 'velAvg': round(0.3 * k ** 0.5, 3), 'velVar': 0.15}},
            {'name': f'OWN_FS_STEP{level}_LICKS', 'base': 'MFX_FIREBOLT_COLLIDE',
             'set': {'ppsValue': round(20.0 * k), 'visSizeStart_S': sz(k, 13, 18), 'velAvg': round(0.25 * k ** 0.5, 3)}},
            {'name': f'MFX_FIRESTORM_INVESTBLAST{level}', 'base': 'OWN_BOLT_HIT_FLASH', 'replaces': True,
             'with': [f'OWN_FS_STEP{level}_SPARKS', f'OWN_FS_STEP{level}_LICKS'],
             'set': {'ppsValue': 3.0, 'visSizeStart_S': sz(k, 45, 45)}},
        ]
    effects += flight('MFX_FIRESTORM_CAST', 'OWN_FS_FLY', 1.3)
    effects += spread('MFX_FIRESTORM_SPREAD_SMALL', 1.0) + spread('MFX_FIRESTORM_SPREAD', 1.3)
    return effects


def firerain() -> list:
    """The fire rain (FIRERAIN; rain.md): burning gobs of the ball's fire fall from 10 m over a circle of 10 m around
    the caster, each with a streak of fire behind it; where one lands (particles.d "land") its fire splashes, throws
    sparks, burns on the ground for a second and a half and smokes. The engine's zone of the rain's damage is the
    emitter's circle (CollisionZone), so the record keeps the game's circle of 1000, its one key and its emission keys
    (80 a second for 2.5 s, then down over 4 s)."""
    up = {'dirMode_S': 'DIR', 'dirFOR_S': 'WORLD', 'dirAngleHead': 0.0, 'dirAngleHeadVar': 179.0, 'dirAngleElev': 40.0,
          'dirAngleElevVar': 45.0}
    return [
        # in the hand: the bolt's small flame on the palm, burning drops falling from it
        {'name': 'OWN_FR_HAND_DRIPS', 'base': 'OWN_BOLT_EMBERS', 'inherit': 0.3,
         'set': {'ppsValue': 18.0, 'shpDim_S': '3', 'dirMode_S': 'DIR', 'dirFOR_S': 'WORLD', 'dirAngleHead': 0.0,
                 'dirAngleHeadVar': 179.0, 'dirAngleElev': -90.0, 'dirAngleElevVar': 10.0, 'velAvg': 0.04,
                 'velVar': 0.02, 'flyGravity_S': '0 -0.00098 0', 'lspPartAvg': 500.0, 'lspPartVar': 150.0,
                 'visSizeStart_S': '1.2 1.2', 'visSizeEndScale': 0.6, 'visTexColorStart_S': '255 190 90',
                 'visTexColorEnd_S': '200 50 0'}},
        {'name': 'MFX_FIRERAIN_INIT', 'base': 'OWN_HAND_FLAME', 'replaces': True,
         'with': ['OWN_BOLT_HAND_LICKS', 'OWN_FR_HAND_DRIPS'],
         'set': {'visSizeStart_S': '14 19', **foot(19)}, 'lean': {'rise': 1.4, 'max_deg': 70, 'response': 0.1}},
        # where a gob lands: its fire splashes (tongues stopped by the air), a short flash, sparks thrown up, the ground
        # burns for a second and a half and goes down, a puff of smoke rises
        {'name': 'OWN_FR_LAND_FLASH', 'base': 'OWN_BOLT_HIT_FLASH',
         'set': {'ppsValue': 2.0, 'visSizeStart_S': '45 45', 'lspPartAvg': 140.0, 'lspPartVar': 30.0}},
        {'name': 'OWN_FR_LAND_SPARKS', 'base': 'OWN_BOLT_HIT_SPARKS',
         'set': {'ppsValue': 14.0, 'velAvg': 0.3, 'velVar': 0.15, 'lspPartAvg': 600.0, 'lspPartVar': 250.0, **up}},
        {'name': 'OWN_FR_LAND_FIRE', 'base': 'OWN_HAND_LICKS', 'inherit': 0.0,
         'set': {'ppsValue': 26.0, 'ppsScaleKeys_S': '1 1 0.9 0.7 0.5 0.3', 'ppsFPS': 4.0, 'ppsIsLooping': 0,
                 'ppsIsSmooth': 1, 'shpType_S': 'CIRCLE', 'shpFOR_S': 'WORLD', 'shpIsVolume': 1, 'shpDim_S': '18',
                 'shpOffsetVec_S': '0 8 0', 'visSizeStart_S': '26 40', 'visSizeEndScale': 0.6, 'lspPartAvg': 380.0,
                 'lspPartVar': 100.0, 'velAvg': 0.03, 'velVar': 0.01}},
        {'name': 'OWN_FR_LAND_SMOKE', 'base': 'OWN_BOLT_HIT_PUFF',
         'set': {'ppsValue': 1.0, 'shpDim_S': '6', 'shpOffsetVec_S': '0 25 0', 'visSizeStart_S': '30 30',
                 'visSizeEndScale': 3.0, 'visAlphaStart': 55.0, 'lspPartAvg': 2200.0, 'lspPartVar': 400.0}},
        {'name': 'OWN_FR_LAND', 'base': 'OWN_HAND_LICKS', 'inherit': 0.0, 'drag': 4.0,
         'with': ['OWN_FR_LAND_FLASH', 'OWN_FR_LAND_SPARKS', 'OWN_FR_LAND_FIRE', 'OWN_FR_LAND_SMOKE'],
         'set': {**BURST, 'ppsValue': 10.0, 'shpType_S': 'SPHERE', 'shpIsVolume': 1, 'shpDim_S': '5', 'velAvg': 0.25,
                 'velVar': 0.1, 'flyGravity_S': '0 8e-05 0', 'visSizeStart_S': '16 22', 'visSizeEndScale': 1.4,
                 'lspPartAvg': 280.0, 'lspPartVar': 80.0, **up}},
        # the gobs: the looping fire of the ball, 42 cm, thrown down at 3 m/s from 10 m and falling freely (they land
        # at about 15 m/s after 1.25 s), each with a streak of fire about 5 m long that draws into the place it landed
        # (at 30 cm and 3 m the gobs read as white sparks from 18 m: the game's streaks are 7 m)
        {'name': 'MFX_FIRERAIN_RAIN', 'base': 'OWN_BALL_FLAMES', 'replaces': True, 'inherit': 0.0, 'land': 'OWN_FR_LAND',
         'set': {'ppsValue': 80.0, 'ppsScaleKeys_S': '1 1 1 1 1 0.8 0.7 0.6 0.5 0.4 0.3 0.2 0.1', 'ppsFPS': 2.0,
                 'ppsIsLooping': 0, 'ppsIsSmooth': 0, 'useEmittersFOR': 0,
                 'shpType_S': 'CIRCLE', 'shpFOR_S': 'WORLD', 'shpIsVolume': 1, 'shpDim_S': '1000', 'shpScaleKeys_S': '1',
                 'shpOffsetVec_S': '0 1000 0', 'dirMode_S': 'DIR', 'dirFOR_S': 'WORLD', 'dirAngleHead': 10.0,
                 'dirAngleHeadVar': 20.0, 'dirAngleElev': -84.0, 'dirAngleElevVar': 5.0, 'velAvg': 0.3,
                 'velVar': 0.15, 'flyGravity_S': '0 -0.00098 0', 'visSizeStart_S': '42 42', 'visSizeEndScale': 0.85,
                 'visAlphaStart': 230.0, 'visTexColorStart_S': '255 165 85', 'visTexColorEnd_S': '255 105 30',
                 'lspPartAvg': 2500.0, 'lspPartVar': 300.0,
                 'trlTexture_S': 'OWN_FIRE_TRAIL.TGA', 'trlWidth': 24.0, 'trlFadeSpeed': 0.35}},
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
        # the wind's share is small: at the torch's 0.5 a breeze blew every tongue off to one side of the body
        {'name': 'OWN_BODY_TALL', 'base': 'OWN_HAND_LICKS', 'inherit': 0.85, 'wind': 0.2,
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
        # the one effect on the root keeps the game's smoke and brings the body's light: the game's own is a point a
        # metre wide at the belt that hardly reaches the ground. The range follows the fire's stages, with a flicker, and
        # ends at 0: the keys play once, the smoke outlives the fire
        {'name': 'MFX_MAGICFIRE_HUMANSMOKE', 'base': 'MFX_MAGICFIRE_HUMANSMOKE', 'replaces': True, 'set': {},
         'light': {'range': 450, 'color': [255, 150, 60], 'fps': 6.0,
                   'keys': [0.4, 0.8, 1, 0.94, 1.05, 0.97, 1.03, 0.93, 1, 1.06, 0.95, 1.02, 0.96, 1.04, 0.93, 1, 1.05,
                            0.96, 1.02, 0.94, 1, 0.97, 1.03, 0.95, 0.8, 0.55, 0.3, 0.1, 0]}},
        # the short tongues on the clothes: born on the body's mesh, they rise and go out. Each of the seven emitters
        # brings a seventh of the smoke, the embers and the hot air, over its own bone
        {'name': 'MFX_MAGICFIRE_HUMANBURN', 'base': 'OWN_HAND_LICKS', 'replaces': True, 'inherit': 0.92, 'wind': 0.1,
         'with': ['OWN_BODY_TALL', 'OWN_BODY_SMOKE', 'OWN_BODY_EMBERS', 'OWN_BODY_HEAT'],
         'set': {**burn, 'ppsValue': 30.0, 'shpType_S': 'MESH', 'shpIsVolume': 0, 'shpDim_S': '0 0 0', 'shpOffsetVec_S': '0 0 0',
                 'visSizeStart_S': '24 40', 'visSizeEndScale': 0.7, 'lspPartAvg': 380.0, 'lspPartVar': 100.0,
                 'velAvg': 0.02, 'velVar': 0.01}},
    ]


def cmd_build(_args) -> int:
    PARTICLES.parent.mkdir(parents=True, exist_ok=True)
    effects = firebolt() + fireball() + charged_fireball() + firestorm() + firerain() + body_fire()
    PARTICLES.write_text(json.dumps({'format': 1, 'effects': effects}, indent=1) + '\n')
    print(f'{PARTICLES.relative_to(ROOT)}: {len(effects)} effects')
    return 0


def cmd_pack(args) -> int:
    output = STAGING / args.set
    if output.exists():
        sys.exit(f'{output}: the staging set exists; choose a new name')
    missing = [name for _, name in ATLASES.values() if not (args.atlases / name).is_file()]
    if missing:
        sys.exit(f'{args.atlases}: no {", ".join(missing)} (explosion_atlas.py)')
    target = output / PARTICLES.relative_to(CONTENT)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(PARTICLES, target)
    index = {}
    for texture, (rel, name) in ATLASES.items():
        (output / 'materials' / rel).parent.mkdir(parents=True)
        shutil.copyfile(args.atlases / name, output / 'materials' / rel)
        index[texture] = {'albedo': rel}
    (output / 'materials/materials.json').write_text(json.dumps(index, indent=2) + '\n')
    print(f'{output}: the spells\' fire as a part; it needs the effects of torch-model-* and fire-worlds-* and smoke-*')
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('build').set_defaults(func=cmd_build)
    pack = sub.add_parser('pack')
    pack.add_argument('--set', required=True)
    pack.add_argument('--atlases', type=Path, required=True, help='the folder of explosion_atlas.py outputs')
    pack.set_defaults(func=cmd_pack)
    args = parser.parse_args()
    return args.func(args)


if __name__ == '__main__':
    raise SystemExit(main())
