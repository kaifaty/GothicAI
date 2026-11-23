"""Build the harbour net-drying rack (NW_HARBOUR_FISHINGNET_01): an L of hewn timber with nets.

blender -b --factory-startup -P net_rack.py -- net-rack.json OUT_DIR   (run from the repository root)
Writes OUT_DIR/model.glb (timber, rolled nets, rope, iron: for bake_atlas.py) and OUT_DIR/nets.glb
(the hanging net panels: alpha-tested, tiled, two-sided; merged after the bake by merge_glb.py).
config: {"id": "rig-net-rack", "target": "data/content/mesh3d-rigging.json", "seed": 9,
         "timbers": [[x0, y0, z0, x1, y1, z1], ...]   hewn posts and beams as boxes (m, Blender axes),
         "clamps": [[x, y, z, axis, width, height], ...] iron bands round a pair of beams at a post,
         "braces": [[x, y, z_top, axis, sign], ...] knee braces from a post up to the beam over it,
         "cleats": [[x, y, axis], ...] short boards up both faces of a post under the beams (the original's),
         "lashings": [[x, y, z, axis], ...], "panels": [{"bar": [x0, y0, z, x1, y1], "drop": [front, back],
         "folds": 5, "belly": 0.08, "half": half the bar's width, "sag": droop m}, ...], "rolls": [[x, y, top_z, length, radius, "spindle"|"column", hang_z], ...],
         "wraps": [[x0, x1, y0, y1, z0, z1], ...], "loose": [[x, y, z, length], ...], "tile_m": 0.42, "stave_tile": 0.6}
Asset harbour-rigging (intent.md, concept rig-net-rack attempt 1). The construction follows the original
(its mesh as an underlay: the places of its posts and beams, docs/own-content.md): four posts, pairs of
top beams that clamp the posts between them on both arms, a second tier of beams, a tie from the corner
along the long arm overhanging its end, a lower rail; iron bands round the beam pairs at the posts, knee
braces ("lashings": rope coils at the top corners, used until the original's rope wraps were built as
"wraps"; the harbour rack has none: two of them hung under the beams as a lump, sixth review). The first review (2026-10-30) found the first version thin,
flat and in the wrong places. Net panels hang over a bar on both sides with vertical folds ("folds") and
a belly ("belly" m), the hem a little uneven. Rolled nets hang on a rope loop: a spindle (thick in the
middle) or a column (nearly straight, gathered at the top), bound with rope every 0.6 m; with "hang_z" the
roll hangs lower on a rope from that height, as the original's rolls hang from the rope wound round a beam.
"wraps" are those windings: rope wound round a top beam in tight turns over the box the original's
ROPEWALL wraps fill (outer bounds, rope included; the beam runs along the longer horizontal side). The
fifth review found a net lump under the beam where the original has rope wound round it.
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


def tube(Pp, points, rad, sides, material, tile_v=0.15, ref=None, closed=False):
    """A tube along points; "ref" fixes the ring orientation (a vertical roll would otherwise twist its UVs
    into a screw), "closed" joins the last ring to the first with no caps (a binding round a roll)."""
    rings, along = [], 0.0
    part = Pp.new_part()
    n = len(points)
    for k, p in enumerate(points):
        if closed:
            t = (points[(k + 1) % n] - points[(k - 1) % n]).normalized()
        else:
            t = (points[min(k + 1, n - 1)] - points[max(k - 1, 0)]).normalized()
        side = t.cross(ref) if ref is not None else t.cross(Z)
        if side.length < 1e-6:
            side = t.cross(X)
        side.normalize()
        upv = side.cross(t)
        r = rad[k] if isinstance(rad, (list, np.ndarray)) else rad
        if k:
            along += (p - points[k - 1]).length
        rings.append(([Pp.vert(p + (side * math.cos(2 * math.pi * s / sides) + upv * math.sin(2 * math.pi * s / sides)) * r, part)
                       for s in range(sides)], along, r))
    for k in range(len(rings) - (0 if closed else 1)):
        (a, va, ra), (b, vb, rb) = rings[k], rings[(k + 1) % len(rings)]
        for s in range(sides):
            s2 = (s + 1) % sides
            u0, u1 = s / sides * 2 * math.pi * ra / tile_v, (s + 1) / sides * 2 * math.pi * ra / tile_v
            Pp.poly((a[s], a[s2], b[s2], b[s]), material, ((u0, va / tile_v), (u1, va / tile_v), (u1, vb / tile_v), (u0, vb / tile_v)))
    if closed:
        return
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


# --- timber: posts and beams as hewn boxes, each a little out of true ---
for x0, y0, z0, x1, y1, z1 in cfg["timbers"]:
    size = (x1 - x0, y1 - y0, z1 - z0)
    axis = int(np.argmax(size))
    ax = (X, Y, Z)[axis]
    a = Vector((x0, y0, z0)) + Vector(size) / 2 - ax * (size[axis] / 2)
    b = a + lean(ax, 0.006) * size[axis]
    others = [i for i in range(3) if i != axis]
    w, h = size[others[0]], size[others[1]]
    side = (X, Y, Z)[others[0]]
    n = ax.cross(side)
    P.wood((a + b) / 2, (b - a).normalized(), side, n, size[axis], w * rng.uniform(0.96, 1.04), h * rng.uniform(0.96, 1.04), FRAME, tile)

for x, y, z, axis, width, height in cfg.get("clamps", []):
    # an iron band round a pair of beams: four flat straps closing a rectangle across the beam axis
    along = X if axis == "x" else Y
    across = Y if axis == "x" else X
    c = Vector((x, y, z))
    t_band, w_band = 0.02, 0.1
    # a hewn block laid across the pair on top, as the original's
    P.wood(c + Z * (height / 2 + 0.06), across, along, Z, width + 0.1, 0.18, 0.12, FRAME, tile)
    for s in (-1, 1):
        P.strap(c + Z * (s * height / 2), along, across, Z * s, w_band, width + 2 * t_band, t_band, IRON)
        P.board(c + across * (s * (width / 2 + t_band / 2)), along, Z, across, w_band, height + 2 * t_band, t_band, IRON)

for x, y, z_top, axis, sign in cfg.get("braces", []):
    along = X if axis == "x" else Y
    up = cfg.get("brace", [0.7, 0.12])
    a = Vector((x, y, z_top - up[0]))
    b = Vector((x, y, z_top)) + along * (sign * up[0])
    timber(a, b, up[1], up[1], up=Y if axis == "x" else X)

for x, y, axis in cfg.get("cleats", []):
    # a cleat: a short board nailed up the post's face under the beams, as the original has, not a brace
    across = X if axis == "y" else Y
    for s in (-1, 1):
        c = Vector((x, y, 3.62)) + across * (s * 0.135)
        P.wood(c, Z, Y if axis == "y" else X, across * s, 0.9, 0.16, 0.05, FRAME, tile)

for x, y, z, axis in cfg.get("lashings", []):
    lashing(Vector((x, y, z)), X if axis == "x" else Y, 0.17, turns=5)

# --- net panels: over a bar, both sides, vertical folds, a belly, the hem a little uneven ---
for panel in cfg.get("panels", []):
    x0, y0, z_bar, x1, y1 = panel["bar"]
    start, end = Vector((x0, y0, z_bar)), Vector((x1, y1, z_bar))
    along = (end - start).normalized()
    normal = along.cross(Z).normalized()
    width = (end - start).length
    nu, nv = 14, 10
    folds, belly = panel.get("folds", 5), panel.get("belly", 0.08)
    part = N.new_part(wound=True)
    hem = np.convolve(rng.uniform(-0.05, 0.05, nu + 3), np.ones(3) / 3, 'valid')   # a smooth, slightly uneven hem
    phase = rng.uniform(0, 6.28)
    for side, drop in zip((1, -1), panel["drop"]):
        grid = []
        for i in range(nu + 1):
            u = i / nu
            sag = panel.get("sag", 0.0) * math.sin(math.pi * u)        # the net droops between its ends
            top = start + along * (width * u) + normal * (side * panel.get("half", 0.12)) + Vector((0, 0, 0.03 - sag * 0.25))
            depth = top.z - (floor + drop) + hem[i]
            col = []
            for j in range(nv + 1):
                f = j / nv
                fold = math.sin(2 * math.pi * folds * u + phase + side) * (0.03 + 0.07 * f)
                bulge = belly * math.sin(math.pi * f) * (0.6 + 0.4 * math.sin(math.pi * u))
                # the net gathers a little toward the middle as it hangs
                gather = (0.5 - u) * 0.06 * f
                droop = sag * math.sin(math.pi * f) * 0.75
                col.append(top + Vector((0, 0, -depth * f - droop)) + normal * (side * (bulge + fold)) + along * gather)
            grid.append(col)
        up = [[N.vert(q, part) for q in col] for col in grid]
        down = [[N.vert(q, part) for q in col] for col in grid]
        for i in range(nu):
            for j in range(nv):
                q = [(i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1)]
                uv = [((width * a / nu) / tile_m, (grid[a][b] - grid[a][0]).length / tile_m + side * 0.3) for a, b in q]
                N.poly([up[a][b] for a, b in q], NET, uv)
                N.poly([down[a][b] for a, b in q[::-1]], NET, uv[::-1])

# --- rolled nets: a spindle or a column on a rope loop, bound with rope every 0.6 m ---
for x, y, top_z, length, radius, shape, *hang in cfg.get("rolls", []):
    bend = Vector((rng.uniform(-0.06, 0.06), rng.uniform(-0.06, 0.06), 0))
    n_seg = 26
    pts, rads = [], []
    for k in range(n_seg + 1):
        f = k / n_seg
        pts.append(Vector((x, y, top_z - 0.15 - length * f)) + bend * math.sin(math.pi * f))
        if shape == "spindle":
            r = radius * (0.25 + 0.75 * math.sin(math.pi * f) ** 0.7)
        else:
            r = radius * min(1.0, 0.45 + 3 * f) * (1 - 0.15 * f)
        # the net bulges between the bindings and is drawn in under each: a smooth cylinder read as a
        # post (fifth review: «гладкий цилиндр в крапинку, а нужно веретено»)
        r *= 0.9 + 0.14 * abs(math.sin(math.pi * f * length / 0.6))
        rads.append(r * rng.uniform(0.96, 1.04))
    rads[0], rads[-1] = radius * 0.35, radius * (0.6 if shape == "spindle" else 0.85)
    # a blunt, rounded foot: one more ring a little lower and a little narrower, then the flat cap
    pts.append(pts[-1] - Vector((0, 0, radius * 0.15)))
    rads.append(rads[-1] * 0.8)
    tube(P, pts, rads, 12, ROLL, tile_v=tile_m, ref=Y)
    for k in range(1, int(length / 0.6) + 1):
        f = k * 0.6 / length
        if f >= 0.95:
            break
        c = pts[0] + (pts[-1] - pts[0]) * f + bend * math.sin(math.pi * f)
        r = float(np.interp(f, np.linspace(0, 1, n_seg + 1), rads[:n_seg + 1])) * 0.98
        ring = [c + Vector((math.cos(a) * r, math.sin(a) * r, 0.0)) for a in np.linspace(0, 2 * math.pi, 17)[:-1]]
        tube(P, ring, 0.014, 5, ROPE, closed=True)
    tube(P, [pts[0], Vector((x, y, (hang[0] if hang else top_z + 0.02)))], 0.014, 5, ROPE)

# --- rope wound round the top beams ---
r_w = 0.02
for x0, x1, y0, y1, z0, z1 in cfg.get("wraps", []):
    along_x = (x1 - x0) > (y1 - y0)
    a0, a1 = (x0, x1) if along_x else (y0, y1)          # along the beam
    c0, c1 = (y0, y1) if along_x else (x0, x1)          # across it
    cc, hw = (c0 + c1) / 2, (c1 - c0) / 2 - r_w
    zc, hh = (z0 + z1) / 2, (z1 - z0) / 2 - r_w
    corner = 0.035
    # a rounded rectangle round the beam section, as the rope bends over the beam's edges
    ring = []
    for qx, qz in ((1, 1), (-1, 1), (-1, -1), (1, -1)):
        start = math.atan2(qz, qx) - math.pi / 4
        for f in (0.0, 0.5):
            ang = start + f * math.pi / 2
            ring.append((qx * (hw - corner) + corner * math.cos(ang), qz * (hh - corner) + corner * math.sin(ang)))
    pitch = r_w * 2.3
    turns = int(((a1 - a0) - 2 * r_w) / pitch)
    pts = []
    for k in range(turns * len(ring) + 1):
        u, w = ring[k % len(ring)]
        s = a0 + r_w + pitch * k / len(ring)
        pts.append(Vector((s, cc + u, zc + w)) if along_x else Vector((cc + u, s, zc + w)))
    tube(P, pts, r_w, 5, ROPE)

# --- loose ropes hanging from the beams ---
for x, y, z, length in cfg.get("loose", []):
    swing = Vector((rng.uniform(-0.1, 0.1), rng.uniform(-0.1, 0.1), 0))
    tube(P, [Vector((x, y, z)) + swing * (f ** 2) - Vector((0, 0, length * f)) for f in np.linspace(0, 1, 7)], 0.012, 5, ROPE)

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
