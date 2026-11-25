#!/usr/bin/env python3
"""Move the 1,024 spruce A instances in crown-fix to a seeded NEWWORLD grid."""
import argparse
from hashlib import file_digest
import json
from pathlib import Path
import shutil
import sys
from tempfile import TemporaryDirectory

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / 'scripts/content/world'))
from world_census import load_world, rasterize, sea_level  # noqa: E402
from world_classes import is_cave_room, material_class  # noqa: E402

WORLD = 'NEWWORLD'
COUNT = 1024
SEED = 7
MIN_GAP_M = 8.
AREA_CELL_M = 4.
ROOT_PROBES_M = np.array([(0., 0.), (.5, 0.), (-.5, 0.), (0., .5), (0., -.5)])
ROAD_MARKERS = ('PATH', 'ROAD')
VOBS = Path('worlds') / WORLD / 'vobs.json'
FIR_ROOT = Path.home() / '.local/share/gothic-ai/remaster/trees-fir'


def surface_class(material):
    name = material['name']
    kind = material_class(name, material.get('group'))
    if kind == 'ground':
        if is_cave_room(name):
            return 'cave'
        surface = f"{name} {material.get('texture', '')}".upper()
        if any(word in surface for word in ROAD_MARKERS):
            return 'road'
    return kind


def top_surface(triangles, kinds, points):
    """Cast vertical rays against the world mesh, including roof and water surfaces."""
    a, b, c = triangles.transpose(1, 0, 2)
    low, high = triangles[:, :, [0, 2]].min(1), triangles[:, :, [0, 2]].max(1)
    result, heights = np.full(len(points), 'void', dtype=kinds.dtype), np.full(len(points), np.nan)
    tiles, indices = np.unique(np.floor(points / 64), axis=0, return_inverse=True)
    for i, (x, z) in enumerate(tiles):
        rows = np.flatnonzero(indices == i)
        near = np.flatnonzero(((low <= ((x + 1) * 64, (z + 1) * 64)) &
                               (high >= (x * 64, z * 64))).all(axis=1))
        u, v = (a[near] - c[near]).T, (b[near] - c[near]).T
        det = v[2] * u[0] - v[0] * u[2]
        valid = np.abs(det) > 1e-9
        near, u, v, det = near[valid], u[:, valid], v[:, valid], det[valid]
        if not len(near):
            continue
        dx = points[rows, 0, None] - c[near, 0]
        dz = points[rows, 1, None] - c[near, 2]
        l0 = (v[2] * dx - v[0] * dz) / det
        l1 = (u[0] * dz - u[2] * dx) / det
        inside = (l0 >= -1e-9) & (l1 >= -1e-9) & (l0 + l1 <= 1 + 1e-9)
        y = np.where(inside, c[near, 1] + l0 * u[1] + l1 * v[1], -np.inf)
        # Resolve coplanar edges in favor of the exclusion.
        top = y.max(axis=1, keepdims=True)
        blocked = inside & (y >= top - 1e-5) & (kinds[near] != 'ground')
        hit = inside.any(axis=1)
        winner = near[np.where(blocked.any(axis=1), blocked.argmax(axis=1), y.argmax(axis=1))]
        result[rows[hit]] = kinds[winner[hit]]
        heights[rows[hit]] = top[hit, 0]
    return result, heights


