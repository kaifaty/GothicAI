"""Build the fishermen's basket of fish (OWN_HARBOUR_FISHBASKET_01), a new prop of the harbour's traces of life.

blender -b --factory-startup -P fish_basket.py -- fish-basket.json OUT_DIR   (run from the repository root)
Asset harbour-life (step M4 of docs/world-detail-plan.md), concept life-basket attempt 1: an oval basket of
unpeeled willow, a thick rolled rim, two rod-loop handles on the long ends, a little lopsided, heaped a little
above the rim with cod-like fish lying every way; two tails and a head over the rim, strands of kelp between.
The weave is the wicker tile on a walled shell (rods as geometry would cost ten times the budget); the rim,
the handles and one broken stake are rods. Each fish is a lofted body with a tail fin; its back and its belly
are two zones, so the bake gives the dark olive back and the dull grey belly their own colours. A mound of
the same fish skin under the top layer hides the empty floor between the fish.
config: {"goal": {...}, "seed": 3, "rim": [a, b] half axes m, "base": [a, b], "height": m, "wall": m,
         "bulge": m, "rim_r": m, "fish": [[x, y, z, yaw_deg, length, bend, roll_deg, droop], ...],
         "kelp": [[[x, y, z], ...], ...], "wicker_tile_m": m}
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
rng = np.random.default_rng(cfg.get("seed", 3))
keys = [z["key"] for z in goal["zones"]]
WICKER, BACK, BELLY, KELP = (keys.index(k) for k in ("OWN_PROP_WICKER", "OWN_PROP_FISH_BACK", "OWN_PROP_FISH_BELLY",
                                                      "OWN_PROP_KELP"))
P = Parts(rng, 0.1)
SEG = cfg.get("segments", 36)
H = cfg["height"]
rim_a, rim_b = cfg["rim"]
base_a, base_b = cfg["base"]
wall = cfg.get("wall", 0.012)
bulge = cfg.get("bulge", 0.02)
rim_r = cfg.get("rim_r", 0.022)
tile_w = cfg.get("wicker_tile_m", 0.54)


def oval(t, h):
    """A point of the outer wall at angle t, height h: a slightly squared oval, the bulge on one side."""
    s = h / H
    a = base_a + (rim_a - base_a) * s
    b = base_b + (rim_b - base_b) * s
    c, si = math.cos(t), math.sin(t)
    e = 2.4  # squared a little, as a woven oval stands
    r = 1.0 / (abs(c) ** e + abs(si) ** e) ** (1 / e)
    belly = bulge * math.sin(math.pi * s) * max(0.0, math.cos(t - 0.6)) ** 2
    return Vector((c * r * a + c * belly, si * r * b + si * belly, h))


def tube(points, rad, sides, material, tile_v=0.15, closed=False):
    """A tube along points (net_rack.py's), radius per point or one; "closed" joins the ends."""
    rings, along = [], 0.0
    part = P.new_part()
    n = len(points)
    for k, p in enumerate(points):
        if closed:
            t = (points[(k + 1) % n] - points[(k - 1) % n]).normalized()
        else:
            t = (points[min(k + 1, n - 1)] - points[max(k - 1, 0)]).normalized()
        side = t.cross(Z)
        if side.length < 1e-6:
            side = t.cross(X)
        side.normalize()
        upv = side.cross(t)
        r = rad[k] if isinstance(rad, (list, np.ndarray)) else rad
        if k:
            along += (p - points[k - 1]).length
        rings.append(([P.vert(p + (side * math.cos(2 * math.pi * s / sides) + upv * math.sin(2 * math.pi * s / sides)) * r, part)
                       for s in range(sides)], along, r))
    for k in range(len(rings) - (0 if closed else 1)):
        (a, va, ra), (b, vb, rb) = rings[k], rings[(k + 1) % len(rings)]
        for s in range(sides):
            s2 = (s + 1) % sides
            u0, u1 = s / sides * 2 * math.pi * ra / tile_v, (s + 1) / sides * 2 * math.pi * ra / tile_v
            P.poly((a[s], a[s2], b[s2], b[s]), material, ((u0, va / tile_v), (u1, va / tile_v), (u1, vb / tile_v), (u0, vb / tile_v)))
    if not closed:
        for ring, flip in ((rings[0][0], True), (rings[-1][0], False)):
            cap = [(0.02 * math.cos(2 * math.pi * s / sides), 0.02 * math.sin(2 * math.pi * s / sides)) for s in range(sides)]
            P.poly(ring[::-1] if flip else ring, material, cap[::-1] if flip else cap)


# --- the basket: outer wall, inner wall, floor, the rolled rim over the top of the walls ---
part = P.new_part()
rows = [0.0, 0.015, 0.05, 0.11, 0.17, 0.23, H - 0.035]
angles = [2 * math.pi * i / SEG + rng.uniform(-0.01, 0.01) for i in range(SEG)]
outer = []
for h in rows:
    ring = []
    for t in angles:
        p = oval(t, h)
        if h == 0.0:  # the base edge is rounded in: the bottom row stands a little inside the wall
            p = Vector((p.x * 0.96, p.y * 0.95, 0.0))
        ring.append(P.vert(p + Vector((rng.uniform(-1, 1) * 0.002, rng.uniform(-1, 1) * 0.002, 0)), part))
    outer.append(ring)
inner = []
for h in [0.03, *rows[2:]]:
    ring = []
    for t in angles:
        p = oval(t, h)
        d = Vector((p.x, p.y, 0)).normalized()
        ring.append(P.vert(Vector((p.x, p.y, h)) - d * wall, part))
    inner.append(ring)
arc = [0.0]
for i in range(1, SEG + 1):
    arc.append(arc[-1] + (oval(angles[i % SEG], H / 2) - oval(angles[i - 1], H / 2)).length)


def wall_strip(rings, heights, flip):
    for k in range(len(rings) - 1):
        for i in range(SEG):
            j = (i + 1) % SEG
            quad = (rings[k][i], rings[k][j], rings[k + 1][j], rings[k + 1][i])
            uv = ((arc[i] / tile_w, heights[k] / tile_w), (arc[i + 1] / tile_w, heights[k] / tile_w),
                  (arc[i + 1] / tile_w, heights[k + 1] / tile_w), (arc[i] / tile_w, heights[k + 1] / tile_w))
            P.poly(quad[::-1] if flip else quad, WICKER, uv[::-1] if flip else uv)


wall_strip(outer, rows, False)
wall_strip(inner, [0.03, *rows[2:]], True)
for i in range(SEG):  # the top between the walls, under the rim
    j = (i + 1) % SEG
    P.poly((outer[-1][i], outer[-1][j], inner[-1][j], inner[-1][i]), WICKER,
           ((arc[i] / tile_w, 0), (arc[i + 1] / tile_w, 0), (arc[i + 1] / tile_w, 0.02), (arc[i] / tile_w, 0.02)))
P.poly(outer[0][::-1], WICKER, [(v.co.x / tile_w, v.co.y / tile_w) for v in outer[0][::-1]])
P.poly(inner[0], WICKER, [(v.co.x / tile_w, v.co.y / tile_w) for v in inner[0]])

rim = []
for i, t in enumerate(angles):
    p = oval(t, H - 0.035)
    d = Vector((p.x, p.y, 0)).normalized()
    rim.append(Vector((p.x, p.y, H - rim_r)) - d * (wall / 2) + Vector((0, 0, rng.uniform(-0.004, 0.004))))
tube(rim, [rim_r * rng.uniform(0.85, 1.1) for _ in rim], 6, WICKER, tile_v=0.12, closed=True)
for side in (-1, 1):  # rod-loop handles on the long ends
    pts = []
    for k in range(11):
        a = math.pi * k / 10
        y = math.cos(a) * 0.075
        p = oval(math.atan2(y, side * rim_a) if side > 0 else math.pi - math.atan2(y, rim_a), H - 0.03)
        pts.append(Vector((p.x + side * 0.004, p.y, H - 0.02 + math.sin(a) * 0.065)))
    tube(pts, 0.011, 5, WICKER, tile_v=0.1)
p = oval(2.4, 0.09)  # one broken stake sticking out low on the side
d = Vector((p.x, p.y, 0)).normalized()
tube([p - d * 0.005, p + d * 0.04 + Vector((0, 0, 0.05))], 0.0045, 4, WICKER, tile_v=0.1)

# --- the fish ---


def fish(x, y, z, yaw, length, bend, roll, droop):
    """A cod-like fish: blunt head, deep body, thin tail stalk, a forked tail fin; back and belly zones."""
    yawr, rollr = math.radians(yaw), math.radians(roll)
    fwd = Vector((math.cos(yawr), math.sin(yawr), 0))
    side0 = Vector((-fwd.y, fwd.x, 0))
    n_rings, sides = 11, 8
    part = P.new_part()
    centres, frames, radii = [], [], []
    for k in range(n_rings):
        t = k / (n_rings - 1)
        # spine: bends sideways and, towards the tail, droops (a tail over the rim)
        c = Vector((x, y, z)) + fwd * ((t - 0.4) * length) + side0 * (bend * length * (t - 0.4) ** 2) \
            - Vector((0, 0, droop * max(0.0, t - 0.55) ** 2 * 4 * length))
        depth = length * 0.12 * (t ** 0.45) * (1 - t) ** 0.85 / 0.43 + 0.003  # a cod is about a quarter as deep as long
        width = depth * (0.7 - 0.3 * t)
        centres.append(c)
        radii.append((width, depth))
    rings = []
    for k, c in enumerate(centres):
        t_vec = (centres[min(k + 1, n_rings - 1)] - centres[max(k - 1, 0)]).normalized()
        s = t_vec.cross(Z)
        s = s.normalized() if s.length > 1e-6 else side0
        u = s.cross(t_vec)
        # roll the section: a fish lies on its side as often as on its belly
        s, u = s * math.cos(rollr) + u * math.sin(rollr), u * math.cos(rollr) - s * math.sin(rollr)
        frames.append((s, u))
        w, d = radii[k]
        ring = []
        for j in range(sides):
            a = 2 * math.pi * j / sides
            ring.append(P.vert(c + s * (math.cos(a) * w) + u * (math.sin(a) * d * (1.0 if math.sin(a) > 0 else 0.85)), part))
        rings.append(ring)
    along = [0.0]
    for k in range(1, n_rings):
        along.append(along[-1] + (centres[k] - centres[k - 1]).length)
    for k in range(n_rings - 1):
        for j in range(sides):
            j2 = (j + 1) % sides
            mid = 2 * math.pi * (j + 0.5) / sides
            zone = BACK if math.sin(mid) > -0.15 else BELLY
            P.poly((rings[k][j], rings[k][j2], rings[k + 1][j2], rings[k + 1][j]), zone,
                   ((j / sides * 0.12, along[k]), ((j + 1) / sides * 0.12, along[k]),
                    ((j + 1) / sides * 0.12, along[k + 1]), (j / sides * 0.12, along[k + 1])))
    head = P.vert(centres[0] - (centres[1] - centres[0]).normalized() * length * 0.035, part)
    for j in range(sides):
        j2 = (j + 1) % sides
        P.poly((rings[0][j2], rings[0][j], head), BACK if math.sin(2 * math.pi * (j + 0.5) / sides) > -0.15 else BELLY,
               ((0, 0), (0.02, 0), (0.01, 0.02)))
    tip = P.vert(centres[-1] + (centres[-1] - centres[-2]).normalized() * 0.004, part)
    for j in range(sides):
        j2 = (j + 1) % sides
        P.poly((rings[-1][j], rings[-1][j2], tip), BACK, ((0, 0), (0.02, 0), (0.01, 0.02)))
    # forked tail fin: a thin plate in the plane of the body's height
    s, u = frames[-1]
    base = centres[-1]
    t_vec = (centres[-1] - centres[-2]).normalized()
    span, reach, th = length * 0.13, length * 0.14, 0.003
    outline = [base + u * (span * 0.25), base + t_vec * reach + u * span, base + t_vec * (reach * 0.65),
               base + t_vec * reach - u * span, base - u * (span * 0.25)]
    fin = P.new_part()
    front = [P.vert(p + s * th, fin) for p in outline]
    back = [P.vert(p - s * th, fin) for p in outline]
    uv = [(0.0, 0.0), (0.06, 0.05), (0.03, 0.03), (0.06, 0.0), (0.0, 0.02)]
    for idx in ((0, 1, 2), (0, 2, 4), (4, 2, 3)):
        P.poly([front[i] for i in idx], BACK, [uv[i] for i in idx])
        P.poly([back[i] for i in idx[::-1]], BACK, [uv[i] for i in idx[::-1]])
    for i in range(5):
        j = (i + 1) % 5
        P.poly((front[i], back[i], back[j], front[j]), BACK, ((0, 0), (0.005, 0), (0.005, 0.02), (0, 0.02)))
    # a small dorsal fin
    c = centres[4]
    s, u = frames[4]
    d = radii[4][1]
    t_vec = (centres[5] - centres[3]).normalized()
    pts = [c + u * d * 0.95 - t_vec * length * 0.06, c + u * (d + length * 0.045), c + u * d * 0.95 + t_vec * length * 0.05]
    fin = P.new_part()
    fa = [P.vert(p + s * 0.002, fin) for p in pts]
    fb = [P.vert(p - s * 0.002, fin) for p in pts]
    P.poly(fa, BACK, [(0, 0), (0.02, 0.03), (0.04, 0)])
    P.poly(fb[::-1], BACK, [(0.04, 0), (0.02, 0.03), (0, 0)])
    for i in range(3):
        j = (i + 1) % 3
        P.poly((fa[i], fb[i], fb[j], fa[j]), BACK, ((0, 0), (0.004, 0), (0.004, 0.02), (0, 0.02)))


for spec in cfg["fish"]:
    fish(*spec)

# the mound of fish under the top layer: an oval dome inside the rim, fish skin, so no floor shows between them
part = P.new_part()
mound_rings = []
for k, (scale, h) in enumerate(((1.0, H - 0.07), (0.7, H - 0.025), (0.35, H - 0.005))):
    ring = []
    for t in angles[::2]:
        p = oval(t, H - 0.035)
        d = Vector((p.x, p.y, 0)).normalized()
        q = Vector((p.x, p.y, 0)) - d * (wall + 0.004)
        ring.append(P.vert(Vector((q.x * scale, q.y * scale, h)), part))
    mound_rings.append(ring)
top = P.vert(Vector((0, 0, H + 0.0)), part)
m = len(mound_rings[0])
for k in range(2):
    for i in range(m):
        j = (i + 1) % m
        P.poly((mound_rings[k][i], mound_rings[k][j], mound_rings[k + 1][j], mound_rings[k + 1][i]), BACK,
               ((i * 0.05, k * 0.1), ((i + 1) * 0.05, k * 0.1), ((i + 1) * 0.05, (k + 1) * 0.1), (i * 0.05, (k + 1) * 0.1)))
for i in range(m):
    j = (i + 1) % m
    P.poly((mound_rings[2][i], mound_rings[2][j], top), BACK, ((0, 0), (0.05, 0), (0.025, 0.05)))
P.poly(mound_rings[0][::-1], BACK, [(v.co.x, v.co.y) for v in mound_rings[0][::-1]])

# kelp: flat ribbons draped over the fish
for strand in cfg.get("kelp", []):
    pts = [Vector(p) for p in strand]
    part = P.new_part()
    width = rng.uniform(0.025, 0.04)
    rows = {lift: ([], []) for lift in (0.004, 0.0015)}  # a thin closed ribbon: upper and lower face
    along = 0.0
    for k, p in enumerate(pts):
        t = (pts[min(k + 1, len(pts) - 1)] - pts[max(k - 1, 0)]).normalized()
        s = t.cross(Z).normalized()
        w = width * (1 - 0.5 * k / len(pts))
        for lift, (left, right) in rows.items():
            left.append(P.vert(p + s * w / 2 + Vector((0, 0, lift)), part))
            right.append(P.vert(p - s * w / 2 + Vector((0, 0, lift)), part))
    (ul, ur), (dl, dr) = rows[0.004], rows[0.0015]
    for k in range(len(pts) - 1):
        along1 = along + (pts[k + 1] - pts[k]).length
        P.poly((ur[k], ur[k + 1], ul[k + 1], ul[k]), KELP, ((0, along), (0, along1), (width, along1), (width, along)))
        P.poly((dl[k], dl[k + 1], dr[k + 1], dr[k]), KELP, ((width, along), (width, along1), (0, along1), (0, along)))
        for a, b in ((ul, dl), (dr, ur)):
            P.poly((a[k], a[k + 1], b[k + 1], b[k]), KELP, ((0, along), (0, along1), (0.003, along1), (0.003, along)))
        along = along1
    for a, b, c, d in ((ul[0], ur[0], dr[0], dl[0]), (ur[-1], ul[-1], dl[-1], dr[-1])):
        P.poly((a, b, c, d), KELP, ((0, 0), (width, 0), (width, 0.003), (0, 0.003)))

P.finish(goal, out, lo, hi)
