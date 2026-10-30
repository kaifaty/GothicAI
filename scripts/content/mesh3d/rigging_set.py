#!/usr/bin/env python3
"""Build, bake and pack the harbour rigging set (asset harbour-rigging, step M3 of docs/world-detail-plan.md).

    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/rigging_set.py models [ITEM ...]            # Blender models on this machine
    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/rigging_set.py bake [ITEM ...] [--ryzen]    # bake_atlas.py; --ryzen: gq batch
    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/rigging_set.py pack --set rigging-1         # content-staging/<set>

Run from the repository root. Every item is one original visual: a Blender script and config in
scripts/content/mesh3d/blender/, numbers from data/content/mesh3d-rigging.json (rigging_targets.py) or the
older pilot/market targets for the crate and the crate pile. Outputs go to
~/.local/share/gothic-ai/content-staging/rigging-build/<item>/ (model.glb, metrics.json, atlas/).
Tiles: market-set/crate-tiles for the crates, rigging-tiles for the rest (net_tiles.py writes the nets;
the wood, iron and rope tiles are the market set's image_gen tiles, copied by `tiles`).

The nets are not baked: they are alpha-tested tiles. pack gives a net material its tile as its own
materials.json entry, sets alphaMode MASK on it in the GLB, and merges the rack's baked model with its
net panels. Everything else is packed as pack_prop.py does: one atlas, one key per atlas material.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[3]
BLENDER_DIR = ROOT / 'scripts/content/mesh3d/blender'
STAGING = Path.home() / '.local/share/gothic-ai/content-staging'
BUILD = STAGING / 'rigging-build'
TILES = STAGING / 'rigging-tiles'
MARKET = STAGING / 'market-set'
BLENDER = os.environ.get('BLENDER', '/Applications/Blender.app/Contents/MacOS/Blender')
RYZEN_TASK = 'gothic-ai/rigging'
RYZEN_BLENDER = '~/gothic-ai/mesh3d/blender/blender'

# item: visual, builder script, config, atlas config (None: not baked), tiled materials of the GLB
# {zone key: [material name, tile folder, linear colour target or None]}
ITEMS = {
    'crate': dict(visual='NW_HARBOUR_CRATE_01', script='crate.py', cfg='crate-rig.json', atlas='crate-rig-atlas.json'),
    'crate-pile': dict(visual='NW_HARBOUR_CRATEPILE_01', script='crate_pile.py', cfg='crate-pile-rig.json', atlas='crate-pile-rig-atlas.json'),
    'crate-broken': dict(visual='NW_HARBOUR_CRATEBROKEN_01', script='crate_broken.py', cfg='crate-broken.json',
                         atlas='crate-broken-atlas.json'),
    'rope': dict(visual='NW_HARBOUR_ROPE_01', script='rope_coil.py', cfg='rope-coil.json', atlas='rope-coil-atlas.json'),
    'cannon': dict(visual='NW_HARBOUR_CANON_01', script='cannon.py', cfg='cannon.json', atlas='cannon-atlas.json'),
    'bollard-rope': dict(visual='NW_HARBOUR_IRONROPEHOLDER_WROPE_01', script='bollard.py', cfg='bollard-rope.json',
                         atlas='bollard-rope-atlas.json'),
    'bollard': dict(visual='NW_HARBOUR_IRONROPEHOLDER_01', script='bollard.py', cfg='bollard.json', atlas='bollard-atlas.json'),
    'net-big': dict(visual='NW_HARBOUR_FISHINGNETBIG', script='net_heap.py', cfg='net-big.json', atlas=None,
                    tiled={'OWN_PROP_NET': ['OWN_NETBIG_HARBOUR_NET', 'own_prop_net', [0.05, 0.04, 0.025]],
                           'OWN_PROP_NET_ROLL': ['OWN_NETBIG_HARBOUR_CORE', 'own_prop_net_roll', [0.03, 0.025, 0.016]],
                           'OWN_PROP_ROPE': ['OWN_NETBIG_HARBOUR_ROPE', 'own_prop_rope', [0.045, 0.036, 0.024]]}),
    'net-small': dict(visual='NW_HARBOUR_FISHINGNETSMALL', script='net_heap.py', cfg='net-small.json', atlas=None,
                      tiled={'OWN_PROP_NET': ['OWN_NETSMALL_HARBOUR_NET', 'own_prop_net', [0.05, 0.04, 0.025]],
                           'OWN_PROP_NET_ROLL': ['OWN_NETSMALL_HARBOUR_CORE', 'own_prop_net_roll', [0.03, 0.025, 0.016]],
                             'OWN_PROP_ROPE': ['OWN_NETSMALL_HARBOUR_ROPE', 'own_prop_rope', [0.045, 0.036, 0.024]]}),
    'net-rack': dict(visual='NW_HARBOUR_FISHINGNET_01', script='net_rack.py', cfg='net-rack.json', atlas='net-rack-atlas.json',
                     tiled={'OWN_PROP_NET': ['OWN_NETRACK_HARBOUR_NET', 'own_prop_net_fine', [0.032, 0.027, 0.018]]}, extra_glb='nets.glb'),
}
REGISTRY = ROOT / 'data/content/prop-models.jsonl'
# the concept each model was built from (content-sources paths, `remaster.py select` of the asset)
CONCEPTS = {
    'crate': 'reports/content/2026-10-crates-redesign: variant 2, chosen by the user 2026-10-21',
    'crate-pile': 'reports/content/2026-10-crates-redesign: variant 2, chosen by the user 2026-10-21',
    'crate-broken': 'harbour-rigging/concept-rig-crate-broken-attempt-1.png',
    'rope': 'harbour-rigging/concept-rig-cannon-attempt-3.png (the coil beside the cannon); form from the original',
    'cannon': 'harbour-rigging/concept-rig-cannon-attempt-1.png',
    'bollard-rope': 'harbour-rigging/concept-rig-bollard-rope-attempt-2.png',
    'bollard': 'harbour-rigging/concept-rig-bollard-rope-attempt-2.png (without the rope)',
    'net-big': 'harbour-rigging/concept-rig-net-heap-attempt-1.png',
    'net-small': 'harbour-rigging/concept-rig-net-heap-attempt-1.png',
    'net-rack': 'harbour-rigging/concept-rig-net-rack-attempt-1.png',
}
COPIED_TILES = {  # rigging-tiles/<key> <- market-set/<folder>
    'own_prop_wood_frame': 'tiles/own_prop_wood_frame', 'own_prop_iron_band': 'tiles/own_prop_iron_band',
    'own_prop_iron_cast': 'tiles/own_prop_iron_band', 'own_prop_iron_bore': 'tiles/own_prop_iron_band',
    'own_prop_rope': 'tiles/own_prop_rope',
}


def run(cmd: list[str], **kw) -> None:
    print('+', ' '.join(shlex.quote(str(c)) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def cmd_tiles(_args) -> int:
    for key, folder in COPIED_TILES.items():
        dst = TILES / key
        dst.mkdir(parents=True, exist_ok=True)
        for png in (MARKET / folder).glob('*.png'):
            shutil.copyfile(png, dst / png.name)
    run([sys.executable, ROOT / 'scripts/content/mesh3d/net_tiles.py', '--out', TILES])
    return 0


def cmd_models(args) -> int:
    for item in args.items or ITEMS:
        spec = ITEMS[item]
        run([BLENDER, '-b', '--factory-startup', '-P', BLENDER_DIR / spec['script'], '--', BLENDER_DIR / spec['cfg'], BUILD / item],
            cwd=ROOT, stdout=subprocess.DEVNULL)
        print(item, (BUILD / item / 'metrics.json').read_text().replace('\n', ' ')[:400])
    return 0


def atlas_config(item: str, staging: str) -> dict:
    """The item's atlas config with absolute paths under `staging` (the staging root on that machine)."""
    spec = ITEMS[item]
    cfg = json.loads((BLENDER_DIR / spec['atlas']).read_text())
    cfg['glb'] = f'{staging}/rigging-build/{item}/model.glb'
    cfg['maps_dir'] = f"{staging}/{cfg['maps_dir']}"
    return cfg


