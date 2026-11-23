#!/usr/bin/env python3
"""Build the staging sets of the Megascans pilot from scans kept outside git.

    uv run --no-project --with numpy --with pillow --with scipy python \
        scripts/content/megascans_pilot.py check
    uv run --no-project --with numpy --with pillow --with scipy python \
        scripts/content/megascans_pilot.py build megascans-pilot-1 --size 2048 [--raw]

Scan files stay in ~/.local/share/gothic-ai/content-sources/megascans-pilot and in the staging sets:
the Fab licence forbids sharing them outside the project's own machines. Only this code and reports go to git.

`check` decides the normal map convention of each scan: tangent-space Y must rise with the height toward
the top of the image (OpenGL style, what the engine reads). The forest floor has a Bump map; the boulder
has only its AO, which is dark in pits, so AO stands in for height.

`build` writes albedo, normal and ORM as DDS with materials.json and meshes.json. Unless --raw, the place
keeps its palette: a linear gain brings the boulder's mean to the original's, and the forest floor takes its
colour patches from the original and only the fine detail from the scan.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import shutil

import numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter

Image.MAX_IMAGE_PIXELS = None

SOURCES = Path.home() / '.local/share/gothic-ai/content-sources/megascans-pilot'
STAGING = Path.home() / '.local/share/gothic-ai/content-staging'
FLOOR = 'Forest_Floor_vmhkbiqg_8K'
GROUND_TEXTURE = 'NW_NATURE_WALDBODEN_03.TGA'
GROUND_TILE_M = 5.023  # median UV scale of this texture in Khorinis, reports/content/2026-10-world-textures
SCAN_TILE_M = 3.0      # scan area of the forest floor, from its metadata
BOULDER_VISUAL = 'NW_NATURE_STONE_BIGHIGH_01'
BOULDER_MATERIAL = 'OWN_MEGASCANS_BOULDER'
BOULDER_ORIGINAL = 'NW_MISC_CAVEWALL_02'


def load(path: Path, size: int | None = None, mode: str = 'RGB') -> np.ndarray:
    with Image.open(path) as image:
        image = image.convert(mode)
        if size and image.size != (size, size):
            image = image.resize((size, size), Image.Resampling.LANCZOS)
        return np.asarray(image)


def linear(srgb: np.ndarray) -> np.ndarray:
    value = srgb.astype(np.float64) / 255
    return np.where(value <= .04045, value / 12.92, ((value + .055) / 1.055) ** 2.4)


def encode(value: np.ndarray) -> np.ndarray:
    value = np.clip(value, 0, 1)
    srgb = np.where(value <= .0031308, value * 12.92, 1.055 * value ** (1 / 2.4) - .055)
    return (srgb * 255 + .5).astype(np.uint8)


def original_albedo(texture: str) -> np.ndarray:
    base = next(Path.home().glob('.cache/gothic-ai/base/*/textures'))
    return read_dds_mean(base / f'{texture}.dds')


def read_dds_mean(path: Path) -> np.ndarray:
    """Mean linear RGB of the top mip of a DXT1/DXT3/DXT5 file, from its block end colours."""
    data = path.read_bytes()
    height, width = np.frombuffer(data, '<u4', 2, 12)
    fourcc = data[84:88]
    block = 8 if fourcc == b'DXT1' else 16
    colour_offset = 0 if block == 8 else 8
    count = int(max(1, width // 4) * max(1, height // 4))
    blocks = np.frombuffer(data, np.uint8, count * block, 128).reshape(-1, block)
    ends = np.frombuffer(blocks[:, colour_offset:colour_offset + 4].tobytes(), '<u2').reshape(-1, 2)
    red, green, blue = ends >> 11, (ends >> 5) & 63, ends & 31
    rgb = np.stack([red / 31, green / 63, blue / 31], axis=-1).reshape(-1, 3)
    return linear((rgb * 255).astype(np.uint8)).mean(axis=0)


def tilt_sign(normal: np.ndarray, height: np.ndarray) -> float:
    """Correlation of the normal's Y with the height's growth down the image: positive is OpenGL style."""
    green = normal[..., 1].astype(np.float64) / 255 - .5
    growth = np.gradient(height.astype(np.float64), axis=0)
    return float(np.corrcoef(green.ravel(), growth.ravel())[0, 1])


def check(_args) -> int:
    floor = SOURCES / 'forest_floor'
    size = 2048
    bump = load(floor / f'{FLOOR}_Bump.jpg', size, 'L')
    print('floor against Bump:', round(tilt_sign(load(floor / f'{FLOOR}_Normal.jpg', size), bump), 3))
    ao = load(floor / f'{FLOOR}_AO.jpg', size, 'L')
    print('floor against AO (calibrates the stand-in):', round(tilt_sign(load(floor / f'{FLOOR}_Normal.jpg', size), ao), 3))
    textures = SOURCES / 'boulder/Textures'
    orm = load(textures / 'T_vivvecldw_8K_ORM.png', size)
    print('boulder against AO:', round(tilt_sign(load(textures / 'T_vivvecldw_8K_N.png', size), orm[..., 0]), 3))
    print('boulder ORM means:', orm.reshape(-1, 3).mean(axis=0).round(1).tolist())
    return 0


def compressor():
    path = Path(__file__).with_name('test-material.py')
    spec = importlib.util.spec_from_file_location('material_fixture', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.write_dds


def matched(albedo: np.ndarray, texture: str, raw: bool) -> tuple[np.ndarray, list[float]]:
    if raw:
        return albedo, [1., 1., 1.]
    value = linear(albedo)
    gains = original_albedo(texture) / value.reshape(-1, 3).mean(axis=0)
    return encode(value * gains), gains.round(3).tolist()


def mixed(scan_srgb: np.ndarray, size: int, patch_m: float) -> np.ndarray:
    """Linear colour = the original's blurred patches x the scan's detail, on the original's 5 m tile."""
    base = next(Path.home().glob('.cache/gothic-ai/base/*/textures'))
    with Image.open(base / 'NW_NATURE_WALDBODEN_03.dds') as image:
        original = np.asarray(image.convert('RGB').resize((size, size), Image.Resampling.BICUBIC))
    sigma = patch_m / GROUND_TILE_M * size
    scan = np.tile(linear(scan_srgb), (2, 2, 1)).astype(np.float32)
    patches = np.stack([gaussian_filter(linear(original)[..., c].astype(np.float32), sigma, mode='wrap') for c in range(3)], axis=2)
    local = np.stack([gaussian_filter(scan[..., c], sigma, mode='wrap') for c in range(3)], axis=2)
    ratio = np.clip(scan / np.maximum(local, 1e-4), .1, 4.)
    luma = ratio @ np.array([.2126, .7152, .0722], dtype=np.float32)
    keep = luma[..., None] * (ratio / np.maximum(luma[..., None], 1e-4)) ** .35
    return encode(patches * keep)


