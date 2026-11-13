#!/usr/bin/env python3
"""Pack rendered flame frames (fire_flipbook.py) into a looping atlas flipbook for an own particle effect.

    OPENCV_IO_ENABLE_OPENEXR=1 uv run --no-project --with numpy --with pillow --with opencv-python-headless \
        python scripts/content/fire/fire_atlas.py FRAMES_DIR OUT_DDS [--frames 64] [--fade 16] [--cols 8] [--rows 8] \
        [--white 0.95] [--keep 1.0] [--sides 0.0] [--cone 'BASE SLOPE'] [--scale 1.0] [--search] [--sheet PNG]
        [--atlas-png PNG] [--step 2] [--blur STEP]

reports/engine/2026-11-01-fire-rendering.md, section 8. Pairs of 48 fps frames average into one (the motion blur
of 1/24 s, without which a flipbook strobes). --step N takes one cell per N frames and --blur K averages the first
K of them (default all N): a torch simulated at twice its size puffs sqrt(2) slower, so its cells are spaced wider
in the sim and played faster (reports/content/remaster/wall-torch/flame-sim.md). The loop takes FRAMES+FADE cells: the last FADE fade into the first
FADE, so frame FRAMES-1 runs on into frame 0. Cells are filled row by row from the top left, as pfx.vert reads them
(particles.d "flipbook" {cols, rows, frames, fps}). The flame is additive: RGB carries it, alpha its brightest
channel. The render is linear EXR with the hot core far above 1; a hard clip turned it into a flat slab (r1), so each
channel goes through a shoulder 1 - exp(-k x), k set so that the 99th percentile of the lit pixels' brightest
channel lands at WHITE: red fills first, so the core runs to yellow-white as in a real flame, the tips stay
orange-red. Then sRGB, as the engine decodes particle textures. DXT5 with box mips. --keep takes the bottom part of
each frame: a flame of 0.4-0.6 m (r3) fills the lower half of the 1.6 m render, so --keep 0.5 gives square cells
at the same texels per centimetre and half the atlas. --sides S fades the outer S of each frame's width to black
(a smooth step): a tongue filmed in its gap (fire_tongue.py) also catches the gas leaking between the pot and the
stones along the whole frame, and on a card that turns to the camera such wide wings and the stones' hard
holdout edges show the plane. --cone BASE SLOPE keeps only a cone that widens upward from the bottom centre,
half-width BASE + SLOPE * height in frame widths (soft over 0.06): a tongue widens as it rises, while that leak
lies flat at the stones' height and a fade by width alone left it as flat spikes beside each tongue (r2).
--scale F resizes each frame (area filter), e.g. 0.5 for a torch rendered at twice the cell size. --search picks
the loop's window anywhere in the sim instead of its end: the one whose tail is most like its head where they
cross-fade (difference weighted by w(1 - w), the ghost of two tongues is worst half-way), as EmberGen users move
the loop range until the seam is gone (reports/content/remaster/wall-torch/flame-sim.md). --atlas-png writes the
atlas at full size as 8-bit sRGB RGBA, for previews (flipbook_clip.py) without decoding DXT5.
"""
from __future__ import annotations

import argparse
import importlib.util
import struct
from pathlib import Path

import cv2
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('material_fixture', ROOT / 'scripts/content/test-material.py')
dds = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dds)


def srgb(linear: np.ndarray) -> np.ndarray:
    c = np.clip(linear, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1 / 2.4) - 0.055)


def load(path: Path) -> np.ndarray:
    """A linear EXR of the render as RGB float."""
    a = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
    if a is None:
        raise SystemExit(f'{path}: unreadable (OPENCV_IO_ENABLE_OPENEXR=1?)')
    return np.ascontiguousarray(a[..., 2::-1], dtype=np.float32)


