#!/usr/bin/env python3
"""Build a staging set with grass along the edges of the rock strips: the `fringe` layer of a projected material.

    uv run --no-project --with numpy --with pillow --with scipy python \
        scripts/content/landscape_fringe.py rock-fringe-1 [--from rock-projection-world] [--screen R G B]

Copies a set with the projected rock (task 1 of reports/content/2026-11-landscape-pilot/b3-rock-material.md) and
gives the two strip textures, NW_NATURE_STEIN_02 and _04, a `fringe` record field: the maps of one CC0 grass scan by
world position at its real size, laid where the original painted grass (the probe's STRIPS shares of v).

The layer's maps: colour, normal (as the rock's), ORM with the scan's AO and roughness and, in the blue (grass has no
metal), its height equalised to 0..1, which the shader compares with the threshold across the tufts. The colour is
brought to the mean of the original's painted grass (its pixels by the probe's grass mask, read only for the mean),
times the on-screen correction measured on frames of the game. No pixel of the original goes into a map.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import struct

import numpy as np

SOURCES = Path.home() / '.local/share/gothic-ai/content-sources/landscape-probe'
STAGING = Path.home() / '.local/share/gothic-ai/content-staging'
SCAN = 'sparse_grass'   # 2 m
TILE_M = 2.
# On-screen correction. Round 1 took the rock's of the probe, .78 / .72 / .56 (landscape_probe.SCREEN): the mock-up
# showed the original's grass pixels through it.
# Round 1 with it (2026-11-13): on the same pixels of grass on `ledge` and `road-3` the original was brighter and
# greener, original / new 1.246 / 1.328 / 1.422 (linear display RGB); round 2 takes their product.
SCREEN = (.97, .96, .80)


def sibling(name: str):
    path = Path(__file__).with_name(name)
    spec = importlib.util.spec_from_file_location(path.stem.replace('-', '_'), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def painted_grass(probe, pilot) -> np.ndarray:
    """Mean linear RGB of the original's painted grass on the strip textures: the pixels its mask holds as grass."""
    from PIL import Image
    base = sorted(Path.home().glob('.cache/gothic-ai/base/*/textures'), key=lambda p: p.stat().st_mtime)[-1]
    values = []
    for texture, strip in probe.STRIPS.items():
        with Image.open(base / f'{texture}.dds') as image:
            original = np.asarray(image.convert('RGB'))
        mask = probe.grass_mask(original, strip['top'], strip['bottom'])
        values.append(pilot.linear(original[mask > .5]))
    return np.concatenate(values).mean(axis=0)


def equalised(height: np.ndarray) -> np.ndarray:
    """0..255 with every value as frequent: the share of grass at threshold t is 1 - t."""
    order = np.argsort(height, axis=None, kind='stable')
    rank = np.empty(height.size, np.float64)
    rank[order] = np.arange(height.size) / max(height.size - 1, 1)
    return (rank.reshape(height.shape) * 255 + .5).astype(np.uint8)


