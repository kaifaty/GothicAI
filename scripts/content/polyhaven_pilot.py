#!/usr/bin/env python3
"""Build a staging set with a CC0 Poly Haven boulder in place of NW_NATURE_STONE_BIGHIGH_01.

    uv run --no-project --with numpy --with pillow --with scipy python \
        scripts/content/polyhaven_pilot.py polyhaven-pilot-1 [--raw]

The other half of the Megascans pilot: the same place, cameras and colour rule, to see what the Fab library adds
over scans that may go into open packs. Sources stay in
~/.local/share/gothic-ai/content-sources/polyhaven-pilot. The low mesh and normal map come from
mesh3d/scan_decimate.py and mesh3d/blender/scan_rock.py.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import shutil

import numpy as np

SOURCES = Path.home() / '.local/share/gothic-ai/content-sources/polyhaven-pilot'
STAGING = Path.home() / '.local/share/gothic-ai/content-staging'
VISUAL = 'NW_NATURE_STONE_BIGHIGH_01'
MATERIAL = 'OWN_POLYHAVEN_BOULDER'
ORIGINAL = 'NW_MISC_CAVEWALL_02'


def sibling(name: str):
    path = Path(__file__).with_name(name)
    spec = importlib.util.spec_from_file_location(path.stem.replace('-', '_'), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('set')
    parser.add_argument('--asset', default='boulder_01')
    parser.add_argument('--size', type=int, default=4096)
    parser.add_argument('--raw', action='store_true', help='keep the scan\'s own colour')
    args = parser.parse_args()

    output = STAGING / args.set
    if output.exists():
        raise SystemExit(f'{output}: staging set exists; choose a new set name')
    pilot = sibling('megascans_pilot.py')
    compress = sibling('test-material.py').write_dds
    bake = SOURCES / 'work' / f'{args.asset}_low'

    def write_dds(path: Path, image: np.ndarray, normal: bool = False) -> None:
        value = image.astype(np.float32) / 255
        if normal:  # as megascans_pilot.build: PNG +Y up, the engine's bitangent follows texture V down
            value = value * 2 - 1
            value[..., 1] *= -1
            value[..., 2] = np.maximum(value[..., 2], 1e-3)
        compress(path, value, normal=normal)

    materials = output / 'materials/polyhaven_boulder'
    materials.mkdir(parents=True)
    albedo = pilot.load(bake / 'diffuse.png', args.size)
    gains = [1., 1., 1.]
    if not args.raw:  # the new unwrap leaves black gaps: only baked texels count toward the mean
        value = pilot.linear(albedo)
        factor = pilot.original_albedo(ORIGINAL) / value[albedo.sum(axis=2) > 0].mean(axis=0)
        albedo, gains = pilot.encode(value * factor), factor.round(3).tolist()
    write_dds(materials / 'albedo.dds', albedo)
    write_dds(materials / 'normal.dds', pilot.load(bake / 'normal.png', args.size), normal=True)
    write_dds(materials / 'orm.dds', pilot.load(bake / 'arm.png', args.size))  # AO, rough, metal

    (output / 'materials/materials.json').write_text(json.dumps({f'{MATERIAL}.TGA': dict(
        albedo='polyhaven_boulder/albedo.dds', normal='polyhaven_boulder/normal.dds',
        orm='polyhaven_boulder/orm.dds', scale=1., antitile='off', antitile_cell=1.)}, indent=2) + '\n')
    (output / 'meshes/polyhaven').mkdir(parents=True)
    shutil.copy2(bake / 'boulder.glb', output / 'meshes/polyhaven/boulder.glb')
    (output / 'meshes/meshes.json').write_text(json.dumps({VISUAL: dict(glb='polyhaven/boulder.glb')}, indent=2) + '\n')
    report = dict(asset=args.asset, license='CC0', size=args.size, gains=gains,
                  **json.loads((bake / 'metrics.json').read_text()))
    (output / 'pilot.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