def make_layout(triangles, kinds, minimum_height, area_m2):
    rng = np.random.default_rng(SEED)
    horizontal = triangles[:, :, [0, 2]]
    low, high = horizontal.min(axis=(0, 1)), horizontal.max(axis=(0, 1))
    step = np.sqrt(area_m2 / (COUNT * 1.07))
    for step in (step, step * .96, step * .92, step * .88):
        axes = [np.arange(lo, hi + 1) for lo, hi in zip(np.floor(low / step), np.floor(high / step), strict=True)]
        grid = np.stack(np.meshgrid(*axes, indexing='ij'), axis=-1).reshape(-1, 2)
        jitter = min(.25 * step, (step - MIN_GAP_M - .01) / 2)
        assert jitter > 0, 'The candidate grid is too fine for the minimum spacing'
        xz = np.round((grid + .5) * step + rng.uniform(-jitter, jitter, grid.shape), 3)
        surface, y = top_surface(triangles, kinds, (xz[:, None] + ROOT_PROBES_M).reshape(-1, 2))
        dry_ground = (surface == 'ground') & (y > minimum_height)
        usable = dry_ground.reshape(-1, len(ROOT_PROBES_M)).all(axis=1)
        if usable.sum() >= COUNT:
            break
    else:
        raise ValueError('The land grid did not yield 1,024 planting positions')
    selected = rng.choice(np.flatnonzero(usable), COUNT, replace=False)
    heights = y.reshape(-1, len(ROOT_PROBES_M))[selected].min(axis=1)
    points = np.column_stack((xz[selected, 0], heights, xz[selected, 1]))
    xz = xz[selected]
    distances = np.linalg.norm(xz[:, None] - xz[None, :], axis=2)
    distances[np.diag_indices(COUNT)] = np.inf
    assert len(points) == COUNT and np.isfinite(points).all() and distances.min() >= MIN_GAP_M
    assert (top_surface(triangles, kinds, xz)[0] == 'ground').all()
    return points, dict(grid_step_m=float(step), jitter_m=float(jitter), candidate_cells=len(grid),
                        suitable_cells=int(usable.sum()), minimum_gap_m=float(distances.min()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', required=True, type=Path, help='Imported base containing NEWWORLD chunks')
    parser.add_argument('--source', type=Path, default=FIR_ROOT / 'crown-fix/forest-1024')
    parser.add_argument('--out', type=Path, default=FIR_ROOT / 'world-spread')
    args = parser.parse_args()
    if args.out.exists() or args.out.is_symlink():
        parser.error(f'output already exists: {args.out}')
    world = load_world(args.base, WORLD)
    minimum_height = sea_level(args.base, WORLD, mesh=world)
    assert minimum_height is not None, 'NEWWORLD has no ocean surface to set the shore level'
    kinds = np.array([surface_class(material) for material in world.materials])[world.material]
    keep = kinds != 'vegetation'
    triangles, kinds = world.positions[keep] * [-1, 1, 1], kinds[keep]
    cells, height, hit = rasterize(triangles, AREA_CELL_M)
    # Last hit per cell is the uppermost surface; exclusions win ties.
    order = np.lexsort((kinds[hit] != 'ground', np.round(height, 5), cells[:, 1], cells[:, 0]))
    ordered = cells[order]
    last = np.r_[(ordered[1:] != ordered[:-1]).any(axis=1), True]
    top = order[last]
    surfaces = kinds[hit[top]]
    surfaces = np.where((surfaces == 'ground') & (height[top] <= minimum_height), 'below-sea', surfaces)
    labels, counts = np.unique(surfaces, return_counts=True)
    area = {str(label): float(n * AREA_CELL_M**2) for label, n in zip(labels, counts, strict=True)}
    points, grid = make_layout(triangles, kinds, minimum_height, area['ground'])
    again, again_grid = make_layout(triangles, kinds, minimum_height, area['ground'])
    assert np.array_equal(points, again) and grid == again_grid

    data = json.loads((args.source / VOBS).read_text())
    trees = [vob for vob in data['vobs'] if vob.get('visual_name') == 'OWN_TREE_FIR_A_4.3DS']
    assert len(trees) == COUNT and all(vob['vob_name'].startswith('tree-cost-') for vob in trees)
    for vob, point in zip(trees, points, strict=True):
        position = np.round(point * 100, 1)
        offset = position - vob['position']
        vob['position'] = position.tolist()
        vob['bbox'] = {limit: (offset + bound).tolist() for limit, bound in vob['bbox'].items()}

    stats = dict(seed=SEED, base=args.base.name, world=WORLD, count=COUNT, **grid, area_cell_m=AREA_CELL_M,
                 sea_level_m=minimum_height,
                 surface_area_m2=area, trees_per_hectare=COUNT / area['ground'] * 10_000,
                 occupied_chunks=len(np.unique(np.floor(points[:, [0, 2]] / 64), axis=0)),
                 position_bounds_m=[points.min(axis=0).tolist(), points.max(axis=0).tolist()])
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(dir=args.out.parent, prefix='.spread-') as work:
        output = Path(work) / args.out.name
        forest = output / 'forest-1024'
        shutil.copytree(args.source, forest)
        (forest / VOBS).write_text(json.dumps(data, separators=(',', ':')) + '\n')
        (output / 'content').symlink_to(forest.name, target_is_directory=True)
        report = json.dumps(stats, indent=2) + '\n'
        (output / 'layout.json').write_text(report)
        entries = []
        files = sorted(f for f in forest.rglob('*') if f.is_file())
        for file in [*files, output / 'layout.json']:
            with file.open('rb') as stream:
                entries.append(f"{file_digest(stream, 'sha256').hexdigest()}  {file.relative_to(output)}\n")
        (output / 'SHA256SUMS').write_text(''.join(entries))
        output.rename(args.out)
    print(report, end='')


if __name__ == '__main__':
    main()
