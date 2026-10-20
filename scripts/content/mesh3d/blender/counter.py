"""Build the merchant's long shop counter by parts from an image_gen concept and a numeric target.

blender -b --factory-startup -P counter.py -- counter.json OUT_DIR   (run from the repository root)
config: {"id": "counter", "target": "data/content/mesh3d-market.json", "seed": 1, "top": 0.06,
         "top_rows": 6, "overhang": 0.06, "flap": 0.62, "flap_planks": 3, "bays": 5, "post": [0.085, 0.06],
         "rails": [0.075, 0.07], "bay_boards": 5, "board": 0.025, "kick": [0.12, 0.03], "end_boards": 5,
         "shelf_depth": 0.55, "shelves": [0.17, 0.48], "shelf_t": 0.03, "gap": 0.005, "width_jitter": 0.15,
         "tilt_deg": 0.5, "corner": [0.16, 0.05, 0.004], "hinge": [0.32, 0.05, 0.005], "nail": 0.004,
         "nail_radius": 0.009, "stave_tile": 0.38, "planks_per_tile": 4}
The customer side faces +Y in Blender (glTF -Z): there the original's top overhangs its
body. The original is one closed block over the whole bbox (its top covers all of it),
so the concept's L is folded into that rectangle: a top of planks with staggered butt
joints over the full depth, a hinged flap of cross planks at the -X end, a framed front
of posts every bay with vertical boards and a kick board, framed ends, and on the shop
side open shelves (two boards) in front of a board partition that closes the inside.
Iron: corner straps on the front corners of the top, strap hinges on the flap, studs
on the front posts. Part counts come from the concept; size from the target.
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
nail_h, nail_r = cfg.get("nail", 0.004), cfg.get("nail_radius", 0.009)
gap = cfg.get("gap", 0.005)
tilt = math.radians(cfg.get("tilt_deg", 0.5))


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


def boards(c0, c1, along, across, normal, length, thick, count, material, jitter=True):
    """Boards side by side from c0 to c1 (along `across`), grain along `along`."""
    pos = 0.0
    for wd in split((c1 - c0).length, count, gap):
        wood(c0 + across * (pos + wd / 2), along, across, normal, length, wd, thick, material, jitter)
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


# --- layout: X along the counter, Y toward the customer, Z up ---
iron_t = cfg.get("corner", [0.16, 0.05, 0.004])[2]
top_t = cfg.get("top", 0.06)
z_top = hi.z - iron_t                       # the corner straps on the top meet the bbox
z_under = z_top - top_t
over = cfg.get("overhang", 0.06)
y_front = hi.y - over                       # face of the front frame
y_back = lo.y
x0, x1 = lo.x, hi.x
bt = cfg.get("board", 0.025)
pw, pd = cfg.get("post", [0.085, 0.06])
rail_top, rail_bottom = cfg.get("rails", [0.075, 0.07])
kick_h, kick_out = cfg.get("kick", [0.12, 0.03])
z_floor = lo.z
h_body = z_under - z_floor
zc = (z_under + z_floor) / 2

# Top: rows of planks along the counter with staggered butt joints; the flap at -X.
flap = cfg.get("flap", 0.62)
xf = x0 + flap                               # where the fixed top starts
rows = split(hi.y - lo.y, cfg.get("top_rows", 6), gap)
y = lo.y
for i, wd in enumerate(rows):
    joint = xf + (x1 - xf) * (0.42 + 0.16 * rng.uniform() if i % 2 else 0.55 + 0.16 * rng.uniform())
    for a, b in ((xf + gap / 2, joint - gap / 2), (joint + gap / 2, x1)):
        wood(Vector(((a + b) / 2, y + wd / 2, z_under + top_t / 2)), X, Y, Z, b - a, wd, top_t, PLANK, jitter=False)
    y += wd + gap
boards(Vector((x0 + 0.004, 0, z_under + top_t / 2)), Vector((xf - gap / 2, 0, z_under + top_t / 2)), Y, X, Z,
       hi.y - lo.y - 0.006, top_t, cfg.get("flap_planks", 3), PLANK, jitter=False)
# Battens under the top tie the rows together.
for x in np.linspace(x0 + 0.3, x1 - 0.3, 4):
    wood(Vector((x, (y_front + y_back) / 2, z_under - 0.012)), Y, X, Z, y_front - y_back - 0.1, 0.06, 0.024, FRAME, jitter=False)

# Front: posts every bay, top and bottom rails, vertical boards, a kick board.
bays = cfg.get("bays", 5)
post_x = np.linspace(x0 + pw / 2, x1 - pw / 2, bays + 1)
fy = y_front - pd / 2
for i, x in enumerate(post_x):
    wood(Vector((x, fy, zc + kick_h / 2)), Z, X, Y, h_body - kick_h, pw, pd, FRAME, jitter=0 < i < bays)
z_kick = z_floor + kick_h
wood(Vector((0, fy, z_under - rail_top / 2)), X, Z, Y, x1 - x0 - 2 * pw, rail_top, pd * 0.85, FRAME)
wood(Vector((0, fy, z_kick + rail_bottom / 2)), X, Z, Y, x1 - x0 - 2 * pw, rail_bottom, pd * 0.85, FRAME)
wood(Vector((0, y_front + kick_out - 0.03 / 2, z_floor + kick_h / 2)), X, Z, Y, x1 - x0, kick_h, 0.03, FRAME, jitter=False)
for a, b in zip(post_x[:-1], post_x[1:]):
    za, zb = z_kick + rail_bottom, z_under - rail_top
    boards(Vector((a + pw / 2 + gap, fy - pd / 2 + bt / 2 + 0.008, (za + zb) / 2)),
           Vector((b - pw / 2 - gap, fy - pd / 2 + bt / 2 + 0.008, (za + zb) / 2)), Z, X, Y,
           zb - za + 0.02, bt, cfg.get("bay_boards", 5), PLANK)
for x in post_x:  # square studs where the rails cross a post
    for z in (z_under - rail_top / 2, z_kick + rail_bottom / 2):
        P.nail(Vector((x, y_front, z)), X, Z, Y, nail_r * 1.3, nail_h, IRON)

# Shop side: corner and bay posts, open shelves in front of a partition, a floor board.
depth = cfg.get("shelf_depth", 0.55)
y_part = y_back + depth                      # the partition closes the inside
by = y_back + pd / 2
for x in post_x:
    wood(Vector((x, by, zc)), Z, X, -Y, h_body, pw, pd, FRAME, jitter=False)
boards(Vector((x0 + pw, y_part + bt / 2, zc)), Vector((x1 - pw, y_part + bt / 2, zc)), Z, X, -Y,
       h_body, bt, 2 * bays + 2, PLANK, jitter=False)
shelf_t = cfg.get("shelf_t", 0.03)
for zs in [z_floor + 0.03] + [z_floor + h for h in cfg.get("shelves", [0.17, 0.48])]:
    for a, b in zip(post_x[:-1], post_x[1:]):
        wood(Vector(((a + b) / 2, y_back + 0.01 + (y_part - y_back - 0.01) / 2, zs + shelf_t / 2)), X, Y, Z,
             b - a - pw, y_part - y_back - 0.01, shelf_t, PLANK)
wood(Vector((0, by, z_under - rail_top / 2)), X, Z, -Y, x1 - x0 - 2 * pw, rail_top, pd * 0.8, FRAME)

# Ends: a middle post, rails and vertical boards between the front and back corner posts.
for sx in (-1, 1):
    xe = sx * (hi.x - bt / 2)
    ya, yb = y_back + pd, y_front - pd
    wood(Vector((sx * (hi.x - pd / 2), (ya + yb) / 2, zc)), Z, Y, X * sx, h_body, pw, pd, FRAME, jitter=False)
    for half in ((ya, (ya + yb) / 2 - pw / 2), ((ya + yb) / 2 + pw / 2, yb)):
        boards(Vector((xe - sx * 0.01, half[0] + gap, zc)), Vector((xe - sx * 0.01, half[1] - gap, zc)), Z, Y, X * sx,
               h_body - 0.02, bt, max(2, cfg.get("end_boards", 5) // 2), PLANK)
    wood(Vector((sx * (hi.x - pd / 2), (ya + yb) / 2, z_under - rail_top / 2)), Y, Z, X * sx, yb - ya, rail_top, pd * 0.8, FRAME)

# Iron: straps over the front corners of the top, hinges on the flap.
leg, width, thick = cfg.get("corner", [0.16, 0.05, 0.004])
for sx in (-1, 1):
    corner = Vector((sx * hi.x, hi.y, z_top))
    P.plate_l(corner, X * -sx, -Y, Z, leg, width, thick, IRON)
    P.board(Vector((sx * (hi.x - leg / 2), hi.y - 0.002, z_under + top_t / 2)), X, Z, Y, leg, top_t * 0.8, 0.004, IRON)
hinge_len, hinge_w, hinge_t = cfg.get("hinge", [0.32, 0.05, 0.005])
for yh in (lo.y + 0.28, hi.y - 0.28):
    outline = [(0, -hinge_w / 2), (hinge_len * 0.5, -hinge_w * 0.42), (hinge_len - 0.04, -hinge_w * 0.34), (hinge_len, 0),
               (hinge_len - 0.04, hinge_w * 0.34), (hinge_len * 0.5, hinge_w * 0.42), (0, hinge_w / 2)]
    flat(Vector((xf + 0.07, yh, z_top - 0.0005)), -X, Y, Z, outline, hinge_t - 0.001, IRON)
    prism(Vector((xf, yh, z_top - 0.005)), Y, 0.009, hinge_w + 0.02, 6, IRON)

P.finish(goal, out, lo, hi, {"bays": bays, "flap_m": flap, "shelf_depth_m": depth})
