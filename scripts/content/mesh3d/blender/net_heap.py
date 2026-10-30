"""Build a loose heap of fishing net (NW_HARBOUR_FISHINGNETBIG, _SMALL) from layered sheets.

blender -b --factory-startup -P net_heap.py -- net-heap.json OUT_DIR   (run from the repository root)
config: {"id": "rig-net-big", "target": "data/content/mesh3d-rigging.json", "seed": 6, "grid": [18, 24],
         "layers": [[1.0, 0, 0], [0.72, 0.12, -0.18]], "folds": [3, [0.05, 0.1], [0.5, 0.9]],
         "dome": 0.7, "edge": 0.08, "lift": 0.025, "rope": [0.02, 6, 2], "tile_m": 0.42}
Asset harbour-rigging (intent.md, concept rig-net-heap attempt 1): an oval flat heap of coarse net, highest
in the middle, the ground showing through the meshes. Each layer is a sheet over an oval of the bbox
("layers": [scale, x shift, y shift] as shares of the bbox half sizes), its outline wobbling by "edge";
its height is a dome ("dome" shapes it) plus a few soft folds ("folds": count, amplitude range m,
wavelength range m) that fade toward the edge, so the edge lies on the ground. An upper layer lies "lift"
over the one under it. The net is alpha-tested: every face is emitted twice, back to back, so it shows
from both sides. A thicker border rope ("rope": [thickness, sides, arcs]) may run in loose arcs over the heap (0 arcs in the
harbour set: an addition the review asked to drop). With "core" (a share of the radius) an opaque core of
packed net lies under the layers, so the middle reads dense. Net UVs are planar in metres over tile_m (one net tile, net_tiles.py); the rope's wrap once around
and run along it. The top of the heap touches the bbox top.
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
rng = np.random.default_rng(cfg.get("seed", 6))
keys = [z["key"] for z in goal["zones"]]
NET, ROPE = keys.index("OWN_PROP_NET"), keys.index("OWN_PROP_ROPE")
CORE = keys.index("OWN_PROP_NET_ROLL") if "OWN_PROP_NET_ROLL" in keys else None
P = Parts(rng, 0.1)
tile_m = cfg.get("tile_m", 0.42)
nx, ny = cfg.get("grid", [18, 24])
c = Vector(((lo.x + hi.x) / 2, (lo.y + hi.y) / 2, lo.z))
ax, ay = (hi.x - lo.x) / 2 - 0.01, (hi.y - lo.y) / 2 - 0.01
H = hi.z - lo.z
dome = cfg.get("dome", 0.7)
edge = cfg.get("edge", 0.08)
lift = cfg.get("lift", 0.025)
n_folds, amp, wave = cfg.get("folds", [3, [0.05, 0.1], [0.5, 0.9]])


def make_layer(scale, sx, sy):
    """Height function of one layer: oval mask with a wobbling outline, dome and folds."""
    harm = rng.uniform(-1, 1, (4, 2))
    folds = [(rng.uniform(*amp), rng.uniform(*wave), rng.uniform(0, math.pi), rng.uniform(0, 2 * math.pi)) for _ in range(n_folds)]
    cx, cy = c.x + sx * ax, c.y + sy * ay
    rx, ry = ax * scale, ay * scale

    def rho(x, y):
        th = math.atan2((y - cy) / ry, (x - cx) / rx)
        wob = 1 + edge * sum(h[0] * math.cos((k + 2) * th) + h[1] * math.sin((k + 2) * th) for k, h in enumerate(harm)) / 2
        return math.hypot((x - cx) / rx, (y - cy) / ry) / wob

    def height(x, y):
        r = rho(x, y)
        if r >= 1:
            return None
        base = (1 - r * r) ** dome
        f = sum(a * math.sin(2 * math.pi * ((x - cx) * math.cos(d) + (y - cy) * math.sin(d)) / w + ph)
                for a, w, d, ph in folds)
        return base, f * (1 - r) ** 0.6
    return height, scale


layers = [make_layer(*l) for l in cfg.get("layers", [[1.0, 0, 0], [0.72, 0.12, -0.18]])]
# heights: the first layer's dome reaches h0, every layer above adds its lift; the top touches the bbox
h0 = H - 0.01 - lift * (len(layers) - 1) - amp[1] * 0.5


def layer_z(i, x, y):
    """Height of layer i at (x, y): on the layers below it or on the ground."""
    z = lo.z
    for k in range(i + 1):
        hv = layers[k][0](x, y)
        if hv is None:
            continue
        base, f = hv
        z = max(z, lo.z + 0.006 + base * h0 * layers[k][1] + max(f, -base * h0 * 0.6) + (lift * k if k else 0))
    return z


tops = []
for i, (height, scale) in enumerate(layers):
    part = P.new_part(wound=True)
    xs = np.linspace(lo.x, hi.x, nx + 1)
    ys = np.linspace(lo.y, hi.y, ny + 1)
    verts = {}
    for a, x in enumerate(xs):
        for b, y in enumerate(ys):
            if height(x, y) is not None:
                verts[a, b] = Vector((x, y, layer_z(i, x, y)))
    up, down = {}, {}
    for a in range(nx):
        for b in range(ny):
            quad = [(a, b), (a + 1, b), (a + 1, b + 1), (a, b + 1)]
            if not all(q in verts for q in quad):
                continue
            for q in quad:
                if q not in up:
                    up[q], down[q] = P.vert(verts[q], part), P.vert(verts[q], part)
            uv = [((verts[q].x - lo.x) / tile_m, (verts[q].y - lo.y) / tile_m) for q in quad]
            P.poly([up[q] for q in quad], NET, uv)
            P.poly([down[q] for q in quad[::-1]], NET, uv[::-1])
    tops.append(verts)

# the dense middle: an opaque core of packed net under the layers, out to "core" of the radius, so the
# ground shows through only toward the edges (fourth review, 2026-10-30)
if cfg.get("core") and CORE is not None:
    part = P.new_part(wound=True)
    rings = []
    n_r, n_a = 6, 24
    for i in range(n_r + 1):
        f = cfg["core"] * i / n_r
        ring = []
        for a in np.linspace(0, 2 * math.pi, n_a + 1)[:-1]:
            x, y = c.x + f * ax * math.cos(a), c.y + f * ay * math.sin(a)
            ring.append(P.vert(Vector((x, y, max(layer_z(0, x, y) - 0.03, lo.z + 0.004))), part))
        rings.append(ring)
    centre_v = P.vert(Vector((c.x, c.y, max(layer_z(0, c.x, c.y) - 0.03, lo.z + 0.004))), part)
    uv = lambda v: ((v.co.x - lo.x) / tile_m, (v.co.y - lo.y) / tile_m)
    for j in range(n_a):
        j2 = (j + 1) % n_a
        P.poly([centre_v, rings[1][j], rings[1][j2]], CORE, [uv(centre_v), uv(rings[1][j]), uv(rings[1][j2])])
    for i in range(1, n_r):
        for j in range(n_a):
            j2 = (j + 1) % n_a
            q = [rings[i][j], rings[i + 1][j], rings[i + 1][j2], rings[i][j2]]
            P.poly(q, CORE, [uv(v) for v in q])

# border rope: loose arcs over the heap, laid on the top layer's surface
r_t, sides, arcs = cfg.get("rope", [0.02, 6, 2])
rad = r_t / 2


def tube(points, part):
    rings, along = [], 0.0
    for k, p in enumerate(points):
        t = (points[min(k + 1, len(points) - 1)] - points[max(k - 1, 0)]).normalized()
        side = t.cross(Z)
        if side.length < 1e-6:
            side = t.cross(X)
        side.normalize()
        upv = side.cross(t)
        if k:
            along += (p - points[k - 1]).length
        rings.append(([P.vert(p + (side * math.cos(2 * math.pi * s / sides) + upv * math.sin(2 * math.pi * s / sides)) * rad, part)
                       for s in range(sides)], along))
    for k in range(len(rings) - 1):
        (a, va), (b, vb) = rings[k], rings[k + 1]
        for s in range(sides):
            s2 = (s + 1) % sides
            u0, u1 = s / sides * 2 * math.pi * rad / 0.15, (s + 1) / sides * 2 * math.pi * rad / 0.15
            P.poly((a[s], a[s2], b[s2], b[s]), ROPE, ((u0, va / 0.15), (u1, va / 0.15), (u1, vb / 0.15), (u0, vb / 0.15)))
    cap = [(0.01 * math.cos(2 * math.pi * s / sides), 0.01 * math.sin(2 * math.pi * s / sides)) for s in range(sides)]
    P.poly(rings[0][0][::-1], ROPE, cap[::-1])
    P.poly(rings[-1][0], ROPE, cap)


top = len(layers) - 1
for k in range(int(arcs)):
    a0 = rng.uniform(0, 2 * math.pi)
    span = rng.uniform(2.0, 3.2)
    rr = rng.uniform(0.45, 0.8)
    pts = []
    for f in np.linspace(0, 1, 22):
        th = a0 + span * f
        r = rr * (1 + 0.15 * math.sin(3 * th + k))
        x, y = c.x + r * ax * math.cos(th), c.y + r * ay * math.sin(th)
        pts.append(Vector((x, y, layer_z(top, x, y) + rad * 0.8)))
    tube(pts, P.new_part())

P.finish(goal, out, lo, hi, {"layers": len(layers)})
