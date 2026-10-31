"""Build a town fireplace (NW_CITY_FIREPLACE_NORMAL_01, _LARGE_01, _LARGE_02, _LARGE_021).

blender -b --factory-startup -P fireplace.py -- fireplace.json OUT_DIR   (run from the repository root)
config (glTF metres, the room is at -z, the wall at "wall" z):
  {"id": ..., "target": "data/content/mesh3d-hearth.json", "seed": n, "floor": y, "wall": z,
   "hearth": [x_half, z_front, top_y]           the floor slab in front of and inside the fire opening,
   "block": [x_half, z_front, top_y]            the lower block round the opening,
   "shelf": [x_half, z_front, thick]            the mantle slab on the block,
   "opening": [x_half, top_y, chamfer, depth]   the fire opening (chamfer: the cut of its upper corners),
   "hood": [[y, x_half, z_front], ...]          the chimney breast from the shelf top (first row) to the ceiling,
   "stones": {"course": [h0, h1], "length": [l0, l1], "proud": d, "gap": g, "regular": bool},
   "logs": n}
Asset city-hearth (intent.md, wave 2; form from the original's mesh: every plane of the original read
from its underlay). The original paints its masonry on a few flat planes; here the same outline is a
dark mortar core with every stone a separate bevelled block laid in courses on the faces the player
sees: rubble of uneven stones for the poor houses, squared courses for the lower town, smooth ashlar
for the upper quarter ("regular"). The opening is a sooted box with a few logs on embers.
"""
import math
import sys
from pathlib import Path

import numpy as np
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parts import Parts, args, load_goal  # noqa: E402
from hearth_parts import dome, stick  # noqa: E402

cfg, out = args()
goal, lo, hi = load_goal(cfg)
rng = np.random.default_rng(cfg.get("seed", 19))
keys = [z["key"] for z in goal["zones"]]
STONE = keys.index("OWN_HEARTH_ASHLAR" if "OWN_HEARTH_ASHLAR" in keys else "OWN_HEARTH_MASONRY")
SOOT, LOG, EMBER = (keys.index(k) for k in ("OWN_HEARTH_SOOT", "OWN_HEARTH_LOG", "OWN_HEARTH_EMBER"))
CARVE = keys.index("OWN_HEARTH_CARVING") if "OWN_HEARTH_CARVING" in keys else STONE
P = Parts(rng, 0.1)
st = cfg["stones"]
floor, wall = cfg["floor"], cfg["wall"]


def b(x, y, z):
    return Vector((x, -z, y))


