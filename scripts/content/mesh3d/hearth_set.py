#!/usr/bin/env python3
"""Build, bake and pack the kitchen and hearth set (asset city-hearth): wave 1 open fire, wave 2 hearths in the houses.

    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/hearth_set.py models [ITEM ...]
    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/hearth_set.py bake [ITEM ...] [--ryzen]
    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/hearth_set.py pack --set hearth-1

Run from the repository root. Every item is one original visual: a Blender script and config in
scripts/content/mesh3d/blender/, numbers from data/content/mesh3d-hearth.json (hearth_targets.py). The form
comes from the original's mesh (the user's decision of 2026-10-31, reports/content/remaster/city-hearth),
the material from the selected concepts. Outputs go to ~/.local/share/gothic-ai/content-staging/city-hearth/
build/<item>/ (model.glb, metrics.json, atlas/); tiles are city-hearth/tiles (hearth-tiles.json, market_tiles.py).
Packing, DDS and the GLB material renames are rigging_set.py's.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rigging_set as rig  # noqa: E402

ROOT = rig.ROOT
BLENDER_DIR = rig.BLENDER_DIR
STAGING = rig.STAGING
LAYER = ROOT / 'data/content/world/NEWWORLD/vobs.d/city-hearth.json'
PARTICLES = ROOT / 'data/content/particles.d/city-hearth-steam.json'
BUILD = STAGING / 'city-hearth/build'
TILES = STAGING / 'city-hearth/tiles'
BLENDER = rig.BLENDER
RYZEN_TASK = 'gothic-ai/city-hearth'
REGISTRY = ROOT / 'data/content/prop-models.jsonl'

ITEMS = {
    'cauldron': dict(visual='CAULDRON_OC', script='cauldron.py', cfg='cauldron.json', atlas='cauldron-atlas.json'),
    'ground-fire': dict(visual='FIREPLACE_GROUND', script='ground_fire.py', cfg='ground_fire.json', atlas='ground-fire-atlas.json'),
    'pan': dict(visual='PAN_OC', script='pan.py', cfg='pan.json', atlas='pan-atlas.json'),
    'tripod': dict(visual='OC_FIREPLACE_V5', script='tripod.py', cfg='tripod.json', atlas='tripod-atlas.json'),
    'campfire': dict(visual='NW_MISC_FIREPLACE_01', script='campfire.py', cfg='campfire.json', atlas='campfire-atlas.json'),
    'brazier': dict(visual='DT_FIREPLACE_V1', script='brazier.py', cfg='brazier.json', atlas='brazier-atlas.json'),
    # wave 2: hearths in the houses
    'stove': dict(visual='STOVE_NW_CITY_01', script='stove.py', cfg='stove.json', atlas='stove-atlas.json'),
    'fireplace': dict(visual='NW_CITY_FIREPLACE_NORMAL_01', script='fireplace.py', cfg='fireplace.json', atlas='fireplace-atlas.json'),
    'fireplace-large': dict(visual='NW_CITY_FIREPLACE_LARGE_01', script='fireplace.py', cfg='fireplace-large.json',
                            atlas='fireplace-large-atlas.json'),
    'fireplace-wide': dict(visual='NW_CITY_FIREPLACE_LARGE_02', script='fireplace.py', cfg='fireplace-wide.json',
                           atlas='fireplace-wide-atlas.json'),
    'fireplace-wide-low': dict(visual='NW_CITY_FIREPLACE_LARGE_021', script='fireplace.py', cfg='fireplace-wide-low.json',
                               atlas='fireplace-wide-low-atlas.json'),
}
# what each model takes from a concept: the material and character; its form is the original's mesh
CONCEPTS = {
    'cauldron': 'city-hearth/concept-hearth-cauldron-attempt-6.png',
    'ground-fire': 'city-hearth/concept-hearth-cauldron-attempt-6.png (the fire ring under the cauldron)',
    'pan': 'city-hearth/concept-hearth-pan-attempt-8.png',
    'tripod': 'city-hearth/concept-hearth-tripod-attempt-6.png',
    'campfire': 'city-hearth/concept-hearth-tripod-attempt-6.png (the campfire under the tripod)',
    'brazier': 'city-hearth/concept-hearth-brazier-attempt-9.png',
    'stove': 'city-hearth/concept-hearth-stove-attempt-2.png',
    'fireplace': 'city-hearth/concept-hearth-fireplace-attempt-1.png',
    'fireplace-large': 'city-hearth/concept-hearth-fireplace-large-attempt-2.png',
    'fireplace-wide': 'city-hearth/concept-hearth-fireplace-wide-attempt-2.png',
    'fireplace-wide-low': 'city-hearth/concept-hearth-fireplace-wide-attempt-2.png (the same fireplace with the low hood)',
}


def cmd_models(args) -> int:
    for item in args.items or ITEMS:
        spec = ITEMS[item]
        rig.run([BLENDER, '-b', '--factory-startup', '-P', BLENDER_DIR / spec['script'], '--', BLENDER_DIR / spec['cfg'],
                 BUILD / item], cwd=ROOT, stdout=subprocess.DEVNULL)
        print(item, (BUILD / item / 'metrics.json').read_text().replace('\n', ' ')[:300])
        if json.loads((BUILD / item / 'metrics.json').read_text()).get('inside_out_parts'):
            sys.exit(f'{item}: closed parts inside out, the engine would cull them (parts.py prints which)')
    return 0


def atlas_config(item: str, staging: str) -> dict:
    cfg = json.loads((BLENDER_DIR / ITEMS[item]['atlas']).read_text())
    cfg['glb'] = f'{staging}/city-hearth/build/{item}/model.glb'
    cfg['maps_dir'] = f"{staging}/{cfg['maps_dir']}"
    return cfg


def cmd_bake(args) -> int:
    items = args.items or list(ITEMS)
    if not args.ryzen:
        for item in items:
            path = BUILD / item / 'atlas-config.json'
            path.write_text(json.dumps(atlas_config(item, str(STAGING)), indent=1))
            rig.run([BLENDER, '-b', '--factory-startup', '-P', BLENDER_DIR / 'bake_atlas.py', '--', path, BUILD / item / 'atlas'],
                    cwd=ROOT, stdout=subprocess.DEVNULL)
        return 0
    remote = subprocess.run(['ssh', 'ryzen', 'printf %s "$HOME"'], check=True, capture_output=True, text=True).stdout
    task = f'{remote}/{RYZEN_TASK}'
    staging = f'{task}/staging'
    rig.run(['ssh', 'ryzen', f'mkdir -p {task}/repo/scripts/content/mesh3d {staging}/city-hearth/build; df -h ~ | tail -1'])
    rig.run(['rsync', '-a', '--delete', '--exclude=__pycache__', f'{BLENDER_DIR}/', f'ryzen:{task}/repo/scripts/content/mesh3d/blender/'])
    rig.run(['rsync', '-a', '--delete', f'{TILES}/', f'ryzen:{staging}/city-hearth/tiles/'])
    lines = ['set -e', f'cd {task}/repo']
    for item in items:
        rig.run(['rsync', '-a', '--delete', f'{BUILD / item}/', f'ryzen:{staging}/city-hearth/build/{item}/'])
        cfg_path = f'{staging}/city-hearth/build/{item}/atlas-config.json'
        rig.run(['ssh', 'ryzen', f'cat > {cfg_path}'], input=json.dumps(atlas_config(item, staging), indent=1).encode())
        log = f'{staging}/city-hearth/build/{item}/bake.log'
        lines.append(f'{rig.RYZEN_BLENDER} -b --factory-startup -t {args.threads} -P scripts/content/mesh3d/blender/bake_atlas.py -- '
                     f'{cfg_path} {staging}/city-hearth/build/{item}/atlas > {log} 2>&1 || {{ tail -30 {log}; exit 1; }}')
        lines.append(f'grep ATLAS {log} | cut -c1-300')
    rig.run(['ssh', 'ryzen', f'cat > {task}/bake.sh'], input=('\n'.join(lines) + '\n').encode())
    rig.run([ROOT / 'scripts/gq', '--host', 'ryzen', 'run', '--class', 'batch', '--threads', str(args.threads), '--mem', '8G',
             '--max', '60m', '--wait-max', '3600', '--name', 'city-hearth atlas bake', '--', 'bash', f'{task}/bake.sh'],
            env={**os.environ, 'GQ_OWNER': 'city-hearth'})
    for item in items:
        rig.run(['rsync', '-a', '--delete', f'ryzen:{staging}/city-hearth/build/{item}/atlas/', f'{BUILD / item}/atlas/'])
    return 0


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cmd_pack(args) -> int:
    write_dds = rig.load_compressor()
    output = STAGING / args.set
    if output.exists():
        shutil.rmtree(output)
    materials, meshes, records = {}, {}, []
    tail = dict(scale=1.0, antitile='off', antitile_cell=1)
    for item in args.items or ITEMS:
        spec = ITEMS[item]
        name = f'hearth-{item}'
        mesh_dir = output / 'meshes' / name
        mesh_dir.mkdir(parents=True, exist_ok=True)
        bake = BUILD / item / 'atlas'
        keys = json.loads((bake / 'metrics.json').read_text())['materials']
        entry = rig.write_maps(write_dds, output / 'materials', output / 'materials' / name / 'atlas', bake / 'maps' / keys[0].lower())
        atlas_cfg = json.loads((BLENDER_DIR / spec['atlas']).read_text())
        # pot water: how cloudy the engine's clear water is (materials.json "turbidity"), by the zone's material key
        turbidity = {atlas_cfg['slots'][zone][0]: value for zone, value in atlas_cfg.get('turbidity', {}).items()}
        # ... and over how many cm of liquid it reaches that (a stew: a few cm; absent: at once, as water)
        depth = {atlas_cfg['slots'][zone][0]: value for zone, value in atlas_cfg.get('turbidity_depth', {}).items()}
        for key in keys:
            if f'{key}.TGA' in materials:
                raise SystemExit(f'{item}: material key {key} already used by another item')
            materials[f'{key}.TGA'] = {**entry, **tail, **({'turbidity': turbidity[key]} if key in turbidity else {}),
                                     **({'turbidity_depth': depth[key]} if key in depth else {})}
        used = rig.glb_edit(bake / 'model.glb', mesh_dir / 'model.glb', {}, set())
        missing = [m for m in used if f'{m}.TGA' not in materials]
        if missing:
            raise SystemExit(f'{item}: GLB materials without an entry: {missing}')
        meshes[spec['visual']] = {'glb': f'{name}/model.glb'}
        records.append(record(item, args.set, output, name, used, materials))
        print(f'{item}: {spec["visual"]} <- {name}, materials {used}')
    (output / 'materials' / 'materials.json').write_text(json.dumps(materials, indent=2) + '\n')
    (output / 'meshes' / 'meshes.json').write_text(json.dumps(meshes, indent=2) + '\n')
    # fire under Edda's cauldron and steam over the pots on a fire (user, 2026-11-01): a vob layer and own PFX,
    # kept in git and carried into the set the way building_set.py compose expects them
    for source, target in ((LAYER, output / 'worlds' / 'NEWWORLD' / 'vobs.d' / LAYER.name),
                           (PARTICLES, output / 'particles.d' / PARTICLES.name)):
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    ids = {r['id'] for r in records}
    old = [json.loads(line) for line in REGISTRY.read_text().splitlines()] if REGISTRY.is_file() else []
    REGISTRY.write_text(''.join(json.dumps(r, ensure_ascii=False, sort_keys=True) + '\n'
                                for r in [r for r in old if r['id'] not in ids] + records))
    print(f'{args.set}: {len(meshes)} meshes, {len(materials)} materials -> {output}')
    return 0


def record(item: str, set_name: str, output: Path, name: str, used: list[str], materials: dict) -> dict:
    """One line of data/content/prop-models.jsonl: how the item's model was made and what was shipped."""
    spec = ITEMS[item]
    build = BUILD / item
    cfg_path = BLENDER_DIR / spec['cfg']
    cfg = json.loads(cfg_path.read_text())
    metrics = json.loads((build / 'metrics.json').read_text())
    atlas = json.loads((build / 'atlas' / 'metrics.json').read_text())
    files = {'glb': {'path': f'{set_name}/meshes/{name}/model.glb', 'sha256': sha(output / 'meshes' / name / 'model.glb')}}
    for mat in used:
        for kind, rel in materials[f'{mat}.TGA'].items():
            if kind in ('albedo', 'normal', 'orm'):
                files[f'{mat}.{kind}'] = {'path': f'{set_name}/materials/{rel}', 'sha256': sha(output / 'materials' / rel)}
    return {'id': f'city-hearth-{item}', 'kind': 'propmodel', 'asset': 'city-hearth', 'visual': spec['visual'], 'set': set_name,
            'builder': {'script': f'scripts/content/mesh3d/blender/{spec["script"]}', 'sha256': sha(BLENDER_DIR / spec['script']),
                        'config': f'scripts/content/mesh3d/blender/{spec["cfg"]}', 'config_sha256': sha(cfg_path)},
            'target': {'file': cfg['target'], 'id': cfg['id']},
            'model': {k: metrics[k] for k in ('triangles', 'budget', 'bbox_delta_min_m', 'bbox_delta_max_m', 'blender_version') if k in metrics},
            'concept': CONCEPTS.get(item), 'form': 'the original mesh (user, 2026-10-31)',
            'atlas': {'config': f'scripts/content/mesh3d/blender/{spec["atlas"]}', 'config_sha256': sha(BLENDER_DIR / spec['atlas']),
                      'size': atlas.get('atlas'), 'coverage': atlas.get('coverage'),
                      'zones': {k: {'target': v.get('target'), 'gain': v.get('gain')} for k, v in atlas.get('zones', {}).items()}},
            'files': files}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('models'); p.add_argument('items', nargs='*'); p.set_defaults(func=cmd_models)
    p = sub.add_parser('bake'); p.add_argument('items', nargs='*'); p.add_argument('--ryzen', action='store_true')
    p.add_argument('--threads', type=int, default=12); p.set_defaults(func=cmd_bake)
    p = sub.add_parser('pack'); p.add_argument('--set', required=True); p.add_argument('items', nargs='*'); p.set_defaults(func=cmd_pack)
    args = parser.parse_args()
    return args.func(args)


if __name__ == '__main__':
    raise SystemExit(main())
