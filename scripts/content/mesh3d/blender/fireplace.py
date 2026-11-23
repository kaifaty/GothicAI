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
    """A closed solid between rows [(y, x_half, z_front)] back to the wall: front, sides, top and bottom.
    Wound by hand, each face away from the solid's centre: the top row lies on the wall (no depth), so the
    solid has a degenerate top, and the recalculation turned the hood inside out; the engine then culled
    its sides and the hood read as a flat slab (city-hearth, the user, 2026-10-31)."""
    part = P.new_part(wound=True)
    ring = lambda y, xh, zf: [P.vert(b(-xh, y, zf), part), P.vert(b(xh, y, zf), part), P.vert(b(xh, y, wall), part), P.vert(b(-xh, y, wall), part)]
    rings = [ring(*r) for r in rows]
    centre = sum((v.co for r in rings for v in r), Vector()) / (4 * len(rings))

    def face(q, uv):
        a, c = q[1].co - q[0].co, q[2].co - q[0].co
        n = a.cross(c)
        if n.length < 1e-9:  # a degenerate first triangle: take the other diagonal
            n = (q[2].co - q[0].co).cross(q[3].co - q[0].co)
        mid = sum((v.co for v in q), Vector()) / len(q)
        if n.dot(mid - centre) < 0:
            q, uv = q[::-1], uv[::-1]
        P.poly(q, mat, uv)

    for k in range(len(rings) - 1):
        for i in range(4):
            j = (i + 1) % 4
            q = [rings[k][i], rings[k][j], rings[k + 1][j], rings[k + 1][i]]
            if (q[0].co - q[1].co).length < 1e-6 and (q[2].co - q[3].co).length < 1e-6:
                continue
            face(q, [(v.co.x, v.co.z) if i % 2 == 0 else (v.co.y, v.co.z) for v in q])
    for r in (rings[0], rings[-1]):
        if (r[0].co - r[3].co).length > 1e-6:  # the top ring on the wall has no depth: no cap
            face(list(r), [(v.co.x, v.co.y) for v in r])


def course_face(p00, p10, p01, p11, normal, mat, rough=True, relief=1.0):
    """Lay stones in courses over the planar quad p00 (bottom left) .. p11 (top right), proud of it along
    normal. Each stone is a closed box; rubble stones vary in height, length, depth and tilt. "relief"
    scales how far they stand proud: on the hood the fire below lit their undersides as bright lines."""
    height = (p01 - p00).length
    h0, h1 = st["course"]
    l0, l1 = st["length"]
    gap = st["gap"]
    y = 0.0
    row = 0
    while y < height - 0.04:
        h = min(rng.uniform(h0, h1) if rough else h1, height - y)
        t0, t1 = y / height, (y + h) / height
        # the course keeps within the narrower end of its band, so no stone hangs over a sloping edge
        left = p00.lerp(p01, (t0 + t1) / 2)
        right = p10.lerp(p11, (t0 + t1) / 2)
        along = (right - left).normalized()
        w0, w1 = (p10.lerp(p11, t0) - p00.lerp(p01, t0)).length, (p10.lerp(p11, t1) - p00.lerp(p01, t1)).length
        width = min(w0, w1)
        left = left + along * (((right - left).length - width) / 2)
        up = normal.cross(along).normalized()  # across the course; the face's slanted side edge sheared the stones
        if up.dot(p01 - p00) < 0:
            up = -up
        square = up.dot((p01 - p00).normalized())  # the course's height across it, not along the slanted edge
        x = -rng.uniform(0, l1 * 0.6) if rough else -(l1 / 2) * (row % 2)
        while x < width - 0.02:
            ln = rng.uniform(l0, l1) if rough else l1
            xa, xb = max(x, 0.0), min(x + ln, width)
            if xb - xa > 0.06:
                c = left + along * ((xa + xb) / 2)
                proud = st["proud"] * relief * (rng.uniform(0.4, 1.3) if rough else 1.0)
                tilt = rng.uniform(-0.04, 0.04) if rough else 0.0
                ax = (along + up * tilt).normalized()
                P.board(c + normal * (proud / 2), ax, up, normal, xb - xa - gap, h * square - gap, proud + 0.02, mat)
            x += ln
        y += h
        row += 1


