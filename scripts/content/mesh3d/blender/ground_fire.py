"""Build the ground fire under the cauldron (FIREPLACE_GROUND): a ring of fieldstones round an ember bed.

blender -b --factory-startup -P ground_fire.py -- ground_fire.json OUT_DIR   (run from the repository root)
config: {"id": "hearth-ground-fire", "target": "data/content/mesh3d-hearth.json", "seed": 5,
         "stones": n, "ring": [r_min, r_max], "stone": [[x0, x1], [y0, y1], [z0, z1]] sizes (m),
         "bed": [radius, height], "lumps": [embers, coals], "ash": [r0, r1], "sticks": [n, length, radius]}
Asset city-hearth (intent.md, wave 1, form from the original's mesh). The original is a ring of nine
angular stones about 0.9 m across, a low glowing cone in the middle and charred sticks between the
stones. Here: n broken fieldstones at their own sizes round the ring, a low ember bed with lumps of
glowing embers and dark coals on it, a ragged ash ring under the stones and charred sticks lying
inward with their burnt ends in the bed. The cauldron stands on it (CAULDRON_OC, same place).
"""
import math
import sys
from pathlib import Path

import numpy as np
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parts import Parts, args, load_goal  # noqa: E402
from hearth_parts import ash, dome, lump, stick, stone  # noqa: E402

cfg, out = args()
goal, lo, hi = load_goal(cfg)
rng = np.random.default_rng(cfg.get("seed", 5))
keys = [z["key"] for z in goal["zones"]]
STONE, EMBER, ASH, LOG = (keys.index(k) for k in ("OWN_HEARTH_STONE", "OWN_HEARTH_EMBER", "OWN_HEARTH_ASH", "OWN_HEARTH_LOG"))
P = Parts(rng, 0.1)
c = Vector(((lo.x + hi.x) / 2, (lo.y + hi.y) / 2, 0.0))

n = cfg["stones"]
r_min, r_max = cfg["ring"]
(sx0, sx1), (sy0, sy1), (sz0, sz1) = cfg["stone"]
start = rng.uniform(0, 2 * math.pi)
for k in range(n):
    a = start + 2 * math.pi * (k + rng.uniform(-0.22, 0.22)) / n
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
count, length, radius = cfg["sticks"]
for k in range(count):
    a = 2 * math.pi * (k + 0.5 + rng.uniform(-0.3, 0.3)) / count + start
    out_r, in_r = bed_r + length * 0.55, bed_r + length * 0.55 - length
    tail = c + Vector((out_r * math.cos(a), out_r * math.sin(a), radius * 0.8))
    tip = c + Vector((in_r * math.cos(a + 0.2), in_r * math.sin(a + 0.2), bed_h * 0.6))
    stick(P, rng, tail, tip, radius * rng.uniform(0.8, 1.2), LOG, burnt=(ASH, 0.45))

metrics = P.finish(goal, out, lo, hi, {"stones": n})
