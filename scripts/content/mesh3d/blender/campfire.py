"""Build the campfire (NW_MISC_FIREPLACE_01): a ring of big stones round a tent of split logs on embers.

blender -b --factory-startup -P campfire.py -- campfire.json OUT_DIR   (run from the repository root)
config: {"id": "hearth-campfire", "target": "data/content/mesh3d-hearth.json", "seed": 13, "ground": y,
         "stones": n, "ring": [r0, r1], "stone": [[x0, x1], [y0, y1], [z0, z1]],
         "logs": n, "log": [radius0, radius1], "foot": [r0, r1], "top": [r, h], "burnt": share,
         "bed": [radius, height], "lumps": [embers, coals], "ash": [r0, r1]}
Asset city-hearth (intent.md, wave 1; form from the original's mesh). The original: fifteen big grey
stones round a ring 2 m across, up to 0.4 m high, a tent of six or seven barked logs to 0.8 m and a
mound of red embers under them, 0.4 m high in the middle (its edge is the ground, "ground" glTF y). The logs lean in from their feet on the
embers to the top, crossing there; their lower ends are charred. The tripod (OC_FIREPLACE_V5) stands
over it in some camps.
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
rng = np.random.default_rng(cfg.get("seed", 13))
keys = [z["key"] for z in goal["zones"]]
STONE, LOG, EMBER, ASH = (keys.index(k) for k in ("OWN_HEARTH_STONE", "OWN_HEARTH_LOG", "OWN_HEARTH_EMBER", "OWN_HEARTH_ASH"))
P = Parts(rng, 0.1)
c = Vector(((lo.x + hi.x) / 2, (lo.y + hi.y) / 2, cfg["ground"]))
n = cfg["stones"]
(sx0, sx1), (sy0, sy1), (sz0, sz1) = cfg["stone"]
start = rng.uniform(0, 2 * math.pi)
for k in range(n):
    a = start + 2 * math.pi * (k + rng.uniform(-0.2, 0.2)) / n
    r = rng.uniform(*cfg["ring"])
    stone(P, rng, c + Vector((r * math.cos(a), r * math.sin(a), 0)),
          (rng.uniform(sx0, sx1), rng.uniform(sy0, sy1), rng.uniform(sz0, sz1)), STONE, sink=0.03, points=24)
bed_r, bed_h = cfg["bed"]
dome(P, rng, c, bed_r, bed_h, EMBER, sides=18, rings=4)
embers, coals = cfg["lumps"]
for k in range(embers + coals):
    a, r = rng.uniform(0, 2 * math.pi), bed_r * math.sqrt(rng.uniform(0, 0.9))
    lump(P, rng, c + Vector((r * math.cos(a), r * math.sin(a), bed_h * (1 - (r / bed_r) ** 2) - 0.006)),
         rng.uniform(0.02, 0.05), EMBER if k < embers else ASH)
ash(P, rng, c, *cfg["ash"], ASH, sides=32)
m = cfg["logs"]
top_r, top_h = cfg["top"]
r0, r1 = cfg["log"]
lean = rng.uniform(0, 2 * math.pi)
for k in range(m):
    a = lean + 2 * math.pi * (k + rng.uniform(-0.25, 0.25)) / m
    fr = rng.uniform(*cfg["foot"])
    foot = c + Vector((math.cos(a) * fr, math.sin(a) * fr, bed_h * max(1 - (fr / bed_r) ** 2, 0) + 0.03))  # on the ember mound
    b = a + math.pi + rng.uniform(-0.6, 0.6)
    top = c + Vector((math.cos(b) * top_r * rng.uniform(0.3, 1.0), math.sin(b) * top_r * rng.uniform(0.3, 1.0),
                      top_h * rng.uniform(0.8, 1.0)))
    top = top + (top - foot).normalized() * 0.12  # the log runs on past the crossing
    stick(P, rng, top, foot, rng.uniform(r0, r1), LOG, burnt=(ASH, cfg["burnt"]), sides=7)
P.finish(goal, out, lo, hi, {"stones": n, "logs": m})
