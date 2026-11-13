"""Build the wall torch (OW_MISC_WALL_TORCH_01): a hewn pine club in two forged rings, its top third wrapped in pitch-soaked tow.

blender -b --factory-startup -P wall_torch.py -- wall-torch.json OUT_DIR   (run from the repository root)
config: {"id": "wall-torch", "seed": 7, "goal": {...the target: bbox of the original in glTF metres, zones...},
         "butt": [x, y, z], "tip": [x, y, z], "wall": z      the original's club ends and wall plane, its mesh cm
         "radius": [[t, r], ...]                              the club's radius along it, metres
         "club": {"sides": 14, "step": 0.045, "facets": n, "crook": 0.005},
         "upper": {"at": [t0, t1], "thick": 0.004, "teeth": 14, "tooth": [0.012, 0.022], "arm": 0.025, "spike": 0.09},
         "lower": {"at": [t0, t1], "thick": 0.0035, "teeth": 10, "tooth": [0.008, 0.014], "arm": 0.02, "spike": 0.06},
         "wrap": {"profile": [[t, r], ...], "sides": 20, "lumps": 0.004, "skirt": 0.012, "wires": [t, ...], "wire": 0.0018,
                  "hot_from": 0.7},
         "drips": n}
Asset wall-torch-model (reports/content/remaster/wall-torch-model: intent.md, concept.md). The original is a
six-sided club 80 cm long leaning 24 degrees from the wall, a toothed iron band at two thirds of its length and
one on the butt, a peg from the wall to each band; its burning end is painted. Here the club is round hewn pine,
7 cm thick at the butt and 12 cm at the top, wedged by its taper in the upper ring; each ring is forged in one
piece with its peg, whose far end is a spike driven into the wall; the top 25 cm above the upper ring carries the
wrap chosen by the user (variant B): a lumpy bundle of tow and rags in pitch under three turns of wire, with
hardened drips of pitch below it. The wrap's creases hold the embers (bake_atlas.py "glow").
Along the club t runs in metres from the butt; around it the angle starts on the wall side, where the tile seam hides.
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
rng = np.random.default_rng(cfg.get("seed", 7))
keys = [z["key"] for z in goal["zones"]]
WOOD, IRON, TOW, HOT, PITCH = (keys.index(k) for k in ("OWN_TORCH_WOOD", "OWN_TORCH_IRON", "OWN_TORCH_TOW", "OWN_TORCH_TOW_HOT",
                                                       "OWN_TORCH_PITCH"))
P = Parts(rng, 0.1)


def blender(p):
    """The original's mesh cm (x, y up, z away from the wall) as Blender metres: glTF (-x, y, z) / 100, Z up."""
    return Vector((-p[0] / 100, -p[2] / 100, p[1] / 100))


butt, tip = blender(cfg["butt"]), blender(cfg["tip"])
wall_y = -cfg["wall"] / 100
axis = (tip - butt).normalized()
length = (tip - butt).length
side = Vector((1, 0, 0))
back = axis.cross(side)  # across the club, towards the wall and up
radius_t, radius_r = zip(*cfg["radius"])
crook = cfg["club"].get("crook", 0.005)
bend = [(rng.uniform(0.6, 1.4), rng.uniform(0, 2 * math.pi)) for _ in range(2)]


def centre(t):
    """The club's centre line: the axis with a slight crook, none at the two rings' pegs' ends."""
    s = t / length
    return (butt + axis * t + side * (crook * math.sin(math.pi * bend[0][0] * s + bend[0][1]) * math.sin(math.pi * s))
            + back * (crook * 0.6 * math.sin(math.pi * bend[1][0] * s + bend[1][1]) * math.sin(math.pi * s)))


def club_radius(t):
    return float(np.interp(t, radius_t, radius_r))


def around(t, angle, r):
    return centre(t) + (back * math.cos(angle) + side * math.sin(angle)) * r


def loft(rings, material, around_m, runs, part, caps=(True, True), cap_material=None):
    """Quads between closed rings of equal size and fans over the end rings, wound to face out of a tube whose
    rings turn from the wall side towards +X (around()). UV: u runs once round over around_m metres (a whole number
    of tile repeats, so the tile has no seam), v is the run along, metres. material may be material(k) of the strip
    from ring k."""
    n = len(rings[0])
    for k in range(len(rings) - 1):
        for i in range(n):
            j = (i + 1) % n
            u0, u1 = around_m * i / n, around_m * (i + 1) / n
            P.poly([rings[k][j], rings[k][i], rings[k + 1][i], rings[k + 1][j]], material(k) if callable(material) else material,
                   [(u1, runs[k]), (u0, runs[k]), (u0, runs[k + 1]), (u1, runs[k + 1])])
    for end, ring in ((0, rings[0]), (1, rings[-1])):
        if not caps[end]:
            continue
        mid = sum((v.co for v in ring), Vector()) / n
        c = P.vert(mid, part)
        a, b = (ring[1].co - ring[0].co).normalized(), None
        normal = (ring[1].co - ring[0].co).cross(ring[2].co - ring[0].co).normalized()
        b = normal.cross(a)
        flat = lambda v: ((v.co - mid).dot(a), (v.co - mid).dot(b))
        for i in range(n):
            tri = [c, ring[i], ring[(i + 1) % n]] if end == 0 else [c, ring[(i + 1) % n], ring[i]]
            mat = cap_material if cap_material is not None else (material(len(rings) - 2 if end else 0) if callable(material) else material)
            P.poly(tri, mat, [flat(v) for v in tri])