def carved_band(p0, p1, up, normal, h, depth):
    """A carved band laid on a face: a solid panel standing "depth" proud, two mouldings framing it and a
    run of leaves set alternately on the slant, like a vine; p0..p1 its centre line, "up" across it in the
    face. It lies on the hood and wraps its sides like the original's (a thin openwork strip hung in
    front of the hood read as a loose fret, the user, 2026-10-31)."""
    width = (p1 - p0).length
    along = (p1 - p0).normalized()
    mid = (p0 + p1) / 2
    P.board(mid + normal * (depth / 2), along, up, normal, width, h, depth, CARVE)
    for dy in (-h / 2 + 0.025, h / 2 - 0.025):  # the two mouldings framing it
        P.board(mid + up * dy + normal * (depth + 0.012), along, up, normal, width + 0.01, 0.045, 0.024, CARVE)
    n = max(3, int(width / 0.16))
    for k in range(n):
        c = p0 + along * (width * (k + 0.5) / n) + normal * (depth + 0.008)
        tilt = 0.7 if k % 2 == 0 else -0.7
        ax = (along * math.cos(tilt) + up * math.sin(tilt)).normalized()
        P.board(c, ax, normal.cross(ax).normalized(), normal, width / n * 1.1, h * 0.3, 0.016, CARVE)
        P.board(c + up * (h * 0.17 * (1 if k % 2 else -1)), along, up, normal, 0.035, 0.035, 0.03, CARVE)


hx, hzf, htop = cfg["hearth"]
bx, bzf, btop = cfg["block"]
sx, szf, sth = cfg["shelf"]
ox, otop, och, odepth = cfg["opening"]
# mortar core: the hearth slab, the block, the shelf, the hood
box(-hx, hx, floor, htop, hzf, wall, CARVE)  # the hearth slab: dressed stone, lighter than the masonry
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
box(-ox, ox, htop, htop + 0.006, bzf + 0.01, back, SOOT)  # the sooted floor inside the firebox, over the light slab
box(-sx, sx, btop, btop + sth, szf, wall, CARVE)  # the mantle shelf: one dressed slab
hood = cfg["hood"]
shell([(y, x - 0.02, z + 0.02) for y, x, z in hood], STONE)
# stones of the block's front, left and right of the opening and over it
front = Vector((0, 1, 0))  # glTF -z is Blender +y
fz = bzf - 0.005
course_face(b(-bx, htop, fz), b(-ox, htop, fz), b(-bx, btop, fz), b(-ox, btop, fz), front, STONE, not st["regular"])
course_face(b(ox, htop, fz), b(bx, htop, fz), b(ox, btop, fz), b(bx, btop, fz), front, STONE, not st["regular"])
course_face(b(-ox, otop, fz), b(ox, otop, fz), b(-ox, btop, fz), b(ox, btop, fz), front, STONE, not st["regular"])
if och > 0:  # the cut corners of the opening: a three-sided stone fills each upper corner through the firebox
    for s in (-1, 1):
        part = P.new_part()
        tri = [(s * ox, otop), (s * (ox - och), otop), (s * ox, otop - och)]
        fr = [P.vert(b(x, y, fz - st["proud"]), part) for x, y in tri]
        bk = [P.vert(b(x, y, back), part) for x, y in tri]
        P.poly(fr, CARVE if st["regular"] else STONE, [(x, y) for x, y in tri])
        P.poly(bk[::-1], SOOT, [(x, y) for x, y in tri[::-1]])
        for i in range(3):
            j = (i + 1) % 3
            P.poly([fr[i], bk[i], bk[j], fr[j]], SOOT if i == 1 else STONE, [(0, 0), (odepth, 0), (odepth, 0.1), (0, 0.1)])
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
    course_face(p00, p10, p01, p11, normal, STONE, not st["regular"], relief=0.4)
