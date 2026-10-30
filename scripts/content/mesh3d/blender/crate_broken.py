"""Build the smashed harbour crate (NW_HARBOUR_CRATEBROKEN_01) by parts and a numeric target.

blender -b --factory-startup -P crate_broken.py -- crate-broken.json OUT_DIR   (run from the repository root)
config: {"id": "rig-crate-broken", "target": "data/content/mesh3d-rigging.json", "seed": 3,
         "post": [0.085, 0.035], "batten": [0.07, 0.025], "plank": 0.022, "gap": [0.004, 0.01],
         "plank_width": 0.19, "tilt_deg": 0.8, "proud": 0.003, "rope": [0.014, 6, 10],
         "nail_radius": 0.009, "nail": 0.004, "stave_tile": 0.4, "planks_per_tile": 4,
         "notch": [0.4, 0.34], "side_break": [0.2, 0.74], "floor_hole": [-0.45, 0.15], "floor_corner": 0.3,
         "splinter": [0.04, 0.12], "loose": true}
The same crate as harbour_crate.py (reports/content/2026-10-crates-redesign, variant 2): corner posts
and a rope handle on each short end, horizontal planks of uneven width with one upright batten on
the long sides. Broken as the original (asset harbour-rigging, intent.md): no lid; the +x end has a V-shaped breach in
its middle, notch[0] wide at the top, down to notch[1] of the height. Optional, off for the harbour set
after the first review (one wall broken, not three): the -y long side broken off above side_break[1] of
the height for x > side_break[0]; the middle floor plank broken through over floor_hole (x from, x to,
metres); the -y floor plank without its last floor_corner metres. "rope_ends": "none" drops the handle. Each
broken plank ends in a torn zigzag with splinters of 4–12 cm. One loose plank leans inside. The crate is
open, so there is no dark core: the inner faces of the planks are the planks themselves.
"""
import math
import sys
from pathlib import Path

import numpy as np
from mathutils import Matrix, Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from harbour_crate import X, Y, Z, _rope, _widths  # noqa: E402
from parts import Parts, args, load_goal  # noqa: E402

cfg, out = args()
goal, lo, hi = load_goal(cfg)
rng = np.random.default_rng(cfg.get("seed", 3))
keys = [z["key"] for z in goal["zones"]]
PLANK, BRACE, NAIL, ROPE = (keys.index(k) for k in
                            ("OWN_PROP_WOOD_PLANK", "OWN_PROP_WOOD_BRACE", "OWN_PROP_IRON_NAIL", "OWN_PROP_ROPE"))
tile = cfg.get("stave_tile", 0.4)
P = Parts(rng, tile / cfg.get("planks_per_tile", 4))
nail_r, nail_h = cfg.get("nail_radius", 0.009), cfg.get("nail", 0.004)
L, W, H = hi.x - lo.x - 2 * nail_h, hi.y - lo.y - 2 * nail_h, hi.z - lo.z
base = Vector(((lo.x + hi.x) / 2, (lo.y + hi.y) / 2, lo.z))
post_w, post_t = cfg.get("post", [0.085, 0.035])
bat_w, bat_t = cfg.get("batten", [0.07, 0.025])
pt = cfg.get("plank", 0.022)
gap = cfg.get("gap", [0.004, 0.01])
target = cfg.get("plank_width", 0.19)
tilt = math.radians(cfg.get("tilt_deg", 0.8))
proud = cfg.get("proud", 0.003)
s_min, s_max = cfg.get("splinter", [0.04, 0.12])
x_end, y_side = L / 2 - post_t, W / 2 - bat_t


def at(x, y, z):
    return base + Vector((x, y, z))


def turned(axis, normal):
    return Matrix.Rotation(rng.uniform(-tilt, tilt), 3, normal) @ axis


def nail(spot, n_w, a_w):
    P.nail(spot, a_w, n_w.cross(a_w), n_w, nail_r, nail_h, NAIL)


