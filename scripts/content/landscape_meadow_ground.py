#!/usr/bin/env python3
"""Build a staging set with the meadow's own ground: a sward under the grass, earth where it is bare.

    uv run --no-project --with numpy --with pillow --with scipy python \
        scripts/content/landscape_meadow_ground.py meadow-ground-1 [--from meadow-grass-1] [--again] \
        [--sward-size 2048] [--rebake] [--preview DIR]

reports/engine/2026-11-14-meadow-ground. Copies a set with the meadow's grass, gives the meadow's texture the
`ground` field with two layers and `grass.fine`, and the road's textures the `verge` field. Both layers are baked in
Blender from scans seen straight down (mesh3d/blender/meadow_patch.py; once, into the work folder), then toned here:

- sward: the low growth and the thatch over the soil. Colour times the shadow between the blades (the share of the
  sky times a root-to-tip curve, as landscape_grass_layer.py), the soil under them darker; patches wider than half
  a metre are levelled, so the tile has nothing to find again; the mean is brought to the meadow's painted ground,
  times the on-screen correction SWARD_SCREEN;
- earth: the soil of the scan with its green sprouts turned to straw and its tone drawn to the road's earth, the
  pebbles greyer and lighter than it, hollows darker; the mean is brought to the earth of the road's texture, times
  EARTH_SCREEN: a bare spot and a rut read as one soil;
- in the blue of both ORMs the height, ranked per mip (landscape_fringe.write_orm): the shader lets the higher
  layer win where the cover is between none and whole.

No pixel of the original goes into a map: its textures are read for mean colours only.
`--again` rebuilds the maps of a set this script made (it holds `meadow-ground.json`); no other set is written to.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import shutil
import subprocess
from pathlib import Path

import numpy as np
from scipy.ndimage import gaussian_filter, maximum_filter

HERE = Path(__file__).parent
SOURCES = Path.home() / '.local/share/gothic-ai/content-sources/landscape-probe'
STAGING = Path.home() / '.local/share/gothic-ai/content-staging'
TUFTS, ROCKS, GROUND = 'grass_medium_01', 'rock_moss_set_01', 'sparse_grass'   # Poly Haven, CC0
MEADOW = 'NW_NATURE_WALDBODEN_03'
ROADS = ('NW_NATURE_PATH_06', 'NW_NATURE_PATH_06_TCROSS', 'NW_NATURE_PATH_06_END', 'NW_NATURE_PATH_05',
         'NW_NATURE_PATH_05_TO_06')
ROAD_EARTH = (.2, .8)       # the share of the road texture's width that is earth: its ruts and the ridge between
SWARD_TILE_M, EARTH_TILE_M = 4., 2.
SWARD_SIZE, EARTH_SIZE = 2048, 2048
FAR = 5.3                   # the sward's far repeat: 21.2 m, no multiple of the near one
REACH_CM, MARGIN_CM = 90, 50
SOIL_SHADE = .5             # the soil under the growth against the growth's own albedo
CANOPY_M = .035             # the root-to-tip curve of the sward: against the tallest growth this near
CURVE, FLOOR = 1.3, .35
LEVEL_M, LEVEL = .5, .75    # patches wider than this are levelled by this share
SWARD_UP, EARTH_UP = .3, .0     # share of the plane's normal in the layer's
AO_POWER = .3
SWARD_STEMS = .55          # the thatch against its baked colour: pale straw, but no glitter over the growth
EARTH_LEVEL = .6
ROUGH_FLOOR = .85
SOIL_TO_ROAD = .65          # the soil's chroma drawn to the road's earth by this share
STONE_GREY = .2             # a pebble's chroma drawn to grey by this share
STONE_LIGHT = 1.9           # and its luma against the soil's
# On-screen corrections of the mean colour, by frames of the game (scripts/grass_look.py tone), linear RGB.
SWARD_SCREEN = (1., 1., 1.)
EARTH_SCREEN = (.62, .60, .55)   # a lit layer comes out lighter than the road's paint beside it: down to the road's
SWARD_CONTRAST = 1.
EARTH_CONTRAST = 1.4


def sibling(name: str):
    path = HERE / name
    spec = importlib.util.spec_from_file_location(path.stem.replace('-', '_'), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def luma(rgb: np.ndarray) -> np.ndarray:
    return rgb @ np.array((.2126, .7152, .0722))


def bake(layer: str, folder: Path, size: int, tile: float, seed: int) -> None:
    blender = os.environ.get('BLENDER') or shutil.which('blender') or '/Applications/Blender.app/Contents/MacOS/Blender'
    subprocess.run([blender, '-b', '-P', str(HERE / 'mesh3d/blender/meadow_patch.py'), '--', '--layer', layer,
                    '--tufts', str(SOURCES / TUFTS / f'{TUFTS}_4k.gltf'), '--rocks', str(SOURCES / ROCKS / f'{ROCKS}_4k.gltf'),
                    '--ground', str(SOURCES / GROUND), '--out', str(folder), '--size', str(size), '--tile', str(tile),
                    '--seed', str(seed)], check=True, stdout=subprocess.DEVNULL)


def baked(layer: str, folder: Path, size: int, tile: float, seed: int, again: bool) -> dict:
    if again or not (folder / 'bake.npz').exists():
        bake(layer, folder, size, tile, seed)
    patch = json.loads((folder / 'patch.json').read_text())
    if (patch['layer'], patch['size'], patch['tile_m'], patch['seed']) != (layer, size, tile, seed):
        raise SystemExit(f'{folder}: baked as {patch["layer"]} at {patch["size"]} px, {patch["tile_m"]} m, '
                         f'seed {patch["seed"]}; --rebake')
    passes = {key: value.astype(np.float32) for key, value in np.load(folder / 'bake.npz').items()}
    kind = np.clip(passes.pop('kind'), 0, 1)
    passes.update(growth=kind[..., 0], stone=kind[..., 1], stem=kind[..., 2], soil=1 - np.clip(kind.sum(axis=2), 0, 1),
                  patch=patch)
    return passes


def original_means(pilot) -> tuple[np.ndarray, np.ndarray]:
    """Mean linear RGB of the meadow's painted ground and of the earth of the road's texture."""
    from PIL import Image
    base = sorted(Path.home().glob('.cache/gothic-ai/base/*/textures'), key=lambda p: p.stat().st_mtime)[-1]

    def linear(texture):
        with Image.open(base / f'{texture}.dds') as image:
            return pilot.linear(np.asarray(image.convert('RGB')))
    road = linear(ROADS[0])
    width = road.shape[1]
    earth = road[:, int(ROAD_EARTH[0] * width):int(ROAD_EARTH[1] * width)]
    return linear(MEADOW).reshape(-1, 3).mean(axis=0), earth.reshape(-1, 3).mean(axis=0)


def soil(colour: np.ndarray, road: np.ndarray) -> np.ndarray:
    """The scan's soil without its green: sprouts to straw, the chroma drawn to the road's earth."""
    level = np.maximum(luma(colour), 1e-5)[..., None]
    chroma = colour / level
    sprout = np.clip((colour[..., 1] / np.maximum(colour[..., 0], 1e-5) - .8) / .25, 0, 1)[..., None]
    straw = np.array((1.5, 1.28, .6))
    straw /= luma(straw)
    earth = road / luma(road)
    chroma = chroma * (1 - SOIL_TO_ROAD) + earth * SOIL_TO_ROAD
    return level * (chroma * (1 - sprout) + straw * 1.25 * sprout)


def spread(value: np.ndarray, exponent: float) -> np.ndarray:
    level = np.maximum(luma(value), 1e-5)
    middle = np.exp(np.log(level).mean())
    return value * ((level / middle) ** (exponent - 1))[..., None]


def to_mean(value: np.ndarray, target: np.ndarray, screen) -> tuple[np.ndarray, np.ndarray]:
    gains = target * np.array(screen) / value.reshape(-1, 3).mean(axis=0)
    return np.clip(value * gains, 0, 1), gains


def sward_maps(passes: dict, tile: float, meadow: np.ndarray, road: np.ndarray, screen, contrast) -> dict:
    texel = tile / passes['height'].shape[0]
    height, light = passes['height'], np.clip(passes['light'], 0, 1)
    reach = max(int(CANOPY_M / texel), 1)
    canopy = gaussian_filter(maximum_filter(height, size=reach, mode='wrap'), reach / 2, mode='wrap')
    share = np.clip(height / (canopy + .008), 0, 1)
    shade = light * (FLOOR + (1 - FLOOR) * share ** CURVE)
    earth = passes['soil'][..., None]
    colour = passes['colour'] * (1 - earth) + soil(passes['colour'], road) * SOIL_SHADE * earth
    colour = colour * (1 - (1 - SWARD_STEMS) * passes['stem'][..., None])
    value = spread(colour * shade[..., None], contrast)
    low = gaussian_filter(luma(value), LEVEL_M / texel, mode='wrap')
    value = value / ((low / low.mean()) ** LEVEL)[..., None]
    value, gains = to_mean(value, meadow, screen)
    normal = passes['normal'] * (1 - SWARD_UP) + np.array((0, 0, SWARD_UP))
    rough = ROUGH_FLOOR + (1 - ROUGH_FLOOR) * np.clip(passes['rough'], 0, 1)
    # the height the layers are told apart by: the canopy, so a tuft is an island and not its blades one by one
    orm = np.stack([light ** AO_POWER, rough, canopy], axis=2)
    return dict(albedo=value, normal=normal, orm=orm, gains=gains, soil_share=float(earth.mean()),
                growth_share=float(passes['growth'].mean()), stem_share=float(passes['stem'].mean()))


def earth_maps(passes: dict, tile: float, road: np.ndarray, screen, contrast) -> dict:
    texel = tile / passes['height'].shape[0]
    height, light = passes['height'], np.clip(passes['light'], 0, 1)
    earth, stone = passes['soil'][..., None], passes['stone'][..., None]
    ground = soil(passes['colour'], road)
    ground_luma = float((luma(ground) * earth[..., 0]).sum() / max(earth.sum(), 1))
    rock = luma(passes['colour'])
    rock_mean = float((rock * stone[..., 0]).sum() / max(stone.sum(), 1e-6))
    tint = road / luma(road) * (1 - STONE_GREY) + STONE_GREY
    pebble = (rock / max(rock_mean, 1e-6))[..., None] ** .7 * ground_luma * STONE_LIGHT * tint
    colour = ground * earth + pebble * stone + passes['colour'] * np.clip(1 - earth - stone, 0, 1)
    # hollows of the soil darker, its crumbs lighter: the scan's relief against its surroundings of 1.5 cm
    hollow = np.clip(1 + (height - gaussian_filter(height, .015 / texel, mode='wrap')) / .004 * .12, .75, 1.15)
    shade = light * np.where(earth[..., 0] > .5, hollow, 1.)
    value = spread(colour * shade[..., None], contrast)
    low = gaussian_filter(luma(value), LEVEL_M / texel, mode='wrap')
    value = value / ((low / low.mean()) ** EARTH_LEVEL)[..., None]
    value, gains = to_mean(value, road, screen)
    normal = passes['normal'] * (1 - EARTH_UP) + np.array((0, 0, EARTH_UP))
    rough = ROUGH_FLOOR + (1 - ROUGH_FLOOR) * np.clip(passes['rough'], 0, 1)
    # a pebble's height is its own over the plane, the soil's is the scan's relief: the pebbles on top of it
    orm = np.stack([(light * np.clip(hollow, 0, 1)) ** AO_POWER, rough, height + .02 * stone[..., 0]], axis=2)
    return dict(albedo=value, normal=normal, orm=orm, gains=gains, stone_share=float(stone.mean()),
                stem_share=float(passes['stem'].mean() + passes['growth'].mean()))


def halved(maps: dict) -> dict:
    """The maps at half the size: colour in linear light, the normal's length restored by the writer."""
    def half(image):
        size = image.shape[0] // 2
        return image.reshape(size, 2, size, 2, -1).mean(axis=(1, 3))
    return dict(maps, albedo=half(maps['albedo']), normal=half(maps['normal']), orm=half(maps['orm']))


