"""Build the poor household's low cupboard by parts from an image_gen concept and a numeric target.

blender -b --factory-startup -P cupboard.py -- cupboard.json OUT_DIR   (run from the repository root)
config: {"id": "cupboard", "target": "data/content/mesh3d-market.json", "seed": 1, "top": 0.05,
         "top_planks": 3, "overhang": 0.04, "plinth": 0.1, "plinth_back": 0.01, "board": 0.025,
         "side_boards": 5, "back_boards": 5, "stile": 0.075, "mullion": 0.085, "rails": [0.075, 0.06],
         "frame_depth": 0.04, "door": [0.07, 0.08, 0.03], "door_planks": 3, "door_gap": 0.006,
         "hinge": [0.42, 0.045, 0.005], "ring": [0.032, 0.007], "gap": 0.005, "width_jitter": 0.15,
         "tilt_deg": 0.6, "nail": 0.004, "nail_radius": 0.008, "stave_tile": 0.38, "planks_per_tile": 4}
The front faces +Y in Blender (glTF -Z): the original's top and plinth stand out on
that side. The origin sits just under the top, as the target says. A top of planks
overhangs the body at the front and sides; the body is a box of vertical boards on
the sides and back over a floor board and a plinth set back a little. The face frame
(two corner stiles, a middle post, top and bottom rails) holds two inset doors, each
a frame of four battens around vertical planks, with two strap hinges, a pintle on
the stile and a ring pull. Part counts come from the concept; size and origin from
the target.
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
PLANK, FRAME, IRON = (keys.index(k) for k in ("OWN_PROP_WOOD_PLANK", "OWN_PROP_WOOD_FRAME", "OWN_PROP_IRON_BAND"))
tile = cfg.get("stave_tile", 0.38)
P = Parts(rng, tile / cfg.get("planks_per_tile", 4))
nail_h, nail_r = cfg.get("nail", 0.004), cfg.get("nail_radius", 0.008)
gap = cfg.get("gap", 0.005)
tilt = math.radians(cfg.get("tilt_deg", 0.6))


def wobble(*axes):
    """The same small random rotation applied to a part's axes: hand-fitted, not machined."""
    R = Matrix.Rotation(rng.uniform(-tilt, tilt), 3, axes[2]) @ Matrix.Rotation(rng.uniform(-tilt, tilt) / 2, 3, axes[1])
    return [R @ a for a in axes]


def wood(c, ax_len, ax_w, ax_n, length, width, thick, material, jitter=True):
    if jitter:
        ax_len, ax_w, ax_n = wobble(ax_len, ax_w, ax_n)
    P.wood(c, ax_len, ax_w, ax_n, length, width, thick, material, tile)


def split(total, count, gap_):
    """Widths of count boards with a little spread that fill total with gaps between them."""
    widths = 1 + cfg.get("width_jitter", 0.15) * rng.uniform(-1, 1, count)
    return widths / widths.sum() * (total - gap_ * (count - 1))


def boards(c0, c1, along, across, normal, length, thick, count, material):
    """Boards side by side from c0 to c1 (along `across`), grain along `along`."""
    span = (c1 - c0).length
    pos = 0.0
    for wd in split(span, count, gap):
        centre = c0 + across * (pos + wd / 2)
        wood(centre, along, across, normal, length, wd, thick, material)
        pos += wd + gap


def flat(c, ax_a, ax_b, ax_n, outline, thick, material):
    """A flat plate: a convex outline in (a, b) extruded along ax_n from c."""
    part = P.new_part()
    low, top = ([P.vert(c + ax_a * a + ax_b * b + ax_n * d, part) for a, b in outline] for d in (0.0, thick))
    P.poly(top, material, list(outline))
    P.poly(low[::-1], material, list(outline[::-1]))
    n = len(outline)
    for i in range(n):
        j = (i + 1) % n
        P.poly((low[i], low[j], top[j], top[i]), material,
               ((i * 0.02, 0), ((i + 1) * 0.02, 0), ((i + 1) * 0.02, thick), (i * 0.02, thick)))


