"""Build the harbour net-drying rack (NW_HARBOUR_FISHINGNET_01): an L of hewn timber with nets.

blender -b --factory-startup -P net_rack.py -- net-rack.json OUT_DIR   (run from the repository root)
Writes OUT_DIR/model.glb (timber, rolled nets, rope, iron: for bake_atlas.py) and OUT_DIR/nets.glb
(the hanging net panels: alpha-tested, tiled, two-sided; merged after the bake by merge_glb.py).
config: {"id": "rig-net-rack", "target": "data/content/mesh3d-rigging.json", "seed": 9,
         "long": {"x": -2.3, "posts": [-3.4, 0.3, 3.7], "beam_z": 4.35, "rail": [-3.4, 0.3, 2.45]},
         "short": {"y": -3.4, "posts": [-2.3, 1.6], "beam_z": 4.05},
         "post": 0.2, "beam": [0.2, 0.18], "brace": [0.6, 0.12],
         "panels": [{"arm": "long", "from": 0.6, "to": 3.3, "drop": [1.0, 2.0], "bar": "beam"}, ...],
         "rolls": [[x, y, length, radius], ...], "loose": [[x, y, length], ...], "tile_m": 0.42, "stave_tile": 0.6}
Asset harbour-rigging (intent.md, concept rig-net-rack attempt 1). The plan follows the original's L: the
long arm runs along y at "long.x" with three posts and a top beam overhanging both ends, a lower rail
between its first two posts; the short arm runs along x at "short.y" with two posts and a top beam a
little lower, crossing the long beam at the corner. Knee braces under the beams at every post, rope
lashings at the joints, one iron strap at the corner. Net panels hang over a bar: draped on both
sides, sagging between the ends, the hem ragged and the drop different on each side ("drop" [front,
back] metres above the floor). Rolled nets hang from the beam ends on a rope loop: tapered, a little
bent, a bundle as thick as the roll's radius down most of its length, gathered at the loop and tapered to the foot,
bound with rope every 0.6 m. A few loose ropes hang from the beams.
"""
import math
import sys
from pathlib import Path

import bpy
import numpy as np
from mathutils import Matrix, Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parts import X, Y, Z, Parts, args, finish_nodes, load_goal  # noqa: E402

cfg, out = args()
goal, lo, hi = load_goal(cfg)
rng = np.random.default_rng(cfg.get("seed", 9))
keys = [z["key"] for z in goal["zones"]]
FRAME, NET, ROLL, ROPE, IRON = (keys.index(k) for k in ("OWN_PROP_WOOD_FRAME", "OWN_PROP_NET", "OWN_PROP_NET_ROLL",
                                                        "OWN_PROP_ROPE", "OWN_PROP_IRON_BAND"))
tile = cfg.get("stave_tile", 0.6)
tile_m = cfg.get("tile_m", 0.42)
P = Parts(rng, tile / 4)        # baked: timber, rolled nets, rope, iron
N = Parts(rng, 0.1)             # tiled: net panels
floor = lo.z
post_w = cfg.get("post", 0.2)
beam_w, beam_h = cfg.get("beam", [0.2, 0.18])
L, S = cfg["long"], cfg["short"]


def lean(axis, amount=0.012):
    """A hewn timber is never quite plumb or level: a small random tilt."""
    return (Matrix.Rotation(rng.uniform(-amount, amount), 3, X) @ Matrix.Rotation(rng.uniform(-amount, amount), 3, Y) @ axis).normalized()


def timber(a, b, w, h, up=Z):
    """A hewn beam or post from point a to point b, w across, h along `up` (or the side axis)."""
    d = b - a
    along = d.normalized()
    side = along.cross(up)
    if side.length < 1e-6:
        side = along.cross(X)
    side.normalize()
    n = side.cross(along)
    P.wood((a + b) / 2, along, side, n, d.length, w * rng.uniform(0.95, 1.05), h * rng.uniform(0.95, 1.05), FRAME, tile)


def tube(Pp, points, rad, sides, material, tile_v=0.15):
    rings, along = [], 0.0
    part = Pp.new_part()
    for k, p in enumerate(points):
        t = (points[min(k + 1, len(points) - 1)] - points[max(k - 1, 0)]).normalized()
        side = t.cross(Z)
        if side.length < 1e-6:
            side = t.cross(X)
        side.normalize()
        upv = side.cross(t)
        r = rad[k] if isinstance(rad, (list, np.ndarray)) else rad
        if k:
            along += (p - points[k - 1]).length
        rings.append(([Pp.vert(p + (side * math.cos(2 * math.pi * s / sides) + upv * math.sin(2 * math.pi * s / sides)) * r, part)
                       for s in range(sides)], along, r))
    for k in range(len(rings) - 1):
        (a, va, ra), (b, vb, rb) = rings[k], rings[k + 1]
        for s in range(sides):
            s2 = (s + 1) % sides
            u0, u1 = s / sides * 2 * math.pi * ra / tile_v, (s + 1) / sides * 2 * math.pi * ra / tile_v
            Pp.poly((a[s], a[s2], b[s2], b[s]), material, ((u0, va / tile_v), (u1, va / tile_v), (u1, vb / tile_v), (u0, vb / tile_v)))
    for ring, flip in ((rings[0][0], True), (rings[-1][0], False)):
        cap = [(0.05 * math.cos(2 * math.pi * s / sides), 0.05 * math.sin(2 * math.pi * s / sides)) for s in range(sides)]
        Pp.poly(ring[::-1] if flip else ring, material, cap[::-1] if flip else cap)


