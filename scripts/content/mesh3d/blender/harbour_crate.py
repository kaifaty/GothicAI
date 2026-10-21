"""One harbour crate by parts, shared by crate.py (the single crate) and crate_pile.py.

The design comes from reports/content/2026-10-crates-redesign (object-design, variant 2):
rough plank boxes made by harbour carpenters. Each short end has two thick corner posts
nailed over horizontal end planks and a rope handle between them; each long side is
horizontal planks of uneven width with one upright batten in the middle; the lid is
planks along the length lying between the posts. No metal. Wear is geometry where it
is cheap: uneven plank widths, small in-plane tilts, planks proud or sunk by a few
millimetres, one broken plank with a gap onto the dark core, one shifted lid plank.

crate(P, zones, base, size, R, cfg, rng): base is the centre of the bottom, size the
(length, width, height) along the crate's own x, y, z, R its 3x3 rotation. cfg keys
(all optional): post [width, thick], batten [width, thick], plank, gap [min, max],
plank_width (target width in m), tilt_deg, proud, rope [radius, sides, segments],
rope_ends ("both", "one", "none"), nails (bool), nail_radius, nail, broken (bool),
lid_shift (m, one plank), lid_offset [dx, dy] (m, the whole lid pushed aside). zones: {"plank", "brace", "inner", "rope",
"nail"} material indices; "nail" may be None when nails are off. The dark core takes "inner": it is
seen only through the gaps, so it has its own colour target and does not pull the planks' mean
down. tile is the wood tile size in metres.
"""
import math

from mathutils import Matrix, Vector

X, Y, Z = Vector((1, 0, 0)), Vector((0, 1, 0)), Vector((0, 0, 1))


def _widths(rng, span, gap, target):
    """Uneven plank widths (each 0.7–1.3 of the mean) that fill span with gaps between them."""
    n = max(2, round(span / target))
    w = rng.uniform(0.7, 1.3, n)
    gaps = rng.uniform(gap[0], gap[1], n - 1)
    w = w / w.sum() * (span - gaps.sum())
    return w, gaps


