"""Build the harbour cannon (NW_HARBOUR_CANON_01): a cast-iron barrel on a low wooden ship carriage.

blender -b --factory-startup -P cannon.py -- cannon.json OUT_DIR   (run from the repository root)
config: {"id": "rig-cannon", "target": "data/content/mesh3d-rigging.json", "seed": 4, "sides": 16,
         "barrel": [[y, r], ...] profile from the cascabel to the muzzle face (m, y along the barrel),
         "bore": [0.065, 0.3], "trunnion": [0.06, 0.05], "elevation_deg": 4, "axis_height": 0.72,
         "cheek": [0.09, 0.30], "axles": [0.3, -0.58], "axle": [0.14, 0.12], "wheel": [0.16, 0.075, 12],
         "trail": [-0.9, 0.12], "stave_tile": 0.4}
Asset harbour-rigging (intent.md, concept rig-cannon attempt 1). The barrel is turned from the profile:
cascabel knob, base ring, reinforce, a ring before the trunnions, the chase tapering to the muzzle neck
and the flared muzzle swell; its face has a dark bore "bore" [radius, depth] in its own zone. The
trunnions [radius, length out of the barrel] sit on the cheeks under iron cap squares. The barrel is
raised elevation_deg about the trunnions. The carriage is the ship truck carriage of the concept: two
cheeks of thick plank with one big step (high under the trunnions, low behind) ("cheek" [thickness, gap from the barrel axis to the cheek's inner face]),
two axletree beams at y "axles", four solid wheels "wheel" [radius, thickness, sides] of two planks
each on iron pins, a bed plank between the cheeks, a wedge (quoin) under the breech and a long wedge of a
trail down to the ground at y trail[0]. Sizes and the origin come from the target: the wheels stand on the bbox floor, the
muzzle touches the bbox front.
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
rng = np.random.default_rng(cfg.get("seed", 4))
keys = [z["key"] for z in goal["zones"]]
CAST, BORE, FRAME, IRON = (keys.index(k) for k in
                           ("OWN_PROP_IRON_CAST", "OWN_PROP_IRON_BORE", "OWN_PROP_WOOD_FRAME", "OWN_PROP_IRON_BAND"))
tile = cfg.get("stave_tile", 0.4)
P = Parts(rng, tile / 4)
sides = cfg.get("sides", 16)
cx = (lo.x + hi.x) / 2
floor = lo.z

# --- the carriage -------------------------------------------------------------------------------
wheel_r, wheel_t, wheel_n = cfg.get("wheel", [0.16, 0.075, 12])
axle_w, axle_h = cfg.get("axle", [0.14, 0.12])
y_front, y_rear = cfg.get("axles", [0.3, -0.58])
axle_z = floor + wheel_r
cheek_t, cheek_gap = cfg.get("cheek", [0.09, 0.30])
half_w = (hi.x - lo.x) / 2


def cylinder(c, axis, radius, length, n, material, ring_uv=0.0):
    """A closed n-sided cylinder along axis; sides take the tile along the axis, the caps a planar map."""
    part = P.new_part()
    a = axis.normalized()
    u = a.cross(Z) if abs(a.dot(Z)) < 0.9 else a.cross(X)
    u.normalize()
    v = a.cross(u)
    ends = []
    for s in (-1, 1):
        ends.append([P.vert(c + a * (s * length / 2) + (u * math.cos(2 * math.pi * k / n) + v * math.sin(2 * math.pi * k / n)) * radius, part)
                     for k in range(n)])
    for k in range(n):
        k2 = (k + 1) % n
        u0, u1 = ring_uv + k / n * 2 * math.pi * radius, ring_uv + (k + 1) / n * 2 * math.pi * radius
        P.poly((ends[0][k], ends[0][k2], ends[1][k2], ends[1][k]), material, ((u0, 0), (u1, 0), (u1, length), (u0, length)))
    cap = [(radius * math.cos(2 * math.pi * k / n), radius * math.sin(2 * math.pi * k / n)) for k in range(n)]
    P.poly(ends[1], material, cap)
    P.poly(ends[0][::-1], material, cap[::-1])


def slab(points_yz, x0, x1, material):
    """A plank standing in the yz plane from x0 to x1, outline given as a convex-free polygon (y, z) list."""
    part = P.new_part()
    v_off = rng.uniform(0, tile)
    left = [P.vert(Vector((x0, y, z)), part) for y, z in points_yz]
    right = [P.vert(Vector((x1, y, z)), part) for y, z in points_yz]
    uv = [(z + 0.3, y + v_off) for y, z in points_yz]          # grain along y
    P.poly(right, material, uv)
    P.poly(left[::-1], material, uv[::-1])
    n = len(points_yz)
    for i in range(n):
        j = (i + 1) % n
        seg = (Vector(points_yz[j]) - Vector(points_yz[i])).length
        P.poly((left[i], left[j], right[j], right[i]), material, ((0, v_off), (seg, v_off), (seg, v_off + x1 - x0), (0, v_off + x1 - x0)))


# axletrees and wheels
for yy in (y_front, y_rear):
    P.wood(Vector((cx, yy, axle_z)), X, Y, Z, 2 * half_w - 2 * wheel_t - 0.01, axle_w, axle_h, FRAME, tile)
    for s in (-1, 1):
        x_w = cx + s * (half_w - wheel_t / 2 - 0.045)   # the pins end at the bbox side
        cylinder(Vector((x_w, yy, axle_z)), X, wheel_r * rng.uniform(0.97, 1.0), wheel_t, wheel_n, FRAME, rng.uniform(0, tile))
        # the plank seam across the wheel: a shallow dark strip is left to the atlas; an iron pin and washer
        cylinder(Vector((x_w + s * (wheel_t / 2 + 0.012), yy, axle_z)), X, 0.035, 0.024, 8, IRON)
        cylinder(Vector((x_w + s * (wheel_t / 2 + 0.03), yy, axle_z)), X, 0.012, 0.03, 6, IRON)

# cheeks: stepped brackets, high at the front where the trunnions rest, three steps down to the rear
axis_z = floor + cfg.get("axis_height", 0.72)        # barrel axis at the trunnions, above the floor
trun_r, trun_out = cfg.get("trunnion", [0.06, 0.05])
y_trun = cfg.get("trunnion_y", 0.02)
bottom = axle_z + axle_h / 2 - 0.01
top_front = axis_z - trun_r + 0.005
rear = y_rear - axle_w / 2 - 0.04
front = y_front + axle_w / 2 + 0.05
# one big step as the original's cheek: high under the trunnions, low behind, the top of the rear part
# falling toward the trail
steps = [(front, bottom), (front, top_front - 0.04), (front - 0.06, top_front), (y_trun - 0.2, top_front),
         (y_trun - 0.2, top_front - 0.18), (rear, top_front - 0.2), (rear, bottom)]
for s in (-1, 1):
    x_in = cx + s * cheek_gap
    x_out = x_in + s * cheek_t
    slab(steps, min(x_in, x_out), max(x_in, x_out), FRAME)
    # cap square over the trunnion and two bolts down the cheek
    cap_c = Vector(((x_in + x_out) / 2, y_trun, top_front + trun_r + 0.012))
    P.board(cap_c, Y, X, Z, 2 * trun_r + 0.08, cheek_t * 1.05, 0.024, IRON)
    for dy in (-trun_r - 0.025, trun_r + 0.025):
        cylinder(Vector(((x_in + x_out) / 2, y_trun + dy, top_front + 0.02)), Z, 0.014, 0.05, 6, IRON)

# bed plank between the cheeks, transom at the front, the quoin under the breech, the trail
P.wood(Vector((cx, (front + rear) / 2, bottom + 0.025)), Y, X, Z, front - rear, 2 * cheek_gap - 0.004, 0.05, FRAME, tile)
P.wood(Vector((cx, front - 0.06, bottom + 0.12)), X, Z, Y, 2 * cheek_gap - 0.004, 0.16, 0.08, FRAME, tile)
# the quoin: a wedge on the bed under the breech, its thick end to the rear
q_len, q_w = 0.34, 0.2
q_top = axis_z - 0.255 - 0.012 - (y_trun + 0.5) * math.tan(math.radians(cfg.get("elevation_deg", 4)))
q_bot = bottom + 0.05
wedge = [(-0.36 + q_len / 2, q_bot), (-0.36 + q_len / 2, q_bot + 0.03), (-0.36 - q_len / 2, max(q_top, q_bot + 0.06)),
         (-0.36 - q_len / 2, q_bot)]
slab(wedge, cx - q_w / 2, cx + q_w / 2, FRAME)
trail0, trail_h = cfg.get("trail", [-0.9, 0.12])
trail_y = lo.y if trail0 is None else max(lo.y, trail0)
# the trail: a long wedge from under the cheeks' rear down to the ground at the bbox end, as the original's
wedge = [(rear + 0.12, floor + 0.01), (rear + 0.12, bottom + 0.08), (trail_y, floor + trail_h * 0.35), (trail_y, floor + 0.01)]
slab(wedge, cx - 0.13, cx + 0.13, FRAME)

# --- the barrel ------------------------------------------------------------------------------------
profile = cfg.get("barrel")
elev = math.radians(cfg.get("elevation_deg", 4))
turn = (Matrix.Translation(Vector((cx, y_trun, axis_z))) @ Matrix.Rotation(elev, 4, "X")
        @ Matrix.Translation(Vector((0, -y_trun, 0))))
part = P.new_part()
rings = []
along = 0.0
for k, (y, r) in enumerate(profile):
    if k:
        along += math.hypot(y - profile[k - 1][0], r - profile[k - 1][1])
    rings.append(([P.vert(turn @ Vector((r * math.cos(2 * math.pi * j / sides), y, r * math.sin(2 * math.pi * j / sides))), part)
                   for j in range(sides)], along, r))
for k in range(len(rings) - 1):
    (a, va, ra), (b, vb, rb) = rings[k], rings[k + 1]
    for j in range(sides):
        j2 = (j + 1) % sides
        u0, u1 = j / sides * 2 * math.pi * 0.22, (j + 1) / sides * 2 * math.pi * 0.22
        P.poly((a[j2], a[j], b[j], b[j2]), CAST, ((u1, va), (u0, va), (u0, vb), (u1, vb)))
first = rings[0][0]
P.poly(first[::-1], CAST, [(0.05 * math.cos(t), 0.05 * math.sin(t)) for t in
                                               (2 * math.pi * j / sides for j in range(sides))][::-1])
# muzzle face: a ring from the swell's radius in to the bore, then the bore as a dark blind tube
bore_r, bore_d = cfg.get("bore", [0.065, 0.3])
y_face, r_face = profile[-1]
outer = rings[-1][0]
inner = [P.vert(turn @ Vector((bore_r * math.cos(2 * math.pi * j / sides), y_face, bore_r * math.sin(2 * math.pi * j / sides))), part)
         for j in range(sides)]
for j in range(sides):
    j2 = (j + 1) % sides
    P.poly((outer[j2], outer[j], inner[j], inner[j2]), CAST,
           ((r_face, j2 / sides), (r_face, j / sides), (bore_r, j / sides), (bore_r, j2 / sides)))
deep = [P.vert(turn @ Vector((bore_r * math.cos(2 * math.pi * j / sides), y_face - bore_d, bore_r * math.sin(2 * math.pi * j / sides))), part)
        for j in range(sides)]
for j in range(sides):
    j2 = (j + 1) % sides
    P.poly((inner[j2], inner[j], deep[j], deep[j2]), BORE, ((j2 / sides, 0), (j / sides, 0), (j / sides, bore_d), (j2 / sides, bore_d)))
P.poly(deep, BORE, [(0.05 * math.cos(2 * math.pi * j / sides), 0.05 * math.sin(2 * math.pi * j / sides)) for j in range(sides)])
# trunnions: short cylinders out to the cheeks, inside the cap squares
for s in (-1, 1):
    r_at = np.interp(y_trun, [p[0] for p in profile], [p[1] for p in profile])
    length = cheek_gap + cheek_t * 0.8 - r_at + 0.02
    cylinder(Vector((cx + s * (r_at - 0.02 + length / 2), y_trun, axis_z)), X, trun_r, length, 10, CAST)

P.finish(goal, out, lo, hi, {"elevation_deg": cfg.get("elevation_deg", 4), "axis_height": round(axis_z - floor, 3)})
