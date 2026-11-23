#!/usr/bin/env python3
"""The look of a flame atlas on screen at night, before filming: the engine's emission and tone curve by hand.

    uv run --no-project --with numpy --with pillow python scripts/content/fire/flame_tone.py ATLAS VARIANT...

reports/content/remaster/wall-torch/fire-on-wall.md, «3б: разбор и способ». ATLAS is the flame's texture (DDS or
PNG, RGBA: fire_atlas.py). A VARIANT is "colour,from,to,edge": the particle's colour (0..255, grey) and the
particles.d "hdr" of the effect; "colour,0,0,1" is an effect without "hdr". The script repeats mainEmissive of
shader/materials/main.frag (the colour multiplies the sRGB texel before the decode, inverse ACES times 3, the
night's gain of 6 shaped by "hdr") and the tone curve of tonemapping.glsl, and prints for the zones of the atlas
by brightness (the percentile of its lit texels) the gain, the light before the tone curve in the scene's white
and the colour on screen over three backgrounds: black, the light plaster by the torch under the white lamp of
the original and the same plaster under an orange lamp.
"""
from __future__ import annotations

import sys

import numpy as np
from PIL import Image

NIGHT_GAIN = 6.0  # EmissiveMaxGain, common.glsl
LUMA = np.array([0.2126, 0.7152, 0.0722])
ZONES = (30, 50, 75, 90, 97, 99.5)
BACKGROUNDS = (
    ('black', np.zeros(3)),
    ('plaster', np.array([1.38, 0.93, 0.81])),       # 40 cm from the torch, the lamp's limit 30
    ('orange wall', np.array([1.38, 0.45, 0.10])),   # the same wall under an orange lamp
)


def srgb_decode(x: np.ndarray) -> np.ndarray:
    return np.where(x <= 0.04045, x / 12.92, ((x + 0.055) / 1.055) ** 2.4)


def aces(x: np.ndarray) -> np.ndarray:
    return np.clip((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0, 1)


def aces_inv(x: np.ndarray) -> np.ndarray:
    root = np.sqrt(np.maximum(-1.0127 * x * x + 1.3702 * x + 0.0009, 0))
    return (-0.59 * x + 0.03 - root) / (2.0 * (2.43 * x - 2.51))


def screen(x: np.ndarray) -> tuple[int, ...]:
    return tuple(int(v) for v in (aces(x) ** (1 / 2.2) * 255).round())


def main() -> int:
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    texels = np.asarray(Image.open(sys.argv[1]).convert('RGBA')).astype(np.float64) / 255
    rgb, alpha = texels[..., :3], texels[..., 3]
    # zones by the texel's light at full colour and no gain
    own = ((aces_inv(srgb_decode(rgb)) * 3.0) * alpha[..., None]) @ LUMA
    lit = own > 0.01
    levels = np.percentile(own[lit], ZONES)
    for spec in sys.argv[2:]:
        colour, start, end, edge = (float(v) for v in spec.split(','))
        light = aces_inv(srgb_decode(rgb * colour / 255)) * 3.0
        peak = light.max(-1)
        if end > start:
            k = np.clip((peak - start) / (end - start), 0, 1)
            gain = 1 + (NIGHT_GAIN - 1) * (edge + (1 - edge) * k * k * (3 - 2 * k))
        else:
            gain = np.full_like(peak, NIGHT_GAIN)
        light = light * gain[..., None]
        print(f'colour {colour:.0f}, hdr from {start:g} to {end:g} edge {edge:g}')
        for zone, level in zip(ZONES, levels):
            pick = lit & (np.abs(own - level) < 0.05 * level + 1e-3)
            mean = (light[pick] * alpha[pick][:, None]).mean(0)
            looks = ' | '.join(f'{name}: {screen(mean + back)}' for name, back in BACKGROUNDS)
            print(f'  {zone:5.1f}%  gain {gain[pick].mean():4.2f}  light ({mean[0]:6.2f}, {mean[1]:6.2f}, {mean[2]:5.2f})  {looks}')
    print('backgrounds alone: ' + ' | '.join(f'{name}: {screen(back)}' for name, back in BACKGROUNDS))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
