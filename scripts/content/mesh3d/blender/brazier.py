"""Build the fire basket with a pot on it (DT_FIREPLACE_V1).

blender -b --factory-startup -P brazier.py -- brazier.json OUT_DIR   (run from the repository root)
config: {"id": "hearth-brazier", "target": "data/content/mesh3d-hearth.json", "seed": 11,
         "bowl": {"profile": [[y, r], ...], "wall": t, "bed": y, "sides": n},
         "pot": {"profile": [[y, r], ...], "wall": t, "floor": y, "sides": n},
         "angles": [deg, ...] of the feet and the brackets, "foot": [[r, y] top, [r, y] tip, width, thick],
         "bracket": [[r, y] bottom, [r, y] top, width, thick], "lumps": [embers, coals], "sticks": n}
Asset city-hearth (intent.md, wave 1; form from the original's mesh, y in glTF metres). A deep forged
bowl on four short feet holds embers and charred sticks; four upright brackets from its rim carry an
eight-sided pot-bellied pot, its bottom a hand above the rim so the glow shows in the gap.
"""
import math
import sys
from pathlib import Path

import numpy as np
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parts import Parts, args, load_goal  # noqa: E402
from hearth_parts import bar, dome, lump, octagon, stick, turned  # noqa: E402

cfg, out = args()
goal, lo, hi = load_goal(cfg)
rng = np.random.default_rng(cfg.get("seed", 11))
keys = [z["key"] for z in goal["zones"]]
IRON, RIVET, EMBER, ASH = (keys.index(k) for k in ("OWN_HEARTH_IRON", "OWN_HEARTH_RIVET", "OWN_HEARTH_EMBER", "OWN_HEARTH_ASH"))
P = Parts(rng, 0.1)
c = Vector(((lo.x + hi.x) / 2, (lo.y + hi.y) / 2, 0))


def vessel(spec, bend=None, inner_to=None):
    prof = spec["profile"]
    y0 = prof[0][0]
    outer = [(y - y0, r) for y, r in prof]
    w = spec["wall"]
    inner = [(outer[-1][0], prof[-1][1] - w * 1.3)] + [(y - y0, r - w) for y, r in reversed(prof[:-1]) if y >= inner_to]
    return turned(P, Vector((c.x, c.y, y0)), outer, inner, spec["sides"], IRON, radius_at=bend)


bowl = cfg["bowl"]
vessel(bowl, inner_to=bowl["bed"] - 0.02)
pot = cfg["pot"]
vessel(pot, bend=lambda h, t, r: octagon(r, t, 0.08), inner_to=pot["floor"])  # flat faces, only the edges softened
(fr0, fy0), (fr1, fy1), fw, ft = cfg["foot"]
(br0, by0), (br1, by1), bw, bt = cfg["bracket"]
for deg in cfg["angles"]:
    t = math.radians(deg)
    d = Vector((math.cos(t), math.sin(t), 0))
    bar(P, c + d * fr0 + Vector((0, 0, fy0)), c + d * fr1 + Vector((0, 0, fy1)), fw, ft, IRON)
    bar(P, c + d * br0 + Vector((0, 0, by0)), c + d * br1 + Vector((0, 0, by1)), bw, bt, RIVET)
bed_r = float(np.interp(bowl["bed"], [p[0] for p in bowl["profile"]], [p[1] for p in bowl["profile"]])) - bowl["wall"] - 0.01
bed = c + Vector((0, 0, bowl["bed"]))
dome(P, rng, bed, bed_r, 0.05, EMBER, sides=16, rings=3)
embers, coals = cfg["lumps"]
for k in range(embers + coals):
    a, r = rng.uniform(0, 2 * math.pi), bed_r * math.sqrt(rng.uniform(0, 0.85))
    lump(P, rng, bed + Vector((r * math.cos(a), r * math.sin(a), 0.05 * (1 - (r / bed_r) ** 2) - 0.005)),
         rng.uniform(0.015, 0.035), EMBER if k < embers else ASH)
for k in range(cfg["sticks"]):
    a = rng.uniform(0, 2 * math.pi)
    p0 = bed + Vector((math.cos(a) * bed_r * 0.8, math.sin(a) * bed_r * 0.8, 0.02))
    p1 = bed + Vector((math.cos(a + 2.4) * bed_r * 0.5, math.sin(a + 2.4) * bed_r * 0.5, 0.06))
    stick(P, rng, p0, p1, 0.02, ASH, sides=5)
P.finish(goal, out, lo, hi, {"angles": cfg["angles"]})