def ring(c, axis, r_in, r_out, width, segments, material, tilt_to=None):
    """A closed ring of rectangular section around axis through c (copied from cart.py)."""
    part = P.new_part()
    a = axis.normalized()
    b = (X if abs(a.dot(X)) < 0.9 else Y).cross(a).normalized() if tilt_to is None else tilt_to.normalized()
    d = a.cross(b)
    rows = []
    for i in range(segments):
        t = 2 * math.pi * i / segments
        radial = b * math.cos(t) + d * math.sin(t)
        rows.append([P.vert(c + radial * r + a * (s * width / 2), part)
                     for r, s in ((r_in, -1), (r_out, -1), (r_out, 1), (r_in, 1))])
    arc = 2 * math.pi / segments
    for i in range(segments):
        j = (i + 1) % segments
        u0, u1 = i * arc * r_out, (i + 1) * arc * r_out
        for k in range(4):
            m = (k + 1) % 4
            side = (r_in if k in (0, 3) else r_out) if k % 2 == 0 else width
            P.poly((rows[i][k], rows[j][k], rows[j][m], rows[i][m]), material,
                   ((u0, 0), (u1, 0), (u1, side), (u0, side)))


def prism(c, axis, radius, length, sides, material):
    """A closed capped n-sided prism along axis through c (copied from cart.py)."""
    part = P.new_part()
    a = axis.normalized()
    b = (X if abs(a.dot(X)) < 0.9 else Y).cross(a).normalized()
    d = a.cross(b)
    ends = [[P.vert(c + a * (s * length / 2) + (b * math.cos(2 * math.pi * i / sides)
                                                + d * math.sin(2 * math.pi * i / sides)) * radius, part)
             for i in range(sides)] for s in (-1, 1)]
    edge = 2 * math.pi * radius / sides
    for i in range(sides):
        j = (i + 1) % sides
        P.poly((ends[0][i], ends[0][j], ends[1][j], ends[1][i]), material,
               ((i * edge, 0), ((i + 1) * edge, 0), ((i + 1) * edge, length), (i * edge, length)))
    for ring_, sign in ((ends[0], -1), (ends[1], 1)):
        verts = ring_ if sign > 0 else ring_[::-1]
        P.poly(verts, material, [((v.co - c).dot(b), (v.co - c).dot(d)) for v in verts])


# --- layout: X across the front, Y toward the front, Z up ---
top_t = cfg.get("top", 0.05)
over = cfg.get("overhang", 0.04)
bt = cfg.get("board", 0.025)
z_top = hi.z
z_under = z_top - top_t
z_floor = lo.z + cfg.get("plinth", 0.1)
bx = hi.x - over                         # body half-width
y_back = lo.y
y_front = hi.y - over                    # face of the frame
fd = cfg.get("frame_depth", 0.04)

# Top: planks along the width over the whole bbox; the front plank carries the overhang.
count = cfg.get("top_planks", 3)
widths = split(hi.y - lo.y, count, gap)
y = lo.y
for wd in widths:
    wood(Vector((0, y + wd / 2, z_under + top_t / 2)), X, Y, Z, hi.x - lo.x, wd, top_t, PLANK, jitter=False)
    y += wd + gap
# Two cleats under the top hold its planks together; they hide the gaps from below.
for sx in (-1, 1):
    wood(Vector((sx * (bx - bt - 0.03), (y_back + y_front) / 2, z_under - 0.012)), Y, X, Z,
         y_front - y_back - fd - 0.01, 0.05, 0.024, FRAME, jitter=False)

# Body: vertical side boards, back boards, a floor board.
h_body = z_under - z_floor
zc = (z_under + z_floor) / 2
for sx in (-1, 1):
    boards(Vector((sx * (bx - bt / 2), y_back + 0.003, zc)), Vector((sx * (bx - bt / 2), y_front - fd, zc)), Z, Y, X * sx,
           h_body, bt, cfg.get("side_boards", 5), PLANK)
boards(Vector((-bx + bt, y_back + bt / 2 + 0.003, zc)), Vector((bx - bt, y_back + bt / 2 + 0.003, zc)), Z, X, -Y,
       h_body, bt, cfg.get("back_boards", 5), PLANK)
wood(Vector((0, (y_back + y_front - fd) / 2 + bt / 2, z_floor + bt / 2)), X, Y, Z,
     2 * bx - 2 * bt, y_front - fd - y_back - bt, bt, PLANK, jitter=False)

# Plinth: a low frame of boards set back from the body.
pb = cfg.get("plinth_back", 0.01)
ph = z_floor - lo.z
pz = lo.z + ph / 2
px = bx - pb
wood(Vector((0, y_front - pb - bt / 2, pz)), X, Z, Y, 2 * px, ph, bt, FRAME, jitter=False)  # it stands on the floor
wood(Vector((0, y_back + pb + bt / 2, pz)), X, Z, -Y, 2 * px, ph, bt, FRAME, jitter=False)
for sx in (-1, 1):
    wood(Vector((sx * (px - bt / 2), (y_front + y_back) / 2, pz)), Y, Z, X * sx, y_front - y_back - 2 * pb - 2 * bt, ph, bt,
         FRAME, jitter=False)

