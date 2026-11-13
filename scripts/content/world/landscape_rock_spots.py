#!/usr/bin/env python3
"""Places for scanned rocks along the foot of the rock walls of a region: the object layer of a content set.

    python3 scripts/content/world/landscape_rock_spots.py NEWWORLD --rect 4,-1,5,0 \
        --near NW_FARM2_TO_TAVERN_01,NW_FARM2_TO_TAVERN_02,NW_FARM2_TO_TAVERN_03,NW_FARM2_TO_TAVERN_04 \
        --rocks <work>/rocks/rocks.json --out <work>/spots.json

The foot of a wall is where a steep rock triangle shares an edge with walkable ground. A rock stands on the
ground beside such an edge, close enough to lean on the wall; smaller ones gather round the bigger or lie alone
where a big one has no room (a road between two walls). The meshes' scales are the tiers, biggest first. Nothing
stands closer to a waynet edge than its tier allows or closer than OBJECT_CM to an object of the base, so paths
and quest places stay as they are.
The result is the `add` list of `worlds/<WORLD>/vobs.d/*.json` (docs/asset-migration.md); the layer has no scale,
so a small rock is its own mesh. The choice is seeded: the same base gives the same places.
"""
from __future__ import annotations

import argparse
import json
import math
import random
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from world_classes import material_class  # noqa: E402

WALL_DEG = 50.0      # a rock triangle steeper than this is a wall
GROUND_DEG = 25.0    # a rock stands on ground flatter than this
STEP_CM = 150.0      # candidates along a foot edge
NEAR_CM = 3500.0     # around the named waypoints, in the ground plane ...
LEVEL_CM = 800.0     # ... and this close to their height: a gorge under the road is another place
OBJECT_CM = 300.0    # clear of the base's objects
# Per tier, biggest first: centre clear of waynet edges (cm, plus half of the rock's half width), gap to any
# rock placed before it, and how far from a bigger rock it may lie.
TIERS = ((230.0, 420.0, None), (230.0, 170.0, (160.0, 330.0)), (120.0, 90.0, (70.0, 200.0)))
SEED = 20261113


def chunk(path: Path):
    """Triangles of a chunk in Gothic centimetres: (a, b, c, material name, material group)."""
    data = path.read_bytes()
    length = struct.unpack_from('<I', data, 12)[0]
    doc, blob = json.loads(data[20:20 + length]), data[20 + length + 8:]

    def accessor(index):
        a = doc['accessors'][index]
        view = doc['bufferViews'][a['bufferView']]
        offset = view.get('byteOffset', 0) + a.get('byteOffset', 0)
        count = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3}[a['type']]
        code = {5126: 'f', 5125: 'I', 5123: 'H', 5121: 'B'}[a['componentType']]
        stride = view.get('byteStride', struct.calcsize(code) * count)
        return [struct.unpack_from('<' + code * count, blob, offset + i * stride) for i in range(a['count'])]

    for mesh in doc['meshes']:
        for primitive in mesh['primitives']:
            material = doc['materials'][primitive['material']]
            group = (material.get('extras') or {}).get('group')
            points = [(-x, y, z) for x, y, z in accessor(primitive['attributes']['POSITION'])]
            index = [i[0] for i in accessor(primitive['indices'])]
            for t in range(0, len(index), 3):
                yield points[index[t]], points[index[t + 1]], points[index[t + 2]], material['name'], group


def normal(a, b, c):
    u, v = [b[i] - a[i] for i in range(3)], [c[i] - a[i] for i in range(3)]
    n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    length = math.sqrt(sum(x * x for x in n))
    return tuple(x / length for x in n) if length else (0.0, 0.0, 0.0)


def height(triangles, x, z, below):
    """The highest ground under (x, z) not above `below`, with the ground's slope; None over nothing."""
    best = None
    for a, b, c, n in triangles:
        d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2])
        if d == 0:
            continue
        u = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d
        v = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d
        if u < 0 or v < 0 or u + v > 1:
            continue
        y = u * a[1] + v * b[1] + (1 - u - v) * c[1]
        if y <= below and (best is None or y > best[0]):
            best = (y, math.degrees(math.acos(min(1.0, abs(n[1])))))
    return best


