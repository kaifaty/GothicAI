#!/usr/bin/env python3
"""Build, bake and pack the new props of the harbour's traces of life (asset harbour-life, step M4 of
docs/world-detail-plan.md): a basket of fish, a stave bucket and a laundry line. They have no original:
they are new visuals that only the vob layer places (scripts/content/harbour_life.py).

    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/life_set.py tiles
    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/life_set.py models [ITEM ...]
    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/life_set.py bake [ITEM ...] [--ryzen]
    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/life_set.py pack --set life-1

Run from the repository root. Builders and configs are in scripts/content/mesh3d/blender/; outputs go to
~/.local/share/gothic-ai/content-staging/life-build/<item>/. Tiles (life-tiles.json, market_tiles.py): the
wicker and the fish skin are this asset's image_gen tiles, the rest copies of the market set's. Packing is
rigging_set.py's: one atlas per item, one materials.json key per atlas material.
"""
from __future__ import annotations

import argparse
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
BUILD = STAGING / 'life-build'
RYZEN_TASK = 'gothic-ai/harbour-life'
ITEMS = {
    'basket': dict(visual='OWN_HARBOUR_FISHBASKET_01', script='fish_basket.py', cfg='fish-basket.json', atlas='fish-basket-atlas.json'),
    'bucket': dict(visual='OWN_HARBOUR_BUCKET_01', script='stave_bucket.py', cfg='stave-bucket.json', atlas='stave-bucket-atlas.json'),
    'laundry': dict(visual='OWN_HARBOUR_LAUNDRY_01', script='laundry_line.py', cfg='laundry-line.json', atlas='laundry-line-atlas.json'),
}
REGISTRY = rig.REGISTRY
CONCEPTS = {
    'basket': 'harbour-life/concept-life-basket-attempt-1.png',
    'bucket': 'harbour-life/concept-life-bucket-attempt-2.png',
    'laundry': 'harbour-life/concept-life-laundry-attempt-2.png',
}


def cmd_tiles(_args) -> int:
    rig.run([sys.executable, ROOT / 'scripts/content/mesh3d/market_tiles.py', '--spec', BLENDER_DIR / 'life-tiles.json'])
    return 0


def cmd_models(args) -> int:
    for item in args.items or ITEMS:
        spec = ITEMS[item]
        rig.run([rig.BLENDER, '-b', '--factory-startup', '-P', BLENDER_DIR / spec['script'], '--', BLENDER_DIR / spec['cfg'], BUILD / item],
                cwd=ROOT, stdout=subprocess.DEVNULL)
        print(item, (BUILD / item / 'metrics.json').read_text().replace('\n', ' ')[:400])
    return 0


def atlas_config(item: str, staging: str) -> dict:
    cfg = json.loads((BLENDER_DIR / ITEMS[item]['atlas']).read_text())
    cfg['glb'] = f'{staging}/life-build/{item}/model.glb'
    cfg['maps_dir'] = f"{staging}/{cfg['maps_dir']}"
    return cfg


def cmd_bake(args) -> int:
    items = args.items or list(ITEMS)
    if not args.ryzen:
        for item in items:
            path = BUILD / item / 'atlas-config.json'
            path.write_text(json.dumps(atlas_config(item, str(STAGING)), indent=1))
            rig.run([rig.BLENDER, '-b', '--factory-startup', '-P', BLENDER_DIR / 'bake_atlas.py', '--', path, BUILD / item / 'atlas'],
                    cwd=ROOT, stdout=subprocess.DEVNULL)
        return 0
    # Ryzen: a copy of the Blender scripts, the tiles and the built models; the bake runs in one gq batch job
    remote = subprocess.run(['ssh', 'ryzen', 'printf %s "$HOME"'], check=True, capture_output=True, text=True).stdout
    task = f'{remote}/{RYZEN_TASK}'
    staging = f'{task}/staging'
    rig.run(['ssh', 'ryzen', f'mkdir -p {task}/repo/scripts/content/mesh3d {staging}/life-build; df -h ~ | tail -1'])
    rig.run(['rsync', '-a', '--delete', '--exclude=__pycache__', f'{BLENDER_DIR}/', f'ryzen:{task}/repo/scripts/content/mesh3d/blender/'])
    rig.run(['rsync', '-a', '--delete', f'{STAGING / "life-tiles"}/', f'ryzen:{staging}/life-tiles/'])
    lines = ['set -e', f'cd {task}/repo']
    for item in items:
        rig.run(['rsync', '-a', '--delete', '--exclude=atlas', f'{BUILD / item}/', f'ryzen:{staging}/life-build/{item}/'])
        cfg_path = f'{staging}/life-build/{item}/atlas-config.json'
        rig.run(['ssh', 'ryzen', f'cat > {cfg_path}'], input=json.dumps(atlas_config(item, staging), indent=1).encode())
        lines.append(f'{rig.RYZEN_BLENDER} -b --factory-startup -t {args.threads} -P scripts/content/mesh3d/blender/bake_atlas.py -- '
                     f'{cfg_path} {staging}/life-build/{item}/atlas > {staging}/life-build/{item}/bake.log 2>&1 '
                     f'|| {{ tail -30 {staging}/life-build/{item}/bake.log; exit 1; }}')
        lines.append(f'grep ATLAS {staging}/life-build/{item}/bake.log | cut -c1-300')
    rig.run(['ssh', 'ryzen', f'cat > {task}/bake.sh'], input=('\n'.join(lines) + '\n').encode())
    rig.run([ROOT / 'scripts/gq', '--host', 'ryzen', 'run', '--class', 'batch', '--threads', str(args.threads), '--mem', '8G',
             '--max', '40m', '--wait-max', '3600', '--name', 'harbour-life atlas bake', '--', 'bash', f'{task}/bake.sh'],
            env={**os.environ, 'GQ_OWNER': 'harbour-life'})
    for item in items:
        rig.run(['rsync', '-a', '--delete', f'ryzen:{staging}/life-build/{item}/atlas/', f'{BUILD / item}/atlas/'])
    return 0