def lashing(centre, axis, radius, turns=4, rope=0.022):
    """Rope wound a few turns around a timber at a joint."""
    axis = axis.normalized()
    u = axis.cross(Z) if abs(axis.dot(Z)) < 0.9 else axis.cross(X)
    u.normalize()
    v = axis.cross(u)
    pts = []
    for i in range(turns * 10 + 1):
        a = 2 * math.pi * i / 10
        off = (i / 10 - turns / 2) * rope * 1.05
        pts.append(centre + axis * off + (u * math.cos(a) + v * math.sin(a)) * radius)
    tube(P, pts, rope / 2, 5, ROPE)


# --- the long arm: three posts, a top beam over them, a lower rail, knee braces ---
xL = L["x"]
y0, y1 = lo.y + 0.01, hi.y - 0.01
for y in L["posts"]:
    foot = Vector((xL, y, floor))
    timber(foot, foot + lean(Z) * (L["beam_z"] - beam_h / 2 - floor + 0.01), post_w, post_w, up=X)
timber(Vector((xL, y0, L["beam_z"])), Vector((xL, y1, L["beam_z"] + rng.uniform(-0.03, 0.03))), beam_w, beam_h)
r0, r1, rz = L["rail"]
timber(Vector((xL + post_w * 0.5 + 0.06, r0 - 0.25, rz)), Vector((xL + post_w * 0.5 + 0.06, r1 + 0.25, rz + 0.04)), 0.12, 0.12)
brace_len, brace_w = cfg.get("brace", [0.6, 0.12])
for y in L["posts"]:
    for s in (-1, 1):
        a = Vector((xL, y, L["beam_z"] - beam_h / 2 - brace_len * 0.7))
        b = Vector((xL, y + s * brace_len * 0.7, L["beam_z"] - beam_h / 2))
        timber(a, b, brace_w, brace_w, up=X)
    lashing(Vector((xL, y, L["beam_z"])), Y, 0.135)

# --- the short arm: two posts, a beam a little lower crossing the long beam at the corner ---
yS = S["y"]
x0, x1 = lo.x + 0.01, hi.x - 0.01
for x in S["posts"]:
    if abs(x - xL) < 0.05:
        x = xL + post_w + 0.04          # the corner: a second post beside the long arm's post
    foot = Vector((x, yS, floor))
    timber(foot, foot + lean(Z) * (S["beam_z"] - beam_h / 2 - floor + 0.01), post_w, post_w, up=Y)
    for s in (-1, 1):
        a = Vector((x, yS, S["beam_z"] - beam_h / 2 - brace_len * 0.7))
        b = Vector((x + s * brace_len * 0.7, yS, S["beam_z"] - beam_h / 2))
        timber(a, b, brace_w, brace_w, up=Y)
    lashing(Vector((x, yS, S["beam_z"])), X, 0.135)
timber(Vector((x0, yS, S["beam_z"])), Vector((x1, yS, S["beam_z"] + rng.uniform(-0.03, 0.03))), beam_w, beam_h)
# iron strap round the corner joint
P.strap(Vector((xL, yS, S["beam_z"] + beam_h / 2 + 0.002)), X, Y, Z, 0.5, 0.07, 0.008, IRON)
P.strap(Vector((xL, yS, L["beam_z"] + beam_h / 2 + 0.002)), Y, X, Z, 0.5, 0.07, 0.008, IRON)

# --- net panels: draped over a bar, sagging, ragged hem ---
def bar_line(panel):
    if panel["arm"] == "long":
        z = L["beam_z"] if panel.get("bar", "beam") == "beam" else L["rail"][2]
        xo = 0.0 if panel.get("bar", "beam") == "beam" else post_w * 0.5 + 0.06
        return (lambda t: Vector((xL + xo, panel["from"] + (panel["to"] - panel["from"]) * t, z))), X, (0.11 if panel.get("bar", "beam") == "beam" else 0.07)
    z = S["beam_z"]
    return (lambda t: Vector((panel["from"] + (panel["to"] - panel["from"]) * t, yS, z))), Y, 0.11