# quoins along every sloping edge of the hood: straight stones over the stepped ends of the courses
for (y0, x0, z0), (y1, x1, z1) in zip(rows, rows[1:]):
    if abs(x1 - x0) < 0.02:
        continue
    for s_ in (-1, 1):
        a_, c_ = b(s_ * x0, y0, z0 - 0.005), b(s_ * x1, y1, z1 - 0.005)
        p00, p10, p01 = b(-x0, y0, z0 - 0.005), b(x0, y0, z0 - 0.005), b(-x1, y1, z1 - 0.005)
        n_ = (p10 - p00).cross(p01 - p00).normalized()
        if n_.y < 0:
            n_ = -n_
        edge = (c_ - a_).normalized()
        inward = n_.cross(edge).normalized()
        if inward.x * s_ > 0:
            inward = -inward
        P.board((a_ + c_) / 2 + inward * 0.07 + n_ * (st["proud"] + 0.01), edge, inward, n_, (c_ - a_).length, 0.16, st["proud"] * 2 + 0.03, STONE)
if cfg.get("carving"):  # on the hood's lowest slope, across its front and round both sides
    y0, y1 = cfg["carving"]
    (ya, xa, za), (yb, xb, zb) = rows[0], rows[1]
    yc = (y0 + y1) / 2
    t = (yc - ya) / (yb - ya)
    xh, zc = xa + (xb - xa) * t, za + (zb - za) * t
    depth = st["proud"] * 2 + 0.06  # over the quoins
    p00, p10, p01 = b(-xa, ya, za), b(xa, ya, za), b(-xb, yb, zb)
    n_front = (p10 - p00).cross(p01 - p00).normalized()
    if n_front.y < 0:
        n_front = -n_front
    along = Vector((1, 0, 0))
    up = n_front.cross(along).normalized()
    if up.z < 0:
        up = -up
    h = (y1 - y0) / up.z
    carved_band(b(-xh - depth, yc, zc - 0.005), b(xh + depth, yc, zc - 0.005), up, n_front, h, depth)
    for s_ in (-1, 1):  # the sides, from the front corner back to the wall
        a_, c_, w_ = b(s_ * xa, ya, za), b(s_ * xb, yb, zb), b(s_ * xa, ya, wall)
        n_side = (c_ - a_).cross(w_ - a_).normalized()
        if n_side.x * s_ < 0:
            n_side = -n_side
        along_s = (b(s_ * xh, yc, wall - 0.01) - b(s_ * xh, yc, zc)).normalized()
        up_s = n_side.cross(along_s).normalized()
        if up_s.z < 0:
            up_s = -up_s
        carved_band(b(s_ * xh, yc, zc - 0.005 + depth * 0.5), b(s_ * xh, yc, wall - 0.01), up_s, n_side, (y1 - y0) / up_s.z, depth)
# the fire: an ember bed and a few logs on it
fc = b(0, htop, bzf + odepth / 2)
dome(P, rng, fc, min(ox * 0.8, 0.45), 0.05, EMBER, sides=14, rings=2)
for k in range(cfg.get("logs", 3)):
    a = rng.uniform(-0.5, 0.5)
    half = ox * 0.6
    stick(P, rng, fc + Vector((-half * math.cos(a), -half * math.sin(a) * 0.4, 0.06 + 0.05 * k)),
          fc + Vector((half * math.cos(a), half * math.sin(a) * 0.4, 0.06 + 0.04 * k)), 0.05, LOG, burnt=(EMBER, 0.3), sides=6)
P.finish(goal, out, lo, hi, {"regular": st["regular"]})
