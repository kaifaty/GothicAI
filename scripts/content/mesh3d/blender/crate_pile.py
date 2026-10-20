"""Build a pile of framed crates (NW_HARBOUR_CRATEPILE_01) by parts and a numeric target.

blender -b --factory-startup -P crate_pile.py -- crate-pile.json OUT_DIR   (run from the repository root)
config: {"id": "crate-pile", "target": "data/content/mesh3d-market.json", "seed": 1,
         "crates": [{"at": [fx, fy], "size": [x, y, z], "on": index or null, "yaw": degrees, "planks": n}, ...],
         "frame": 0.08, "batten": 0.022, "raise": 0.01, "plank": 0.018, "gap": 0.006, "width_jitter": 0.12,
         "plate_leg": 0.1, "plate_width": 0.05, "plate": 0.004, "stave_tile": 0.38, "planks_per_tile": 4}
A low-poly cousin of the pilot crate (crate.py), so the pile reads as the same
harbour crates: every visible face is a frame of four battens around recessed
planks over a dark core, with L plates on the four top corners; no nails, no
bottom. "at" places a crate's centre as fractions of the bbox footprint, "on"
stacks it on an earlier crate, "yaw" turns it about its own centre. The layout
is read from in-game shots of the original; sizes keep the pile inside its bbox.
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
PLANK, BRACE, IRON = (keys.index(k) for k in ("OWN_PROP_WOOD_PLANK", "OWN_PROP_WOOD_BRACE", "OWN_PROP_IRON_BAND"))
tile = cfg.get("stave_tile", 0.38)
P = Parts(rng, tile / cfg.get("planks_per_tile", 4))
fw, bt = cfg.get("frame", 0.08), cfg.get("batten", 0.022)
raise_, pt, gap = cfg.get("raise", 0.01), cfg.get("plank", 0.018), cfg.get("gap", 0.006)
jitter = cfg.get("width_jitter", 0.12)
plate_leg, plate_w, plate_t = cfg.get("plate_leg", 0.1), cfg.get("plate_width", 0.05), cfg.get("plate", 0.004)


def crate(base, size, yaw, planks):
    """A crate standing on base (centre of its bottom), size (x, y, z) before turning by yaw."""
    R = Matrix.Rotation(math.radians(yaw), 3, "Z")
    ax, ay = R @ X, R @ Y
    hx, hy, hz = size[0] / 2, size[1] / 2, size[2] / 2
    centre = base + Z * hz
    top = hz - plate_t                     # battens of the top face end under the plates

    def at(a, b, c):
        return centre + ax * a + ay * b + Z * c

    # Faces: outward normal (local), in-plane axes (local), ranges along them, offset of the face.
    faces = [
        ((0, 1, 0), (1, 0, 0), (0, 0, 1), (-hx, hx), (-hz, top - bt), hy),
        ((0, -1, 0), (1, 0, 0), (0, 0, 1), (-hx, hx), (-hz, top - bt), hy),
        ((1, 0, 0), (0, 1, 0), (0, 0, 1), (-hy + bt, hy - bt), (-hz, top - bt), hx),
        ((-1, 0, 0), (0, 1, 0), (0, 0, 1), (-hy + bt, hy - bt), (-hz, top - bt), hx),
        ((0, 0, 1), (1, 0, 0), (0, 1, 0), (-hx, hx), (-hy, hy), top),
    ]

    def world(v):
        return ax * v[0] + ay * v[1] + Z * v[2]

    for n, a, b, (a_lo, a_hi), (b_lo, b_hi), offset in faces:
        nw, aw, bw = world(n), world(a), world(b)
        a_mid, b_mid = (a_lo + a_hi) / 2, (b_lo + b_hi) / 2
        plane = centre + nw * offset
        outer = plane - nw * (bt / 2)
        for bb in (b_lo + fw / 2, b_hi - fw / 2):
            P.wood(outer + aw * a_mid + bw * bb, aw, bw, nw, a_hi - a_lo, fw, bt, BRACE, tile)
        for aa in (a_lo + fw / 2, a_hi - fw / 2):
            P.wood(outer + aw * aa + bw * b_mid, bw, aw, nw, b_hi - b_lo - 2 * fw, fw, bt, BRACE, tile)
        span = b_hi - b_lo - 2 * fw + 0.02
        length = a_hi - a_lo - 2 * fw + 0.02
        widths = 1 + jitter * rng.uniform(-1, 1, planks)
        widths = widths / widths.sum() * (span - gap * (planks - 1))
        bb = b_lo + fw - 0.01
        for w in widths:
            c = plane - nw * (raise_ + pt / 2 + rng.uniform(-0.002, 0.002)) + aw * a_mid + bw * (bb + w / 2)
            P.wood(c, aw, bw, nw, length, w, pt, PLANK, tile)
            bb += w + gap
    # Dark core behind the plank gaps, open at the bottom like the pilot crate.
    inset = raise_ + pt + 0.003
    core = P.new_part()
    corners = [P.vert(at(sx * (hx - inset), sy * (hy - inset), sz * (hz - inset)), core)
               for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]
    for quad in ((0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)):
        P.poly([corners[i] for i in quad], PLANK, [(0, 0), (1, 0), (1, 1), (0, 1)])
    # L plates on the four top corners.
    for sx in (-1, 1):
        for sy in (-1, 1):
            a, b = ax * -sx, ay * -sy
            if a.cross(b).dot(Z) < 0:
                a, b = b, a
            P.plate_l(at(sx * hx, sy * hy, top), a, b, Z, plate_leg, plate_w, plate_t, IRON)
    return centre + Z * hz                 # the centre of its top


tops = []
for spec in cfg["crates"]:
    sx, sy, sz = spec["size"]
    if spec.get("on") is None:
        fx, fy = spec["at"]
        base = Vector((lo.x + fx * (hi.x - lo.x), lo.y + fy * (hi.y - lo.y), lo.z))
    else:
        below = tops[spec["on"]]
        base = Vector((below.x + spec.get("shift", [0, 0])[0], below.y + spec.get("shift", [0, 0])[1], below.z))
    tops.append(crate(base, (sx, sy, sz), spec.get("yaw", 0.0), spec.get("planks", 4)))

P.finish(goal, out, lo, hi, {"crates": len(cfg["crates"])})
