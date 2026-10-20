"""Build a peasant table by parts from an image_gen concept and a numeric target.

blender -b --factory-startup -P table.py -- table.json OUT_DIR
config: {"id": "table", "seed": 1, "top": 0.06, "planks": 5, "end_board": 0.12, "gap": 0.006,
         "width_jitter": 0.1, "apron": 0.2, "apron_t": 0.03, "inset": 0.08, "leg": 0.1, "foot": 0.07,
         "splay_deg": 8, "stretcher_h": 0.15, "stretcher": [0.05, 0.06], "strap": [0.22, 0.05, 0.004],
         "nail": 0.004, "nail_radius": 0.008, "stave_tile": 0.38, "planks_per_tile": 4}
The top's planks run along the length between two end boards over a dark
underlay that closes the gaps; an apron sits back under the top; four tapered
legs splay outward as far as the target bbox allows; iron straps tie each leg
to the apron. Nail heads on the end boards are the highest points and meet the
bbox. Part counts come from the concept; size, origin and budget from the target.
"""
import math
import sys
from pathlib import Path

import numpy as np
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parts import X, Y, Z, Parts, args, load_goal  # noqa: E402

cfg, out = args()
goal, lo, hi = load_goal(cfg)
rng = np.random.default_rng(cfg.get("seed", 1))
keys = [z["key"] for z in goal["zones"]]
TOP, FRAME, IRON = (keys.index(next(k for k in keys if k.endswith(s))) for s in ("TOP", "FRAME", "BRACKET"))
tile = cfg.get("stave_tile", 0.38)
P = Parts(rng, tile / cfg.get("planks_per_tile", 4))
centre = (lo + hi) / 2
hx, hy = (hi.x - lo.x) / 2, (hi.y - lo.y) / 2

nail_h, nail_r = cfg.get("nail", 0.004), cfg.get("nail_radius", 0.008)
top_t = cfg.get("top", 0.06)
z_top = hi.z - nail_h                 # upper face of the top
z_under = z_top - top_t               # its underside
end_w = cfg.get("end_board", 0.12)
gap = cfg.get("gap", 0.006)

# Top: end boards across the short sides, planks between them, a dark underlay closing the gaps.
for sx in (-1, 1):
    c = Vector((centre.x + sx * (hx - end_w / 2), centre.y, z_top - top_t / 2))
    P.wood(c, Y, X, Z, 2 * hy, end_w, top_t, TOP, tile)
count = cfg.get("planks", 5)
widths = 1 + cfg.get("width_jitter", 0.1) * rng.uniform(-1, 1, count)
widths = widths / widths.sum() * (2 * hy - gap * (count - 1))
plank_len = 2 * (hx - end_w)
y = centre.y - hy
for w in widths:
    P.wood(Vector((centre.x, y + w / 2, z_top - top_t / 2)), X, Y, Z, plank_len, w, top_t, TOP, tile)
    for sx in (-1, 1):  # a nail through each end board into the plank
        P.nail(Vector((centre.x + sx * (hx - end_w / 2), y + w / 2, z_top)), X, Y, Z, nail_r, nail_h, IRON)
    y += w + gap
under = P.new_part()
ux, uy, uz = hx - end_w + 0.01, hy - 0.01, z_under + 0.012
quad = [Vector((centre.x + sx * ux, centre.y + sy * uy, uz)) for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
corners = [P.vert(q, under) for q in quad] + [P.vert(q - Z * 0.01, under) for q in quad]
for idx in ((3, 2, 1, 0), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)):
    P.poly([corners[i] for i in idx], TOP, [(0, 0), (1, 0), (1, 1), (0, 1)])

# Apron, set back from the top's edge; the long sides run past the short ones.
inset, apron_h, apron_t = cfg.get("inset", 0.08), cfg.get("apron", 0.2), cfg.get("apron_t", 0.03)
ax, ay = hx - inset, hy - inset       # outer faces of the apron
z_apron = z_under - apron_h / 2
for sy in (-1, 1):
    P.wood(Vector((centre.x, centre.y + sy * (ay - apron_t / 2), z_apron)), X, Z, Y * sy, 2 * ax, apron_h, apron_t, FRAME, tile)
for sx in (-1, 1):
    P.wood(Vector((centre.x + sx * (ax - apron_t / 2), centre.y, z_apron)), Y, Z, X * sx, 2 * (ay - apron_t), apron_h,
           apron_t, FRAME, tile)

# Legs: tops flush with the apron corners, splayed outward as far as the bbox allows.
leg, foot = cfg.get("leg", 0.1), cfg.get("foot", 0.07)
height = z_under - lo.z
top_cx, top_cy = ax - leg / 2, ay - leg / 2
room = min(hx - top_cx, hy - top_cy) - foot / 2
splay = min(math.radians(cfg.get("splay_deg", 8)), math.atan(room / height))
off = height * math.tan(splay)
legs = []
for sx in (-1, 1):
    for sy in (-1, 1):
        top = Vector((centre.x + sx * top_cx, centre.y + sy * top_cy, z_under))
        bottom = Vector((centre.x + sx * (top_cx + off), centre.y + sy * (top_cy + off), lo.z))
        P.post(top, bottom, leg, foot, X, Y, FRAME, tile)
        legs.append((sx, sy, top, bottom))


def leg_at(top, bottom, z):
    return top + (bottom - top) * ((top.z - z) / (top.z - bottom.z))


# Two low stretchers, one between the legs at each short end.
sh = lo.z + cfg.get("stretcher_h", 0.15)
st_w, st_t = cfg.get("stretcher", [0.05, 0.06])
for sx in (-1, 1):
    ends = [leg_at(t, b, sh) for s_x, s_y, t, b in legs if s_x == sx]
    mid = (ends[0] + ends[1]) / 2
    P.wood(mid, Y, Z, X, (ends[0] - ends[1]).length, st_t, st_w, FRAME, tile)

# Iron straps: one on each outer face at every leg, from the corner onto the apron, two nails each.
s_len, s_w, s_t = cfg.get("strap", [0.22, 0.05, 0.004])
z_strap = z_under - apron_h / 2  # mid-apron, where the leg meets it; the top overhang would hide it higher up
for sx, sy, top, bottom in legs:
    corner = leg_at(top, bottom, z_strap)
    for normal, along, face in ((Y * sy, X * -sx, ay), (X * sx, Y * -sy, ax)):
        # lie on whichever is further out at this height: the apron face or the splayed leg
        base = max(face, abs((corner - centre).dot(normal)) + leg / 2 * (1 - (top.z - z_strap) / height * (1 - foot / leg)))
        start = centre + normal * (base + 0.001) + Z * (z_strap - centre.z)
        start += along * 0 + (corner - centre - normal * (corner - centre).dot(normal) - Z * (corner - centre).dot(Z))
        start -= along * (leg / 2)
        P.strap(start + along * (s_len / 2), along, Z, normal, s_len, s_w, s_t, IRON)
        for f in (0.2, 0.75):
            P.nail(start + along * (s_len * f) + normal * s_t, along, Z, normal, nail_r, nail_h, IRON)

P.finish(goal, out, lo, hi, {"splay_deg": round(math.degrees(splay), 2)})
