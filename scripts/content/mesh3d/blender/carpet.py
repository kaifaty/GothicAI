"""Build the merchant's carpet (NW_CITY_CARPET_03) from our own image_gen concept and a numeric target.

blender -b --factory-startup -P carpet.py -- carpet.json OUT_DIR   (run from the repository root)
config: {"id": "carpet", "target": "data/content/mesh3d-market.json", "seed": 1, "grid": [14, 10],
         "thickness": 0.012, "bumps": 0.003, "fringe": [0.08, 0.014, 26], "curl": [0.45, 0.035],
         "fringe_uv": [0.477, 0.5]}
A thin slab lying on the ground with slight bumps and one corner turned up, as on
the concept's top view; that top view itself, cut to the carpet's rectangle, is
the panel texture own_prop_carpet (market_tiles.py), so the top face spans UV 0..1.
Fringes are thin flat tassels along both short ends, mapped onto a cream spot of
the design. The bottom lies on the ground and is not built.
"""
import math
import sys
from pathlib import Path

import numpy as np
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parts import X, Y, Z, Parts, args, load_goal  # noqa: E402

cfg, out = args()
goal, lo, hi = load_goal(cfg)
rng = np.random.default_rng(cfg.get("seed", 1))
CARPET = 0
P = Parts(rng, 0.1)
part = P.new_part(wound=True)   # an open shell (no bottom): the top and edges below are wound outward

nx, ny = cfg.get("grid", [14, 10])
thick, bumps = cfg.get("thickness", 0.012), cfg.get("bumps", 0.003)
f_len, f_w, f_count = cfg.get("fringe", [0.08, 0.014, 26])
curl_r, curl_h = cfg.get("curl", [0.45, 0.035])
x0, x1 = lo.x + f_len, hi.x - f_len           # the woven body; fringes reach the bbox ends
y0, y1 = lo.y, hi.y
W, H = x1 - x0, y1 - y0
corner = Vector((x1, y0, 0))                   # the turned-up corner: right end, near edge (concept's lower right)
top_max = hi.z - lo.z - thick


def height(x, y):
    """Top surface above the ground: thickness, small bumps, and the curl near the corner."""
    d = math.hypot(x - corner.x, y - corner.y)
    lift = curl_h * max(0.0, 1 - d / curl_r) ** 2
    bump = bumps * math.sin(7.1 * x + 1.3) * math.sin(5.3 * y + 0.7)
    return min(thick + max(bump, -thick * 0.4) + lift, thick + top_max)


xs = np.linspace(x0, x1, nx + 1)
ys = np.linspace(y0, y1, ny + 1)
top = [[P.vert(Vector((x, y, lo.z + height(x, y))), part) for x in xs] for y in ys]
for j in range(ny):
    for i in range(nx):
        quad = (top[j][i], top[j][i + 1], top[j + 1][i + 1], top[j + 1][i])
        P.poly(quad, CARPET, [((xs[a] - x0) / W, (ys[b] - y0) / H) for a, b in ((i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1))])

# Edges down to the ground, mapped onto the outer border band of the design.
ring = [(i, 0) for i in range(nx)] + [(nx, j) for j in range(ny)] + [(i, ny) for i in range(nx, 0, -1)] + [(0, j) for j in range(ny, 0, -1)]
base = {}
for i, j in ring:
    v = top[j][i].co
    base[i, j] = P.vert(Vector((v.x, v.y, lo.z)), part)
for k, (i, j) in enumerate(ring):
    i2, j2 = ring[(k + 1) % len(ring)]
    u0, u1 = (xs[i] - x0) / W, (xs[i2] - x0) / W
    v0, v1 = (ys[j] - y0) / H, (ys[j2] - y0) / H
    # outward winding: the ring runs counter-clockwise seen from above
    P.poly((base[i, j], base[i2, j2], top[j2][i2], top[j][i]), CARPET,
           ((u0, v0), (u1, v1), (u1, v1), (u0, v0)))

# Fringes: flat tassels on the ground along both short ends.
fu, fv = cfg.get("fringe_uv", [0.477, 0.5])
spot = [(fu - 0.003, fv - 0.003), (fu + 0.003, fv - 0.003), (fu + 0.003, fv + 0.003), (fu - 0.003, fv + 0.003)]
for sx, xe in ((-1, x0), (1, x1)):
    for y in np.linspace(y0 + 0.06, y1 - 0.06, f_count):
        p = P.new_part(wound=True)   # a lone quad: the normal recalculation turned every tassel face down
        angle = rng.uniform(-0.18, 0.18)
        length = f_len * rng.uniform(0.75, 1.0)
        d = Vector((sx * math.cos(angle), math.sin(angle), 0))
        s = Vector((-d.y, d.x, 0)) * (f_w / 2)
        start = Vector((xe - sx * 0.005, y, lo.z + 0.003))
        end = start + d * length
        # counter-clockwise seen from above at both ends, so the tassel faces up
        quad = [P.vert(end - s, p), P.vert(end + s, p), P.vert(start + s, p), P.vert(start - s, p)]
        P.poly(quad, CARPET, spot)

P.finish(goal, out, lo, hi, {"grid": [nx, ny]})
