#!/usr/bin/env python3
"""How much the shadow of a spruce changes between two of its GLBs, with no game.

    uv run --offline --no-project --with numpy python scripts/content/fir_shadow_ladder.py \
      FULL.glb L1.glb L2.glb L3.glb L4.glb [--px 800 400] [--pairs 0:1 1:2 2:3 0:3] [--png OUT.png] [--json OUT.json]
      [--meshlets]

Each GLB is drawn into the plane across the sun as the shadow pass draws it: the wood solid, a card where the
atlas's coverage mask says so (scripts/content/mesh3d/blender/fir-boughs-mask.npz, as tree_density.py). A texel of
the picture is the tree's height over PX: the virtual shadow map takes the mip whose texel is a pixel of the main
view, and PX is the tree's height there. A texel is in shadow when more than half of its RAYS by RAYS rays hit the
tree; the shadow the ground and the crown receive is that picture under the 3 texel box of the sun's filter
(vsm_sun_shadow.glsl).

Numbers of a pair of GLBs at a height, the mean of the AZIMUTHS turns of the tree under a sun ELEVATION degrees high
(noon of the fir-bough benches) and, in brackets, the largest of them:

- `changed`: the sum of the absolute difference of the two filtered shadows over the sum of the larger of the two, the
  share of the shadow that changes when one GLB replaces the other;
- `texels`: the same by whole texels before the filter, texels in one shadow only over texels in either;
- `area`: the coarser GLB's shadow area over the finer one's, minus one.

The PNG puts the shadows of the first turn side by side at the first PX.

--meshlets prints what a GLB costs the shadow pass instead: its meshlets as PackedMesh::buildMeshlets cuts them where
no triangle shares a wedge (the unused triangle of the lowest position next, 64 vertices or 64 triangles a meshlet),
the median diameter of their spheres, and the pairs of a meshlet and a shadow page of one tree at each PX: a page is
128 texels, a meshlet as wide as D pages lies in (D + 1)^2 of them. Each number comes twice: for the positions in the
file's order and in the order of TreeBough::sortLevelInSpace (piece by piece along the Z-order curve of the pieces'
middles).
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

BLENDER = Path(__file__).resolve().parent / 'mesh3d' / 'blender'
sys.path.insert(0, str(BLENDER))
import tree_density
import tree_glb

RAYS, ELEVATION, AZIMUTHS = 3, 57., (0., 90., 180., 270.)
MESHLET, PAGE = 64, 128  # PackedMesh::MaxVert and MaxPrim, VSM_PAGE_SIZE


def light_basis(azimuth: float, elevation: float = ELEVATION) -> np.ndarray:
    """Columns: two axes across the light and the direction to it, Z up."""
    a, e = np.radians(azimuth), np.radians(elevation)
    to_light = np.array([np.cos(a) * np.cos(e), np.sin(a) * np.cos(e), np.sin(e)])
    u = np.cross(to_light, (0., 0., 1.))
    u /= np.linalg.norm(u)
    return np.stack([u, np.cross(to_light, u), to_light], axis=1)


def shadow(parts: list[dict], mask: np.ndarray, basis: np.ndarray, low: np.ndarray, ppm: float, shape) -> np.ndarray:
    """Texels of the plane across the light the tree covers."""
    fine = (shape[0] * RAYS, shape[1] * RAYS)
    hit = np.zeros(fine[0] * fine[1], bool)
    for part in parts:
        points = (part['position'].astype(np.float64) @ basis - low) * (ppm * RAYS)
        cutout = part['material'].get('alphaMode') == 'MASK'
        for _, pixel, *_ in tree_density.fragments(points, part['index'], fine, part['uv'] if cutout else None,
                                                   mask if cutout else None):
            hit[pixel] = True
    return hit.reshape(shape[0], RAYS, shape[1], RAYS).sum(axis=(1, 3)) > RAYS * RAYS // 2


def filtered(texels: np.ndarray) -> np.ndarray:
    """The 3 texel box of the sun's shadow filter."""
    padded = np.pad(texels.astype(np.float64), 1)
    rows, columns = texels.shape
    return sum(padded[y:y + rows, x:x + columns] for y in range(3) for x in range(3)) / 9.


def shadows(models: list[list[dict]], mask: np.ndarray, px: float, azimuth: float) -> list[np.ndarray]:
    """The shadows of every model in one frame, a texel of the first model's height over px."""
    basis = light_basis(azimuth)
    full = np.concatenate([part['position'] for part in models[0]])
    ppm = px / float(full[:, 2].max() - full[:, 2].min())
    every = np.concatenate([part['position'] for parts in models for part in parts]).astype(np.float64) @ basis
    low = np.append(every[:, :2].min(axis=0) - 4. / ppm, 0.)
    shape = tuple(np.ceil((every[:, :2].max(axis=0) - low[:2]) * ppm).astype(int)[::-1] + 4)
    return [shadow(parts, mask, basis, low, ppm, shape) for parts in models]


def compare(fine: np.ndarray, coarse: np.ndarray) -> dict:
    a, b = filtered(fine), filtered(coarse)
    return dict(changed=float(np.abs(a - b).sum() / np.maximum(a, b).sum()),
                texels=float((fine ^ coarse).sum() / (fine | coarse).sum()),
                area=float(coarse.sum() / fine.sum() - 1.))


