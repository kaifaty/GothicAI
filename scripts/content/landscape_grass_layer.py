#!/usr/bin/env python3
"""Build a staging set whose grass along the rock edges is baked from 3D grass: the `fringe` layer with depth.

    uv run --no-project --with numpy --with pillow --with scipy python \
        scripts/content/landscape_grass_layer.py rock-grass-2 [--from rock-projection-world] \
        [--screen R G B] [--contrast K] [--rough-floor F] [--preview DIR]

Step 1 of reports/engine/2026-11-13-grass/README.md. Copies a set with the projected rock and gives the two strip
textures the `fringe` field, as landscape_fringe.py does, but the layer's maps come from a patch of 3D tufts seen
straight down (mesh3d/blender/grass_patch.py; baked once into the work folder, `--rebake` to bake again):

- colour: the bake's albedo times the shadow between the blades. The engine's AO dims ambient light only, so in the
  sun a flat layer shows no shadow by itself; the painter of the original drew it, and so does this. The shadow is
  the share of the sky a blade sees (the bake's light pass) times a root-to-tip curve, (height / height of the
  canopy around) ** 1.5 over a floor;
- the colour is then brought to the original's painted grass in mean (per channel, times the on-screen correction
  `--screen`) and in spread (`--contrast`: the exponent of luma around its mean), both measured on frames of the
  game by landscape_grass_measure.py;
- the colour's mips are averaged in linear light: the writer of test-material.py averages the encoded values,
  and a texture of bright tips over dark gaps then darkens with distance (by a fifth at 1.5 cm a texel);
- normal: the bake's, leaned toward the plane; ORM: the sky share as AO, weakened (the shadow is in the colour
  already, and in the shade the engine would apply it twice), the scan's roughness, and in the blue the height,
  which the shader compares with the threshold across the tufts (ranked per mip, landscape_fringe). The height
  there is the blades' plus the canopy's with a trail down the image, down a wall: under a tall clump the
  threshold leaves a tongue of grass half a metre long, not a fringe of single blades.

No pixel of the original goes into a map: its painted grass is read for the mean colour only.
`--mask` writes the set the measure needs instead: a flat magenta layer without tufts (height 0), so that the frames
show where the solid band of the layer lies.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import shutil
import struct
import subprocess
from pathlib import Path

import numpy as np

HERE = Path(__file__).parent
SOURCES = Path.home() / '.local/share/gothic-ai/content-sources/landscape-probe'
STAGING = Path.home() / '.local/share/gothic-ai/content-staging'
TUFTS = 'grass_medium_01'   # Poly Haven, CC0: 17 tufts with one atlas
GROUND = 'sparse_grass'     # Poly Haven, CC0: the ground under them
TILE_M = 4.
SIZE = 4096
CURVE = 1.5                 # brightness = height share ** 1.5 (AMD's grass), over FLOOR
FLOOR = .22
CANOPY_M = .12              # the height share is taken against the tallest grass this near
UP = .35                    # share of the plane's normal in the layer's
AO_POWER = .3               # AO = sky share ** .3: on the colour's weight .88 instead of .67
ROUGH_FLOOR = 0.            # the scan's roughness as it is: .70 on average, blades from .44, and they shine
TONGUE_M = .5               # the canopy's trail down the image falls e times over this
TONGUE = .6                 # its weight against the blades' own height
# On-screen correction of the mean and the exponent of the spread, by rounds (2026-11-13), measured on the solid
# band of `ledge` and `road-3` (landscape_grass_measure.py), original against layer, linear display RGB.
# Round 1: 1.03 / 1.04 / 1.02, the last correction of the scanned layer (landscape_fringe.SCREEN) times its
# residue, and exponent 1: it gave the tile, at the original's texel (1 cm), the spread of the original's painted
# grass. Mips averaged encoded, AO = sky share. On the frames original / layer = 1.034 / 1.081 / 0.789, luma
# 1.063, spread 1.040; but 1.02 on `ledge` against 1.15 on `road-3`, and 1.3-1.4 in the shade of both.
# Round 2: linear mips, the weaker AO, less straw in the bake, the tongues; the correction is the hue of round 1
# on `ledge`, the nearer view, 0.961 / 1.022 / 0.725, times .895 for what the mips and the AO were expected to
# add. Exponent 1.06: with the straw gone the tile's spread at 1 cm fell from .75 of its mean (the painted grass
# has .754) to .71, and the exponent returns it. On the frames original / layer = 1.053 / 1.019 / 0.857, luma
# 1.023, spread 1.054; in the shade 1.12-1.18; but luma 0.94 on `ledge` against 1.22 on `road-3`, the blue a
# quarter over the original's on `ledge`, and the lit grass at 17:30 a quarter brighter than the original's.
# The cause is ROUGH_FLOOR, measured: the same set with `--rough-floor .85` (matte blades, as the scanned layer's)
# gives luma 1.14 on `ledge`, 1.29 on `road-3`, 1.22 lit at 17:30, and the blue 1.26, the scanned layer's ratio.
# The round that follows is that floor and the correction times these ratios; it is the user's to order.
SCREEN = (.886, .951, .663)
CONTRAST = 1.06


def sibling(name: str):
    path = HERE / name
    spec = importlib.util.spec_from_file_location(path.stem.replace('-', '_'), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def luma(rgb: np.ndarray) -> np.ndarray:
    return rgb @ np.array((.2126, .7152, .0722))


def bake(folder: Path, size: int, tile: float, seed: int) -> None:
    blender = os.environ.get('BLENDER') or shutil.which('blender') or '/Applications/Blender.app/Contents/MacOS/Blender'
    subprocess.run([blender, '-b', '-P', str(HERE / 'mesh3d/blender/grass_patch.py'), '--',
                    '--source', str(SOURCES / TUFTS / f'{TUFTS}_4k.gltf'), '--ground', str(SOURCES / GROUND),
                    '--out', str(folder), '--size', str(size), '--tile', str(tile), '--seed', str(seed)], check=True)


def shadow(height: np.ndarray, light: np.ndarray, texel_m: float) -> np.ndarray:
    """What the colour is multiplied by: the sky share times the root-to-tip curve."""
    from scipy.ndimage import gaussian_filter, maximum_filter
    reach = max(int(CANOPY_M / texel_m), 1)
    canopy = gaussian_filter(maximum_filter(height, size=reach, mode='wrap'), reach / 2, mode='wrap')
    share = np.clip(height / (canopy + .03), 0, 1)    # 3 cm: bare ground has no tips
    return np.clip(light, 0, 1) * (FLOOR + (1 - FLOOR) * share ** CURVE)


def cut_height(height: np.ndarray, texel_m: float) -> np.ndarray:
    """The height the shader cuts the tufts by: the blades' own plus the canopy with a trail toward the image's
    bottom. The layer lies on a wall with the image's rows down it, so the trail is where long leaves hang."""
    from scipy.ndimage import gaussian_filter, maximum_filter
    reach = max(int(CANOPY_M / texel_m), 1)
    trail = gaussian_filter(maximum_filter(height, size=reach, mode='wrap'), reach / 2, mode='wrap')
    decay = np.exp(-texel_m / TONGUE_M)
    rows = len(trail)
    for row in range(2 * rows):     # twice round the torus
        trail[row % rows] = np.maximum(trail[row % rows], trail[row % rows - 1] * decay)
    return height + TONGUE * trail


