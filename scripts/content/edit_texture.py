#!/usr/bin/env python3
"""Maps of a texture remastered by editing the original: an image_gen edit of the original texture
(the user's decision of 2026-10-23, docs/own-content.md, «Граница с оригиналом») becomes the albedo,
its colour taken from the original's broad colour, and a normal map from its own fine light.

    python3 scripts/content/edit_texture.py EDIT.png ORIGINAL.png OUT.png --period W H [--size W H]

The generator draws the original sharper but drifts its colour warm and yellow and over-sharpens
the finest grain into a pepper; the colour of every broad patch (about a centimetre and a half, three pixels of the original) is the original's,
the edit gives the light of all finer detail, its finest ripple softened. The relief is the edit's
light less its broad patches, scaled to a median normal tilt, as the normal-map rule asks (docs/remaster-pipeline.md). Such a texture is derived from the original: its
registry entry says so, and the original's file stays out of git.
"""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np

import piece_bake as pb
import timber_bake as tb

COLOUR_SIZE = .015     # metres: the original's colour is taken back over patches of about this size
RELIEF_SIZE = .012     # metres: light finer than this becomes relief
TILT = 8.              # median normal tilt in degrees: the plaster about 7, the oak 9-10 (rule of 2026-10-23)
ROUGHNESS = .9
CHROMA_SIZE = .002     # metres: the original's hue is read this finely (under one of its pixels on a 3 m square)
FINE = (2.5, .4)       # the edit's finest ripple: blur in pixels of the working size, how much of it stays
RELIEF_BLUR = None     # metres: the relief's finest band smoothed this much (None: one pixel of the working size);
                       # about .01 takes out the generator's scribbled grass and grain (harbour-quay, 2026-10-24)
DARK = .01            # linear light: where the original is darker, its own hue is noise, the broad patch's is used


def _load(path: Path, size) -> np.ndarray:
    from PIL import Image
    image = Image.open(path).convert('RGB').resize(size, Image.LANCZOS)
    return (np.asarray(image, dtype=np.float64) / 255) ** 2.2


def _normal(height: np.ndarray, metres: float) -> np.ndarray:
    dy, dx = np.gradient(height, metres)
    normal = np.stack([-dx, dy, np.ones_like(dx)], axis=-1)
    return normal / np.linalg.norm(normal, axis=-1, keepdims=True)


def _median_tilt(normal: np.ndarray) -> float:
    return float(np.degrees(np.median(np.arccos(np.clip(normal[..., 2], -1, 1)))))


def remaster(edit: np.ndarray, original: np.ndarray, period, tilt: float = TILT,
             relief_size: float = RELIEF_SIZE, relief_blur: float | None = RELIEF_BLUR,
             original_scale: float = 1.) -> dict:
    """Albedo, normal and roughness of the edit, its colour taken back from the original."""
    from scipy.ndimage import gaussian_filter
    rows, cols = edit.shape[:2]
    metres = period[1] / rows
    sigma = COLOUR_SIZE / metres
    # the colour all from the original's broad patches, the edit giving only the light of its detail;
    # its finest ripple (the generator's over-sharpened pepper) softened
    light = edit @ tb.LUM
    soft = gaussian_filter(light, FINE[0], mode='wrap')
    detail = soft + (light - soft) * FINE[1]
    ratio = detail / np.maximum(gaussian_filter(light, sigma, mode='wrap'), 1e-6)
    # the hue at the original's own pixels (moss stays green, not blended into the brown round it), the
    # light of the original's broad patches times the edit's detail
    # never finer than half a pixel of the original: its upscale rings in colour at every edge, and a hue read
    # finer than that draws the rings as red rims round the joints (harbour-quay, 2026-10-24)
    chroma_sigma = max(CHROMA_SIZE / metres, .5 * original_scale)
    near = np.stack([gaussian_filter(original[..., k], chroma_sigma, mode='wrap') for k in range(3)], -1)
    hue = near / np.maximum(near @ tb.LUM, 1e-6)[..., None]
    # a dark pixel's hue is mostly noise, and over a lighter stone of the edit it shows as a red or green rim
    broad = np.stack([gaussian_filter(original[..., k], sigma, mode='wrap') for k in range(3)], -1)
    broad_hue = broad / np.maximum(broad @ tb.LUM, 1e-6)[..., None]
    weight = np.clip((near @ tb.LUM) / DARK, 0, 1)[..., None]
    hue = weight * hue + (1 - weight) * broad_hue
    albedo = np.clip(hue * (gaussian_filter(original @ tb.LUM, sigma, mode='wrap') * ratio)[..., None], 0, 1)
    fine = np.log(np.maximum(light, 1e-4)) - np.log(np.maximum(gaussian_filter(light, relief_size / metres, mode='wrap'), 1e-4))
    height = gaussian_filter(fine, 1. if relief_blur is None else max(relief_blur / metres, 1.), mode='wrap')
    scale, target = .001, tilt
    for _ in range(4):                                   # tilt grows about linearly with the scale
        tilt = _median_tilt(_normal(height * scale, metres))
        scale *= target / max(tilt, 1e-3)
    normal = _normal(height * scale, metres)
    return dict(albedo=albedo, normal=normal, roughness=np.full((rows, cols), ROUGHNESS))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('edit', type=Path, help='the image_gen edit of the original texture')
    parser.add_argument('original', type=Path, help='the original texture (a PNG outside the repository)')
    parser.add_argument('out', type=Path)
    parser.add_argument('--period', type=float, nargs=2, required=True, metavar=('W', 'H'),
                        help='metres of wall the texture covers')
    parser.add_argument('--size', type=int, nargs=2, default=(1024, 1024), metavar=('W', 'H'))
    parser.add_argument('--tilt', type=float, default=TILT, help='median normal tilt, degrees (rubble about 14-16)')
    parser.add_argument('--relief-size', type=float, default=RELIEF_SIZE, help='metres: finer light becomes relief')
    parser.add_argument('--relief-blur', type=float, default=RELIEF_BLUR,
                        help="metres: smooth the relief's finest band (about .01 against the generator's scribbles)")
    args = parser.parse_args(argv)
    work = (args.size[0] * 2, args.size[1] * 2)
    from PIL import Image
    with Image.open(args.original) as image:
        scale = work[1] / image.height                   # working pixels per pixel of the original
    result = remaster(_load(args.edit, work), _load(args.original, work), args.period, args.tilt,
                      args.relief_size, args.relief_blur, scale)
    pb.write_maps(args.out, tuple(args.size), result['albedo'], result['normal'], {'roughness': result['roughness']})
    print(args.out)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
