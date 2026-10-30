"""Build the laundry line of a poor harbour household (OWN_HARBOUR_LAUNDRY_01), a new prop of the harbour's traces of life.

blender -b --factory-startup -P laundry_line.py -- laundry-line.json OUT_DIR   (run from the repository root)
Asset harbour-life (step M4 of docs/world-detail-plan.md), concept life-laundry attempt 2: two crooked
unbarked poles with a natural fork at the top, each leaning its own way; a hemp rope tied in the forks,
sagging in the middle; washing folded over it and hanging straight down, heavy and damp: a long linen
shirt with its sleeves, a pair of woollen hose, a small faded red cloth, a patched sackcloth apron and two
grey rags, unevenly spaced, two of them overlapping. Every cloth is a thin closed slab with vertical folds
that deepen towards the hem and an uneven hem, so it reads from both sides and casts a shadow.
The origin is on the ground between the poles; the rope runs along X.
config: {"goal": {...}, "seed": 5, "span": m, "height": m, "sag": m, "pole_r": [bottom, top], "lean_deg": [a, b],
         "cloths": [{"kind": "shirt|hose|sheet", "zone": KEY, "at": share of the span, "width": m, "drop": m,
                     "rag": hem unevenness m, "folds": n, "depth": fold depth m, "off": [dx, dy] m}, ...]}
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
rng = np.random.default_rng(cfg.get("seed", 5))
keys = [z["key"] for z in goal["zones"]]
BARK, ROPE = keys.index("OWN_PROP_BARK"), keys.index("OWN_PROP_ROPE")
P = Parts(rng, 0.1)
span, height, sag = cfg["span"], cfg["height"], cfg["sag"]
r_bottom, r_top = cfg.get("pole_r", [0.05, 0.04])


def tube(points, rad, sides, material, tile_v=0.3, closed=False):
    """A tube along points (net_rack.py's); radius per point or one."""
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
            cap = [(0.02 * math.cos(2 * math.pi * s / sides), 0.02 * math.sin(2 * math.pi * s / sides)) for s in range(sides)]
            P.poly(ring[::-1] if flip else ring, material, cap[::-1] if flip else cap)


# --- poles: crooked, leaning, a fork at the top that holds the rope ---
forks = []
for side, lean in zip((-1, 1), cfg.get("lean_deg", [5, -4])):
    base = Vector((side * span / 2, rng.uniform(-0.03, 0.03), 0.0))
    tilt = Matrix.Rotation(math.radians(lean), 3, Y) @ Matrix.Rotation(math.radians(rng.uniform(-3, 3)), 3, X)
    up = tilt @ Z
    pts, rad = [], []
    for k in range(8):
        s = k / 7
        wobble = Vector((math.sin(s * 5.0 + side) * 0.02, math.cos(s * 4.0 + side) * 0.015, 0))
        pts.append(base + up * (height * 0.94 * s) + wobble * math.sin(math.pi * s))
        rad.append(r_bottom + (r_top - r_bottom) * s)
    tube(pts, rad, 7, BARK, tile_v=0.4)
    top = pts[-1]
    for branch in (-1, 1):  # the fork: two short branches spread across the rope's line
        d = (up + Y * (branch * 0.55) + X * (-side * 0.08)).normalized()
        tube([top - up * 0.06, top + d * 0.12, top + d * 0.24], [r_top * 0.8, r_top * 0.6, r_top * 0.45], 5, BARK, tile_v=0.4)
    forks.append(top + up * 0.03)

# --- the rope: tied in both forks, a catenary sag ---
a, b = forks
rope = []
for k in range(25):
    s = k / 24
    p = a.lerp(b, s) - Z * (sag * 4 * s * (1 - s))
    rope.append(p)
tube(rope, 0.008, 6, ROPE, tile_v=0.12)
for end in (a, b):  # turns of rope round each fork
    pts = [end + Vector((0.01 * math.sin(t * 0.5), math.cos(t) * 0.05, math.sin(t) * 0.05 - 0.01 + 0.002 * t))
           for t in np.linspace(0, 6 * math.pi, 25)]
    tube(pts, 0.008, 5, ROPE, tile_v=0.12)


def rope_at(x):
    s = (x - a.x) / (b.x - a.x)
    return a.lerp(b, s) - Z * (sag * 4 * s * (1 - s))


def cloth(x0, width, drop, zone, rag, folds, depth, dy=0.0, splay=0.0, thick=0.006):
    """A cloth hanging from the rope between x0 and x0 + width: a thin closed slab with folds."""
    part = P.new_part()
    cols = max(4, int(width / 0.04))
    rows = 6
    phase = rng.uniform(0, 2 * math.pi)
    hem = [drop * (1 + rng.uniform(-1, 1) * rag / drop) for _ in range(cols + 1)]
    grid = {}
    for side in (1, -1):
        for i in range(cols + 1):
            u = i / cols
            x = x0 + width * u
            top = rope_at(x) + Z * 0.008
            for j in range(rows + 1):
                v = j / rows
                down = hem[i] * v
                fold = depth * (0.35 + 0.65 * v) * math.sin(2 * math.pi * folds * u + phase)
                belly = 0.015 * math.sin(math.pi * u) * v
                pos = Vector((x + splay * v * (u - 0.5) * 0.3, top.y + dy + fold + belly + side * thick / 2, top.z - down))
                grid[side, i, j] = P.vert(pos, part)
    for i in range(cols):
        for j in range(rows):
            uv = [(width * i / cols, hem[i] * j / rows), (width * (i + 1) / cols, hem[i + 1] * j / rows),
                  (width * (i + 1) / cols, hem[i + 1] * (j + 1) / rows), (width * i / cols, hem[i] * (j + 1) / rows)]
            front = [grid[-1, i, j], grid[-1, i + 1, j], grid[-1, i + 1, j + 1], grid[-1, i, j + 1]]
            P.poly(front[::-1], zone, uv[::-1])
            back = [grid[1, i, j], grid[1, i + 1, j], grid[1, i + 1, j + 1], grid[1, i, j + 1]]
            P.poly(back, zone, uv)
    for i in range(cols):  # top fold over the rope and the hem
        for j in (0, rows):
            q = [grid[-1, i, j], grid[-1, i + 1, j], grid[1, i + 1, j], grid[1, i, j]]
            P.poly(q if j == 0 else q[::-1], zone, ((0, 0), (0.04, 0), (0.04, thick), (0, thick)))
    for i, flip in ((0, False), (cols, True)):  # side edges
        for j in range(rows):
            q = [grid[-1, i, j], grid[1, i, j], grid[1, i, j + 1], grid[-1, i, j + 1]]
            P.poly(q[::-1] if flip else q, zone, ((0, 0), (thick, 0), (thick, 0.1), (0, 0.1)))


for c in cfg["cloths"]:
    zone = keys.index(c["zone"])
    x = a.x + (b.x - a.x) * c["at"]
    w, d = c["width"], c["drop"]
    rag, folds, depth = c.get("rag", 0.02), c.get("folds", 2), c.get("depth", 0.02)
    dy = c.get("off", 0.0)
    if c["kind"] == "shirt":  # body and two sleeves hanging at its sides, a little splayed
        cloth(x - w / 2, w, d, zone, rag, folds, depth, dy)
        for side in (-1, 1):
            cloth(x + side * (w / 2 + 0.07) - 0.065, 0.13, d * 0.6, zone, rag, 1, 0.012, dy + 0.012, splay=side * 0.4)
    elif c["kind"] == "hose":  # two legs and the waistband over the rope
        cloth(x - w / 2, w, 0.12, zone, 0.005, 2, 0.01, dy)
        for side in (-1, 1):
            cloth(x + side * w / 4 - 0.075, 0.15, d, zone, rag, 1, 0.015, dy + 0.01 * side)
    else:
        cloth(x - w / 2, w, d, zone, rag, folds, depth, dy)

P.finish(goal, out, lo, hi)
