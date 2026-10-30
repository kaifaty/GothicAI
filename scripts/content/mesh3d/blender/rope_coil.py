"""Build the harbour rope coil (NW_HARBOUR_ROPE_01) as the original lays it: one coil, two ends.

blender -b --factory-startup -P rope_coil.py -- rope-coil.json OUT_DIR   (run from the repository root)
config: {"id": "rig-rope", "target": "data/content/mesh3d-rigging.json", "seed": 2, "thick": 0.08, "sides": 6,
         "segment": 0.12, "hole": 0.1, "rise": 0.8, "wobble": 0.012, "pitch": 0.88, "tile_m": 0.15,
         "outer_tail": [-1.0, 0.25], "inner_tail": [1.0, -0.2]}
Asset harbour-rigging (intent.md): a flat coil of thick rope about 1.8 m across, the turns stepping up
toward the middle like the original's low stepped cone, 0.47 m high. The rope is one tube laid along a
spiral on that cone: from the hole (radius "hole") out to the rim, each turn "pitch" rope thicknesses wider and
a little lower ("rise" shapes the cone: 1 is straight, < 1 rounder). Neighbouring turns overlap in height,
so no gap shows under them. The outer end leaves the rim and runs over the ground to the -y end of the
bbox; the inner end comes up out of the hole, runs over the turns and down to the ground toward +y.
"outer_tail" and "inner_tail" are [y direction, sideways bend in m]. Every turn wobbles a little
("wobble", m) so the coil is laid by hand, not turned on a lathe. UVs wrap once around the rope and run
along it in metres over tile_m, so the rope tile's strands follow the rope.
"""
import math
import sys
from pathlib import Path

import numpy as np
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parts import X, Z, Parts, args, load_goal  # noqa: E402

cfg, out = args()
goal, lo, hi = load_goal(cfg)
rng = np.random.default_rng(cfg.get("seed", 2))
ROPE = 0
P = Parts(rng, 0.1)
thick, sides = cfg.get("thick", 0.08), cfg.get("sides", 6)
rad = thick / 2
tile_m = cfg.get("tile_m", 0.15)
seg = cfg.get("segment", 0.12)
centre = Vector(((lo.x + hi.x) / 2, (lo.y + hi.y) / 2, lo.z))
r_out = (hi.x - lo.x) / 2 - rad            # the rim turn touches the bbox in x
r_hole = cfg.get("hole", 0.1)
height = hi.z - lo.z
top = height - rad - thick * 0.9            # the inner end lies over the top turn and stays in the bbox
rise = cfg.get("rise", 0.8)
wobble = cfg.get("wobble", 0.012)
pitch = cfg.get("pitch", 0.88)              # turn spacing in rope thicknesses: < 1, the turns press together


def cone(r):
    """Height of a turn's centre at radius r: the rim turn on the ground, the innermost at `top`."""
    f = min(max((r - r_hole) / (r_out - r_hole), 0.0), 1.0)
    return rad + (top - rad) * (1 - f) ** rise


def tube(points, part, taper=(False, False)):
    """A rope along a polyline: n-sided rings, UV u once around, v along the length; both ends capped."""
    rings, along = [], 0.0
    n = len(points)
    for k, p in enumerate(points):
        t = (points[min(k + 1, n - 1)] - points[max(k - 1, 0)]).normalized()
        side = t.cross(Z)
        if side.length < 1e-6:
            side = t.cross(X)
        side.normalize()
        up = side.cross(t)
        r = rad
        if taper[1]:
            r *= 1 - 0.45 * max(0.0, (k - (n - 4)) / 3)
        if taper[0]:
            r *= 1 - 0.45 * max(0.0, (3 - k) / 3)
        if k:
            along += (p - points[k - 1]).length
        rings.append(([P.vert(p + (side * math.cos(2 * math.pi * s / sides) + up * math.sin(2 * math.pi * s / sides)) * r, part)
                       for s in range(sides)], along))
    for k in range(n - 1):
        (a, va), (b, vb) = rings[k], rings[k + 1]
        for s in range(sides):
            s2 = (s + 1) % sides
            u0, u1 = s / sides * (2 * math.pi * rad) / tile_m, (s + 1) / sides * (2 * math.pi * rad) / tile_m
            P.poly((a[s], a[s2], b[s2], b[s]), ROPE, ((u0, va / tile_m), (u1, va / tile_m), (u1, vb / tile_m), (u0, vb / tile_m)))
    cap = [(rad * math.cos(2 * math.pi * s / sides) / tile_m, rad * math.sin(2 * math.pi * s / sides) / tile_m) for s in range(sides)]
    P.poly(rings[0][0][::-1], ROPE, cap[::-1])
    P.poly(rings[-1][0], ROPE, cap)


