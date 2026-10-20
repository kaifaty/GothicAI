"""Build a wall trophy of a shield and two crossed swords (NW_CITY_DECO_SHIELD_01) from an image_gen concept.

blender -b --factory-startup -P deco_shield.py -- deco-shield.json OUT_DIR   (run from the repository root)
config: {"id": "deco-shield", "target": "data/content/mesh3d-market.json", "seed": 1,
         "shield": [0.56, 0.78, 0.022, 0.02], "boards": 4, "rim": [0.02, 0.006], "boss": [0.07, 0.03],
         "blade": [0.05, 0.009], "guard": [0.2, 0.026, 0.02], "grip": [0.14, 0.032], "pommel": 0.028,
         "wood_tile": 0.38, "steel_tile": 0.3}
A heater shield of four vertical boards, a little convex, painted in two halves
(dull red and off-white, no emblem) with an iron rim and boss, hangs in front of two
long swords crossed in an X: hilts above the shield's top corners, tips below its
lower edge, as on the concept. It is built with the wall on +Y and mirrored at the
end: the original hangs with its flat back on -Y and faces +Y (glTF -Z); the
trophy fills the bbox, sword hilts at its top corners.
"""
import math
import sys
from pathlib import Path

import bmesh
import numpy as np
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parts import X, Y, Z, Parts, args, load_goal  # noqa: E402

cfg, out = args()
goal, lo, hi = load_goal(cfg)
rng = np.random.default_rng(cfg.get("seed", 1))
keys = [z["key"] for z in goal["zones"]]
RED, WHITE, IRON, STEEL, GRIP = (keys.index(k) for k in ("OWN_PROP_WOOD_PAINTED", "OWN_PROP_WOOD_PAINTED_LIGHT",
                                                         "OWN_PROP_IRON_BAND", "OWN_PROP_STEEL_BLADE", "OWN_PROP_WOOD_HAFT"))
wood_tile, steel_tile = cfg.get("wood_tile", 0.38), cfg.get("steel_tile", 0.3)
P = Parts(rng, wood_tile / 4)
cx = (lo.x + hi.x) / 2
wall = hi.y                      # the flat back against the wall
front_dir = -Y

# --- swords: behind the shield, in a plane just off the wall ---
blade_w, blade_t = cfg.get("blade", [0.05, 0.009])
gl, gw, gt = cfg.get("guard", [0.2, 0.026, 0.02])
grip_len, grip_w = cfg.get("grip", [0.14, 0.032])
pommel = cfg.get("pommel", 0.028)
plane_y = wall - 0.015
centre = Vector((cx, plane_y, (lo.z + hi.z) / 2 + 0.03))


def sword(top, tip):
    """Pommel at top, blade down to tip: pommel, grip, guard, then a lens-section blade."""
    axis = (tip - top).normalized()
    side = axis.cross(Y).normalized()
    p_c = top + axis * (pommel / 2)
    P.block(p_c, (pommel, pommel * 0.8, pommel), IRON, 0.1)  # chunky pommel (axis-aligned is fine at this size)
    g0 = top + axis * pommel
    P.post(g0, g0 + axis * grip_len, grip_w, grip_w * 0.9, side, Y, GRIP, 0.3)
    guard_c = g0 + axis * (grip_len + gt / 2)
    P.board(guard_c, side, axis, Y, gl, gt, gw, IRON)
    b0 = guard_c + axis * (gt / 2)
    part = P.new_part()
    length = (tip - b0).length
    ring = lambda c, w: [P.vert(c + side * w / 2, part), P.vert(c + Y * blade_t / 2, part),
                         P.vert(c - side * w / 2, part), P.vert(c - Y * blade_t / 2, part)]
    r0, r1 = ring(b0, blade_w), ring(b0 + axis * (length * 0.85), blade_w * 0.8)
    tip_v = P.vert(tip, part)
    for i in range(4):
        j = (i + 1) % 4
        P.poly((r0[i], r0[j], r1[j], r1[i]), STEEL, ((i * 0.02, 0), ((i + 1) * 0.02, 0), ((i + 1) * 0.02, length / steel_tile), (i * 0.02, length / steel_tile)))
        P.poly((r1[i], r1[j], tip_v), STEEL, ((0, 0), (0.02, 0), (0.01, 0.05)))
    P.poly(r0[::-1], STEEL, [(0, 0), (0.02, 0), (0.02, 0.02), (0, 0.02)])


reach_x, reach_top = hi.x - cx - gl / 2 * 0.72, hi.z - 0.015   # the guard ends stay inside the bbox
for sx in (-1, 1):
    top = Vector((cx + sx * reach_x, plane_y, reach_top))
    tip_dir = (centre - top).normalized()
    tip = top + tip_dir * ((reach_top - (lo.z + 0.01)) / -tip_dir.z)
    sword(top, tip)