for panel in cfg.get("panels", []):
    line, normal, half = bar_line(panel)
    nu, nv = 10, 9
    part = N.new_part(wound=True)
    hem = rng.uniform(-0.12, 0.12, nu + 1)
    for side, drop in zip((1, -1), panel["drop"]):
        grid = []
        for i in range(nu + 1):
            t = i / nu
            top = line(t) + normal * (side * half) + Vector((0, 0, 0.06))
            depth = top.z - (floor + drop) + hem[i] * (1 if side > 0 else 0.6)
            sag_out = 0.03 * math.sin(math.pi * t)
            col = []
            for j in range(nv + 1):
                f = j / nv
                # the net hangs straight down, bellying out a little and swinging gently along the bar
                p = top + Vector((0, 0, -depth * f)) + normal * (side * (sag_out + 0.06 * math.sin(math.pi * f) * (0.5 + 0.5 * math.sin(3 * t + j))))
                p += (Vector((1, 0, 0)) if normal == Y else Vector((0, 1, 0))) * (0.04 * math.sin(2 * math.pi * f + 4 * t) * f)
                col.append(p)
            grid.append(col)
        up = [[N.vert(p, part) for p in col] for col in grid]
        down = [[N.vert(p, part) for p in col] for col in grid]
        width = (line(1) - line(0)).length
        for i in range(nu):
            for j in range(nv):
                q = [(i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1)]
                uv = [((width * a / nu) / tile_m, (grid[a][b] - grid[a][0]).length / tile_m + side * 0.3) for a, b in q]
                N.poly([up[a][b] for a, b in q], NET, uv)
                N.poly([down[a][b] for a, b in q[::-1]], NET, uv[::-1])

# --- rolled nets: tapered bundles hanging on a rope loop from the beam ends, bound every 0.6 m ---
for x, y, length, radius in cfg.get("rolls", []):
    top_z = (L["beam_z"] if abs(x - xL) < 0.4 else S["beam_z"]) - beam_h / 2 - 0.12
    bend = Vector((rng.uniform(-0.08, 0.08), rng.uniform(-0.08, 0.08), 0))
    n_seg = 12
    pts, rads = [], []
    for k in range(n_seg + 1):
        f = k / n_seg
        pts.append(Vector((x, y, top_z - length * f)) + bend * math.sin(math.pi * f))
        # thick in the middle, gathered at the top, tapering to the foot; lumpy
        rads.append(radius * (1.0 - 0.25 * f) * min(1.0, 0.55 + 4 * f) * rng.uniform(0.94, 1.06))
    rads[0], rads[-1] = radius * 0.45, radius * 0.35
    tube(P, pts, rads, 10, ROLL, tile_v=tile_m)
    for k in range(1, int(length / 0.6) + 1):
        f = k * 0.6 / length
        if f >= 0.97:
            break
        c = pts[0] + (pts[-1] - pts[0]) * f + bend * math.sin(math.pi * f)
        r = float(np.interp(f, np.linspace(0, 1, n_seg + 1), rads)) * 0.97
        ring = [c + Vector((math.cos(a) * r, math.sin(a) * r, 0.0)) for a in np.linspace(0, 2 * math.pi, 11)]
        tube(P, ring, 0.012, 5, ROPE)
    # the loop up to the beam
    beam_top = (L["beam_z"] if abs(x - xL) < 0.4 else S["beam_z"]) + beam_h / 2
    tube(P, [pts[0], Vector((x, y, beam_top + 0.02))], 0.012, 5, ROPE)

# --- loose ropes hanging from the beams ---
for x, y, length in cfg.get("loose", []):
    z = (L["beam_z"] if abs(x - xL) < 0.4 else S["beam_z"]) - beam_h / 2
    swing = Vector((rng.uniform(-0.1, 0.1), rng.uniform(-0.1, 0.1), 0))
    tube(P, [Vector((x, y, z + 0.01)) + swing * (f ** 2) - Vector((0, 0, length * f)) for f in np.linspace(0, 1, 7)], 0.012, 5, ROPE)

metrics = finish_nodes(goal, out, lo, hi, [(goal["id"], P)])
# the net panels: their own GLB with the NET zone only
for o in list(bpy.context.scene.objects):
    bpy.data.objects.remove(o)
obj, open_edges = N.to_object(goal, goal["id"] + "_nets")
obj.select_set(True)
bpy.context.view_layer.objects.active = obj
bpy.ops.export_scene.gltf(filepath=str(out / "nets.glb"), export_format="GLB", use_selection=True, export_apply=True,
                          export_yup=True, export_extras=True, export_normals=True, export_texcoords=True,
                          export_animations=False, export_attributes=True)
metrics["net_triangles"] = len(obj.data.polygons)
(out / "metrics.json").write_text(__import__("json").dumps(metrics, indent=2))
print("NETS", metrics["net_triangles"])
