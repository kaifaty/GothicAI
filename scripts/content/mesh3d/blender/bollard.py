"""Build the harbour mooring bollard (NW_HARBOUR_IRONROPEHOLDER_WROPE_01, and _01 without the rope).

blender -b --factory-startup -P bollard.py -- bollard-rope.json OUT_DIR   (run from the repository root)
config: {"id": "rig-bollard-rope", "target": "data/content/mesh3d-rigging.json", "seed": 5, "sides": 16,
         "plate": [0.12, 0.05, 0.16], "post": [[z, r], ...] from the plate up to the cap's top,
         "bolts": [0.03, 0.025], "rope": [0.05, 5, 0.38, 0.72, 7], "rope_tail": [-1, 0.2], "tile_m": 0.15}
Asset harbour-rigging (intent.md, concept rig-bollard-rope attempt 2). A flat cast plate shaped as a
stretched octagon filling the bbox in plan, "plate" [thickness, bevel of its upper edge, corner cut as a
share of the shorter side], four bolt heads [radius, height]; on it one round post turned from the
"post" profile (flared foot, waist, mushroom cap). With "rope" [thickness, turns, z from, z to, sides]
the rope is wound in a helix around the post's waist, each turn pressed on the one below, and its end
comes out of the lowest turn and runs down to the plate and over its edge toward rope_tail [y side,
sideways bend], ending at the plate's edge. Without "rope" the bollard is bare (NW_HARBOUR_IRONROPEHOLDER_01).
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
rng = np.random.default_rng(cfg.get("seed", 5))
keys = [z["key"] for z in goal["zones"]]
CAST = keys.index("OWN_PROP_IRON_CAST")
ROPE = keys.index("OWN_PROP_ROPE") if "OWN_PROP_ROPE" in keys else None
P = Parts(rng, 0.1)
sides = cfg.get("sides", 16)
c = Vector(((lo.x + hi.x) / 2, (lo.y + hi.y) / 2, lo.z))
thick, bevel, cut = cfg.get("plate", [0.12, 0.05, 0.16])
hx, hy = (hi.x - lo.x) / 2, (hi.y - lo.y) / 2
tile_m = cfg.get("tile_m", 0.15)

# --- the plate: a stretched octagon, its upper edge bevelled ---
k = cut * min(hx, hy) * 2


def octagon(ax, ay, kk):
    return [(ax - kk, -ay), (ax, -ay + kk), (ax, ay - kk), (ax - kk, ay), (-ax + kk, ay), (-ax, ay - kk), (-ax, -ay + kk), (-ax + kk, -ay)]


part = P.new_part()
rings = [[P.vert(c + Vector((x, y, z)), part) for x, y in octagon(ax, ay, kk)]
         for ax, ay, kk, z in ((hx, hy, k, 0.0), (hx, hy, k, thick - bevel), (hx - bevel, hy - bevel, k * 0.8, thick))]
for a, b in zip(rings, rings[1:]):
    for i in range(8):
        j = (i + 1) % 8
        seg = (a[j].co - a[i].co).length
        P.poly((a[i], a[j], b[j], b[i]), CAST, ((0, 0), (seg, 0), (seg, 0.08), (0, 0.08)))
P.poly(rings[-1], CAST, [(v.co.x - c.x, v.co.y - c.y) for v in rings[-1]])
P.poly(rings[0][::-1], CAST, [(v.co.x - c.x, v.co.y - c.y) for v in rings[0][::-1]])

# bolt heads near the four long-side corners
b_r, b_h = cfg.get("bolts", [0.03, 0.025])
for sx in (-1, 1):
    for sy in (-1, 1):
        part = P.new_part()
        centre = c + Vector((sx * (hx - bevel - 0.09), sy * (hy - bevel - 0.08), thick - 0.003))
        base = [P.vert(centre + Vector((b_r * math.cos(t), b_r * math.sin(t), 0)), part) for t in np.linspace(0, 2 * math.pi, 7)[:-1]]
        top = [P.vert(centre + Vector((0.75 * b_r * math.cos(t), 0.75 * b_r * math.sin(t), b_h)), part) for t in np.linspace(0, 2 * math.pi, 7)[:-1]]
        for i in range(6):
            j = (i + 1) % 6
            P.poly((base[i], base[j], top[j], top[i]), CAST, ((0, 0), (0.03, 0), (0.03, 0.03), (0, 0.03)))
        P.poly(top, CAST, [(0.02 * math.cos(t), 0.02 * math.sin(t)) for t in np.linspace(0, 2 * math.pi, 7)[:-1]])

# --- the post, turned: foot, waist, mushroom cap; the profile's z is above the plate's top ---
profile = cfg["post"]
scale_top = (hi.z - lo.z - thick) / profile[-1][0]       # the cap's top touches the bbox
part = P.new_part()
rings = []
for z, r in profile:
    rings.append([P.vert(c + Vector((r * math.cos(2 * math.pi * j / sides), r * math.sin(2 * math.pi * j / sides),
                                     thick - 0.01 + z * scale_top)), part) for j in range(sides)])
along = 0.0
for i in range(len(rings) - 1):
    a, b = rings[i], rings[i + 1]
    step = (b[0].co - a[0].co).length
    for j in range(sides):
        j2 = (j + 1) % sides
        u0, u1 = j / sides * 0.9, (j + 1) / sides * 0.9
        P.poly((a[j], a[j2], b[j2], b[j]), CAST, ((u0, along), (u1, along), (u1, along + step), (u0, along + step)))
    along += step
P.poly(rings[-1], CAST, [(0.1 * math.cos(2 * math.pi * j / sides), 0.1 * math.sin(2 * math.pi * j / sides)) for j in range(sides)])
P.poly(rings[0][::-1], CAST, [(0.1 * math.cos(2 * math.pi * j / sides), 0.1 * math.sin(2 * math.pi * j / sides)) for j in range(sides)][::-1])


def post_radius(z_world):
    z = (z_world - c.z - thick + 0.01) / scale_top
    return float(np.interp(z, [p[0] for p in profile], [p[1] for p in profile]))


def tube(points, part, rad, n):
    rings, along = [], 0.0
    for kk, p in enumerate(points):
        t = (points[min(kk + 1, len(points) - 1)] - points[max(kk - 1, 0)]).normalized()
        side = t.cross(Z)
        if side.length < 1e-6:
            side = t.cross(X)
        side.normalize()
        up = side.cross(t)
        if kk:
            along += (p - points[kk - 1]).length
        rings.append(([P.vert(p + (side * math.cos(2 * math.pi * s / n) + up * math.sin(2 * math.pi * s / n)) * rad, part)
                       for s in range(n)], along))
    for kk in range(len(rings) - 1):
        (a, va), (b, vb) = rings[kk], rings[kk + 1]
        for s in range(n):
            s2 = (s + 1) % n
            u0, u1 = s / n * 2 * math.pi * rad / tile_m, (s + 1) / n * 2 * math.pi * rad / tile_m
            P.poly((a[s], a[s2], b[s2], b[s]), ROPE, ((u0, va / tile_m), (u1, va / tile_m), (u1, vb / tile_m), (u0, vb / tile_m)))
    cap = [(rad * math.cos(2 * math.pi * s / n) / tile_m, rad * math.sin(2 * math.pi * s / n) / tile_m) for s in range(n)]
    P.poly(rings[0][0][::-1], ROPE, cap[::-1])
    P.poly(rings[-1][0], ROPE, cap)


extra = {}
if cfg.get("rope") and ROPE is not None:
    r_t, turns, z0, z1, n = cfg["rope"]
    rad = r_t / 2
    z0, z1 = c.z + z0, c.z + z1
    pts = []
    phase = rng.uniform(0, 2 * math.pi)
    steps = int(turns * 20)
    for i in range(steps + 1):
        f = i / steps
        a = phase + 2 * math.pi * turns * f
        z = z1 - (z1 - z0) * f
        r = post_radius(z) + rad * 0.92
        pts.append(Vector((c.x + r * math.cos(a), c.y + r * math.sin(a), z)))
    # the end leaves the lowest turn along its tangent, drops to the plate and runs over its edge
    y_side, bend = cfg.get("rope_tail", [-1, 0.2])
    last, prev = pts[-1], pts[-2]
    t = (last - prev)
    t.z = 0
    t.normalize()
    edge_y = c.y + y_side * (hy - rad - 0.01)
    drop = [last + t * 0.08 + Vector((0, 0, -0.05))]
    for f in np.linspace(0.15, 1, 8):
        y = last.y + (edge_y - last.y) * f
        x = last.x + t.x * 0.08 + bend * math.sin(f * math.pi / 2)
        over = max(0.0, abs(y - c.y) - (hy - bevel))          # over the bevel the plate's top falls away
        drop.append(Vector((x, y, c.z + thick - over + rad)))
    tube(pts + drop, P.new_part(), rad, int(n))
    extra = {"rope_turns": turns}

P.finish(goal, out, lo, hi, extra)