def cmd_pack(args) -> int:
    write_dds = rig.load_compressor()
    output = STAGING / args.set
    if output.exists():
        shutil.rmtree(output)
    materials, meshes, records = {}, {}, []
    entry_tail = dict(scale=1.0, antitile='off', antitile_cell=1)
    for item in args.items or ITEMS:
        spec = ITEMS[item]
        name = f'life-{item}'
        mesh_dir = output / 'meshes' / name
        mesh_dir.mkdir(parents=True, exist_ok=True)
        bake = BUILD / item / 'atlas'
        keys = json.loads((bake / 'metrics.json').read_text())['materials']
        entry = rig.write_maps(write_dds, output / 'materials', output / 'materials' / name / 'atlas', bake / 'maps' / keys[0].lower())
        for key in keys:
            materials[f'{key}.TGA'] = {**entry, **entry_tail}
        used = rig.glb_edit(bake / 'model.glb', mesh_dir / 'model.glb', {}, set())
        missing = [m for m in used if f'{m}.TGA' not in materials]
        if missing:
            raise SystemExit(f'{item}: GLB materials without an entry: {missing}')
        meshes[spec['visual']] = {'glb': f'{name}/model.glb'}
        records.append(record(item, args.set, output, name, used, materials))
        print(f'{item}: {spec["visual"]} <- {name}, materials {used}')
    (output / 'materials' / 'materials.json').write_text(json.dumps(materials, indent=2) + '\n')
    (output / 'meshes' / 'meshes.json').write_text(json.dumps(meshes, indent=2) + '\n')
    ids = {r['id'] for r in records}
    old = [json.loads(line) for line in REGISTRY.read_text().splitlines()] if REGISTRY.is_file() else []
    REGISTRY.write_text(''.join(json.dumps(r, ensure_ascii=False, sort_keys=True) + '\n'
                                for r in [r for r in old if r['id'] not in ids] + records))
    print(f'{args.set}: {len(meshes)} meshes, {len(materials)} materials -> {output}; {len(records)} records in {REGISTRY.relative_to(ROOT)}')
    return 0


def record(item: str, set_name: str, output: Path, name: str, used: list[str], materials: dict) -> dict:
    """One line of data/content/prop-models.jsonl, as rigging_set.record."""
    spec = ITEMS[item]
    build = BUILD / item
    cfg_path = BLENDER_DIR / spec['cfg']
    cfg = json.loads(cfg_path.read_text())
    metrics = json.loads((build / 'metrics.json').read_text())
    atlas = json.loads((build / 'atlas' / 'metrics.json').read_text())
    rec = {'id': f'harbour-life-{item}', 'kind': 'propmodel', 'asset': 'harbour-life', 'visual': spec['visual'], 'set': set_name,
           'builder': {'script': f'scripts/content/mesh3d/blender/{spec["script"]}', 'sha256': rig.sha(BLENDER_DIR / spec['script']),
                       'config': f'scripts/content/mesh3d/blender/{spec["cfg"]}', 'config_sha256': rig.sha(cfg_path)},
           'target': {'file': f'scripts/content/mesh3d/blender/{spec["cfg"]}', 'id': cfg['id']},
           'model': {k: metrics[k] for k in ('triangles', 'budget', 'bbox_delta_min_m', 'bbox_delta_max_m', 'blender_version') if k in metrics},
           'concept': CONCEPTS[item],
           'atlas': {'config': f'scripts/content/mesh3d/blender/{spec["atlas"]}', 'config_sha256': rig.sha(BLENDER_DIR / spec['atlas']),
                     'size': atlas.get('atlas'), 'coverage': atlas.get('coverage'),
                     'zones': {k: {'target': v.get('target'), 'gain': v.get('gain')} for k, v in atlas.get('zones', {}).items()}}}
    files = {'glb': {'path': f'{set_name}/meshes/{name}/model.glb', 'sha256': rig.sha(output / 'meshes' / name / 'model.glb')}}
    for mat in used:
        for kind, rel in materials[f'{mat}.TGA'].items():
            if kind in ('albedo', 'normal', 'orm'):
                files[f'{mat}.{kind}'] = {'path': f'{set_name}/materials/{rel}', 'sha256': rig.sha(output / 'materials' / rel)}
    rec['files'] = files
    return rec


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('tiles').set_defaults(func=cmd_tiles)
    p = sub.add_parser('models'); p.add_argument('items', nargs='*'); p.set_defaults(func=cmd_models)
    p = sub.add_parser('bake'); p.add_argument('items', nargs='*'); p.add_argument('--ryzen', action='store_true')
    p.add_argument('--threads', type=int, default=12); p.set_defaults(func=cmd_bake)
    p = sub.add_parser('pack'); p.add_argument('--set', required=True); p.add_argument('items', nargs='*'); p.set_defaults(func=cmd_pack)
    args = parser.parse_args()
    return args.func(args)


if __name__ == '__main__':
    raise SystemExit(main())
