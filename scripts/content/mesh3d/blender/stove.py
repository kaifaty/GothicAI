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
dome(P, rng, b(gx, y1 - 0.06, gz), gr - 0.01, 0.04, EMBER, sides=16, rings=2)
turned(P, b(gx, top, gz), [(0.0, gr + 0.01), (0.03, gr + 0.012), (0.035, gr - 0.012)], [(0.035, gr - 0.02), (0.0, gr - 0.02)],
       24, RIVET)
for k in range(nbars):
    zz = gz - gr * 0.85 + (2 * gr * 0.85) * k / (nbars - 1)
    w = math.sqrt(max(gr * gr - (zz - gz) ** 2, 0)) * 2
    P.board(b(gx, top + 0.02, zz), X, Z, Y, w, 0.016, 0.02, RIVET)
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
for k in range(holes):  # glowing vent holes in a ring on the door
    t = 2 * math.pi * k / holes
    c = b(dr * 0.6 * math.cos(t), dy + dr * 0.6 * math.sin(t), front - dt - 0.002)
    P.board(c, X, Z, Y, 0.028, 0.028, 0.004, EMBER)
P.board(b(0, dy, front - dt - 0.002), X, Z, Y, 0.05, 0.05, 0.004, EMBER)
stick(P, rng, b(-dr * 0.35, dy - 0.02, front - dt - 0.02), b(dr * 0.35, dy - 0.02, front - dt - 0.02), 0.008, RIVET, sides=4)
for s in (-1, 1):  # hinges on the left, the latch bar on the right
    bar(P, b(-dr - 0.06, dy + s * dr * 0.5, front - dt / 2), b(-dr * 0.4, dy + s * dr * 0.5, front - dt / 2), 0.04, 0.012, RIVET)
bar(P, b(dr * 0.7, dy, front - dt - 0.006), b(dr + 0.08, dy, front - dt - 0.006), 0.035, 0.012, RIVET)
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
