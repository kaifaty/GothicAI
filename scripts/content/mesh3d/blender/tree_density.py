"""How open a tree's crown is, read from its GLB: two side views a quarter turn apart, no ground. NumPy only.

    python tree_density.py TREE.glb --mask ATLAS-MASK.npz [--png OUT.png] [--json OUT.json]

Each view is an orthographic picture of the file at PX pixels a metre, RAYS by RAYS rays a pixel: the wood is
solid, a card is there where the atlas's coverage mask says so (the mask is the atlas's alpha brought to a
quarter of its side with the coverage kept, fir_set.py textures). Twin triangles are drawn once. A pixel belongs
to the tree when more than half of its rays hit it, as in a render of that size. Numbers of a view, all above
ABOVE metres but the width:

- `background_share`: the share of the crown's outline through which the background shows. The outline is the
  picture of the tree closed by a disc of CLOSE metres with the holes inside it filled: it follows the notches
  between the tiers and takes in every hole in the crown. PX and CLOSE are set by the concept of spruce A: its
  picture is 47 pixels a metre, and its top third, which stands against the sky, gives 0.05 to 0.06 this way
  (with a disc of 0.4 m, which bridges the notches between the tiers, 0.17 to 0.20);
- `trunk_open_length_share`: the share of the trunk's rows (of rays) where more than half of its width is in
  front of everything else of the tree, and `trunk_open_area_share`, the same by rays;
- `crown_width_m`: the widest the tree is below ABOVE metres.

The PNG shows both views: the crown dark, the background inside the outline red, the open trunk orange.
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tree_glb  # noqa: E402

PX, RAYS, ABOVE, CLOSE = 48, 3, 3., .1


def load_mask(path: Path) -> np.ndarray:
    data = np.load(path)
    size = int(data['size'])
    return np.unpackbits(data['bits'])[:size * size].reshape(size, size).astype(bool)


def raster(points: np.ndarray, index: np.ndarray, shape, uv: np.ndarray | None = None,
           mask: np.ndarray | None = None) -> np.ndarray:
    """The nearest depth per pixel. points (n, 3): column, row and depth, the first two in pixels."""
    depth = np.full(shape, np.inf, np.float32)
    tri = points[index].astype(np.float64)
    low = np.floor(tri[..., :2].min(axis=1)).astype(np.int64)
    span = np.maximum(np.ceil(tri[..., :2].max(axis=1)).astype(np.int64) - low, 1)
    bucket = np.ceil(np.log2(span)).astype(np.int64)
    for wide, tall in np.unique(bucket, axis=0):
        chosen = np.flatnonzero((bucket[:, 0] == wide) & (bucket[:, 1] == tall))
        ox, oy = (v.reshape(-1) + .5 for v in np.meshgrid(np.arange(2 ** wide), np.arange(2 ** tall)))
        for part in np.array_split(chosen, max(1, len(chosen) * len(ox) // 2_000_000)):
            a, b, c = (tri[part, k] for k in range(3))
            x = low[part, 0, None] + ox[None]
            y = low[part, 1, None] + oy[None]
            area = (b[:, 1] - c[:, 1]) * (a[:, 0] - c[:, 0]) + (c[:, 0] - b[:, 0]) * (a[:, 1] - c[:, 1])
            flat = np.abs(area) < 1e-9
            area = np.where(flat, 1., area)[:, None]
            w0 = ((b[:, 1] - c[:, 1])[:, None] * (x - c[:, 0, None]) + (c[:, 0] - b[:, 0])[:, None] * (y - c[:, 1, None])) / area
            w1 = ((c[:, 1] - a[:, 1])[:, None] * (x - c[:, 0, None]) + (a[:, 0] - c[:, 0])[:, None] * (y - c[:, 1, None])) / area
            w2 = 1 - w0 - w1
            inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0) & ~flat[:, None]
            inside &= (x > 0) & (x < shape[1]) & (y > 0) & (y < shape[0])
            if uv is not None:
                corner = uv[index[part]].astype(np.float64)
                u = w0 * corner[:, 0, 0, None] + w1 * corner[:, 1, 0, None] + w2 * corner[:, 2, 0, None]
                v = w0 * corner[:, 0, 1, None] + w1 * corner[:, 1, 1, None] + w2 * corner[:, 2, 1, None]
                size = mask.shape[0]
                inside &= mask[np.clip((v * size).astype(np.int64), 0, size - 1),
                               np.clip((u * size).astype(np.int64), 0, size - 1)]
            z = (w0 * a[:, 2, None] + w1 * b[:, 2, None] + w2 * c[:, 2, None])[inside]
            np.minimum.at(depth, (y[inside].astype(np.int64), x[inside].astype(np.int64)), z.astype(np.float32))
    return depth


def grown(picture: np.ndarray, radius: int) -> np.ndarray:
    """Dilation by a disc; the picture's border is far enough from the tree."""
    out = picture.copy()
    for dy in range(-radius, radius + 1):
        reach = int((radius * radius - dy * dy) ** .5)
        rows = np.roll(picture, dy, axis=0)
        line = rows.copy()
        for dx in range(1, reach + 1):
            line |= np.roll(rows, dx, axis=1) | np.roll(rows, -dx, axis=1)
        out |= line
    return out


