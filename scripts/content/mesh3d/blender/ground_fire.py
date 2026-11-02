"""Build the ground fire under the cauldron (FIREPLACE_GROUND): a ring of fieldstones round an ember bed.

blender -b --factory-startup -P ground_fire.py -- ground_fire.json OUT_DIR   (run from the repository root)
config: {"id": "hearth-ground-fire", "target": "data/content/mesh3d-hearth.json", "seed": 5,
         "stones": n, "ring": [r_min, r_max], "stone": [[x0, x1], [y0, y1], [z0, z1]] sizes (m),
         "bed": [radius, height], "lumps": [embers, coals], "ash": [r0, r1],
         "logs": {"gaps": [i, ...], "widen": share, "radius": [r0, r1], "inner": r, "outer": r, "char": [m0, m1],
                  "glow": [m0, m1]}, "brands": [n, length, radius]}
Asset city-hearth (intent.md, wave 1, form from the original's mesh). The original is a ring of nine
angular stones about 0.9 m across, a low glowing cone in the middle and charred sticks between the
stones. Here: n broken fieldstones at their own sizes round the ring, a low ember bed with lumps of
glowing embers and dark coals on it, a ragged ash ring under the stones and charred sticks lying
inward with their burnt ends in the bed. The cauldron stands on it (CAULDRON_OC, same place).
Composition of 2026-11-02 (reports/content/remaster/city-hearth/fire-composition.md, concept-attempt-3): logs
fed in a star through gaps in the ring that are "widen" times wider than the others; each burns only at its
inner end (zone GLOW: charcoal with heat in the cracks, emissive), then CHAR, then bark outside the ring; two
burnt-down brands lie on the bed.
"""
import math
import sys
from pathlib import Path

import numpy as np
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parts import Parts, args, load_goal  # noqa: E402
from hearth_parts import ash, burning_log, dome, lump, stone  # noqa: E402

cfg, out = args()
goal, lo, hi = load_goal(cfg)
rng = np.random.default_rng(cfg.get("seed", 5))
keys = [z["key"] for z in goal["zones"]]
STONE, EMBER, ASH, LOG, CHAR, GLOW = (keys.index(k) for k in ("OWN_HEARTH_STONE", "OWN_HEARTH_EMBER", "OWN_HEARTH_ASH",
                                                               "OWN_HEARTH_LOG", "OWN_HEARTH_CHAR", "OWN_HEARTH_GLOW"))
P = Parts(rng, 0.1)
c = Vector(((lo.x + hi.x) / 2, (lo.y + hi.y) / 2, 0.0))

n = cfg["stones"]
r_min, r_max = cfg["ring"]
(sx0, sx1), (sy0, sy1), (sz0, sz1) = cfg["stone"]
logs = cfg["logs"]
start = rng.uniform(0, 2 * math.pi)
# intervals between stone k and k+1: the gaps a log goes through are wider
weights = np.array([logs["widen"] if k in logs["gaps"] else 1.0 for k in range(n)])
bounds = np.concatenate([[0.0], np.cumsum(weights)]) / weights.sum() * 2 * math.pi
angles = []
for k in range(n):
    a = start + bounds[k] + rng.uniform(-0.1, 0.1) * 2 * math.pi / n
    angles.append(a)
    r = rng.uniform(r_min, r_max)
    size = (rng.uniform(sx0, sx1), rng.uniform(sy0, sy1), rng.uniform(sz0, sz1))
    stone(P, rng, c + Vector((r * math.cos(a), r * math.sin(a), 0)), size, STONE)

bed_r, bed_h = cfg["bed"]
dome(P, rng, c, bed_r, bed_h, EMBER)
embers, coals = cfg["lumps"]
for k in range(embers + coals):
    a, r = rng.uniform(0, 2 * math.pi), bed_r * math.sqrt(rng.uniform(0, 0.85))
    h = bed_h * (1 - (r / bed_r) ** 2) - 0.008
    lump(P, rng, c + Vector((r * math.cos(a), r * math.sin(a), h)), rng.uniform(0.02, 0.045), EMBER if k < embers else ASH)
a0, a1 = cfg["ash"]
ash(P, rng, c, a0, a1, ASH)
tips = []  # where each log burns, for the flame tongues of the vob layer (Blender metres, Z up)
for k in logs["gaps"]:
    a = (angles[k] + angles[(k + 1) % n] + (2 * math.pi if k == n - 1 else 0)) / 2 + rng.uniform(-0.06, 0.06)
    radius = rng.uniform(*logs["radius"])
    out_r, in_r = logs["outer"], logs["inner"] + rng.uniform(-0.02, 0.03)
    yaw = rng.uniform(-0.12, 0.12)  # not quite radial
    tail = c + Vector((out_r * math.cos(a), out_r * math.sin(a), radius * 0.9))
    in_h = bed_h * max(0.0, 1 - (in_r / bed_r) ** 2)
    tip = c + Vector((in_r * math.cos(a + yaw), in_r * math.sin(a + yaw), in_h + radius * 0.7))
    char_len = rng.uniform(*logs["char"])
    glow_len = rng.uniform(*logs["glow"])
    burning_log(P, rng, tail, tip, radius, LOG, CHAR, GLOW, char_len, glow_len)
    mid = tip + (tail - tip).normalized() * glow_len * 0.5
    tips.append({"tip": [round(v, 4) for v in tip], "glow_mid": [round(v, 4) for v in mid],
                 "angle_deg": round(math.degrees(a) % 360, 1), "centre": [round(v, 4) for v in c]})
count, length, radius = cfg["brands"]
for k in range(count):  # burnt-down ends of earlier logs lying on the bed
    a = rng.uniform(0, 2 * math.pi)
    p0 = c + Vector((bed_r * 0.15 * math.cos(a), bed_r * 0.15 * math.sin(a), bed_h * 0.8))
    b = a + rng.uniform(1.8, 2.6)
    p1 = c + Vector((bed_r * 0.75 * math.cos(b), bed_r * 0.75 * math.sin(b), bed_h * 0.35))
    burning_log(P, rng, p1, p0, radius * rng.uniform(0.8, 1.1), CHAR, CHAR, GLOW, length * 0.3, length * 0.45, sides=6)

metrics = P.finish(goal, out, lo, hi, {"stones": n, "logs": len(logs["gaps"]), "log_tips": tips})
