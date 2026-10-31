#!/usr/bin/env python3
"""Lay out the harbour's traces of life (asset harbour-life, step M4 of docs/world-detail-plan.md) as a vob layer.

    uv run --no-project --with numpy python scripts/content/harbour_life.py layer --set life-1 --ground-set harbour-life-1 \
        --copy-to harbour-life-1
    uv run --no-project --with numpy python scripts/content/harbour_life.py ground X Z [Y_FROM]

`layer` reads the placements (data/content/world/harbour-life-placements.json), puts every item on the world
mesh and writes the layer `worlds/NEWWORLD/vobs.d/harbour-life.json`: into the staging set `--set` (the props
set, life_set.py), into each `--copy-to` set and as the copy in git, data/content/world/NEWWORLD/vobs.d/.
The ground and the models' boxes come from `--ground-set`, the working content with the props composed in.

A placement names a visual, the spot (x, z in Gothic centimetres) and a yaw. Its height comes from the world
mesh under the visual's footprint: the highest of the centre and the four corners, so a corner may float a
little on a slope but nothing sinks into the stones; `sink` (cm) lowers it. `on` puts it on the top of an
earlier placement instead (a crate on a crate). `tilt` [about x, about z] in degrees leans it after the yaw:
a load that was dumped, not set down. The ground is the base world plus the chunks of the staging set (the
crane decks and stairs of the working content).

The visual's box comes from the staging set's own model (meshes.json) or from the base mesh, the same
lookup as the engine's: own GLBs are metres around their origin, base meshes carry extras.gothic.bbox in cm.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import struct
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/content/world'))
import chunk_glb  # noqa: E402

DATA = Path.home() / '.local/share/gothic-ai'
STAGING = DATA / 'content-staging'
PLACEMENTS = ROOT / 'data/content/world/harbour-life-placements.json'
LAYER_COPY = ROOT / 'data/content/world/NEWWORLD/vobs.d/harbour-life.json'
LAYER_PATH = Path('worlds/NEWWORLD/vobs.d/harbour-life.json')
# the harbour and the poor quarter, with a margin (Gothic cm, x and z)
AREA = (-11000.0, 7000.0, -8000.0, 4000.0)
CELL = 200.0


def base_root() -> Path:
    if os.environ.get('OPENGOTHIC_BASE'):
        return Path(os.environ['OPENGOTHIC_BASE'])
    cache = Path(os.environ.get('OPENGOTHIC_BASE_CACHE', Path.home() / '.cache/gothic-ai/base'))
    # the newest imported base; OPENGOTHIC_BASE picks another
    return max((p for p in cache.iterdir() if len(p.name) == 64 and (p / 'worlds/NEWWORLD/chunks').is_dir()),
               key=lambda p: (p / 'base.json').stat().st_mtime)


def glb_json(path: Path) -> dict:
    data = path.read_bytes()
    length = struct.unpack('<I', data[12:16])[0]
    return json.loads(data[20:20 + length])


class Ground:
    """Triangles of the world mesh in the area, Gothic cm, with a grid over their XZ boxes."""

    def __init__(self, set_root: Path | None):
        chunks = {}
        for path in sorted((base_root() / 'worlds/NEWWORLD/chunks').glob('*.glb')):
            chunks[path.name] = path
        if set_root is not None:
            for path in sorted((set_root / 'worlds/NEWWORLD/chunks').glob('*.glb')):
                chunks[path.name] = path
        tris = []
        for path in chunks.values():
            doc = glb_json(path)
            boxes = [doc['accessors'][p['attributes']['POSITION']] for p in doc['meshes'][0]['primitives']]
            lo, hi = -max(b['max'][0] for b in boxes), -min(b['min'][0] for b in boxes)
            zlo, zhi = min(b['min'][2] for b in boxes), max(b['max'][2] for b in boxes)
            if hi < AREA[0] or lo > AREA[1] or zhi < AREA[2] or zlo > AREA[3]:
                continue
            for prim in chunk_glb.read_chunk(path).primitives:
                p = prim.positions.astype(np.float64)
                p[..., 0] *= -1.0  # glTF -x is Gothic x
                tris.append(p)
        self.tris = np.concatenate(tris)
        lo = self.tris[..., [0, 2]].min(axis=1)
        hi = self.tris[..., [0, 2]].max(axis=1)
        # the waypoints say where people walk: the floor of a spot is the surface just under the nearest one,
        # not a roof or an eave above it, nor the sea bed under the quay
        points = json.loads((base_root() / 'worlds/NEWWORLD/waynet.json').read_text())['waynet']['points']
        self.waypoints = np.array([p['position'] for p in points], dtype=float)
        self.grid: dict[tuple[int, int], list[int]] = {}
        for i, (a, b) in enumerate(zip(np.floor(lo / CELL).astype(int), np.floor(hi / CELL).astype(int))):
            for gx in range(a[0], b[0] + 1):
                for gz in range(a[1], b[1] + 1):
                    self.grid.setdefault((gx, gz), []).append(i)

    def heights(self, x: float, z: float) -> list[float]:
        """Heights of every triangle above or below (x, z), highest first."""
        idx = self.grid.get((math.floor(x / CELL), math.floor(z / CELL)), [])
        out = []
        for t in self.tris[idx]:
            (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = t
            d = (z1 - z2) * (x0 - x2) + (x2 - x1) * (z0 - z2)
            if abs(d) < 1e-9:
                continue
            a = ((z1 - z2) * (x - x2) + (x2 - x1) * (z - z2)) / d
            b = ((z2 - z0) * (x - x2) + (x0 - x2) * (z - z2)) / d
            c = 1.0 - a - b
            if min(a, b, c) >= -1e-6:
                out.append(a * y0 + b * y1 + c * y2)
        return sorted(out, reverse=True)

    def wall(self, x: float, z: float, floor: float, reach: float = 300.0) -> tuple[np.ndarray, np.ndarray, float]:
        """The nearest wall to (x, z) standing on this floor: (its foot point, its direction, the distance).
        A wall is a steep triangle at least 0.3 m tall that reaches into 0.2-2 m above the floor; seen from above it is a segment."""
        best = None
        cells = int(math.ceil(reach / CELL))
        gx, gz = math.floor(x / CELL), math.floor(z / CELL)
        seen = set()
        p = np.array([x, z])
        for dx in range(-cells, cells + 1):
            for dz in range(-cells, cells + 1):
                for i in self.grid.get((gx + dx, gz + dz), []):
                    if i in seen:
                        continue
                    seen.add(i)
                    t = self.tris[i]
                    n = np.cross(t[1] - t[0], t[2] - t[0])
                    if np.linalg.norm(n) < 1e-6 or abs(n[1]) / np.linalg.norm(n) > 0.3:
                        continue
                    if t[:, 1].min() > floor + 200 or t[:, 1].max() < floor + 20 or np.ptp(t[:, 1]) < 30:
                        continue
                    flat = t[:, [0, 2]]
                    pair = max(((a, b) for a in range(3) for b in range(a + 1, 3)),
                               key=lambda ab: np.linalg.norm(flat[ab[0]] - flat[ab[1]]))
                    a, b = flat[pair[0]], flat[pair[1]]
                    d = b - a
                    length = np.linalg.norm(d)
                    if length < 20:
                        continue
                    s = np.clip(np.dot(p - a, d) / length ** 2, 0, 1)
                    foot = a + d * s
                    dist = np.linalg.norm(p - foot)
                    if dist <= reach and (best is None or dist < best[2]):
                        best = (foot, d / length, dist)
        if best is None:
            raise ValueError(f'no wall within {reach:.0f} cm of ({x:.0f}, {z:.0f})')
        return best

    def floor(self, x: float, z: float) -> float:
        """The walking floor at (x, z): the first surface under the nearest waypoint's height plus 0.6 m."""
        d = ((self.waypoints[:, [0, 2]] - np.array([x, z])) ** 2).sum(axis=1)
        return self.height(x, z, float(self.waypoints[int(np.argmin(d)), 1]) + 60.0)

    def height(self, x: float, z: float, below: float) -> float:
        """The first surface under `below` at (x, z)."""
        for y in self.heights(x, z):
            if y <= below:
                return y
        raise ValueError(f'no ground under ({x:.0f}, {below:.0f}, {z:.0f})')