def measure(models: list[list[dict]], mask: np.ndarray, heights: list[float], pairs: list[tuple[int, int]],
            azimuths=AZIMUTHS) -> list[dict]:
    out = []
    for px in heights:
        turns = [shadows(models, mask, px, azimuth) for azimuth in azimuths]
        for fine, coarse in pairs:
            rows = [compare(turn[fine], turn[coarse]) for turn in turns]
            out.append(dict(px=px, fine=fine, coarse=coarse,
                            **{key: float(np.mean([row[key] for row in rows])) for key in rows[0]},
                            **{f'{key}_max': float(max((row[key] for row in rows), key=abs)) for key in rows[0]}))
    return out


def in_space(position: np.ndarray, index: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """The positions renumbered as TreeBough::sortLevelInSpace does it, and the triangles on the new numbers."""
    piece = np.arange(len(position))

    def root(i: int) -> int:
        while piece[i] != i:
            piece[i] = piece[piece[i]]
            i = piece[i]
        return i
    for a, b, c in index:
        first = root(a)
        piece[root(b)] = first
        piece[root(c)] = first
    roots = np.array([root(i) for i in range(len(position))])
    of = np.unique(roots, return_inverse=True)[1]
    middle = np.stack([np.bincount(of, position[:, axis]) / np.bincount(of) for axis in range(3)], axis=1)
    low = position.min(axis=0)
    cell = np.clip((middle - low) / max(float((position.max(axis=0) - low).max()), 1e-6) * 1023, 0, 1023).astype(np.int64)
    code = sum(((cell[:, axis] >> bit) & 1) << (3 * bit + axis) for bit in range(10) for axis in range(3))
    order = np.lexsort((np.arange(len(position)), roots, code[of]))
    moved = np.empty(len(position), np.int64)
    moved[order] = np.arange(len(position))
    return position[order], moved[index]


def meshlets(position: np.ndarray, index: np.ndarray) -> list[float]:
    """The diameters of the meshlets' spheres."""
    out, corners, triangles = [], set(), 0

    def close() -> None:
        points = position[list(corners)]
        out.append(2. * float(np.linalg.norm(points - (points.max(axis=0) + points.min(axis=0)) / 2., axis=1).max()))
    for triangle in index[np.argsort(index.min(axis=1), kind='stable')].tolist():
        if triangles == MESHLET or len(corners | set(triangle)) > MESHLET:
            close()
            corners, triangles = set(), 0
        corners |= set(triangle)
        triangles += 1
    close()
    return out


def cost(parts: list[dict], heights: list[float]) -> list[dict]:
    """Meshlets, their median diameter and the pairs with shadow pages at each height: file order, then space order."""
    every = np.concatenate([part['position'] for part in parts])
    height = float(every[:, 2].max() - every[:, 2].min())
    out = []
    for sort in (False, True):
        diameters = []
        for part in parts:
            position, index = part['position'].astype(np.float64), part['index']
            diameters += meshlets(*(in_space(position, index) if sort else (position, index)))
        diameters = np.array(diameters)
        out.append(dict(meshlets=len(diameters), median=float(np.median(diameters)),
                        pairs=[float(((diameters * px / (PAGE * height) + 1.) ** 2).sum()) for px in heights]))
    return out


def sheet(texels: list[np.ndarray]) -> np.ndarray:
    """The shadows side by side: dark on light."""
    gap = np.ones((texels[0].shape[0], 8), bool)
    row = np.concatenate([np.concatenate([~picture, gap], axis=1) for picture in texels], axis=1)
    return np.repeat((row[::-1] * 215 + 40).astype(np.uint8)[..., None], 3, axis=2)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('glb', type=Path, nargs='+', help='the full GLB and its levels, finer first')
    parser.add_argument('--mask', type=Path, default=BLENDER / 'fir-boughs-mask.npz')
    parser.add_argument('--px', type=float, nargs='+', default=[800., 400.])
    parser.add_argument('--pairs', nargs='+', default=['0:1', '1:2', '2:3', '0:3'])
    parser.add_argument('--png', type=Path)
    parser.add_argument('--json', type=Path)
    parser.add_argument('--meshlets', action='store_true', help='the cost of each GLB in the shadow pass instead')
    args = parser.parse_args()
    models = [tree_glb.read(path) for path in args.glb]
    mask = tree_density.load_mask(args.mask)
    pairs = [tuple(map(int, pair.split(':'))) for pair in args.pairs]
    names = ['full'] + [f'L{i}' for i in range(1, len(models))]
    if args.meshlets:
        for name, parts in zip(names, models):
            file, space = cost(parts, args.px)
            print(f"{name:>4}: {sum(len(part['index']) for part in parts):6d} triangles, "
                  f"{file['meshlets']:4d} meshlets of {file['median']:5.1f} m, in space order {space['meshlets']:4d} of "
                  f"{space['median']:4.1f} m; pairs " + ', '.join(
                      f'{a:.0f} -> {b:.0f} at {px:g} px' for px, a, b in zip(args.px, file['pairs'], space['pairs'])))
        return 0
    rows = measure(models, mask, args.px, pairs)
    for row in rows:
        print(f"{row['px']:6g} px  {names[row['fine']]:>4} | {names[row['coarse']]:<4}"
              f"  changed {row['changed']:6.1%} ({row['changed_max']:6.1%})"
              f"  texels {row['texels']:6.1%} ({row['texels_max']:6.1%})"
              f"  area {row['area']:+7.1%} ({row['area_max']:+7.1%})")
    if args.json:
        args.json.write_text(json.dumps(dict(elevation=ELEVATION, azimuths=AZIMUTHS, rays=RAYS, rows=rows), indent=1) + '\n')
    if args.png:
        tree_density.png(args.png, sheet(shadows(models, mask, args.px[0], AZIMUTHS[0])))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
