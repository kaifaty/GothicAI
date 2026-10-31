"""Build the tripod with a pot hanging over a campfire (OC_FIREPLACE_V5); the campfire under it is
NW_MISC_FIREPLACE_01, a separate visual.

blender -b --factory-startup -P tripod.py -- tripod.json OUT_DIR   (run from the repository root)
config: {"id": "hearth-tripod", "target": "data/content/mesh3d-hearth.json", "seed": 9,
         "feet": [[x, y, z], ...] glTF metres, "apex": [x, y, z], "over": m the poles run past the apex,
         "pole": radius, "ring": [x, y, z] where the three chains meet, "pot": {"centre": [x, z],
         "profile": [[y, r], ...] outer from the bottom to the rim, "wall": t, "liquid": y, "sides": n},
         "hooks": [deg, ...] where the chains hold the rim, "link": [length, wire, width]}
Asset city-hearth (intent.md, wave 1; form from the original's mesh: feet, apex, chain ring, pot
profile in glTF metres from its underlay). Three dark hewn poles cross at the apex and run on a
little past it, a short chain hangs from the crossing to a ring, three chains from the ring to the
rim of an eight-sided pot with flat faces and soft edges, water in it.
"""
import math
import sys
from pathlib import Path

import numpy as np
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parts import Parts, args, load_goal  # noqa: E402
from hearth_parts import chain, octagon, stick, turned  # noqa: E402

cfg, out = args()
goal, lo, hi = load_goal(cfg)
rng = np.random.default_rng(cfg.get("seed", 9))
keys = [z["key"] for z in goal["zones"]]
WOOD, IRON, RIVET, LIQUID = (keys.index(k) for k in ("OWN_HEARTH_WOOD", "OWN_HEARTH_IRON", "OWN_HEARTH_RIVET", "OWN_HEARTH_LIQUID"))
P = Parts(rng, 0.1)


def b(p):
    """glTF metres to Blender axes."""
    return Vector((p[0], -p[2], p[1]))


apex = b(cfg["apex"])
for foot in cfg["feet"]:
    f = b(foot)
    top = apex + (apex - f).normalized() * cfg["over"] * rng.uniform(0.8, 1.2)
    stick(P, rng, f, top, cfg["pole"] * rng.uniform(0.9, 1.1), WOOD, sides=6)
ring = b(cfg["ring"])
lk, lw, lwd = cfg["link"]
chain(P, apex - Vector((0, 0, cfg["pole"] * 1.5)), ring, IRON, lk, lw, lwd)
pot = cfg["pot"]
pc = b([pot["centre"][0], 0, pot["centre"][1]])
profile = [(y, r) for y, r in pot["profile"]]
y0 = profile[0][0]
outer = [(y - y0, r) for y, r in profile]
wall = pot["wall"]
top_h = outer[-1][0]
inner = [(top_h, outer[-1][1] - wall * 1.4)] + [(y - y0, r - wall) for y, r in reversed(profile[:-1]) if y >= pot["liquid"] - 0.02]
turned(P, Vector((pc.x, pc.y, y0)), outer, inner, pot["sides"], IRON, radius_at=lambda h, t, r: octagon(r, t))
# water: an open disc wound up by hand, the recalculation could turn it down
part = P.new_part(wound=True)
lr = [octagon(float(np.interp(pot["liquid"], [p[0] for p in profile], [p[1] for p in profile])) - wall - 0.002, 2 * math.pi * i / pot["sides"])
      for i in range(pot["sides"])]
verts = [P.vert(Vector((pc.x + r * math.cos(2 * math.pi * i / pot["sides"]), pc.y + r * math.sin(2 * math.pi * i / pot["sides"]),
                        pot["liquid"])), part) for i, r in enumerate(lr)]
centre = P.vert(Vector((pc.x, pc.y, pot["liquid"])), part)
for i in range(len(verts)):
    P.poly([centre, verts[i], verts[(i + 1) % len(verts)]], LIQUID, [(0, 0), (0.1, 0), (0, 0.1)])
rim_y = profile[-1][0]
rim_r = profile[-1][1]
for deg in cfg["hooks"]:
    t = math.radians(deg)
    hook = Vector((pc.x + octagon(rim_r, t) * math.cos(t), pc.y + octagon(rim_r, t) * math.sin(t), rim_y + 0.01))
    chain(P, ring, hook, IRON, lk, lw, lwd)
P.finish(goal, out, lo, hi, {"poles": len(cfg["feet"])})
