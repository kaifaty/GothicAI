"""Build the cooking cauldron (CAULDRON_OC): a riveted sheet-iron pot standing on the fire stones.

blender -b --factory-startup -P cauldron.py -- cauldron.json OUT_DIR   (run from the repository root)
config: {"id": "hearth-cauldron", "target": "data/content/mesh3d-hearth.json", "seed": 3, "sides": 40,
         "profile": [[h, r], ...] outer radius by height from the flat bottom to the rim (m),
         "wall": 0.012, "rim": 0.015, "liquid": 0.57,
         "bands": [[h0, h1], ...], "band_proud": 0.005, "rivet_step": 0.10, "rivet": [0.009, 0.005],
         "dents": [0.004, 5], "oval": 0.006, "tile": 0.5}
Asset city-hearth (intent.md, wave 1; the user chose on 2026-10-31 to take the form from the original's
mesh and the material from concept hearth-cauldron attempt 6). The pot is turned from the measured
profile: a flat bottom, the belly widest at 0.4 of the height, the neck, and a collar flaring out almost
to the belly. Its centre and floor are the original's (bbox of the target). Inside, the wall comes down
to a floor; the water at "liquid" m is a flat disc in its own zone, drawn as clear engine water. Each band is a raised ring
"band_proud" proud of the wall with rivet heads every "rivet_step" m. The wall is hammered: small
dents "dents" [depth, count around] and a slight oval, so the contour is round but not turned.
"""
import math
import sys
from pathlib import Path

import numpy as np
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parts import Parts, args, load_goal  # noqa: E402

cfg, out = args()
goal, lo, hi = load_goal(cfg)
rng = np.random.default_rng(cfg.get("seed", 3))
keys = [z["key"] for z in goal["zones"]]
IRON, RIVET, LIQUID = (keys.index(k) for k in ("OWN_HEARTH_IRON", "OWN_HEARTH_RIVET", "OWN_HEARTH_LIQUID"))
P = Parts(rng, 0.1)
sides = cfg["sides"]
cx, cy, floor = (lo.x + hi.x) / 2, (lo.y + hi.y) / 2, lo.z
profile = np.array(cfg["profile"], dtype=float)
dent_depth, dent_count = cfg.get("dents", [0.004, 5])
phases = rng.uniform(0, 2 * math.pi, 3)
oval = cfg.get("oval", 0.0)


def radius(h, t):
    """Outer radius at height h and angle t: the profile, a slight oval and hammer dents."""
    r = float(np.interp(h, profile[:, 0], profile[:, 1]))
    belly = math.sin(math.pi * min(max(h / profile[-1, 0], 0.0), 1.0))  # dents fade at the bottom and rim
    wobble = (math.sin(dent_count * t + phases[0]) * math.sin(3.1 * h * 2 * math.pi + phases[1])
              + 0.5 * math.sin((dent_count + 2) * t + phases[2]))
    return r + oval * math.cos(2 * t) - dent_depth * belly * max(wobble, -0.3)


def point(h, t, r):
    return Vector((cx + r * math.cos(t), cy + r * math.sin(t), floor + h))


def lathe(rows, material, part, closed=False):
    """Quads between consecutive rings rows = [(h, [r(t) ...])], the last back to the first if closed;
    u = arc length, v = run along the profile. Returns the rings."""
    rings = [[P.vert(point(h, 2 * math.pi * i / sides, rs[i]), part) for i in range(sides)] for h, rs in rows]
    pairs = list(range(len(rings) - 1)) + ([len(rings) - 1] if closed else [])
    run = 0.0
    for k in pairs:
        k1 = (k + 1) % len(rings)
        (h0, r0), (h1, r1) = rows[k], rows[k1]
        step = math.hypot(h1 - h0, np.mean(r1) - np.mean(r0))
        for i in range(sides):
            j = (i + 1) % sides
            a, b = 2 * math.pi * np.mean(r0) * i / sides, 2 * math.pi * np.mean(r0) * (i + 1) / sides
            P.poly([rings[k][i], rings[k][j], rings[k1][j], rings[k1][i]], material,
                   [(a, run), (b, run), (b, run + step), (a, run + step)])
        run += step
    return rings


