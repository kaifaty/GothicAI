"""Build the single harbour crate (NW_HARBOUR_CRATE_01) by parts and a numeric target.

blender -b --factory-startup -P crate.py -- crate.json OUT_DIR
config: {"id": "crate", "target": mesh3d-pilot.json (default: the repo's), "seed": 1,
         "post": [0.085, 0.035], "batten": [0.07, 0.025], "plank": 0.022, "gap": [0.004, 0.01],
         "plank_width": 0.19, "tilt_deg": 0.6, "proud": 0.002, "rope": [0.014, 6, 10], "rope_ends": "both",
         "nails": true, "nail_radius": 0.009, "nail": 0.004, "broken": true, "lid_shift": 0.015,
         "stave_tile": 0.38, "planks_per_tile": 4}
The design is the harbour crate of reports/content/2026-10-crates-redesign (skill
object-design, variant 2): corner posts and a rope handle on each short end, horizontal
planks with one upright batten on each long side, a plank lid, nails, no metal. The parts
come from harbour_crate.py, shared with crate_pile.py. The target gives size, origin and
budget: the crate stands on the bbox floor, long side along x, and its nail heads touch
the bbox. Every plank, post, batten and nail is its own closed part with a "_part" number,
so bake_atlas.py can tone each one; tile UVs are in metres with the grain along the part.
"""
import sys
from pathlib import Path

import numpy as np
from mathutils import Matrix, Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from harbour_crate import crate  # noqa: E402
from parts import Parts, args, load_goal  # noqa: E402

cfg, out = args()
goal, lo, hi = load_goal(cfg)
rng = np.random.default_rng(cfg.get("seed", 1))
keys = [z["key"] for z in goal["zones"]]
zones = {"plank": keys.index("OWN_PROP_WOOD_PLANK"), "brace": keys.index("OWN_PROP_WOOD_BRACE"),
         "inner": keys.index("OWN_PROP_WOOD_INNER"), "rope": keys.index("OWN_PROP_ROPE"),
         "nail": keys.index("OWN_PROP_IRON_NAIL")}
tile = cfg.get("stave_tile", 0.38)
P = Parts(rng, tile / cfg.get("planks_per_tile", 4))
nh = cfg.get("nail", 0.004) if cfg.get("nails", True) else 0.0
size = (hi.x - lo.x - 2 * nh, hi.y - lo.y - 2 * nh, hi.z - lo.z - nh)
base = Vector(((lo.x + hi.x) / 2, (lo.y + hi.y) / 2, lo.z))
crate(P, zones, base, size, Matrix.Identity(3), {"nails": True, **cfg}, rng, tile)
P.finish(goal, out, lo, hi)
