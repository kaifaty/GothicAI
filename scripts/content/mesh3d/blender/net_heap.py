"""Build a heap of fishing net (NW_HARBOUR_FISHINGNETBIG, _SMALL) from layered sheets.

blender -b --factory-startup -P net_heap.py -- net-heap.json OUT_DIR   (run from the repository root)
config: {"id": "rig-net-big", "target": "data/content/mesh3d-rigging.json", "seed": 6,
         "layers": [[1.0, 0, 0], [0.86, 0.05, -0.04], ...], "rings": 9, "segments": 30,
         "profile": [0.92, 0.6, 0.68, 0.4], "edge": 0.1, "lift": 0.022, "folds": [3, [0.02, 0.05], [0.4, 0.8]],
         "tile_m": 0.52}
Asset harbour-rigging (intent.md, item 8): a low dense dome of net, the ground showing only toward the edge.
The original's heap lies flat on the ground over the outer third of its radius, rises steeply at about 0.7
of it to a plateau about two thirds of its height, with a crown in the middle (its vertices: rim at the
bottom of the bbox out from 0.68, plateau 0.65-0.73 of the height at 0.4-0.7, top at 0.2); that is
"profile" [radius share where the rise starts from the ground, where it reaches the plateau, plateau
height share, crown radius]. The heap is "layers" sheets of net piled on each other ([scale, x shift, y
shift] as shares of the bbox half sizes), each smaller than the one under it, each "lift" metres over
it: through five layers of alpha-tested net the middle reads as dense net in layers, and only the
outermost one lies over the ground. A layer is a polar grid ("rings" x "segments"), so its outline is a
soft wobbling curve ("edge": its share), not the staircase a square grid leaves (fifth review: «контур не
восьмиугольник или зонт, а мягкий и неровный»); near its own edge each layer falls onto the one under it,
and the lowest onto the ground. "folds" (count, amplitude range m, wavelength range m) ripple each layer,
fading toward its edge. Every face is emitted twice, back to back, so the net shows from both sides. Net
UVs are planar in metres over tile_m, each layer turned and shifted at random so the meshes of the layers
do not line up into a moire. The lower layers are lit like the top one, so through the meshes the heap read
as even felt; the inner part of every layer under the top "top_layers" is the darker OWN_PROP_NET_UNDER, as
the shade deep in a heap: only where a layer above covers it with a margin ("covered": share of that
layer's radius), so a band of a lower layer left open between two rims keeps the net's own tone (choosing
by a face's place in its own layer left hard dark crescents between the terraces, round 8). The first versions laid two sheets and an opaque core: in the game that read
as one pale veil over a smooth shell (fifth review, 2026-10-30).
"""
import math
import sys
from pathlib import Path

import numpy as np
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parts import Parts, args, load_goal  # noqa: E402

cfg, out = args()
goal, lo, hi = load_goal(cfg)
rng = np.random.default_rng(cfg.get("seed", 6))
keys = [z["key"] for z in goal["zones"]]
NET = keys.index("OWN_PROP_NET")
UNDER = keys.index("OWN_PROP_NET_UNDER") if "OWN_PROP_NET_UNDER" in keys else NET
P = Parts(rng, 0.1)
tile_m = cfg.get("tile_m", 0.52)
c = Vector(((lo.x + hi.x) / 2, (lo.y + hi.y) / 2, lo.z))
ax, ay = (hi.x - lo.x) / 2 - 0.01, (hi.y - lo.y) / 2 - 0.01
H = hi.z - lo.z
edge = cfg.get("edge", 0.1)
lift = cfg.get("lift", 0.022)
n_folds, amp, wave = cfg.get("folds", [3, [0.02, 0.05], [0.4, 0.8]])
foot, top_r, plateau, crown = cfg.get("profile", [0.92, 0.6, 0.68, 0.4])
layer_cfg = cfg.get("layers", [[1.0, 0, 0], [0.86, 0.05, -0.04], [0.72, -0.04, 0.05], [0.57, 0.06, 0.02], [0.42, -0.03, -0.05]])
n_rings, n_seg = cfg.get("rings", 9), cfg.get("segments", 30)
# the top layer's crown touches the bbox top
h0 = H - 0.008 - lift * (len(layer_cfg) - 1) - amp[1]


def smooth(x):
    x = min(max(x, 0.0), 1.0)
    return x * x * (3 - 2 * x)


def profile(r):
    """Height share of the heap at r (0 centre, 1 the outermost layer's rim): a steep rise from the rim to
    the plateau, then a crown in the middle."""
    rise = smooth((foot - r) / (foot - top_r))
    top = 1 - (min(r / crown, 1.0)) ** 2
    return plateau * rise + (1 - plateau) * top * rise