def rivet(c, n, radius, height, material):
    """A low six-sided rivet head on the surface at c facing n; its open base is sunk into the band."""
    part = P.new_part(wound=True)
    P.nails += 2  # six open base edges, counted as two three-sided heads by finish()
    a = n.cross(Vector((0, 0, 1))).normalized()
    b = n.cross(a)
    base = [P.vert(c + (a * math.cos(t) + b * math.sin(t)) * radius - n * 0.002, part)
            for t in (2 * math.pi * i / 6 for i in range(6))]
    if a.cross(b).dot(n) < 0:
        base.reverse()
    tip = P.vert(c + n * height, part)
    for i in range(6):
        P.poly((base[i], base[(i + 1) % 6], tip), material, [(0, 0), (radius, 0), (radius / 2, height)])


def cap(ring, material, up):
    """A flat fan over a ring (the bottom, the liquid), planar UVs in metres."""
    c = sum((v.co for v in ring), Vector()) / len(ring)
    part = round(ring[0][P.part_layer])
    centre = P.vert(c, part)
    for i in range(len(ring)):
        a, b = ring[i], ring[(i + 1) % len(ring)]
        tri = [centre, a, b] if up else [centre, b, a]
        P.poly(tri, material, [(v.co.x, v.co.y) for v in tri])


# --- the pot: outer wall, rim, inner wall down to the inner floor, flat bottom -------------------------
heights = np.unique(np.concatenate([np.linspace(0, profile[-1, 0], cfg.get("rings", 16)), profile[:, 0]]))
angles = [2 * math.pi * i / sides for i in range(sides)]
pot = P.new_part()
outer = [(h, [radius(h, t) for t in angles]) for h in heights]
wall, rim_w = cfg.get("wall", 0.012), cfg.get("rim", 0.015)
top = heights[-1]
liquid = cfg["liquid"]
# The water is clear (the engine's water surface): the inner wall runs down to a floor as thick as the wall,
# and that floor is what one sees through it.
# Under the water the wall is seen dimly: three rings there, the fine ones above.
inner_h = sorted({*np.linspace(wall * 1.5, liquid - 0.02, 3), *[h for h in heights if h > liquid - 0.02]}, reverse=True)
inner = [(top, [radius(top, t) - rim_w for t in angles])] + \
        [(h, [radius(h, t) - wall for t in angles]) for h in inner_h if h < top]
rings = lathe(outer + inner, IRON, pot)  # outer wall up, across the rim, inner wall down
cap(rings[0], IRON, up=False)
cap(rings[-1], IRON, up=True)  # the inner floor: the shell stays closed
# the liquid: a flat disc just above where the inner wall meets it
surface = P.new_part(wound=True)  # an open disc: wound up by hand, the recalculation could turn it down
lr = [radius(liquid, t) - wall - 0.001 for t in angles]
liq = [P.vert(point(liquid, t, r), surface) for t, r in zip(angles, lr)]
cap(liq, LIQUID, up=True)

# --- bands and rivets -------------------------------------------------------------------------------
proud = cfg.get("band_proud", 0.005)
rivet_r, rivet_h = cfg.get("rivet", [0.009, 0.005])
for h0, h1 in cfg["bands"]:
    band = P.new_part()
    hs = np.linspace(h0, h1, 3)
    outside = [(h, [radius(h, t) + proud for t in angles]) for h in hs]
    under = [(h, [radius(h, t) - 0.001 for t in angles]) for h in (h1, h0)]
    lathe([under[1]] + outside + [under[0]], RIVET, band, closed=True)  # a closed ring round the wall
    hm = (h0 + h1) / 2
    count = max(8, round(2 * math.pi * float(np.interp(hm, profile[:, 0], profile[:, 1])) / cfg.get("rivet_step", 0.1)))
    for k in range(count):
        t = 2 * math.pi * (k + rng.uniform(-0.12, 0.12)) / count
        r = radius(hm, t) + proud
        n = Vector((math.cos(t), math.sin(t), 0))
        rivet(point(hm, t, r), n, rivet_r, rivet_h, RIVET)

metrics = P.finish(goal, out, lo, hi, {"sides": sides, "liquid_m": liquid, "bands": cfg["bands"]})