# Face frame.
stile, mullion = cfg.get("stile", 0.075), cfg.get("mullion", 0.085)
rail_top, rail_bottom = cfg.get("rails", [0.075, 0.06])
fy = y_front - fd / 2
for sx in (-1, 1):
    wood(Vector((sx * (bx - stile / 2), fy, zc)), Z, X, Y, h_body, stile, fd, FRAME)
wood(Vector((0, fy, zc)), Z, X, Y, h_body, mullion, fd, FRAME)
wood(Vector((0, fy, z_under - rail_top / 2)), X, Z, Y, 2 * (bx - stile), rail_top, fd * 0.9, FRAME)
wood(Vector((0, fy, z_floor + rail_bottom / 2)), X, Z, Y, 2 * (bx - stile), rail_bottom, fd * 0.9, FRAME)

# Doors in the openings: four battens around vertical planks, then iron.
batten, door_rail, door_t = cfg.get("door", [0.07, 0.08, 0.03])
dg = cfg.get("door_gap", 0.006)
x_in, x_out = mullion / 2 + dg, bx - stile - dg
z_lo, z_hi = z_floor + rail_bottom + dg, z_under - rail_top - dg
dw, dh = x_out - x_in, z_hi - z_lo
dz = (z_lo + z_hi) / 2
dy = y_front - door_t / 2 + 0.006          # a little proud of the frame
hinge_len, hinge_w, hinge_t = cfg.get("hinge", [0.42, 0.045, 0.005])
ring_r, ring_t = cfg.get("ring", [0.032, 0.007])
for sx in (-1, 1):
    xc = sx * (x_in + x_out) / 2
    for s in (-1, 1):
        wood(Vector((xc + s * (dw / 2 - batten / 2), dy, dz)), Z, X, Y, dh, batten, door_t, FRAME)
        wood(Vector((xc, dy, dz + s * (dh / 2 - door_rail / 2))), X, Z, Y, dw - 2 * batten, door_rail, door_t, FRAME)
    inner = dw - 2 * batten
    boards(Vector((xc - inner / 2, dy - 0.006, dz)), Vector((xc + inner / 2, dy - 0.006, dz)), Z, X, Y,
           dh - 2 * door_rail + 0.01, door_t * 0.7, cfg.get("door_planks", 3), PLANK)
    face = dy + door_t / 2
    # Strap hinges from the outer edge across the door, ending in a point; a pintle on the stile.
    hinge_x = sx * (x_out + dg / 2)
    for zh in (z_hi - door_rail / 2, z_lo + door_rail / 2):
        L = hinge_len
        outline = [(0, -hinge_w / 2), (L * 0.45, -hinge_w * 0.42), (L - 0.04, -hinge_w * 0.36), (L, 0),
                   (L - 0.04, hinge_w * 0.36), (L * 0.45, hinge_w * 0.42), (0, hinge_w / 2)]  # convex, tapering
        flat(Vector((hinge_x + sx * 0.01, face, zh)), X * -sx, Z, Y, outline, hinge_t, IRON)
        prism(Vector((hinge_x + sx * 0.012, face + 0.006, zh)), Z, 0.009, hinge_w + 0.03, 6, IRON)
        for along in (0.06, 0.18, L - 0.07):
            P.nail(Vector((hinge_x - sx * along, face + hinge_t, zh)), X, Z, Y, nail_r, nail_h, IRON)
    # Ring pull on the leading edge, beside the middle post: a back plate and a hanging ring.
    rx = sx * (x_in + batten * 0.55)
    rz = dz + 0.04
    flat(Vector((rx, face, rz)), X, Z, Y, [(-0.022, -0.022), (0.022, -0.022), (0.022, 0.022), (-0.022, 0.022)], 0.004, IRON)
    ring(Vector((rx, face + 0.012, rz - ring_r + 0.008)), (Y + Z * 0.35).normalized(), ring_r - ring_t, ring_r, ring_t,
         10, IRON, tilt_to=X)

P.finish(goal, out, lo, hi, {"door": [round(dw, 3), round(dh, 3)], "body_half_width": round(bx, 3)})
