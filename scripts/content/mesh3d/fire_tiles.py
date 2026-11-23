#!/usr/bin/env python3
"""Tiles of burning and burnt wood for the hearth logs, and the glow mask of the ember tile (asset city-hearth).

    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/fire_tiles.py TILES_DIR

reports/content/remaster/city-hearth/fire-composition.md: a log under the pot burns only at its inner end, where
the wood has gone to charcoal broken into blocks with orange heat in the cracks between them; behind that the
wood is charred black, then bark. In life charred wood splits into blocks of 1-3 cm ("alligator" char): the
cracks along the grain run long and deep, the ones across it are shorter. A glowing end is coal with the heat
showing in the cracks and in over half the blocks, a fifth skinned with white-grey ash. Charred wood without heat
is near black with a silvery sheen (fresh charcoal is smoother than wood).

Writes own_hearth_glow/ and own_hearth_char/ in TILES_DIR as the other tiles (albedo, normal, roughness,
metallic, ao, height PNG, 1024 px, periodic, the tile spans 0.25 m: atlas scale 4 on UVs in metres, V along
the grain), plus emission.png: the share of each texel that glows (bake_atlas.py "glow"). The glow tile's
emission is its cracks and the warm blocks; the char tile's is zero. own_hearth_ember/emission.png comes from
that tile's own colour: its orange is the glow, its grey lumps are not.
The relief is designed, not stretched: blocks 2-4 mm proud, cracks 4-6 mm deep at 4.1 px per mm, so the atlas
bakes these normals at strength 1 ("tile_normal").
"""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter

SIZE = 1024
SPAN_MM = 250.0  # the tile in mm


def periodic_noise(rng: np.random.Generator, sigma: float) -> np.ndarray:
    n = gaussian_filter(rng.standard_normal((SIZE, SIZE)), sigma, mode='wrap')
    return n / (n.std() + 1e-9)


def srgb(lin: np.ndarray) -> np.ndarray:
    c = np.clip(lin, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1 / 2.4) - 0.055)


