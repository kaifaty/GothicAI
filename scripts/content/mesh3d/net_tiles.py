#!/usr/bin/env python3
"""Procedural tiles for the harbour rigging set: knotted fishing net with alpha, and its rolled-up form.

    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/net_tiles.py \
        --out ~/.local/share/gothic-ai/content-staging/rigging-tiles [--size 1024] [--seed 7]

Asset harbour-rigging (intent.md, P3: «сеть читается сетью из узловатой верёвки, с ячейками насквозь»).
image_gen does not give a tileable net with a clean alpha, so the net is drawn here, like the procedural
wax, glass and bone tiles of the market set. One tile is TILE_M metres: CELLS x CELLS square meshes, each
knot jittered a little (the jitter repeats with the tile, so it tiles), every cord a twisted strand with a
slight sag between its knots, a fat knot at each crossing. Height gives the normal map (round cord, knot on
top) and the alpha (cord or knot = opaque). The colour is a dark grey-brown cord whose mean over the opaque
texels is NET_TARGET (linear RGB), the original net's mean (NW_HARBOUR_FISHINGNET_01, alpha >= 0.5).

own_prop_net/      albedo.png (RGBA, alpha 0/255), normal.png, roughness.png, ao.png, metallic.png, height.png
own_prop_net_fine/ the same at 3 cm meshes: the dense net hung on the drying rack
own_prop_net_roll/ the fine net in four layers over a dark core, opaque: a rolled or bundled net
Every other tile of the set comes from the market set's tiles (see rigging_tiles in rigging_build.sh).
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

TILE_M = 0.42
CELLS = 7
CORD_M = 0.02
KNOT_M = 0.034
FINE = dict(cells=14, cord_m=0.009, knot_m=0.015)     # the dense net hung on the rack: 3 cm meshes
NET_TARGET = np.array([0.090, 0.073, 0.044])     # linear RGB of the original net's opaque texels
ROLL_CORE = np.array([0.012, 0.010, 0.007])


def draw(size: int, seed: int, shift: tuple[float, float] = (0.0, 0.0), cells: int = CELLS, cord_m: float = CORD_M,
         knot_m: float = KNOT_M) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Height (0..1 cord radius units), strand shading and a cord mask of one tile, drawn at 2x and reduced."""
    rng = np.random.default_rng(seed)
    big = size * 2
    px = big / TILE_M
    CELLS = cells
    cord_r, knot_r = cord_m / 2 * px, knot_m / 2 * px
    cell = big / CELLS
    jitter = rng.uniform(-0.09, 0.09, (CELLS, CELLS, 2)) * cell
    knots = np.array([[(i + 0.5 + shift[0]) * cell + jitter[i, j, 0], (j + 0.5 + shift[1]) * cell + jitter[i, j, 1]]
                      for i in range(CELLS) for j in range(CELLS)]).reshape(CELLS, CELLS, 2)
    height = np.zeros((big, big), np.float32)
    twist = np.zeros((big, big), np.float32)
    yy, xx = np.mgrid[0:big, 0:big].astype(np.float32)

    def cord(p0, p1, sag, phase):
        """A cord from p0 to p1 (pixels, may leave the tile: drawn wrapped) sagging by `sag` pixels."""
        nonlocal height, twist
        d = p1 - p0
        length = np.hypot(*d)
        n = np.array([-d[1], d[0]]) / length
        t = np.linspace(0, 1, int(length / 2) + 2)
        pts = p0[None] + d[None] * t[:, None] + n[None] * (sag * 4 * t * (1 - t))[:, None]
        lo = np.floor(pts.min(0) - cord_r - 2).astype(int)
        hi = np.ceil(pts.max(0) + cord_r + 2).astype(int)
        for ox in (-big, 0, big):
            for oy in (-big, 0, big):
                x0, x1 = lo[0] + ox, hi[0] + ox
                y0, y1 = lo[1] + oy, hi[1] + oy
                if x1 < 0 or y1 < 0 or x0 >= big or y0 >= big:
                    continue
                xs, ys = slice(max(x0, 0), min(x1, big)), slice(max(y0, 0), min(y1, big))
                gx, gy = xx[ys, xs] - ox, yy[ys, xs] - oy
                q = np.stack([gx, gy], -1)[..., None, :] - pts[None, None]
                dist2 = (q ** 2).sum(-1)
                k = dist2.argmin(-1)
                dist = np.sqrt(np.take_along_axis(dist2, k[..., None], -1)[..., 0])
                along = t[k] * length
                across = np.einsum('...i,i->...', np.stack([gx, gy], -1) - pts[k], n)
                h = np.sqrt(np.clip(1 - (dist / cord_r) ** 2, 0, 1))
                # three strands twisted around the cord: shading by a diagonal phase
                s = 0.5 + 0.5 * np.cos(2 * np.pi * (along / (cord_r * 2.6) + across / (cord_r * 2.2)) + phase)
                better = h > height[ys, xs]
                height[ys, xs] = np.where(better, h, height[ys, xs])
                twist[ys, xs] = np.where(better, s, twist[ys, xs])

    for i in range(CELLS):
        for j in range(CELLS):
            p = knots[i, j]
            right = knots[(i + 1) % CELLS, j] + (np.array([big, 0]) if i == CELLS - 1 else 0)
            down = knots[i, (j + 1) % CELLS] + (np.array([0, big]) if j == CELLS - 1 else 0)
            cord(p, right, rng.uniform(-0.05, 0.05) * cell, rng.uniform(0, 6.28))
            cord(p, down, rng.uniform(-0.05, 0.05) * cell, rng.uniform(0, 6.28))
    # knots: a flattened lump on top of each crossing
    w = int(knot_r * 1.3) + 2
    for i in range(CELLS):
        for j in range(CELLS):
            cx, cy = knots[i, j]
            for ox in (-big, 0, big):
                for oy in (-big, 0, big):
                    x0, y0 = int(cx + ox) - w, int(cy + oy) - w
                    if x0 + 2 * w < 0 or y0 + 2 * w < 0 or x0 >= big or y0 >= big:
                        continue
                    xs, ys = slice(max(x0, 0), min(x0 + 2 * w, big)), slice(max(y0, 0), min(y0 + 2 * w, big))
                    dx, dy = xx[ys, xs] - cx - ox, yy[ys, xs] - cy - oy
                    r = np.hypot(dx / 1.15, dy / 0.9)
                    h = 1.25 * np.sqrt(np.clip(1 - (r / knot_r) ** 2, 0, 1))
                    lump = 0.55 + 0.45 * np.cos(np.arctan2(dy, dx) * 3 + i + j)
                    better = h > height[ys, xs]
                    height[ys, xs] = np.where(better, h, height[ys, xs])
                    twist[ys, xs] = np.where(better, lump, twist[ys, xs])
    mask = height > 0.02
    reduce = lambda a: a.reshape(size, 2, size, 2).mean((1, 3))
    return reduce(height), reduce(twist), reduce(mask.astype(np.float32))