def cmd_bake(args) -> int:
    items = [i for i in (args.items or ITEMS) if ITEMS[i]['atlas']]
    if not args.ryzen:
        for item in items:
            path = BUILD / item / 'atlas-config.json'
            path.write_text(json.dumps(atlas_config(item, str(STAGING)), indent=1))
            run([BLENDER, '-b', '--factory-startup', '-P', BLENDER_DIR / 'bake_atlas.py', '--', path, BUILD / item / 'atlas'],
                cwd=ROOT, stdout=subprocess.DEVNULL)
        return 0
    # Ryzen: a copy of the Blender scripts and the built models; the bake runs in one gq batch job.
    remote = subprocess.run(['ssh', 'ryzen', 'printf %s "$HOME"'], check=True, capture_output=True, text=True).stdout
    task = f'{remote}/{RYZEN_TASK}'
    staging = f'{task}/staging'
    run(['ssh', 'ryzen', f'mkdir -p {task}/repo/scripts/content/mesh3d {task}/repo/data/content {staging}/rigging-build {staging}/market-set; df -h ~ | tail -1'])
    run(['rsync', '-a', '--delete', '--exclude=__pycache__', f'{BLENDER_DIR}/', f'ryzen:{task}/repo/scripts/content/mesh3d/blender/'])
    run(['rsync', '-a', f'{TILES}/', f'ryzen:{staging}/rigging-tiles/'])
    run(['rsync', '-a', f'{MARKET}/crate-tiles/', f'ryzen:{staging}/market-set/crate-tiles/'])
    lines = ['set -e', f'cd {task}/repo']
    for item in items:
        run(['rsync', '-a', '--delete', f'{BUILD / item}/', f'ryzen:{staging}/rigging-build/{item}/'])
        cfg = atlas_config(item, staging)
        cfg_path = f'{staging}/rigging-build/{item}/atlas-config.json'
        run(['ssh', 'ryzen', f'cat > {cfg_path}'], input=json.dumps(cfg, indent=1).encode())
        lines.append(f'{RYZEN_BLENDER} -b --factory-startup -t {args.threads} -P scripts/content/mesh3d/blender/bake_atlas.py -- '
                     f'{cfg_path} {staging}/rigging-build/{item}/atlas > {staging}/rigging-build/{item}/bake.log 2>&1 '
                     f'|| {{ tail -30 {staging}/rigging-build/{item}/bake.log; exit 1; }}')
        lines.append(f'grep ATLAS {staging}/rigging-build/{item}/bake.log | cut -c1-300')
    script = '\n'.join(lines) + '\n'
    run(['ssh', 'ryzen', f'cat > {task}/bake.sh'], input=script.encode())
    run([ROOT / 'scripts/gq', '--host', 'ryzen', 'run', '--class', 'batch', '--threads', str(args.threads), '--mem', '8G',
         '--max', '60m', '--wait-max', '3600', '--name', 'rigging atlas bake', '--', 'bash', f'{task}/bake.sh'],
        env={**os.environ, 'GQ_OWNER': 'rigging'})
    for item in items:
        run(['rsync', '-a', '--delete', f'ryzen:{staging}/rigging-build/{item}/atlas/', f'{BUILD / item}/atlas/'])
    return 0