def write_orm(dds, path: Path, orm: np.ndarray) -> None:
    """DXT1 ORM whose blue, the height, is equalised on every mip level by itself: the mips average the height toward
    its middle, and a threshold on it then cuts a straight line from afar; ranked per level, the share of grass at a
    threshold stays the same at any distance and the edge stays ragged at the scale the level still holds."""
    size = orm.shape[0]
    levels = int(np.log2(size)) + 1
    header = [124, 0xA1007, size, size, (size // 4) ** 2 * 8, 0, levels] + [0] * 11
    header += [32, 4, int.from_bytes(b'DXT1', 'little'), 0, 0, 0, 0, 0, 0x401008, 0, 0, 0, 0]
    image = orm.astype(np.float64) / 255
    with path.open('wb') as output:
        output.write(b'DDS ' + struct.pack('<31I', *header))
        for level in range(levels):
            ranked = image.copy()
            # a handful of texels has no distribution to keep: the middle
            ranked[..., 2] = equalised(image[..., 2]) / 255 if image[..., 2].size >= 64 else .5
            output.write(dds.bc1(dds.blocks(dds.quantize(ranked))).tobytes())
            if level + 1 < levels:
                image = dds.downsample(image)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('set')
    parser.add_argument('--from', dest='source', default='rock-projection-world',
                        help='staging set with the projected rock to copy (default: %(default)s)')
    parser.add_argument('--size', type=int, default=2048)
    parser.add_argument('--tile', type=float, default=TILE_M, help='metres per repeat of the grass')
    parser.add_argument('--screen', type=float, nargs=3, default=SCREEN, metavar=('R', 'G', 'B'),
                        help='on-screen correction of the grass colour, measured on frames of the game')
    parser.add_argument('--raw', action='store_true', help="keep the scan's own colour")
    args = parser.parse_args()

    source, output = STAGING / args.source, STAGING / args.set
    if output.exists():
        raise SystemExit(f'{output}: staging set exists; choose a new set name')
    index_path = source / 'materials/materials.json'
    index = json.loads(index_path.read_text())
    probe, pilot = sibling('landscape_probe.py'), sibling('megascans_pilot.py')
    for texture in probe.STRIPS:
        if 'projection' not in index.get(f'{texture}.TGA', {}):
            raise SystemExit(f'{index_path}: {texture} has no projection; the fringe needs it')
    dds = sibling('test-material.py')
    compress = dds.write_dds
    shutil.copytree(source, output)

    def write_dds(path: Path, image: np.ndarray, normal: bool = False) -> None:
        value = image.astype(np.float32) / 255
        if normal:  # as landscape_probe: PNG +Y up, the engine's bitangent follows texture V down
            value = value * 2 - 1
            value[..., 1] *= -1
            value[..., 2] = np.maximum(value[..., 2], 1e-3)
        compress(path, value, normal=normal)

    scan = SOURCES / SCAN
    directory = output / 'materials/landscape_fringe'
    directory.mkdir(parents=True)
    albedo = pilot.load(scan / f'{SCAN}_diff_4k.jpg', args.size)
    gains, target = [1., 1., 1.], painted_grass(probe, pilot)
    if not args.raw:
        value = pilot.linear(albedo)
        factor = target / value.reshape(-1, 3).mean(axis=0) * np.array(args.screen)
        albedo, gains = pilot.encode(value * factor), factor.round(3).tolist()
    write_dds(directory / 'albedo.dds', albedo)
    write_dds(directory / 'normal.dds', pilot.load(scan / f'{SCAN}_nor_gl_4k.png', args.size), normal=True)
    arm = pilot.load(scan / f'{SCAN}_arm_4k.jpg', args.size)
    from PIL import Image
    with Image.open(scan / f'{SCAN}_disp_4k.png') as image:  # 16 bit: convert('L') clips it to white
        height = np.asarray(image.convert('F').resize((args.size,) * 2, Image.Resampling.LANCZOS), np.float64)
    write_orm(dds, directory / 'orm.dds', np.stack([arm[..., 0], arm[..., 1], height], axis=2))

    fringe = dict(albedo='landscape_fringe/albedo.dds', normal='landscape_fringe/normal.dds',
                  orm='landscape_fringe/orm.dds', tile_m=args.tile)
    for texture, strip in probe.STRIPS.items():
        record = dict(fringe, top=list(strip['top']))
        if strip['bottom']:
            record['bottom'] = list(strip['bottom'])
        index[f'{texture}.TGA']['fringe'] = record
    (output / 'materials/materials.json').write_text(json.dumps(index, indent=2) + '\n')
    report = dict(source=args.source, scan=SCAN, size=args.size, tile_m=args.tile, screen=list(args.screen),
                  painted_grass=target.round(4).tolist(), gains=gains, strips=probe.STRIPS)
    (output / 'fringe.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
