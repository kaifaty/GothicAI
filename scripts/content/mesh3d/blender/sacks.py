"""Build a heap of full grain sacks (OC_SAECKE_01) from an image_gen concept and a numeric target.

blender -b --factory-startup -P sacks.py -- sacks.json OUT_DIR   (run from the repository root)
config: {"id": "sacks", "target": "data/content/mesh3d-market.json", "seed": 1, "grid": [12, 10],
         "sack": [0.95, 0.44, 0.34], "sacks": [{"at": [fx, fy, fz], "yaw": deg, "up": false, "scale": 1.0}, ...],
         "patch": [index, 0.18], "cloth_tile": 0.5, "rope_tile": 0.15}
Each sack is a soft pillow along its own axis: a rounded bottom, a full body that
sags flat where it lies, a neck pinched by a twist of rope and a small flared tuft,
with gentle wrinkles. Positions come from the concept (four lying side by side with
their necks to the front, two across on top, one standing at the end) and are given
as fractions of the target bbox; "up" stands a sack on its bottom.
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
CLOTH, ROPE = keys.index("OWN_PROP_CLOTH"), keys.index("OWN_PROP_ROPE")
cloth_tile, rope_tile = cfg.get("cloth_tile", 0.5), cfg.get("rope_tile", 0.15)
P = Parts(rng, 0.1)
nu, nv = cfg.get("grid", [12, 10])
L0, W0, H0 = cfg.get("sack", [0.95, 0.44, 0.34])


def scale_at(u):
    """Cross-section scale along the sack, 0 = bottom end, 1 = tip of the tuft: a rounded bottom,
    a belly fuller in the middle, a pinched neck and a small flared tuft."""
    if u < 0.2:
        return math.sqrt(max(1 - ((0.2 - u) / 0.2) ** 2, 0.0)) * 0.9 + 0.1
    if u < 0.7:
        return 1.0 + 0.1 * math.sin(math.pi * (u - 0.2) / 0.5)
    if u < 0.88:
        return 1.0 - 0.82 * ((u - 0.7) / 0.18) ** 0.9
    return 0.18 + 0.2 * ((u - 0.88) / 0.12)


def sack(base, yaw, up, size):
    L, W, H = L0 * size, W0 * size, H0 * size
    R = Matrix.Rotation(math.radians(yaw), 3, "Z")
    if up:   # standing: its axis vertical, the flat side down becomes the bottom
        R = R @ Matrix.Rotation(-math.pi / 2, 3, "Y")
    part = P.new_part()
    phase = rng.uniform(0, 6.28, 3)
    rows = []
    for i in range(nu + 1):
        u = i / nu
        s = scale_at(u)
        x = -L / 2 + u * L
        row = []
        for k in range(nv):
            t = 2 * math.pi * k / nv
            c, sn = math.cos(t), math.sin(t)
            wrinkle = 1 + 0.05 * math.sin(3 * t + phase[0] + 4 * u) * math.sin(7 * u + phase[1])
            y = W / 2 * s * c * wrinkle
            z = H / 2 * s * sn * wrinkle
            if sn < 0 and not up:
                z *= 0.55                                    # sagging flat onto what it lies on
            row.append(P.vert(base + R @ Vector((x, y, z + (H / 2 * 0.55 if not up else 0))), part))
        rows.append(row)
    perim = math.pi * (W + H) / 2
    for i in range(nu):
        for k in range(nv):
            k2 = (k + 1) % nv
            u0, u1 = k / nv * perim / cloth_tile, (k + 1) / nv * perim / cloth_tile
            v0, v1 = i / nu * L / cloth_tile, (i + 1) / nu * L / cloth_tile
            P.poly((rows[i][k], rows[i][k2], rows[i + 1][k2], rows[i + 1][k]), CLOTH, ((u0, v0), (u1, v0), (u1, v1), (u0, v1)))
    tip = P.vert(base + R @ Vector((L / 2 + 0.01, 0, (H / 2 * 0.55 if not up else 0))), part)
    end = P.vert(base + R @ Vector((-L / 2 - 0.005, 0, (H / 2 * 0.55 if not up else 0))), part)
    for k in range(nv):
        k2 = (k + 1) % nv
        P.poly((rows[nu][k], rows[nu][k2], tip), CLOTH, ((0, 0), (0.05, 0), (0.025, 0.05)))
        P.poly((rows[0][k2], rows[0][k], end), CLOTH, ((0, 0), (0.05, 0), (0.025, 0.05)))
    # the tie: a twist of rope round the neck
    u = 0.86
    s = scale_at(u) * 1.25
    neck = base + R @ Vector((-L / 2 + u * L, 0, (H / 2 * 0.55 if not up else 0)))
    axis = R @ X
    a = axis.normalized()
    b = Z.cross(a) if abs(a.dot(Z)) < 0.9 else Y.cross(a)
    b.normalize()
    d = a.cross(b)
    part = P.new_part()
    seg = 8
    ring = []
    for k in range(seg):
        t = 2 * math.pi * k / seg
        radial = b * math.cos(t) * (W / 2 * s) + d * math.sin(t) * (H / 2 * s)
        ring.append([P.vert(neck + radial * f + a * da, part) for f, da in ((0.9, -0.015), (1.15, -0.015), (1.15, 0.015), (0.9, 0.015))])
    for k in range(seg):
        k2 = (k + 1) % seg
        for j in range(4):
            j2 = (j + 1) % 4
            P.poly((ring[k][j], ring[k2][j], ring[k2][j2], ring[k][j2]), ROPE, ((k / seg, 0), ((k + 1) / seg, 0), ((k + 1) / seg, 0.2), (k / seg, 0.2)))
    return base, R, (L, W, H)


placed = []
for spec in cfg["sacks"]:
    fx, fy, fz = spec["at"]
    base = Vector((lo.x + fx * (hi.x - lo.x), lo.y + fy * (hi.y - lo.y), lo.z + fz * (hi.z - lo.z)))
    placed.append(sack(base, spec.get("yaw", 0.0), spec.get("up", False), spec.get("scale", 1.0)))

# optional: a square patch of other cloth sewn on top of a sack that nothing lies on
if cfg.get("patch"):
    idx, size = cfg["patch"]
    base, R, (L, W, H) = placed[idx]
    centre = base + R @ Vector((-L * 0.1, 0, H * 0.55 + H / 2 + 0.004))
    P.board(centre, R @ X, R @ Y, Z, size, size, 0.006, CLOTH)

P.finish(goal, out, lo, hi, {"sacks": len(placed)})