def jagged(centre, along, ax_w, n_w, a0, a1, w, thick, sign, material):
    """A board from a0 to a1 along `along` whose end at a1 (sign 1) or a0 (sign -1) is torn: the end is
    a zigzag of 4–6 points across the width, each at its own length, so it reads as split wood and
    not as a saw cut. A closed prism; UVs as Parts.board (one plank of the tile across the face)."""
    part = P.new_part()
    plank, v_off = int(rng.integers(0, 4)), rng.uniform(0, tile)
    k = int(rng.integers(4, 7))
    across = np.linspace(-w / 2, w / 2, k)
    torn = rng.uniform(0.0, 1.0, k) ** 1.5 * rng.uniform(s_min, s_max)
    torn[rng.integers(0, k)] = rng.uniform(s_min, s_max)          # one long splinter
    fixed = a0 if sign == 1 else a1
    reach = min(s_max, abs(a1 - a0) - 0.03)          # the torn end never passes the fixed one
    outline = [(fixed, -w / 2)] + [((a1 if sign == 1 else a0) - sign * reach * (1 - d / s_max), c)
                                   for c, d in zip(across, torn)] + [(fixed, w / 2)]
    if sign == -1:
        outline = outline[::-1]
    # outline runs around the face; make it counter-clockwise in (along, across)
    area = sum(x0 * y1 - x1 * y0 for (x0, y0), (x1, y1) in zip(outline, outline[1:] + outline[:1]))
    if area < 0:
        outline = outline[::-1]
    pu = P.plank_u
    rings = []
    for side in (-1, 1):
        rings.append([P.vert(centre + along * a + ax_w * c + n_w * (side * thick / 2), part) for a, c in outline])
    uv = [(plank * pu + (c / w + 0.5) * pu, a + v_off) for a, c in outline]
    P.poly(rings[1], material, uv)
    P.poly(rings[0][::-1], material, uv[::-1])
    m = len(outline)
    for i in range(m):
        j = (i + 1) % m
        seg = (Vector(outline[j]) - Vector(outline[i])).length
        # the torn edge takes its own plank's strip of the tile, not a light corner of it
        u0 = plank * pu + (outline[i][1] / w + 0.5) * pu
        u1 = plank * pu + (outline[j][1] / w + 0.5) * pu
        P.poly((rings[0][i], rings[0][j], rings[1][j], rings[1][i]), material,
               ((u0, outline[i][0] + v_off), (u1, outline[j][0] + v_off), (u1, outline[j][0] + v_off + 0.01),
                (u0, outline[i][0] + v_off + 0.01)))


def plank_span(a0, a1, centre, along, ax_w, n_w, w, broken_end=None):
    """A plank from a0 to a1 along `along` (centre gives the other coordinates at a = 0); a broken
    end (0 for a0, 1 for a1) is torn by jagged()."""
    if broken_end is None:
        P.wood(centre + along * ((a0 + a1) / 2), along, ax_w, n_w, a1 - a0, w, pt, PLANK, tile)
        return
    jagged(centre, along, ax_w, n_w, a0, a1, w, pt, 1 if broken_end == 1 else -1, PLANK)


wall_top = H - 0.006   # no lid: the walls run up to the posts' tops less a hair

# --- long sides: horizontal planks between the posts, an upright batten; -y broken at the top right ---
sb_x, sb_h = cfg.get("side_break", [0.2, 0.74])
mid_x = -0.12 * L
for s in (-1, 1):
    n_w = Y * s
    widths, gaps = _widths(rng, wall_top, gap, target)
    z = 0.0
    for k, w in enumerate(widths):
        depth = y_side - pt / 2 + rng.uniform(-proud, proud)
        along = turned(X, n_w) if k else X
        centre = at(0, s * depth, z + w / 2)
        if s == -1 and z > sb_h * wall_top - 0.02:     # broken off on the right
            stop = sb_x * L + rng.uniform(-0.08, 0.08)
            plank_span(-x_end, stop, centre, along, Z, n_w, w, broken_end=1)
        else:
            plank_span(-x_end, x_end, centre, along, Z, n_w, w)
        for xe in (-x_end + 0.035, x_end - 0.035):
            if not (s == -1 and z > sb_h * wall_top - 0.02 and xe > 0):
                nail(at(xe, s * y_side, z + w / 2), n_w, X)
        z += w + (gaps[k] if k < len(gaps) else 0)
    top = wall_top - 0.02
    P.wood(at(mid_x, s * (W / 2 - bat_t / 2), top / 2 + 0.01), turned(Z, n_w), X, n_w,
           top, bat_w * rng.uniform(0.9, 1.1), bat_t, BRACE, tile)

