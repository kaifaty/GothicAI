"""Build a weapon dealer's open barrel full of blades (NW_CITY_WEAPON_BARREL_01) from an image_gen concept.

blender -b --factory-startup -P weapon_barrel.py -- weapon-barrel.json OUT_DIR   (run from the repository root)
config: {"id": "weapon-barrel", "target": "data/content/mesh3d-market.json", "seed": 1, "barrel_height": 0.9,
         "sides": 16, "end_ratio": 0.84, "hoops": [0.1, 0.5, 0.9], "hoop": [0.06, 0.008], "rim": 0.022, "depth": 0.22,
         "weapons": [{"kind": "sword"|"short"|"axe"|"spear", "at": [dx, dy], "lean": [deg_x, deg_y], "length": m}, ...],
         "hilt": {"pommel": 0.05, "grip": [0.14, 0.036], "guard": [0.24, 0.03, 0.026]}, "blade_width": 0.058,
         "axe": [0.22, 0.18], "spear": [0.32, 0.085], "stave_tile": 0.38, "steel_tile": 0.3}
The barrel stands on the ground with an open top: staves as separate parts, three
raised hoops, a rim whose inner wall leads down to a dark fill. The original's
origin sits at the barrel's middle and its hilts reach above, as here. Blades stand
hilt-up in a loose fan leaning out from the middle; the axe and the spear stand
head-up on their hafts.
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
STAVE, IRON, STEEL, HAFT = (keys.index(k) for k in ("OWN_PROP_WOOD_STAVE", "OWN_PROP_IRON_BAND", "OWN_PROP_STEEL_BLADE", "OWN_PROP_WOOD_HAFT"))
tile, steel_tile = cfg.get("stave_tile", 0.38), cfg.get("steel_tile", 0.3)
P = Parts(rng, tile / 4)
plank_u = P.plank_u

# --- barrel: standing, open at the top ---
bh = cfg.get("barrel_height", 0.9)
sides = cfg.get("sides", 16)
cx, cy = (lo.x + hi.x) / 2, (lo.y + hi.y) / 2
r_belly = min(hi.x - lo.x, hi.y - lo.y) / 2 - 0.012
end_ratio = cfg.get("end_ratio", 0.84)
r_end = r_belly * end_ratio
hoop_w, hoop_h = cfg.get("hoop", [0.06, 0.008])
rim, depth = cfg.get("rim", 0.022), cfg.get("depth", 0.22)
z0 = lo.z


def radius(z):
    t = 2 * (z - bh / 2) / bh
    return r_end + (r_belly - r_end) * (1 - t * t)


profile = [(0.0, r_end, None)]
for f in cfg.get("hoops", [0.1, 0.5, 0.9]):
    a, b = f * bh - hoop_w / 2, f * bh + hoop_w / 2
    profile += [(a, radius(a), STAVE), (a, radius(a) + hoop_h, IRON), (b, radius(b) + hoop_h, IRON), (b, radius(b), IRON)]
profile += [(bh, r_end, STAVE), (bh, r_end - rim, STAVE), (bh - depth, r_end - rim, STAVE)]
step = 2 * math.pi / sides
for k in range(sides):
    part = P.new_part()
    plank = int(rng.integers(0, 4))
    v_off = rng.uniform(0, tile)
    a0, a1 = k * step, (k + 1) * step
    ring = lambda z, r, a: P.vert(Vector((cx + r * math.cos(a), cy + r * math.sin(a), z0 + z)), part)
    rows = [(ring(z, r, a0), ring(z, r, a1)) for z, r, _ in profile]
    along = 0.0
    for i in range(1, len(profile)):
        (z_p, r_p, _), (z, r, mat) = profile[i - 1], profile[i]
        seg = math.hypot(z - z_p, r - r_p)
        if seg < 1e-6:
            continue
        u0, u1 = plank * plank_u, (plank + 1) * plank_u
        P.poly((rows[i - 1][0], rows[i - 1][1], rows[i][1], rows[i][0]), mat,
               ((u0, v_off + along), (u1, v_off + along), (u1, v_off + along + seg), (u0, v_off + along + seg)))
        along += seg
# bottom head and the dark fill inside, both closing the tube
for z, r, flip in ((0.0, r_end, True), (bh - depth, r_end - rim, False)):
    part = P.new_part()
    verts = [P.vert(Vector((cx + r * math.cos(k * step), cy + r * math.sin(k * step), z0 + z)), part) for k in range(sides)]
    P.poly(verts[::-1] if flip else verts, STAVE, [(0.5 + 0.5 * math.cos(k * step), 0.5 + 0.5 * math.sin(k * step)) for k in range(sides)][::-1 if flip else 1])

# --- weapons ---
mouth = z0 + bh - depth * 0.6


def frame(axis):
    flat = axis.cross(Z)
    if flat.length < 1e-4:
        flat = X.copy()
    flat.normalize()
    return flat, axis.cross(flat).normalized()


def lens(start, end, width, thick, material, part, taper=0.75, point=True):
    axis = (end - start).normalized()
    flat, edge = frame(axis)
    ring = lambda c, w: [P.vert(c + flat * w / 2, part), P.vert(c + edge * thick / 2, part),
                         P.vert(c - flat * w / 2, part), P.vert(c - edge * thick / 2, part)]
    length = (end - start).length
    r0 = ring(start, width)
    r1 = ring(start + axis * length * (0.85 if point else 1.0), width * taper)
    for i in range(4):
        j = (i + 1) % 4
        P.poly((r0[i], r0[j], r1[j], r1[i]), material, ((i * 0.02, 0), ((i + 1) * 0.02, 0), ((i + 1) * 0.02, length / steel_tile), (i * 0.02, length / steel_tile)))
    P.poly(r0[::-1], material, [(0, 0), (0.02, 0), (0.02, 0.02), (0, 0.02)])
    if point:
        tip = P.vert(end, part)
        for i in range(4):
            P.poly((r1[i], r1[(i + 1) % 4], tip), material, ((0, 0), (0.02, 0), (0.01, 0.04)))
    else:
        P.poly(r1, material, [(0, 0), (0.02, 0), (0.02, 0.02), (0, 0.02)])


hilt = cfg.get("hilt", {"pommel": 0.05, "grip": [0.14, 0.036], "guard": [0.24, 0.03, 0.026]})


def knob(centre, axis, radius, length):
    """A round pommel: an octagonal disc along the blade's axis, faceted ends."""
    part = P.new_part()
    flat, edge = frame(axis)
    rings = []
    for f, rr in ((-0.5, 0.65), (-0.25, 1.0), (0.25, 1.0), (0.5, 0.65)):
        rings.append([P.vert(centre + axis * (f * length) + (flat * math.cos(2 * math.pi * k / 8) + edge * math.sin(2 * math.pi * k / 8)) * radius * rr, part)
                      for k in range(8)])
    for i in range(3):
        for k in range(8):
            k2 = (k + 1) % 8
            P.poly((rings[i][k], rings[i][k2], rings[i + 1][k2], rings[i + 1][k]), IRON, ((0, 0), (0.02, 0), (0.02, 0.02), (0, 0.02)))
    P.poly(rings[0][::-1], IRON, [(0, 0)] * 8)
    P.poly(rings[3], IRON, [(0, 0)] * 8)