class Visuals:
    """Box of a visual, cm around its origin, in its own axes (Gothic x, y, z)."""

    def __init__(self, set_root: Path | None):
        self.own = {}
        index = set_root / 'meshes/meshes.json' if set_root is not None else None
        if index is not None and index.is_file():
            for name, entry in json.loads(index.read_text()).items():
                self.own[name.upper()] = set_root / 'meshes' / entry['glb']
        self.base = base_root() / 'meshes'
        self.cache: dict[str, tuple[np.ndarray, np.ndarray]] = {}

    def box(self, name: str) -> tuple[np.ndarray, np.ndarray]:
        key = name.upper()
        if key not in self.cache:
            if key in self.own:
                doc = glb_json(self.own[key])
                lo = np.full(3, np.inf)
                hi = np.full(3, -np.inf)
                for mesh in doc['meshes']:
                    for prim in mesh['primitives']:
                        acc = doc['accessors'][prim['attributes']['POSITION']]
                        lo = np.minimum(lo, acc['min'])
                        hi = np.maximum(hi, acc['max'])
                lo, hi = lo * 100.0, hi * 100.0
                lo[0], hi[0] = -hi[0], -lo[0]  # glTF x is mirrored
            else:
                path = self.base / f'{key}.glb'
                if not path.is_file():
                    raise ValueError(f'{name}: no own model and no base mesh')
                lo, hi = (np.array(v, dtype=float) for v in glb_json(path)['meshes'][0]['extras']['gothic']['bbox'])
            self.cache[key] = (lo, hi)
        return self.cache[key]