def whole_tiles(metres, tile=0.25):
    """The nearest whole number of tile repeats to a run of this many metres, as metres of UV."""
    return max(round(metres / tile), 1) * tile


# --- the club: hewn round, with flat cuts of the axe that run 10-25 cm along it
club = cfg["club"]
sides = club.get("sides", 14)
facets = [(rng.uniform(0, 2 * math.pi), rng.uniform(0.04, length - 0.3), rng.uniform(0.05, 0.13), rng.uniform(0.003, 0.008))
          for _ in range(club.get("facets", 16))]


def hewn(t, angle):
    r = club_radius(t)
    for at, mid, half, depth in facets:
        fall = 1 - ((t - mid) / half) ** 2
        turn = math.cos(angle - at)
        if fall > 0 and turn > 0.3:
            r = min(r, (club_radius(t) - depth * fall) / turn)
    return r


part = P.new_part()
top = cfg["wrap"]["profile"][0][0] + 0.045  # the club ends just inside the wrap, which is closed below
stations = [0.0, 0.008] + list(np.arange(club.get("step", 0.045), top, club.get("step", 0.045))) + [top]
rings = []
for k, t in enumerate(stations):
    shrink = 0.86 if k == 0 else 1.0  # the butt's edge is knocked off
    rings.append([P.vert(around(t, 2 * math.pi * i / sides, hewn(t, 2 * math.pi * i / sides) * shrink
                                * (1 + rng.uniform(-0.012, 0.012))), part) for i in range(sides)])
loft(rings, WOOD, whole_tiles(2 * math.pi * club_radius(length / 2)), stations, part)


# --- a forged ring with a toothed lower edge, in one piece with its peg to the wall
def ring_with_peg(spec):
    t0, t1 = spec["at"]
    thick, n = spec["thick"], 16
    part = P.new_part()
    inner = lambda t: club_radius(t) + 0.0008
    wobble = [rng.uniform(-0.0006, 0.0006) for _ in range(n)]
    rows = [(t0, inner(t0) - 0.002), (t0, inner(t0) + thick), (t1, inner(t1) + thick), (t1, inner(t1) - 0.002)]
    rings = [[P.vert(around(t + (wobble[i] * 2 if r > inner(t) else 0), 2 * math.pi * i / n, r + (wobble[i] if r > inner(t) else 0)), part)
              for i in range(n)] for t, r in rows]
    around_m = whole_tiles(2 * math.pi * inner(t0))
    loft(rings + [rings[0]], IRON, around_m, [0, thick, thick + (t1 - t0), 2 * thick + (t1 - t0), 2 * thick + 2 * (t1 - t0)], part,
         caps=(False, False))
    teeth = spec["teeth"]
    for k in range(teeth):  # each tooth: a wedge of the same strip lying on the wood below the ring
        tooth = P.new_part()
        a0, a1 = 2 * math.pi * (k + rng.uniform(0.02, 0.1)) / teeth, 2 * math.pi * (k + 1 - rng.uniform(0.02, 0.1)) / teeth
        am = (a0 + a1) / 2 + rng.uniform(-0.03, 0.03)
        tt = t0 - rng.uniform(*spec["tooth"])
        base = [(a0, t0 + 0.001), (am, t0 + 0.001), (a1, t0 + 0.001)]
        o = [P.vert(around(t, a, inner(t) + thick * 0.95), tooth) for a, t in base] + [P.vert(around(tt, am, inner(tt) + thick * 0.7), tooth)]
        i_ = [P.vert(around(t, a, inner(t) - 0.001), tooth) for a, t in base] + [P.vert(around(tt, am, inner(tt) - 0.001), tooth)]
        uv = lambda v: ((v.co - o[0].co).dot(side), (v.co - o[0].co).dot(axis))
        for face in ([o[0], o[1], o[3]], [o[1], o[2], o[3]], [i_[1], i_[0], i_[3]], [i_[2], i_[1], i_[3]],
                     [o[0], o[3], i_[3], i_[0]], [o[3], o[2], i_[2], i_[3]], [o[1], o[0], i_[0], i_[1]], [o[2], o[1], i_[1], i_[2]]):
            P.poly(face, IRON, [uv(v) for v in face])
    # the peg: a square bar from the ring's wall side level to the wall, a shoulder where the hammer drove it,
    # then the spike inside the wall
    tm = (t0 + t1) / 2
    start = centre(tm) + back * (inner(tm) + thick * 0.4)
    w = spec["arm"]
    reach = wall_y - start.y
    peg = P.new_part()
    rows = [(0.0, w * 1.25, w * 1.5), (min(0.03, reach * 0.3), w, w), (reach * 0.55, w * 0.96, w * 0.96), (reach - 0.03, w, w),
            (reach - 0.016, w * 1.36, w * 1.36), (reach - 0.004, w * 1.3, w * 1.3), (reach, w * 0.9, w * 0.9),
            (reach + spec["spike"], w * 0.14, w * 0.14)]
    turn = rng.uniform(-0.12, 0.12)  # a forged bar is never quite square to the wall
    rings = []
    for s, wide, high in rows:
        ring = []
        for cx, cz in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            x, z = cx * wide / 2 * (1 + rng.uniform(-0.04, 0.04)), cz * high / 2 * (1 + rng.uniform(-0.04, 0.04))
            ring.append(P.vert(start + Vector((x * math.cos(turn) - z * math.sin(turn), s, x * math.sin(turn) + z * math.cos(turn))), peg))
        rings.append(ring)
    loft(rings, IRON, 4 * w, [s for s, _, _ in rows], peg)
    return start


