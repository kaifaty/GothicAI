#!/usr/bin/env python3
"""Build a staging set for the layers probe of the landscape pilot: a scanned rock material and scanned rocks.

    uv run --no-project --with numpy --with pillow --with scipy python \
        scripts/content/landscape_probe.py landscape-layers-1 [--layers material kit] [--fringe]

Two layers, each can be built alone, so every one gets its own frame in the game:

- `material`: the wall textures NW_NATURE_STEIN_02/03/04 take the colour, normal and AO/roughness maps of one
  CC0 scan of a bedded cliff. The colour is brought to the mean of the texture it replaces. The material's UV
  scale puts the scan near its real size; an integer scale keeps every seam of the original unwrap whole.
  With `--fringe` the two textures laid as a strip (STEIN_02, STEIN_04) keep the original's painted grass along
  their top and bottom over the scanned rock, at scale 1: a mock-up of what a blend or vegetation layer has to
  bring back, not a deliverable - it holds the original's pixels.
- `kit`: rocks of one scanned set (mesh3d/blender/rock_set.py) through the object layer, at the places of
  world/landscape_rock_spots.py, in their own colour.

Sources stay outside git in ~/.local/share/gothic-ai/content-sources/landscape-probe (Poly Haven, CC0; the list
is in its credits.json). Note: reports/content/2026-11-landscape-pilot/layers-probe.md.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import shutil

import numpy as np

SOURCES = Path.home() / '.local/share/gothic-ai/content-sources/landscape-probe'
STAGING = Path.home() / '.local/share/gothic-ai/content-staging'
WORLD = 'NEWWORLD'
SCAN = 'marble_cliff_03'           # 5.75 m of a horizontally bedded cliff
REFERENCE = 'NW_NATURE_STEIN_03'   # the plain rock of the three: the other two carry a painted grass band
# Texture -> UV scale. One repeat of the original covers 9-12 m (STEIN_02/03) and 2.5 m (STEIN_04) at the
# probe's spot, so the scan is shown at 4.5-6 m and 2.5 m.
WALLS = {'NW_NATURE_STEIN_02': 2., 'NW_NATURE_STEIN_03': 2., 'NW_NATURE_STEIN_04': 1.}
# The strips: where the painted grass is, as shares of the texture's height - solid from the edge to `band`,
# tufts by colour to `tufts` - and how many times the scan repeats across the texture at scale 1.
STRIPS = {'NW_NATURE_STEIN_02': dict(top=(.20, .40), bottom=None, repeats=2),
          'NW_NATURE_STEIN_04': dict(top=(.17, .33), bottom=(.14, .26), repeats=1)}
ROCKSET = 'rock_moss_set_01'
ROCK_MATERIAL = 'OWN_LANDSCAPE_ROCKSET_01'
# The engine shows an original texture through its own curve and lights it by Lambert, so equal texture means
# do not give equal walls on screen. With the means alone the sunlit wall came out 1.4-1.5 times brighter and
# paler than the original in the same frames; this is that ratio (original / new, linear display RGB), eased.
SCREEN = (.78, .72, .56)


def sibling(name: str):
    path = Path(__file__).with_name(name)
    spec = importlib.util.spec_from_file_location(path.stem.replace('-', '_'), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def grass_mask(original: np.ndarray, top, bottom) -> np.ndarray:
    """0..1 where the original texture shows grass: a solid band at an edge plus greenish tufts below it."""
    from scipy.ndimage import gaussian_filter
    value = original.astype(np.float32) / 255
    red, green = value[..., 0], value[..., 1]
    colour = np.clip(((green - red) / (green + red + 1e-3) + .03) / .06, 0, 1)
    rows = (np.arange(value.shape[0], dtype=np.float32) + .5)[:, None] / value.shape[0]
    mask = np.zeros(value.shape[:2], np.float32)
    for edge, distance in ((top, rows), (bottom, 1 - rows)):
        if edge:
            band, tufts = edge
            mask = np.maximum(mask, np.clip((band - distance) / .06, 0, 1) + colour * np.clip((tufts - distance) / .06, 0, 1))
    return np.clip(gaussian_filter(np.clip(mask, 0, 1), 1.), 0, 1)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('set')
    parser.add_argument('--layers', nargs='+', choices=('material', 'kit'), default=['material', 'kit'])
    parser.add_argument('--antitile', choices=('off', 'offset', 'rotate'), default='off',
                        help='hex anti-tiling of the scan; its cells cut the beds with straight seams')
    parser.add_argument('--cell', type=float, default=1., help='anti-tiling cell, in repeats of the scan')
    parser.add_argument('--size', type=int, default=4096)
    parser.add_argument('--fringe', action='store_true', help='keep the painted grass of the strip textures (mock-up)')
    parser.add_argument('--raw', action='store_true', help='keep the scans\' own colour')
    parser.add_argument('--screen', type=float, nargs=3, default=SCREEN, metavar=('R', 'G', 'B'),
                        help='correction of the wall colour measured on frames of the game')
    args = parser.parse_args()

    output = STAGING / args.set
    if output.exists():
        raise SystemExit(f'{output}: staging set exists; choose a new set name')
    pilot = sibling('megascans_pilot.py')
    compress = sibling('test-material.py').write_dds

    def write_dds(path: Path, image: np.ndarray, normal: bool = False) -> None:
        value = image.astype(np.float32) / 255
        if normal:  # as megascans_pilot.build: PNG +Y up, the engine's bitangent follows texture V down
            value = value * 2 - 1
            value[..., 1] *= -1
            value[..., 2] = np.maximum(value[..., 2], 1e-3)
        compress(path, value, normal=normal)

    index, report = {}, dict(layers=args.layers)
    target = pilot.original_albedo(REFERENCE)

    if 'material' in args.layers:
        scan = SOURCES / SCAN
        directory = output / 'materials/landscape_rock'
        directory.mkdir(parents=True)
        albedo = pilot.load(scan / f'{SCAN}_diff_4k.jpg', args.size)
        gains = [1., 1., 1.]
        if not args.raw:
            value = pilot.linear(albedo)
            factor = target / value.reshape(-1, 3).mean(axis=0) * np.array(args.screen)
            albedo, gains = pilot.encode(value * factor), factor.round(3).tolist()
        normal = pilot.load(scan / f'{SCAN}_nor_gl_4k.png', args.size)
        write_dds(directory / 'albedo.dds', albedo)
        write_dds(directory / 'normal.dds', normal, normal=True)
        orm = pilot.load(scan / f'{SCAN}_arm_4k.jpg', args.size)
        write_dds(directory / 'orm.dds', orm)  # AO, rough, metal
        for texture, scale in WALLS.items():
            index[f'{texture}.TGA'] = dict(albedo='landscape_rock/albedo.dds', normal='landscape_rock/normal.dds',
                                           orm='landscape_rock/orm.dds', scale=scale, antitile=args.antitile,
                                           antitile_cell=args.cell)
        if args.fringe:
            from PIL import Image
            base = next(Path.home().glob('.cache/gothic-ai/base/*/textures'))
            for texture, strip in STRIPS.items():
                with Image.open(base / f'{texture}.dds') as image:
                    original = image.convert('RGB')
                    grass = np.asarray(original.resize((args.size, args.size), Image.Resampling.LANCZOS))
                    mask = Image.fromarray((grass_mask(np.asarray(original), strip['top'], strip['bottom']) * 255).astype(np.uint8))
                mask = (np.asarray(mask.resize((args.size, args.size), Image.Resampling.BICUBIC)).astype(np.float32) / 255)[..., None]
                repeats = strip['repeats']

                def tiled(image: np.ndarray) -> np.ndarray:
                    small = np.asarray(Image.fromarray(image).resize((args.size // repeats,) * 2, Image.Resampling.LANCZOS))
                    return np.tile(small, (repeats, repeats, 1)).astype(np.float32)

                # The original's pixels now go the way of an own material: the same on-screen correction.
                colour = pilot.linear(tiled(albedo).astype(np.uint8)) * (1 - mask) + \
                    pilot.linear(grass) * np.array(args.screen) * mask
                flat, soft = np.array([127.5, 127.5, 255.]), np.array([255., 235., 0.])  # no relief; AO 1, rough .92
                name = f'landscape_{texture.lower()}'
                directory = output / 'materials' / name
                directory.mkdir(parents=True)
                write_dds(directory / 'albedo.dds', pilot.encode(colour))
                write_dds(directory / 'normal.dds', tiled(normal) * (1 - mask) + flat * mask, normal=True)
                write_dds(directory / 'orm.dds', tiled(orm) * (1 - mask) + soft * mask)
                index[f'{texture}.TGA'] = dict(albedo=f'{name}/albedo.dds', normal=f'{name}/normal.dds',
                                               orm=f'{name}/orm.dds', scale=1., antitile='off', antitile_cell=1.)
        report['material'] = dict(scan=SCAN, size=args.size, gains=gains, screen=list(args.screen), scales=WALLS,
                                  antitile=args.antitile, fringe=sorted(STRIPS) if args.fringe else [])

    if 'kit' in args.layers:
        textures = SOURCES / ROCKSET / 'textures'
        directory = output / 'materials/landscape_rockset'
        directory.mkdir(parents=True)
        albedo = pilot.load(textures / f'{ROCKSET}_diff_4k.jpg', args.size)
        # The set keeps its own colour: brought to the wall's brightness (x1.57) the mossy rocks came out pale and
        # orange in the game. Only the on-screen correction of an own material applies.
        gain = [1., 1., 1.]
        if not args.raw:
            gain = list(args.screen)
            albedo = pilot.encode(pilot.linear(albedo) * np.array(gain))
        rough = pilot.load(textures / f'{ROCKSET}_rough_4k.jpg', args.size, 'L')
        write_dds(directory / 'albedo.dds', albedo)
        write_dds(directory / 'normal.dds', pilot.load(textures / f'{ROCKSET}_nor_gl_4k.jpg', args.size), normal=True)
        write_dds(directory / 'orm.dds', np.stack([np.full_like(rough, 255), rough, np.zeros_like(rough)], axis=2))
        index[f'{ROCK_MATERIAL}.TGA'] = dict(albedo='landscape_rockset/albedo.dds', normal='landscape_rockset/normal.dds',
                                             orm='landscape_rockset/orm.dds', scale=1., antitile='off', antitile_cell=1.)
        rocks = json.loads((SOURCES / 'work/rocks/rocks.json').read_text())
        (output / 'meshes/landscape').mkdir(parents=True)
        for rock in rocks:
            shutil.copy2(SOURCES / 'work/rocks' / rock['glb'], output / 'meshes/landscape' / rock['glb'])
        (output / 'meshes/meshes.json').write_text(
            json.dumps({rock['visual']: dict(glb=f'landscape/{rock["glb"]}') for rock in rocks}, indent=2) + '\n')
        layer = output / f'worlds/{WORLD}/vobs.d'
        layer.mkdir(parents=True)
        shutil.copy2(SOURCES / 'work/spots.json', layer / 'landscape-rocks.json')
        spots = json.loads((SOURCES / 'work/spots.json').read_text())['add']
        tris = {rock['visual']: rock['tris'] for rock in rocks}
        report['kit'] = dict(set=ROCKSET, size=args.size, gain=gain, meshes=len(rocks), placed=len(spots),
                             triangles=sum(tris[spot['visual']] for spot in spots))

    (output / 'materials/materials.json').write_text(json.dumps(index, indent=2) + '\n')
    (output / 'probe.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
