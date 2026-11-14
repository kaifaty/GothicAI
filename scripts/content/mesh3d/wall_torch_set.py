#!/usr/bin/env python3
"""Build, bake and pack the wall torch (asset wall-torch-model): its own model with the glowing wrap and its fire.

    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/wall_torch_set.py tiles
    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/wall_torch_set.py model
    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/wall_torch_set.py bake
    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/wall_torch_set.py layer
    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/wall_torch_set.py places --base BASE
    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/wall_torch_set.py pack --set torch-model-1

Run from the repository root. reports/content/remaster/wall-torch-model: intent.md, concept.md, research.md.
- tiles: the surface tiles of wall-torch-tiles.json (market_tiles.py) and the wrap's glow mask (torch_tiles.py);
- model: blender/wall_torch.py with wall-torch.json;
- bake: blender/bake_atlas.py with wall-torch-atlas.json, on this machine (one prop, 1024 px, about three minutes);
- layer: the vob layer of the trial torch at the Red Lantern from the model's numbers: the base mesh vob shows the
  own model in place (vobs.d "visual": the torch's lamp hangs under that vob, so it cannot be removed), the
  original's flame effect and flare go, the fire's emitters sit on the wrap, the base lamp's light moves to the
  flame (vobs.d "offset_cm"). Written to reports/content/remaster/wall-torch-model/content/, in git;
- places: the same for every wall torch of a place (PLACES) from the base's vobs.json, one layer file a place:
  each torch's flame, sparks and flare go, its own lamp (the child lamp that is not static) moves into the flame
  and follows it; a torch without such a lamp gets a light in the flame. Written next to the trial's layer, in git;
- pack: a staging set, a part of the working content (docs/remaster-pipeline.md, sets): the model as
  OWN_WALL_TORCH_01, its atlas as three materials (the wrap's with "emissive"), the layers, the effects and the
  flame atlas OWN_TORCH_FLAME.TGA. It takes the place of the part torch-fire-*: the working set is composed from
  the set before that part, `building_set.py compose working-N <the set without torch-fire> NAME`. The soot's
  OWN_HUT_SMOKE.TGA is the harbour's (a `motion-*` part), as for torch-fire-*.
Outputs: ~/.local/share/gothic-ai/content-staging/wall-torch-model/{tiles,build/torch}.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hearth_set as hearth  # noqa: E402
import rigging_set as rig  # noqa: E402

ROOT = rig.ROOT
BLENDER_DIR = rig.BLENDER_DIR
STAGING = rig.STAGING
BLENDER = rig.BLENDER
TILES = STAGING / 'wall-torch-model/tiles'
BUILD = STAGING / 'wall-torch-model/build/torch'
CONTENT = ROOT / 'reports/content/remaster/wall-torch-model/content'
LAYER = CONTENT / 'worlds/NEWWORLD/vobs.d/wall-torch-fire.json'
PARTICLES = CONTENT / 'particles.d/wall-torch-fire.json'
REGISTRY = ROOT / 'data/content/prop-models.jsonl'
SCRIPT, CFG, ATLAS, TILE_SPEC = 'wall_torch.py', 'wall-torch.json', 'wall-torch-atlas.json', 'wall-torch-tiles.json'
VISUAL, NAME = 'OWN_WALL_TORCH_01', 'wall-torch'
CONCEPT = 'wall-torch-model/concept-attempt-8.png (variant B, the wrap; the form is the original mesh)'
FLAME = ('OWN_TORCH_FLAME.TGA', 'own-torch-flame/albedo.dds')
FLAME_SOURCE = Path.home() / '.local/share/gothic-ai/content/materials' / FLAME[1]

# The trial torch at the Red Lantern, NEWWORLD (vobs.json of the base): the mesh's pose, what goes with it and
# the lamp that stays where it is
POSE = dict(position=[420.322968, 94.2908325, -3086.30713],
            rotation=[[-0.159367204, 0.123762719, -0.979433656], [-0.0478692688, 0.989975274, 0.132883757],
                      [0.986060917, 0.0680620596, -0.15184553]])
MESH_ID, EFFECT_ID, FLARE_ID, LAMP_ID = 20157, 20158, 20161, 20160
LAMP_AT = np.array([401.884857, 125.647934, -3086.54468])
CARD, SIDE_CARD = (34.0, 44.0), (21.0, 32.0)  # the flame cards of OWN_TORCH_FLAME and _SIDE, cm (particles.d)
TORCH_MESH = 'OW_MISC_WALL_TORCH_01.3DS'
GONE = ('FIRE_MEDIUM.PFX', 'FIRE_SPARKS.PFX', 'ZFLARE6.TGA')  # the original's flame, sparks and flare under a torch
LAMP = {'color': [230, 84, 15], 'near': 8, 'jitter_cm': 1.5}  # the torch's light: the flame's colour (wall-torch)
# Other places of the torch: every OW_MISC_WALL_TORCH_01 of NEWWORLD inside the box (x and z, world cm). `range` is
# the light of a torch that has no lamp of its own, the common range of the place's torch lamps.
PLACES = {'xardas': dict(box=((29000, 31600), (-16300, -14100)), count=23, range=200)}


def cmd_tiles(_args) -> int:
    rig.run([sys.executable, Path(__file__).with_name('market_tiles.py'), '--spec', BLENDER_DIR / TILE_SPEC], cwd=ROOT)
    rig.run([sys.executable, Path(__file__).with_name('torch_tiles.py'), TILES], cwd=ROOT)
    return 0


def cmd_model(_args) -> int:
    rig.run([BLENDER, '-b', '--factory-startup', '-P', BLENDER_DIR / SCRIPT, '--', BLENDER_DIR / CFG, BUILD], cwd=ROOT,
            stdout=subprocess.DEVNULL)
    metrics = json.loads((BUILD / 'metrics.json').read_text())
    print(json.dumps(metrics)[:400])
    if metrics.get('inside_out_parts') or metrics.get('open_edges'):
        sys.exit('the model has closed parts inside out or open edges: the engine would cull or show through them')
    if metrics['triangles'] > metrics['budget']:
        sys.exit(f"{metrics['triangles']} triangles over the budget {metrics['budget']}")
    return 0


def cmd_bake(_args) -> int:
    rig.run([BLENDER, '-b', '--factory-startup', '-P', BLENDER_DIR / 'bake_atlas.py', '--', BLENDER_DIR / ATLAS, BUILD / 'atlas'],
            cwd=ROOT, stdout=subprocess.DEVNULL)
    print((BUILD / 'atlas/metrics.json').read_text()[:400])
    return 0


def world(point_m, pose=POSE) -> np.ndarray:
    """A point of the model, glTF metres (x, y up, z from the wall), in the world's cm at a torch's pose."""
    local = np.array([-point_m[0], point_m[1], point_m[2]]) * 100  # the loader mirrors X and turns metres into cm
    return np.array(pose['rotation']) @ local + np.array(pose['position'])


def at(point) -> list:
    return [round(float(v), 1) for v in point]


def fire(pose) -> dict:
    """Where the fire of the torch at a pose sits, world cm: the emitters (key's end, effect, point), the light."""
    cfg = json.loads((BLENDER_DIR / CFG).read_text())
    metrics = json.loads((BUILD / 'metrics.json').read_text())
    butt, tip = np.array(cfg['butt']), np.array(cfg['tip'])
    axis = np.array(pose['rotation']) @ ((tip - butt) / np.linalg.norm(tip - butt))  # the club's axis in the world
    lean = np.array([axis[0], 0.0, axis[2]]) / np.hypot(axis[0], axis[2])           # the way its top leans
    top = world(metrics['wrap_top_gltf_m'], pose)
    up = np.array([0.0, 1.0, 0.0])
    wrap_cm = (cfg['wrap']['profile'][-1][0] - metrics['wrap_base_t_m']) * 100
    radius_cm = max(r for _, r in cfg['wrap']['profile']) * 100
    # the flame card's lower edge 8 cm under the wrap's top, so the flame's body starts on the wrap's sides;
    # small tongues are born on a sphere round its upper part; sparks leave its top; burning drops fall from the
    # torn lower edge on the side that faces down; the soot starts where the flame ends
    flame = top - axis * 4 + up * (CARD[1] / 2 - 4)
    # two smaller flames on the top's rim, a third of a turn apart, widen the fire's base to the wrap's width
    across = np.cross(up, lean)
    rim = [top - axis * 2 + (lean * math.cos(a) + across * math.sin(a)) * radius_cm * 0.6 + up * (SIDE_CARD[1] / 2 - 6)
           for a in (math.radians(100), math.radians(-100))]
    points = [('flame', 'OWN_TORCH_FLAME', flame),
                ('flame-a', 'OWN_TORCH_FLAME_SIDE', rim[0]),
                ('flame-b', 'OWN_TORCH_FLAME_SIDE', rim[1]),
                ('licks', 'OWN_TORCH_LICKS', top - axis * 8),
                ('sparks', 'OWN_TORCH_SPARKS', top + up * 5),
                ('drops', 'OWN_TORCH_DROPS', top - axis * wrap_cm + lean * radius_cm),
                ('soot', 'OWN_TORCH_SOOT', top + up * 24)]
    return dict(emitters=points, light=top + up * 10 + lean, top=top, lean_deg=float(np.degrees(np.arccos(axis[1]))))


def emitters(key, points, seed) -> list:
    """The layer's `add` entries of one torch's fire; the three flames flicker, each from its own seed."""
    entries = []
    for end, pfx, position in points:
        entry = {'key': f'{key}-{end}', 'kind': 'emitter', 'pfx': pfx, 'position': at(position), 'yaw_deg': 0}
        if 'flame' in end:
            # a head 14 cm thick puffs at about 1.5 / sqrt(0.14) Hz, slower than the bare club's end (research.md);
            # the side flames keep their own time
            entry['flicker'] = {'pulse_hz': 4.5, 'jitter_hz': 12, 'amount': 0.2, 'seed': seed + len(entries)}
        entries.append(entry)
    return entries


def write_layer(path, layer) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    row = lambda entries: [json.dumps(e) + (',' if i + 1 < len(entries) else '') for i, e in enumerate(entries)]
    if len(layer['visual']) == 1:  # the trial's file keeps its form
        lines = ['{', ' "format": 1,', f' "visual": {json.dumps(layer["visual"])},', f' "remove": {json.dumps(layer["remove"])},',
                 ' "light": [' + ', '.join(json.dumps(e) for e in layer['light']) + '],', ' "add": [']
    else:
        lines = ['{', ' "format": 1,', ' "visual": [', *('  ' + r for r in row(layer['visual'])), ' ],',
                 f' "remove": {json.dumps(layer["remove"])},', ' "light": [', *('  ' + r for r in row(layer['light'])), ' ],',
                 ' "add": [']
    path.write_text('\n'.join(lines + ['  ' + r for r in row(layer['add'])] + [' ]', '}']) + '\n')


def cmd_layer(_args) -> int:
    spot = fire(POSE)
    layer = {'format': 1, 'visual': [{'id': MESH_ID, 'visual': VISUAL}], 'remove': [EFFECT_ID, FLARE_ID],
             'light': [{'id': LAMP_ID, 'color': LAMP['color'], 'near': LAMP['near'], 'flicker_from': 'wall-torch-fire-flame',
                        'jitter_cm': LAMP['jitter_cm'], 'offset_cm': at(spot['light'] - LAMP_AT)}],
             'add': emitters('wall-torch-fire', spot['emitters'], 7)}
    write_layer(LAYER, layer)
    print(f'{LAYER.relative_to(ROOT)}: the wrap\'s top at {at(spot["top"])}, the light {at(spot["light"])}, lean {spot["lean_deg"]:.1f} deg in the world')
    return 0


def base_torches(vobs_json, box) -> list:
    """The wall torches of the base inside a box of x and z, by id: the mesh vob with its children."""
    found = []

    def walk(vob):
        x, _, z = vob['position']
        if str(vob.get('visual_name', '')).upper() == TORCH_MESH and box[0][0] < x < box[0][1] and box[1][0] < z < box[1][1]:
            found.append(vob)
        for child in vob.get('children', []):
            walk(child)

    for vob in json.loads(Path(vobs_json).read_text())['vobs']:
        walk(vob)
    return sorted(found, key=lambda vob: vob['id'])


def place_layer(name, torches) -> dict:
    """The layer of a place: every torch shows the own model, loses the original's fire and gets the own one."""
    place = PLACES[name]
    layer = {'format': 1, 'visual': [], 'remove': [], 'light': [], 'add': []}
    for torch in torches:
        key = f'wall-torch-{name}-{torch["id"]}'
        spot = fire(torch)
        children = torch.get('children', [])
        layer['visual'].append({'id': torch['id'], 'visual': VISUAL})
        layer['remove'] += [c['id'] for c in children if str(c.get('visual_name', '')).upper() in GONE]
        layer['add'] += emitters(key, spot['emitters'], 7 + torch['id'] * 3)
        signal = {'flicker_from': f'{key}-flame', 'jitter_cm': LAMP['jitter_cm']}
        own = [c for c in children if c['type'] == 'zCVobLight' and not c['is_static']]
        if len(own) > 1:
            sys.exit(f'torch {torch["id"]}: {len(own)} lamps of its own')
        if own:
            offset = at(spot['light'] - np.array(own[0]['position']))
            if max(abs(v) for v in offset) > 100:
                sys.exit(f'torch {torch["id"]}: its lamp {own[0]["id"]} is {offset} cm from the flame, over the layer\'s 100')
            layer['light'].append({'id': own[0]['id'], 'color': LAMP['color'], 'near': LAMP['near'], **signal, 'offset_cm': offset})
        else:  # a static fill lamp or none: the flame gets a light of its own, after its emitter in the file
            layer['add'].append({'key': f'{key}-light', 'kind': 'light', 'position': at(spot['light']), 'yaw_deg': 0,
                                 'range': place['range'], 'color': LAMP['color'], 'near': LAMP['near'], **signal})
    return layer


def cmd_places(args) -> int:
    for name, place in PLACES.items():
        torches = base_torches(args.base / 'worlds/NEWWORLD/vobs.json', place['box'])
        if len(torches) != place['count']:
            sys.exit(f'{name}: {len(torches)} torches in the box, {place["count"]} expected')
        layer = place_layer(name, torches)
        path = LAYER.with_name(f'wall-torch-{name}.json')
        write_layer(path, layer)
        added = sum(e['kind'] == 'light' for e in layer['add'])
        print(f'{path.relative_to(ROOT)}: {len(torches)} torches, {len(layer["remove"])} vobs go, '
              f'{len(layer["light"])} lamps move into the flame, {added} lights added')
    return 0


def cmd_pack(args) -> int:
    output = STAGING / args.set
    if output.exists():
        sys.exit(f'{output}: the staging set exists; choose a new name')
    if not args.atlas.is_file():
        sys.exit(f'{args.atlas}: the flame atlas is missing')
    write_dds = rig.load_compressor()
    bake = BUILD / 'atlas'
    keys = json.loads((bake / 'metrics.json').read_text())['materials']
    maps = bake / 'maps' / keys[0].lower()
    entry = rig.write_maps(write_dds, output / 'materials', output / 'materials' / NAME / 'atlas', maps)
    atlas_cfg = json.loads((BLENDER_DIR / ATLAS).read_text())
    glowing = {atlas_cfg['slots'][zone][0] for zone in atlas_cfg.get('glow', {})}
    hearth.write_glow_orm(output / 'materials' / NAME / 'atlas', maps)  # the embers' mask in the ORM alpha
    materials = {f'{key}.TGA': {**entry, 'scale': 1.0, 'antitile': 'off', 'antitile_cell': 1,
                                **({'emissive': args.emissive} if key in glowing else {})} for key in keys}
    mesh_dir = output / 'meshes' / NAME
    mesh_dir.mkdir(parents=True)
    used = rig.glb_edit(bake / 'model.glb', mesh_dir / 'model.glb', {}, set())
    missing = [m for m in used if f'{m}.TGA' not in materials]
    if missing:
        sys.exit(f'GLB materials without an entry: {missing}')
    flame = output / 'materials' / FLAME[1]
    flame.parent.mkdir(parents=True)
    shutil.copyfile(args.atlas, flame)
    materials[FLAME[0]] = {'albedo': FLAME[1]}
    (output / 'materials/materials.json').write_text(json.dumps(materials, indent=2) + '\n')
    (output / 'meshes/meshes.json').write_text(json.dumps({VISUAL: {'glb': f'{NAME}/model.glb'}}, indent=2) + '\n')
    for source in (*sorted(LAYER.parent.glob('wall-torch-*.json')), PARTICLES):
        target = output / source.relative_to(CONTENT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    metrics = json.loads((BUILD / 'metrics.json').read_text())
    atlas = json.loads((bake / 'metrics.json').read_text())
    files = {'glb': {'path': f'{args.set}/meshes/{NAME}/model.glb', 'sha256': rig.sha(mesh_dir / 'model.glb')}}
    for kind in ('albedo', 'normal', 'orm'):
        files[f'atlas.{kind}'] = {'path': f'{args.set}/materials/{entry[kind]}', 'sha256': rig.sha(output / 'materials' / entry[kind])}
    record = {'id': 'wall-torch-model', 'kind': 'propmodel', 'asset': 'wall-torch-model', 'visual': VISUAL, 'set': args.set,
              'replaces': 'OW_MISC_WALL_TORCH_01 at the Red Lantern and in Xardas\'s tower, by the vob layers',
              'builder': {'script': f'scripts/content/mesh3d/blender/{SCRIPT}', 'sha256': rig.sha(BLENDER_DIR / SCRIPT),
                          'config': f'scripts/content/mesh3d/blender/{CFG}', 'config_sha256': rig.sha(BLENDER_DIR / CFG)},
              'target': {'file': f'scripts/content/mesh3d/blender/{CFG}', 'id': 'wall-torch'},
              'model': {k: metrics[k] for k in ('triangles', 'budget', 'bbox_delta_min_m', 'bbox_delta_max_m', 'blender_version')},
              'concept': CONCEPT, 'form': 'the original mesh, measured',
              'atlas': {'config': f'scripts/content/mesh3d/blender/{ATLAS}', 'config_sha256': rig.sha(BLENDER_DIR / ATLAS),
                        'size': atlas.get('atlas'), 'coverage': atlas.get('coverage'), 'materials': keys, 'emissive': args.emissive,
                        'zones': {k: {'target': v.get('target'), 'gain': v.get('gain')} for k, v in atlas.get('zones', {}).items()}},
              'files': files}
    old = [json.loads(line) for line in REGISTRY.read_text().splitlines()] if REGISTRY.is_file() else []
    REGISTRY.write_text(''.join(json.dumps(r, ensure_ascii=False, sort_keys=True) + '\n'
                                for r in [r for r in old if r['id'] != record['id']] + [record]))
    print(f'{args.set}: {VISUAL} <- {NAME}, materials {used}, emissive {args.emissive} on {sorted(glowing)} -> {output}')
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    for name, func in (('tiles', cmd_tiles), ('model', cmd_model), ('bake', cmd_bake), ('layer', cmd_layer)):
        sub.add_parser(name).set_defaults(func=func)
    places = sub.add_parser('places')
    places.add_argument('--base', type=Path, required=True, help='the game base (its worlds/NEWWORLD/vobs.json)')
    places.set_defaults(func=cmd_places)
    pack = sub.add_parser('pack')
    pack.add_argument('--set', required=True)
    pack.add_argument('--atlas', type=Path, default=FLAME_SOURCE, help='the flame atlas (default: the installed content\'s)')
    pack.add_argument('--emissive', type=float, default=0.5, help='the glow of the wrap, 0..1 (materials.json "emissive")')
    pack.set_defaults(func=cmd_pack)
    args = parser.parse_args()
    return args.func(args)


if __name__ == '__main__':
    raise SystemExit(main())
