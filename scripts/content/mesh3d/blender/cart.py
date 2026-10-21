"""Build the market hand cart by parts from an image_gen concept and a numeric target.

blender -b --factory-startup -P cart.py -- cart.json OUT_DIR   (run from the repository root)
config: {"id": "cart", "target": "data/content/mesh3d-market.json", "seed": 1, "bed_width": 1.28, "bed_length": 1.7,
         "beam": [0.08, 0.1], "post": 0.08, "rails": [0.2, 0.43], "rail": [0.12, 0.04], "planks": 7, "plank": 0.03,
         "gap": 0.008, "width_jitter": 0.12, "posts": 3, "wheel_width": 0.06, "felloe": 0.07, "tyre": 0.022,
         "spokes": 8, "segments": 24, "hub": [0.085, 0.24], "cross_bar": 1.25, "nail": 0.004, "nail_radius": 0.008,
         "stave_tile": 0.38, "planks_per_tile": 4}
The axle is the target's origin and the wheel radius its depth below it. The bed
sits on two long beams that run on as the shafts; the whole frame is tilted about
the axle until the shaft tips rest on the ground and the rear posts reach the top
of the bbox. Rails run inside the posts on both long sides and the back; the front
is open. Wheels: felloe ring, spokes, hub and an iron tyre with one patch plate.
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


def ring(c, axis, r_in, r_out, width, segments, material, u_scale=1.0, part=None):
    """A closed ring of rectangular section around axis through c; tile U runs around it."""
    part = P.new_part() if part is None else part
    a = axis.normalized()
    b = (X if abs(a.dot(X)) < 0.9 else Y).cross(a).normalized()
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
        u0, u1 = i * arc * r_out * u_scale, (i + 1) * arc * r_out * u_scale
        for k in range(4):
            m = (k + 1) % 4
            side = (r_in if k in (0, 3) else r_out) if k % 2 == 0 else width
            P.poly((rows[i][k], rows[j][k], rows[j][m], rows[i][m]), material,
                   ((u0, 0), (u1, 0), (u1, side), (u0, side)))


def prism(c, axis, radius, length, sides, material):
    """A closed capped n-sided prism (hub, axle end) along axis through c."""
    part = P.new_part()
    a = axis.normalized()
    b = (X if abs(a.dot(X)) < 0.9 else Y).cross(a).normalized()
    d = a.cross(b)
    ends = []
    for s in (-1, 1):
        ends.append([P.vert(c + a * (s * length / 2) + (b * math.cos(2 * math.pi * i / sides)
                                                          + d * math.sin(2 * math.pi * i / sides)) * radius, part)
                     for i in range(sides)])
    edge = 2 * math.pi * radius / sides
    for i in range(sides):
        j = (i + 1) % sides
        P.poly((ends[0][i], ends[0][j], ends[1][j], ends[1][i]), material,
               ((i * edge, 0), ((i + 1) * edge, 0), ((i + 1) * edge, length), (i * edge, length)))
    for ring_, sign in ((ends[0], -1), (ends[1], 1)):
        verts = ring_ if sign > 0 else ring_[::-1]
        P.poly(verts, material, [((v.co - c).dot(b), (v.co - c).dot(d)) for v in verts])


# --- the frame in a level local space: X across, Y forward to the shaft tips, Z up, axle at the origin ---
bed_w = cfg.get("bed_width", 1.28)
beam_w, beam_h = cfg.get("beam", [0.08, 0.1])
post = cfg.get("post", 0.07)
inset = 0.002                                  # posts are this much smaller on each side (below)
y_rear, y_tip = lo.y, hi.y
radius = -lo.z
beam_x = bed_w / 2 - beam_w / 2
beam_z = 0.04 + beam_h / 2                     # the beams rest on the axle
floor_z = beam_z + beam_h / 2                  # underside of the floor planks
plank_t = cfg.get("plank", 0.03)

# Tilt about the axle until the shaft tips rest on the ground (front down), then let the
# rear posts reach the top of the bbox: A sin a + B cos a = C for the tip's underside.
# The tilt shortens the frame, so the level lengths are stretched until its ends meet the bbox.
for _ in range(4):
    A, B, C = y_tip, beam_z - beam_h / 2, lo.z
    angle = math.asin(C / math.hypot(A, B)) - math.atan2(B, A)
    y_tip = (hi.y + (beam_z + beam_h / 2) * math.sin(angle)) / math.cos(angle)   # the tip's upper edge
    y_rear = (lo.y + B * math.sin(angle)) / math.cos(angle)                       # the rear's lower edge
R = Matrix.Rotation(angle, 3, "X")
y_front = y_rear + cfg.get("bed_length", 1.7)
top_z = (hi.z - (y_rear + inset) * math.sin(angle)) / math.cos(angle)   # the rear posts' back top edge
wall = top_z - floor_z - plank_t


def w(v):
    """Level local point -> tilted world point (rotation about the axle)."""
    return R @ Vector(v)


def board(c, ax_len, ax_w, ax_n, length, width, thick, material):
    P.wood(w(c), R @ ax_len, R @ ax_w, R @ ax_n, length, width, thick, material, tile)


# Long beams that run on into the shafts, a little thinner toward the tips.
for sx in (-1, 1):
    board((sx * beam_x, (y_rear + y_tip) / 2, beam_z), Y, Z, X * sx, y_tip - y_rear, beam_h, beam_w, FRAME)
# Cross bearers under the floor: rear, over the axle, front.
for yc in (y_rear + beam_w / 2, 0.0, y_front - beam_w / 2):
    board((0, yc, beam_z), X, Z, Y, 2 * beam_x - beam_w, beam_h * 0.8, beam_w, FRAME)
# Floor planks along the bed.
count = cfg.get("planks", 7)
widths = 1 + cfg.get("width_jitter", 0.12) * rng.uniform(-1, 1, count)
gap = cfg.get("gap", 0.008)
widths = widths / widths.sum() * (bed_w - gap * (count - 1))
x = -bed_w / 2
for wd in widths:
    board((x + wd / 2, (y_rear + y_front) / 2, floor_z + plank_t / 2), Y, X, Z, y_front - y_rear, wd, plank_t, PLANK)
    x += wd + gap
# Posts: three along each long side, one more in the middle of the back. A post is as wide as
# the beam it passes through, and its place is flush with the beam, the floor's edge and ends:
# faces sharing a plane z-fight (coplanar.py). So each post is 2 mm smaller on every side than
# its place and its foot ends 2 mm up inside the beam (the middle one inside the floor plank).
post_in = post - 2 * inset
post_ys = np.linspace(y_rear + post / 2, y_front - post / 2, cfg.get("posts", 3))
for sx in (-1, 1):
    for yc in post_ys:
        P.post(w((sx * (bed_w / 2 - post / 2), yc, top_z)), w((sx * (bed_w / 2 - post / 2), yc, beam_z - beam_h / 2 + inset)),
               post_in, post_in, R @ X, R @ Y, FRAME, tile)
P.post(w((0, y_rear + post / 2, top_z)), w((0, y_rear + post / 2, floor_z + inset)), post_in, post_in, R @ X, R @ Y, FRAME, tile)
# Rails inside the posts: both long sides and the back.
rail_h, rail_t = cfg.get("rail", [0.1, 0.035])
inner = post - inset                           # from the bed's edge to the posts' inner faces
for h in cfg.get("rails", [0.22, 0.45]):
    zc = floor_z + plank_t + h
    for sx in (-1, 1):
        board((sx * (bed_w / 2 - inner - rail_t / 2), (y_rear + y_front) / 2, zc), Y, Z, X * sx,
              y_front - y_rear - 0.02, rail_h, rail_t, PLANK)
    board((0, y_rear + inner + rail_t / 2, zc), X, Z, -Y, bed_w - 2 * inner - 2 * rail_t, rail_h, rail_t, PLANK)
# Cross bar between the shafts.
board((0, y_front + cfg.get("cross_bar", 1.25) * (y_tip - y_front) / 2, beam_z), X, Z, Y, 2 * beam_x - beam_w, 0.07, 0.06, FRAME)

# Iron: L plates on the outer corners of the bed and caps on the shaft tips, with nails.
plate = 0.12
for sx in (-1, 1):
    for yc, sy in ((y_rear, 1), (y_front, -1)):
        corner = (sx * bed_w / 2, yc, beam_z - beam_h / 2 + 0.01)
        P.plate_l(w(corner), R @ (Y * sy), R @ Z, R @ (X * sx), plate, 0.05, 0.004, IRON)
        for along, up in ((0.08, 0.025), (0.025, 0.08)):
            P.nail(w((corner[0] + sx * 0.004, yc + sy * along, corner[2] + up)), R @ Y, R @ Z, R @ (X * sx), nail_r, nail_h, IRON)
    tip = (sx * beam_x, y_tip - 0.06, beam_z)
    P.board(w(tip), R @ Y, R @ Z, R @ X, 0.1, beam_h + 0.008, beam_w + 0.008, IRON)

# Axle under the bed, out to the hubs.
hub_r, hub_len = cfg.get("hub", [0.075, 0.24])
wheel_x = hi.x - hub_len / 2 - 0.02
prism(Vector((0, 0, 0)), X, 0.035, 2 * wheel_x, 8, IRON)

# Wheels (not tilted: they turn about the axle anyway).
ww, felloe, tyre = cfg.get("wheel_width", 0.06), cfg.get("felloe", 0.07), cfg.get("tyre", 0.022)
segments, spokes = cfg.get("segments", 24), cfg.get("spokes", 8)
for sx in (-1, 1):
    c = Vector((sx * wheel_x, 0, 0))
    ring(c, X, radius - tyre - felloe, radius - tyre, ww, segments, FRAME)
    ring(c, X, radius - tyre, radius, ww * 0.9, segments, IRON)
    prism(c + X * (sx * 0.02), X, hub_r, hub_len, 10, FRAME)
    for i in range(spokes):
        t = 2 * math.pi * (i + 0.5) / spokes
        radial = Y * math.cos(t) + Z * math.sin(t)
        tangent = Y * -math.sin(t) + Z * math.cos(t)
        P.post(c + radial * (radius - tyre - felloe + 0.01), c + radial * (hub_r - 0.01), 0.035, 0.05, X, tangent, FRAME, tile)
    # one repaired tyre: a patch plate over a crack, two nails
    if sx > 0:
        t = math.radians(200)
        radial = Y * math.cos(t) + Z * math.sin(t)
        tangent = Y * -math.sin(t) + Z * math.cos(t)
        P.board(c + radial * (radius + 0.002), tangent, X, radial, 0.12, ww * 0.95, 0.006, IRON)

P.finish(goal, out, lo, hi, {"tilt_deg": round(math.degrees(angle), 2), "wall": round(wall, 3), "wheel_radius": round(radius, 3)})
