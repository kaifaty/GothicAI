"""Build the kitchen stove with its hood (STOVE_NW_CITY_01, a mob with one attachment).

blender -b --factory-startup -P stove.py -- stove.json OUT_DIR   (run from the repository root)
config: {"id": "hearth-stove", "target": "data/content/mesh3d-hearth.json", "seed": 17,
         "box": [half_width, half_depth, bottom, top], "legs": [size, height], "bevel": m,
         "plate": [overhang, thickness], "edge": [width, proud], "grate": [cx, cz, radius, bars],
         "door": [cy, radius, thick, holes], "shelf": [x_half, y0, y1, z0, z1],
         "hood": {"y": [y0, y1], "bottom": [x_half, z_front], "top": [x_half, z_front], "back": z, "sheet": t, "seams": n}}
Asset city-hearth (intent.md, wave 2; form from the original's mesh, glTF metres, the front is -z: the
cook stands at ZS_POS0, z -0.78). A cast-iron box on four square feet: bevelled plates framed by corner
angle irons, a top plate overhanging a little with a round grate over embers, a round fire door with
vent holes glowing through and a ring handle. Behind it on the wall a plank shelf on two brackets, and
above it a hammered copper hood, a truncated pyramid open at the bottom with a folded rim and seams.
"""
import math
import sys
from pathlib import Path

import numpy as np
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parts import X, Y, Z, Parts, args, load_goal  # noqa: E402
from hearth_parts import bar, dome, stick, turned  # noqa: E402

cfg, out = args()
goal, lo, hi = load_goal(cfg)
rng = np.random.default_rng(cfg.get("seed", 17))
keys = [z["key"] for z in goal["zones"]]
CAST, RIVET, COPPER, EMBER, WOOD = (keys.index(k) for k in
                                   ("OWN_HEARTH_CAST", "OWN_HEARTH_RIVET", "OWN_HEARTH_COPPER", "OWN_HEARTH_EMBER", "OWN_HEARTH_WOOD"))
P = Parts(rng, 0.1)


def b(x, y, z):
    """glTF metres to Blender axes."""
    return Vector((x, -z, y))


hx, hz, y0, y1 = cfg["box"]
# the box: one board per face, so every plate is its own part with a slight bevel look from the frame
P.board(b(0, (y0 + y1) / 2, 0), X, Z, Y, 2 * hx - 0.02, y1 - y0, 2 * hz - 0.02, CAST)
ls, lh = cfg["legs"]
for sx in (-1, 1):
    for sz in (-1, 1):
        P.post(b(sx * (hx - ls / 2 - 0.02), y0, sz * (hz - ls / 2 - 0.02)), b(sx * (hx - ls / 2 - 0.02), 0, sz * (hz - ls / 2 - 0.02)),
               ls, ls * 0.85, X, Y, CAST, 0.3)
ew, ep = cfg["edge"]
for sx in (-1, 1):  # corner angle irons up the four vertical edges and round the top and bottom
    for sz in (-1, 1):
        c = b(sx * (hx - ew / 2 + ep), (y0 + y1) / 2, sz * (hz - ew / 2 + ep))
        P.board(c, Z, X, Y, y1 - y0, ew, ew, RIVET)
for yy in (y0 + ew / 2, y1 - ew / 2):
    for sz in (-1, 1):
        P.board(b(0, yy, sz * (hz - ew / 2 + ep)), X, Z, Y, 2 * hx + 2 * ep, ew, ew, RIVET)
    for sx in (-1, 1):
        P.board(b(sx * (hx - ew / 2 + ep), yy, 0), Y, Z, X, 2 * hz + 2 * ep, ew, ew, RIVET)
ov, pt = cfg["plate"]
gx, gz, gr, nbars = cfg["grate"]
# top plate with a round hole for the grate: a ring of quads between the hole and the square outline
part = P.new_part()
n = 32
top, bot = y1 + pt, y1
outline = []
for i in range(n):
    t = 2 * math.pi * i / n
    dx, dz = math.cos(t), math.sin(t)
    k = 1 / max(abs(dx) / (hx + ov), abs(dz) / (hz + ov))
    outline.append((dx * k, dz * k))
hole = [(gx + gr * math.cos(2 * math.pi * i / n), gz + gr * math.sin(2 * math.pi * i / n)) for i in range(n)]
rings = {}
for name, pts, yy in (("ot", outline, top), ("ob", outline, bot), ("ht", hole, top), ("hb", hole, bot)):
    rings[name] = [P.vert(b(x, yy, z), part) for x, z in pts]