def way_distance(x, z, segments):
    best = math.inf
    for (ax, az), (bx, bz) in segments:
        dx, dz = bx - ax, bz - az
        length2 = dx * dx + dz * dz
        t = 0.0 if length2 == 0 else min(1.0, max(0.0, ((x - ax) * dx + (z - az) * dz) / length2))
        best = min(best, math.hypot(x - (ax + t * dx), z - (az + t * dz)))
    return best


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('world')
    parser.add_argument('--base', type=Path)
    parser.add_argument('--rect', required=True, help='X0,Z0,X1,Z1 in chunk coordinates, inclusive')
    parser.add_argument('--near', required=True, help='waypoints the rocks gather round, comma separated')
    parser.add_argument('--rocks', type=Path, required=True, help='rocks.json of mesh3d/blender/rock_set.py')
    parser.add_argument('--count', type=int, nargs='+', default=[8, 10, 30], help='rocks per scale, biggest first')
    parser.add_argument('--key', default='landscape-rock')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()

    base = args.base or next(Path.home().glob('.cache/gothic-ai/base/*/worlds')).parent
    world = base / 'worlds' / args.world
    x0, z0, x1, z1 = map(int, args.rect.split(','))
    walls, ground = [], []
    for cx in range(x0, x1 + 1):
        for cz in range(z0, z1 + 1):
            for a, b, c, name, group in chunk(world / 'chunks' / f'{cx}_{cz}.glb'):
                kind, n = material_class(name, group), normal(a, b, c)
                slope = math.degrees(math.acos(min(1.0, abs(n[1]))))
                if kind == 'rock' and not name.startswith('S:') and slope > WALL_DEG:
                    walls.append((a, b, c))
                elif kind == 'ground' and slope < WALL_DEG:
                    ground.append((a, b, c, n))

    def edge(p, q):
        p, q = tuple(round(v, 1) for v in p), tuple(round(v, 1) for v in q)
        return (p, q) if p <= q else (q, p)

    wall_edges = {edge(t[i], t[(i + 1) % 3]) for t in walls for i in range(3)}
    waynet = json.loads((world / 'waynet.json').read_text())['waynet']
    points = {p['name']: p['position'] for p in waynet['points']}
    order = [(p['position'][0], p['position'][2]) for p in waynet['points']]
    edges = waynet['edges']
    shift = 1 if edges and max(max(e) for e in edges) >= len(order) else 0  # the base numbers points from 1
    segments = [(order[a - shift], order[b - shift]) for a, b in edges]
    centres = [points[name] for name in args.near.split(',')]

    objects = []

    def collect(nodes):
        for node in nodes:
            if node.get('position') and node.get('visual'):
                objects.append((node['position'][0], node['position'][2]))
            collect(node.get('children', []))

    collect(json.loads((world / 'vobs.json').read_text())['vobs'])

    # Candidates: points along a foot edge, pushed out onto the ground side.
    feet = []
    for a, b, c, n in ground:
        middle = [(a[i] + b[i] + c[i]) / 3 for i in range(3)]
        for p, q in ((a, b), (b, c), (c, a)):
            if edge(p, q) not in wall_edges:
                continue
            along = (q[0] - p[0], q[2] - p[2])
            length = math.hypot(*along)
            if length < 1:
                continue
            out = (-along[1] / length, along[0] / length)
            if out[0] * (middle[0] - p[0]) + out[1] * (middle[2] - p[2]) < 0:
                out = (-out[0], -out[1])
            for k in range(max(1, int(length / STEP_CM))):
                t = (k + 0.5) / max(1, int(length / STEP_CM))
                x, y, z = (p[i] + (q[i] - p[i]) * t for i in range(3))
                if any(math.hypot(x - cx, z - cz) <= NEAR_CM and abs(y - cy) <= LEVEL_CM for cx, cy, cz in centres):
                    feet.append((x, y, z, out))

    rocks = json.loads(args.rocks.read_text())
    scales = sorted({r['scale'] for r in rocks}, reverse=True)
    if len(scales) > len(TIERS) or len(args.count) != len(scales):
        raise SystemExit(f'{len(scales)} scales in {args.rocks}: give --count {len(scales)} numbers, at most {len(TIERS)}')
    rng = random.Random(SEED)
    rng.shuffle(feet)
    placed = []  # (x, z)

    def stand(x, z, near_y, rock, way, gap):
        radius = max(rock['size_m'][0], rock['size_m'][1]) * 50
        if way_distance(x, z, segments) < way + radius * 0.5:
            return None
        if any(math.hypot(x - ox, z - oz) < OBJECT_CM for ox, oz in objects):
            return None
        if any(math.hypot(x - px, z - pz) < gap for px, pz in placed):
            return None
        found = height(ground, x, z, near_y + 250)
        if found is None or found[1] > GROUND_DEG or abs(found[0] - near_y) > 250:
            return None
        placed.append((x, z))
        return dict(visual=rock['visual'], position=[round(x, 1), round(found[0] - 8 * rock['scale'], 1), round(z, 1)],
                    yaw_deg=round(rng.uniform(0, 360), 1))

    added, anchors, counts = [], [], []
    for scale, count, (way, gap, reach) in zip(scales, args.count, TIERS):
        tier = [r for r in rocks if r['scale'] == scale]
        made, tries = [], 0
        while len(made) < count and tries < max(count, 1) * 80 and feet:
            tries += 1
            rock = rng.choice(tier)
            if reach and anchors and tries % 2:  # beside a bigger rock
                (ax, ay, az), out = rng.choice(anchors)
                angle = math.atan2(out[1], out[0]) + rng.uniform(-1.9, 1.9)
                distance = rng.uniform(*reach)
                spot = stand(ax + math.cos(angle) * distance, az + math.sin(angle) * distance, ay, rock, way, gap)
            else:                                # at the foot of the wall, leaning on it
                x, y, z, out = feet[tries % len(feet)]
                distance = max(rock['size_m'][0], rock['size_m'][1]) * 50 * rng.uniform(0.35, 0.9)
                spot = stand(x + out[0] * distance, z + out[1] * distance, y, rock, way, gap)
            if spot:
                made.append((spot, out))
        added += [spot for spot, _ in made]
        anchors += [(spot['position'], out) for spot, out in made]
        counts.append(len(made))
    for number, spot in enumerate(added, 1):
        spot.update(key=f'{args.key}-{number:02d}', kind='mesh', collision='mesh')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(dict(format=1, add=added), indent=1) + '\n')
    print(f'{len(walls)} wall triangles, {len(feet)} foot candidates, rocks per scale {dict(zip(scales, counts))} '
          f'-> {args.out}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
