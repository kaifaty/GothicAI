#!/usr/bin/env python3
"""Build, bake and pack the kitchen and hearth set (asset city-hearth): wave 1 open fire, wave 2 hearths in the houses,
wave 3 the street lamp.

    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/hearth_set.py models [ITEM ...]
    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/hearth_set.py bake [ITEM ...] [--ryzen]
    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/hearth_set.py pack --set hearth-1
    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/hearth_set.py fire-tiles

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
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rigging_set as rig  # noqa: E402

ROOT = rig.ROOT
BLENDER_DIR = rig.BLENDER_DIR
STAGING = rig.STAGING
LAYER = ROOT / 'data/content/world/NEWWORLD/vobs.d/city-hearth.json'
PARTICLES = ROOT / 'data/content/particles.d/city-hearth-steam.json'
# the street lamp's own fire (lamp-design.md): a steady candle flame and a flickering light in place of the base
# bundle FIRETREE_LAMP.ZEN, which the 43 lamps spawn on their fire slot
LAMP_PARTICLES = ROOT / 'data/content/particles.d/city-hearth-lamp.json'
LAMP_TREE = ROOT / 'data/content/vobtrees.d/FIRETREE_LAMP.json'
STEAM_TEXTURE, STEAM_DDS = 'OWN_POT_STEAM.TGA', 'own-pot-steam/albedo.dds'
# the flame flipbook of OWN_POT_FIRE_FB: rendered from a Mantaflow sim by scripts/content/fire/fire_flipbook.py on
# Ryzen and packed by fire_atlas.py into the sources (16 MB, outside git); reports/engine/2026-11-01-fire-rendering.md
FIRE_SOURCES = Path.home() / '.local/share/gothic-ai/content-sources'
FIRE_ATLASES = [('OWN_FIRE_FLIPBOOK.TGA', 'own-fire-flipbook/albedo.dds', FIRE_SOURCES / 'fire-flipbook/albedo.dds')] + [
    (f'OWN_FIRE_TONGUE_{i}.TGA', f'own-fire-tongue/tongue-{i}.dds', FIRE_SOURCES / f'fire-tongue/tongue-{i}.dds')
    for i in (1, 2, 3)] + [
    ('OWN_CANDLE_FLAME.TGA', 'own-candle-flame/albedo.dds', FIRE_SOURCES / 'candle-flame/albedo.dds')]  # candle_flame.py
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
    # wave 3: the street lamp; its filigree faces are an alpha-tested tile (lamp_tiles.py), not baked
    'lamp': dict(visual='FIREPLACE_NW_LAMP_01', script='lamp.py', cfg='lamp.json', atlas='lamp-atlas.json',
                 extra_glb='lace.glb', tiled={'OWN_HEARTH_LACE': ('OWN_HEARTH_LAMP_LACE', 'own_hearth_lace')}),
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
    'lamp': 'city-hearth/concept-attempt-5.png (a candle in the open filigree, the lace\'s shadow)',
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


def cmd_fire_tiles(args) -> int:
    """Burning and charred wood tiles and the ember glow mask (fire_tiles.py), the street lamp's filigree and candle
    (lamp_tiles.py), after market_tiles.py made the rest."""
    rig.run([sys.executable, Path(__file__).with_name('fire_tiles.py'), TILES], cwd=ROOT)
    rig.run([sys.executable, Path(__file__).with_name('lamp_tiles.py'), TILES], cwd=ROOT)
    return 0


def write_glow_orm(folder: Path, maps: Path) -> None:
    """The ORM again as DXT5 with the glow mask of the bake (emission.png) in its alpha, as the engine reads it
    for a material with "emissive" (main.frag, mainGBuffer). Box mips: a glowing crack fades out with distance
    as its share of the texel does."""
    spec = importlib.util.spec_from_file_location('material_fixture', ROOT / 'scripts/content/test-material.py')
    dds = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(dds)
    orm = np.stack([rig.read_png(maps / f'{n}.png', 'L') for n in ('ao', 'roughness', 'metallic')]
                   + [rig.read_png(maps / 'emission.png', 'L')], axis=2)
    h, w = orm.shape[:2]
    levels = int(np.log2(min(w, h))) + 1
    header = [124, 0xA1007, h, w, (w // 4) * (h // 4) * 16, 0, levels] + [0] * 11
    header += [32, 4, int.from_bytes(b'DXT5', 'little'), 0, 0, 0, 0, 0, 0x401008, 0, 0, 0, 0]
    with (folder / 'orm.dds').open('wb') as out:
        out.write(b'DDS ' + struct.pack('<31I', *header))
        for level in range(levels):
            out.write(dds.bc3(dds.blocks(dds.quantize(orm))).tobytes())
            if level + 1 < levels:
                orm = dds.downsample(orm)


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
        # embers and burning log ends: the materials of the glowing zones give off light where the ORM alpha says
        glowing = {atlas_cfg['slots'][zone][0] for zone in atlas_cfg.get('glow', {})}
        if glowing:
            write_glow_orm(output / 'materials' / name / 'atlas', bake / 'maps' / keys[0].lower())
        # pot water: how cloudy the engine's clear water is (materials.json "turbidity"), by the zone's material key
        turbidity = {atlas_cfg['slots'][zone][0]: value for zone, value in atlas_cfg.get('turbidity', {}).items()}
        # ... and over how many cm of liquid it reaches that (a stew: a few cm; absent: at once, as water)
        depth = {atlas_cfg['slots'][zone][0]: value for zone, value in atlas_cfg.get('turbidity_depth', {}).items()}
        for key in keys:
            if f'{key}.TGA' in materials:
                raise SystemExit(f'{item}: material key {key} already used by another item')
            materials[f'{key}.TGA'] = {**entry, **tail, **({'turbidity': turbidity[key]} if key in turbidity else {}),
                                     **({'turbidity_depth': depth[key]} if key in depth else {}),
                                     **({'emissive': 1.0} if key in glowing else {})}
        glb, names, masked = bake / 'model.glb', {}, set()
        if spec.get('extra_glb'):  # parts that are not baked (the lamp's filigree) join the baked model here
            glb = BUILD / item / 'merged.glb'
            rig.run([BLENDER, '-b', '--factory-startup', '-P', BLENDER_DIR / 'merge_glb.py', '--', glb, bake / 'model.glb',
                     BUILD / item / spec['extra_glb']], stdout=subprocess.DEVNULL)
        for zone, (key, tile) in spec.get('tiled', {}).items():
            folder = output / 'materials' / name / key.lower()
            entry_tiled = rig.write_maps(write_dds, output / 'materials', folder, TILES / tile, alpha=True)
            materials[f'{key}.TGA'] = {**entry_tiled, **tail}
            names[zone] = key
            masked.add(key)
        used = rig.glb_edit(glb, mesh_dir / 'model.glb', names, masked)
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
                           (PARTICLES, output / 'particles.d' / PARTICLES.name),
                           (LAMP_PARTICLES, output / 'particles.d' / LAMP_PARTICLES.name),
                           (LAMP_TREE, output / 'vobtrees.d' / LAMP_TREE.name)):
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    write_steam_sprite(output)
    copy_fire_flipbook(output)
    if 'lamp' in (args.items or ITEMS):
        write_lamp_cookie(output, write_dds)
        write_lamp_glow(output)
    ids = {r['id'] for r in records}
    old = [json.loads(line) for line in REGISTRY.read_text().splitlines()] if REGISTRY.is_file() else []
    REGISTRY.write_text(''.join(json.dumps(r, ensure_ascii=False, sort_keys=True) + '\n'
                                for r in [r for r in old if r['id'] not in ids] + records))
    print(f'{args.set}: {len(meshes)} meshes, {len(materials)} materials -> {output}')
    return 0


def write_steam_sprite(output: Path) -> None:
    """OWN_POT_STEAM.TGA, the sprite of the pot steam (visName_S of OWN_POT_STEAM): white, its alpha a soft round
    window times smooth noise, so a puff is torn at the edge (no image_gen, no original pixels). Without it
    LIGHTSMOKE's dark smoke sprite turned the steam into a brown haze."""
    import importlib.util
    import numpy as np
    from scipy.ndimage import gaussian_filter
    spec = importlib.util.spec_from_file_location('harbour_motion', ROOT / 'scripts/content/harbour_motion.py')
    hm = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(hm)
    size, rng = 128, np.random.default_rng(11)
    noise = sum(w * gaussian_filter(rng.standard_normal((size, size)), s, mode='wrap') for s, w in ((12, 0.6), (4, 0.4)))
    noise = (noise - noise.min()) / (noise.max() - noise.min())
    yy, xx = np.mgrid[0:size, 0:size]
    r = np.hypot((xx + .5) / size * 2 - 1, (yy + .5) / size * 2 - 1)
    window = np.clip((1.0 - r) / 0.7, 0, 1)
    alpha = window * window * (3 - 2 * window) * (0.35 + 0.65 * noise)
    hm.write_soft_dds(output / 'materials' / STEAM_DDS, np.dstack([np.ones((size, size, 3)), alpha]))
    index_path = output / 'materials' / 'materials.json'
    index = json.loads(index_path.read_text())
    index[STEAM_TEXTURE] = {'albedo': STEAM_DDS}
    index_path.write_text(json.dumps(index, indent=2) + '\n')