for i in range(n):
    j = (i + 1) % n
    for a, c in (("ht", "ot"), ("ob", "hb"), ("ot", "ob"), ("hb", "ht")):
        q = [rings[a][i], rings[a][j], rings[c][j], rings[c][i]]
        P.poly(q, CAST, [(v.co.x, v.co.y) for v in q])
# embers under the grate and the bars over them
dome(P, rng, b(gx, y1 + 0.002, gz), gr - 0.01, 0.018, EMBER, sides=16, rings=2)  # on the box top, under the grate
grate_ring = [b(gx + gr * math.cos(2 * math.pi * k / 24), top + 0.015, gz + gr * math.sin(2 * math.pi * k / 24)) for k in range(25)]
for p0, p1 in zip(grate_ring, grate_ring[1:]):  # an open cast ring round the grate, no floor over the embers
    bar(P, p0, p1, 0.03, 0.03, RIVET)
for k in range(nbars):
    zz = gz - gr * 0.85 + (2 * gr * 0.85) * k / (nbars - 1)
    w = math.sqrt(max(gr * gr - (zz - gz) ** 2, 0)) * 2
    P.board(b(gx, top + 0.02, zz), X, Z, Y, w, 0.016, 0.02, RIVET)
# forged scrolls in the four corners of the front and both side faces: a curl of short bars
def scroll(corner, ax, ay, normal, size):
    """A cast volute from the corner along ax, winding in toward ay; ax, ay unit vectors in the face. A low
    relief of the plate, not a bent wire: the round bars standing off the face read as hooks."""
    pts = []
    for k in range(9):
        t = k / 8
        ang = math.pi * 1.9 * t
        r = size * 0.45 * (1 - 0.75 * t)
        cx_ = size * 0.55
        pts.append(corner + ax * (cx_ - r * math.cos(ang) + (1 - t) * size * 0.25) + ay * (size * 0.4 + r * math.sin(ang)))
    for p0, p1 in zip(pts, pts[1:]):  # flat in the plate: bar() keeps its width horizontal and stood these on edge
        d = p1 - p0
        P.board((p0 + p1) / 2 + normal * 0.003, d.normalized(), normal.cross(d.normalized()).normalized(), normal,
                d.length + 0.008, 0.016, 0.006, RIVET)


sc = cfg.get("scroll", 0.12)
faces = [(Vector((0, 1, 0)), lambda u, v: b(u, v, -hz - 0.002), (1, 0)),  # front (glTF -z)
         (Vector((1, 0, 0)), lambda u, v: b(hx + 0.002, v, u), (0, 1)),
         (Vector((-1, 0, 0)), lambda u, v: b(-hx - 0.002, v, u), (0, 1))]
for normal, at, _ in faces:
    for su in (-1, 1):
        for sv in (-1, 1):
            u0, v0 = su * (hx - ew - 0.01), (y0 + ew + 0.01) if sv < 0 else (y1 - ew - 0.01)
            corner = at(u0, v0)
            ax = (at(u0 - su * 0.1, v0) - corner).normalized()
            ay = (at(u0, v0 - sv * 0.1) - corner).normalized()
            scroll(corner, ax, ay, normal, sc)
# the fire door on the front face (glTF -z) with vent holes and a ring handle
dy, dr, dt, holes = cfg["door"]
front = -hz
door = b(0, dy, front - dt / 2)
part = P.new_part()
disc = []
for i in range(24):
    t = 2 * math.pi * i / 24
    disc.append((dr * math.cos(t), dr * math.sin(t)))
fr = [P.vert(b(x, dy + y, front - dt), part) for x, y in disc]
bk = [P.vert(b(x, dy + y, front + 0.002), part) for x, y in disc]
cf, cb = P.vert(b(0, dy, front - dt), part), P.vert(b(0, dy, front + 0.002), part)
for i in range(24):
    j = (i + 1) % 24
    P.poly([cf, fr[i], fr[j]], CAST, [(0, 0), (disc[i][0], disc[i][1]), (disc[j][0], disc[j][1])])
    P.poly([cb, bk[j], bk[i]], CAST, [(0, 0), (disc[j][0], disc[j][1]), (disc[i][0], disc[i][1])])
    P.poly([fr[i], bk[i], bk[j], fr[j]], CAST, [(0, 0), (0, dt), (0.02, dt), (0.02, 0)])
ring_pts = [b(dr * 1.08 * math.cos(2 * math.pi * k / 20), dy + dr * 1.08 * math.sin(2 * math.pi * k / 20), front - 0.006) for k in range(21)]
for p0, p1 in zip(ring_pts, ring_pts[1:]):  # a raised cast ring round the door
    bar(P, p0, p1, 0.022, 0.014, RIVET)
