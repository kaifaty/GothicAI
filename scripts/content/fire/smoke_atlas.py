#!/usr/bin/env python3
"""A smoke flipbook of the Unity Labs set (CC0) as a DXT5 atlas for the game's particles.

    uv run --no-project --with numpy --with pillow --with opencv-python-headless \
        python scripts/content/fire/smoke_atlas.py WispySmoke03b_8x8.tga OUT.dds [--alpha 1.6] [--value 0.8]

reports/content/remaster/smoke/README.md. The set's atlas is 8 x 8 cells of 128 px, 64 frames of one puff turning
and unwinding, its light baked from above. The colour stays a grey (the game lights it by the sun, the sky and the
fires next to it); `--value` scales it, `--alpha` scales the opacity so that a thin wisp survives the fade of a
particle's life. Cells keep their place: particles.d plays them with `flipbook` (cols 8, rows 8, frames 64).
"""
from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('fire_atlas', HERE / 'fire_atlas.py')
atlas = importlib.util.module_from_spec(spec)
spec.loader.exec_module(atlas)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('source', type=Path)
    parser.add_argument('out', type=Path)
    parser.add_argument('--alpha', type=float, default=1.0)
    parser.add_argument('--value', type=float, default=1.0)
    args = parser.parse_args()
    rgba = np.asarray(Image.open(args.source).convert('RGBA'), dtype=np.float32) / 255
    rgba[..., :3] = np.clip(rgba[..., :3] * args.value, 0, 1)
    rgba[..., 3] = np.clip(rgba[..., 3] * args.alpha, 0, 1)
    atlas.write_dxt5(args.out, rgba)
    cover = float((rgba[..., 3] > 0.02).mean())
    print(f'{args.out}: {rgba.shape[1]}x{rgba.shape[0]}, alpha mean {rgba[..., 3].mean():.3f}, max {rgba[..., 3].max():.2f}, '
          f'covered {cover:.0%}, grey {rgba[..., :3][rgba[..., 3] > 0.1].mean():.2f}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
