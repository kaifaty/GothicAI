#!/usr/bin/env python3
"""Places for grass tufts along the grass band of the rock strips of a region: the object layer of a content set.

    uv run --no-project --with numpy --with scipy python scripts/content/world/landscape_tuft_spots.py NEWWORLD \
        --rect 4,-1,5,0 --materials <set>/materials/materials.json --tufts <set>/grass-tufts.json --out <layer>.json

Step 2 of reports/engine/2026-11-13-grass/README.md. The band is the `fringe` layer of the strip textures
(landscape_grass_layer.py): grass from the strip's top edge (mesh v = 0) down to a boundary the shader moves by
world noise. This script repeats that arithmetic of `projectionFringe` (material_projection.glsl) on points
scattered over the strips' triangles of the base: the clamp of v, the hash, the noise and the wander of the
boundary are the shader's own, so a point's `t` here is the pixel's there: t <= 0 is solid grass, 0..1 the zone
where the layer thins out (grass on the share 1 - t of the texels), above 1 rock. What is not repeated is the
layer's height map, which says which texel of the zone is grass: a root may stand on a bare texel of it.

Four populations, all seeded, the same base gives the same places:
- fringe: roots in the zone where the layer thins out (t from 0.1 to 0.7), in clumps with gaps by a noise of a few
  metres; the leaves hang from there over the rest of the zone and the rock;
- crease: along the strip's top edge, where the wall meets the ground above it: grass that stands over the edge;
- band: thinly across the solid band;
- foot: sparse upright grass in the band at the foot of a wall (`bottom` of the record).

A tuft's own axes (glb_cards.py): X along the wall, Y up the surface, Z out of it; the layer's `rotation` turns
them to the place. "Down the surface" is gravity laid into the triangle's plane, a quarter of it the direction
of growing v (they differ by 9 degrees at the median), or growing v alone on a surface flatter than 20 degrees.
A hanging tuft is then aimed at the rock found 0.7 of its length down, so it follows a bend between triangles;
where the rock there turns away by more than BEND_DEG, or the tip would hang in the air or sink, the tuft gives
place to a shorter shape.
"""
from __future__ import annotations

import argparse
import json
import math
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from world_classes import material_class  # noqa: E402

SEED = 20261113
# projectionFringe, material_projection.glsl
WANDER, WANDER_CM, OVER_TOP, OVER_FOOT = .45, 900., .05, .5

SAMPLES_M2 = 60.          # candidate points per square metre of a strip
CLOUD_CM = 9.             # spacing of the points the rock around is known by
ROOT_T = (.1, .7)         # the fringe's roots, in t
EDGE_CM = (3., 16.)       # the crease's roots below the top edge
FLAT_UP = .8              # normal's Y above which nothing hangs: the surface is ground
BEND_DEG = 40.
SUNK_CM, AIR_CM = 6., 35.
SWING_DEG = 14.
# population: share of the candidates kept where the clump noise is high, the noise's range that maps to 0..1,
# spacing inside the population and to tufts placed before it (cm), wavelength of the clumps (cm), shapes.
POPULATIONS = dict(
    fringe=dict(keep=1., noise=(.38, .6), gap=46., other=30., wave=330.,
                shapes=(('LONG', .3), ('MID', .32), ('WISP', .16), ('DROOP', .22))),
    crease=dict(keep=1., noise=(.4, .62), gap=36., other=30., wave=420.,
                shapes=(('DROOP', .45), ('CLUMP', .55))),
    band=dict(keep=.5, noise=(.45, .7), gap=100., other=45., wave=500.,
              shapes=(('CLUMP', .5), ('DROOP', .35), ('MID', .15))),
    foot=dict(keep=.5, noise=(.45, .7), gap=80., other=45., wave=500., shapes=(('CLUMP', 1.),)),
)
SHORTER = dict(LONG='MID', WISP='MID', MID='DROOP')
BIG = .55                 # share of the full-size tufts; the rest take the smaller mesh


