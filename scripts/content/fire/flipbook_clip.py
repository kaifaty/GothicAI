#!/usr/bin/env python3
"""Play an atlas flipbook as the engine does and write it as a clip: cells cross-faded at the playback rate.

    uv run --no-project --with numpy --with pillow python scripts/content/fire/flipbook_clip.py ATLAS_PNG OUT_MP4 \
        --fps 32 [--cols 8] [--rows 8] [--frames 64] [--speed 1] [--loops 3] [--height 280] [--video-fps 60] \
        [--label TEXT]

reports/content/remaster/wall-torch/flame-sim.md, step 5. ATLAS_PNG is fire_atlas.py --atlas-png (sRGB RGBA, cells
row by row from the top left). pfx.vert takes cell floor(t * fps) mod frames and the next one and mixes them by the
fraction; the texture is decoded from sRGB, so the mix is linear, and the flame adds onto black. --speed 0.25 plays
at a quarter of the speed, where a loop's seam would show; --loops sets the clip's length in loops of the atlas.
--height is the flame frame's height on screen in pixels (a 0.6 m frame from 1 m in 1080p is about 560 px, the
flame in it about 280). --occluder draws the torch head as a dark box over the cell, as its mesh hides the card
in the game. Needs ffmpeg on PATH.
"""
from __future__ import annotations

import argparse
import subprocess
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


def to_linear(c: np.ndarray) -> np.ndarray:
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def to_srgb(c: np.ndarray) -> np.ndarray:
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1 / 2.4) - 0.055)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('atlas', type=Path)
    parser.add_argument('out', type=Path)
    parser.add_argument('--fps', type=float, required=True, help='cells per second, the flipbook fps')
    parser.add_argument('--cols', type=int, default=8)
    parser.add_argument('--rows', type=int, default=8)
    parser.add_argument('--frames', type=int, default=64)
    parser.add_argument('--speed', type=float, default=1.0)
    parser.add_argument('--loops', type=float, default=3.0)
    parser.add_argument('--height', type=int, default=560)
    parser.add_argument('--video-fps', type=int, default=60)
    parser.add_argument('--label', type=str, default='')
    # a dark box in the frame, 'X0 X1 Y0 Y1' in fractions of the cell from its bottom left: the torch head mesh,
    # which in the game hides the card behind it
    parser.add_argument('--occluder', type=str)
    args = parser.parse_args()
    atlas = np.asarray(Image.open(args.atlas).convert('RGB'), dtype=np.float32) / 255
    h, w = atlas.shape[0] // args.rows, atlas.shape[1] // args.cols
    out_h = args.height // 2 * 2
    out_w = int(round(w * out_h / h)) // 2 * 2
    cells = []
    for i in range(args.frames):
        r, c = divmod(i, args.cols)
        cell = Image.fromarray(np.rint(atlas[r * h:(r + 1) * h, c * w:(c + 1) * w] * 255).astype('uint8'))
        cells.append(to_linear(np.asarray(cell.resize((out_w, out_h), Image.LANCZOS), dtype=np.float32) / 255))
    pad_w, pad_h = max(out_w + 80, 320) // 2 * 2, out_h + 60
    duration = args.loops * args.frames / args.fps / args.speed
    count = int(round(duration * args.video_fps))
    ffmpeg = subprocess.Popen(
        ['ffmpeg', '-y', '-loglevel', 'error', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{pad_w}x{pad_h}',
         '-r', str(args.video_fps), '-i', '-', '-c:v', 'libx264', '-crf', '14', '-pix_fmt', 'yuv420p',
         str(args.out)], stdin=subprocess.PIPE)
    x0, y0 = (pad_w - out_w) // 2, 10
    for k in range(count):
        t = k / args.video_fps * args.speed * args.fps
        a = int(np.floor(t)) % args.frames
        frac = t - np.floor(t)
        lin = cells[a] * (1 - frac) + cells[(a + 1) % args.frames] * frac
        if args.occluder:
            ox0, ox1, oy0, oy1 = (float(v) for v in args.occluder.split())
            lin = lin.copy()
            lin[int(out_h * (1 - oy1)):int(out_h * (1 - oy0)), int(out_w * ox0):int(out_w * ox1)] = 0.004
        frame = np.zeros((pad_h, pad_w, 3), dtype=np.uint8)
        frame[y0:y0 + out_h, x0:x0 + out_w] = np.rint(to_srgb(lin) * 255).astype('uint8')
        img = Image.fromarray(frame)
        draw = ImageDraw.Draw(img)
        draw.text((8, pad_h - 44), args.label, fill=(200, 200, 200))
        draw.text((8, pad_h - 24), f'cell {a:2d}  x{args.speed:g}  {args.fps:g} cells/s', fill=(150, 150, 150))
        ffmpeg.stdin.write(img.tobytes())
    ffmpeg.stdin.close()
    if ffmpeg.wait():
        raise SystemExit('ffmpeg failed')
    print(f'{args.out}: {count} frames, {duration:.1f} s, {pad_w}x{pad_h}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