def crate(P, zones, base, size, R, cfg, rng, tile):
    L, W, H = size
    ex, ey, ez = R @ X, R @ Y, R @ Z

    def at(x, y, z):
        return base + ex * x + ey * y + ez * z

    post_w, post_t = cfg.get("post", [0.085, 0.035])
    bat_w, bat_t = cfg.get("batten", [0.07, 0.025])
    pt = cfg.get("plank", 0.022)
    gap = cfg.get("gap", [0.004, 0.01])
    target = cfg.get("plank_width", 0.19)
    tilt = math.radians(cfg.get("tilt_deg", 0.6))
    proud = cfg.get("proud", 0.002)
    nails = cfg.get("nails", False) and zones.get("nail") is not None
    nail_r, nail_h = cfg.get("nail_radius", 0.009), cfg.get("nail", 0.004)
    PLANK, BRACE, INNER, ROPE, NAIL = zones["plank"], zones["brace"], zones["inner"], zones["rope"], zones.get("nail")

    lid_top = H
    wall_top = H - pt                     # the walls end under the lid planks
    x_end = L / 2 - post_t                # outer face of the end planks = inner face of the posts
    y_side = W / 2 - bat_t                # outer face of the long-side planks
    turned = lambda axis, normal: Matrix.Rotation(rng.uniform(-tilt, tilt), 3, normal) @ axis

    def nail(spot, normal_w, a_w):
        if nails:
            P.nail(spot, a_w, normal_w.cross(a_w), normal_w, nail_r, nail_h, NAIL)

    # --- long sides: horizontal planks running between the posts, upright batten in the middle ---
    broken_side = int(rng.integers(0, 2)) if cfg.get("broken", True) else -1
    mid_x = rng.uniform(-0.06, 0.06) * L
    for s_i, s in enumerate((-1, 1)):
        n_w = ey * s
        widths, gaps = _widths(rng, wall_top, gap, target)
        broken_k = int(rng.integers(0, len(widths))) if s_i == broken_side else -1
        z = 0.0
        for k, w in enumerate(widths):
            depth = y_side - pt / 2 + rng.uniform(-proud, proud)
            along = turned(ex, n_w) if k else ex   # the bottom plank stays level on the ground
            if k == broken_k:   # two pieces with a gap onto the dark core
                hole_at, hole = rng.uniform(-0.25, 0.25) * L, rng.uniform(0.08, 0.15)
                for a0, a1 in ((-x_end, hole_at - hole / 2), (hole_at + hole / 2, x_end)):
                    P.wood(at((a0 + a1) / 2, s * depth, z + w / 2), along, ez, n_w, a1 - a0, w, pt, PLANK, tile)
            else:
                P.wood(at(0, s * depth, z + w / 2), along, ez, n_w, 2 * x_end, w, pt, PLANK, tile)
            for xe in (-x_end + 0.035, x_end - 0.035):
                nail(at(xe, s * y_side, z + w / 2), n_w, ex)
            nail(at(mid_x, s * (W / 2), z + w / 2), n_w, ex)
            z += w + (gaps[k] if k < len(gaps) else 0)
        P.wood(at(mid_x, s * (W / 2 - bat_t / 2), (wall_top - 0.02) / 2 + 0.01), turned(ez, n_w), ex, n_w,
               wall_top - 0.02, bat_w * rng.uniform(0.9, 1.1), bat_t, BRACE, tile)

    # --- short ends: horizontal planks behind two corner posts, a rope handle between them ---
    rope_ends = cfg.get("rope_ends", "both")
    rope_side = int(rng.integers(0, 2))
    for e_i, e in enumerate((-1, 1)):
        n_w = ex * e
        widths, gaps = _widths(rng, wall_top, gap, target)
        z = 0.0
        for k, w in enumerate(widths):
            depth = x_end - pt / 2 + rng.uniform(-proud, proud)
            P.wood(at(e * depth, 0, z + w / 2), turned(ey, n_w) if k else ey, ez, n_w, 2 * (y_side - pt), w, pt, PLANK, tile)
            z += w + (gaps[k] if k < len(gaps) else 0)
        for s in (-1, 1):
            pw = post_w * rng.uniform(0.9, 1.1)
            P.wood(at(e * (L / 2 - post_t / 2), s * (W / 2 - pw / 2), lid_top / 2), turned(ez, n_w), ey, n_w,
                   lid_top, pw, post_t, BRACE, tile)
            for f in (0.15, 0.5, 0.85):
                nail(at(e * (L / 2), s * (W / 2 - pw / 2), lid_top * f), n_w, ey)
        if rope_ends == "both" or (rope_ends == "one" and e_i == rope_side):
            _rope(P, ROPE, at, e, x_end, W, wall_top, cfg)

    # --- lid: planks along the length between the posts, one of them shifted ---
    widths, gaps = _widths(rng, W - 0.01, gap, target * 0.9)
    shifted = int(rng.integers(0, len(widths)))
    ox, oy = cfg.get("lid_offset", [0.0, 0.0])
    y = -(W - 0.01) / 2 + oy
    for k, w in enumerate(widths):
        shift = cfg.get("lid_shift", 0.015) * (1 if rng.uniform() < 0.5 else -1) if k == shifted else 0.0
        length = 2 * x_end - 0.004 - abs(shift)
        P.wood(at(ox + shift / 2, y + w / 2, lid_top - pt / 2 + rng.uniform(-proud, proud)), turned(ex, ez), ey, ez,
               length, w, pt, PLANK, tile)
        for xe in (-x_end + 0.03, x_end - 0.03):
            nail(at(ox + xe, y + w / 2, lid_top), ez, ex)
        y += w + (gaps[k] if k < len(gaps) else 0)

    # --- dark core behind the gaps, open at the bottom ---
    cx, cy, cz = x_end - pt - 0.003, y_side - pt - 0.003, wall_top - 0.003
    core = P.new_part()
    corners = [P.vert(at(sx * cx, sy * cy, cz if sz > 0 else 0.0), core) for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]
    for quad in ((0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)):
        P.poly([corners[i] for i in quad], INNER, [(0, 0), (1, 0), (1, 1), (0, 1)])
    return at(0, 0, lid_top)


def _rope(P, ROPE, at, e, x_end, W, wall_top, cfg):
    """A sagging U of rope on the end face; its two ends dip into the planks."""
    radius, sides, segs = cfg.get("rope", [0.014, 6, 10])
    half, sag, zc = 0.2 * W, 0.11, 0.74 * wall_top
    part = P.new_part()
    rings = []
    for i in range(segs + 1):
        t = math.pi * i / segs
        out = 0.006 + 0.014 * math.sin(t) - (0.02 if i in (0, segs) else 0.0)   # stays inside the posts
        c = at(e * (x_end + out), half * math.cos(t), zc - sag * math.sin(t))
        nxt = at(e * x_end, half * math.cos(t + 0.01), zc - sag * math.sin(t + 0.01))
        tangent = (nxt - at(e * x_end, half * math.cos(t), zc - sag * math.sin(t))).normalized()
        side = tangent.cross((at(e, 0, 0) - at(0, 0, 0)).normalized()).normalized()
        up = tangent.cross(side).normalized()
        rings.append([P.vert(c + (side * math.cos(2 * math.pi * k / sides) + up * math.sin(2 * math.pi * k / sides)) * radius, part)
                      for k in range(sides)])
    along = 0.0
    for i in range(segs):
        step = (rings[i + 1][0].co - rings[i][0].co).length
        for k in range(sides):
            k2 = (k + 1) % sides
            P.poly((rings[i][k], rings[i][k2], rings[i + 1][k2], rings[i + 1][k]), ROPE,
                   ((k / sides * 0.1, along), ((k + 1) / sides * 0.1, along), ((k + 1) / sides * 0.1, along + step),
                    (k / sides * 0.1, along + step)))
        along += step
    P.poly(rings[0][::-1], ROPE, [(0, 0)] * sides)
    P.poly(rings[-1], ROPE, [(0, 0)] * sides)