def chunk(path: Path):
    """Primitives of a chunk in Gothic centimetres: (material name, group, positions, normals, uv) per triangle."""
    data = path.read_bytes()
    length = struct.unpack_from('<I', data, 12)[0]
    doc, blob = json.loads(data[20:20 + length]), data[20 + length + 8:]

    def accessor(index):
        a = doc['accessors'][index]
        view = doc['bufferViews'][a['bufferView']]
        count = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3}[a['type']]
        kind = np.dtype({5126: '<f4', 5125: '<u4', 5123: '<u2', 5121: 'u1'}[a['componentType']])
        stride = view.get('byteStride', kind.itemsize * count)
        offset = view.get('byteOffset', 0) + a.get('byteOffset', 0)
        raw = np.frombuffer(blob, np.uint8, (a['count'] - 1) * stride + kind.itemsize * count, offset)
        rows = np.lib.stride_tricks.as_strided(raw, (a['count'], kind.itemsize * count), (stride, 1))
        return np.ascontiguousarray(rows).view(kind).reshape(a['count'], count).astype(np.float64 if kind.kind == 'f' else np.int64)

    for mesh in doc['meshes']:
        for primitive in mesh['primitives']:
            material = doc['materials'][primitive['material']]
            index = accessor(primitive['indices'])[:, 0].reshape(-1, 3)
            mirror = np.array((-1., 1., 1.))
            position = accessor(primitive['attributes']['POSITION']) * mirror
            normal = accessor(primitive['attributes']['NORMAL']) * mirror
            uv = accessor(primitive['attributes']['TEXCOORD_0'])
            yield (material['name'], (material.get('extras') or {}).get('group'),
                   position[index], normal[index], uv[index])


def unit(v):
    return v / np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-12)


def fringe_hash(cx, cy):
    h = (cx.astype(np.int64).astype(np.uint32) * np.uint32(0x8da6b343)) \
        ^ (cy.astype(np.int64).astype(np.uint32) * np.uint32(0xd8163841))
    h = (h ^ (h >> np.uint32(16))) * np.uint32(0x7feb352d)
    h = (h ^ (h >> np.uint32(15))) * np.uint32(0x846ca68b)
    return ((h ^ (h >> np.uint32(16))) & np.uint32(0xffff)).astype(np.float64) * (2. / 65535.) - 1.


def fringe_noise(x, y):
    cx, cy = np.floor(x), np.floor(y)
    fx, fy = x - cx, y - cy
    fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    low = fringe_hash(cx, cy) * (1 - fx) + fringe_hash(cx + 1, cy) * fx
    high = fringe_hash(cx, cy + 1) * (1 - fx) + fringe_hash(cx + 1, cy + 1) * fx
    return low * (1 - fy) + high * fy


def fringe_t(point, v_mesh, record):
    """(t of the top band, t of the bottom band) of the shader at world points with the mesh's v."""
    v = np.where((v_mesh >= -OVER_TOP) & (v_mesh <= 1 + OVER_FOOT), np.clip(v_mesh, 0, 1), v_mesh - np.floor(v_mesh))
    x, z = point[:, 0] / WANDER_CM, point[:, 2] / WANDER_CM
    n = .8 * fringe_noise(x, z) + .2 * fringe_noise(x * 4 + 17, z * 4 + 17)
    m = 1 - WANDER * (.5 + .5 * n)
    result = []
    for band, along in ((record.get('top'), v), (record.get('bottom'), 1 - v)):
        if band and band[1] > 0:
            result.append((along - band[0] * m) / np.maximum((band[1] - band[0]) * m, 1e-4))
        else:
            result.append(np.full(len(point), 2.))
    return result


def clump_noise(point, wave, shift):
    """0..1, the same kind of noise at the scale of a clump; `shift` parts the populations."""
    x, z = point[:, 0] / wave + shift, point[:, 2] / wave + shift * 1.7 + point[:, 1] / (wave * 1.3)
    return .5 + .5 * (.7 * fringe_noise(x, z) + .3 * fringe_noise(x * 3.1 + 5, z * 3.1 + 5))


