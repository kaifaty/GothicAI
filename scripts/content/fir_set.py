#!/usr/bin/env python3
"""Pack the spruce's baked sheets into DDS and build the probe set: five spruces on the pilot's forest edge.

    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/fir_set.py \
        textures --boughs DIR --bark DIR --out WORK
    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/fir_set.py \
        probe fir-probe-1 --textures WORK [--from working-28]

`textures` reads what mesh3d/blender/fir_boughs.py and fir_bark.py baked (boughs.npz, bark.npz) and writes
WORK/{boughs,bark}-{albedo,normal,orm}.dds, PNG previews for mesh3d/blender/fir_views.py, boughs-mask.npz (the
atlas's coverage for mesh3d/blender/tree_density.py; its copy in git is mesh3d/blender/fir-boughs-mask.npz) and
textures.json:

- albedo is the baked colour times the baked occlusion (the engine's map AO dims only the sky's light, so the
  shade between needles and in the cracks of the bark has to be in the colour), then brought per channel to the
  palette of mesh3d/blender/fir.json: the mean of the needles, and of the bark above the foot. The palette comes
  from the scene measurement of reports/content/2026-11-trees/fir/README.md, not from the concept;
- the bough atlas keeps coverage in alpha and every mip averages it (glb_cards.write_cutout); its normals are
  flattened by FLAT: single needles tilt up to 90 degrees and would sparkle on a card lit by the crown's hull;
- normals are baked +Y up the picture and written for the engine, whose bitangent follows V down.

`probe` copies a working set by hard links into ~/.local/share/gothic-ai/remaster/trees-fir/NAME and adds, in
new files only, the two materials, PROBE spruces built by mesh3d/blender/fir_tree.py (the vob layer has no scale,
so every size is its own GLB; each is checked by tree_check.py and tree_density.py) and the layer
worlds/NEWWORLD/vobs.d/fir-probe.json: meshes without collision beside the original's firs at the north edge of
the meadow east of the tavern. Both materials say `"foliage": {"anchor": "tree"}` (FOLIAGE; the engine's rule is
reports/engine/2026-11-16-modern-trees/tree-wind.md): the bark and the boughs bend as one body, so their `wind`
is the same, and only the boughs flutter and let the light through.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import shutil
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).parent
BLENDER = HERE / 'mesh3d/blender'
STAGING = Path.home() / '.local/share/gothic-ai/content-staging'
PROBES = Path.home() / '.local/share/gothic-ai/remaster/trees-fir'
FOLDER = 'trees_fir'
WORLD = 'NEWWORLD'
VISUAL = 'OWN_TREE_FIR_A'
FLAT = .6                   # share of the needles' tilt kept in the atlas's normals
AO_NEEDLES, AO_BARK = 1., .7    # share of the baked occlusion that goes into the colour
ROUGH_NEEDLES, ROUGH_BARK = .85, .9
FOLIAGE = dict(bark=dict(anchor='tree', wind=1., flutter=0., translucency=0.),
               boughs=dict(anchor='tree', wind=1., flutter=1., translucency=.35))
# Gothic centimetres: x, the ground's height, z; yaw; scale; seed. The north edge of the meadow east of the tavern,
# ground NW_NATURE_WALDBODEN_03, 5 to 8 m south of the original's firs. The fourth is spruce A itself: the noon
# sun stands in the west (-x), so its western neighbour is the smallest and stands north of the line from the
# far camera of scripts/bench/fir-probe.bench, and the larger ones are further west.
PROBE = (((38700., 3456.2, 1000.), 40., 1.2, 33),
         ((39650., 3412.5, 700.), 115., 1.1, 21),
         ((40400., 3378., 950.), 250., .8, 48),
         ((41250., 3323.5, 650.), 0., 1., None),
         ((42100., 3227.1, 1600.), 170., .9, 59))


def sibling(name: str, folder: Path = HERE):
    path = folder / name
    spec = importlib.util.spec_from_file_location(path.stem.replace('-', '_'), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def png(path: Path, image: np.ndarray) -> None:
    from PIL import Image
    Image.fromarray(image).save(path)


def engine_normal(normal: np.ndarray) -> np.ndarray:
    vector = normal.copy()
    vector[..., 1] *= -1
    vector[..., 2] = np.maximum(vector[..., 2], 1e-3)
    return vector


def halved(image: np.ndarray, times: int) -> np.ndarray:
    for _ in range(times):
        h, w = image.shape[:2]
        image = image.reshape(h // 2, 2, w // 2, 2, -1).mean(axis=(1, 3))
    return image


def textures(args) -> int:
    cards, dds, pilot = sibling('glb_cards.py'), sibling('test-material.py'), sibling('megascans_pilot.py')
    palette = json.loads(args.config.read_text())['palette']
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    report = {}

    def srgb(colour) -> list:
        return [round(float(v), 1) for v in pilot.encode(np.asarray(colour, np.float64))]

    def target(name: str) -> np.ndarray:
        return pilot.linear(np.array(palette[name], np.uint8)).astype(np.float64)

    # the boughs
    data = np.load(args.boughs / 'boughs.npz')
    alpha = data['alpha'].astype(np.float32)
    covered = alpha > 1e-3
    colour = np.where(covered[..., None], data['colour'].astype(np.float32) / np.maximum(alpha, 1e-3)[..., None], 0)
    ao = data['ao'].astype(np.float32)
    albedo = colour * (1 - AO_NEEDLES * (1 - ao))[..., None]
    mean = (albedo * alpha[..., None]).sum(axis=(0, 1), dtype=np.float64) / alpha.sum(dtype=np.float64)
    gain = target('needles_srgb') / mean
    albedo = np.clip(albedo * gain, 0, 1).astype(np.float32)
    coverage = cards.write_cutout(dds, pilot.encode, out / 'boughs-albedo.dds', albedo, alpha)
    normal = data['normal'].astype(np.float32)
    normal[..., :2] *= FLAT
    normal[..., 2] = np.maximum(normal[..., 2], .05)
    normal /= np.linalg.norm(normal, axis=2, keepdims=True)
    normal = np.where(covered[..., None], normal, (0., 0., 1.)).astype(np.float32)
    normal = cards.spread_colour(normal, alpha)
    dds.write_dds(out / 'boughs-normal.dds', engine_normal(normal), normal=True)
    shade = cards.spread_colour((1 - .5 * (1 - ao))[..., None], alpha)
    orm = halved(np.concatenate([shade, np.full_like(shade, ROUGH_NEEDLES), np.zeros_like(shade)], axis=2), 1)
    dds.write_dds(out / 'boughs-orm.dds', orm.astype(np.float32))
    png(out / 'boughs.png', np.concatenate([pilot.encode(cards.spread_colour(albedo, alpha)),
                                           np.rint(alpha * 255).astype(np.uint8)[..., None]], axis=2))
    png(out / 'boughs-normal.png', np.rint((normal * .5 + .5) * 255).astype(np.uint8))
    # the coverage mask tree_density.py reads: a quarter of the side, as much of it covered as of the atlas
    quarter = halved(alpha[..., None], 2)[..., 0]
    level = np.quantile(quarter, 1 - float((alpha > .5).mean()))
    np.savez_compressed(out / 'boughs-mask.npz', bits=np.packbits(quarter > level), size=quarter.shape[0])
    report['boughs'] = dict(size=int(alpha.shape[0]), coverage_by_mip=[round(c, 4) for c in coverage],
                            mask=dict(size=int(quarter.shape[0]), coverage=round(float((quarter > level).mean()), 4)),
                            baked_mean_srgb=srgb(mean), gain=gain.round(3).tolist(),
                            mean_srgb=srgb((albedo * alpha[..., None]).sum(axis=(0, 1), dtype=np.float64)
                                           / alpha.sum(dtype=np.float64)))

    # the bark
    data = np.load(args.bark / 'bark.npz')
    sheet = json.loads((args.bark / 'bark.json').read_text())
    ao = data['ao'].astype(np.float32)
    albedo = data['colour'].astype(np.float32) * (1 - AO_BARK * (1 - ao))[..., None]
    upper = slice(0, round(albedo.shape[0] * (1 - sheet['foot_share'])))
    mean = albedo[upper].reshape(-1, 3).mean(axis=0, dtype=np.float64)
    gain = target('bark_srgb') / mean
    albedo = np.clip(albedo * gain, 0, 1).astype(np.float32)
    dds.write_dds(out / 'bark-albedo.dds', pilot.encode(albedo).astype(np.float32) / 255)
    normal = data['normal'].astype(np.float32)
    normal /= np.maximum(np.linalg.norm(normal, axis=2, keepdims=True), 1e-6)
    dds.write_dds(out / 'bark-normal.dds', engine_normal(normal), normal=True)
    shade = (1 - .6 * (1 - ao))[..., None]
    orm = halved(np.concatenate([shade, np.full_like(shade, ROUGH_BARK), np.zeros_like(shade)], axis=2), 1)
    dds.write_dds(out / 'bark-orm.dds', orm.astype(np.float32))
    png(out / 'bark.png', pilot.encode(albedo))
    png(out / 'bark-normal.png', np.rint((normal * .5 + .5) * 255).astype(np.uint8))
    report['bark'] = dict(size=int(albedo.shape[0]), baked_mean_srgb=srgb(mean), gain=gain.round(3).tolist(),
                          mean_srgb=srgb(albedo[upper].reshape(-1, 3).mean(axis=0, dtype=np.float64)),
                          foot_mean_srgb=srgb(albedo[upper.stop:].reshape(-1, 3).mean(axis=0, dtype=np.float64)))
    report['bytes'] = {p.name: p.stat().st_size for p in sorted(out.glob('*.dds'))}
    (out / 'textures.json').write_text(json.dumps(report, indent=1) + '\n')
    print(json.dumps(report, indent=1))
    return 0


def rewrite(path: Path, more: dict) -> None:
    """Add records to an index of the copied set in a new file: the copy shares its files with the source."""
    index = json.loads(path.read_text()) if path.exists() else {}
    clash = sorted(set(index) & set(more))
    if clash:
        raise SystemExit(f'{path}: the source set already has {clash}')
    path.unlink(missing_ok=True)
    path.write_text(json.dumps(index | more, indent=2) + '\n')


def probe(args) -> int:
    sys.path.insert(0, str(BLENDER))
    import fir_tree
    import tree_check
    import tree_density
    import tree_glb
    source, output = STAGING / args.source, PROBES / args.set
    if output.exists():
        raise SystemExit(f'{output}: the set exists; choose a new name')
    if not (source / 'materials/materials.json').exists():
        raise SystemExit(f'{source}: no such working set')
    config = json.loads(args.config.read_text())
    work = output.with_name(output.name + '.tmp')
    if work.exists():
        shutil.rmtree(work)
    work.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source, work, copy_function=os.link)

    directory = work / 'materials' / FOLDER
    directory.mkdir()
    materials = {}
    for part, name in config['materials'].items():
        for kind in ('albedo', 'normal', 'orm'):
            shutil.copyfile(args.textures / f'{part}-{kind}.dds', directory / f'{part}-{kind}.dds')
        materials[f'{name}.TGA'] = dict({kind: f'{FOLDER}/{part}-{kind}.dds' for kind in ('albedo', 'normal', 'orm')},
                                        scale=1., antitile='off', antitile_cell=1., foliage=FOLIAGE[part])
    rewrite(work / 'materials/materials.json', materials)

    folder = work / 'meshes' / FOLDER
    folder.mkdir()
    meshes, added, trees = {}, [], []
    mask = tree_density.load_mask(BLENDER / config['atlas']['mask'])
    for number, (position, yaw, scale, seed) in enumerate(PROBE, 1):
        visual = f'{VISUAL}_{number}'
        tree = fir_tree.build(config, seed, scale)
        glb = folder / f'{visual}.glb'
        tree_glb.write(glb, [{k: v for k, v in p.items() if k != 'owner'} for p in tree.parts], visual,
                       extras=dict(tree=config['id'], seed=config['seed'] if seed is None else seed, scale=scale))
        read = tree_check.check(glb)
        fails = list(read['fails'])
        if read['triangles_total'] > config['budget_tris']:
            fails.append(f"{read['triangles_total']} triangles, the budget is {config['budget_tris']}")
        density = tree_density.measure(glb, mask)
        fails += tree_density.rules(density, config['density'], scale)
        if fails:
            raise SystemExit(f'{visual}: {fails}')
        meshes[visual] = dict(glb=f'{FOLDER}/{glb.name}')
        added.append(dict(key=f'fir-probe-{number}', kind='mesh', visual=visual, position=list(position),
                          yaw_deg=yaw, collision='none'))
        trees.append(dict(visual=visual, seed=config['seed'] if seed is None else seed, scale=scale, yaw_deg=yaw,
                          position_cm=list(position), triangles=read['triangles_total'],
                          height_m=tree.metrics['height_m'], crown_width_m=density['crown_width_m'],
                          background_share=density['background_share'],
                          trunk_open_length_share=density['trunk_open_length_share'], bytes=glb.stat().st_size))
    rewrite(work / 'meshes/meshes.json', meshes)
    layer = work / 'worlds' / WORLD / 'vobs.d/fir-probe.json'
    layer.write_text(json.dumps(dict(format=1, add=added), indent=1) + '\n')
    report = dict(set=args.set, source=args.source, trees=trees,
                  textures=json.loads((args.textures / 'textures.json').read_text()), materials=materials)
    (work / 'fir-probe.json').write_text(json.dumps(report, indent=1) + '\n')
    work.rename(output)
    print(json.dumps(dict(set=str(output), trees=trees), indent=1))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--config', type=Path, default=BLENDER / 'fir.json')
    commands = parser.add_subparsers(dest='command', required=True)
    command = commands.add_parser('textures')
    command.add_argument('--boughs', type=Path, required=True)
    command.add_argument('--bark', type=Path, required=True)
    command.add_argument('--out', type=Path, required=True)
    command = commands.add_parser('probe')
    command.add_argument('set')
    command.add_argument('--textures', type=Path, required=True)
    command.add_argument('--from', dest='source', default='working-28')
    args = parser.parse_args()
    return textures(args) if args.command == 'textures' else probe(args)


if __name__ == '__main__':
    raise SystemExit(main())
