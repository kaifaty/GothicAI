"""Build a stone-pier bench by parts from an image_gen concept and a numeric target.

blender -b --factory-startup -P bench.py -- bench.json OUT_DIR
config: {"id": "bench", "seed": 1, "zones": [...] (overrides the pilot's), "planks": 2, "plank": 0.09,
         "gap": 0.008, "pier": [0.35, 0.756], "pier_inset": 0.25, "courses": 3, "joint": 0.01,
         "stone_jitter": 0.006, "cramp": [0.05, 0.004, 0.11], "nail": 0.004, "nail_radius": 0.008,
         "stave_tile": 0.38, "stone_tile": 2.5, "planks_per_tile": 4}
The original city bench is a thick seat on two stone piers, so its supports are
stone (zone group STONE), not the pilot's guessed wooden frame. Each pier is
courses of separate stones around a dark mortar core; the seat planks lie on
the piers; an iron cramp over each plank at each pier is bent down its edges.
Nail heads on the cramps are the highest points and meet the bbox.
"""
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
SEAT, STONE, IRON = (keys.index(next(k for k in keys if k.endswith(s))) for s in ("SEAT", "PIER", "BRACKET"))
tile, stone_tile = cfg.get("stave_tile", 0.38), cfg.get("stone_tile", 2.5)
P = Parts(rng, tile / cfg.get("planks_per_tile", 4))
centre = (lo + hi) / 2
hx, hy = (hi.x - lo.x) / 2, (hi.y - lo.y) / 2

nail_h, nail_r = cfg.get("nail", 0.004), cfg.get("nail_radius", 0.008)
cramp_w, cramp_t, cramp_drop = cfg.get("cramp", [0.05, 0.004, 0.11])
plank_t = cfg.get("plank", 0.09)
z_seat = hi.z - nail_h - cramp_t          # top face of the seat
z_pier = z_seat - plank_t                 # top of the piers
pier_x, pier_y = cfg.get("pier", [0.35, 0.756])
pier_y = min(pier_y, 2 * hy)
inset = cfg.get("pier_inset", 0.25)
joint, jitter = cfg.get("joint", 0.01), cfg.get("stone_jitter", 0.006)
courses = cfg.get("courses", 3)

# Piers: courses of two stones each, the split moving course to course, around a dark mortar core.
for sx in (-1, 1):
    px = centre.x + sx * (hx - inset - pier_x / 2)
    core = Vector((pier_x - 2 * joint, pier_y - 2 * joint, z_pier - lo.z - joint))
    P.block(Vector((px, centre.y, lo.z + core.z / 2)), core, STONE, stone_tile)
    course_h = (z_pier - lo.z) / courses
    for c in range(courses):
        z0 = lo.z + c * course_h
        # Running bond: courses alternate 2 x 3 and 1 x 2 stones (along the length x across the depth).
        nx, ny = (2, 3) if c % 2 == 0 else (1, 2)
        xs = np.sort(np.r_[0, rng.uniform(0.4, 0.6, nx - 1) if nx > 1 else [], 1]) * pier_x + px - pier_x / 2
        ys = np.sort(np.r_[0, (np.arange(1, ny) + rng.uniform(-0.2, 0.2, ny - 1)) / ny, 1]) * pier_y + centre.y - pier_y / 2
        for i, (x0, x1) in enumerate(zip(xs[:-1], xs[1:])):
            for j, (y0, y1) in enumerate(zip(ys[:-1], ys[1:])):
                # joints only between stones; the outer stones meet the pier's faces, a few mm inside at most
                x0 += 0 if i == 0 else joint / 2
                x1 -= 0 if i == nx - 1 else joint / 2
                y0 += 0 if j == 0 else joint / 2
                y1 -= 0 if j == ny - 1 else joint / 2
                inward = rng.uniform(0, jitter)
                size = Vector((x1 - x0 - (inward if i in (0, nx - 1) else 0), y1 - y0, course_h - joint))
                cx = (x0 + x1) / 2 + (inward / 2 if i == 0 else -inward / 2 if i == nx - 1 else 0)
                P.block(Vector((cx, (y0 + y1) / 2, z0 + joint / 2 + size.z / 2)), size, STONE, stone_tile)

# Seat: planks side by side along the full length.
count = cfg.get("planks", 2)
gap = cfg.get("gap", 0.008)
seat_w = min(cfg.get("seat_width", 0.608), 2 * hy)
widths = 1 + 0.06 * rng.uniform(-1, 1, count)
widths = widths / widths.sum() * (seat_w - gap * (count - 1))
y = centre.y - seat_w / 2
planks = []
for w in widths:
    P.wood(Vector((centre.x, y + w / 2, z_seat - plank_t / 2)), X, Y, Z, 2 * hx, w, plank_t, SEAT, tile)
    planks.append((y, y + w))
    y += w + gap

# Cramps: a strap over each plank at each pier, bent down both long edges, two nails on top.
for sx in (-1, 1):
    px = centre.x + sx * (hx - inset - pier_x / 2)
    for y0, y1 in planks:
        width = y1 - y0
        P.strap(Vector((px, (y0 + y1) / 2, z_seat)), Y, X, Z, width + 2 * cramp_t, cramp_w, cramp_t, IRON)
        for ye, sign in ((y0, -1), (y1, 1)):
            c = Vector((px, ye + sign * cramp_t / 2, z_seat + cramp_t - cramp_drop / 2))
            P.board(c, Z, X, Y * sign, cramp_drop, cramp_w, cramp_t, IRON)
        for f in (0.3, 0.7):
            P.nail(Vector((px, y0 + width * f, z_seat + cramp_t)), X, Y, Z, nail_r, nail_h, IRON)

P.finish(goal, out, lo, hi)
