#!/usr/bin/env python3
"""Pack the spruce's baked sheets into DDS and build the probe set: five spruces on the pilot's forest edge.

    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/fir_set.py \
        textures --boughs DIR --bark DIR --out WORK
    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/fir_set.py \
        probe fir-probe-3 --textures WORK [--from working-28] [--pairs | --sides flip]
    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/fir_set.py \
        forest tree-cost-1024 --textures WORK [--from working-34] [--count 1024] [--share 1] [--no-sample]

`textures` reads what mesh3d/blender/fir_boughs.py and fir_bark.py baked (boughs.npz, bark.npz) and writes
WORK/{boughs,bark}-{albedo,normal,orm}.dds, PNG previews for mesh3d/blender/fir_views.py, boughs-mask.npz (the
atlas's coverage for mesh3d/blender/tree_density.py; its copy in git is mesh3d/blender/fir-boughs-mask.npz) and
textures.json:

- albedo is the baked colour times the baked occlusion (the engine's map AO dims only the sky's light, so the
  shade between the shoots and in the cracks of the bark has to be in the colour), then brought per channel to
  the palette of mesh3d/blender/fir.json: the mean of the needles, and of the bark above the foot. The palette
  comes from the scene measurement of reports/content/2026-11-trees/fir/README.md, not from the concept;
- the boughs' lights are held down (LIGHT_KNEE, LIGHT_SLOPE): the shoots on top of a bough are in the sun in
  the game as well, and a sheet whose lightest tenth is twice its mean gave a crown with yellow-green sparks
  where the scene has dark needles. textures.json says the ratio that is left (light_to_mean);
- the bough atlas keeps coverage in alpha and every mip averages it (glb_cards.write_cutout); its normals are
  the slopes of the baked height, smoothed over RELIEF_BLUR pixels, so they follow the round of a shoot and the
  step from a shoot to the one under it, not single needles, which would sparkle; no slope is steeper than
  RELIEF_MAX degrees;
- normals have +Y up the picture and are written for the engine, whose bitangent follows V down.

`probe` copies a working set by hard links into ~/.local/share/gothic-ai/remaster/trees-fir/NAME and adds, in
new files only, the two materials, PROBE spruces built by mesh3d/blender/fir_tree.py (the vob layer has no scale,
so every size is its own GLB; each passes the checks of mesh3d/blender/fir.py) and the layer
worlds/NEWWORLD/vobs.d/fir-probe.json: meshes without collision beside the original's firs at the north edge of
the meadow east of the tavern. Both materials say `"foliage": {"anchor": "tree"}` (FOLIAGE; the engine's rule is
reports/engine/2026-11-16-modern-trees/tree-wind.md): the bark and the boughs bend as one body, so their `wind`
is the same, and only the boughs flutter and let the light through. A bough's triangles are listed once and the
boughs' record says `"two_sided": "same"`: the engine draws both sides and lights them by the card's normals, as
it lit the two copies before (reports/engine/2026-11-16-modern-trees/two-sided.md). `--sides flip` writes the
other value of the field, the side away from the winding lit by the normal turned to the eye; `--pairs` lists
the triangles twice under a one-sided record, as fir-probe-2 has them.

`forest` builds a set of the cost stand (reports/engine/2026-11-16-modern-trees/tree-cost.md) the same way: the
five spruces of PROBE, thinned to `--share` of their triangles by mesh3d/blender/fir_tree.py thin() when below 1,
stand in a grid of `--count` (forest_layout(), X0 and the next constants) on the ground of the base's world mesh,
and spruce A alone on the hill west of it (LONE) unless `--no-sample`. The layer is
worlds/NEWWORLD/vobs.d/tree-cost.json, the numbers of the spruces are in tree-cost.json of the set. `--count 0`
is the control: the same set without the forest. The spruces of `--share 1` are those of `probe`, byte for byte.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import math
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
AO_NEEDLES, AO_BARK = .85, .7   # share of the baked occlusion that goes into the colour
LIGHT_KNEE, LIGHT_SLOPE = 1.15, .4      # needles lighter than KNEE means of the sheet keep SLOPE of the excess
RELIEF_BLUR, RELIEF, RELIEF_MAX = 3., .6, 50.       # pixels; share of the height's slope kept; degrees
LUMA = np.array((.2126, .7152, .0722), np.float32)
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

# The cost stand (tree-cost mode, reports/engine/2026-11-16-modern-trees/tree-cost.md): a grid of SIDE x SIDE spruces,
# STEP apart (the original's forest has 6.6 m to the nearest neighbour), each shifted up to JITTER, east of the
# meadow of PROBE and of the rock hill east of it. Column i runs east from X0, row j north from Z0; a forest of
# n spruces is the block of the first sqrt(n) columns and the middle sqrt(n) rows, so 64, 256 and 1 024 nest at
# the middle of the west edge, where scripts/bench/perf-trees.bench looks from. Each place takes one of the five
# spruces of PROBE and a yaw by FOREST_SEED, the same in every forest. LONE: spruce A alone on the top of the hill,
# west of the forest, for the pairs of frames at distances; the hill hides the forest behind it from the west.
X0, Z0, STEP, SIDE, JITTER = 51500., -16060., 660., 32, 150.
FOREST_SEED = 20261118
LONE = (48500., 500.)
GROUND = ('ground', 'rock')     # the material classes a root stands on


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


def relief(height: np.ndarray, alpha: np.ndarray, px_per_m: float) -> np.ndarray:
    """Normals of the baked height: x to the right, y up the picture, z out of the sheet."""
    from scipy import ndimage
    weight = ndimage.gaussian_filter(alpha, RELIEF_BLUR)
    smooth = ndimage.gaussian_filter(height * alpha, RELIEF_BLUR) / np.maximum(weight, 1e-4)
    down, right = np.gradient(smooth)
    slope = np.stack([-right, down], axis=2) * (px_per_m * RELIEF)
    steep = np.linalg.norm(slope, axis=2, keepdims=True)
    slope *= np.minimum(1., np.tan(np.radians(RELIEF_MAX)) / np.maximum(steep, 1e-6))
    normal = np.concatenate([slope, np.ones_like(steep)], axis=2)
    return (normal / np.linalg.norm(normal, axis=2, keepdims=True)).astype(np.float32)


def textures(args) -> int:
    cards, dds, pilot = sibling('glb_cards.py'), sibling('test-material.py'), sibling('megascans_pilot.py')
    config = json.loads(args.config.read_text())
    palette = config['palette']
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

    def mean_of(image: np.ndarray) -> np.ndarray:
        return (image * alpha[..., None]).sum(axis=(0, 1), dtype=np.float64) / alpha.sum(dtype=np.float64)

    light = albedo @ LUMA
    knee = LIGHT_KNEE * float(mean_of(light[..., None])[0])
    albedo *= np.where(light > knee, (knee + (light - knee) * LIGHT_SLOPE) / np.maximum(light, 1e-9), 1.)[..., None]
    mean = mean_of(albedo)
    gain = target('needles_srgb') / mean
    albedo = np.clip(albedo * gain, 0, 1).astype(np.float32)
    solid = alpha > .5
    light = (albedo @ LUMA)[solid]
    lightest = albedo[solid][light >= np.quantile(light, .9)].mean(axis=0, dtype=np.float64)
    coverage = cards.write_cutout(dds, pilot.encode, out / 'boughs-albedo.dds', albedo, alpha)
    normal = relief(data['height'].astype(np.float32), alpha, config['atlas']['px_per_m'])
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
                            baked_mean_srgb=srgb(mean), gain=gain.round(3).tolist(), mean_srgb=srgb(mean_of(albedo)),
                            lightest_tenth_srgb=srgb(lightest),
                            light_to_mean=(lightest / albedo[solid].mean(axis=0, dtype=np.float64)).round(2).tolist())

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


def stage(source_name: str, name: str) -> tuple[Path, Path]:
    """A hard-linked copy of a working set under a temporary name: (the copy, the final path)."""
    source, output = STAGING / source_name, PROBES / name
    if output.exists():
        raise SystemExit(f'{output}: the set exists; choose a new name')
    if not (source / 'materials/materials.json').exists():
        raise SystemExit(f'{source}: no such working set')
    work = output.with_name(output.name + '.tmp')
    if work.exists():
        shutil.rmtree(work)
    work.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source, work, copy_function=os.link)
    return work, output


def add_materials(work: Path, config: dict, textures: Path, pairs: bool = False, sides: str = 'same') -> dict:
    directory = work / 'materials' / FOLDER
    directory.mkdir()
    materials = {}
    for part, name in config['materials'].items():
        for kind in ('albedo', 'normal', 'orm'):
            shutil.copyfile(textures / f'{part}-{kind}.dds', directory / f'{part}-{kind}.dds')
        materials[f'{name}.TGA'] = dict({kind: f'{FOLDER}/{part}-{kind}.dds' for kind in ('albedo', 'normal', 'orm')},
                                        scale=1., antitile='off', antitile_cell=1., foliage=FOLIAGE[part])
        if part == 'boughs' and not pairs:
            materials[f'{name}.TGA']['two_sided'] = sides
    rewrite(work / 'materials/materials.json', materials)
    return materials


def probe(args) -> int:
    sys.path.insert(0, str(BLENDER))
    import fir_tree
    import tree_check
    import tree_density
    import tree_glb
    config = json.loads(args.config.read_text())
    work, output = stage(args.source, args.set)
    materials = add_materials(work, config, args.textures, args.pairs, args.sides)

    folder = work / 'meshes' / FOLDER
    folder.mkdir()
    meshes, added, trees = {}, [], []
    mask = tree_density.load_mask(BLENDER / config['atlas']['mask'])
    for number, (position, yaw, scale, seed) in enumerate(PROBE, 1):
        visual = f'{VISUAL}_{number}'
        tree = fir_tree.build(config, seed, scale, args.pairs)
        glb = folder / f'{visual}.glb'
        tree_glb.write(glb, [{k: v for k, v in p.items() if k != 'owner'} for p in tree.parts], visual,
                       extras=dict(tree=config['id'], seed=config['seed'] if seed is None else seed, scale=scale))
        read = tree_check.check(glb)
        fails = list(read['fails'])
        drawn = read['triangles_total'] - read['cards']['pairs']
        if drawn > config['budget_tris']:
            fails.append(f"{drawn} triangles without the twins, the budget is {config['budget_tris']}")
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


def forest_layout(count: int) -> list[dict]:
    """The places of a forest of `count` spruces, a square of an even number: column, row, x and z in Gothic
    centimetres, yaw, and the number of the spruce of PROBE (1 to 5)."""
    side = math.isqrt(count)
    if side * side != count or side % 2 or side > SIDE:
        raise SystemExit(f'{count}: a forest is a square of an even side up to {SIDE}: 0, 4, 16, 64, 256, 1024')
    rng = np.random.default_rng(FOREST_SEED)
    shift = rng.uniform(-JITTER, JITTER, (SIDE, SIDE, 2))
    yaw = rng.uniform(0., 360., (SIDE, SIDE))
    spruce = rng.integers(1, len(PROBE) + 1, (SIDE, SIDE))
    rows = range((SIDE - side) // 2, (SIDE + side) // 2)
    return [dict(column=i, row=j, x=X0 + (i + .5) * STEP + shift[i, j, 0], z=Z0 + (j + .5) * STEP + shift[i, j, 1],
                 yaw=round(float(yaw[i, j]), 1), spruce=int(spruce[i, j]))
            for i in range(side) for j in rows]


def ground_heights(world: Path, points: np.ndarray, reach: float = 50.) -> np.ndarray:
    """The height of the ground (GROUND classes of the world mesh) under each (x, z): the lowest of the highest
    surface at the point and at four points `reach` cm round it, so a trunk on a slope does not hang over it."""
    sys.path.insert(0, str(HERE / 'world'))
    from landscape_tuft_spots import chunk
    from world_classes import material_class
    offsets = np.array([(0., 0.), (reach, 0.), (-reach, 0.), (0., reach), (0., -reach)])
    probes = (points[:, None, :] + offsets[None]).reshape(-1, 2)
    keys = {(int(math.floor(x / 6400)), int(math.floor(z / 6400))) for x, z in probes}
    triangles = np.concatenate([position for cx, cz in sorted(keys) if (world / 'chunks' / f'{cx}_{cz}.glb').exists()
                                for name, group, position, _, _ in chunk(world / 'chunks' / f'{cx}_{cz}.glb')
                                if material_class(name, group) in GROUND])
    a, b, c = triangles[:, 0], triangles[:, 1], triangles[:, 2]
    low, high = triangles[..., [0, 2]].min(axis=1), triangles[..., [0, 2]].max(axis=1)
    height = np.full(len(probes), np.nan)
    for k, (x, z) in enumerate(probes):
        near = np.flatnonzero((low[:, 0] <= x) & (x <= high[:, 0]) & (low[:, 1] <= z) & (z <= high[:, 1]))
        det = (b[near, 2] - c[near, 2]) * (a[near, 0] - c[near, 0]) + (c[near, 0] - b[near, 0]) * (a[near, 2] - c[near, 2])
        ok = np.abs(det) > 1e-9
        near, det = near[ok], det[ok]
        l0 = ((b[near, 2] - c[near, 2]) * (x - c[near, 0]) + (c[near, 0] - b[near, 0]) * (z - c[near, 2])) / det
        l1 = ((c[near, 2] - a[near, 2]) * (x - c[near, 0]) + (a[near, 0] - c[near, 0]) * (z - c[near, 2])) / det
        inside = (l0 >= -1e-6) & (l1 >= -1e-6) & (1 - l0 - l1 >= -1e-6)
        if inside.any():
            y = l0 * a[near, 1] + l1 * b[near, 1] + (1 - l0 - l1) * c[near, 1]
            height[k] = y[inside].max()
    height = height.reshape(len(points), len(offsets))
    if np.isnan(height[:, 0]).any():
        raise SystemExit(f'no ground under {points[np.isnan(height[:, 0])][:5].tolist()}')
    return np.nanmin(height, axis=1)


def newest_base() -> Path:
    bases = [p.parent for p in Path.home().glob('.cache/gothic-ai/base/*/worlds') if (p / WORLD / 'chunks').is_dir()]
    if not bases:
        raise SystemExit('no base with world chunks in ~/.cache/gothic-ai/base; pass --base')
    return max(bases, key=lambda p: (p / 'worlds' / WORLD / 'chunks').stat().st_mtime)


def forest(args) -> int:
    """The cost stand: a forest of `count` spruces of PROBE thinned to `share` of their triangles, and LONE."""
    sys.path.insert(0, str(BLENDER))
    import fir_tree
    import tree_check
    import tree_density
    import tree_glb
    config = json.loads(args.config.read_text())
    layout = forest_layout(args.count)
    base = args.base or newest_base()
    points = np.array([(t['x'], t['z']) for t in layout] + [LONE])
    height = ground_heights(base / 'worlds' / WORLD, points)
    work, output = stage(args.source, args.set)
    materials = add_materials(work, config, args.textures)

    folder = work / 'meshes' / FOLDER
    folder.mkdir()
    meshes, spruces = {}, []
    mask = tree_density.load_mask(BLENDER / config['atlas']['mask'])
    for number, (_, _, scale, seed) in enumerate(PROBE, 1):
        visual = f'{VISUAL}_{number}'
        full = fir_tree.build(config, seed, scale)
        tree = fir_tree.thin(full, args.share, number)
        glb = folder / f'{visual}.glb'
        tree_glb.write(glb, [{k: v for k, v in p.items() if k != 'owner'} for p in tree.parts], visual,
                       extras=dict(tree=config['id'], seed=config['seed'] if seed is None else seed, scale=scale,
                                   share=args.share))
        read = tree_check.check(glb)
        density = tree_density.measure(glb, mask)
        # a thinned spruce is a stand for the cost, not a tree whose look is checked: its fails are reported
        spruces.append(dict(visual=visual, seed=config['seed'] if seed is None else seed, scale=scale,
                            triangles=read['triangles_total'], triangles_full=full.metrics['triangles'],
                            triangles_by_material=read['triangles'], thin=tree.metrics.get('thin'),
                            height_m=round(float(tree.metrics['bbox_max_m'][2]), 3),
                            crown_width_m=density['crown_width_m'], background_share=density['background_share'],
                            check_fails=read['fails'] + tree_density.rules(density, config['density'], scale),
                            bytes=glb.stat().st_size))
        meshes[visual] = dict(glb=f'{FOLDER}/{glb.name}')
    rewrite(work / 'meshes/meshes.json', meshes)
    added = [dict(key=f'tree-cost-{t["column"]}-{t["row"]}', kind='mesh', visual=f'{VISUAL}_{t["spruce"]}',
                  position=[round(t['x'], 1), round(float(y), 1), round(t['z'], 1)], yaw_deg=t['yaw'], collision='none')
             for t, y in zip(layout, height)]
    if args.sample:
        added.append(dict(key='tree-cost-lone', kind='mesh', visual=f'{VISUAL}_4',
                          position=[LONE[0], round(float(height[-1]), 1), LONE[1]], yaw_deg=0., collision='none'))
    layer = work / 'worlds' / WORLD / 'vobs.d/tree-cost.json'
    layer.write_text(json.dumps(dict(format=1, add=added), indent=1) + '\n')
    per_spruce = {s['visual']: s['triangles'] for s in spruces}
    report = dict(set=args.set, source=args.source, base=base.name, share=args.share, count=args.count,
                  sample=args.sample, triangles=sum(per_spruce[a['visual']] for a in added), spruces=spruces,
                  materials=materials, grid=dict(x0=X0, z0=Z0, step=STEP, side=SIDE, jitter=JITTER, seed=FOREST_SEED),
                  lone=dict(position=[LONE[0], round(float(height[-1]), 1), LONE[1]]))
    (work / 'tree-cost.json').write_text(json.dumps(report, indent=1) + '\n')
    work.rename(output)
    print(json.dumps({k: v for k, v in report.items() if k != 'materials'}, indent=1))
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
    command.add_argument('--pairs', action='store_true',
                         help="a bough's triangles twice under a one-sided record, as before two-sided materials")
    command.add_argument('--sides', choices=('same', 'flip'), default='same',
                         help='the value of "two_sided" in the record of the boughs')
    command = commands.add_parser('forest', help='the cost stand: a forest of spruces, maybe thinned')
    command.add_argument('set')
    command.add_argument('--textures', type=Path, required=True,
                         help='a folder with {boughs,bark}-{albedo,normal,orm}.dds: textures, or materials/trees_fir '
                              'of a probe set')
    command.add_argument('--from', dest='source', default='working-34')
    command.add_argument('--count', type=int, default=1024, help='0, 64, 256 or 1024 spruces')
    command.add_argument('--share', type=float, default=1., help="the share of a spruce's triangles kept (thin)")
    command.add_argument('--no-sample', dest='sample', action='store_false', help='without the lone spruce A')
    command.add_argument('--base', type=Path, help='the base the ground is read from (the newest in the cache)')
    args = parser.parse_args()
    return dict(textures=textures, probe=probe, forest=forest)[args.command](args)


if __name__ == '__main__':
    raise SystemExit(main())
