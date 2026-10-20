"""Build a stack of lying barrels (NW_HARBOUR_BARRELGROUP_01) by parts and a numeric target.

blender -b --factory-startup -P barrel_group.py -- barrel-group.json OUT_DIR   (run from the repository root)
config: {"id": "barrel-group", "target": "data/content/mesh3d-market.json", "seed": 1, "rows": [5, 4, 3],
         "sides": 12, "length": 1.05, "end_ratio": 0.8, "hoops": [0.078, 0.27, 0.66, 0.895], "hoop_width": 0.063,
         "hoop_height": 0.008, "rim": 0.022, "recess": 0.03, "stagger": 0.15, "chock": [0.2, 0.12, 0.08],
         "stave_tile": 0.38, "planks_per_tile": 4}
A pyramid of barrels lying front to back, each row resting in the grooves of the
row below; the diameter is solved so the pyramid fills the bbox height and the
bottom row its width. A low-poly cousin of the pilot barrel (barrel.py): the same
four hoops and recessed head, but 12 staves and hoops raised from the stave tube
itself. Every stave is its own part, so the atlas tones staves one by one; wedges
under the bottom row keep it from rolling.
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
STAVE, IRON, FRAME = (keys.index(k) for k in ("OWN_PROP_WOOD_STAVE", "OWN_PROP_IRON_BAND", "OWN_PROP_WOOD_FRAME"))
tile = cfg.get("stave_tile", 0.38)
P = Parts(rng, tile / cfg.get("planks_per_tile", 4))
plank_u = P.plank_u

rows = cfg.get("rows", [5, 4, 3])
sides = cfg.get("sides", 12)
length = cfg.get("length", 1.05)
end_ratio = cfg.get("end_ratio", 0.8)
hoop_h = cfg.get("hoop_height", 0.008)
width, height = hi.x - lo.x, hi.z - lo.z


def pyramid_height(d):
    spacing = (width - d) / (rows[0] - 1)
    rise = math.sqrt(max(d * d - (spacing / 2) ** 2, 0.0))
    return d + rise * (len(rows) - 1)


# Belly diameter (hoops included) that makes the pyramid exactly as tall as the bbox.
d_lo, d_hi = 0.3, width / rows[0]
for _ in range(60):
    d = (d_lo + d_hi) / 2
    d_lo, d_hi = (d, d_hi) if pyramid_height(d) < height else (d_lo, d)
diameter = d
spacing = (width - diameter) / (rows[0] - 1)
rise = math.sqrt(diameter * diameter - (spacing / 2) ** 2)
r_belly = diameter / 2 - hoop_h
r_end = r_belly * end_ratio


def radius(s):
    """Stave surface radius at axial position s in [-length/2, length/2]."""
    t = 2 * s / length
    return r_end + (r_belly - r_end) * (1 - t * t)


# Axial profile: (s, r, material of the band that ends here).
rim, recess = cfg.get("rim", 0.022), cfg.get("recess", 0.03)
half = length / 2
profile = [(-half + recess, r_end - rim, None), (-half, r_end, STAVE)]
hw = cfg.get("hoop_width", 0.063) * length
for f in cfg.get("hoops", [0.078, 0.27, 0.66, 0.895]):
    s0, s1 = -half + f * length - hw / 2, -half + f * length + hw / 2
    profile += [(s0, radius(s0), STAVE), (s0, radius(s0) + hoop_h, IRON), (s1, radius(s1) + hoop_h, IRON),
                (s1, radius(s1), IRON)]
profile += [(half, r_end, STAVE), (half - recess, r_end - rim, STAVE)]


def barrel(centre, roll, yaw):
    """One lying barrel along Y through centre: staves as separate parts, iron bands, two heads."""
    R = Matrix.Rotation(yaw, 3, "Z") @ Matrix.Rotation(-math.pi / 2, 3, "X") @ Matrix.Rotation(roll, 3, "Z")
    # local frame: axis along local Z (becomes world Y), angle around it
    def at(s, r, a):
        return centre + R @ Vector((r * math.cos(a), r * math.sin(a), s))
    step = 2 * math.pi / sides
    for k in range(sides):
        part = P.new_part()
        plank = int(rng.integers(0, 4))
        v_off = rng.uniform(0, tile)
        a0, a1 = k * step, (k + 1) * step
        rings = [(P.vert(at(s, r, a0), part), P.vert(at(s, r, a1), part)) for s, r, _ in profile]
        along = 0.0
        for i in range(1, len(profile)):
            (s_prev, r_prev, _), (s, r, mat) = profile[i - 1], profile[i]
            seg = math.hypot(s - s_prev, r - r_prev)
            if seg < 1e-6:
                continue
            u0, u1 = plank * plank_u, (plank + 1) * plank_u
            v0, v1 = v_off + along, v_off + along + seg
            P.poly((rings[i - 1][0], rings[i - 1][1], rings[i][1], rings[i][0]), mat,   # outward
                   ((u0, v0), (u1, v0), (u1, v1), (u0, v1)))
            along += seg
    for s_head, sign in ((-half + recess, -1), (half - recess, 1)):   # each head faces out of its end
        part = P.new_part()
        r = r_end - rim
        verts = [P.vert(at(s_head, r, k * step), part) for k in range(sides)]
        if sign < 0:
            verts = verts[::-1]
        ou, ov = rng.uniform(0, tile), rng.uniform(0, tile)
        P.poly(verts, FRAME, [(ou + r * math.cos(k * step), ov + r * math.sin(k * step)) for k in range(sides)][::sign])


stagger = cfg.get("stagger", 0.15)
y_mid = (lo.y + hi.y) / 2
room = max((hi.y - lo.y - length) / 2, 0.0)
for level, count in enumerate(rows):
    z = lo.z + diameter / 2 + level * rise
    x0 = lo.x + diameter / 2 + level * spacing / 2
    for i in range(count):
        offset = float(np.clip(rng.normal(0, stagger / 2), -room, room))
        barrel(Vector((x0 + i * spacing, y_mid + offset, z)), rng.uniform(0, 2 * math.pi), rng.uniform(-0.03, 0.03))

# Wedges against the outer side of the two end barrels of the bottom row, front and back.
cw, ch, cd = cfg.get("chock", [0.2, 0.12, 0.08])
r_out = diameter / 2
reach = math.sqrt(max(r_out * r_out - (r_out - ch) ** 2, 0.0))   # where the barrel side is ch above the ground
for x, sx in ((lo.x + r_out - reach - cd / 2, -1), (hi.x - r_out + reach + cd / 2, 1)):
    for sy in (-1, 1):
        P.wood(Vector((x, y_mid + sy * length * 0.3, lo.z + ch / 2)), Y, Z, X * sx, cw, ch, cd, FRAME, tile)

P.finish(goal, out, lo, hi, {"barrels": sum(rows), "diameter": round(diameter, 3), "spacing": round(spacing, 3)})
