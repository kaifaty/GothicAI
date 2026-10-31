"""Build one arm set of the market direction post by parts from an image_gen concept and a numeric target.

blender -b --factory-startup -P sign.py -- sign.json OUT_DIR   (run from the repository root)
config: {"id": "sign", "target": "data/content/mesh3d-market.json", "seed": 1, "post_sides": 10,
         "post_radius": [0.05, 0.046], "point": 0.03, "boards": [[0.06, 0.17, 1.0], [0.53, 0.15, 0.8]],
         "board_t": 0.036, "arrow": 0.11, "nail": 0.004, "nail_radius": 0.009, "stave_tile": 0.38,
         "planks_per_tile": 4}
In NEWWORLD the post of this visual stands at the +Y edge of the bbox (glTF -Z), and two
or three copies turned to different streets share one post; an oil lamp stands on top as
a vob of its own. So the post keeps the original's axis and top, is many-sided (copies at
any angle overlap as one round post), and the arms point one way, to -Y: two arrow
boards under the top ("boards": [drop of the board top below the post top, height,
share of the arm length]), each nailed to the post's side with two nails.
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
FRAME, PLANK, IRON = (keys.index(k) for k in ("OWN_PROP_WOOD_FRAME", "OWN_PROP_WOOD_PLANK", "OWN_PROP_IRON_BAND"))
tile = cfg.get("stave_tile", 0.38)
P = Parts(rng, tile / cfg.get("planks_per_tile", 4))
nail_h, nail_r = cfg.get("nail", 0.004), cfg.get("nail_radius", 0.009)


def post(axis_xy, z0, z1, sides, r0, r1, point, material):
    """A closed many-sided tapered post from z0 to z1 with a low pointed top; grain along it."""
    part = P.new_part()
    c = Vector((axis_xy[0], axis_xy[1], 0))
    rings = []
    for z, r in ((z0, r0), (z1 - point, r1)):
        rings.append([P.vert(c + Vector((math.cos(2 * math.pi * i / sides) * r, math.sin(2 * math.pi * i / sides) * r, z)), part)
                      for i in range(sides)])
    tip = P.vert(c + Vector((0, 0, z1)), part)
    v_off = rng.uniform(0, tile)
    k = int(rng.integers(0, 4))
    length = z1 - point - z0
    for i in range(sides):
        j = (i + 1) % sides
        u0 = k * P.plank_u + i / sides * P.plank_u * 2
        u1 = k * P.plank_u + (i + 1) / sides * P.plank_u * 2
        P.poly((rings[0][i], rings[0][j], rings[1][j], rings[1][i]), material,
               ((u0, v_off), (u1, v_off), (u1, v_off + length), (u0, v_off + length)))
        a, b = rings[1][i].co - c, rings[1][j].co - c
        P.poly((rings[1][i], rings[1][j], tip), material, ((a.x, a.y), (b.x, b.y), (0, 0)))
    P.poly(rings[0][::-1], material, [((v.co - c).x, (v.co - c).y) for v in rings[0][::-1]])


def arrow_board(x0, x1, y_root, y_tip, z_top, height, arrow, material):
    """A board in the YZ plane from the post (y_root) to an arrow point at y_tip; grain along it."""
    part = P.new_part()
    s = 1 if y_tip > y_root else -1
    yb = y_tip - s * arrow                                   # where the point starts
    zm = z_top - height / 2
    outline = [(y_root, z_top - height), (yb, z_top - height), (y_tip, zm), (yb, z_top), (y_root, z_top)]
    if s < 0:
        outline = outline[::-1]
    sides = [[P.vert(Vector((x, y, z)), part) for y, z in outline] for x in (x0, x1)]
    v_off = rng.uniform(0, tile)
    k = int(rng.integers(0, 4))

    def uv(y, z):  # the board's face takes one plank of the tile, grain along Y
        return (k * P.plank_u + (z - (z_top - height)) / height * P.plank_u, v_off + abs(y - y_root))

    P.poly(sides[1], material, [uv(y, z) for y, z in outline])
    P.poly(sides[0][::-1], material, [uv(y, z) for y, z in outline[::-1]])
    n = len(outline)
    for i in range(n):
        j = (i + 1) % n
        (ya, za), (yb_, zb) = outline[i], outline[j]
        edge = math.hypot(yb_ - ya, zb - za)
        P.poly((sides[0][i], sides[0][j], sides[1][j], sides[1][i]), material,
               ((0, 0), (edge, 0), (edge, x1 - x0), (0, x1 - x0)))


sides = cfg.get("post_sides", 10)
r0, r1 = cfg.get("post_radius", [0.05, 0.046])
axis = (0.0, hi.y - r0)                       # the original post's axis: copies share it
post(axis, lo.z, hi.z, sides, r0, r1, cfg.get("point", 0.03), FRAME)

bt = cfg.get("board_t", 0.036)
x1 = hi.x - nail_h                            # the outer face; nail heads meet the bbox
x0 = x1 - bt
y_root = axis[1]                              # the board's end runs into the post
for drop, height, share in cfg.get("boards", [[0.06, 0.17, 1.0], [0.53, 0.15, 0.8]]):
    z_top = hi.z - drop
    y_tip = y_root - share * (y_root - lo.y)
    arrow_board(x0, x1, y_root, y_tip, z_top, height, cfg.get("arrow", 0.11), PLANK)
    for dz in (0.3, 0.7):
        P.nail(Vector((x1, y_root - r1 * 0.35, z_top - height * dz)), Y, Z, X, nail_r, nail_h, IRON)

P.finish(goal, out, lo, hi, {"post_axis_m": [round(axis[0], 4), round(axis[1], 4)]})
