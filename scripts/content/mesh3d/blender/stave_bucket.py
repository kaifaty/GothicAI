"""Build the old wooden stave bucket (OWN_HARBOUR_BUCKET_01), a new prop of the harbour's traces of life.

blender -b --factory-startup -P stave_bucket.py -- stave-bucket.json OUT_DIR   (run from the repository root)
Asset harbour-life (step M4 of docs/world-detail-plan.md), concept life-bucket attempt 2: eleven split oak
staves of uneven width that narrow upwards, two of them rising as ears with a hole, two bands of wound willow
withy (no iron: P4), a hemp rope handle knotted through the ears and sagging to one side, dark water half up.
Each stave is a closed curved board with its own part number, so the bake tones them one by one; the gaps
between them stay open to the dark inside.
config: {"goal": {...}, "seed": 4, "bottom_r": m, "top_r": m, "height": m, "thick": m, "staves": [width share, ...],
         "ears": [i, j], "ear_rise": m, "short": [stave, m], "bands": [[z, height], ...], "water": share of height,
         "sag": m the handle hangs below the holes, wound round the outside half of the bucket}
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
rng = np.random.default_rng(cfg.get("seed", 4))
keys = [z["key"] for z in goal["zones"]]
STAVE, WITHY, ROPE, WATER = (keys.index(k) for k in ("OWN_PROP_WOOD_STAVE", "OWN_PROP_WITHY", "OWN_PROP_ROPE",
                                                      "OWN_PROP_WATER"))
P = Parts(rng, 0.095)
PLANK = 0.38 / 4  # one plank of the stave tile (0.38 m, barrel.py) per stave
r0, r1, H = cfg["bottom_r"], cfg["top_r"], cfg["height"]
th = cfg.get("thick", 0.015)
shares = np.array(cfg["staves"], dtype=float)
shares /= shares.sum()
gap = cfg.get("gap", 0.012)  # radians between staves


def radius(z):
    return r0 + (r1 - r0) * z / H


def tube(points, rad, sides, material, tile_v=0.15, closed=False):
    """A tube along points (net_rack.py's); "closed" joins the ends."""
    rings, along = [], 0.0
    part = P.new_part()
    n = len(points)
    for k, p in enumerate(points):
        t = ((points[(k + 1) % n] - points[(k - 1) % n]) if closed else
             (points[min(k + 1, n - 1)] - points[max(k - 1, 0)])).normalized()
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
            cap = [(0.01 * math.cos(2 * math.pi * s / sides), 0.01 * math.sin(2 * math.pi * s / sides)) for s in range(sides)]
            P.poly(ring[::-1] if flip else ring, material, cap[::-1] if flip else cap)


# --- staves: curved boards round a cone, the outer face bowed a little, ears rising above the rim ---
ears = set(cfg.get("ears", []))
short_i, short_m = cfg.get("short", [-1, 0.0])
start = rng.uniform(0, 2 * math.pi)
a = start
plank = 0
ear_angles = []
for i, share in enumerate(shares):
    span = 2 * math.pi * share - gap
    a0, a1 = a, a + span
    a += 2 * math.pi * share
    if i in ears:
        ear_angles.append((a0 + a1) / 2)
    top = H + (cfg.get("ear_rise", 0.09) if i in ears else 0.0) - (short_m if i == short_i else 0.0) \
        + rng.uniform(-0.004, 0.004)
    part = P.new_part()
    cols = 3
    rows = [0.0, 0.33 * H, 0.66 * H, H] + ([top] if top > H else [])
    if top < H:
        rows[-1] = top
    lean = rng.uniform(-0.004, 0.004)
    shell = {}
    for side, off in (("out", 0.0), ("in", -th)):
        for ri, z in enumerate(rows):
            zr = min(z, H)
            for ci in range(cols):
                t = a0 + span * ci / (cols - 1)
                bow = 0.003 * math.sin(math.pi * ci / (cols - 1)) if side == "out" else 0.0
                r = radius(zr) + off + bow + lean * z / H
                shell[side, ri, ci] = P.vert(Vector((math.cos(t) * r, math.sin(t) * r, z)), part)
    u = lambda ci: (plank % 4) * PLANK + PLANK * ci / (cols - 1)  # noqa: E731
    plank += 1
    for ri in range(len(rows) - 1):
        for ci in range(cols - 1):
            o = [shell["out", ri, ci], shell["out", ri, ci + 1], shell["out", ri + 1, ci + 1], shell["out", ri + 1, ci]]
            uv = [(u(ci), rows[ri]), (u(ci + 1), rows[ri]), (u(ci + 1), rows[ri + 1]), (u(ci), rows[ri + 1])]
            P.poly(o, STAVE, uv)
            n = [shell["in", ri, ci + 1], shell["in", ri, ci], shell["in", ri + 1, ci], shell["in", ri + 1, ci + 1]]
            P.poly(n, STAVE, [uv[1], uv[0], uv[3], uv[2]])
    last = len(rows) - 1
    for ci in range(cols - 1):  # top end grain and bottom
        P.poly((shell["out", last, ci], shell["out", last, ci + 1], shell["in", last, ci + 1], shell["in", last, ci]), STAVE,
               ((0, 0), (0.03, 0), (0.03, th), (0, th)))
        P.poly((shell["in", 0, ci], shell["in", 0, ci + 1], shell["out", 0, ci + 1], shell["out", 0, ci]), STAVE,
               ((0, 0), (0.03, 0), (0.03, th), (0, th)))
    for ci, flip in ((0, True), (cols - 1, False)):  # the two edges facing the neighbours
        for ri in range(last):
            q = [shell["out", ri, ci], shell["in", ri, ci], shell["in", ri + 1, ci], shell["out", ri + 1, ci]]
            P.poly(q[::-1] if flip else q, STAVE, ((0, rows[ri]), (th, rows[ri]), (th, rows[ri + 1]), (0, rows[ri + 1])))

# the bottom board, sunk 2 cm, and the dark water surface
part = P.new_part()
ring_b, ring_t = [], []
for k in range(16):
    t = 2 * math.pi * k / 16
    r = radius(0.02) - th * 0.6
    ring_b.append(P.vert(Vector((math.cos(t) * r, math.sin(t) * r, 0.005)), part))
    ring_t.append(P.vert(Vector((math.cos(t) * r, math.sin(t) * r, 0.025)), part))
P.poly(ring_t, STAVE, [(v.co.x, v.co.y) for v in ring_t])
P.poly(ring_b[::-1], STAVE, [(v.co.x, v.co.y) for v in ring_b[::-1]])
for k in range(16):
    j = (k + 1) % 16
    P.poly((ring_b[k], ring_b[j], ring_t[j], ring_t[k]), STAVE, ((0, 0), (0.02, 0), (0.02, 0.02), (0, 0.02)))
part = P.new_part()
zw = H * cfg.get("water", 0.5)
water = []
for k in range(20):
    t = 2 * math.pi * k / 20
    r = radius(zw) - th * 0.8
    water.append(P.vert(Vector((math.cos(t) * r, math.sin(t) * r, zw)), part))
P.poly(water, WATER, [(v.co.x, v.co.y) for v in water])
under = [P.vert(Vector((v.co.x * 0.98, v.co.y * 0.98, zw - 0.01)), part) for v in water]
P.poly(under[::-1], WATER, [(v.co.x, v.co.y) for v in under[::-1]])
for k in range(20):
    j = (k + 1) % 20
    P.poly((under[k], under[j], water[j], water[k]), WATER, ((0, 0), (0.02, 0), (0.02, 0.01), (0, 0.01)))

# --- withy bands: two or three twisted rods wound round, a little uneven ---
for z, height in cfg["bands"]:
    for strand in range(3):
        pts = []
        for k in range(20):
            t = 2 * math.pi * k / 20
            zz = z + (strand - 1) * height / 3 + math.sin(t * 3 + strand * 2.1) * 0.002
            r = radius(zz) + 0.006 + 0.002 * math.sin(t * 5 + strand)
            pts.append(Vector((math.cos(t) * r, math.sin(t) * r, zz)))
        tube(pts, 0.0055, 4, WITHY, tile_v=0.1, closed=True)

# --- the rope handle: through the ear holes, a knot at each ear, sagging to one side ---
ta, tb = ear_angles
if (tb - ta) % (2 * math.pi) > math.pi:  # go round the shorter way, on one side of the bucket
    ta, tb = tb, ta
sweep = (tb - ta) % (2 * math.pi)
zh = H + cfg.get("ear_rise", 0.09) * 0.55
handle = []
for k in range(17):
    s_ = k / 16
    z = zh - cfg.get("sag", 0.16) * math.sin(math.pi * s_) ** 0.8
    r = radius(min(z, H)) + 0.012 + 0.012 * math.sin(math.pi * s_)
    t = ta + sweep * s_
    handle.append(Vector((math.cos(t) * r, math.sin(t) * r, z)))
tube(handle, 0.009, 6, ROPE, tile_v=0.12)
for end in (handle[0], handle[-1]):  # knots: a few turns round the rope by the ear
    pts = [end + Vector((math.cos(t) * 0.012, math.sin(t) * 0.012, 0.004 * k / 12 - 0.002))
           for k, t in enumerate(np.linspace(0, 4 * math.pi, 13))]
    tube(pts, 0.008, 5, ROPE, tile_v=0.12)

P.finish(goal, out, lo, hi)
