"""Build a bundle of blades on an opened leather wrap (NW_CITY_WEAPON_BAG_01) from an image_gen concept.

blender -b --factory-startup -P weapon_bag.py -- weapon-bag.json OUT_DIR   (run from the repository root)
config: {"id": "weapon-bag", "target": "data/content/mesh3d-market.json", "seed": 1, "wrap": [1.08, 0.37, 0.006, 0.015],
         "blades": [{"length": 1.05, "width": 0.046, "at": 0.08, "fan": 2.0, "shift": 0.0}, ...], "straps": [0.3, 0.72],
         "leather_tile": 0.3, "steel_tile": 0.3}
The wrap lies flat with its long edges curling up a little; on it, side by side and
slightly fanned, lie two long swords, a short sword and a dagger, hilts toward one
end (the original's hilts are at that end of its bbox), with two loose leather
straps across them. "at" is each blade's offset across the wrap from its near edge.
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
LEATHER, STEEL, HAFT = (keys.index(k) for k in ("OWN_PROP_LEATHER", "OWN_PROP_STEEL_BLADE", "OWN_PROP_WOOD_HAFT"))
leather_tile, steel_tile = cfg.get("leather_tile", 0.3), cfg.get("steel_tile", 0.3)
P = Parts(rng, 0.1)

# --- the wrap: a thin sheet, long edges curled up ---
wl, ww, wt, curl = cfg.get("wrap", [1.08, 0.37, 0.006, 0.015])
x0 = hi.x - wl                      # the wrap lies toward the blade-tip end; hilts overhang the other end
y0 = (lo.y + hi.y) / 2 - ww / 2
nx, ny = 10, 6
part = P.new_part()


def lift(j):
    f = abs(2 * j / ny - 1)
    return curl * max(0.0, (f - 0.6) / 0.4) ** 2


top = [[P.vert(Vector((x0 + i / nx * wl, y0 + j / ny * ww, lo.z + wt + lift(j))), part) for i in range(nx + 1)] for j in range(ny + 1)]
bot = [[P.vert(Vector((x0 + i / nx * wl, y0 + j / ny * ww, lo.z + lift(j))), part) for i in range(nx + 1)] for j in range(ny + 1)]
for j in range(ny):
    for i in range(nx):
        uv = [((i + a) / nx * wl / leather_tile, (j + b) / ny * ww / leather_tile) for a, b in ((0, 0), (1, 0), (1, 1), (0, 1))]
        P.poly((top[j][i], top[j][i + 1], top[j + 1][i + 1], top[j + 1][i]), LEATHER, uv)
        P.poly((bot[j + 1][i], bot[j + 1][i + 1], bot[j][i + 1], bot[j][i]), LEATHER, uv[::-1])
ring = [(i, 0) for i in range(nx)] + [(nx, j) for j in range(ny)] + [(i, ny) for i in range(nx, 0, -1)] + [(0, j) for j in range(ny, 0, -1)]
for k, (i, j) in enumerate(ring):
    i2, j2 = ring[(k + 1) % len(ring)]
    P.poly((bot[j][i], bot[j2][i2], top[j2][i2], top[j][i]), LEATHER, ((0, 0), (0.05, 0), (0.05, 0.02), (0, 0.02)))
surface = lo.z + wt


def lens(start, end, width, thick, part, taper=0.8):
    axis = (end - start).normalized()
    flat = Z.cross(axis).normalized()
    length = (end - start).length
    ring = lambda c, w: [P.vert(c + flat * w / 2, part), P.vert(c + Z * thick / 2, part),
                         P.vert(c - flat * w / 2, part), P.vert(c - Z * thick / 2, part)]
    r0, r1 = ring(start, width), ring(start + axis * length * 0.88, width * taper)
    tip = P.vert(end, part)
    for i in range(4):
        j = (i + 1) % 4
        P.poly((r0[i], r0[j], r1[j], r1[i]), STEEL, ((i * 0.02, 0), ((i + 1) * 0.02, 0), ((i + 1) * 0.02, length / steel_tile), (i * 0.02, length / steel_tile)))
        P.poly((r1[i], r1[j], tip), STEEL, ((0, 0), (0.02, 0), (0.01, 0.04)))
    P.poly(r0[::-1], STEEL, [(0, 0), (0.02, 0), (0.02, 0.02), (0, 0.02)])


def blade(pommel_at, axis, length, width):
    """A sword lying flat: pommel, grip, guard and a lens-section blade toward +axis."""
    flat = Z.cross(axis).normalized()
    pm = 0.034
    P.post(pommel_at, pommel_at + axis * pm, pm, pm * 0.7, flat, Z, STEEL, 0.1)
    grip = pommel_at + axis * pm
    hl = 0.11 if length > 0.6 else 0.08
    P.post(grip, grip + axis * hl, 0.028, 0.025, flat, Z, HAFT, 0.3)
    guard = grip + axis * (hl + 0.011)
    gl = 0.14 if length > 0.8 else (0.11 if length > 0.5 else 0.07)
    P.board(guard, flat, axis, Z, gl, 0.022, 0.02, STEEL)
    lens(guard + axis * 0.011, pommel_at + axis * length, width, 0.009, P.new_part())


for spec in cfg["blades"]:
    fan = math.radians(spec.get("fan", 0.0))
    axis = Vector((math.cos(fan), math.sin(fan), 0))
    y = y0 + spec["at"] * ww / 0.37
    z = surface + 0.017
    start = Vector((lo.x + 0.005 + spec.get("shift", 0.0), y, z))   # shorter blades lie further in
    blade(start, axis, spec["length"], spec["width"])

# two loose straps across the blades, each with a small iron buckle
for f in cfg.get("straps", [0.3, 0.72]):
    xs = x0 + f * wl
    c = Vector((xs, (lo.y + hi.y) / 2 + rng.uniform(-0.02, 0.02), surface + 0.036))
    rot = Matrix.Rotation(rng.uniform(-0.15, 0.15), 3, "Z")
    P.board(c, rot @ Y, rot @ X, Z, ww * 1.05, 0.035, 0.004, LEATHER)
    P.board(c + rot @ Y * (ww * 0.3) + Z * 0.004, rot @ Y, rot @ X, Z, 0.04, 0.045, 0.004, STEEL)

P.finish(goal, out, lo, hi, {"blades": len(cfg["blades"])})