def build(args) -> int:
    output = STAGING / args.set
    if output.exists():
        raise SystemExit(f'{output}: staging set exists; choose a new set name')
    compress = compressor()

    def write_dds(path: Path, image: np.ndarray, normal: bool = False) -> None:
        value = image.astype(np.float32) / 255  # the compressor reads 0..1
        if normal:
            # As pack_materials.read_maps: PNG +Y up -> the engine's bitangent follows texture V down.
            vector = value * 2 - 1
            vector[..., 1] *= -1
            vector[..., 2] = np.maximum(vector[..., 2], 1e-3)
            value = vector  # the compressor takes the vector itself and encodes it as DXT5nm
        compress(path, value, normal=normal)

    index, meshes, report = {}, {}, {}

    if not args.boulder_only:
        floor = SOURCES / 'forest_floor'
        directory = output / 'materials/megascans_floor'
        directory.mkdir(parents=True)
        if args.raw:
            size, scale, repeats = args.size, round(GROUND_TILE_M / SCAN_TILE_M, 4), 1
            albedo, gains = load(floor / f'{FLOOR}_BaseColor.jpg', size), [1., 1., 1.]
        else:
            # The original's colour patches on its own 5 m tile, the scan's detail inside them: two scan tiles per
            # texture tile, 2.5 m each. Holds original pixels, so such a texture never goes into the main pack.
            size, scale, repeats = args.mix_size, 1., 2
            albedo = mixed(load(floor / f'{FLOOR}_BaseColor.jpg', size // 2), size, args.patch_m)
            gains = 'patches'

        def tiled(image: np.ndarray) -> np.ndarray:
            return np.tile(image, (repeats, repeats, *([1] * (image.ndim - 2))))

        write_dds(directory / 'albedo.dds', albedo)
        normal = tiled(load(floor / f'{FLOOR}_Normal.jpg', size // repeats))
        write_dds(directory / 'normal.dds', normal, normal=True)
        ao = tiled(load(floor / f'{FLOOR}_AO.jpg', size // repeats, 'L'))
        rough = tiled(load(floor / f'{FLOOR}_Roughness.jpg', size // repeats, 'L'))
        write_dds(directory / 'orm.dds', np.stack([ao, rough, np.zeros_like(ao)], axis=2))
        index[GROUND_TEXTURE] = dict(albedo='megascans_floor/albedo.dds', normal='megascans_floor/normal.dds',
                                     orm='megascans_floor/orm.dds', scale=scale,
                                     antitile='rotate' if repeats == 1 else 'off', antitile_cell=1.)
        tile_m = SCAN_TILE_M if repeats == 1 else GROUND_TILE_M
        report['floor'] = dict(size=size, gains=gains, px_per_m=round(size / tile_m), original_px_per_m=round(512 / GROUND_TILE_M))

    bake = SOURCES / 'work/boulder_low'
    glb = bake / 'boulder.glb'
    if glb.exists():
        directory = output / 'materials/megascans_boulder'
        directory.mkdir(parents=True)
        textures = SOURCES / 'boulder/Textures'
        albedo, gains = matched(load(textures / 'T_vivvecldw_8K_B.png', args.boulder_size), BOULDER_ORIGINAL, args.raw)
        write_dds(directory / 'albedo.dds', albedo)
        write_dds(directory / 'normal.dds', load(bake / 'normal.png', args.boulder_size), normal=True)
        orm = load(textures / 'T_vivvecldw_8K_ORM.png', args.boulder_size)
        write_dds(directory / 'orm.dds', orm)
        index[f'{BOULDER_MATERIAL}.TGA'] = dict(albedo='megascans_boulder/albedo.dds', normal='megascans_boulder/normal.dds',
                                                orm='megascans_boulder/orm.dds', scale=1., antitile='off', antitile_cell=1.)
        (output / 'meshes/megascans').mkdir(parents=True)
        shutil.copy2(glb, output / 'meshes/megascans/boulder.glb')
        meshes[BOULDER_VISUAL] = dict(glb='megascans/boulder.glb')
        report['boulder'] = dict(size=args.boulder_size, gains=gains, **json.loads((bake / 'metrics.json').read_text()))

    (output / 'materials/materials.json').write_text(json.dumps(index, indent=2) + '\n')
    if meshes:
        (output / 'meshes/meshes.json').write_text(json.dumps(meshes, indent=2) + '\n')
    (output / 'pilot.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('check').set_defaults(run=check)
    make = commands.add_parser('build')
    make.add_argument('set')
    make.add_argument('--size', type=int, default=2048)
    make.add_argument('--boulder-size', type=int, default=4096)
    make.add_argument('--raw', action='store_true', help='keep the scans\' own colour')
    make.add_argument('--boulder-only', action='store_true')
    make.add_argument('--mix-size', type=int, default=4096)
    make.add_argument('--patch-m', type=float, default=.12, help='blur of the original colour patches, metres')
    make.set_defaults(run=build)
    args = parser.parse_args()
    return args.run(args)


if __name__ == '__main__':
    raise SystemExit(main())
