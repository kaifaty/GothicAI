#!/usr/bin/env python3
"""The heat haze of the wall torches as a part of the working content.

    python3 scripts/content/fire/torch_haze_set.py build
    python3 scripts/content/fire/torch_haze_set.py pack --set torch-haze-1

reports/render-features/2026-11-16-heat-haze. The torches' fire is the part torch-model-* (wall_torch_set.py), whose
pack needs the model's bake; the haze is a part of its own beside it: one effect and a layer that adds a haze emitter
to every torch, 4 cm under the point where its soot starts (the flame's upper part). `build` reads the torch layers
of reports/content/remaster/wall-torch-model/content and writes this part's content next to them; the set needs
OWN_SMOKE_WISPY.TGA of smoke-*.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import sys

from campfire_set import ROOT, SMOKE, STAGING, haze, write_layer

TORCH = ROOT / 'reports/content/remaster/wall-torch-model/content/worlds/NEWWORLD/vobs.d'
CONTENT = ROOT / 'reports/content/remaster/wall-torch-model/haze/content'
PARTICLES = CONTENT / 'particles.d/torch-haze.json'
LAYER = CONTENT / 'worlds/NEWWORLD/vobs.d/torch-haze.json'
DOWN = 4.0  # cm under the soot's start
# a torch's head is 14 cm thick: a third of the campfire's sprite, a smaller shift and cell, gone 0.9 m over the flame
EFFECT = haze('OWN_TORCH_HAZE', 30, 5, 1200, shift=2.0, cell=4.0)


def cmd_build(_args) -> int:
    add = []
    for path in sorted(TORCH.glob('wall-torch-*.json')):
        for entry in json.loads(path.read_text()).get('add', []):
            if entry.get('pfx') == 'OWN_TORCH_SOOT':
                x, y, z = entry['position']
                add.append({'key': entry['key'].removesuffix('-soot') + '-haze', 'kind': 'emitter', 'pfx': EFFECT['name'],
                            'position': [x, round(y - DOWN, 1), z], 'yaw_deg': 0})
    if not add:
        sys.exit(f'{TORCH}: no torch layers')
    PARTICLES.parent.mkdir(parents=True, exist_ok=True)
    PARTICLES.write_text(json.dumps({'format': 1, 'effects': [EFFECT]}) + '\n')
    write_layer(LAYER, {'format': 1, 'remove': [], 'light': [], 'add': add})
    print(f'{LAYER.relative_to(ROOT)}: {len(add)} torches')
    return 0


def cmd_pack(args) -> int:
    output = STAGING / args.set
    if output.exists():
        sys.exit(f'{output}: the staging set exists; choose a new name')
    for source in (PARTICLES, LAYER):
        target = output / source.relative_to(CONTENT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    print(f'{output}: the torches\' haze as a part; it needs {SMOKE} of smoke-* and the torches of torch-model-*')
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