upper_start = ring_with_peg(cfg["upper"])
lower_start = ring_with_peg(cfg["lower"])

# --- the wrap: a lumpy bundle round the club's top, pinched by the wire, its lower edge ragged
wrap = cfg["wrap"]
n = wrap.get("sides", 20)
lump_amp = wrap.get("lumps", 0.004)
waves = [(int(rng.integers(1, 5)), rng.uniform(18, 50), rng.uniform(0, 2 * math.pi), rng.uniform(0, 2 * math.pi), rng.uniform(0.5, 1.0))
         for _ in range(7)]
fine = [(int(rng.integers(3, 6)), rng.uniform(55, 100), rng.uniform(0, 2 * math.pi), rng.uniform(0, 2 * math.pi), rng.uniform(0.5, 1.0))
        for _ in range(6)]
profile_t, profile_r = zip(*wrap["profile"])
# each turn of wire bites deeper on one side than on the other
bites = [(t, rng.uniform(0, 2 * math.pi), rng.uniform(0.0035, 0.006)) for t in wrap["wires"]]


def wrap_radius(t, angle):
    """The wrap's radius on its side, t up to the last point before the top turns in."""
    lumps = sum(a * math.sin(m * angle + p) * math.sin(q * t + s) for m, q, p, s, a in waves) / 2.2
    lumps += 0.45 * sum(a * math.sin(m * angle + p) * math.sin(q * t + s) for m, q, p, s, a in fine) / 1.8
    folds = 0.3 * math.sin(3 * angle + 70 * t)  # the layers of the winding run round and up
    bite = sum(depth * (0.35 + 0.65 * (0.5 + 0.5 * math.cos(angle - at))) * math.exp(-((t - tw) / 0.007) ** 2) for tw, at, depth in bites)
    r = float(np.interp(t, profile_t[:-3], profile_r[:-3])) + lump_amp * (lumps + folds) - bite
    return max(r, club_radius(min(t, length)) + 0.004)  # a dent never lets the club through


# its shell folds back under the torn edge, where the normal recalculation picks the wrong side: wound here
part = P.new_part(wound=True)
t_low = profile_t[0]
# the lower edge hangs in a few torn tongues: a slow wander round the club plus a jag at every vertex
skirt = wrap.get("skirt", 0.012)
phase = [rng.uniform(0, 2 * math.pi) for _ in range(2)]
ragged = [skirt * (0.8 * math.sin(2 * a + phase[0]) + 0.6 * math.sin(5 * a + phase[1]) + rng.uniform(-0.7, 0.7))
          for a in (2 * math.pi * i / n for i in range(n))]
rings = [[P.vert(around(t_low + 0.03, 2 * math.pi * i / n, club_radius(t_low) - 0.004), part) for i in range(n)],
         [P.vert(around(t_low + ragged[i], 2 * math.pi * i / n, club_radius(t_low) + 0.006 + abs(ragged[i]) * 0.35), part) for i in range(n)]]