def outline(solid: np.ndarray, px: int, close: float) -> np.ndarray:
    """The tree's picture closed by a disc of `close` metres, its holes filled."""
    radius = round(close * px)
    closed = ~grown(~grown(solid, radius), radius) | solid
    outside = np.zeros_like(closed)
    outside[0], outside[-1], outside[:, 0], outside[:, -1] = True, True, True, True
    outside &= ~closed
    while True:
        more = outside.copy()
        more[1:] |= outside[:-1]
        more[:-1] |= outside[1:]
        more[:, 1:] |= outside[:, :-1]
        more[:, :-1] |= outside[:, 1:]
        more &= ~closed
        if np.array_equal(more, outside):
            return ~outside
        outside = more


def png(path: Path, image: np.ndarray) -> None:
    """An 8-bit RGB picture (rows, columns, 3)."""
    rows, columns = image.shape[:2]
    raw = np.concatenate([np.zeros((rows, 1), np.uint8), image.reshape(rows, columns * 3)], axis=1).tobytes()

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', columns, rows, 8, 2, 0, 0, 0))
                     + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def measure(glb: Path, mask: np.ndarray, px: int = PX, above: float = ABOVE, close: float = CLOSE,
            picture: Path | None = None) -> dict:
    prims = tree_glb.read(glb)
    wood = next(p for p in prims if p['material'].get('alphaMode') != 'MASK')
    cards = next(p for p in prims if p['material'].get('alphaMode') == 'MASK')
    everything = np.concatenate([p['position'] for p in prims])
    half = float(np.ceil(np.abs(everything[:, :2]).max() + 1.))
    top = float(np.ceil(everything[:, 2].max() + 1.))
    fine = px * RAYS
    rows, columns = int(top * px), int(2 * half * px)
    shape = (rows * RAYS, columns * RAYS)
    on_trunk = (wood['colour'][:, 1] == 0)[wood['index']].all(axis=1)
    _, first = np.unique(np.sort(cards['index'], axis=1), axis=0, return_index=True)
    fronts = cards['index'][np.sort(first)]
    floor = int(above * px)
    views, pictures = [], []
    for name, across, away in (('along Y', 0, (1, 1.)), ('along X', 1, (0, -1.))):
        def project(position):
            return np.stack([(position[:, across] + half) * fine, position[:, 2] * fine,
                             position[:, away[0]] * away[1]], axis=1)
        trunk = raster(project(wood['position']), wood['index'][on_trunk], shape)
        crown = np.minimum(raster(project(wood['position']), wood['index'][~on_trunk], shape),
                           raster(project(cards['position']), fronts, shape, cards['uv'], mask))
        hit = np.isfinite(trunk) | np.isfinite(crown)
        solid = hit.reshape(rows, RAYS, columns, RAYS).mean(axis=(1, 3)) > .5
        inside = outline(solid, px, close)
        behind = np.isfinite(trunk)
        seen = behind & (trunk < crown)
        width = behind[floor * RAYS:].sum(axis=1)
        open_rows = seen[floor * RAYS:].sum(axis=1) > .5 * width
        wide = np.flatnonzero(solid[:floor].any(axis=0))
        views.append(dict(
            view=name,
            background_share=round(float((inside & ~solid)[floor:].sum() / max(inside[floor:].sum(), 1)), 4),
            outline_m2=round(float(inside[floor:].sum()) / px / px, 2),
            trunk_open_length_share=round(float(open_rows[width > 0].mean()), 4),
            trunk_open_area_share=round(float(seen[floor * RAYS:].sum() / max(behind[floor * RAYS:].sum(), 1)), 4),
            trunk_rows_m=round(float((width > 0).sum()) / fine, 2),
            crown_width_m=round(float(wide.max() - wide.min() + 1) / px, 3)))
        if picture:
            image = np.full((rows, columns, 3), 255, np.uint8)
            image[inside] = (226, 96, 80)
            image[solid] = (44, 60, 44)
            image[seen.reshape(rows, RAYS, columns, RAYS).mean(axis=(1, 3)) > .5] = (240, 160, 30)
            image[floor] = image[floor] // 2 + 60
            pictures.append(image[::-1])
    if picture:
        png(picture, np.concatenate(pictures, axis=1))
    return dict(px_per_m=px, rays=RAYS, above_m=above, closing_m=close, views=views,
                background_share=max(v['background_share'] for v in views),
                trunk_open_length_share=max(v['trunk_open_length_share'] for v in views),
                crown_width_m=[v['crown_width_m'] for v in views])


def rules(density: dict, limits: dict, scale: float = 1.) -> list[str]:
    """The limits of a config (its `density`) the numbers break."""
    fails = []
    if density['background_share'] > limits['background_max']:
        fails.append(f"the background shows through {density['background_share']:.1%} of the crown's outline, "
                     f"the limit is {limits['background_max']:.0%}")
    if density['trunk_open_length_share'] > limits['trunk_open_max']:
        fails.append(f"{density['trunk_open_length_share']:.1%} of the trunk's length is open, "
                     f"the limit is {limits['trunk_open_max']:.0%}")
    low, high = (v * scale for v in limits['width_m'])
    for width in density['crown_width_m']:
        if not low <= width <= high:
            fails.append(f'the crown is {width} m wide at the ground, expected {low:g} to {high:g}')
    return fails


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('glb', type=Path)
    parser.add_argument('--mask', type=Path, required=True)
    parser.add_argument('--png', type=Path)
    parser.add_argument('--json', type=Path)
    args = parser.parse_args()
    text = json.dumps(measure(args.glb, load_mask(args.mask), picture=args.png), indent=1)
    print(text)
    if args.json:
        args.json.write_text(text + '\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
