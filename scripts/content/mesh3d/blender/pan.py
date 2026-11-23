"""Build the frying pan on its stand over a fire ring (PAN_OC): a model with two attachments.

blender -b --factory-startup -P pan.py -- pan.json OUT_DIR   (run from the repository root)
config: {"id": "hearth-pan", "target": "data/content/mesh3d-hearth.json", "seed": 7,
         "pan": {"centre": [x, y], "floor": h, "radius": r, "rim": h, "flare": dr, "wall": t, "sides": n},
         "handle": {"dir_deg": a, "tail": [length, w, h], "grip": [length, radius], "rise": dz},
         "stand": {"feet": [[x, y], ...], "top": h, "radius": r},
         "stones": {"n": n, "ring": [r0, r1], "size": [[x0, x1], [y0, y1], [z0, z1]]},
         "bed": [radius, height], "lumps": [embers, coals], "ash": [r0, r1]}
Asset city-hearth (intent.md, wave 1; form from the original's mesh). The original's attachments keep
their names: "BIP01 PAN" is the pan with its short iron tail and long wooden grip, "BIP01 COOK" the
fire under it: three dark wooden sticks leaning together to a point under the pan, a low glowing bed,
a ring of a dozen small sooted stones and ash. Positions in Blender axes (x, -glTF z) round the bbox centre.
"""
import math
import sys
from pathlib import Path

import numpy as np
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parts import Parts, args, finish_nodes, load_goal  # noqa: E402
from hearth_parts import ash, bar, dome, lump, stick, stone, turned  # noqa: E402

cfg, out = args()
goal, lo, hi = load_goal(cfg)
rng = np.random.default_rng(cfg.get("seed", 7))
keys = [z["key"] for z in goal["zones"]]
IRON, WOOD, STONE, EMBER, ASH = (keys.index(k) for k in
                                 ("OWN_HEARTH_IRON", "OWN_HEARTH_WOOD", "OWN_HEARTH_STONE", "OWN_HEARTH_EMBER", "OWN_HEARTH_ASH"))
pan, cook = Parts(rng, 0.1), Parts(rng, 0.1)

# --- BIP01 PAN: the pan, its tail and grip -----------------------------------------------------------
p = cfg["pan"]
pc = Vector((p["centre"][0], p["centre"][1], 0))
r, f, t, rim, floor = p["radius"], p["flare"], p["wall"], p["rim"], p["floor"]
outer = [(floor, r - f - 0.01), (floor + 0.006, r - f), (rim, r), (rim + 0.004, r + 0.002)]
inner = [(rim + 0.004, r - t), (rim - 0.004, r - t - 0.002), (floor + 0.012, r - f - t)]
turned(pan, pc, outer, inner, p["sides"], IRON)
hd = cfg["handle"]
a = math.radians(hd["dir_deg"])
d = Vector((math.cos(a), math.sin(a), 0))
tl, tw, th = hd["tail"]
start = pc + d * (r - 0.01) + Vector((0, 0, rim - 0.012))
mid = start + d * tl + Vector((0, 0, hd["rise"] * tl))
bar(pan, start, mid, tw, th, IRON)
gl, gr = hd["grip"]
end = mid + d * gl + Vector((0, 0, hd["rise"] * gl))
stick(pan, rng, mid - d * 0.02, end, gr, WOOD, sides=8)

# --- BIP01 COOK: stand, bed, stones, ash ----------------------------------------------------------------
c = Vector((0, 0, 0))
s = cfg["stand"]
apex = Vector((pc.x, pc.y, s["top"]))
for fx, fy in s["feet"]:
    foot = Vector((fx, fy, 0.0))
    stick(cook, rng, foot, apex + (foot - apex).normalized() * 0.02, s["radius"] * rng.uniform(0.9, 1.1), WOOD,
          burnt=(ASH, 0.0) if False else None, sides=5)
bed_r, bed_h = cfg["bed"]
dome(cook, rng, c, bed_r, bed_h, EMBER, sides=14, rings=3)
embers, coals = cfg["lumps"]
for k in range(embers + coals):
    aa, rr = rng.uniform(0, 2 * math.pi), bed_r * math.sqrt(rng.uniform(0, 0.8))
    h = bed_h * (1 - (rr / bed_r) ** 2) - 0.006
    lump(cook, rng, c + Vector((rr * math.cos(aa), rr * math.sin(aa), h)), rng.uniform(0.015, 0.035), EMBER if k < embers else ASH)
st = cfg["stones"]
(sx0, sx1), (sy0, sy1), (sz0, sz1) = st["size"]
base = rng.uniform(0, 2 * math.pi)
for k in range(st["n"]):
    aa = base + 2 * math.pi * (k + rng.uniform(-0.25, 0.25)) / st["n"]
    rr = rng.uniform(*st["ring"])
    stone(cook, rng, c + Vector((rr * math.cos(aa), rr * math.sin(aa), 0)),
          (rng.uniform(sx0, sx1), rng.uniform(sy0, sy1), rng.uniform(sz0, sz1)), STONE, sink=0.01, points=16)
a0, a1 = cfg["ash"]
ash(cook, rng, c, a0, a1, ASH, sides=22)

# the model's origin: the original's geometry sits round the bbox centre (its root translation moves it 0.8 m)
shift = Vector(((lo.x + hi.x) / 2, (lo.y + hi.y) / 2, 0))
for parts in (pan, cook):
    for v in parts.bm.verts:
        v.co += shift
# part numbers must not repeat across the two nodes
offset = pan.parts
for v in cook.bm.verts:
    v[cook.part_layer] += offset
cook.parts += offset
finish_nodes(goal, out, lo, hi, [("BIP01 PAN", pan), ("BIP01 COOK", cook)])
