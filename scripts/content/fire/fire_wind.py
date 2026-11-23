#!/usr/bin/env python3
"""The wind for the own fire's effects: every flame card leans, every tongue, spark, smoke and haze is carried.

    python3 scripts/content/fire/fire_wind.py apply FILE...      # particles.d files kept as JSON in git

reports/content/remaster/fire-worlds/moving-flame.md, steps 4 and 5. The part scripts call `with_wind` on their
effects. The engine gives the world's wind at the emitter (none under a roof, a third behind a wall); `wind` is the
share of it an effect feels. A flame takes half: at the world's full wind a street torch would lie at 60 degrees.
"""
from __future__ import annotations

import json
from pathlib import Path
import sys

FLAME_SHARE = 0.5
# smoke at the world's full wind is torn off the flame and thinned to nothing within a few metres (round 2): everything
# over the fire takes the flame's share and leaves along the flame's own lean
SHARE = {'LICKS': FLAME_SHARE, 'SPARKS': FLAME_SHARE, 'SMOKE': FLAME_SHARE, 'SOOT': FLAME_SHARE, 'HAZE': FLAME_SHARE}
# the flame gas's rise, m/s: the taller the flame the faster, so a campfire leans less than a torch in the same wind
RISE = {'OWN_TORCH': 1.8, 'OWN_BRAZIER': 2.2, 'OWN_HEARTH': 2.5, 'OWN_CAMP': 2.8}
LEAN = {'max_deg': 70, 'response': 0.12}
# cm from the fire's base to the emitters over it (the part scripts' layers): in moving air their particles are born
# that far along the leaning flame, so smoke, sparks and haze leave the flame and not the air beside it
PLUME = {'OWN_CAMP': {'SPARKS': 50, 'SMOKE': 85, 'HAZE': 55},
         'OWN_BRAZIER': {'SPARKS': 25, 'SMOKE': 42, 'HAZE': 36},
         'OWN_TORCH': {'SPARKS': 5, 'SOOT': 24, 'HAZE': 20}}
# smoke that leaves with the wind is spread along a longer way and no longer pools over the fire: it is that much
# denser at birth (round 3: the campfire's smoke was lost against the night)
SMOKE_DENSER = 1.7
LICK_DRAG = 2.0  # 1/s: a tongue takes the wind in half a second; an upward pull keeps its rise at velAvg


def with_wind(data: dict) -> dict:
    for effect in data['effects']:
        name = effect['name']
        family = next((key for key in RISE if name.startswith(key + '_')), None)
        if family is None:
            continue
        kind = name[len(family) + 1:].split('_')[0]
        steady = effect.get('flipbook', {}).get('steady', False)
        if kind == 'FLAME' and steady:
            effect['lean'] = {'rise': RISE[family], **LEAN}
            effect['wind'] = FLAME_SHARE
        elif kind in SHARE:
            if kind == 'LICKS':
                rise = float(effect['set']['velAvg'])  # cm/ms
                effect['drag'] = LICK_DRAG
                effect['set']['flyGravity_S'] = f'0 {rise * LICK_DRAG * 1e-3:g} 0'
            if 'drag' in effect:
                effect['wind'] = SHARE[kind]
                if kind == 'SMOKE' and kind in PLUME.get(family, {}):  # not the hearth's: a room has no wind
                    effect['set']['visAlphaStart'] = round(min(255.0, effect['set']['visAlphaStart'] * SMOKE_DENSER), 1)
                height = PLUME.get(family, {}).get(kind)
                if height:
                    effect['plume'] = {'height': height, 'rise': RISE[family], 'wind': FLAME_SHARE}
    return data


def main() -> int:
    if len(sys.argv) < 3 or sys.argv[1] != 'apply':
        sys.exit(__doc__)
    for name in sys.argv[2:]:
        path = Path(name)
        data = with_wind(json.loads(path.read_text()))
        path.write_text(json.dumps(data) + '\n')
        print(f'{path}: {sum("wind" in e for e in data["effects"])} of {len(data["effects"])} effects feel the wind')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
