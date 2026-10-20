"""Build two coils of thick hemp rope (NW_HARBOUR_ROPE_01) from an image_gen concept and a numeric target.

blender -b --factory-startup -P rope.py -- rope.json OUT_DIR   (run from the repository root)
config: {"id": "rope", "target": "data/content/mesh3d-market.json", "seed": 1, "thick": 0.065, "sides": 5,
         "segment": 0.18, "coil": 0.74, "hole": 0.07, "layers": 3, "outer_turns": 1, "overlap": 0.08,
         "tail": [0.6, 0.2], "tile_m": 0.15}
Each coil is a tube laid along flat spirals: the top layer runs from the centre
hole to the rim, the layers under it only show their two outer turns. The first
coil lies flat; the second rests one edge on the first coil's top and the other
on the ground, so the pile is a little lower than the original's bbox. A loose end leaves each coil's outer turn
and frays to a tip. Tile UVs wrap once around the rope and run along it in metres
over tile_m, so the rope tile's strands follow the rope.
"""
import math
import sys
from pathlib import Path

import numpy as np
from mathutils import Matrix, Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parts import X, Y, Z, Parts, args, load_goal  # noqa: E402

cfg, out = args()
goal, lo, hi = load_goal(cfg)
rng = np.random.default_rng(cfg.get("seed", 1))
ROPE = 0
P = Parts(rng, 0.1)
thick, sides = cfg.get("thick", 0.065), cfg.get("sides", 5)
r_coil, r_hole = cfg.get("coil", 0.7), cfg.get("hole", 0.22)
tile_m = cfg.get("tile_m", 0.15)
rad = thick / 2


def tube(points, part, taper_end=False):
    """A rope along a polyline: n-sided rings, UV u once around, v along the length."""
    rings = []
    along = 0.0
    for k, p in enumerate(points):
        t = (points[min(k + 1, len(points) - 1)] - points[max(k - 1, 0)]).normalized()
        side = t.cross(Z)
        if side.length < 1e-6:
            side = t.cross(X)
        side.normalize()
        up = side.cross(t)
        r = rad * (1 - 0.6 * max(0.0, (k - (len(points) - 4)) / 3)) if taper_end else rad
        if k:
            along += (p - points[k - 1]).length
        rings.append(([P.vert(p + (side * math.cos(2 * math.pi * s / sides) + up * math.sin(2 * math.pi * s / sides)) * r, part)
                       for s in range(sides)], along))
    for k in range(len(rings) - 1):
        (a, va), (b, vb) = rings[k], rings[k + 1]
        for s in range(sides):
            s2 = (s + 1) % sides
            u0, u1 = s / sides * (2 * math.pi * rad) / tile_m, (s + 1) / sides * (2 * math.pi * rad) / tile_m
            P.poly((a[s], a[s2], b[s2], b[s]), ROPE, ((u0, va / tile_m), (u1, va / tile_m), (u1, vb / tile_m), (u0, vb / tile_m)))


def spiral(r_from, r_to, z, phase):
    """Flat spiral from r_from to r_to at height z, one rope thickness per turn, in steps of about
    "segment" metres along the rope (at least 8 per turn near the centre)."""
    turns = (r_to - r_from) / thick
    end = 2 * math.pi * turns
    pts, theta = [], 0.0
    seg = cfg.get("segment", 0.18)
    while True:
        r = r_from + (r_to - r_from) * theta / end
        pts.append(Vector((r * math.cos(phase + theta), r * math.sin(phase + theta), z)))
        if theta >= end:
            return pts
        theta = min(theta + min(seg / max(r, 1e-3), 2 * math.pi / 8), end)


def coil(place, tail_angle):
    """One coil in local space (centre at the origin, base at z = 0), moved by place (a 4x4 matrix);
    its loose end leaves the bottom layer toward tail_angle (radians, local)."""
    layers = cfg.get("layers", 3)
    outer = cfg.get("outer_turns", 2)
    part = P.new_part()
    last = None
    for layer in range(layers):
        z = rad + layer * thick * 0.85
        if layer == layers - 1:
            pts = spiral(r_hole, r_coil - thick * 0.6, z, rng.uniform(0, 6.28))
        else:
            turns = outer
            phase = tail_angle - 2 * math.pi * turns if layer == 0 else rng.uniform(0, 6.28)
            pts = spiral(r_coil - outer * thick, r_coil, z, phase)
        if layer == 0:
            last = pts[-1]
        tube([place @ p for p in pts], part)
    # loose end: from the bottom layer's outer turn straight out over the ground, fraying to a tip
    length, bend = cfg.get("tail", [0.6, 0.2])
    d = Vector((math.cos(tail_angle), math.sin(tail_angle), 0))
    side = Z.cross(d)
    pts = [last + d * (length * f) + side * (bend * math.sin(f * math.pi) * 0.5) for f in np.linspace(0, 1, 8)]
    tube([place @ p for p in pts], P.new_part(), taper_end=True)


cx = (lo.x + hi.x) / 2
# The first coil lies flat at the near end; the second rests its near edge on the first coil's
# top, overlapping its rim by "overlap", and its far edge on the ground.
layers = cfg.get("layers", 3)
y_first = lo.y + r_coil + 0.02
top_first = thick + (layers - 1) * thick * 0.85
edge = y_first + r_coil - cfg.get("overlap", 0.2)
tilt = math.asin(min(top_first / (2 * r_coil), 1.0))
first = Matrix.Translation(Vector((cx, y_first, lo.z)))
second = (Matrix.Translation(Vector((cx, edge + r_coil * math.cos(tilt), lo.z + r_coil * math.sin(tilt) + rad * 0.5)))
          @ Matrix.Rotation(-tilt, 4, "X"))
coil(first, math.pi + 0.35)   # toward -X, a little toward the near end
coil(second, -0.35)           # toward +X
P.finish(goal, out, lo, hi, {"tilt_deg": round(math.degrees(tilt), 2)})