def box(x0, x1, y0, y1, z0, z1, mat):
    """A closed axis-aligned box in glTF metres."""
    c = b((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2)
    P.board(c, Vector((1, 0, 0)), Vector((0, 0, 1)), Vector((0, 1, 0)), x1 - x0, y1 - y0, z1 - z0, mat)


def shell(rows, mat):
    """A closed solid between rows [(y, x_half, z_front)] back to the wall: front, sides, top and bottom."""
    part = P.new_part()
    ring = lambda y, xh, zf: [P.vert(b(-xh, y, zf), part), P.vert(b(xh, y, zf), part), P.vert(b(xh, y, wall), part), P.vert(b(-xh, y, wall), part)]
    rings = [ring(*r) for r in rows]
    for k in range(len(rings) - 1):
        for i in range(4):
            j = (i + 1) % 4
            q = [rings[k][i], rings[k][j], rings[k + 1][j], rings[k + 1][i]]
            P.poly(q, mat, [(v.co.x, v.co.z) if i % 2 == 0 else (v.co.y, v.co.z) for v in q])
    P.poly(rings[0][::-1], mat, [(v.co.x, v.co.y) for v in rings[0][::-1]])
    P.poly(rings[-1], mat, [(v.co.x, v.co.y) for v in rings[-1]])


def course_face(p00, p10, p01, p11, normal, mat, rough=True):
    """Lay stones in courses over the planar quad p00 (bottom left) .. p11 (top right), proud of it along
    normal. Each stone is a closed box; rubble stones vary in height, length, depth and tilt."""
    height = (p01 - p00).length
    h0, h1 = st["course"]
    l0, l1 = st["length"]
    gap = st["gap"]
    y = 0.0
    row = 0
    while y < height - 0.04:
        h = min(rng.uniform(h0, h1) if rough else h1, height - y)
        t0, t1 = y / height, (y + h) / height
        left = p00.lerp(p01, (t0 + t1) / 2)
        right = p10.lerp(p11, (t0 + t1) / 2)
        width = (right - left).length
        along = (right - left).normalized()
        up = (p01 - p00).normalized()
        x = -rng.uniform(0, l1 * 0.6) if rough else -(l1 / 2) * (row % 2)
        while x < width - 0.02:
            ln = rng.uniform(l0, l1) if rough else l1
            xa, xb = max(x, 0.0), min(x + ln, width)
            if xb - xa > 0.06:
                c = left + along * ((xa + xb) / 2) + up * 0.0 + (p00.lerp(p01, t0 + (t1 - t0) / 2) - p00.lerp(p01, (t0 + t1) / 2))
                c = left + along * ((xa + xb) / 2)
                proud = st["proud"] * (rng.uniform(0.4, 1.3) if rough else 1.0)
                tilt = rng.uniform(-0.04, 0.04) if rough else 0.0
                ax = (along + up * tilt).normalized()
                P.board(c + normal * (proud / 2), ax, up, normal, xb - xa - gap, h - gap, proud + 0.02, mat)
            x += ln
        y += h
        row += 1


def carved_band(p0, p1, y0, y1, normal, depth):
    """A band of simple carved leaves: a row of rounded lozenges proud of the face."""
    width = (p1 - p0).length
    along = (p1 - p0).normalized()
    n = max(4, int(width / 0.22))
    for k in range(n):
        c = p0 + along * (width * (k + 0.5) / n)
        for s, (dy, sc) in enumerate(((0.0, 1.0), (0.0, 0.55))):
            P.board(c + normal * (depth * (s + 1) / 2) + Vector((0, 0, (y0 + y1) / 2 - c.z)), along, Vector((0, 0, 1)), normal,
                    width / n * 0.8 * sc, (y1 - y0) * 0.8 * sc, depth, CARVE)


hx, hzf, htop = cfg["hearth"]
bx, bzf, btop = cfg["block"]
sx, szf, sth = cfg["shelf"]
ox, otop, och, odepth = cfg["opening"]
# mortar core: the hearth slab, the block, the shelf, the hood
box(-hx, hx, floor, htop, hzf, wall, SOOT)
# the block round a real firebox: two piers and a lintel; the firebox has sooted sides, back and roof
cz = bzf + 0.02
box(-bx + 0.02, -ox - 0.02, htop, btop, cz, wall, STONE)
box(ox + 0.02, bx - 0.02, htop, btop, cz, wall, STONE)
box(-ox - 0.02, ox + 0.02, otop - 0.02, btop, cz, wall, STONE)
back = bzf + odepth
box(-ox, ox, htop, otop, back, back + 0.04, SOOT)
for s_ in (-1, 1):
    box(min(s_ * ox, s_ * (ox + 0.04)), max(s_ * ox, s_ * (ox + 0.04)), htop, otop, bzf, back, SOOT)
box(-ox, ox, otop - 0.04, otop, bzf, back, SOOT)
box(-sx, sx, btop, btop + sth, szf, wall, STONE)
hood = cfg["hood"]
shell([(y, x - 0.02, z + 0.02) for y, x, z in hood], STONE)
# stones of the block's front, left and right of the opening and over it
front = Vector((0, 1, 0))  # glTF -z is Blender +y
fz = bzf - 0.005
course_face(b(-bx, htop, fz), b(-ox, htop, fz), b(-bx, btop, fz), b(-ox, btop, fz), front, STONE, not st["regular"])
course_face(b(ox, htop, fz), b(bx, htop, fz), b(ox, btop, fz), b(bx, btop, fz), front, STONE, not st["regular"])
course_face(b(-ox, otop, fz), b(ox, otop, fz), b(-ox, btop, fz), b(ox, btop, fz), front, STONE, not st["regular"])
if och > 0:  # the cut corners of the opening: two stones set on the slant
    for s in (-1, 1):
        a = b(s * (ox - och), otop, fz)
        c = b(s * ox, otop - och, fz)
        P.board((a + c) / 2 + front * 0.03, (c - a).normalized(), Vector((0, 1, 0)).cross((c - a).normalized()).normalized(),
                front, (c - a).length + 0.06, 0.12, 0.06, STONE)
# side faces of the block (seen from the room at an angle)
for s in (-1, 1):
    side = Vector((s, 0, 0))
    a, c = b(s * bx, htop, bzf), b(s * bx, htop, wall - 0.02)
    course_face(a, c, b(s * bx, btop, bzf), b(s * bx, btop, wall - 0.02), side, STONE, not st["regular"]) if s < 0 else \
        course_face(c, a, b(s * bx, btop, wall - 0.02), b(s * bx, btop, bzf), side, STONE, not st["regular"])
# the hood's front, course by course, on each sloping segment
rows = [tuple(r) for r in hood]  # the first row stands on the shelf
for (y0, x0, z0), (y1, x1, z1) in zip(rows, rows[1:]):
    p00, p10, p01, p11 = b(-x0, y0, z0 - 0.005), b(x0, y0, z0 - 0.005), b(-x1, y1, z1 - 0.005), b(x1, y1, z1 - 0.005)
    normal = (p10 - p00).cross(p01 - p00).normalized()
    if normal.y < 0:
        normal = -normal
    course_face(p00, p10, p01, p11, normal, STONE, not st["regular"])
if cfg.get("carving"):
    y0, y1 = cfg["carving"]
    (ya, xa, za) = rows[0]
    carved_band(b(-xa * 0.95, (y0 + y1) / 2, za - 0.02), b(xa * 0.95, (y0 + y1) / 2, za - 0.02), y0, y1, front, 0.025)
# the fire: an ember bed and a few logs on it
fc = b(0, htop, bzf + odepth / 2)
dome(P, rng, fc, min(ox * 0.8, 0.45), 0.05, EMBER, sides=14, rings=2)
for k in range(cfg.get("logs", 3)):
    a = rng.uniform(-0.5, 0.5)
    half = ox * 0.6
    stick(P, rng, fc + Vector((-half * math.cos(a), -half * math.sin(a) * 0.4, 0.06 + 0.05 * k)),
          fc + Vector((half * math.cos(a), half * math.sin(a) * 0.4, 0.06 + 0.04 * k)), 0.05, LOG, burnt=(EMBER, 0.3), sides=6)
P.finish(goal, out, lo, hi, {"regular": st["regular"]})
