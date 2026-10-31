"""Build a thick candle in an iron drip dish (NW_CITY_CANDLE_01) from an image_gen concept and a numeric target.

blender -b --factory-startup -P candle.py -- candle.json OUT_DIR   (run from the repository root)
config: {"id": "candle", "target": "data/content/mesh3d-market.json", "seed": 1, "segments": 20,
         "dish": [0.18, 0.032, 0.006], "feet": [3, 0.025, 0.03], "handle": [0.05, 0.012, 0.008],
         "candle": [0.06, 0.2, 4.0], "candle_grid": [24, 10], "drips": 0, "drip": 0.0, "drip_width": [0.085, 0.04],
         "drip_rods": [8, 14], "puddle": [0.11, 0.009],
         "iron_tile": 0.2, "wax_tile": 0.15}
The dish is a lathe shell (floor, raised rim with a rolled lip) on three stubby
feet, with a flat ring handle on the far side, as on the concept. The candle is
a thick leaning cylinder with a melted, uneven top folding into a crater round
the wick. Its drips are separate rods half sunk into its side ("drip_rods": count,
sides), thicker toward the bottom and ending in a drop; the older swell of the
surface ("drips", "drip") is still there but off, it gave blocky ledges. A wobbly
puddle of wax spreads over the dish floor.
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
IRON, WAX = keys.index("OWN_PROP_IRON_BAND"), keys.index("OWN_PROP_WAX")
iron_tile, wax_tile = cfg.get("iron_tile", 0.2), cfg.get("wax_tile", 0.15)
P = Parts(rng, 0.1)
seg = cfg.get("segments", 20)


def lathe(centre, profile, material, tile, part=None, close=True, wobble=None):
    """Revolve (r, z) profile points around the vertical axis through centre."""
    part = P.new_part() if part is None else part
    rings = []
    for k in range(seg):
        a = 2 * math.pi * k / seg
        rings.append([P.vert(centre + Vector(((r + (wobble(a, i) if wobble else 0)) * math.cos(a),
                                              (r + (wobble(a, i) if wobble else 0)) * math.sin(a), z)), part)
                      for i, (r, z) in enumerate(profile)])
    along = [0.0]
    for i in range(1, len(profile)):
        along.append(along[-1] + math.dist(profile[i - 1], profile[i]))
    for k in range(seg):
        k2 = (k + 1) % seg
        for i in range(len(profile) - 1):
            r = max(profile[i][0], 0.01)
            u0, u1 = k / seg * 2 * math.pi * r / tile, (k + 1) / seg * 2 * math.pi * r / tile
            v0, v1 = along[i] / tile, along[i + 1] / tile
            P.poly((rings[k][i], rings[k2][i], rings[k2][i + 1], rings[k][i + 1]), material,
                   ((u0, v0), (u1, v0), (u1, v1), (u0, v1)))
    return rings


# --- dish: its centre sits so that dish plus handle fill the footprint ---
r_dish, rim_h, wall = cfg.get("dish", [0.18, 0.032, 0.006])
n_feet, foot_h, foot_w = cfg.get("feet", [3, 0.025, 0.03])
r_ring, ring_w, ring_t = cfg.get("handle", [0.05, 0.012, 0.008])
cx = (lo.x + hi.x) / 2
cy = lo.y + r_dish + 0.004
base = Vector((cx, cy, lo.z + foot_h))
# profile from the axis under the floor, out, up the rim, over the lip and back in over the floor
dish_profile = [(0.0, 0.0), (r_dish - 0.01, 0.0), (r_dish, 0.006), (r_dish + 0.004, rim_h), (r_dish - wall, rim_h + 0.002),
                (r_dish - wall - 0.006, wall + 0.004), (r_dish - 0.03, wall), (0.0, wall)]
lathe(base, dish_profile, IRON, iron_tile)
for i in range(n_feet):
    a = 2 * math.pi * i / n_feet + math.pi / 2
    at = base + Vector((math.cos(a), math.sin(a), 0)) * (r_dish * 0.65)
    P.post(at + Z * 0.002, at - Z * foot_h, foot_w, foot_w * 0.8, X, Y, IRON, iron_tile)
# flat ring handle on the far side, joined to the rim by a short neck
ring_c = base + Y * (r_dish + r_ring + 0.012) + Z * (rim_h * 0.6)
P.board(base + Y * (r_dish + 0.008) + Z * (rim_h * 0.6), Y, X, Z, 0.03, 0.024, ring_t * 0.7, IRON)   # thinner: not coplanar with the ring
part = P.new_part()
rows = []
for k in range(seg):
    a = 2 * math.pi * k / seg
    radial = Vector((math.cos(a), math.sin(a), 0))
    rows.append([P.vert(ring_c + radial * r + Z * dz, part)
                 for r, dz in ((r_ring - ring_w, -ring_t / 2), (r_ring, -ring_t / 2), (r_ring, ring_t / 2), (r_ring - ring_w, ring_t / 2))])
for k in range(seg):
    k2 = (k + 1) % seg
    for i in range(4):
        i2 = (i + 1) % 4
        P.poly((rows[k][i], rows[k2][i], rows[k2][i2], rows[k][i2]), IRON, ((0, 0), (0.1, 0), (0.1, 0.05), (0, 0.05)))

# --- wax puddle on the floor ---
r_pud, t_pud = cfg.get("puddle", [0.095, 0.008])
floor = base + Z * wall
phase = rng.uniform(0, 6.28, 3)
def pud_wobble(a, i):
    return r_pud * 0.22 * (math.sin(3 * a + phase[0]) * 0.5 + math.sin(5 * a + phase[1]) * 0.3) if i < 2 else 0.0
lathe(floor, [(r_pud, 0.0), (r_pud * 0.9, t_pud), (0.0, t_pud)], WAX, wax_tile, wobble=pud_wobble)

# --- candle: a leaning cylinder whose surface swells into drips; uneven melted top, crater, wick ---
r_c, h_c, lean = cfg.get("candle", [0.06, 0.2, 4.0])
h_c = min(h_c, hi.z - floor.z - 0.004)
tilt = Matrix.Rotation(math.radians(lean), 3, "X")
csides, crings = cfg.get("candle_grid", [24, 9])
drips = [(rng.uniform(0, 2 * math.pi), rng.uniform(0.35, 0.95)) for _ in range(cfg.get("drips", 6))]  # angle, reach down


def swell(a, f):
    """Extra radius at angle a and height fraction f (0 bottom .. 1 top): drips hang from the top
    edge, fade in smoothly above their tip and end in a round drop."""
    extra = 0.0
    for da, reach in drips:
        d = math.atan2(math.sin(a - da), math.cos(a - da))
        tip = 1 - reach
        t = min(max((f - tip) / 0.08, 0.0), 1.0)
        body = t * t * (3 - 2 * t)
        drop = 0.9 * math.exp(-((f - tip - 0.035) / 0.04) ** 2)
        w0, w1 = cfg.get("drip_width", [0.13, 0.06])
        width = w0 + w1 * (1 - f)
        extra = max(extra, cfg.get("drip", 0.013) * (body + drop) * math.exp(-(d / width) ** 2))
    return extra


ph = rng.uniform(0, 2 * math.pi, 2)   # a wavy melted edge, not a sawtooth
top_drop = np.array([0.01 + 0.006 * math.sin(2 * a + ph[0]) + 0.004 * math.sin(5 * a + ph[1])
                     for a in np.linspace(0, 2 * math.pi, csides, endpoint=False)])
part = P.new_part()
rows = []
for k in range(csides):
    a = 2 * math.pi * k / csides
    radial = Vector((math.cos(a), math.sin(a), 0))
    col = [radial * (r_c + 0.012)]                              # foot flaring into the puddle
    for j in range(crings + 1):
        f = j / crings
        z = f * (h_c - top_drop[k])
        col.append(radial * (r_c + swell(a, f)) + Z * z)
    col.append(radial * (r_c * 0.6) + Z * (h_c - 0.014 - top_drop[k] * 0.5))   # melted rim folding in
    rows.append([P.vert(floor + tilt @ p, part) for p in col])
centre_top = P.vert(floor + tilt @ (Z * (h_c - 0.03)), part)
centre_bottom = P.vert(floor, part)   # a hidden cap under the foot: open, the body came out inside out in game
n = len(rows[0])
for k in range(csides):
    k2 = (k + 1) % csides
    P.poly((rows[k2][0], rows[k][0], centre_bottom), WAX, ((0, 0), (0.1, 0), (0.05, 0.1)))
    u0, u1 = k / csides * 2 * math.pi * r_c / wax_tile, (k + 1) / csides * 2 * math.pi * r_c / wax_tile
    for i in range(n - 1):
        v0, v1 = i * h_c / crings / wax_tile, (i + 1) * h_c / crings / wax_tile
        P.poly((rows[k][i], rows[k2][i], rows[k2][i + 1], rows[k][i + 1]), WAX, ((u0, v0), (u1, v0), (u1, v1), (u0, v1)))
    P.poly((rows[k][n - 1], rows[k2][n - 1], centre_top), WAX, ((0, 0), (0.1, 0), (0.05, 0.1)))
# drip rods: rounded ridges half sunk into the side, thin at the rim, ending in a fat drop
n_rods, rod_sides = cfg.get("drip_rods", [0, 14])
for d in range(n_rods):
    a = 2 * math.pi * (d + rng.uniform(-0.3, 0.3)) / max(n_rods, 1)
    radial = Vector((math.cos(a), math.sin(a), 0))
    side_v = Z.cross(radial)
    reach = rng.uniform(0.25, 0.85) * h_c
    top_z = h_c - 0.012
    steps = 6
    part = P.new_part()
    rings = []
    wob = rng.uniform(-0.6, 0.6)
    for j in range(steps + 1):
        f = j / steps
        z = top_z - f * reach
        rr = 0.005 + 0.004 * f + (0.006 * math.exp(-((f - 0.92) / 0.08) ** 2))
        c = radial * (r_c - rr * 0.35) + side_v * (0.006 * wob * math.sin(f * 3.0)) + Z * z
        rings.append([P.vert(floor + tilt @ (c + (radial * math.cos(2 * math.pi * k / rod_sides) + side_v * math.sin(2 * math.pi * k / rod_sides)) * rr), part)
                      for k in range(rod_sides)])
    tip_v = P.vert(floor + tilt @ (radial * (r_c - 0.003) + Z * (top_z - reach - 0.008)), part)
    for j in range(steps):
        for k in range(rod_sides):
            k2 = (k + 1) % rod_sides
            P.poly((rings[j][k2], rings[j][k], rings[j + 1][k], rings[j + 1][k2]), WAX, ((0, 0), (0.02, 0), (0.02, 0.02), (0, 0.02)))
    for k in range(rod_sides):
        P.poly((rings[steps][(k + 1) % rod_sides], rings[steps][k], tip_v), WAX, ((0, 0), (0.02, 0), (0.01, 0.02)))
    # cap the top: an open end showed the inside of the rod from above
    P.poly(rings[0], WAX, [(0.01 + 0.01 * math.cos(2 * math.pi * k / rod_sides), 0.01 + 0.01 * math.sin(2 * math.pi * k / rod_sides))
                           for k in range(rod_sides)])

# wick
P.post(floor + tilt @ (Z * (h_c - 0.008)), floor + tilt @ (Z * (h_c - 0.035)), 0.006, 0.007, X, Y, IRON, iron_tile)

P.finish(goal, out, lo, hi, {"candle_height": round(h_c, 3)})