def normal_from(height: np.ndarray, strength: float) -> np.ndarray:
    gy, gx = np.gradient(ndimage.gaussian_filter(height, 0.7, mode='wrap'))
    n = np.stack([-gx * strength, gy * strength, np.ones_like(height)], -1)   # PNG +Y up, as the tiles of stage 1
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return n


def save(folder: Path, albedo: np.ndarray, alpha: np.ndarray | None, height: np.ndarray, rough: np.ndarray, ao: np.ndarray, strength: float):
    folder.mkdir(parents=True, exist_ok=True)
    srgb = np.clip(albedo, 0, 1) ** (1 / 2.2)
    rgb = (srgb * 255 + 0.5).astype(np.uint8)
    if alpha is not None:
        Image.fromarray(np.dstack([rgb, (alpha * 255).astype(np.uint8)]), 'RGBA').save(folder / 'albedo.png')
    else:
        Image.fromarray(rgb, 'RGB').save(folder / 'albedo.png')
    n = normal_from(height, strength)
    Image.fromarray(((n * 0.5 + 0.5) * 255 + 0.5).astype(np.uint8), 'RGB').save(folder / 'normal.png')
    for name, a in (('roughness', rough), ('ao', ao), ('height', height / max(height.max(), 1e-6)),
                    ('metallic', np.zeros_like(rough))):
        Image.fromarray((np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8), 'L').save(folder / f'{name}.png')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--size', type=int, default=1024)
    parser.add_argument('--seed', type=int, default=7)
    args = parser.parse_args()
    out = args.out.expanduser()
    rng = np.random.default_rng(args.seed)

    def net(folder, **shape):
        height, twist, mask = draw(args.size, args.seed, **shape)
        alpha = (mask >= 0.5).astype(np.float32)
        # fibre noise along the cords, a little lighter on the tops, darker in the twist grooves
        fibre = ndimage.gaussian_filter(rng.normal(0, 1, height.shape), 1.2, mode='wrap')
        tone = (0.62 + 0.38 * twist) * (0.7 + 0.3 * height) * (1 + 0.12 * fibre)
        tone *= ndimage.zoom(rng.uniform(0.85, 1.15, (6, 6)), args.size / 6, order=1, mode='grid-wrap')
        colour = tone[..., None] * np.array([1.0, 0.86, 0.6])[None, None]
        opaque = alpha > 0.5
        gain = NET_TARGET / colour[opaque].mean(0)
        albedo = colour * gain
        # alpha-tested texels outside the cord keep the cord colour (no dark fringe in the mips)
        _, (iy, ix) = ndimage.distance_transform_edt(~opaque, return_indices=True)
        save(out / folder, albedo[iy, ix], alpha, height, np.full_like(height, 0.92), 0.55 + 0.45 * np.clip(height, 0, 1), 6.0)
        return height, tone, mask, alpha, gain, albedo[opaque].mean(0)

    height, tone, mask, alpha, gain, mean = net('own_prop_net')
    fine = net('own_prop_net_fine', **FINE)
    # rolled net: the fine net in four layers over a dark core, low relief, opaque
    layers = [draw(args.size, args.seed + k, (rng.uniform(0, 1), rng.uniform(0, 1)), **FINE) for k in range(4)]
    h_roll = np.maximum.reduce([(1 - 0.2 * k) * l[0] for k, l in enumerate(layers)]) * 0.5
    lit = np.maximum.reduce([(1 - 0.25 * k) * (0.62 + 0.38 * l[1]) * l[2] for k, l in enumerate(layers)])
    roll = ROLL_CORE[None, None] + lit[..., None] * np.array([1.0, 0.86, 0.6]) * gain * 0.6
    save(out / 'own_prop_net_roll', roll, None, h_roll, np.full_like(height, 0.95), 0.5 + 0.5 * np.clip(h_roll, 0, 1), 3.0)
    meta = {'tile_m': TILE_M, 'cells': CELLS, 'cord_m': CORD_M, 'knot_m': KNOT_M, 'coverage': round(float(alpha.mean()), 4),
            'net_mean_linear': [round(float(v), 4) for v in mean], 'fine': {**FINE, 'coverage': round(float(fine[3].mean()), 4)},
            'seed': args.seed, 'size': args.size}
    (out / 'own_prop_net' / 'tile.json').write_text(json.dumps(meta, indent=1) + '\n')
    print(json.dumps(meta))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
