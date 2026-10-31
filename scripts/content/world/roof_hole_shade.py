#!/usr/bin/env python3
"""The shade and the sides of a real roof hole (roof_holes.py): OWN_ROOF_HOLE_SHADE and OWN_ROOF_HOLE_WALL.

A roof hole shows the hut's interior, which the engine lights warm and bright; in Gothic the painted hole
is dark. The user (2026-10-26): dim it, not black, since a fire burns inside. The face at the bottom of
each hole faces out and uses this material with the multiply blend (alpha_func MULTIPLY): the engine draws
it unlit, the texture taken as emission (shader/lighting/tonemapping.glsl textureEmmisive: 3 x inverse ACES
of the linear colour) and blended as 2 x that x what is behind. A flat grey of linear ACES(SHADE / 6) keeps
SHADE of the interior's light; from inside the face is culled and the sky shows.

The sides of a hole are the cavity under the shingles. In the roof's own light timber the interior's fire
lit them as bright as a wall; they take the timber darkened to WALL_DARK of its linear colour.

    uv run --no-project --with numpy --with pillow python scripts/content/world/roof_hole_shade.py <content dir> \
        [--shade 0.18] [--wall-dark 0.3] [--wood <timber albedo.png>]
"""
import argparse
import importlib.util
import json
from pathlib import Path

import numpy as np

NAME = 'OWN_ROOF_HOLE_SHADE.TGA'
FOLDER = 'own-roof-hole-shade'
WALL = 'OWN_ROOF_HOLE_WALL.TGA'
WALL_FOLDER = 'own-roof-hole-wall'
WOOD = Path.home() / '.local/share/gothic-ai/content-staging/city-timber-sources/timber-7/timber-7-nw-city-woodbeam-02/albedo.png'


def srgb(linear):
    return 1.055 * linear ** (1 / 2.4) - .055 if linear > .0031308 else 12.92 * linear


def aces(x):
    return x * (2.51 * x + .03) / (x * (2.43 * x + .59) + .14)


def decode(srgb_values):
    return np.where(srgb_values <= .04045, srgb_values / 12.92, ((srgb_values + .055) / 1.055) ** 2.4)


def encode(linear):
    return np.where(linear <= .0031308, linear * 12.92, 1.055 * np.power(linear, 1 / 2.4) - .055)


def main(content, shade, wall_dark, wood):
    spec = importlib.util.spec_from_file_location('test_material', Path(__file__).resolve().parents[1] / 'test-material.py')
    tm = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tm)
    folder = Path(content) / 'materials' / FOLDER
    folder.mkdir(parents=True, exist_ok=True)
    size = 16
    grey = np.full((size, size, 3), srgb(aces(shade / 6)), dtype=np.float32)
    tm.write_dds(folder / 'albedo.dds', grey)
    tm.write_dds(folder / 'normal.dds', np.dstack([np.zeros((size, size)), np.zeros((size, size)), np.ones((size, size))]).astype(np.float32), normal=True)
    tm.write_dds(folder / 'orm.dds', np.dstack([np.ones((size, size)), np.ones((size, size)), np.zeros((size, size))]).astype(np.float32))
    from PIL import Image
    folder_w = Path(content) / 'materials' / WALL_FOLDER
    folder_w.mkdir(parents=True, exist_ok=True)
    timber = np.asarray(Image.open(wood).convert('RGB').resize((512, 512), Image.LANCZOS), dtype=np.float32) / 255
    tm.write_dds(folder_w / 'albedo.dds', encode(decode(timber) * wall_dark).astype(np.float32))
    flat = np.dstack([np.zeros((512, 512)), np.zeros((512, 512)), np.ones((512, 512))]).astype(np.float32)
    tm.write_dds(folder_w / 'normal.dds', flat, normal=True)
    tm.write_dds(folder_w / 'orm.dds', np.dstack([np.ones((512, 512)), np.full((512, 512), .9), np.zeros((512, 512))]).astype(np.float32))
    index_path = Path(content) / 'materials' / 'materials.json'
    index = json.loads(index_path.read_text()) if index_path.exists() else {}
    index[NAME] = {'albedo': f'{FOLDER}/albedo.dds', 'normal': f'{FOLDER}/normal.dds', 'orm': f'{FOLDER}/orm.dds',
                   'scale': 1.0, 'antitile': 'off', 'antitile_cell': 1.0}
    index[WALL] = {'albedo': f'{WALL_FOLDER}/albedo.dds', 'normal': f'{WALL_FOLDER}/normal.dds', 'orm': f'{WALL_FOLDER}/orm.dds',
                   'scale': 1.0, 'antitile': 'off', 'antitile_cell': 1.0}
    index_path.write_text(json.dumps(index, indent=2) + '\n')
    print(f'{NAME}: grey {srgb(aces(shade / 6)):.3f} sRGB, keeps {shade:.0%} of the light behind it')


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('content')
    ap.add_argument('--shade', type=float, default=.18)
    ap.add_argument('--wall-dark', type=float, default=.3)
    ap.add_argument('--wood', default=str(WOOD))
    a = ap.parse_args()
    main(a.content, a.shade, a.wall_dark, a.wood)
