#!/usr/bin/env python3
"""The glow mask of the wall torch's wrap tile (asset wall-torch-model).

    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/torch_tiles.py TILES_DIR

A wrap of tow and rags in pitch does not glow all over: the crust on top is black, and the heat shows where the
air reaches the smouldering fibre, in the creases between the layers of the winding and under the lifted edges
of the crust (reports/content/remaster/wall-torch-model/research.md). The tile's height map marks those creases:
its lowest texels. Heat creeps along a crease in stretches, so a smooth periodic noise leaves about half of each
dark; the rest glows by its depth. The first mask took 30 % of the tile for creases and lit them at full strength:
in the game the wrap read as a spotted hide, orange over a third of it (first look, 2026-11-14). Writes own_torch_tow/emission.png, the share of each texel that glows
(bake_atlas.py "glow"), periodic like the tile.
"""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter

CREASE = 0.16  # the share of the tile that counts as a crease, its lowest texels
COVER = 0.45   # the share of the creases that hold heat


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('tiles', type=Path)
    args = parser.parse_args()
    folder = args.tiles / 'own_torch_tow'
    height = np.asarray(Image.open(folder / 'height.png').convert('L'), dtype=np.float32) / 255
    height = gaussian_filter(height, 1.5, mode='wrap')  # single dark fibres are not creases
    level = np.quantile(height, CREASE)
    depth = np.clip((level - height) / max(level - np.quantile(height, 0.03), 1e-6), 0, 1)
    rng = np.random.default_rng(11)
    patch = gaussian_filter(rng.standard_normal(height.shape), 40, mode='wrap')
    patch = np.clip((patch - np.quantile(patch, 1 - COVER)) / (patch.std() * 0.6), 0, 1)
    glow = np.clip(depth * patch * 1.2, 0, 1)
    glow = np.maximum(glow, gaussian_filter(glow, 2.5, mode='wrap') * 0.4)  # the heat lights the crease's walls a little
    Image.fromarray(np.rint(glow * 255).astype(np.uint8), 'L').save(folder / 'emission.png')
    print(f'own_torch_tow/emission.png: {np.mean(glow > 0.3):.1%} of the tile glows above 0.3, mean {glow.mean():.3f}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
