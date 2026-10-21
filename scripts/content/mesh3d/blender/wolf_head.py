"""Build a mounted wolf head trophy (NW_CITY_DECO_WOLFHEAD_01) from an image_gen concept and a numeric target.

blender -b --factory-startup -P wolf_head.py -- wolf-head.json OUT_DIR   (run from the repository root)
config: {"id": "wolf-head", "target": "data/content/mesh3d-market.json", "seed": 1, "around": 14,
         "plaque": [0.1, 3], "sections": [[s, w, h, dz], ...], "jaw": [[s, w, h], ...], "jaw_open_deg": 22,
         "turn_deg": 14, "dip_deg": 10, "ruff": [24, 0.12], "ears": [0.19, 0.12], "fangs": 4, "eye": 0.026, "nose": 0.036, "eye_at": 0.6,
         "fur_tile": 0.3, "wood_tile": 0.38}
A pointed shield plaque of three boards against the wall (the original's plaque is
on that side of its bbox), and a wolf's head lofted from elliptical sections along
its muzzle axis: a wide fur ruff at the plaque, the skull, cheeks, the stop and a
narrowing muzzle to the nose. The lower jaw hangs open under the muzzle; fangs in
both jaws; two pointed ears; the ruff breaks into jagged clumps; two amber eyes
at "eye_at" of the way to the muzzle tip and a black nose at the tip, each a
shallow dome in its own zone. The head looks a little down and to one side, as
on the concept.
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
rng = np.random.default_rng(cfg.get("seed", 1))
keys = [z["key"] for z in goal["zones"]]
WOOD, FUR, BONE, EYE, NOSE = (keys.index(k) for k in ("OWN_PROP_WOOD_PLANK", "OWN_PROP_FUR", "OWN_PROP_BONE",
                                                      "OWN_PROP_EYE", "OWN_PROP_NOSE"))
fur_tile, wood_tile = cfg.get("fur_tile", 0.3), cfg.get("wood_tile", 0.38)
P = Parts(rng, wood_tile / 4)
around = cfg.get("around", 14)
cx, cz = (lo.x + hi.x) / 2, (lo.z + hi.z) / 2

# --- plaque: pointed shield of vertical boards, flat on the wall (-Y) ---
thick, boards = cfg.get("plaque", [0.1, 3])
w, h = hi.x - lo.x, hi.z - lo.z
outline = [(-w / 2, h / 2 - 0.08), (-w / 2 + 0.08, h / 2), (w / 2 - 0.08, h / 2), (w / 2, h / 2 - 0.08), (w / 2, -h * 0.05)]
for a in np.linspace(0, 1, 6)[1:]:      # flanks curving in to a point at the bottom
    outline.append((w / 2 * (1 - a) ** 0.8, -h * 0.05 - (h * 0.45) * a ** 1.4))
for a in np.linspace(1, 0, 6)[1:-1]:
    outline.append((-w / 2 * (1 - a) ** 0.8, -h * 0.05 - (h * 0.45) * a ** 1.4))
outline.append((-w / 2, -h * 0.05))


def clip(poly, x0, x1):
    for bound, keep in ((x0, lambda x: x >= x0), (x1, lambda x: x <= x1)):
        res = []
        for i, p in enumerate(poly):
            q = poly[(i + 1) % len(poly)]
            if keep(p[0]):
                res.append(p)
            if keep(p[0]) != keep(q[0]):
                f = (bound - p[0]) / (q[0] - p[0])
                res.append((bound, p[1] + f * (q[1] - p[1])))
        poly = res
    return poly


wall, face = lo.y, lo.y + thick
bevel = 0.02
edges = np.linspace(-w / 2, w / 2, boards + 1)
for b in range(boards):
    poly = clip(outline, edges[b] + 0.002, edges[b + 1] - 0.002)
    part = P.new_part()
    ou, ov = rng.uniform(0, wood_tile), rng.uniform(0, wood_tile)
    # the front face is a little smaller than the back: a carved bevel round the edge
    shrink = lambda x, z: (x * (1 - bevel / (w / 2)), z * (1 - bevel / (h / 2)))
    fr = [P.vert(Vector((cx + sx, face, cz + sz)), part) for sx, sz in (shrink(x, z) for x, z in poly)]
    bk = [P.vert(Vector((cx + x, wall, cz + z)), part) for x, z in poly]
    # Front and back as fans from the centroid: the flanks are concave, and the shared n-gon
    # triangulation folded a triangle over a reflex corner of the front face (seen from behind in game).
    mx, mz = sum(x for x, _ in poly) / len(poly), sum(z for _, z in poly) / len(poly)
    for i in range(len(poly)):
        (x0, z0), (x1, z1) = poly[i], poly[(i + 1) % len(poly)]
        assert (x0 - mx) * (z1 - mz) - (z0 - mz) * (x1 - mx) < 0, "board outline is not star-shaped from its centroid"
    mid_fr = P.vert(Vector((cx + shrink(mx, mz)[0], face, cz + shrink(mx, mz)[1])), part)
    mid_bk = P.vert(Vector((cx + mx, wall, cz + mz)), part)
    for i in range(len(poly)):
        j = (i + 1) % len(poly)
        uv = [(ou + poly[k][0], ov + poly[k][1]) for k in (i, j)] + [(ou + mx, ov + mz)]
        P.poly((fr[j], fr[i], mid_fr), WOOD, [uv[1], uv[0], uv[2]])
        P.poly((bk[i], bk[j], mid_bk), WOOD, uv)
    n = len(poly)
    for i in range(n):
        j = (i + 1) % n
        P.poly((fr[i], fr[j], bk[j], bk[i]), WOOD, ((0, 0), (0.05, 0), (0.05, 0.05), (0, 0.05)))

# --- head frame: axis out of the plaque, dipped and turned ---
turn, dip = math.radians(cfg.get("turn_deg", 14)), math.radians(cfg.get("dip_deg", 10))
R = Matrix.Rotation(turn, 3, "Z") @ Matrix.Rotation(-dip, 3, "X")
fwd, side, up = R @ Y, R @ X, R @ Z
root = Vector((cx, face - 0.02, cz + 0.02))
reach = hi.y - root.y - 0.01


def loft(sections, centre_of, part, material, close_tip=True):
    """Elliptical sections (s, width, height, dz) along fwd from root; returns the rings."""
    rings = []
    for s, sw, sh, dz in sections:
        c = centre_of(s, dz)
        rings.append([P.vert(c + side * (sw / 2 * math.cos(2 * math.pi * k / around))
                             + up * (sh / 2 * math.sin(2 * math.pi * k / around)), part) for k in range(around)])
    along = 0.0
    for i in range(len(rings) - 1):
        seg = abs(sections[i + 1][0] - sections[i][0])
        for k in range(around):
            k2 = (k + 1) % around
            perim = math.pi * (sections[i][1] + sections[i][2]) / 2
            u0, u1 = k / around * perim / fur_tile, (k + 1) / around * perim / fur_tile
            P.poly((rings[i][k], rings[i][k2], rings[i + 1][k2], rings[i + 1][k]), material,
                   ((u0, along / fur_tile), (u1, along / fur_tile), (u1, (along + seg) / fur_tile), (u0, (along + seg) / fur_tile)))
        along += seg
    if close_tip:
        tip = P.vert(centre_of(sections[-1][0] + 0.012, sections[-1][3]), part)
        for k in range(around):
            P.poly((rings[-1][k], rings[-1][(k + 1) % around], tip), material, ((0, 0), (0.05, 0), (0.025, 0.05)))
    return rings


scale_s = reach / cfg["sections"][-1][0]
centre = lambda s, dz: root + fwd * (s * scale_s) + up * dz
part = P.new_part()
head = loft([(s, sw, sh, dz) for s, sw, sh, dz in cfg["sections"]], centre, part, FUR)
# a fur collar closes the head against the plaque
base = P.vert(root - fwd * 0.01, part)
for k in range(around):
    P.poly((head[0][(k + 1) % around], head[0][k], base), FUR, ((0, 0), (0.05, 0), (0.025, 0.05)))

# lower jaw: hangs open below the muzzle
open_a = math.radians(cfg.get("jaw_open_deg", 22))
jaw = cfg["jaw"]
hinge_s = jaw[0][0]
hinge = centre(hinge_s, -0.03)
J = Matrix.Rotation(-open_a, 3, (R @ X).normalized())
jaw_centre = lambda s, dz: hinge + J @ (fwd * ((s - hinge_s) * scale_s) + up * dz)
loft([(s, jw, jh, -jh / 2) for s, jw, jh in jaw], jaw_centre, P.new_part(), FUR)

# fangs: upper at the front of the muzzle, lower at the front of the jaw
n_f = cfg.get("fangs", 4)
s_tip = cfg["sections"][-1][0]
mz = [s for s in cfg["sections"] if s[0] >= s_tip * 0.8][0]
for k, sx in enumerate((-1, 1)):
    top = centre(s_tip * 0.86, mz[3]) + side * (sx * mz[1] * 0.3) - up * (mz[2] * 0.35)
    P.post(top, top - up * 0.05, 0.016, 0.003, side, fwd, BONE, 0.1)
    low = jaw_centre(jaw[-1][0] * 0.9, -jaw[-1][2] / 2) + side * (sx * jaw[-1][1] * 0.3) + (J @ up) * (jaw[-1][2] * 0.35)
    P.post(low, low + (J @ up) * 0.035, 0.014, 0.003, side, fwd, BONE, 0.1)

# ears: pointed, standing up and a little back, on the skull
ear_h, ear_w = cfg.get("ears", [0.13, 0.07])
skull = [s for s in cfg["sections"] if s[0] >= s_tip * 0.35][0]
for sx in (-1, 1):
    b = centre(skull[0], skull[3]) + side * (sx * skull[1] * 0.28) + up * (skull[2] * 0.42)
    p = P.new_part()
    corners = [P.vert(b + side * (ear_w / 2) , p), P.vert(b + fwd * (ear_w * 0.5), p), P.vert(b - side * (ear_w / 2), p), P.vert(b - fwd * (ear_w * 0.4), p)]
    apex = P.vert(b + up * ear_h - fwd * (ear_h * 0.35) + side * (sx * 0.02), p)
    for i in range(4):
        P.poly((corners[i], corners[(i + 1) % 4], apex), FUR, ((0, 0), (0.1, 0), (0.05, 0.15)))
    P.poly(corners[::-1], FUR, ((0, 0), (0.05, 0), (0.05, 0.05), (0, 0.05)))   # hidden base: open, one ear came out inside out


def dome(centre, normal, radius, depth, material, segs=8):
    """A low dome (eye, nose) on the surface: a ring at the base and a rounded cap toward normal."""
    n = normal.normalized()
    a = n.cross(Z) if abs(n.dot(Z)) < 0.9 else n.cross(X)
    a.normalize()
    b = n.cross(a)
    part = P.new_part()
    rings = []
    for f, rr in ((0.0, 1.0), (0.55, 0.8), (0.9, 0.45)):
        rings.append([P.vert(centre + n * (depth * f) + (a * math.cos(2 * math.pi * k / segs) + b * math.sin(2 * math.pi * k / segs)) * radius * rr, part)
                      for k in range(segs)])
    tip = P.vert(centre + n * depth, part)
    for i in range(2):
        for k in range(segs):
            k2 = (k + 1) % segs
            P.poly((rings[i][k], rings[i][k2], rings[i + 1][k2], rings[i + 1][k]), material, ((0, 0), (0.02, 0), (0.02, 0.02), (0, 0.02)))
    for k in range(segs):
        P.poly((rings[2][k], rings[2][(k + 1) % segs], tip), material, ((0, 0), (0.02, 0), (0.01, 0.02)))
    P.poly(rings[0][::-1], material, [(0, 0)] * segs)


# eyes at the stop, set into the sides of the head, looking forward and out
eye_r = cfg.get("eye", 0.017)
stop = [sec for sec in cfg["sections"] if sec[0] >= s_tip * cfg.get("eye_at", 0.55)][0]
for sx in (-1, 1):
    c = centre(stop[0], stop[3]) + side * (sx * stop[1] * 0.4) + up * (stop[2] * 0.08)
    dome(c, side * (sx * 0.75) + fwd * 0.55 + up * 0.15, eye_r, eye_r * 0.9, EYE)
# a black nose capping the muzzle tip
tip_sec = cfg["sections"][-1]
nose_c = centre(tip_sec[0], tip_sec[3]) + up * (tip_sec[2] * 0.2)
dome(nose_c, fwd * 0.85 + up * 0.35, cfg.get("nose", 0.032), 0.022, NOSE, segs=10)

# ruff: jagged clumps of fur fanning out round the neck at the plaque
n_ruff, length = cfg.get("ruff", [18, 0.09])
s0 = cfg["sections"][1]
for k in range(n_ruff):
    t = 2 * math.pi * (k + rng.uniform(-0.2, 0.2)) / n_ruff
    radial = side * math.cos(t) + up * math.sin(t)
    c = centre(s0[0], s0[3]) + side * (s0[1] / 2 * 0.85 * math.cos(t)) + up * (s0[2] / 2 * 0.85 * math.sin(t))
    tangent = fwd.cross(radial).normalized()
    p = P.new_part()
    wd = 0.07 * rng.uniform(0.8, 1.2)
    a, b2 = P.vert(c + tangent * wd / 2, p), P.vert(c - tangent * wd / 2, p)
    back = P.vert(c - fwd * 0.05, p)
    tipv = P.vert(c + radial * length * rng.uniform(0.7, 1.2) + fwd * 0.03, p)
    P.poly((a, b2, tipv), FUR, ((0, 0), (0.2, 0), (0.1, 0.3)))
    P.poly((b2, back, tipv), FUR, ((0, 0), (0.2, 0), (0.1, 0.3)))
    P.poly((back, a, tipv), FUR, ((0, 0), (0.2, 0), (0.1, 0.3)))

P.finish(goal, out, lo, hi, {})