def make_layer(scale, sx, sy):
    harm = rng.uniform(-1, 1, (5, 2)) / np.arange(1, 6)[:, None] ** 0.5
    folds = [(rng.uniform(*amp), rng.uniform(*wave), rng.uniform(0, math.pi), rng.uniform(0, 2 * math.pi)) for _ in range(n_folds)]
    cx, cy = c.x + sx * ax, c.y + sy * ay

    def wobble(th):
        return 1 + edge * sum(h[0] * math.cos((k + 2) * th) + h[1] * math.sin((k + 2) * th) for k, h in enumerate(harm)) / 1.6
    ths = np.linspace(0, 2 * math.pi, 360, endpoint=False)
    rc = np.array([wobble(th) * math.cos(th) for th in ths])
    rs = np.array([wobble(th) * math.sin(th) for th in ths])
    # the outline fitted to "scale" of the half sizes on both axes: its extents span exactly the layer
    kx, ky = 2 / (rc.max() - rc.min()), 2 / (rs.max() - rs.min())
    ox, oy = (rc.max() + rc.min()) / 2, (rs.max() + rs.min()) / 2

    def point(th, f):
        """A point of the layer at angle th, f of the way out to its rim."""
        return (cx + ax * scale * (wobble(th) * math.cos(th) * f - ox * f) * kx,
                cy + ay * scale * (wobble(th) * math.sin(th) * f - oy * f) * ky)

    def radius(th):
        """The layer's rim at angle th, as a share of the heap's half sizes (for layer_f)."""
        return scale * wobble(th) * (kx + ky) / 2

    def fold(x, y):
        return sum(a * math.sin(2 * math.pi * ((x - cx) * math.cos(d) + (y - cy) * math.sin(d)) / w + ph)
                   for a, w, d, ph in folds)
    # rings and segments shrink with the layer, so the small upper layers cost little
    return dict(cx=cx, cy=cy, radius=radius, point=point, fold=fold, rings=max(3, round(n_rings * scale)),
                segments=max(12, round(n_seg * scale ** 0.5)))


layers = [make_layer(*l) for l in layer_cfg]
r_max = max(layers[0]["radius"](th) for th in np.linspace(0, 2 * math.pi, 90))


def heap_r(x, y):
    """Distance from the heap centre as a share of the outermost rim."""
    return math.hypot((x - c.x) / ax, (y - c.y) / ay) / r_max


def layer_f(i, x, y):
    """Where (x, y) lies in layer i: 0 at its centre, 1 on its rim, more outside it."""
    L = layers[i]
    dx, dy = (x - L["cx"]) / ax, (y - L["cy"]) / ay
    return math.hypot(dx, dy) / L["radius"](math.atan2(dy, dx))


def layer_z(i, x, y, f=None):
    """Height of layer i at (x, y), at f (0 centre, 1 its rim) of its own radius: on the dome, or falling
    near its rim onto the layer under it (the lowest onto the ground)."""
    L = layers[i]
    f = layer_f(i, x, y) if f is None else f
    if i and layer_f(i - 1, x, y) >= 1:
        under = lo.z + 0.006
    else:
        under = layer_z(i - 1, x, y) + 0.008 if i else lo.z + 0.006
    full = lo.z + 0.006 + h0 * profile(heap_r(x, y)) + lift * i + L["fold"](x, y) * max(1 - f, 0) ** 0.7
    s = smooth((f - 0.72) / 0.28)
    return max(full * (1 - s) + under * s, under)


for i, L in enumerate(layers):
    part = P.new_part(wound=True)
    turn, shift = rng.uniform(0, math.pi), rng.uniform(0, tile_m, 2)
    cos_t, sin_t = math.cos(turn), math.sin(turn)
    n_rings_i, n_seg_i = L["rings"], L["segments"]
    thetas = np.linspace(0, 2 * math.pi, n_seg_i + 1)[:-1]
    grid = {}
    for a, th in enumerate(thetas):
        for k in range(1, n_rings_i + 1):
            f = k / n_rings_i
            # rings closer together toward the rim, where the layer bends down
            f = 1 - (1 - f) ** 1.25
            x, y = L["point"](th, f)
            grid[a, k] = Vector((x, y, layer_z(i, x, y, f)))
    centre = Vector((L["cx"], L["cy"], layer_z(i, L["cx"], L["cy"], 0.0)))

    def uv(v):
        x, y = v.x - lo.x, v.y - lo.y
        return ((x * cos_t - y * sin_t) / tile_m + shift[0], (x * sin_t + y * cos_t) / tile_m + shift[1])
    up, down = {}, {}

    def mat(corners):
        """The darker under-net where a layer above covers this face, the net's own tone elsewhere."""
        if i >= len(layers) - cfg.get("top_layers", 1):
            return NET
        x = sum(v.x for v in corners) / len(corners)
        y = sum(v.y for v in corners) / len(corners)
        covered = any(layer_f(j, x, y) < cfg.get("covered", 0.8) for j in range(i + 1, len(layers)))
        return UNDER if covered else NET

    def pair(key, v):
        if key not in up:
            up[key], down[key] = P.vert(v, part), P.vert(v, part)
        return up[key], down[key]
    pair("c", centre)
    for a in range(n_seg_i):
        a2 = (a + 1) % n_seg_i
        tri = [("c", centre), ((a, 1), grid[a, 1]), ((a2, 1), grid[a2, 1])]
        for key, v in tri:
            pair(key, v)
        m = mat([v for _, v in tri])
        P.poly([up[k] for k, _ in tri], m, [uv(v) for _, v in tri])
        P.poly([down[k] for k, _ in tri[::-1]], m, [uv(v) for _, v in tri[::-1]])
        for k in range(1, n_rings_i):
            quad = [((a, k), grid[a, k]), ((a, k + 1), grid[a, k + 1]), ((a2, k + 1), grid[a2, k + 1]), ((a2, k), grid[a2, k])]
            for key, v in quad:
                pair(key, v)
            m = mat([v for _, v in quad])
            P.poly([up[q] for q, _ in quad], m, [uv(v) for _, v in quad])
            P.poly([down[q] for q, _ in quad[::-1]], m, [uv(v) for _, v in quad[::-1]])

P.finish(goal, out, lo, hi, {"layers": len(layers)})