# --- short ends: planks behind two corner posts; +x has a U breach in the middle, -x a rope handle ---
nw_, nh_ = cfg.get("notch", [0.4, 0.34])
for e in (-1, 1):
    n_w = X * e
    widths, gaps = _widths(rng, wall_top, gap, target)
    z = 0.0
    span = y_side - pt
    off = rng.uniform(-0.06, 0.06)
    for k, w in enumerate(widths):
        depth = x_end - pt / 2 + rng.uniform(-proud, proud)
        along = turned(Y, n_w) if k else Y
        centre = at(e * depth, 0, z + w / 2)
        v0 = nh_ * wall_top
        half = nw_ / 2 * (z + w / 2 - v0) / (wall_top - v0) * rng.uniform(0.85, 1.15)   # a V: wider at the top
        if e == 1 and half > 0.04:
            plank_span(-span, off - half, centre, along, Z, n_w, w, broken_end=1)
            plank_span(off + half * rng.uniform(0.8, 1.2), span, centre, along, Z, n_w, w, broken_end=0)
        else:
            plank_span(-span, span, centre, along, Z, n_w, w)
        z += w + (gaps[k] if k < len(gaps) else 0)
    for s in (-1, 1):
        pw = post_w * rng.uniform(0.9, 1.1)
        P.wood(at(e * (L / 2 - post_t / 2), s * (W / 2 - pw / 2), wall_top / 2), turned(Z, n_w), Y, n_w, wall_top, pw, post_t, BRACE, tile)
        for f in (0.15, 0.5, 0.85):
            nail(at(e * (L / 2), s * (W / 2 - pw / 2), wall_top * f), n_w, Y)
    if e == -1 and cfg.get("rope_ends", "one") != "none":
        _rope(P, ROPE, lambda x, y, z: at(x, y, z), e, x_end, W, wall_top, cfg)

# --- floor: planks along the length lying inside; the middle one broken through, a corner missing ---
fl_w = W - 2 * bat_t - 2 * pt - 0.004
widths, gaps = _widths(rng, fl_w, gap, target * 1.1)
hole0, hole1 = cfg.get("floor_hole") or (None, None)
corner = cfg.get("floor_corner", 0.3)
y = -fl_w / 2
x_in = x_end - pt - 0.002
for k, w in enumerate(widths):
    centre = at(0, y + w / 2, pt / 2 + 0.001)
    along = turned(X, Z)
    if hole0 is not None and k == len(widths) // 2 + 1:   # broken through: two pieces, a gap onto the ground
        plank_span(-x_in, hole0, centre, along, Y, Z, w, broken_end=1)
        plank_span(hole1, x_in, centre, along, Y, Z, w, broken_end=0)
    elif k == 0 and corner > 0:                # the -y plank lacks its end at +x
        plank_span(-x_in, x_in - corner, centre, along, Y, Z, w, broken_end=1)
    else:
        plank_span(-x_in, x_in, centre, along, Y, Z, w)
    y += w + (gaps[k] if k < len(gaps) else 0)

# --- a loose broken plank leaning on the +y wall inside ---
if cfg.get("loose", True):
    length, w = 0.62, 0.15
    lean = math.radians(62)
    along = Vector((0.25, -math.cos(lean), math.sin(lean))).normalized()
    across = X.cross(along).normalized() if abs(along.dot(X)) < 0.9 else Y
    across = Vector((1, 0.15, 0)).normalized()
    across = (across - along * across.dot(along)).normalized()
    n_w = along.cross(across).normalized()
    foot = at(-0.1, y_side - pt - 0.32, pt + 0.012)
    P.wood(foot + along * (length / 2), along, across, n_w, length, w, pt, PLANK, tile)

P.finish(goal, out, lo, hi)
