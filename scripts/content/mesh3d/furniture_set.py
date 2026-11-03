#!/usr/bin/env python3
"""Build, bake and pack the house furniture set (asset city-furniture): stools, armchair, beds, dressers, tables, bench.

    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/furniture_set.py tiles
    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/furniture_set.py models [ITEM ...]
    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/furniture_set.py bake [ITEM ...] [--ryzen]
    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/furniture_set.py pack --set furniture-1

Run from the repository root. Every item is one original visual built by blender/furniture.py from its config
blender/furniture-<item>.json; numbers in data/content/mesh3d-furniture.json (furniture_targets.py). The form is the
original's mesh, the material and character the selected concept (user, 2026-11-02: «Да, по мешу»;
reports/content/remaster/city-furniture). One wood, one iron, one cloth for the set (rule O1): the tiles are the
market set's, copied under the zone keys (tiles), and each zone's colour is set by the atlas to the original's.
Outputs: ~/.local/share/gothic-ai/content-staging/city-furniture/build/<item>/ (model.glb, metrics.json, atlas/).
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
BUILD = STAGING / 'city-furniture/build'
TILES = STAGING / 'city-furniture/tiles'
RYZEN_TASK = 'gothic-ai/city-furniture'
REGISTRY = ROOT / 'data/content/prop-models.jsonl'
MARKET = STAGING / 'market-set/tiles'
HEARTH = STAGING / 'city-hearth/tiles'
# zone tile <- source tile: the set shares the market's wood, iron, cloth and leather (rule O1)
TILE_SOURCES = {
    'own_furn_wood': MARKET / 'own_prop_wood_plank', 'own_furn_darkwood': MARKET / 'own_prop_wood_frame',
    'own_furn_oak': MARKET / 'own_prop_wood_plank', 'own_furn_iron': MARKET / 'own_prop_iron_band',
    'own_furn_linen': MARKET / 'own_prop_canvas', 'own_furn_trim': MARKET / 'own_prop_canvas',
    'own_furn_straw': MARKET / 'own_prop_sacking', 'own_furn_leather': MARKET / 'own_prop_leather',
    'own_furn_brass': MARKET / 'own_prop_brass', 'own_furn_paper': MARKET / 'own_prop_canvas',
    'own_furn_stone': HEARTH / 'own_hearth_stone',
}
# zone colours (linear): the original's textures read at their native size (intent.md, «Что это за место»), the rich
# oak a little lighter than the plain pine, the leather #592D1B, the linen #6A5A47 with a green border
TARGETS = {
    'OWN_FURN_WOOD': [0.034, 0.024, 0.015], 'OWN_FURN_DARKWOOD': [0.040, 0.026, 0.016], 'OWN_FURN_OAK': [0.075, 0.046, 0.028],
    'OWN_FURN_IRON': [0.022, 0.022, 0.024], 'OWN_FURN_LINEN': [0.14, 0.11, 0.075], 'OWN_FURN_TRIM': [0.035, 0.065, 0.028],
    'OWN_FURN_STRAW': [0.089, 0.058, 0.022], 'OWN_FURN_LEATHER': [0.10, 0.026, 0.012], 'OWN_FURN_BRASS': [0.16, 0.11, 0.04],
    'OWN_FURN_PAPER': [0.45, 0.40, 0.30], 'OWN_FURN_STONE': [0.12, 0.105, 0.085],
}
# The brightness and hue of each item come from the texture it replaces (linear means of the original texture,
# measured 2026-11-02 from original_view.py at native size): one target per zone for the whole set baked the warm
# plain table and the light rich table the same cold dull brown, and in game they read grey.
TABLENORMAL, TABLEPEASANT, TABLERICH = [0.0573, 0.0296, 0.0117], [0.0667, 0.0496, 0.0357], [0.1232, 0.0576, 0.0315]
CUPBOARD, GALLOW = [0.0541, 0.0366, 0.0259], [0.0432, 0.0311, 0.0173]
ITEM_TARGETS = {
    'chair': {'OWN_FURN_WOOD': TABLEPEASANT}, 'chair-rich': {'OWN_FURN_DARKWOOD': TABLENORMAL},
    'throne': {'OWN_FURN_LEATHER': [0.1172, 0.0323, 0.0149]}, 'bed': {'OWN_FURN_WOOD': GALLOW},
    'bed-rich': {'OWN_FURN_DARKWOOD': TABLENORMAL, 'OWN_FURN_LINEN': [0.1664, 0.1169, 0.0742]},  # OBJ_CITY_BEDEDEL_01
    'bed-master': {'OWN_FURN_DARKWOOD': TABLENORMAL, 'OWN_FURN_LINEN': [0.3828, 0.3301, 0.227]},  # the canopy's NW_SHIP_SAIL_02
    'cupboard-poor': {'OWN_FURN_WOOD': CUPBOARD}, 'cupboard-rich-1': {'OWN_FURN_DARKWOOD': CUPBOARD},
    'cupboard-rich-2': {'OWN_FURN_DARKWOOD': CUPBOARD}, 'bookshelf': {'OWN_FURN_DARKWOOD': CUPBOARD},
    'table': {'OWN_FURN_WOOD': TABLENORMAL},
    'table-rich': {'OWN_FURN_OAK': TABLERICH, 'OWN_FURN_DARKWOOD': [round(c * 0.8, 4) for c in TABLERICH]},
    'bench-2': {'OWN_FURN_WOOD': [0.0589, 0.0412, 0.0326], 'OWN_FURN_STONE': [0.1121, 0.074, 0.0434]},
}
SCALES = {'OWN_FURN_WOOD': 2.5, 'OWN_FURN_DARKWOOD': 2.5, 'OWN_FURN_OAK': 2.0, 'OWN_FURN_IRON': 5.0, 'OWN_FURN_LINEN': 4.0,
          'OWN_FURN_TRIM': 6.0, 'OWN_FURN_STRAW': 3.0, 'OWN_FURN_LEATHER': 3.0, 'OWN_FURN_BRASS': 8.0, 'OWN_FURN_PAPER': 6.0,
          'OWN_FURN_STONE': 2.0}
GROUPS = {'WOOD': ('', 'WOOD'), 'DARKWOOD': ('', 'WOOD'), 'OAK': ('', 'WOOD'), 'IRON': ('_IRON', 'METAL'), 'BRASS': ('_IRON', 'METAL'),
          'LINEN': ('_CLOTH', 'UNDEF'), 'TRIM': ('_CLOTH', 'UNDEF'), 'STRAW': ('_CLOTH', 'UNDEF'), 'LEATHER': ('_CLOTH', 'UNDEF'),
          'PAPER': ('_BOOK', 'UNDEF'), 'STONE': ('_STONE', 'STONE')}
ITEMS = {
    'chair': 'CHAIR_NW_NORMAL_01', 'chair-rich': 'CHAIR_NW_EDEL_01', 'throne': 'THRONE_NW_CITY_01',
    'bed': 'BEDHIGH_NW_NORMAL_01', 'bed-rich': 'BEDHIGH_NW_EDEL_01', 'bed-master': 'BEDHIGH_NW_MASTER_01',
    'cupboard-poor': 'NW_CITY_CUPBOARD_POOR_01', 'cupboard-rich-1': 'NW_CITY_CUPBOARD_RICH_01',
    'cupboard-rich-2': 'NW_CITY_CUPBOARD_RICH_02', 'bookshelf': 'BOOK_NW_CITY_CUPBOARD_01',
    'table': 'NW_CITY_TABLE_NORMAL_01', 'table-rich': 'NW_CITY_TABLE_RICH_02', 'bench-2': 'BENCH_NW_CITY_02',
}
CONCEPT = {'chair': 13, 'chair-rich': 14, 'throne': 15, 'bed': 16, 'bed-rich': 17, 'bed-master': 18, 'cupboard-poor': 19,
           'cupboard-rich-1': 20, 'cupboard-rich-2': 21, 'bookshelf': 22, 'table': 23, 'table-rich': 24, 'bench-2': 25}


def cmd_tiles(args) -> int:
    for key, source in TILE_SOURCES.items():
        if not source.is_dir():
            raise SystemExit(f'{source}: missing (market_tiles.py / hearth_set.py made it)')
        shutil.copytree(source, TILES / key, dirs_exist_ok=True)
        if key in GRAIN:
            solid_grain(TILES / key)
    print(f'{len(TILE_SOURCES)} tiles -> {TILES}')
    return 0


GRAIN = ('own_furn_wood', 'own_furn_darkwood', 'own_furn_oak')


def solid_grain(folder: Path, planks: int = 4, margin: float = 0.025, blend: float = 0.06) -> None:
    """Turn a tile of planks into one solid board's grain, in every map. The furniture's boards are separate parts
    with UVs in metres across them, so the tile's dark plank seams crossed every board each 10 cm and read in game
    as a grid of dark dashes on the table top (2026-11-02). Each plank's inside, its seam cut off at the margins, is
    stretched to its column and blended into its neighbours over a soft overlap, wrapping round."""
    import numpy as np
    from PIL import Image
    for path in sorted(folder.glob('*.png')):
        image = Image.open(path)
        mode = image.mode
        a = np.asarray(image.convert('RGBA' if 'A' in mode else 'RGB'), dtype=np.float64)
        h, w = a.shape[:2]
        q, m, f = w // planks, int(w * margin), int(w * blend)
        acc, weight = np.zeros_like(a), np.zeros((h, w, 1))
        ramp = np.minimum(1.0, np.minimum(np.arange(q + 2 * f) + 0.5, q + 2 * f - np.arange(q + 2 * f) - 0.5) / (2 * f))
        for k in range(planks):
            inside = a[:, k * q + m:(k + 1) * q - m]
            col = np.asarray(Image.fromarray(inside.clip(0, 255).astype(np.uint8)).resize((q + 2 * f, h), Image.BICUBIC), dtype=np.float64)
            xs = (np.arange(q + 2 * f) + k * q - f) % w
            acc[:, xs] += col * ramp[None, :, None]
            weight[:, xs] += ramp[None, :, None]
        out = acc / np.maximum(weight, 1e-6)
        if path.stem == 'normal':  # keep the normals unit length after the blend
            n = out[..., :3] / 127.5 - 1
            n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-6)
            out[..., :3] = (n + 1) * 127.5
        Image.fromarray(out.round().clip(0, 255).astype(np.uint8)).convert(mode).save(path)


def cmd_models(args) -> int:
    for item in args.items or ITEMS:
        rig.run([rig.BLENDER, '-b', '--factory-startup', '-P', BLENDER_DIR / 'furniture.py', '--',
                 BLENDER_DIR / f'furniture-{item}.json', BUILD / item], cwd=ROOT, stdout=subprocess.DEVNULL)
        metrics = json.loads((BUILD / item / 'metrics.json').read_text())
        print(item, {k: metrics[k] for k in ('triangles', 'open_edges', 'inside_out_parts', 'bbox_delta_min_m', 'bbox_delta_max_m')})
        if metrics.get('inside_out_parts'):
            sys.exit(f'{item}: closed parts inside out')
    return 0


def atlas_config(item: str, staging: str) -> dict:
    """The bake config of one item: its zones (from the target), one material key per sound group."""
    targets = json.loads((ROOT / 'data/content/mesh3d-furniture.json').read_text())['targets']
    goal = next(t for t in targets if t['visual'] == ITEMS[item])
    zones = [z['key'] for z in goal['zones']]
    base = 'OWN_FURN_' + item.upper().replace('-', '_')
    slots = {}
    for key in zones:
        suffix, group = GROUPS[key.replace('OWN_FURN_', '')]
        slots[key] = [base + suffix, group]
    metal = [k for k in zones if GROUPS[k.replace('OWN_FURN_', '')][1] == 'METAL']
    return {'glb': f'{staging}/city-furniture/build/{item}/model.glb', 'maps_dir': f'{staging}/city-furniture/tiles',
            'scales': {k: SCALES[k] for k in zones}, 'targets': {k: ITEM_TARGETS[item].get(k, TARGETS[k]) for k in zones}, 'slots': slots,
            'metal': metal, 'edge_wear': {k: (0.35 if k in ('OWN_FURN_WOOD', 'OWN_FURN_DARKWOOD', 'OWN_FURN_OAK') else 0.2) for k in zones},
            'tile_normal': {k: 0.3 for k in zones if 'WOOD' in k or k == 'OWN_FURN_OAK' or k == 'OWN_FURN_LEATHER'},
            'streaks': {'length': 0.2, 'rust': 0.0, 'dirt': 0.25, 'rust_colour': [0.045, 0.024, 0.011], 'column': 0.01},
            'size': 1024, 'seed': 1, 'angle_limit': 66, 'margin': 0.004, 'metal_scale': 0.0, 'wet_height': 0.0, 'algae': 0.0,
            'bleach': 0.0, 'tone_spread': 0.08,
            'crevice': {'distance': 0.015, 'dirt': 0.4, 'colour': [0.006, 0.005, 0.004]}}


def cmd_bake(args) -> int:
    items = args.items or list(ITEMS)
    if not args.ryzen:
        for item in items:
            path = BUILD / item / 'atlas-config.json'
            path.write_text(json.dumps(atlas_config(item, str(STAGING)), indent=1))
            rig.run([rig.BLENDER, '-b', '--factory-startup', '-P', BLENDER_DIR / 'bake_atlas.py', '--', path, BUILD / item / 'atlas'],
                    cwd=ROOT, stdout=subprocess.DEVNULL)
        return 0
    remote = subprocess.run(['ssh', 'ryzen', 'printf %s "$HOME"'], check=True, capture_output=True, text=True).stdout
    task = f'{remote}/{RYZEN_TASK}'
    staging = f'{task}/staging'
    rig.run(['ssh', 'ryzen', f'mkdir -p {task}/repo/scripts/content/mesh3d {staging}/city-furniture/build; df -h ~ | tail -1'])
    rig.run(['rsync', '-a', '--delete', '--exclude=__pycache__', f'{BLENDER_DIR}/', f'ryzen:{task}/repo/scripts/content/mesh3d/blender/'])
    rig.run(['rsync', '-a', '--delete', f'{TILES}/', f'ryzen:{staging}/city-furniture/tiles/'])
    lines = ['set -e', f'cd {task}/repo']
    for item in items:
        rig.run(['rsync', '-a', '--delete', f'{BUILD / item}/', f'ryzen:{staging}/city-furniture/build/{item}/'])
        cfg_path = f'{staging}/city-furniture/build/{item}/atlas-config.json'
        rig.run(['ssh', 'ryzen', f'cat > {cfg_path}'], input=json.dumps(atlas_config(item, staging), indent=1).encode())
        log = f'{staging}/city-furniture/build/{item}/bake.log'
        lines.append(f'{rig.RYZEN_BLENDER} -b --factory-startup -t {args.threads} -P scripts/content/mesh3d/blender/bake_atlas.py -- '
                     f'{cfg_path} {staging}/city-furniture/build/{item}/atlas > {log} 2>&1 || {{ tail -30 {log}; exit 1; }}')
        lines.append(f'grep ATLAS {log} | cut -c1-200')
    rig.run(['ssh', 'ryzen', f'cat > {task}/bake.sh'], input=('\n'.join(lines) + '\n').encode())
    rig.run([ROOT / 'scripts/gq', '--host', 'ryzen', 'run', '--class', 'batch', '--threads', str(args.threads), '--mem', '8G',
             '--max', '90m', '--wait-max', '3600', '--name', 'city-furniture atlas bake', '--', 'bash', f'{task}/bake.sh'],
            env={**os.environ, 'GQ_OWNER': 'city-furniture'})
    for item in items:
        rig.run(['rsync', '-a', '--delete', f'ryzen:{staging}/city-furniture/build/{item}/atlas/', f'{BUILD / item}/atlas/'])
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
        name = f'furniture-{item}'
        mesh_dir = output / 'meshes' / name
        mesh_dir.mkdir(parents=True, exist_ok=True)
        bake = BUILD / item / 'atlas'
        keys = json.loads((bake / 'metrics.json').read_text())['materials']
        entry = rig.write_maps(write_dds, output / 'materials', output / 'materials' / name / 'atlas', bake / 'maps' / keys[0].lower())
        for key in keys:
            if f'{key}.TGA' in materials:
                raise SystemExit(f'{item}: material key {key} already used by another item')
            materials[f'{key}.TGA'] = {**entry, **tail}
        used = rig.glb_edit(bake / 'model.glb', mesh_dir / 'model.glb', {}, set())
        missing = [m for m in used if f'{m}.TGA' not in materials]
        if missing:
            raise SystemExit(f'{item}: GLB materials without an entry: {missing}')
        meshes[ITEMS[item]] = {'glb': f'{name}/model.glb'}
        records.append(record(item, args.set, output, name, used, materials))
        print(f'{item}: {ITEMS[item]} <- {name}, materials {used}')
    (output / 'materials' / 'materials.json').write_text(json.dumps(materials, indent=2) + '\n')
    (output / 'meshes' / 'meshes.json').write_text(json.dumps(meshes, indent=2) + '\n')
    ids = {r['id'] for r in records}
    old = [json.loads(line) for line in REGISTRY.read_text().splitlines()] if REGISTRY.is_file() else []
    REGISTRY.write_text(''.join(json.dumps(r, ensure_ascii=False, sort_keys=True) + '\n'
                                for r in [r for r in old if r['id'] not in ids] + records))
    print(f'{args.set}: {len(meshes)} meshes, {len(materials)} materials -> {output}')
    return 0


def record(item: str, set_name: str, output: Path, name: str, used: list[str], materials: dict) -> dict:
    """One line of data/content/prop-models.jsonl: how the item's model was made and what was shipped."""
    cfg_path = BLENDER_DIR / f'furniture-{item}.json'
    cfg = json.loads(cfg_path.read_text())
    metrics = json.loads((BUILD / item / 'metrics.json').read_text())
    atlas = json.loads((BUILD / item / 'atlas' / 'metrics.json').read_text())
    files = {'glb': {'path': f'{set_name}/meshes/{name}/model.glb', 'sha256': sha(output / 'meshes' / name / 'model.glb')}}
    for mat in used:
        for kind, rel in materials[f'{mat}.TGA'].items():
            if kind in ('albedo', 'normal', 'orm'):
                files[f'{mat}.{kind}'] = {'path': f'{set_name}/materials/{rel}', 'sha256': sha(output / 'materials' / rel)}
    return {'id': f'city-furniture-{item}', 'kind': 'propmodel', 'asset': 'city-furniture', 'visual': ITEMS[item], 'set': set_name,
            'builder': {'script': 'scripts/content/mesh3d/blender/furniture.py', 'sha256': sha(BLENDER_DIR / 'furniture.py'),
                        'config': f'scripts/content/mesh3d/blender/furniture-{item}.json', 'config_sha256': sha(cfg_path)},
            'target': {'file': cfg['target'], 'id': cfg['id']},
            'model': {k: metrics[k] for k in ('triangles', 'budget', 'bbox_delta_min_m', 'bbox_delta_max_m', 'blender_version') if k in metrics},
            'concept': f'city-furniture/concept-attempt-{CONCEPT[item]}.png', 'form': 'the original mesh (user, 2026-11-02)',
            'atlas': {'size': atlas.get('atlas'), 'coverage': atlas.get('coverage'),
                      'zones': {k: {'target': v.get('target'), 'gain': v.get('gain')} for k, v in atlas.get('zones', {}).items()}},
            'files': files}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('tiles'); p.set_defaults(func=cmd_tiles)
    p = sub.add_parser('models'); p.add_argument('items', nargs='*'); p.set_defaults(func=cmd_models)
    p = sub.add_parser('bake'); p.add_argument('items', nargs='*'); p.add_argument('--ryzen', action='store_true')
    p.add_argument('--threads', type=int, default=12); p.set_defaults(func=cmd_bake)
    p = sub.add_parser('pack'); p.add_argument('--set', required=True); p.add_argument('items', nargs='*'); p.set_defaults(func=cmd_pack)
    args = parser.parse_args()
    return args.func(args)


if __name__ == '__main__':
    raise SystemExit(main())