def scatter(rng, triangles, per_cm2):
    """Random points on triangles: (index of the triangle, barycentric weights)."""
    area = np.linalg.norm(np.cross(triangles[:, 1] - triangles[:, 0], triangles[:, 2] - triangles[:, 0]), axis=1) / 2
    count = rng.poisson(area * per_cm2)
    owner = np.repeat(np.arange(len(triangles)), count)
    a, b = rng.random(len(owner)), rng.random(len(owner))
    flip = a + b > 1
    a, b = np.where(flip, 1 - a, a), np.where(flip, 1 - b, b)
    return owner, np.stack([1 - a - b, a, b], axis=1)


class Spacing:
    """Accepted points on a grid: is a new one clear of them by a radius."""
    CELL = 45.

    def __init__(self):
        self.cells = {}

    def key(self, p):
        return tuple(int(math.floor(c / self.CELL)) for c in p)

    def clear(self, p, radius, kinds):
        reach = int(math.ceil(radius / self.CELL))
        cx, cy, cz = self.key(p)
        for x in range(cx - reach, cx + reach + 1):
            for y in range(cy - reach, cy + reach + 1):
                for z in range(cz - reach, cz + reach + 1):
                    for q, kind in self.cells.get((x, y, z), ()):
                        if kind in kinds and (q[0] - p[0]) ** 2 + (q[1] - p[1]) ** 2 + (q[2] - p[2]) ** 2 < radius ** 2:
                            return False
        return True

    def add(self, p, kind):
        self.cells.setdefault(self.key(p), []).append((p, kind))