runs = [0.0, 0.03]
# the side by even steps; the top by the profile's own last points, which turn back down into a shallow hollow
side_t, side_r = profile_t[:-3], profile_r[:-3]
body = [(t, float(np.interp(t, side_t, side_r))) for t in np.arange(t_low + 0.016, side_t[-1], wrap.get("step", 0.014))]
body += list(zip(profile_t[-4:], profile_r[-4:]))
previous = (t_low, club_radius(t_low) + 0.006)
for k, (t, base) in enumerate(body):
    last = k == len(body) - 1
    rise = base - float(np.interp(min(t, side_t[-1]), side_t, side_r)) if k >= len(body) - 3 else 0.0  # the top's own radius
    rings.append([P.vert(around(t, 2 * math.pi * i / n, base if last else wrap_radius(min(t, side_t[-1]), 2 * math.pi * i / n) + rise), part)
                  for i in range(n)])
    runs.append(runs[-1] + math.hypot(t - previous[0], base - previous[1]))
    previous = (t, base)
# the upper part, under the flame, is the one that smoulders: its own zone of the same tile, with the glow
hot_from = wrap.get("hot_from", 0.7)
strip_t = [t_low + 0.03, t_low] + [t for t, _ in body]
loft(rings, lambda k: HOT if strip_t[k] >= hot_from else TOW, whole_tiles(2 * math.pi * max(profile_r), 0.2), runs, part)

# --- the wire: three uneven turns in the pinches, each with a twisted tail
for k, t in enumerate(wrap["wires"]):
    part = P.new_part()
    r_wire, seg = wrap.get("wire", 0.0018), 18
    tilt_a, tilt = rng.uniform(0, 2 * math.pi), rng.uniform(0.004, 0.011)
    rings = []
    for s in range(seg):
        angle = 2 * math.pi * s / seg
        tt = t + tilt * math.cos(angle - tilt_a)
        c = around(tt, angle, wrap_radius(tt, angle) + r_wire * 0.3)
        outward = (c - centre(tt)).normalized()
        rings.append([P.vert(c + (outward * math.cos(a) + axis * math.sin(a)) * r_wire, part)
                      for a in (math.pi / 2, math.pi * 7 / 6, math.pi * 11 / 6)])
    loft(rings + [rings[0]], IRON, 0.02, [0.3 * s / seg for s in range(seg + 1)], part, caps=(False, False))
    tail = P.new_part()  # the twisted ends stick out
    angle = rng.uniform(0.7, 2.4) * (1 if k % 2 else -1)
    root = around(t + tilt * math.cos(angle - tilt_a), angle, wrap_radius(t, angle) + r_wire)
    outward = (root - centre(t)).normalized()
    tangent = axis.cross(outward).normalized()
    rings = []
    for s, grow in ((0.0, 1.5), (0.008, 1.3), (0.017, 0.8)):
        c = root + outward * s + axis * (s * rng.uniform(-0.6, 0.6)) + tangent * (s * rng.uniform(-0.5, 0.5))
        rings.append([P.vert(c + (tangent * math.cos(a) + axis * math.sin(a)) * r_wire * grow, tail) for a in (0, math.pi * 2 / 3, math.pi * 4 / 3)])
    loft(rings, IRON, 0.02, [0, 0.008, 0.017], tail)

# --- hardened pitch: drips run from the wrap down the lower side of the leaning club and over the ring
u0, u1 = cfg["upper"]["at"]
for k in range(cfg.get("drips", 7)):
    part = P.new_part()
    angle = math.pi + rng.normal(0, 0.9)  # mostly the side away from the wall: it faces down
    start_t = t_low + rng.uniform(-0.004, 0.012)
    run = rng.uniform(0.03, 0.085)
    width = rng.uniform(0.0028, 0.0045)
    shape = [(0.0, 0.5), (0.25, 1.0), (0.6, 0.72), (0.86, 1.25), (1.0, 0.35)]
    rings = []
    for s, fat in shape:
        t = start_t - run * s
        over = cfg["upper"]["thick"] + 0.0008 if u0 - 0.002 < t < u1 + 0.002 else 0.0
        rho = width * fat
        c = around(t, angle, hewn(t, angle) + over + rho * 0.35)
        outward = (c - centre(t)).normalized()
        tangent = axis.cross(outward).normalized()
        rings.append([P.vert(c + (tangent * math.cos(a) * 1.15 + outward * math.sin(a) * 0.8) * rho, part) for a in np.arange(5) * 2 * math.pi / 5])
    loft(rings, PITCH, 0.03, [run * s for s, _ in shape], part)

flame = centre(profile_t[-1])
P.finish(goal, out, lo, hi, {"club_m": round(length, 4), "lean_deg": round(math.degrees(math.acos(axis.z)), 1),
                             "wrap_top_gltf_m": [round(flame.x, 4), round(flame.z, 4), round(-flame.y, 4)],
                             "wrap_base_t_m": t_low, "upper_peg_start_gltf_m": [round(upper_start.x, 4), round(upper_start.z, 4), round(-upper_start.y, 4)]})