def write_maps(directory: Path, maps: dict, dds, pilot, layer, fringe) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    layer.write_albedo(dds, pilot, directory / 'albedo.dds', maps['albedo'])
    normal = maps['normal'].astype(np.float32).copy()
    normal[..., 1] *= -1     # +Y up the image; the engine's bitangent follows texture v down
    dds.write_dds(directory / 'normal.dds', normal, normal=True)
    orm = maps['orm'].copy()
    orm[..., :2] *= 255
    fringe.write_orm(dds, directory / 'orm.dds', orm)


def preview(folder: Path, name: str, albedo: np.ndarray, tile: float, pilot) -> None:
    """The tile whole, a square metre of it texel for texel, and 12 x 8 m of repeats at 64 px a metre."""
    from PIL import Image
    folder.mkdir(parents=True, exist_ok=True)
    size = albedo.shape[0]

    def small(side):
        factor = size // side
        return albedo.reshape(side, factor, side, factor, 3).mean(axis=(1, 3))

    metre = int(size / tile)
    Image.fromarray(pilot.encode(small(min(size, 1024)))).save(folder / f'{name}-tile.png')
    Image.fromarray(pilot.encode(albedo[:metre, :metre])).save(folder / f'{name}-near.png')
    far = small(int(tile * 64))
    Image.fromarray(pilot.encode(np.tile(far, (int(8 / tile), int(12 / tile), 1)))).save(folder / f'{name}-far.png')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('set')
    parser.add_argument('--from', dest='source', default='meadow-grass-1',
                        help='staging set with the meadow\'s grass to copy (default: %(default)s)')
    parser.add_argument('--again', action='store_true', help='rebuild the maps of a set this script made')
    parser.add_argument('--work', type=Path, default=SOURCES / 'work', help='folder of the bakes')
    parser.add_argument('--rebake', action='store_true')
    parser.add_argument('--bake-size', type=int, default=4096, help='pixels of the sward\'s bake')
    parser.add_argument('--sward-size', type=int, default=SWARD_SIZE, help='pixels of the sward\'s maps')
    parser.add_argument("--earth-size", type=int, default=EARTH_SIZE, help="pixels of the earth's bake and maps")
    parser.add_argument("--seed", type=int, default=11)
    parser.add_argument('--sward-screen', type=float, nargs=3, default=SWARD_SCREEN, metavar=('R', 'G', 'B'))
    parser.add_argument('--earth-screen', type=float, nargs=3, default=EARTH_SCREEN, metavar=('R', 'G', 'B'))
    parser.add_argument('--sward-contrast', type=float, default=SWARD_CONTRAST)
    parser.add_argument('--earth-contrast', type=float, default=EARTH_CONTRAST)
    parser.add_argument('--preview', type=Path, help='write previews of the tiles here and stop: no set')
    args = parser.parse_args()

    source, output = STAGING / args.source, STAGING / args.set
    mine = (output / 'meadow-ground.json').exists()
    if output.exists() and not args.preview and not (args.again and mine):
        raise SystemExit(f'{output}: staging set exists; choose a new set name, or --again for a set made here')
    index = json.loads(((output if mine else source) / 'materials/materials.json').read_text())
    if 'grass' not in index.get(f'{MEADOW}.TGA', {}):
        raise SystemExit(f'{source}: {MEADOW} has no grass; the ground needs it')
    pilot, dds = sibling('megascans_pilot.py'), sibling('test-material.py')
    layer, fringe = sibling('landscape_grass_layer.py'), sibling('landscape_fringe.py')

    meadow, road = original_means(pilot)
    sward = sward_maps(baked('sward', args.work / 'meadow-sward', args.bake_size, SWARD_TILE_M, args.seed, args.rebake),
                       SWARD_TILE_M, meadow, road, args.sward_screen, args.sward_contrast)
    while sward['albedo'].shape[0] > args.sward_size:
        sward = halved(sward)
    earth = earth_maps(baked('earth', args.work / 'meadow-earth', args.earth_size, EARTH_TILE_M, args.seed, args.rebake),
                       EARTH_TILE_M, road, args.earth_screen, args.earth_contrast)

    def summary(maps, tile):
        value = maps['albedo']
        return {key: (round(item, 4) if isinstance(item, float) else item) for key, item in dict(
            maps, size=value.shape[0], tile_m=tile, mean=value.reshape(-1, 3).mean(axis=0).round(4).tolist(),
            luma_std=round(float(luma(value).std() / luma(value).mean()), 3), gains=maps['gains'].round(3).tolist()).items()
            if key not in ('albedo', 'normal', 'orm')}
    report = dict(source=args.source, tufts=TUFTS, rocks=ROCKS, ground=GROUND, seed=args.seed,
                  painted=dict(meadow=meadow.round(4).tolist(), road_earth=road.round(4).tolist()),
                  sward=dict(summary(sward, SWARD_TILE_M), screen=list(args.sward_screen), contrast=args.sward_contrast),
                  earth=dict(summary(earth, EARTH_TILE_M), screen=list(args.earth_screen), contrast=args.earth_contrast),
                  far=FAR, verge=dict(roads=list(ROADS), reach=REACH_CM, margin=MARGIN_CM))
    if args.preview:
        preview(args.preview, 'sward', sward['albedo'], SWARD_TILE_M, pilot)
        preview(args.preview, 'earth', earth['albedo'], EARTH_TILE_M, pilot)
        print(json.dumps(report, indent=2))
        return 0

    if not mine:
        shutil.copytree(source, output)
    write_maps(output / 'materials/meadow_sward', sward, dds, pilot, layer, fringe)
    write_maps(output / 'materials/meadow_earth', earth, dds, pilot, layer, fringe)

    def maps(folder, tile):
        return dict(albedo=f'{folder}/albedo.dds', normal=f'{folder}/normal.dds', orm=f'{folder}/orm.dds', tile_m=tile)
    record = index[f'{MEADOW}.TGA']
    record['grass'] = dict(record['grass'], fine=1)
    record['ground'] = dict(sward=maps('meadow_sward', SWARD_TILE_M), earth=maps('meadow_earth', EARTH_TILE_M), far=FAR)
    for texture in ROADS:
        index[f'{texture}.TGA'] = dict(index.get(f'{texture}.TGA', {}),
                                       verge=dict(of=f'{MEADOW}.TGA', reach=REACH_CM, margin=MARGIN_CM))
    (output / 'materials/materials.json').write_text(json.dumps(index, indent=2) + '\n')
    (output / 'meadow-ground.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
