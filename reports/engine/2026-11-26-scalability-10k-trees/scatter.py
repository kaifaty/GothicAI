"""Sample the effective NEWWORLD surface on a seeded jittered grid, in Gothic centimetres."""
import argparse
import json
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'scripts/content/world'))
from landscape_tuft_spots import chunk
from world_classes import material_class

CELL = 6400
SEED = 26112026


def read_surface(base, overrides):
    files = {f.name: f for f in base.glob('*.glb')}
    files.update({f.name: f for f in overrides.glob('*.glb')})
    verts, kinds = [], []
    for f in sorted(files.values()):
        for name, group, triangles, _, _ in chunk(f):
            material = material_class(name, group)
            if material == 'vegetation':
                continue
            verts.append(triangles)
            kinds.append(np.full(len(triangles), material in ('rock', 'ground')))
    tri, natural = np.concatenate(verts), np.concatenate(kinds)
    a, b, c = tri[:, 0], tri[:, 1], tri[:, 2]
    norm = np.cross(a - c, b - c)
    slope = np.abs(norm[:, 1]) / np.maximum(1.e-12, np.linalg.norm(norm, axis=1))
    lo, hi = tri.min(axis=1)[:, [0, 2]], tri.max(axis=1)[:, [0, 2]]
    bounds = np.stack((lo.min(axis=0), hi.max(axis=0)))
    tiles = {}
    # Chunks are assigned by triangle centre. A triangle can cover a neighbouring chunk.
    for i in range(int(bounds[0, 0] // CELL), int(bounds[1, 0] // CELL) + 1):
        for j in range(int(bounds[0, 1] // CELL), int(bounds[1, 1] // CELL) + 1):
            low = np.array([i, j]) * CELL
            ids = np.flatnonzero(((hi >= low - 50) & (lo <= low + CELL + 50)).all(axis=1)
                                 & (np.abs(norm[:, 1]) > 1.e-9))
            if len(ids):
                tiles[i, j] = (tri[ids], natural[ids], slope[ids], lo[ids], hi[ids])
    return bounds, tiles


def surface_at(tile, x, z):
    tri, dry, slope, lo, hi = tile
    ids = np.flatnonzero((lo[:, 0] <= x) & (x <= hi[:, 0]) & (lo[:, 1] <= z) & (z <= hi[:, 1]))
    a, b, c = tri[ids].transpose(1, 0, 2)
    det = (b[:, 2]-c[:, 2])*(a[:, 0]-c[:, 0]) + (c[:, 0]-b[:, 0])*(a[:, 2]-c[:, 2])
    u = ((b[:, 2]-c[:, 2])*(x-c[:, 0]) + (c[:, 0]-b[:, 0])*(z-c[:, 2]))/det
    v = ((c[:, 2]-a[:, 2])*(x-c[:, 0]) + (a[:, 0]-c[:, 0])*(z-c[:, 2]))/det
    inside = (u >= -1.e-7) & (v >= -1.e-7) & (u+v <= 1+1.e-7)
    if not inside.any():
        return None
    ids = ids[inside]
    y = (u*a[:, 1] + v*b[:, 1] + (1-u-v)*c[:, 1])[inside]
    top = y.argmax()
    if dry[ids[top]]:
        return float(y[top]), float(slope[ids[top]])
    return None


def scatter(bounds, tiles, step, layer=None):
    step_cm = step * 100
    rng = np.random.default_rng(SEED)
    pitch = np.array([step_cm, step_cm * np.sqrt(3)/2 if layer else step_cm])
    world = np.mgrid[tuple(slice(int(a // h)-1, int(b // h)+2) for (a,b),h in zip(bounds.T,pitch))]
    grid = world.reshape(2, -1).T
    points = (grid + .5) * pitch
    if layer:
        points[:,0] += (grid[:,1] % 2) * step_cm/2
    jitter = .018 if layer else .12
    points += rng.uniform(-jitter, jitter, grid.shape) * step_cm
    found = []
    for x, z in points:
        tile = tiles.get((int(x // CELL), int(z // CELL)))
        if tile is None:
            continue
        hit = surface_at(tile, x, z)
        if hit is None:
            continue
        around = (surface_at(tile, x+dx, z+dz) for dx, dz in ((50, 0),(-50, 0),(0, 50),(0,-50)))
        y = min([hit[0]] + [s[0] for s in around if s])
        found.append((round(x, 1), round(y, 1), round(z, 1), hit[1]))
    slopes = np.array([t[3] for t in found])
    print(json.dumps(dict(step_m=step, dry=len(found), below_45deg=int(sum(slopes >= np.cos(np.pi/4))),
                          estimated_dry_km2=round(len(found)*pitch.prod()/1.e10,4))), flush=True)
    if layer:
        if len(found) < 10000:
            raise ValueError('Insufficient dry sites')
        # Keep the flattest 10k sites across the whole island.
        kept = sorted(found, key=lambda point: -point[3])[:10000]
        placed = [dict(key=f'stress-fir-{i}', kind='mesh', visual='OWN_TREE_FIR_A_4',
                       position=list(row[:3]), yaw_deg=round(rng.uniform(0,360),2), collision='none')
                  for i,row in enumerate(kept)]
        layer.write_text(json.dumps(dict(format=1, add=placed), separators=(',', ':'))+'\n')
        print(json.dumps(dict(count=len(kept), max_slope_deg=np.degrees(np.arccos(kept[-1][3])),
                              layer=str(layer))),flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('chunks', type=Path)
    parser.add_argument('overrides', type=Path)
    parser.add_argument('steps', type=float, nargs='+')
    parser.add_argument('--layer', type=Path, help='Write the 10k stand using one triangular lattice pitch')
    args = parser.parse_args()
    if args.layer and len(args.steps) != 1:
        parser.error('--layer expects exactly one pitch')
    bounds, tiles = read_surface(args.chunks, args.overrides)
    print(json.dumps(dict(bounds_xz_cm=bounds.tolist(), tiles=len(tiles))), flush=True)
    for step in args.steps:
        scatter(bounds, tiles, step, args.layer)