for k in range(holes):  # glowing vent holes in a ring on the door
    t = 2 * math.pi * k / holes
    c = b(dr * 0.6 * math.cos(t), dy + dr * 0.6 * math.sin(t), front - dt - 0.002)
    P.board(c, X, Z, Y, 0.028, 0.028, 0.004, EMBER)
P.board(b(0, dy, front - dt - 0.002), X, Z, Y, 0.05, 0.05, 0.004, EMBER)
for s in (-1, 1):  # two hinge straps on the left, from the frame onto the door
    bar(P, b(-dr - 0.06, dy + s * dr * 0.55, front - dt - 0.004), b(-dr * 0.45, dy + s * dr * 0.55, front - dt - 0.004), 0.035, 0.01, RIVET)
# an upright grip on the right on two short standoffs: a crossbar at the hinges' height read as one bar
# through the whole door with them (the user, 2026-10-31)
gx_ = dr * 0.78
for s in (-1, 1):
    bar(P, b(gx_, dy + s * dr * 0.22, front - dt), b(gx_, dy + s * dr * 0.22, front - dt - 0.035), 0.016, 0.016, RIVET)
stick(P, rng, b(gx_, dy - dr * 0.26, front - dt - 0.04), b(gx_, dy + dr * 0.26, front - dt - 0.04), 0.009, RIVET, sides=6)
# shelf on two brackets against the wall
sx_, s0, s1, sz0, sz1 = cfg["shelf"]
P.wood(b(0, s1 - 0.02, (sz0 + sz1) / 2), X, Y, Z, 2 * sx_, sz1 - sz0, 0.04, WOOD, 0.4)
for s in (-1, 1):
    bar(P, b(s * sx_ * 0.75, s0 - 0.12, sz1 - 0.02), b(s * sx_ * 0.75, s1 - 0.04, sz0 + 0.04), 0.04, 0.04, WOOD)
# the copper hood: a sheet shell open at the bottom, a folded rim, vertical seams
h = cfg["hood"]
hy0, hy1 = h["y"]
bx_, bzf = h["bottom"]
tx_, tzf = h["top"]
back = h["back"]
sheet = h["sheet"]
part = P.new_part()
def ring(yy, xh, zf, inset):
    return [b(-xh + inset, yy, zf + inset), b(xh - inset, yy, zf + inset), b(xh - inset, yy, back), b(-xh + inset, yy, back)]
outer = [ring(hy0, bx_, bzf, 0), ring(hy1, tx_, tzf, 0)]
inner = [ring(hy0, bx_, bzf, sheet), ring(hy1, tx_, tzf, sheet)]
vo = [[P.vert(p, part) for p in r] for r in outer]
vi = [[P.vert(p, part) for p in r] for r in inner]
EDGES = ((0, 1), (1, 2), (3, 0))  # front and two sides of [front-left, front-right, back-right, back-left]; the back is the wall
for k, j in EDGES:
    P.poly([vo[0][k], vo[0][j], vo[1][j], vo[1][k]], COPPER, [(0, 0), (1, 0), (1, 0.7), (0, 0.7)])
    P.poly([vi[0][j], vi[0][k], vi[1][k], vi[1][j]], COPPER, [(0, 0), (1, 0), (1, 0.7), (0, 0.7)])
    P.poly([vi[0][k], vi[0][j], vo[0][j], vo[0][k]], COPPER, [(0, 0), (1, 0), (1, 0.01), (0, 0.01)])
    P.poly([vo[1][k], vo[1][j], vi[1][j], vi[1][k]], COPPER, [(0, 0), (1, 0), (1, 0.01), (0, 0.01)])
rim = 0.035
for k, j in EDGES:  # the folded rim round the lower edge
    a, c = outer[0][k], outer[0][j]
    d = (c - a)
    outward = d.cross(Vector((0, 0, 1))).normalized() * -1
    P.board((a + c) / 2 + Vector((0, 0, rim / 2)) + outward * 0.006, d.normalized(), Vector((0, 0, 1)), outward, d.length + 0.02, rim, 0.012, COPPER)
for k in range(h["seams"]):
    t = (k + 1) / (h["seams"] + 1)
    a = outer[0][0].lerp(outer[0][1], t)
    c = outer[1][0].lerp(outer[1][1], t)
    nrm = (outer[0][1] - outer[0][0]).cross(c - a).normalized()
    bar(P, a + nrm * 0.004, c + nrm * 0.004, 0.012, 0.006, RIVET)
P.finish(goal, out, lo, hi, {"door": cfg["door"]})