def rotation(yaw: float, tilt: tuple[float, float]) -> np.ndarray:
    """World-from-local rotation, rows as the layer writes them: yaw about Y, then a lean about X and Z."""
    c, s = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    m = np.array([[c, 0.0, s], [0.0, 1.0, 0.0], [-s, 0.0, c]])
    ax, az = (math.radians(a) for a in tilt)
    rx = np.array([[1, 0, 0], [0, math.cos(ax), -math.sin(ax)], [0, math.sin(ax), math.cos(ax)]])
    rz = np.array([[math.cos(az), -math.sin(az), 0], [math.sin(az), math.cos(az), 0], [0, 0, 1]])
    return rz @ rx @ m


def place(spec: list[dict], ground: Ground, visuals: Visuals) -> tuple[list[dict], list[dict]]:
    """Layer entries and a report row per placement."""
    tops: dict[str, float] = {}
    entries, report = [], []
    for item in spec:
        lo, hi = visuals.box(item['visual'])
        x, z = float(item['x']), float(item['z'])
        yaw = float(item.get('yaw', 0.0))
        if 'wall' in item:
            # against the nearest wall: local X along it, the box's near side `wall` cm off it, then the
            # yaw is turned by item['yaw'] (a dumped thing is never quite square to the wall)
            floor0 = ground.height(x, z, float(item['from'])) if 'from' in item else ground.floor(x, z)
            foot, along, _ = ground.wall(x, z, floor0, float(item.get('reach', 400)))
            out = np.array([x, z]) - foot
            out /= np.linalg.norm(out)
            base = math.degrees(math.atan2(-along[1], along[0]))
            # which local side (+z or -z) faces the wall decides how far the origin stands off it
            local_z = np.array([math.sin(math.radians(base)), math.cos(math.radians(base))])
            depth = -lo[2] if np.dot(local_z, out) > 0 else hi[2]
            x, z = (foot + out * (float(item['wall']) + depth))
            x, z = float(x), float(z)
            yaw = base + yaw
        tilt = tuple(item.get('tilt', (0.0, 0.0)))
        m = rotation(yaw, tilt)
        # the footprint's corners in the world, from the local box at its floor
        corners = [m @ np.array([cx, lo[1], cz]) for cx in (lo[0], hi[0]) for cz in (lo[2], hi[2])]
        if 'on' in item:
            floor = tops[item['on']]
            spots = [floor]
        else:
            start = float(item['from']) if 'from' in item else ground.floor(x, z) + 40.0
            centre = ground.height(x, z, start)
            spots = [centre]
            for p in corners:  # a corner takes the surface nearest the centre's (a step, a kerb), or the centre's
                near = [h for h in ground.heights(x + p[0], z + p[2]) if centre - 80 <= h <= centre + 40]
                spots.append(min(near, key=lambda h: abs(h - centre)) if near else centre)
            floor = max(spots)
        # the lowest local corner after the lean touches the floor
        drop = min((m @ np.array([cx, cy, cz]))[1] for cx in (lo[0], hi[0]) for cy in (lo[1], hi[1]) for cz in (lo[2], hi[2]))
        y = floor - drop - float(item.get('sink', 0.0)) + float(item.get('lift', 0.0))
        tops[item['key']] = y + max((m @ np.array([cx, cy, cz]))[1] for cx in (lo[0], hi[0])
                                    for cy in (lo[1], hi[1]) for cz in (lo[2], hi[2]))
        entry = dict(key=item['key'], kind='mesh', visual=item['visual'], position=[round(x, 1), round(y, 1), round(z, 1)])
        if tilt == (0.0, 0.0):
            entry['yaw_deg'] = round(yaw, 1)
        else:
            entry['rotation'] = [[round(float(v), 6) for v in row] for row in m]
        entry['collision'] = item.get('collision', 'mesh')
        entries.append(entry)
        report.append(dict(key=item['key'], zone=item.get('zone', ''), spread=round(max(spots) - min(spots), 1)))
    return entries, report


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    a = sub.add_parser('layer')
    a.add_argument('--set', required=True, help='staging set that gets the layer (its models and chunks count)')
    a.add_argument('--ground-set', help='staging set whose chunks and models are the ground (default: --set)')
    a.add_argument('--copy-to', action='append', default=[], help='another staging set that gets the same layer file')
    a.add_argument('--placements', type=Path, default=PLACEMENTS)
    g = sub.add_parser('ground')
    g.add_argument('x', type=float)
    g.add_argument('z', type=float)
    g.add_argument('below', type=float, nargs='?', default=1e5)
    g.add_argument('--set')
    args = ap.parse_args()
    if args.cmd == 'ground':
        ground = Ground(STAGING / args.set if args.set else None)
        print([round(h, 1) for h in ground.heights(args.x, args.z)])
        return 0
    target = STAGING / args.set
    source = STAGING / (args.ground_set or args.set)
    spec = json.loads(args.placements.read_text())
    entries, report = place(spec['items'], Ground(source), Visuals(source))
    layer = json.dumps(dict(format=1, add=entries), indent=1, ensure_ascii=False) + '\n'
    for path in (target / LAYER_PATH, LAYER_COPY, *(STAGING / name / LAYER_PATH for name in args.copy_to)):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(layer)
    steep = [r for r in report if r['spread'] > 15]
    print(json.dumps(dict(items=len(entries), layer=str(target / LAYER_PATH), uneven=steep), ensure_ascii=False))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