def write_lamp_glow(output: Path) -> None:
    """OWN_LAMP_GLOW.TGA, the soft halo round the street lamp's candle (OWN_LAMP_GLOW): a candle in a lantern reads
    as a bright point across a street, its glare in the eye, while a 4 x 8 cm flame card covers two pixels at 10 m
    (lamp-r7). Warm, falling off as exp(-r^2) to nothing at the card's edge; additive, alpha its brightest channel."""
    import importlib.util
    spec = importlib.util.spec_from_file_location('harbour_motion', ROOT / 'scripts/content/harbour_motion.py')
    hm = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(hm)
    size = 64
    yy, xx = np.mgrid[0:size, 0:size]
    r = np.hypot((xx + .5) / size * 2 - 1, (yy + .5) / size * 2 - 1)
    glow = np.exp(-r * r * 3.5) * np.clip((1 - r) / 0.3, 0, 1)
    lin = np.stack([glow, glow * 0.5, glow * 0.18], -1)
    rgb = np.where(lin <= 0.0031308, lin * 12.92, 1.055 * np.power(lin, 1 / 2.4) - 0.055)  # particles decode sRGB
    hm.write_soft_dds(output / 'materials' / 'own-lamp-glow' / 'albedo.dds', np.dstack([rgb, rgb.max(-1)]))
    index_path = output / 'materials' / 'materials.json'
    index = json.loads(index_path.read_text())
    index['OWN_LAMP_GLOW.TGA'] = {'albedo': 'own-lamp-glow/albedo.dds'}
    index_path.write_text(json.dumps(index, indent=2) + '\n')


