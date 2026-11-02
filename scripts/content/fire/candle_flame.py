#!/usr/bin/env python3
"""Draw a looping candle flame flipbook for the street lamp's steady flame (OWN_LAMP_CANDLE).

    uv run --no-project --with numpy --with pillow --with opencv-python-headless \
        python scripts/content/fire/candle_flame.py OUT_DDS [--sheet PNG]

reports/content/remaster/city-hearth/lamp-design.md. A candle flame is laminar, not a fire: a teardrop 3-4 cm
tall over the wick that hardly moves inside a lantern. Its parts are what the eye knows: a small blue cup at
the base where the tallow vapour meets the air, a dark zone round the wick, a bright yellow-white core and an
orange-yellow mantle tapering to the tip. A Mantaflow sim (fire_tongue.py) is for turbulent fire; this is drawn.
It breathes: the height swings by about 6 % and the tip leans a millimetre at 1-3 Hz, every motion a whole
number of cycles over the loop, so frame 31 runs into frame 0. 32 frames of 64 x 128 (4 x 8 cm) in an 8 x 4
atlas, 16 fps (a 2 s loop). RGB carries the light, alpha its brightest channel, sRGB as the engine decodes
particle textures: the fire_atlas.py convention (particles.d "flipbook", visAlphaFunc ADD).
"""
from __future__ import annotations

import argparse
import importlib.util
import math
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('fire_atlas', HERE / 'fire_atlas.py')
fire_atlas = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fire_atlas)

FRAMES, COLS, ROWS = 32, 8, 4
CW, CH = 64, 128                # cell in px: 4 x 8 cm, 0.625 mm per px
MM = 40.0 / CW
WICK_Y = 14.0                   # mm above the cell's bottom: the wick's top, where the card is anchored


def frame(k: int) -> np.ndarray:
    """Linear RGB of frame k: the flame as emission, its brightest channel above 1 in the core."""
    phase = 2 * math.pi * k / FRAMES
    height = 34.0 * (1 + 0.04 * math.sin(phase * 2) + 0.02 * math.sin(phase * 5 + 1.1))   # mm, wick to tip
    lean = 0.9 * math.sin(phase * 3 + 0.4) + 0.4 * math.sin(phase * 1 + 2.0)               # mm at the tip
    width = 6.0 * (1 - 0.02 * math.sin(phase * 2))                                          # mm, half width at the belly: a third of the height
    yy, xx = np.mgrid[0:CH, 0:CW].astype(np.float64)
    x = (xx + 0.5 - CW / 2) * MM
    y = (CH - yy - 0.5) * MM - WICK_Y + 3.0      # mm above the flame's root, 3 mm under the wick top
    t = np.clip(y / height, 0, 1)
    # the teardrop: widest at a quarter of its height, tapering to a point; the tip leans
    profile = width * np.sin(np.pi * np.clip(t, 0, 1) ** 0.62) * (1 - 0.15 * t)
    xc = x - lean * t ** 2
    inside = np.clip((profile - np.abs(xc)) / 1.6, 0, 1) * (y > -1.0) * (y < height)
    edge = np.clip(np.abs(xc) / np.maximum(profile, 1e-3), 0, 1)
    # the core: an inner teardrop, yellow-white; the mantle round it orange-yellow; both fade to the tip
    core = np.clip(1 - edge / 0.5, 0, 1) ** 0.8 * np.clip((t - 0.06) / 0.12, 0, 1) * np.clip((0.85 - t) / 0.3, 0, 1)
    mantle = (1 - edge ** 3) * np.clip((1.0 - t) / 0.25, 0, 1) ** 0.8
    # the dark zone round the wick and the blue cup at the root
    dark = np.exp(-((xc / 2.2) ** 2 + ((y - 4.0) / 3.5) ** 2))
    blue = np.exp(-((np.abs(xc) - width * 0.55) ** 2 / 4.0 + ((y - 1.0) / 2.0) ** 2))
    rgb = (np.array([1.0, 0.42, 0.08])[None, None] * mantle[..., None] * 1.4
           + np.array([1.0, 0.8, 0.45])[None, None] * core[..., None] * 2.0)
    rgb *= (1 - 0.75 * dark)[..., None]
    rgb *= inside[..., None]
    rgb += np.array([0.10, 0.18, 0.55])[None, None] * blue[..., None] * 0.6 * (y > -1.0)[..., None]
    # a soft halo of the glow in the air round it, the size of the flame
    r = np.hypot(xc / (width * 1.8), (y - height * 0.35) / (height * 0.55))
    rgb += np.array([1.0, 0.5, 0.15])[None, None] * (np.exp(-r * r * 2.2) * 0.12)[..., None]
    return rgb


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('out', type=Path)
    parser.add_argument('--sheet', type=Path)
    args = parser.parse_args()
    atlas = np.zeros((CH * ROWS, CW * COLS, 4), dtype=np.float32)
    for k in range(FRAMES):
        r, c = divmod(k, COLS)
        lin = frame(k)
        rgb = fire_atlas.srgb(1 - np.exp(-1.2 * lin))  # the same shoulder as fire_atlas: the core runs to white
        atlas[r * CH:(r + 1) * CH, c * CW:(c + 1) * CW, :3] = rgb
        atlas[r * CH:(r + 1) * CH, c * CW:(c + 1) * CW, 3] = rgb.max(axis=2)
    fire_atlas.write_dxt5(args.out, atlas)
    if args.sheet:
        Image.fromarray(np.rint(atlas[..., :3] * 255).astype('uint8')).resize((atlas.shape[1] * 2, atlas.shape[0] * 2),
                                                                             Image.Resampling.NEAREST).save(args.sheet)
    print(f'{args.out}: {FRAMES} frames {CW}x{CH} in {COLS}x{ROWS}, the wick {WICK_Y} mm over the cell bottom')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