def spiral(phase):
    """The coil from the hole out to the rim: one rope thickness per turn, on the cone."""
    turns = (r_out - r_hole) / (thick * pitch)
    end = 2 * math.pi * turns
    pts, theta = [], 0.0
    jitter = rng.uniform(-1, 1, 64)
    while True:
        r = r_hole + (r_out - r_hole) * theta / end
        w = wobble * math.sin(3 * theta / (2 * math.pi) + jitter[int(theta / (2 * math.pi)) % 64])
        rr = min(r + w, r_out)
        a = phase + theta
        pts.append(centre + Vector((rr * math.cos(a), rr * math.sin(a), cone(r))))
        if theta >= end:
            return pts
        theta = min(theta + min(seg / max(r, 1e-3), 2 * math.pi / 10), end)


def surface(r):
    """Height of the coil's top at radius r (the top of the turns under a rope lying over them)."""
    return cone(r) + rad


phase = rng.uniform(0, 2 * math.pi)
coil = spiral(phase)
tube(coil, P.new_part())

# Outer end: leaves the rim turn along its tangent, then bends toward -y and runs to the bbox end.
y_dir, bend = cfg.get("outer_tail", [-1.0, 0.25])
start, before = coil[-1], coil[-2]
tangent = (start - before).normalized()
tangent.z = 0
tangent.normalize()
target = Vector((centre.x + bend, lo.y + rad if y_dir < 0 else hi.y - rad, lo.z + rad))
pts = []
for f in np.linspace(0, 1, 14):
    # a quadratic curve from the rim along the tangent to the target on the ground
    c1 = start + tangent * 0.45
    p = start * (1 - f) ** 2 + c1 * 2 * f * (1 - f) + target * f * f
    p.z = lo.z + rad + (start.z - lo.z - rad) * (1 - f) ** 2
    pts.append(p)
tube(pts, P.new_part(), taper=(False, True))

# Inner end: up out of the hole, over the turns toward +y, down to the ground and out to the bbox end.
y_dir, bend = cfg.get("inner_tail", [1.0, -0.2])
first = coil[0]
pts = [first, first + Vector((0, 0, thick * 0.6))]
for f in np.linspace(0.05, 1, 12):
    r = r_hole + (r_out + thick - r_hole) * f
    pts.append(centre + Vector((bend * f, r * y_dir, surface(min(r, r_out)) + rad * 0.9)))
end_y = hi.y - rad if y_dir > 0 else lo.y + rad
for f in np.linspace(0.15, 1, 6):
    y = centre.y + y_dir * (r_out + thick) + (end_y - centre.y - y_dir * (r_out + thick)) * f
    pts.append(Vector((centre.x + bend + 0.08 * math.sin(f * math.pi), y, lo.z + rad + (rad * 1.5) * (1 - f) ** 2)))
tube(pts, P.new_part(), taper=(False, True))

P.finish(goal, out, lo, hi, {"turns": round((r_out - r_hole) / (thick * pitch), 2), "rim_radius": round(r_out, 3)})
