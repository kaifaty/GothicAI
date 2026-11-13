#!/usr/bin/env python3
"""Build, bake and pack the wall torch (asset wall-torch-model): its own model with the glowing wrap and its fire.

    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/wall_torch_set.py tiles
    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/wall_torch_set.py model
    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/wall_torch_set.py bake
    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/wall_torch_set.py layer
    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/wall_torch_set.py pack --set torch-model-1

Run from the repository root. reports/content/remaster/wall-torch-model: intent.md, concept.md, research.md.
- tiles: the surface tiles of wall-torch-tiles.json (market_tiles.py) and the wrap's glow mask (torch_tiles.py);
- model: blender/wall_torch.py with wall-torch.json;
- bake: blender/bake_atlas.py with wall-torch-atlas.json, on this machine (one prop, 1024 px, about three minutes);
- layer: the vob layer of the trial torch at the Red Lantern from the model's numbers: the base mesh vob shows the
  own model in place (vobs.d "visual": the torch's lamp hangs under that vob, so it cannot be removed), the
  original's flame effect and flare go, the fire's emitters sit on the wrap, the base lamp's light moves to the
  flame (vobs.d "offset_cm"). Written to reports/content/remaster/wall-torch-model/content/, in git;
- pack: a staging set, a part of the working content (docs/remaster-pipeline.md, sets): the model as
  OWN_WALL_TORCH_01, its atlas as three materials (the wrap's with "emissive"), the layer, the effects and the
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


def world(point_m) -> np.ndarray:
    """A point of the model, glTF metres (x, y up, z from the wall), in the world's cm at the trial torch's pose."""
    local = np.array([-point_m[0], point_m[1], point_m[2]]) * 100  # the loader mirrors X and turns metres into cm
    return np.array(POSE['rotation']) @ local + np.array(POSE['position'])


def cmd_layer(_args) -> int:
    cfg = json.loads((BLENDER_DIR / CFG).read_text())
    metrics = json.loads((BUILD / 'metrics.json').read_text())
    butt, tip = np.array(cfg['butt']), np.array(cfg['tip'])
    axis = np.array(POSE['rotation']) @ ((tip - butt) / np.linalg.norm(tip - butt))  # the club's axis in the world
    lean = np.array([axis[0], 0.0, axis[2]]) / np.hypot(axis[0], axis[2])           # the way its top leans
    top = world(metrics['wrap_top_gltf_m'])
    up = np.array([0.0, 1.0, 0.0])
    wrap_cm = (cfg['wrap']['profile'][-1][0] - metrics['wrap_base_t_m']) * 100
    radius_cm = max(r for _, r in cfg['wrap']['profile']) * 100
    at = lambda p: [round(float(v), 1) for v in p]
    # the flame card's lower edge 8 cm under the wrap's top, so the flame's body starts on the wrap's sides;
    # small tongues are born on a sphere round its upper part; sparks leave its top; burning drops fall from the
    # torn lower edge on the side that faces down; the soot starts where the flame ends
    flame = top - axis * 4 + up * (CARD[1] / 2 - 4)
    # two smaller flames on the top's rim, a third of a turn apart, widen the fire's base to the wrap's width
    across = np.cross(up, lean)
    rim = [top - axis * 2 + (lean * math.cos(a) + across * math.sin(a)) * radius_cm * 0.6 + up * (SIDE_CARD[1] / 2 - 6)
           for a in (math.radians(100), math.radians(-100))]
    emitters = [('wall-torch-fire-flame', 'OWN_TORCH_FLAME', flame),
                ('wall-torch-fire-flame-a', 'OWN_TORCH_FLAME_SIDE', rim[0]),
                ('wall-torch-fire-flame-b', 'OWN_TORCH_FLAME_SIDE', rim[1]),
                ('wall-torch-fire-licks', 'OWN_TORCH_LICKS', top - axis * 5),
                ('wall-torch-fire-sparks', 'OWN_TORCH_SPARKS', top + up * 5),
                ('wall-torch-fire-drops', 'OWN_TORCH_DROPS', top - axis * wrap_cm + lean * radius_cm),
                ('wall-torch-fire-soot', 'OWN_TORCH_SOOT', top + up * 24)]
    light = top + up * 10 + lean
    layer = {'format': 1, 'visual': [{'id': MESH_ID, 'visual': VISUAL}], 'remove': [EFFECT_ID, FLARE_ID],
             'light': [{'id': LAMP_ID, 'color': [230, 84, 15], 'near': 8, 'flicker_from': 'wall-torch-fire-flame', 'jitter_cm': 1.5,
                        'offset_cm': at(light - LAMP_AT)}],
             'add': []}
    for key, pfx, position in emitters:
        entry = {'key': key, 'kind': 'emitter', 'pfx': pfx, 'position': at(position), 'yaw_deg': 0}
        if 'flame' in key:
            # a head 14 cm thick puffs at about 1.5 / sqrt(0.14) Hz, slower than the bare club's end (research.md);
            # the side flames keep their own time
            entry['flicker'] = {'pulse_hz': 4.5, 'jitter_hz': 12, 'amount': 0.2, 'seed': 7 + len(layer['add'])}
        layer['add'].append(entry)
    LAYER.parent.mkdir(parents=True, exist_ok=True)
    lines = ['{', ' "format": 1,', f' "visual": {json.dumps(layer["visual"])},', f' "remove": {json.dumps(layer["remove"])},',
             ' "light": [' + ', '.join(json.dumps(e) for e in layer['light']) + '],', ' "add": [']
    lines += ['  ' + json.dumps(e) + (',' if i + 1 < len(layer['add']) else '') for i, e in enumerate(layer['add'])]
    LAYER.write_text('\n'.join(lines + [' ]', '}']) + '\n')
    print(f'{LAYER.relative_to(ROOT)}: the wrap\'s top at {at(top)}, the light {at(light)}, lean {np.degrees(np.arccos(axis[1])):.1f} deg in the world')
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
    for source in (LAYER, PARTICLES):
        target = output / source.relative_to(CONTENT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    metrics = json.loads((BUILD / 'metrics.json').read_text())
    atlas = json.loads((bake / 'metrics.json').read_text())
    files = {'glb': {'path': f'{args.set}/meshes/{NAME}/model.glb', 'sha256': rig.sha(mesh_dir / 'model.glb')}}
    for kind in ('albedo', 'normal', 'orm'):
        files[f'atlas.{kind}'] = {'path': f'{args.set}/materials/{entry[kind]}', 'sha256': rig.sha(output / 'materials' / entry[kind])}
    record = {'id': 'wall-torch-model', 'kind': 'propmodel', 'asset': 'wall-torch-model', 'visual': VISUAL, 'set': args.set,
              'replaces': 'OW_MISC_WALL_TORCH_01 at the Red Lantern, by the vob layer',
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
    pack = sub.add_parser('pack')
    pack.add_argument('--set', required=True)
    pack.add_argument('--atlas', type=Path, default=FLAME_SOURCE, help='the flame atlas (default: the installed content\'s)')
    pack.add_argument('--emissive', type=float, default=0.5, help='the glow of the wrap, 0..1 (materials.json "emissive")')
    pack.set_defaults(func=cmd_pack)
    args = parser.parse_args()
    return args.func(args)


if __name__ == '__main__':
    raise SystemExit(main())
