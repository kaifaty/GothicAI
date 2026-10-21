"""Build a pile of harbour crates (NW_HARBOUR_CRATEPILE_01) by parts and a numeric target.

blender -b --factory-startup -P crate_pile.py -- crate-pile.json OUT_DIR   (run from the repository root)
config: {"id": "crate-pile", "target": "data/content/mesh3d-market.json", "seed": 1,
         "crates": [{"at": [fx, fy], "size": [x, y, z], "on": index or null, "shift": [dx, dy],
                     "yaw": degrees, "tilt": degrees, "rope_ends": "both"|"one"|"none", "lid_offset": [dx, dy]}, ...],
         "crate": {...harbour_crate options...}, "stave_tile": 0.38, "planks_per_tile": 4}
The same harbour crates as crate.py (harbour_crate.py, the design of
reports/content/2026-10-crates-redesign), lighter: no nails, a coarser rope. "at" places a
crate's centre as fractions of the bbox footprint, "on" stands it on an earlier crate's
lid with "shift" in metres, "yaw" turns it about its own centre and "tilt" leans it about
its long axis onto one bottom edge, as on the concept where the upper crates sit askew.
The layout keeps the pile inside its bbox.
"""
import math
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
         "inner": keys.index("OWN_PROP_WOOD_INNER"), "rope": keys.index("OWN_PROP_ROPE"), "nail": None}
tile = cfg.get("stave_tile", 0.38)
P = Parts(rng, tile / cfg.get("planks_per_tile", 4))
options = {"nails": False, "rope": [0.014, 5, 7], **cfg.get("crate", {})}

tops = []
for spec in cfg["crates"]:
    L, W, H = spec["size"]
    yaw, tilt = math.radians(spec.get("yaw", 0.0)), math.radians(spec.get("tilt", 0.0))
    R = Matrix.Rotation(yaw, 3, "Z") @ Matrix.Rotation(tilt, 3, "X")
    if spec.get("on") is None:
        fx, fy = spec["at"]
        base = Vector((lo.x + fx * (hi.x - lo.x), lo.y + fy * (hi.y - lo.y), lo.z))
    else:
        below = tops[spec["on"]]
        dx, dy = spec.get("shift", [0.0, 0.0])
        base = Vector((below.x + dx, below.y + dy, below.z))
    base.z += abs(math.sin(tilt)) * W / 2      # leaning on one bottom edge, the other edge lifts
    own = {k: spec[k] for k in ("rope_ends", "lid_offset", "broken") if k in spec}
    tops.append(crate(P, zones, base, (L, W, H), R, {**options, **own}, rng, tile))

P.finish(goal, out, lo, hi, {"crates": len(cfg["crates"])})