def load_compressor():
    sys.path.insert(0, str(ROOT / 'scripts/content'))
    spec = importlib.util.spec_from_file_location('material_fixture', ROOT / 'scripts/content/test-material.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.write_dds


def read_png(path: Path, mode: str) -> np.ndarray:
    with Image.open(path) as image:
        return np.asarray(image.convert(mode), dtype=np.float32) / 255


def write_maps(write_dds, root: Path, folder: Path, maps: Path, colour: list[float] | None = None, alpha: bool = False) -> dict:
    """albedo/normal/orm DDS from a folder of PNG maps; colour: scale the albedo to this linear mean."""
    folder.mkdir(parents=True, exist_ok=True)
    albedo = read_png(maps / 'albedo.png', 'RGBA' if alpha else 'RGB')
    if colour is not None:
        lin = albedo[..., :3] ** 2.2
        mask = albedo[..., 3] >= 0.5 if alpha else np.ones(lin.shape[:2], bool)
        gain = np.array(colour) / lin[mask].mean(0)
        albedo[..., :3] = np.clip(lin * gain, 0, 1) ** (1 / 2.2)
    if alpha:
        albedo[..., 3] = (albedo[..., 3] >= 0.5).astype(np.float32)
    normal = read_png(maps / 'normal.png', 'RGB') * 2 - 1
    normal[:, :, 1] *= -1  # PNG +Y up -> engine bitangent follows texture V down, as pack_prop.py
    normal[:, :, 2] = np.maximum(normal[:, :, 2], 1e-3)
    normal /= np.linalg.norm(normal, axis=2, keepdims=True)
    write_dds(folder / 'albedo.dds', albedo)
    write_dds(folder / 'normal.dds', normal, normal=True)
    write_dds(folder / 'orm.dds', np.stack((read_png(maps / 'ao.png', 'L'), read_png(maps / 'roughness.png', 'L'),
                                            read_png(maps / 'metallic.png', 'L')), axis=2))
    return {name: (folder / f'{name}.dds').relative_to(root).as_posix() for name in ('albedo', 'normal', 'orm')}


def glb_edit(path: Path, out: Path, names: dict[str, str], masked: set[str]) -> list[str]:
    """Rename GLB materials (zone key -> material key) and set alphaMode MASK on the masked ones."""
    data = path.read_bytes()
    length = struct.unpack_from('<I', data, 12)[0]
    doc = json.loads(data[20:20 + length])
    rest = data[20 + length:]
    for mat in doc.get('materials', []):
        mat['name'] = names.get(mat['name'], mat['name'])
        if mat['name'] in masked:
            mat['alphaMode'] = 'MASK'
            mat['alphaCutoff'] = 0.5
            mat['doubleSided'] = True
    blob = json.dumps(doc, separators=(',', ':')).encode()
    blob += b' ' * (-len(blob) % 4)
    out.write_bytes(struct.pack('<4sII', b'glTF', 2, 12 + 8 + len(blob) + len(rest)) + struct.pack('<I4s', len(blob), b'JSON') + blob + rest)
    return [m['name'] for m in doc.get('materials', [])]


def cmd_pack(args) -> int:
    write_dds = load_compressor()
    output = STAGING / args.set
    if output.exists():
        shutil.rmtree(output)
    materials, meshes, records = {}, {}, []
    entry_tail = dict(scale=1.0, antitile='off', antitile_cell=1)
    for item in args.items or ITEMS:
        spec = ITEMS[item]
        name = f'rigging-{item}'
        mesh_dir = output / 'meshes' / name
        mesh_dir.mkdir(parents=True, exist_ok=True)
        names, masked = {}, set()
        if spec['atlas']:
            bake = BUILD / item / 'atlas'
            keys = json.loads((bake / 'metrics.json').read_text())['materials']
            entry = write_maps(write_dds, output / 'materials', output / 'materials' / name / 'atlas', bake / 'maps' / keys[0].lower())
            for key in keys:
                materials[f'{key}.TGA'] = {**entry, **entry_tail}
            glb = bake / 'model.glb'
            if spec.get('extra_glb'):
                merged = BUILD / item / 'merged.glb'
                run([BLENDER, '-b', '--factory-startup', '-P', BLENDER_DIR / 'merge_glb.py', '--', merged, glb,
                     BUILD / item / spec['extra_glb']], stdout=subprocess.DEVNULL)
                glb = merged
        else:
            glb = BUILD / item / 'model.glb'
        for zone, (key, tile, colour) in spec.get('tiled', {}).items():
            alpha = 'net' in tile and 'roll' not in tile
            entry = write_maps(write_dds, output / 'materials', output / 'materials' / name / key.lower(), TILES / tile, colour, alpha)
            materials[f'{key}.TGA'] = {**entry, **entry_tail}
            names[zone] = key
            if alpha:
                masked.add(key)
        used = glb_edit(glb, mesh_dir / 'model.glb', names, masked)
        # an atlas slot or a tiled material without faces (a zone the model no longer uses) gets no entry
        for key in (keys if spec['atlas'] else []) + [k for k, _, _ in spec.get('tiled', {}).values()]:
            if key not in used:
                materials.pop(f'{key}.TGA', None)
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


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def record(item: str, set_name: str, output: Path, name: str, used: list[str], materials: dict) -> dict:
    """One line of data/content/prop-models.jsonl: how the item's model was made and what was shipped."""
    spec = ITEMS[item]
    build = BUILD / item
    cfg_path = BLENDER_DIR / spec['cfg']
    cfg = json.loads(cfg_path.read_text())
    metrics = json.loads((build / 'metrics.json').read_text())
    rec = {'id': f'harbour-rigging-{item}', 'kind': 'propmodel', 'asset': 'harbour-rigging', 'visual': spec['visual'],
           'set': set_name,
           'builder': {'script': f'scripts/content/mesh3d/blender/{spec["script"]}', 'sha256': sha(BLENDER_DIR / spec['script']),
                       'config': f'scripts/content/mesh3d/blender/{spec["cfg"]}', 'config_sha256': sha(cfg_path)},
           'target': {'file': cfg.get('target', 'data/content/mesh3d-pilot.json'), 'id': cfg['id']},
           'model': {k: metrics[k] for k in ('triangles', 'budget', 'bbox_delta_min_m', 'bbox_delta_max_m', 'blender_version') if k in metrics},
           'concept': CONCEPTS.get(item)}
    if spec['atlas']:
        atlas = json.loads((build / 'atlas' / 'metrics.json').read_text())
        rec['atlas'] = {'config': f'scripts/content/mesh3d/blender/{spec["atlas"]}', 'config_sha256': sha(BLENDER_DIR / spec['atlas']),
                        'size': atlas.get('atlas'), 'coverage': atlas.get('coverage'),
                        'zones': {k: {'target': v.get('target'), 'gain': v.get('gain')} for k, v in atlas.get('zones', {}).items()}}
    if spec.get('tiled'):
        rec['tiles'] = {key: {'tile': f'rigging-tiles/{tile}', 'albedo_sha256': sha(TILES / tile / 'albedo.png'), 'colour': colour}
                        for key, tile, colour in spec['tiled'].values() if key in used}
    if spec.get('extra_glb'):
        rec['model']['extra_triangles'] = metrics.get('net_triangles')
    files = {'glb': {'path': f'{set_name}/meshes/{name}/model.glb', 'sha256': sha(output / 'meshes' / name / 'model.glb')}}
    for mat in used:
        for kind, rel in materials[f'{mat}.TGA'].items():
            if kind in ('albedo', 'normal', 'orm'):
                files[f'{mat}.{kind}'] = {'path': f'{set_name}/materials/{rel}', 'sha256': sha(output / 'materials' / rel)}
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