def place(world: Path, rect, records: dict, tufts: list, key: str = 'landscape-grass-tuft'):
    """records: texture name -> its `fringe` record; tufts: dicts with visual, shape, scale, reach_cm (how far the
    leaves hang below the root; 0 for an upright one). Returns the layer's `add` list and the numbers."""
    from scipy.spatial import cKDTree
    rng = np.random.default_rng(SEED)
    x0, z0, x1, z1 = rect
    strips, solid = [], []
    for cx in range(x0, x1 + 1):
        for cz in range(z0, z1 + 1):
            for name, group, position, normal, uv in chunk(world / 'chunks' / f'{cx}_{cz}.glb'):
                face = np.cross(position[:, 1] - position[:, 0], position[:, 2] - position[:, 0])
                keep = np.linalg.norm(face, axis=1) > 1e-3
                position, normal, uv, face = position[keep], normal[keep], uv[keep], unit(face[keep])
                face *= np.where((face * normal.mean(axis=1)).sum(axis=1) < 0, -1., 1.)[:, None]
                if name in records:
                    strips.append((name, position, face, uv[..., 1]))
                if material_class(name, group) in ('rock', 'ground'):
                    solid.append((position, face))
    if not strips:
        raise SystemExit(f'{world}: no triangles of {sorted(records)} in the rect')
    names = np.concatenate([[name] * len(position) for name, position, _, _ in strips])
    triangle = np.concatenate([position for _, position, _, _ in strips])
    normal = np.concatenate([face for _, _, face, _ in strips])
    v_corner = np.concatenate([v for _, _, _, v in strips])

    # the direction of growing v in the triangle's plane, and "down the surface"
    edge1, edge2 = triangle[:, 1] - triangle[:, 0], triangle[:, 2] - triangle[:, 0]
    system = np.stack([edge1, edge2, normal], axis=1)
    rise = np.stack([v_corner[:, 1] - v_corner[:, 0], v_corner[:, 2] - v_corner[:, 0], np.zeros(len(triangle))], axis=1)
    grow = unit(np.linalg.solve(system, rise[..., None])[..., 0])
    gravity = np.array((0., -1., 0.)) - normal * -normal[:, 1:2]
    steep = np.linalg.norm(gravity, axis=1) > math.sin(math.radians(20))
    agree = (grow * gravity).sum(axis=1) > 0
    down = np.where(steep[:, None], unit(unit(gravity) * .75 + np.where(agree[:, None], grow, 0) * .25), grow)
    down = unit(down - normal * (down * normal).sum(axis=1, keepdims=True))

    owner, weight = scatter(rng, triangle, SAMPLES_M2 / 1e4)
    point = (triangle[owner] * weight[..., None]).sum(axis=1)
    v_mesh = (v_corner[owner] * weight).sum(axis=1)
    t_top, t_bottom = np.full(len(point), 2.), np.full(len(point), 2.)
    for name, record in records.items():
        of = names[owner] == name
        t_top[of], t_bottom[of] = fringe_t(point[of], v_mesh[of], record)

    # the crease: points below the top edges, the edges with v = 0 at both ends
    edge_owner, edge_point = [], []
    for a, b, c in ((0, 1, 2), (1, 2, 0), (2, 0, 1)):
        top = (np.abs(v_corner[:, a]) < .03) & (np.abs(v_corner[:, b]) < .03) & (v_corner[:, c] > .03)
        for index in np.flatnonzero(top):
            p, q, r = triangle[index, a], triangle[index, b], triangle[index, c]
            along = unit(q - p)
            inward = unit((r - p) - along * ((r - p) @ along))
            steps = max(int(np.linalg.norm(q - p) / 8), 1)
            share = (np.arange(steps) + rng.random(steps)) / steps
            edge_point.append(p + (q - p) * share[:, None] + inward * rng.uniform(*EDGE_CM, steps)[:, None])
            edge_owner.append(np.full(steps, index))
    edge_owner = np.concatenate(edge_owner) if edge_owner else np.zeros(0, int)
    edge_point = np.concatenate(edge_point) if edge_point else np.zeros((0, 3))

    # the rock and ground around the strips, as points with normals
    cloud, cloud_normal = [], []
    cell = 300.
    near = {tuple(k) for k in np.unique(np.floor(point / cell).astype(int), axis=0)}
    near = {(x + dx, y + dy, z + dz) for x, y, z in near for dx in (-1, 0, 1) for dy in (-1, 0, 1) for dz in (-1, 0, 1)}
    for position, face in solid:
        index, bary = scatter(rng, position, 1 / CLOUD_CM ** 2)
        points = (position[index] * bary[..., None]).sum(axis=1)
        inside = np.fromiter((tuple(k) in near for k in np.floor(points / cell).astype(int)), bool, len(points))
        cloud.append(points[inside])
        cloud_normal.append(face[index][inside])
    cloud, cloud_normal = np.concatenate(cloud), np.concatenate(cloud_normal)
    tree = cKDTree(cloud)

    by_shape = {}
    for tuft in tufts:
        by_shape.setdefault(tuft['shape'], []).append(tuft)
    for shape in by_shape:
        by_shape[shape].sort(key=lambda tuft: -tuft['scale'])

    def fit(p, n, d, tuft):
        """The tuft's axes at a root: (down, normal), or None where the rock does not carry its length."""
        reach = tuft['reach_cm']
        if reach <= 0:
            return d, n
        _, i = tree.query(p + d * reach * .7 + n * 4)
        chord = cloud[i] - p
        if np.linalg.norm(chord) < reach * .35:
            return None
        aim = unit(chord)
        if aim @ d < math.cos(math.radians(BEND_DEG)):
            return None
        out = unit(n + cloud_normal[i])
        out = unit(out - aim * (out @ aim))
        tip = p + aim * reach + out * 3
        distance, j = tree.query(tip)
        if distance > AIR_CM or (tip - cloud[j]) @ cloud_normal[j] < -SUNK_CM:
            return None
        return aim, out

    populations = dict(
        fringe=(point, owner, (t_top >= ROOT_T[0]) & (t_top <= ROOT_T[1]) & (t_top <= t_bottom)),
        crease=(edge_point, edge_owner, np.ones(len(edge_point), bool)),
        band=(point, owner, (t_top < ROOT_T[0]) & (v_mesh > .03)),
        foot=(point, owner, (t_bottom < .3) & (t_bottom < t_top)),
    )
    spacing, added, counts, shapes_used = Spacing(), [], {}, {}
    for number, (name, (points, owners, mask)) in enumerate(populations.items()):
        rule = POPULATIONS[name]
        chosen = np.flatnonzero(mask)
        if not len(chosen):
            counts[name] = 0
            continue
        noise = clump_noise(points[chosen], rule['wave'], 31.7 * (number + 1))
        chance = np.clip((noise - rule['noise'][0]) / (rule['noise'][1] - rule['noise'][0]), 0, 1) * rule['keep']
        chosen = chosen[rng.random(len(chosen)) < chance]
        rng.shuffle(chosen)
        labels, shares = zip(*rule['shapes'])
        made = 0
        for index in chosen:
            p, n, d = points[index], normal[owners[index]], down[owners[index]]
            if not spacing.clear(p, rule['gap'], (name,)) or not spacing.clear(p, rule['other'], counts.keys()):
                continue
            shape = str(rng.choice(labels, p=shares))
            if n[1] > FLAT_UP and shape in SHORTER:
                shape = 'DROOP'
            size = 0 if rng.random() < BIG else -1
            axes = None
            while True:
                tuft = by_shape[shape][size]
                axes = fit(p, n, d, tuft)
                if axes is not None or shape not in SHORTER:
                    break
                shape = SHORTER[shape]
            if axes is None:
                axes = (d, n)       # the shortest shape stands anywhere
            aim, out = axes
            swing = math.radians(rng.uniform(-SWING_DEG, SWING_DEG))
            up = -aim
            side = np.cross(up, out)
            side, up = side * math.cos(swing) + up * math.sin(swing), up * math.cos(swing) - side * math.sin(swing)
            rotation = np.stack([side, up, out], axis=1)     # columns: the tuft's X, Y, Z in the world
            added.append(dict(key=f'{key}-{len(added) + 1:04d}', kind='mesh', visual=tuft['visual'],
                              position=[round(float(c), 1) for c in p],
                              rotation=[[round(float(c), 5) for c in row] for row in rotation],
                              collision='none'))
            spacing.add(tuple(p), name)
            shapes_used[tuft['visual']] = shapes_used.get(tuft['visual'], 0) + 1
            made += 1
        counts[name] = made
    triangles = {tuft['visual']: tuft['triangles'] for tuft in tufts}
    zone = (t_top > 0) & (t_top < 1)
    stats = dict(seed=SEED, strips={name: int((names == name).sum()) for name in records},
                 strip_area_m2=round(float(np.linalg.norm(np.cross(edge1, edge2), axis=1).sum() / 2e4), 1),
                 band_area_m2=round(float((np.minimum(t_top, t_bottom) <= 0).sum() / SAMPLES_M2), 1),
                 thinning_area_m2=round(float(zone.sum() / SAMPLES_M2), 1),
                 top_edge_m=round(len(edge_point) * .08, 1), cloud_points=len(cloud),
                 placed=counts, tufts=len(added), by_visual=dict(sorted(shapes_used.items())),
                 triangles=sum(triangles[visual] * count for visual, count in shapes_used.items()))
    return added, stats


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('world')
    parser.add_argument('--base', type=Path)
    parser.add_argument('--rect', required=True, help='X0,Z0,X1,Z1 in chunk coordinates, inclusive')
    parser.add_argument('--materials', type=Path, required=True, help='materials.json of the set: the fringe records')
    parser.add_argument('--tufts', type=Path, required=True, help='grass-tufts.json of landscape_grass_tufts.py')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    base = args.base or next(Path.home().glob('.cache/gothic-ai/base/*/worlds')).parent
    records = {name.removesuffix('.TGA'): record['fringe']
               for name, record in json.loads(args.materials.read_text()).items() if 'fringe' in record}
    added, stats = place(base / 'worlds' / args.world, tuple(map(int, args.rect.split(','))), records,
                         json.loads(args.tufts.read_text())['tufts'])
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(dict(format=1, add=added), indent=1) + '\n')
    print(json.dumps(stats, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