def save(path: Path, data: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(np.rint(np.clip(data, 0, 1) * 255).astype(np.uint8)).save(path)


def normal_from_height(h_mm: np.ndarray) -> np.ndarray:
    """Tangent-space normal, PNG +Y up (rows down): n = (-dh/dx, dh/drow, 1) per mm of the tile."""
    mm = SPAN_MM / SIZE
    gx = (np.roll(h_mm, -1, 1) - np.roll(h_mm, 1, 1)) / (2 * mm)
    gr = (np.roll(h_mm, -1, 0) - np.roll(h_mm, 1, 0)) / (2 * mm)
    n = np.stack((-gx, gr, np.ones_like(gx)), -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return n * 0.5 + 0.5


def checks(rng: np.random.Generator, columns: int, rows: float) -> tuple[np.ndarray, np.ndarray]:
    """Alligator char as it splits: long checks along the grain (V) between columns of uneven width that wander
    a little, and in each column its own short cross checks at random heights. Returns the distance to the
    nearest crack in px and a block id."""
    yy, xx = np.mgrid[0:SIZE, 0:SIZE].astype(np.float64)
    edges = np.sort((np.arange(columns) + rng.uniform(-0.3, 0.3, columns)) * SIZE / columns)
    wander = np.stack([periodic_noise(rng, 40)[:, 0] * 6.0 for _ in range(columns)])  # px, along V
    # the column of each texel and its distances to the column's two edges
    pos = np.stack([(edges[k] + wander[k][yy.astype(int)]) for k in range(columns)])  # columns x H x W
    rel = (xx[None] - pos) % SIZE
    left = rel.min(0)
    col = rel.argmin(0)
    right = np.take_along_axis((np.roll(pos, -1, axis=0) - xx[None]) % SIZE, col[None], 0)[0]
    dist = np.minimum(left, right)
    block = col * 1000
    cross_dist = np.full((SIZE, SIZE), 1e9)
    for k in range(columns):
        n = max(2, int(round(rows * rng.uniform(0.8, 1.25))))
        ys = np.sort((np.arange(n) + rng.uniform(-0.3, 0.3, n)) * SIZE / n + rng.uniform(0, SIZE))  % SIZE
        tilt = rng.uniform(-0.12, 0.12, n)
        inside = col == k
        x_local = left - SIZE / columns / 2
        best = np.full((SIZE, SIZE), 1e9)
        idx = np.zeros((SIZE, SIZE), int)
        for i, (y0, t) in enumerate(zip(ys, tilt)):
            d = np.abs((yy - y0 - t * x_local + SIZE / 2) % SIZE - SIZE / 2)
            closer = d < best
            idx = np.where(closer, i, idx)
            best = np.minimum(best, d)
        # the block between two cross checks: count of checks above the texel
        above = np.zeros((SIZE, SIZE), int)
        for y0 in ys:
            above += (yy >= y0)
        cross_dist = np.where(inside, best, cross_dist)
        block = np.where(inside, k * 1000 + above % n, block)
    return np.minimum(dist, cross_dist * 1.15), block


def char_tile(seed: int, glowing: bool) -> dict[str, np.ndarray]:
    rng = np.random.default_rng(seed)
    # blocks about 1.5-2.5 cm across the grain (13 columns over 25 cm) and 1.3-2.1 cm along it
    gap, block = checks(rng, 13, 15)
    _, block = np.unique(block, return_inverse=True)
    block = block.reshape(SIZE, SIZE)
    width = np.clip(6.0 + 3.5 * periodic_noise(rng, 22), 2.5, 13.0)  # crack half-width in px (0.6-3 mm)
    crack = np.clip(1.0 - gap / width, 0, 1) ** 1.3  # 1 in the middle of a crack
    nb = block.max() + 1
    per_block = rng.uniform(0, 1, nb)
    lift = rng.uniform(1.5, 3.5, nb)[block]
    # fibres along the grain (V): fine streaks on the block faces
    fibre = gaussian_filter(rng.standard_normal((SIZE, SIZE)), (0.0, 0.7), mode='wrap')
    fibre = gaussian_filter(fibre, (16.0, 0.0), mode='wrap')
    fibre /= fibre.std()
    dome = np.clip(gap / 30.0, 0, 1) ** 0.6
    h_mm = lift * dome - 5.0 * crack + 0.2 * fibre + 0.2 * periodic_noise(rng, 3)
    tone = rng.uniform(0.8, 1.2, nb)[block]
    charcoal = np.array([0.030, 0.027, 0.025])
    albedo = charcoal * (tone * (1 + 0.18 * fibre))[..., None]
    rough = np.full((SIZE, SIZE), 0.62)  # fresh charcoal has a dull silvery sheen
    emission = np.zeros((SIZE, SIZE))
    if glowing:
        # a thin grey ash film on some blocks, mottled and gone near the cracks where it falls off
        film = rng.uniform(0, 1, nb)[block] * np.clip(periodic_noise(rng, 9) * 0.6 + 0.4, 0, 1)
        film *= (per_block > 0.8)[block] * np.clip((gap - 4) / 20.0, 0, 1)  # a fifth of the blocks: a lit ash skin reads beige at 2 m (r4)
        albedo = albedo + (np.array([0.16, 0.155, 0.145]) - albedo) * (film * 0.6)[..., None]
        rough = np.where(film > 0.2, 0.95, 0.8)
        # heat: every crack glows in its depth; over half the blocks glow dully through from their rims in
        warm = ((per_block > 0.25) & (per_block <= 0.8))[block]
        glow_face = np.where(warm, rng.uniform(0.25, 0.5, nb)[block], 0.0) * np.clip(1.0 - gap / 40.0, 0.0, 1)
        glow_face *= (1 - film)
        hot = np.array([1.0, 0.26, 0.025])  # deep in a crack: red-orange, so the glow's gain saturates to orange, not white (r3)
        dull = np.array([0.5, 0.05, 0.005])  # crack rim and warm block faces: red
        crack_colour = dull + (hot - dull) * (crack ** 2)[..., None]
        albedo = albedo + (crack_colour - albedo) * crack[..., None]
        albedo = albedo + (dull * 0.7 - albedo) * glow_face[..., None]
        emission = np.clip(np.maximum(crack, glow_face), 0, 1)
    else:
        albedo = albedo * (1 - 0.75 * crack[..., None])
    ao = 1.0 - 0.6 * crack
    return dict(albedo=srgb(albedo), normal=normal_from_height(h_mm), roughness=rough, metallic=np.zeros_like(rough),
                ao=ao, height=(h_mm - h_mm.min()) / (np.ptp(h_mm) + 1e-9), emission=emission)


def ember_emission(albedo_srgb: np.ndarray) -> np.ndarray:
    """The ember tile glows where it is orange: saturated and bright; its grey and black lumps do not."""
    rgb = albedo_srgb.astype(np.float64) / 255
    hi, lo = rgb.max(-1), rgb.min(-1)
    sat = (hi - lo) / np.maximum(hi, 1e-6)
    red = rgb[..., 0] >= rgb[..., 1]
    s = np.clip((sat - 0.35) / 0.4, 0, 1) * np.clip((hi - 0.25) / 0.45, 0, 1) * red
    return s * s * (3 - 2 * s)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('tiles', type=Path)
    args = parser.parse_args()
    tiles = args.tiles.expanduser()
    for key, glowing, seed in (('own_hearth_glow', True, 11), ('own_hearth_char', False, 12)):
        maps = char_tile(seed, glowing)
        for name, data in maps.items():
            save(tiles / key / f'{name}.png', data)
        lin = np.where(maps['albedo'] <= 0.04045, maps['albedo'] / 12.92, ((maps['albedo'] + 0.055) / 1.055) ** 2.4)
        print(f'{key}: linear mean {np.round(lin.reshape(-1, 3).mean(0), 4).tolist()}, glowing {float((maps["emission"] > 0.5).mean()):.1%}')
    ember = tiles / 'own_hearth_ember'
    mask = ember_emission(np.asarray(Image.open(ember / 'albedo.png').convert('RGB')))
    save(ember / 'emission.png', mask)
    print(f'own_hearth_ember: glowing {float((mask > 0.5).mean()):.1%}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