def write_lamp_cookie(output: Path, write_dds) -> None:
    """OWN_LAMP_COOKIE.TGA, the street lamp's light mask (lamp_cookie.py from the packed lace tile): the light of
    FIRETREE_LAMP.json leaves through the open filigree only, so the lace falls on the cobbles round the lamp."""
    png = BUILD / 'lamp' / 'cookie.png'
    rig.run([sys.executable, Path(__file__).with_name('lamp_cookie.py'), TILES / 'own_hearth_lace' / 'albedo.png', png], cwd=ROOT)
    target = output / 'materials' / 'own-lamp-cookie' / 'albedo.dds'
    target.parent.mkdir(parents=True, exist_ok=True)
    mask = rig.read_png(png, 'L')
    write_dds(target, np.repeat(mask[..., None], 3, axis=2))
    index_path = output / 'materials' / 'materials.json'
    index = json.loads(index_path.read_text())
    index['OWN_LAMP_COOKIE.TGA'] = {'albedo': 'own-lamp-cookie/albedo.dds'}
    index_path.write_text(json.dumps(index, indent=2) + '\n')


def copy_fire_flipbook(output: Path) -> None:
    """The flame atlases of fire_atlas.py: OWN_FIRE_FLIPBOOK.TGA (the whole fire, OWN_POT_FIRE_FB) and one steady
    tongue per log gap under Edda's cauldron, OWN_FIRE_TONGUE_<i>.TGA (fire_tongue.py, OWN_POT_TONGUE[_i])."""
    index_path = output / 'materials' / 'materials.json'
    index = json.loads(index_path.read_text())
    for texture, dds, source in FIRE_ATLASES:
        if not source.is_file():
            raise SystemExit(f'{source}: missing, pack it with scripts/content/fire/fire_atlas.py')
        target = output / 'materials' / dds
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        index[texture] = {'albedo': dds}
    index_path.write_text(json.dumps(index, indent=2) + '\n')


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
    p = sub.add_parser('fire-tiles'); p.set_defaults(func=cmd_fire_tiles)
    p = sub.add_parser('pack'); p.add_argument('--set', required=True); p.add_argument('items', nargs='*'); p.set_defaults(func=cmd_pack)
    args = parser.parse_args()
    return args.func(args)


if __name__ == '__main__':
    raise SystemExit(main())