def write_dxt5(path: Path, rgba: np.ndarray) -> None:
    """DXT5 with a box-filtered mip chain; width and height powers of two, not necessarily equal."""
    h, w = rgba.shape[:2]
    if w & (w - 1) or h & (h - 1):
        raise ValueError(f'{path}: power-of-two sides, got {w}x{h}')
    levels = int(np.log2(min(w, h))) - 1
    header = [124, 0xA1007, h, w, (w // 4) * (h // 4) * 16, 0, levels] + [0] * 11
    header += [32, 4, int.from_bytes(b'DXT5', 'little'), 0, 0, 0, 0, 0, 0x401008, 0, 0, 0, 0]
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('wb') as out:
        out.write(b'DDS ' + struct.pack('<31I', *header))
        for level in range(levels):
            out.write(dds.bc3(dds.blocks(dds.quantize(rgba))).tobytes())
            if level + 1 < levels:
                rgba = dds.downsample(rgba)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('frames_dir', type=Path)
    parser.add_argument('out', type=Path)
    parser.add_argument('--frames', type=int, default=64)
    parser.add_argument('--fade', type=int, default=16)
    parser.add_argument('--cols', type=int, default=8)
    parser.add_argument('--rows', type=int, default=8)
    parser.add_argument('--white', type=float, default=0.95)
    parser.add_argument('--keep', type=float, default=1.0)
    parser.add_argument('--sides', type=float, default=0.0)
    parser.add_argument('--cone', type=str)
    parser.add_argument('--scale', type=float, default=1.0)
    parser.add_argument('--search', action='store_true')
    parser.add_argument('--sheet', type=Path)
    parser.add_argument('--atlas-png', type=Path)
    parser.add_argument('--step', type=int, default=2, help='sim frames per cell')
    parser.add_argument('--blur', type=int, help='frames averaged per cell, from its first (default --step)')
    args = parser.parse_args()
    step = args.step
    blur = args.blur or step
    if not 1 <= blur <= step:
        raise SystemExit(f'--blur {blur}: 1..{step}')
    files = sorted(args.frames_dir.glob('flame_*.exr'))
    need = step * (args.frames + args.fade)
    if len(files) < need:
        raise SystemExit(f'{len(files)} frames, need {need}')
    files = files[len(files) % step:] if args.search else files[len(files) - need:]  # default: the steadiest end
    seq = [sum(load(f) for f in files[i:i + blur]) / blur for i in range(0, len(files), step)]
    if args.scale != 1.0:
        h, w = seq[0].shape[:2]
        size = (int(round(w * args.scale)), int(round(h * args.scale)))
        seq = [cv2.resize(f, size, interpolation=cv2.INTER_AREA) for f in seq]
    cut = seq[0].shape[0] - int(round(seq[0].shape[0] * args.keep))
    if cut:
        lost = max(float(f[:cut].max()) for f in seq) / max(float(f.max()) for f in seq)
        if lost > 0.02:
            raise SystemExit(f'--keep {args.keep} cuts flame at {lost:.0%} of the peak')
        seq = [f[cut:] for f in seq]
    if args.sides > 0:
        w = seq[0].shape[1]
        x = np.minimum(np.arange(w) + 0.5, w - np.arange(w) - 0.5) / (w * args.sides)
        x = np.clip(x, 0, 1)
        window = (x * x * (3 - 2 * x)).astype(np.float32)[None, :, None]
        seq = [f * window for f in seq]
    if args.cone:
        base, slope = (float(v) for v in args.cone.split())
        h, w = seq[0].shape[:2]
        x = np.abs(np.arange(w) + 0.5 - w / 2) / w
        up = (h - np.arange(h) - 0.5) / w  # height over the frame bottom, in frame widths
        edge = np.clip((base + slope * up[:, None] - x[None, :]) / 0.06, 0, 1)
        window = (edge * edge * (3 - 2 * edge)).astype(np.float32)[..., None]
        seq = [f * window for f in seq]
    n, fade = args.frames, args.fade
    if args.search:
        w = np.arange(fade) / fade
        weight = w * (1 - w)
        scale = float(np.mean([f.mean() for f in seq])) or 1.0
        errors = [sum(weight[k] * float(np.abs(seq[s + n + k] - seq[s + k]).mean()) for k in range(fade))
                  / weight.sum() / scale for s in range(len(seq) - n - fade + 1)]
        best = int(np.argmin(errors))
        print(f'loop window: cells {best}..{best + n + fade - 1} of {len(seq)}, seam {errors[best]:.3f} '
              f'(end of sim {errors[-1]:.3f}, median {float(np.median(errors)):.3f}; mean abs diff / mean)')
        seq = seq[best:best + n + fade]
    loop = []
    for k in range(n):
        if k < fade:  # the tail runs into the head: frame n-1 -> 0 is the sim's own step n-1 -> n
            w = k / fade
            loop.append(seq[n + k] * (1 - w) + seq[k] * w)
        else:
            loop.append(seq[k])
    peaks = np.concatenate([f.max(axis=2).ravel() for f in loop])
    p99 = float(np.percentile(peaks[peaks > 1e-3], 99))
    k = -np.log(1 - args.white) / p99
    h, w = loop[0].shape[:2]
    atlas = np.zeros((h * args.rows, w * args.cols, 4), dtype=np.float32)
    for i, frame in enumerate(loop):
        r, c = divmod(i, args.cols)
        rgb = srgb(1 - np.exp(-k * frame))
        atlas[r * h:(r + 1) * h, c * w:(c + 1) * w, :3] = rgb
        atlas[r * h:(r + 1) * h, c * w:(c + 1) * w, 3] = np.clip(rgb.max(axis=2), 0, 1)
    write_dxt5(args.out, atlas)
    if args.atlas_png:
        Image.fromarray(np.rint(np.clip(atlas, 0, 1) * 255).astype('uint8'), 'RGBA').save(args.atlas_png)
    if args.sheet:
        Image.fromarray(np.rint(atlas[..., :3] * 255).astype('uint8')).resize(
            (atlas.shape[1] // 4, atlas.shape[0] // 4)).save(args.sheet)
    print(f'{args.out}: {len(loop)} frames {w}x{h} in {args.cols}x{args.rows}, atlas {atlas.shape[1]}x{atlas.shape[0]}, '
          f'linear peak {float(peaks.max()):.2f}, p99 {p99:.2f} -> {args.white}, k {k:.2f}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