def write_albedo(dds, pilot, path: Path, value: np.ndarray) -> None:
    """DXT1 colour from linear values, every mip an average in linear light."""
    size = value.shape[0]
    levels = int(np.log2(size)) + 1
    header = [124, 0xA1007, size, size, (size // 4) ** 2 * 8, 0, levels] + [0] * 11
    header += [32, 4, int.from_bytes(b'DXT1', 'little'), 0, 0, 0, 0, 0, 0x401008, 0, 0, 0, 0]
    with path.open('wb') as output:
        output.write(b'DDS ' + struct.pack('<31I', *header))
        for level in range(levels):
            output.write(dds.bc1(dds.blocks(pilot.encode(value))).tobytes())
            if level + 1 < levels:
                value = dds.downsample(value)


def spread(value: np.ndarray, exponent: float) -> np.ndarray:
    """Luma around its geometric mean to the power `exponent`, hue kept."""
    level = np.maximum(luma(value), 1e-5)
    middle = np.exp(np.log(level).mean())
    return value * ((level / middle) ** (exponent - 1))[..., None]


def painted_stats(probe, pilot) -> dict:
    """Mean and spread of luma of the original's painted grass, at its own texel (about 1 cm)."""
    from PIL import Image
    base = sorted(Path.home().glob('.cache/gothic-ai/base/*/textures'), key=lambda p: p.stat().st_mtime)[-1]
    values = []
    for texture, strip in probe.STRIPS.items():
        with Image.open(base / f'{texture}.dds') as image:
            original = np.asarray(image.convert('RGB'))
        rows = int(original.shape[0] * strip['top'][0])     # the solid band
        values.append(luma(pilot.linear(original[:rows])).ravel())
    values = np.concatenate(values)
    return dict(mean=float(values.mean()), std=float(values.std()))


def preview(folder: Path, albedo: np.ndarray, rank: np.ndarray, pilot) -> None:
    """The tile as the game shows it from afar and near, and a wall with the band cut by the height."""
    from PIL import Image
    folder.mkdir(parents=True, exist_ok=True)
    size = albedo.shape[0]

    def small(image, factor):
        return image.reshape(size // factor, factor, size // factor, factor, -1).mean(axis=(1, 3))

    Image.fromarray(pilot.encode(small(albedo, size // 1024))).save(folder / 'tile.png')      # 256 px/m
    Image.fromarray(pilot.encode(np.tile(small(albedo, size // 256), (2, 3, 1)))).save(folder / 'far.png')   # 64 px/m
    Image.fromarray(pilot.encode(albedo[:1024, :1024])).save(folder / 'near.png')
    # a wall 8 m wide, 3 m of it under the edge: solid for 1 m, tufts for 1.2 m more, 128 px/m
    factor = size // 512
    colour, level = np.tile(small(albedo, factor), (1, 2, 1)), small(rank[..., None], factor)[..., 0]
    order = np.argsort(level, axis=None)
    ranked = np.empty(level.size)
    ranked[order] = np.arange(level.size) / (level.size - 1)
    ranked = np.tile(ranked.reshape(level.shape), (1, 2))[:384]
    depth = (np.arange(384) + .5)[:, None] / 128
    wall = np.where((ranked > (depth - 1.) / 1.2)[..., None], colour[:384], np.array((.11, .1, .085)))
    Image.fromarray(pilot.encode(wall)).save(folder / 'wall.png')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('set')
    parser.add_argument('--from', dest='source', default='rock-projection-world',
                        help='staging set with the projected rock to copy (default: %(default)s)')
    parser.add_argument('--bake', type=Path, default=SOURCES / 'work/grass-layer', help='folder of the bake')
    parser.add_argument('--rebake', action='store_true')
    parser.add_argument('--size', type=int, default=SIZE)
    parser.add_argument('--tile', type=float, default=TILE_M, help='metres per repeat of the grass')
    parser.add_argument('--seed', type=int, default=7)
    parser.add_argument('--screen', type=float, nargs=3, default=SCREEN, metavar=('R', 'G', 'B'),
                        help='on-screen correction of the mean colour, measured on frames of the game')
    parser.add_argument('--contrast', type=float, default=CONTRAST, help='exponent of the luma spread')
    parser.add_argument('--rough-floor', type=float, default=ROUGH_FLOOR,
                        help='roughness = floor + (1 - floor) * the scan\'s (default: %(default)s)')
    parser.add_argument('--mask', action='store_true', help='a flat magenta layer without tufts, for the measure')
    parser.add_argument('--preview', type=Path, help='write previews of the tile here and stop: no set')
    args = parser.parse_args()

    source, output = STAGING / args.source, STAGING / args.set
    if output.exists() and not args.preview:
        raise SystemExit(f'{output}: staging set exists; choose a new set name')
    index = json.loads((source / 'materials/materials.json').read_text())
    fringe_set, probe, pilot = sibling('landscape_fringe.py'), sibling('landscape_probe.py'), sibling('megascans_pilot.py')
    for texture in probe.STRIPS:
        if 'projection' not in index.get(f'{texture}.TGA', {}):
            raise SystemExit(f'{source}: {texture} has no projection; the fringe needs it')
    dds = sibling('test-material.py')

    if args.rebake or not (args.bake / 'bake.npz').exists():
        bake(args.bake, args.size, args.tile, args.seed)
    patch = json.loads((args.bake / 'patch.json').read_text())
    if (patch['size'], patch['tile_m'], patch['seed']) != (args.size, args.tile, args.seed):
        raise SystemExit(f'{args.bake}: baked at {patch["size"]} px, {patch["tile_m"]} m, seed {patch["seed"]}; --rebake')
    passes = np.load(args.bake / 'bake.npz')
    height = passes['height'].astype(np.float64)
    light = passes['light'].astype(np.float64)
    shade = shadow(height, light, args.tile / args.size)
    value = spread(passes['colour'].astype(np.float64) * shade[..., None], args.contrast)
    target = fringe_set.painted_grass(probe, pilot)
    factor = target / value.reshape(-1, 3).mean(axis=0) * np.array(args.screen)
    value = np.clip(value * factor, 0, 1)

    # the spread at the original's texel: 512 px over about 5 m of wall
    step = args.size // 512
    coarse = luma(value.reshape(512, step, 512, step, 3).mean(axis=(1, 3)))
    report = dict(source=args.source, tufts=TUFTS, ground=GROUND, patch=patch, size=args.size, tile_m=args.tile,
                  curve=CURVE, floor=FLOOR, canopy_m=CANOPY_M, ao_power=AO_POWER, tongue_m=TONGUE_M, tongue=TONGUE,
                  rough_floor=args.rough_floor,
                  screen=list(args.screen), contrast=args.contrast,
                  painted_grass=target.round(4).tolist(), gains=factor.round(3).tolist(),
                  mean=value.reshape(-1, 3).mean(axis=0).round(4).tolist(),
                  luma_1cm=dict(mean=round(float(coarse.mean()), 4), std=round(float(coarse.std()), 4)),
                  painted_luma=painted_stats(probe, pilot), strips=probe.STRIPS)
    cut = cut_height(height, args.tile / args.size)
    if args.preview:
        preview(args.preview, value, cut, pilot)
        print(json.dumps(report, indent=2))
        return 0

    shutil.copytree(source, output)
    directory = output / 'materials/landscape_grass'
    directory.mkdir(parents=True)
    normal = passes['normal'].astype(np.float32) * (1 - UP) + np.array((0, 0, UP), np.float32)
    normal[..., 1] *= -1     # +Y up the image; the engine's bitangent follows texture v down
    rough = args.rough_floor + (1 - args.rough_floor) * np.clip(passes['rough'].astype(np.float64), 0, 1)
    orm = np.stack([np.clip(light, 0, 1) ** AO_POWER, rough, cut], axis=2)
    if args.mask:
        value = np.broadcast_to(np.array((1., 0., 1.)), value.shape)
        orm[..., 2] = 0
    write_albedo(dds, pilot, directory / 'albedo.dds', value)
    dds.write_dds(directory / 'normal.dds', normal, normal=True)
    if args.mask:
        dds.write_dds(directory / 'orm.dds', orm.astype(np.float32))
    else:
        orm[..., :2] *= 255
        fringe_set.write_orm(dds, directory / 'orm.dds', orm)

    fringe = dict(albedo='landscape_grass/albedo.dds', normal='landscape_grass/normal.dds',
                  orm='landscape_grass/orm.dds', tile_m=args.tile)
    for texture, strip in probe.STRIPS.items():
        record = dict(fringe, top=list(strip['top']))
        if strip['bottom']:
            record['bottom'] = list(strip['bottom'])
        index[f'{texture}.TGA']['fringe'] = record
    (output / 'materials/materials.json').write_text(json.dumps(index, indent=2) + '\n')
    report['mask'] = args.mask
    (output / 'grass-layer.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
