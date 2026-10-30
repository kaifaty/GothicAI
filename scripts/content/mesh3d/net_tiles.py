#!/usr/bin/env python3
"""Procedural tiles for the harbour rigging set: knotted fishing net with alpha, and its rolled-up form.

    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/net_tiles.py \
        --out ~/.local/share/gothic-ai/content-staging/rigging-tiles [--size 1024] [--seed 7]

Asset harbour-rigging (intent.md, P3: «сеть читается сетью из узловатой верёвки, с ячейками насквозь»).
image_gen does not give a tileable net with a clean alpha, so the net is drawn here, like the procedural
wax, glass and bone tiles of the market set. One tile is TILE_M metres: CELLS x CELLS square meshes (4 cm, as the original's heap net), each
knot jittered a little (the jitter repeats with the tile, so it tiles), every cord a twisted strand with a
slight sag between its knots, a fat knot at each crossing. Height gives the normal map (round cord, knot on
top) and the alpha (cord or knot = opaque). The colour is a dark grey-brown cord whose mean over the opaque
texels is NET_TARGET (linear RGB), the original net's mean (NW_HARBOUR_FISHINGNET_01, alpha >= 0.5).

own_prop_net/      albedo.png (RGBA, alpha 0/255), normal.png, roughness.png, ao.png, metallic.png, height.png;
                   the heaps' net, with a thicker cord (HEAP)
own_prop_net_heap/ the heaps' net after the original's texture: thin twisted cord, a dark rim, 24 meshes a tile
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

TILE_M = 0.5
CELLS = 12
CORD_M = 0.011
KNOT_M = 0.019
FINE = dict(cells=17, cord_m=0.009, knot_m=0.015, jitter=0.22)     # the dense net hung on the rack: 3 cm meshes
# the heaps' net: thicker cord, so about 60 % of a cell is cord as on the original's heaps in the frames
# (fifth review: our 1.1 cm cord over 4 cm meshes read as a pale veil with the pavement through it)
HEAP = dict(cord_m=0.016, knot_m=0.026)
NET_TARGET = np.array([0.090, 0.073, 0.044])     # linear RGB of the original net's opaque texels
ROLL_CORE = np.array([0.012, 0.010, 0.007])


def draw(size: int, seed: int, shift: tuple[float, float] = (0.0, 0.0), cells: int = CELLS, cord_m: float = CORD_M,
         knot_m: float = KNOT_M, jitter: float = 0.09) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Height (0..1 cord radius units), strand shading and a cord mask of one tile, drawn at 2x and reduced."""
    rng = np.random.default_rng(seed)
    big = size * 2
    px = big / TILE_M
    CELLS = cells
    cord_r, knot_r = cord_m / 2 * px, knot_m / 2 * px
    cell = big / CELLS
    offs = rng.uniform(-jitter, jitter, (CELLS, CELLS, 2)) * cell
    knots = np.array([[(i + 0.5 + shift[0]) * cell + offs[i, j, 0], (j + 0.5 + shift[1]) * cell + offs[i, j, 1]]
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
            cord(p, right, rng.uniform(-1, 1) * cell * max(0.05, jitter * 0.6), rng.uniform(0, 6.28))
            cord(p, down, rng.uniform(-1, 1) * cell * max(0.05, jitter * 0.6), rng.uniform(0, 6.28))
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


def draw_twisted(size: int, seed: int, cells: int = 24, radius: float = 0.14, halo: float = 0.06, twist: float = 0.32,
                 wave: float = 0.18, spacing: float = 0.2):
    """A woven net of thin two-ply cord, as the original heaps' texture (NW_HARBOUR_FISHINGNET_01): about 24
    meshes a tile, each cord round (light middle, dark edges) with its strands as diagonal bands, an opaque
    dark rim of shadow round it (the original's darkest opaque texels are black: they carry its contrast),
    at each crossing one cord over the other in a checkerboard, the cords a little wavy and unevenly spaced.
    Sizes are shares of a mesh. Returns height, tone (0-1) and the opaque mask; everything tiles."""
    rng = np.random.default_rng(seed)
    cell = size / cells
    xs = np.arange(size) + 0.5
    k = np.arange(1, 4)

    def cords():
        """Per cord: its offset across (px) as a function of the along coordinate, periodic over the tile."""
        base = (np.arange(cells) + 0.5 + rng.uniform(-spacing, spacing, cells)) * cell
        amp = rng.normal(0, wave * cell, (cells, 3)) / k
        ph = rng.uniform(0, 2 * np.pi, (cells, 3))
        return base[:, None] + (amp[:, :, None] * np.sin(2 * np.pi * k[None, :, None] * xs[None, None, :] / size + ph[:, :, None])).sum(1)

    def nearest(offsets, across):
        """Signed distance (px) to the nearest cord, and its index, for every pixel: rows across, columns along."""
        d = across[None, :, None] - offsets[:, None, :]          # cords x across x along
        d = (d + size / 2) % size - size / 2
        i = np.abs(d).argmin(0)
        return np.take_along_axis(d, i[None], 0)[0], i

    r, rh = radius * cell, (radius + halo) * cell
    dh, ih = nearest(cords(), xs)            # horizontal cords: rows y, along x
    dv, iv = nearest(cords(), xs)            # vertical cords: computed as rows x along y, then transposed
    dv, iv = dv.T, iv.T
    yy, xx = np.meshgrid(xs, xs, indexing='ij')

    def cord(d, along):
        on = np.abs(d) < r
        h = np.sqrt(np.clip(1 - (d / r) ** 2, 0, 1))
        strands = (0.5 + 0.5 * np.cos(2 * np.pi * (along / (twist * cell) + d / (2 * r)))) ** 1.6   # two plies at a slant
        return on, h, (0.12 + 0.88 * h ** 0.7) * (0.22 + 0.78 * strands)
    on_h, h_h, t_h = cord(dh, xx)
    on_v, h_v, t_v = cord(dv, yy)
    h_over = ((ih + iv) % 2 == 0)              # checkerboard: which cord lies on top at a crossing
    top_h = on_h & (~on_v | h_over)
    top_v = on_v & ~top_h
    height = np.where(top_h, 0.5 + 0.5 * h_h, np.where(top_v, 0.5 + 0.5 * h_v, 0.0))
    tone = np.where(top_h, t_h, np.where(top_v, t_v, 0.0))
    # the under cord darkens beside the one crossing over it
    under = (on_h & on_v)
    tone = np.where(under & top_h, tone * (0.55 + 0.45 * np.clip(np.abs(dv) / r, 0, 1)), tone)
    tone = np.where(under & top_v, tone * (0.55 + 0.45 * np.clip(np.abs(dh) / r, 0, 1)), tone)
    rim = ((np.abs(dh) < rh) | (np.abs(dv) < rh)) & ~(on_h | on_v)
    tone = np.where(rim, 0.03, tone)
    mask = (on_h | on_v | rim)
    return height, tone, mask


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
        tone = (0.4 + 0.6 * twist) * (0.3 + 0.7 * height ** 0.8) * (1 + 0.12 * fibre)   # crisp strands, dark edges
        tone *= ndimage.zoom(rng.uniform(0.85, 1.15, (6, 6)), args.size / 6, order=1, mode='grid-wrap')
        colour = tone[..., None] * np.array([1.0, 0.86, 0.6])[None, None]
        opaque = alpha > 0.5
        gain = NET_TARGET / colour[opaque].mean(0)
        albedo = colour * gain
        # alpha-tested texels outside the cord keep the cord colour (no dark fringe in the mips)
        _, (iy, ix) = ndimage.distance_transform_edt(~opaque, return_indices=True)
        save(out / folder, albedo[iy, ix], alpha, height, np.full_like(height, 0.92), 0.55 + 0.45 * np.clip(height, 0, 1), 6.0)
        return height, tone, mask, alpha, gain, albedo[opaque].mean(0)

    height, tone, mask, alpha, gain, mean = net('own_prop_net', **HEAP)
    # the heaps' net as the original's texture: thin twisted cord with a dark rim, 24 meshes a tile
    h_tw, t_tw, m_tw = draw_twisted(args.size, args.seed)
    opaque = m_tw
    fibre = 1 + 0.15 * ndimage.gaussian_filter(rng.normal(0, 1, t_tw.shape), 0.8, mode='wrap')
    # the original's opaque texels run from black rims to bright crests (L p10 0.00, p90 0.57): a steep tone
    colour_tw = ((t_tw * fibre) ** 1.7)[..., None] * np.array([1.0, 0.74, 0.38])[None, None]
    albedo_tw = colour_tw * (NET_TARGET / colour_tw[opaque].mean(0))
    _, (iy, ix) = ndimage.distance_transform_edt(~opaque, return_indices=True)
    # matte hemp: a sheen on the cord tops caught the grey sky in the game and washed the gold out of the cord
    rough_tw = np.where(h_tw > 0.5, 0.97 - 0.1 * (h_tw - 0.5) * 2, 0.97)
    save(out / 'own_prop_net_heap', albedo_tw[iy, ix], opaque.astype(np.float32), h_tw, rough_tw,
         0.45 + 0.55 * np.clip(h_tw, 0, 1), 2.0)
    heap_cov = float(opaque.mean())
    fine = net('own_prop_net_fine', **FINE)
    # rolled net: the fine net in four layers over a dark core, low relief, opaque
    layers = [draw(args.size, args.seed + k, (rng.uniform(0, 1), rng.uniform(0, 1)), **FINE) for k in range(4)]
    h_roll = np.maximum.reduce([(1 - 0.2 * k) * l[0] for k, l in enumerate(layers)]) * 0.5
    lit = np.maximum.reduce([(1 - 0.25 * k) * (0.62 + 0.38 * l[1]) * l[2] for k, l in enumerate(layers)])
    roll = ROLL_CORE[None, None] + lit[..., None] * np.array([1.0, 0.86, 0.6]) * gain * 0.6
    save(out / 'own_prop_net_roll', roll, None, h_roll, np.full_like(height, 0.95), 0.5 + 0.5 * np.clip(h_roll, 0, 1), 3.0)
    meta = {'tile_m': TILE_M, 'cells': CELLS, **HEAP, 'coverage': round(float(alpha.mean()), 4),
            'net_mean_linear': [round(float(v), 4) for v in mean], 'fine': {**FINE, 'coverage': round(float(fine[3].mean()), 4)},
            'heap': {'cells': 24, 'coverage': round(heap_cov, 4)}, 'seed': args.seed, 'size': args.size}
    (out / 'own_prop_net' / 'tile.json').write_text(json.dumps(meta, indent=1) + '\n')
    print(json.dumps(meta))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