# --- shield: heater outline, four boards, convex front, painted halves ---
sw, sh, st, bulge = cfg.get("shield", [0.56, 0.78, 0.022, 0.02])
s_top = centre.z + sh * 0.42
outline = [(-sw / 2, s_top), (sw / 2, s_top), (sw / 2, s_top - sh * 0.38)]
for a in np.linspace(0, 1, 6)[1:]:              # right flank curving to the point
    t = a * math.pi / 2
    outline.append((sw / 2 * math.cos(t), s_top - sh * 0.38 - (sh * 0.62) * math.sin(t)))
for a in np.linspace(1, 0, 6)[1:]:              # left flank back up
    t = a * math.pi / 2
    outline.append((-sw / 2 * math.cos(t), s_top - sh * 0.38 - (sh * 0.62) * math.sin(t)))
outline.append((-sw / 2, s_top - sh * 0.38))
back_y = plane_y - blade_t - 0.004             # in front of the blades


def clip(poly, x0, x1):
    """Clip a convex polygon to x0 <= x <= x1 (Sutherland-Hodgman on two vertical lines)."""
    for bound, keep in ((x0, lambda x: x >= x0), (x1, lambda x: x <= x1)):
        res = []
        for i, p in enumerate(poly):
            q = poly[(i + 1) % len(poly)]
            if keep(p[0]):
                res.append(p)
            if keep(p[0]) != keep(q[0]):
                f = (bound - p[0]) / (q[0] - p[0])
                res.append((bound, p[1] + f * (q[1] - p[1])))
        poly = res
    return poly


def front_y(x):
    return back_y - st - bulge * (1 - (2 * x / sw) ** 2)


boards = cfg.get("boards", 4)
edges = np.linspace(-sw / 2, sw / 2, boards + 1)
for b in range(boards):
    poly = clip(outline, edges[b] + 0.002, edges[b + 1] - 0.002)
    part = P.new_part()
    mat = RED if (edges[b] + edges[b + 1]) / 2 < 0 else WHITE
    ou, ov = rng.uniform(0, wood_tile), rng.uniform(0, wood_tile)
    fr = [P.vert(Vector((cx + x, front_y(x), z)), part) for x, z in poly]
    bk = [P.vert(Vector((cx + x, back_y, z)), part) for x, z in poly]
    # the outline runs clockwise seen from the front (-Y): reverse it for the front face
    P.poly(fr[::-1], mat, [(ou + x, ov + z) for x, z in poly][::-1])
    P.poly(bk, mat, [(ou + x, ov + z) for x, z in poly])
    n = len(poly)
    for i in range(n):
        j = (i + 1) % n
        P.poly((fr[i], fr[j], bk[j], bk[i]), mat, ((0, 0), (0.02, 0), (0.02, 0.02), (0, 0.02)))

# a dark backing behind the boards, so the gaps between them read as shadow
part = P.new_part()
backing = [P.vert(Vector((cx + x * 0.97, back_y + 0.002, centre.z + (z - centre.z) * 0.97)), part) for x, z in outline]
P.poly(backing[::-1], IRON, [(x, z) for x, z in outline][::-1])

# iron rim along the front edge of the outline
rim_w, rim_t = cfg.get("rim", [0.02, 0.006])
for i in range(len(outline)):
    (x0, z0), (x1, z1) = outline[i], outline[(i + 1) % len(outline)]
    a, b = Vector((cx + x0, 0, z0)), Vector((cx + x1, 0, z1))
    along = (b - a)
    if along.length < 1e-4:
        continue
    mid = (a + b) / 2
    xm = mid.x - cx
    c = Vector((mid.x, front_y(xm) - rim_t / 2, mid.z))
    inward = Y.cross(along.normalized())
    P.board(c + inward * (rim_w / 2 - 0.003), along.normalized(), inward, front_dir, along.length + 0.006, rim_w, rim_t, IRON)
# iron boss in the middle
r_boss, h_boss = cfg.get("boss", [0.07, 0.03])
boss_c = Vector((cx, front_y(0), centre.z + 0.02))
part = P.new_part()
segs = 12
rings = []
for r, h in ((r_boss, 0.0), (r_boss * 0.8, h_boss * 0.6), (r_boss * 0.4, h_boss * 0.95)):
    rings.append([P.vert(boss_c + Vector((r * math.cos(2 * math.pi * k / segs), -h, r * math.sin(2 * math.pi * k / segs))), part)
                  for k in range(segs)])
top_v = P.vert(boss_c - Y * h_boss, part)
for i in range(2):
    for k in range(segs):
        k2 = (k + 1) % segs
        P.poly((rings[i][k2], rings[i][k], rings[i + 1][k], rings[i + 1][k2]), IRON, ((0, 0), (0.02, 0), (0.02, 0.02), (0, 0.02)))
for k in range(segs):
    P.poly((rings[2][(k + 1) % segs], rings[2][k], top_v), IRON, ((0, 0), (0.02, 0), (0.01, 0.02)))

# Built with the wall on +Y; the originals hang with the wall on -Y and face +Y (glTF -Z), so mirror
# the trophy across the depth of its bbox and turn the faces back outward.
for v in P.bm.verts:
    v.co.y = lo.y + hi.y - v.co.y
bmesh.ops.reverse_faces(P.bm, faces=P.bm.faces[:])

P.finish(goal, out, lo, hi, {})
