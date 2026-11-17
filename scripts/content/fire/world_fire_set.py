#!/usr/bin/env python3
"""The own fire in a world other than the main one, as a part of the working content.

    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/fire/world_fire_set.py build --world OLDWORLD [--base DIR]
    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/fire/world_fire_set.py pack --set fire-oldworld-1

reports/content/remaster/fire-worlds. No new effect and no new model: the layers of a world put the fire that the
main world's parts already hold (torch-model-*, torch-haze-*, campfire-*, brazier-*) on the same kinds of places.
- wall torches (OW_MISC_WALL_TORCH_01): the own model and fire of wall_torch_set.py;
- campfires (NW_MISC_FIREPLACE_01 with FIRE_HOT): campfire_set.py;
- every other burning effect, by the original's own choice of its size: TORCH.PFX gets the torch's fire, FIRE.PFX and
  FIRE_MEDIUM.PFX the brazier's; a lamp that is the effect's parent takes the flame's colour and flicker, a fire
  without one gets a light. A fire the layer cannot remove (a zCPFXController, an effect with a lamp under it: saved
  state hangs on them) burns the set's small tongues in place (the layer's "effect"). A forge keeps the original's
  fire: it is a part of its model's work;
- the smoke that is left: the brazier's thin smoke;
- the vob tree FIRETREE_MEDIUM of the tall pole fires (FIREPLACE_HIGH, FIREPLACE_HIGH2): the brazier's fire.
FIRETREE_LARGE.ZEN is not in the game's archives: a MOB that names it burns only what the level put beside it.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import shutil
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'mesh3d'))
import wall_torch_set as torch  # noqa: E402  (it brings mesh3d's own hearth_set; the fire's hearth_set is not used here)

sys.path.insert(0, str(Path(__file__).resolve().parent))
import campfire_set as camp  # noqa: E402
import rest_set as rest  # noqa: E402

ROOT = camp.ROOT
CONTENT = ROOT / 'reports/content/remaster/fire-worlds/content'
TREE = CONTENT / 'vobtrees.d/FIRETREE_MEDIUM.json'
KIT = {'TORCH.PFX': 'torch', 'FIRE.PFX': 'brazier', 'FIRE_MEDIUM.PFX': 'brazier'}
FORGE = 'BSFIRE'          # the forge's MOB: BSFIRE_OC.MDS
FORGE_REACH = 150.0
NEAR_LAMP = 150.0         # a lamp of the level this close already lights the fire's place: the own light stays short
# Where the fuel's top is against the original effect's origin, cm up, by the frames of round 1: TORCH.PFX starts on
# the holder's top; on the tower's chandelier it hangs over the bowls, and a flame at its origin sank into them;
# FIRE.PFX starts inside the orc lamp's bowl, FIRE_MEDIUM over the coals (rest_set.py)
FUEL_UP = {'TORCH.PFX': 0.0, 'FIRE.PFX': 6.0, 'FIRE_MEDIUM.PFX': -rest.COALS_UNDER_FIRE}
CHANDELIER, CHANDELIER_REACH, CHANDELIER_UP = 'DT_LIGHTER_PENTAGRAMM.3DS', 160.0, 10.0
TORCH_LIGHT_UP = 22.0     # wall_torch_set.py
RANGE = {'torch': (400, 250), 'brazier': (700, 700)}  # the added light: alone, beside a lamp of the level
# FIRETREE_MEDIUM: its root lamp is the slot, FIRE_MEDIUM stands 16 cm over it
TREE_FIRE_UP = 16.0


def layer_dir(world) -> Path:
    return CONTENT / f'worlds/{world}/vobs.d'


def medium_tree() -> dict:
    coals = np.array([0.0, TREE_FIRE_UP - rest.COALS_UNDER_FIRE, 0.0])
    add = rest.brazier_fire('pole', coals, 7, 71, True)
    add.append({'key': 'pole-light', 'kind': 'light', 'position': camp.at(coals + [0, rest.LIGHT_UP, 0]), 'yaw_deg': 0,
                'range': 700, 'color': rest.LAMP['color'], 'near': rest.LAMP['near'], 'flicker_from': 'pole-flame', **rest.SWAY})
    return {'format': 1, 'add': add}


def rest_layer(everything, taken) -> tuple[dict, dict]:
    name = lambda vob: str(vob.get('visual_name', '')).upper()
    forges = [vob for vob, _ in everything if name(vob).startswith(FORGE)]
    lamps = [vob for vob, _ in everything if vob['type'] == 'zCVobLight']
    chandeliers = [vob for vob, _ in everything if name(vob) == CHANDELIER]
    out = {'format': 1, 'effect': [], 'remove': [], 'light': [], 'add': []}
    count = {'torch': 0, 'brazier': 0, 'held': 0, 'forge': 0, 'smoke': 0}
    for vob, parent in everything:
        if name(vob) not in KIT or vob['id'] in taken or (parent and name(parent) == torch.TORCH_MESH):
            continue
        spot = np.array(vob['position'], float)
        if any(math.dist(spot, f['position']) < FORGE_REACH for f in forges):
            count['forge'] += 1
            continue
        kind = KIT[name(vob)]
        key = f'{kind}-{vob["id"]}'
        siblings = parent.get('children', []) if parent and parent['type'] == 'zCVobLight' else []
        smoke = [c for c in siblings + vob.get('children', []) if name(c) == 'FIRE_SMOKE.PFX']
        out['remove'] += [c['id'] for c in smoke]  # a removed vob leaves its children in place
        taken |= {c['id'] for c in smoke}
        up = FUEL_UP[name(vob)]
        if kind == 'torch' and any(math.dist(spot, c['position']) < CHANDELIER_REACH for c in chandeliers):
            up += CHANDELIER_UP
        if kind == 'torch':
            wick = spot + [0, up, 0]
            entries = rest.torch_fire(key, wick, 29 + vob['id'] * 3)
            light, near, sway = wick + [0, TORCH_LIGHT_UP, 0], torch.LAMP['near'], torch.SWAY
        else:
            coals = spot + [0, up, 0]
            entries = rest.brazier_fire(key, coals, vob['id'], 53 + vob['id'] * 3, bool(smoke))
            light, near, sway = coals + [0, rest.LIGHT_UP, 0], rest.LAMP['near'], rest.SWAY
        # a trigger and an effect with a lamp under it hold saved state: the layer cannot remove them, so the vob
        # itself burns the set's small tongues in place and the set comes without them
        held = vob['type'] != 'zCVob' or any(c['id'] not in taken for c in vob.get('children', []))
        if held:
            licks = next(e for e in entries if e['key'].endswith('-licks'))
            entries.remove(licks)
            out['effect'].append({'id': vob['id'], 'pfx': licks['pfx']})
            count['held'] += 1
        else:
            out['remove'].append(vob['id'])
        out['add'] += entries
        signal = {'flicker_from': f'{key}-flame', **sway}
        own = [parent] if parent and parent['type'] == 'zCVobLight' else []
        own = [c for c in own + vob.get('children', []) if c['type'] == 'zCVobLight' and not c['is_static']]
        own = [(camp.at(light - np.array(c['position'])), c) for c in own]
        own = [(offset, c) for offset, c in own if max(abs(v) for v in offset) <= 100]
        if own:
            offset, lamp = own[0]
            out['light'].append({'id': lamp['id'], 'color': rest.LAMP['color'], 'near': near, **signal, 'offset_cm': offset})
        else:
            lit = any(math.dist(spot, lamp['position']) < NEAR_LAMP for lamp in lamps)
            out['add'].append({'key': f'{key}-light', 'kind': 'light', 'position': camp.at(light), 'yaw_deg': 0,
                               'range': RANGE[kind][lit], 'color': rest.LAMP['color'], 'near': near, **signal})
        count[kind] += 1
    for vob, _parent in everything:
        if name(vob) == 'FIRE_SMOKE.PFX' and vob['id'] not in taken:
            out['remove'].append(vob['id'])
            out['add'].append({'key': f'smoke-{vob["id"]}', 'kind': 'emitter', 'pfx': 'OWN_BRAZIER_SMOKE',
                               'position': camp.at(vob['position']), 'yaw_deg': 0})
            count['smoke'] += 1
    return out, count


def write_layer(path, data) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    row = lambda entries: [json.dumps(e) + (',' if i + 1 < len(entries) else '') for i, e in enumerate(entries)]
    block = lambda field: [f' "{field}": [', *('  ' + r for r in row(data[field])), ' ]']
    lines = ['{', ' "format": 1,', *block('effect')[:-1], ' ],', f' "remove": {json.dumps(data["remove"])},',
             *block('light')[:-1], ' ],', *block('add'), '}']
    path.write_text('\n'.join(lines) + '\n')


def cmd_build(args) -> int:
    world = args.world.upper()
    vobs_json = camp.base_dir(args.base) / f'worlds/{world}/vobs.json'
    out = layer_dir(world)
    place = world.lower()

    torch.PLACES[place] = dict(range=250)
    torches = torch.base_torches(vobs_json, torch.EVERYWHERE)
    for one in filter(torch.bound, torches):
        print(f'torch {one["id"]} at {camp.at(one["position"])} stays the original: its fire holds other vobs')
    torches = [t for t in torches if not torch.bound(t) and any(str(c.get('visual_name', '')).upper() in torch.GONE for c in t.get('children', []))]
    walls = torch.place_layer(place, torches)
    torch.write_layer(out / f'wall-torch-{place}.json', walls)
    added = sum(e['kind'] == 'light' for e in walls['add'])
    print(f'wall torches: {len(torches)}, {len(walls["remove"])} vobs go, {len(walls["light"])} lamps move into the flame, {added} lights added')

    fires, left = camp.layer(camp.campfires(vobs_json))
    camp.write_layer(out / 'campfire.json', fires)
    print(f'campfires: {sum(e["key"].endswith("-flame") for e in fires["add"])}, {len(fires["remove"])} removes, {len(left)} cold')

    everything = []

    def walk(vob, parent):
        everything.append((vob, parent))
        for child in vob.get('children', []):
            walk(child, vob)

    for vob in json.loads(vobs_json.read_text())['vobs']:
        walk(vob, None)
    taken = set(walls['remove']) | set(fires['remove'])
    data, count = rest_layer(everything, taken)
    write_layer(out / 'fire-rest.json', data)
    lights = sum(e['kind'] == 'light' for e in data['add'])
    print(f'the rest: {count}, {len(data["remove"])} removes, {len(data["light"])} lamps of the base, {lights} lights added')

    TREE.parent.mkdir(parents=True, exist_ok=True)
    TREE.write_text(json.dumps(medium_tree(), indent=1) + '\n')
    print(f'{TREE.relative_to(ROOT)}: {len(medium_tree()["add"])} entries')
    return 0


def cmd_pack(args) -> int:
    output = camp.STAGING / args.set
    if output.exists():
        sys.exit(f'{output}: the staging set exists; choose a new name')
    for source in sorted(p for p in CONTENT.rglob('*.json')):
        target = output / source.relative_to(CONTENT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    print(f'{output}: the worlds\' fire as a part; it needs the effects of torch-model-*, torch-haze-*, campfire-* and brazier-*')
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    one = sub.add_parser('build')
    one.add_argument('--world', required=True)
    one.add_argument('--base')
    one.set_defaults(func=cmd_build)
    pack = sub.add_parser('pack')
    pack.add_argument('--set', required=True)
    pack.set_defaults(func=cmd_pack)
    args = parser.parse_args()
    return args.func(args)


if __name__ == '__main__':
    raise SystemExit(main())
