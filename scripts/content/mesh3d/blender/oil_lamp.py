"""Build a wall oil lamp (NW_CITY_OILLAMP_01) from an image_gen concept and a numeric target.

blender -b --factory-startup -P oil_lamp.py -- oil-lamp.json OUT_DIR   (run from the repository root)
config: {"id": "oil-lamp", "target": "data/content/mesh3d-market.json", "seed": 1, "segments": 16,
         "plate": [0.2, 0.3, 0.025], "arm": [0.024, 0.04], "arm_top": 0.035, "lamp_from_wall": 0.28,
         "reservoir": [0.085, 0.09], "glass": [0.098, 0.225], "metal_tile": 0.2, "glass_tile": 0.15, "wood_tile": 0.38}
A dark wooden back plate on the post side (the original's metal reaches that side
of its bbox), a forged arm out of it ending in a curl, a ring hanging from the curl
and under it the lamp: a squat brass reservoir, a collar and a tall bulb of milky
glass with a small brass cap, as on the concept.
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
BRASS, GLASS, WOOD = (keys.index(k) for k in ("OWN_PROP_BRASS", "OWN_PROP_GLASS", "OWN_PROP_WOOD_FRAME"))
metal_tile, glass_tile, wood_tile = cfg.get("metal_tile", 0.2), cfg.get("glass_tile", 0.15), cfg.get("wood_tile", 0.38)
P = Parts(rng, wood_tile / 4)
seg = cfg.get("segments", 16)


def lathe(centre, profile, material, tile):
    """Revolve (r, z) profile points around the vertical axis through centre; closed at r = 0 ends."""
    part = P.new_part()
    rings = [[P.vert(centre + Vector((r * math.cos(2 * math.pi * k / seg), r * math.sin(2 * math.pi * k / seg), z)), part)
              for r, z in profile] for k in range(seg)]
    along = [0.0]
    for i in range(1, len(profile)):
        along.append(along[-1] + math.dist(profile[i - 1], profile[i]))
    for k in range(seg):
        k2 = (k + 1) % seg
        for i in range(len(profile) - 1):
            r = max(profile[i][0], profile[i + 1][0], 0.005)
            u0, u1 = k / seg * 2 * math.pi * r / tile, (k + 1) / seg * 2 * math.pi * r / tile
            P.poly((rings[k][i], rings[k2][i], rings[k2][i + 1], rings[k][i + 1]), material,
                   ((u0, along[i] / tile), (u1, along[i] / tile), (u1, along[i + 1] / tile), (u0, along[i + 1] / tile)))


def tube(points, size, material, sides=4):
    """A square forged bar along a polyline."""
    part = P.new_part()
    rings, along = [], 0.0
    for k, p in enumerate(points):
        t = (points[min(k + 1, len(points) - 1)] - points[max(k - 1, 0)]).normalized()
        side = t.cross(Z) if abs(t.dot(Z)) < 0.95 else t.cross(X)
        side.normalize()
        up = side.cross(t)
        if k:
            along += (p - points[k - 1]).length
        rings.append(([P.vert(p + (side * math.cos(2 * math.pi * s / sides + math.pi / 4) + up * math.sin(2 * math.pi * s / sides + math.pi / 4)) * size, part)
                       for s in range(sides)], along))
    for k in range(len(rings) - 1):
        (a, va), (b, vb) = rings[k], rings[k + 1]
        for s in range(sides):
            s2 = (s + 1) % sides
            P.poly((a[s], a[s2], b[s2], b[s]), material, ((s * 0.02, va / metal_tile), ((s + 1) * 0.02, va / metal_tile),
                                                         ((s + 1) * 0.02, vb / metal_tile), (s * 0.02, vb / metal_tile)))
    for (ring, _), flip in ((rings[0], True), (rings[-1], False)):
        P.poly(ring[::-1] if flip else ring, material, [(0, 0), (0.02, 0), (0.02, 0.02), (0, 0.02)])


# back plate on the post side (Blender -Y here), its top a little above the arm
pw, ph, pt = cfg.get("plate", [0.2, 0.3, 0.025])
plate_c = Vector(((lo.x + hi.x) / 2, lo.y + pt / 2, hi.z - ph / 2))
P.wood(plate_c, Z, X, -Y, ph, pw, pt, WOOD, wood_tile)
for dz in (ph * 0.35, -ph * 0.35):   # two nail heads holding it
    P.nail(plate_c + Y * (pt / 2) + Z * dz, X, Z, Y, 0.009, 0.006, BRASS)

# forged arm: straight out, then a curl down; the lamp hangs from the curl
arm_size, curl = cfg.get("arm", [0.012, 0.035])
lamp_y = lo.y + cfg.get("lamp_from_wall", 0.27)
arm_z = hi.z - cfg.get("arm_top", 0.07)
start = Vector((plate_c.x, lo.y + pt, arm_z))
pts = [start + Y * (f * (lamp_y - start.y)) for f in np.linspace(0, 1, 5)]
end = pts[-1]
for a in np.linspace(math.pi / 2, -math.pi * 0.9, 9)[1:]:
    pts.append(end + Vector((0, curl * math.cos(a), curl * (math.sin(a) - 1))))
tube(pts, arm_size / 2, BRASS)
hang = Vector((plate_c.x, lamp_y, arm_z - 2 * curl))

# lamp: reservoir at the bottom of the bbox, glass bulb above, cap and ring up to the curl
r_res, h_res = cfg.get("reservoir", [0.065, 0.08])
r_gl, h_gl = cfg.get("glass", [0.062, 0.2])
bottom = Vector((plate_c.x, lamp_y, lo.z))
lathe(bottom, [(0.0, 0.0), (r_res * 0.55, 0.0), (r_res * 0.95, h_res * 0.25), (r_res, h_res * 0.5), (r_res * 0.8, h_res * 0.85),
               (r_res * 0.55, h_res), (r_res * 0.5, h_res + 0.012), (0.0, h_res + 0.012)], BRASS, metal_tile)
gl = bottom + Z * (h_res + 0.01)
pear = [(0.0, 0.0), (0.55, 0.0), (0.85, 0.06), (0.98, 0.18), (1.0, 0.3), (0.94, 0.42), (0.8, 0.53), (0.62, 0.64),
        (0.48, 0.76), (0.4, 0.88), (0.37, 1.0)]
lathe(gl, [(r_gl * r, h_gl * z) for r, z in pear] + [(0.0, h_gl)], GLASS, glass_tile)
cap = gl + Z * h_gl
lathe(cap, [(0.0, -0.004), (r_gl * 0.42, -0.004), (r_gl * 0.4, 0.014), (0.0, 0.02)], BRASS, metal_tile)
# hanging rod from the cap up to the curl
tube([cap + Z * 0.02, hang], 0.004, BRASS)

P.finish(goal, out, lo, hi, {})