def sword(top, axis, length, blade_w, guard_l):
    flat, edge = frame(axis)
    pommel = hilt["pommel"]
    knob(top + axis * (pommel / 2), axis, pommel / 2, pommel * 0.75)
    grip_len, grip_w = hilt["grip"]
    grip = top + axis * pommel
    P.post(grip, grip + axis * grip_len, grip_w, grip_w * 0.85, flat, edge, HAFT, 0.3)
    _, gw, gt = hilt["guard"]
    guard = grip + axis * (grip_len + gt / 2)
    P.board(guard, flat, axis, edge, guard_l, gt, gw, IRON)
    lens(guard + axis * (gt / 2), top + axis * length, blade_w, 0.011, STEEL, P.new_part())


def plate(outline, origin, u_axis, v_axis, n_axis, thick, material):
    """A closed prism from a convex 2D outline in the (u, v) plane, thick along n."""
    part = P.new_part()
    front = [P.vert(origin + u_axis * u + v_axis * v + n_axis * (thick / 2), part) for u, v in outline]
    back = [P.vert(origin + u_axis * u + v_axis * v - n_axis * (thick / 2), part) for u, v in outline]
    uv = [(u * 2, v * 2) for u, v in outline]
    P.poly(front, material, uv)
    P.poly(back[::-1], material, uv[::-1])
    for i in range(len(outline)):
        j = (i + 1) % len(outline)
        P.poly((front[j], front[i], back[i], back[j]), material, ((0, 0), (0.02, 0), (0.02, 0.02), (0, 0.02)))


def axe(top, axis, length):
    flat, edge = frame(axis)
    P.post(top, top + axis * length, 0.036, 0.04, flat, edge, HAFT, 0.3)
    bw, bh = cfg.get("axe", [0.22, 0.18])
    head = top + axis * 0.08
    P.board(head + flat * 0.05, flat, axis, edge, 0.1, 0.09, 0.03, IRON)    # eye and cheek round the haft
    # the bit: a plate in the plane of haft and cheek, narrow at the cheek, flaring to a curved edge
    outline = [(0.0, -bh * 0.22), (bw * 0.55, -bh * 0.3)]
    for a in np.linspace(-1, 1, 7):
        outline.append((bw - 0.02 * (1 - a * a), a * bh / 2))
    outline += [(bw * 0.55, bh * 0.3), (0.0, bh * 0.22)]
    plate(outline, head + flat * 0.09, flat, -axis, edge, 0.014, STEEL)


def spear(top, axis, length):
    flat, edge = frame(axis)
    hl, hw = cfg.get("spear", [0.32, 0.085])
    socket = top + axis * hl
    lens(socket, top, hw, 0.016, STEEL, P.new_part(), taper=0.9)          # leaf from the socket to the point
    P.post(socket, socket + axis * 0.06, 0.04, 0.034, flat, edge, IRON, 0.1)
    P.post(socket + axis * 0.06, top + axis * length, 0.032, 0.032, flat, edge, HAFT, 0.3)


for spec in cfg["weapons"]:
    dx, dy = spec["at"]
    lx, ly = (math.radians(a) for a in spec["lean"])
    down = (Matrix.Rotation(lx, 3, "X") @ Matrix.Rotation(ly, 3, "Y") @ (-Z)).normalized()
    foot = Vector((cx + dx, cy + dy, z0 + 0.06))
    kind = spec["kind"]
    if kind in ("sword", "short"):
        length = spec.get("length", 1.0 if kind == "sword" else 0.66)
        top = foot - down * length
        g_l = hilt["guard"][0]
        sword(top, down, length, cfg.get("blade_width", 0.058) * (1.0 if kind == "sword" else 0.85), g_l if kind == "sword" else g_l * 0.75)
    elif kind == "axe":
        length = spec.get("length", 0.95)
        axe(foot - down * length, down, length)
    else:
        length = spec.get("length", 1.0)
        spear(foot - down * length, down, length)

P.finish(goal, out, lo, hi, {"weapons": len(cfg["weapons"])})
