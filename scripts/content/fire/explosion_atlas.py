#!/usr/bin/env python3
"""An explosion and a fire ball flipbook of the Unity Labs set (CC0) as DXT5 atlases for the game's particles.

    uv run --no-project --with numpy --with pillow --with opencv-python-headless \
        python scripts/content/fire/explosion_atlas.py explosion Explosion02HD_5x5.tga FIRE.dds SMOKE.dds
    ... explosion_atlas.py loop FireBall01_8x8.tga BALL.dds

reports/content/remaster/magic-fire/fireball.md. `explosion`: the set's atlas is 5 x 5 cells of 409.6 px, 25 frames
of one burst: a white ball that tears into fire and ends as a dark cloud. The engine lights a blended sprite as a
surface and adds an additive one, so the burst is split in two atlases played by two particles at one place: the
fire (additive: RGB carries it, alpha its brightest channel) and the smoke (blended: the cloud, with soot where
the fire was, so the fire has something dark behind it at noon). The fire is what is brighter than the smoke's
grey. Cells are resampled to 256 px and laid 8 x 4, so particles.d plays them with
`flipbook` {cols 8, rows 4, frames 25, once}. `loop`: an 8 x 8 looping fire ball on black, kept as it is with
alpha from its brightest channel.
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

CELL = 256
COLS, ROWS = 8, 4
SOOT = 0.16
CLOUD = 2.2   # the set's cloud is near black; the game lights it as a surface, so it is stored as a mid grey
THIN = 0.9    # and thinner, so the wall shows through its edges


def smoothstep(lo, hi, x):
    t = np.clip((x - lo) / (hi - lo), 0, 1)
    return t * t * (3 - 2 * t)


def explosion(source: Path, fire_out: Path, smoke_out: Path) -> None:
    image = Image.open(source).convert('RGBA')
    side = image.size[0] / 5
    fire = np.zeros((ROWS * CELL, COLS * CELL, 4), dtype=np.float32)
    smoke = np.zeros_like(fire)
    for frame in range(25):
        sx, sy = frame % 5, frame // 5
        cell = image.crop((round(sx * side), round(sy * side), round((sx + 1) * side), round((sy + 1) * side)))
        rgba = np.asarray(cell.resize((CELL, CELL), Image.LANCZOS), dtype=np.float32) / 255
        rgb, alpha = rgba[..., :3], rgba[..., 3:4]
        hot = smoothstep(0.22, 0.55, rgb.max(axis=2, keepdims=True))
        glow = rgb * alpha * hot
        cloud = np.clip(rgb * CLOUD, 0, 0.55) * (1 - hot) + SOOT * hot
        y, x = (frame // COLS) * CELL, (frame % COLS) * CELL
        fire[y:y + CELL, x:x + CELL] = np.concatenate([glow, glow.max(axis=2, keepdims=True)], axis=2)
        smoke[y:y + CELL, x:x + CELL] = np.concatenate([cloud, alpha * THIN * (1 - 0.3 * hot)], axis=2)
    atlas.write_dxt5(fire_out, fire)
    atlas.write_dxt5(smoke_out, smoke)
    for name, data in ((fire_out, fire), (smoke_out, smoke)):
        print(f'{name}: {data.shape[1]}x{data.shape[0]}, alpha mean {data[..., 3].mean():.3f}, max {data[..., 3].max():.2f}')


def loop(source: Path, out: Path) -> None:
    rgb = np.asarray(Image.open(source).convert('RGB'), dtype=np.float32) / 255
    rgba = np.concatenate([rgb, rgb.max(axis=2, keepdims=True)], axis=2)
    atlas.write_dxt5(out, rgba)
    print(f'{out}: {rgba.shape[1]}x{rgba.shape[0]}, alpha mean {rgba[..., 3].mean():.3f}')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    one = sub.add_parser('explosion')
    one.add_argument('source', type=Path)
    one.add_argument('fire', type=Path)
    one.add_argument('smoke', type=Path)
    two = sub.add_parser('loop')
    two.add_argument('source', type=Path)
    two.add_argument('out', type=Path)
    args = parser.parse_args()
    if args.command == 'explosion':
        explosion(args.source, args.fire, args.smoke)
    else:
        loop(args.source, args.out)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
