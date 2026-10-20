"""Build a framed plank crate by parts from an image_gen concept and a numeric target.

blender -b --factory-startup -P crate.py -- crate.json OUT_DIR
config: {"id": "crate", "target": mesh3d-pilot.json (default: the repo's), "seed": 1,
         "frame": 0.09, "batten": 0.025, "raise": 0.012, "plank": 0.02, "gap": 0.006,
         "planks": {"long": 6, "short": 6, "top": 7}, "width_jitter": 0.12, "proud": 0.002,
         "plate_leg": 0.12, "plate_width": 0.06, "plate": 0.004, "nail": 0.004, "nail_radius": 0.008,
         "nails_per_plate": 2, "plank_nails": true, "stave_tile": 0.38, "planks_per_tile": 4}
Part counts are read from the concept by eye; the target gives size, origin and
budget. The outermost points (nail heads) meet the target bbox. Every plank,
batten, plate and nail is its own closed part with a "_part" number, so
bake_atlas.py can tone each one; tile UVs are in metres with the grain along
the part's length, and each board takes one plank of the wood tile.
"""
import sys
from pathlib import Path

import numpy as np
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parts import X, Y, Z, Parts, args, load_goal  # noqa: E402

cfg, out = args()
goal, lo, hi = load_goal(cfg)
rng = np.random.default_rng(cfg.get("seed", 1))
keys = [z["key"] for z in goal["zones"]]
PLANK, BRACE, NAIL, BAND = (keys.index(next(k for k in keys if k.endswith(s))) for s in ("PLANK", "BRACE", "NAIL", "BAND"))
centre = (lo + hi) / 2
half = (hi - lo) / 2

tile = cfg.get("stave_tile", 0.38)
nail_h = cfg.get("nail", 0.004)
plate_t = cfg.get("plate", 0.004)
m = nail_h + plate_t                      # batten outer face, inward from the bbox
fw = cfg.get("frame", 0.09)
batten_t = cfg.get("batten", 0.025)
plank_out = m + cfg.get("raise", 0.012)   # plank outer face, inward from the bbox
plank_t = cfg.get("plank", 0.02)
gap = cfg.get("gap", 0.006)
P = Parts(rng, tile / cfg.get("planks_per_tile", 4))

# Faces: outward normal, plank direction, other in-plane axis, plank count, priority at shared edges.
# The bottom stands on the ground and is not built; sides run down to the bbox floor.
faces = [
    (Y, X, Z, cfg["planks"]["long"], 0), (-Y, X, Z, cfg["planks"]["long"], 0),
    (X, Y, Z, cfg["planks"]["short"], 1), (-X, Y, Z, cfg["planks"]["short"], 1),
    (Z, X, Y, cfg["planks"]["top"], 2),
]
priority = {tuple(f[0]): f[4] for f in faces}


def extent(axis):
    return abs(half.dot(axis))


def side_range(axis, own):
    """Range along an in-plane axis: the higher-priority neighbour keeps the shared edge, so
    coplanar batten faces never overlap; toward the missing bottom the face reaches the ground."""
    e = extent(axis)
    ends = []
    for sign in (-1, 1):
        neighbour = tuple(axis * sign)
        if neighbour == (0.0, 0.0, -1.0) or neighbour == (-0.0, -0.0, -1.0):
            ends.append(e)
        elif priority.get(neighbour, -1) > own:
            ends.append(e - m - batten_t)
        else:
            ends.append(e - m)
    return -ends[0], ends[1]


# Dark core behind the plank gaps.
core_in = plank_out + plank_t + 0.002
core_part = P.new_part()
core = [P.vert(centre + Vector((sx * (half.x - core_in), sy * (half.y - core_in), sz * (half.z - core_in))), core_part)
        for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]
for quad in ((0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)):
    P.poly([core[i] for i in quad], PLANK, [(0, 0), (1, 0), (1, 1), (0, 1)])

width_jitter = cfg.get("width_jitter", 0.12)
proud = cfg.get("proud", 0.002)
for normal, along, across, count, own in faces:
    a_lo, a_hi = side_range(along, own)
    b_lo, b_hi = side_range(across, own)
    face_plane = centre + normal * extent(normal)
    outer = face_plane - normal * (m + batten_t / 2)
    a_mid, b_mid = (a_lo + a_hi) / 2, (b_lo + b_hi) / 2
    # Battens: two along the planks over the whole face, two across between them.
    for b in (b_lo + fw / 2, b_hi - fw / 2):
        P.wood(outer + along * a_mid + across * b, along, across, normal, a_hi - a_lo, fw, batten_t, BRACE, tile)
    for a in (a_lo + fw / 2, a_hi - fw / 2):
        P.wood(outer + along * a + across * b_mid, across, along, normal, b_hi - b_lo - 2 * fw, fw, batten_t, BRACE, tile)
    # Planks inside the frame, 1 cm under the battens' inner edges.
    span = b_hi - b_lo - 2 * fw + 0.02
    length = a_hi - a_lo - 2 * fw + 0.02
    widths = 1 + width_jitter * rng.uniform(-1, 1, count)
    widths = widths / widths.sum() * (span - gap * (count - 1))
    b = b_lo + fw - 0.01
    for w in widths:
        depth = plank_out + plank_t / 2 + rng.uniform(-proud, proud)
        c = face_plane - normal * depth + along * a_mid + across * (b + w / 2)
        P.wood(c, along, across, normal, length, w, plank_t, PLANK, tile)
        if cfg.get("plank_nails", True):  # one nail through each end batten into the plank
            for a in (a_lo + fw / 2, a_hi - fw / 2):
                spot = face_plane - normal * m + along * a + across * (b + w / 2)
                P.nail(spot, along, across, normal, cfg.get("nail_radius", 0.008), nail_h, NAIL)
        b += w + gap

# Corner plates: an L-plate on each visible face around the 8 corners, nails on each.
leg, pw = cfg.get("plate_leg", 0.12), cfg.get("plate_width", 0.06)
nails = cfg.get("nails_per_plate", 2)
axis = {"x": X, "y": Y, "z": Z}
for sx in (-1, 1):
    for sy in (-1, 1):
        for sz in (-1, 1):
            signs = {"x": sx, "y": sy, "z": sz}
            corner_point = centre + Vector((sx * (half.x - m), sy * (half.y - m), sz * (half.z - m)))
            if sz < 0:
                corner_point.z = centre.z - half.z + 0.01  # the lower plates end just above the ground
            for normal_axis, a_axis, b_axis in (("x", "y", "z"), ("y", "x", "z"), ("z", "x", "y")):
                if normal_axis == "z" and sz < 0:
                    continue
                n = axis[normal_axis] * signs[normal_axis]
                a = -axis[a_axis] * signs[a_axis]   # legs run inward from the corner
                b = -axis[b_axis] * signs[b_axis]
                if a.cross(b).dot(n) < 0:
                    a, b = b, a
                P.plate_l(corner_point.copy(), a, b, n, leg, pw, plate_t, BAND)
                for i in range(nails):
                    along_leg = leg * (0.35 + 0.45 * i) if nails > 1 else leg * 0.5
                    first, second = (a, b) if i % 2 == 0 else (b, a)
                    spot = corner_point + n * plate_t + first * along_leg + second * (pw / 2)
                    P.nail(spot, a, b, n, cfg.get("nail_radius", 0.008), nail_h, NAIL)

P.finish(goal, out, lo, hi)
