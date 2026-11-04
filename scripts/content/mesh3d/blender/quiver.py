"""Build the hunter's quiver (ITRW_QUIVER), the body the engine hangs on a bowman's right hip.

blender -b --factory-startup -P quiver.py -- quiver.json OUT_DIR   (run from the repository root)
Design: reports/content/2026-11-quiver (variant 1, "hunter's tube", the user's choice 2026-11-04). A stiff
tube of boiled leather over two hazel hoops, a rolled rim, a stitched leather cap on the bottom, a back seam
and a patch; a band under the mouth with a tab up to a small iron ring; a second strap from a third of the
length up to the belt behind the hip. No arrows: the engine draws them (quiver.h, branch claude/quiver-mount).
A dark cavity "cavity" metres deep under the mouth, so nothing light shows between the arrows.

Frame (as the engine's Quiver): glTF metres, the centre of the mouth at the origin, the quiver along -Y,
+Z the wearer's forward, +X towards the wearer's body (glTF is right-handed and the loader mirrors X, so
the wearer's right, outward, is -X). Blender axes: glTF (x, y, z) = Blender (x, z, -y).

The engine tilts this frame on BIP01 PELVIS (bottom back "tilt_back", out "tilt_out") and pushes it outward
under the armour until the nearest armour point is 2 cm from the tube. So the belt side cannot be modelled to
one armour: the hanger and the tilt strap run "reach" metres towards the body, level in the world, and their
ends go under the belt and into the armour. Close armour hides them; a quiver pushed out up to that much
still hangs on its straps instead of in the air.

config: {"goal": {...}, "seed": n, "length": m, "mouth_r": m, "bottom_r": m, "wall": m, "cavity": m,
         "sides": n, "hoops": [depth, ...], "tilt_back": deg, "tilt_out": deg, "reach": m, ...}
"""
import math
import sys
from pathlib import Path

import bpy
import numpy as np
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parts import Parts, args, load_goal

cfg, out = args()
goal, lo, hi = load_goal(cfg)
rng = np.random.default_rng(cfg.get("seed", 3))
keys = [z["key"] for z in goal["zones"]]
LEATHER, STRAP, IRON, INNER = (keys.index(k) for k in ("OWN_PROP_LEATHER", "OWN_PROP_STRAP", "OWN_PROP_IRON_BAND",
                                                      "OWN_PROP_QUIVER_INNER"))
P = Parts(rng, 0.1)
L, R0, R1 = cfg["length"], cfg["mouth_r"], cfg["bottom_r"]
WALL, CAVITY, SIDES = cfg.get("wall", 0.004), cfg.get("cavity", 0.3), cfg.get("sides", 24)
PH = rng.uniform(0, 2 * math.pi, 3)


def g2b(x, y, z):
    """glTF (x, y, z) -> Blender."""
    return Vector((x, -z, y))


def world_axes():
    """The world's up, the wearer's left (towards the body) and back in this frame, in Blender axes.
    The engine's Quiver::fit with its bind pose axes: right +X, up +Y, forward +Z (Gothic, left-handed);
    the frame's y = -down, z = forward made normal to y, x = y x z; glTF mirrors x."""
    tb, to = math.tan(math.radians(cfg["tilt_back"])), math.tan(math.radians(cfg["tilt_out"]))
    up, right, fwd = np.array([0, 1.0, 0]), np.array([1.0, 0, 0]), np.array([0, 0, 1.0])
    down = -up - fwd * tb + right * to
    down /= np.linalg.norm(down)
    y = -down
    z = fwd - y * fwd.dot(y)
    z /= np.linalg.norm(z)
    x = np.cross(y, z)
    frame = np.stack([x, y, z])  # rows: the frame's axes in the engine's world

    def to_blender(v):
        e = frame @ v  # components along the frame's x, y, z
        return g2b(-e[0], e[1], e[2])
    return to_blender(up), to_blender(-right), to_blender(-fwd)


UP, LEFT, BACK = world_axes()


def radius(y):
    """Outer radius at height y (0 at the mouth, -L at the bottom): a slight taper and the hoops."""
    r = R0 + (R1 - R0) * (-y / L)
    for depth in cfg.get("hoops", []):
        r += 0.0018 * math.exp(-((y + depth) / 0.012) ** 2)
    return r


def wobble(theta, y):
    """Handmade: a few millimetres out of round, changing along the tube."""
    return (0.0012 * math.sin(2 * theta + PH[0] + 4 * y) + 0.0008 * math.sin(3 * theta + PH[1] - 6 * y)
            + 0.0005 * math.sin(theta + PH[2]))


def lathe(profile, loop=False, mats=None, wob=True):
    """Revolve [(r, y), ...] about the axis. Without loop the first and last points sit on the axis (poles),
    so the shell is closed; with loop the profile is a closed ring. mats[i] is the material of segment i.
    UV in metres: u along the circumference at the mean radius, v along the profile."""
    part = P.new_part()
    n = len(profile)
    segs = n if loop else n - 1
    mats = mats or [LEATHER] * segs
    pts, v_along = [], [0.0]
    for i in range(1, n + (1 if loop else 0)):
        a, b = profile[i - 1], profile[i % n]
        v_along.append(v_along[-1] + math.hypot(b[0] - a[0], b[1] - a[1]))
    mean_r = sum(p[0] for p in profile) / n or 0.04
    for r, y in profile:
        if r == 0:
            pts.append([P.vert(g2b(0, y, 0), part)])
            continue
        ring = []
        for k in range(SIDES):
            t = 2 * math.pi * k / SIDES
            rr = r + (wobble(t, y) if wob else 0.0)
            ring.append(P.vert(g2b(rr * math.cos(t), y, rr * math.sin(t)), part))
        pts.append(ring)
    du = 2 * math.pi * mean_r / SIDES
    for i in range(segs):
        a, b = pts[i], pts[(i + 1) % n]
        va, vb = v_along[i], v_along[i + 1]
        for k in range(SIDES):
            k1 = (k + 1) % SIDES
            ua, ub = k * du, (k + 1) * du
            if len(a) == 1:
                P.poly((a[0], b[k], b[k1]), mats[i], ((ua, va), (ua, vb), (ub, vb)))
            elif len(b) == 1:
                P.poly((a[k], b[0], a[k1]), mats[i], ((ua, va), (ua, vb), (ub, va)))
            else:
                P.poly((a[k], b[k], b[k1], a[k1]), mats[i], ((ua, va), (ua, vb), (ub, vb), (ub, va)))


def sector(t0, t1, y0, y1, lift, material, cols=3):
    """A curved patch on the tube between angles t0..t1 and heights y0..y1, "lift" metres thick, closed."""
    part = P.new_part()
    grid = {}
    for side, dr in (("in", -0.001), ("out", lift)):
        for i in range(cols + 1):
            for j, y in enumerate((y0, y1)):
                t = t0 + (t1 - t0) * i / cols
                r = radius(y) + wobble(t, y) + dr
                grid[side, i, j] = P.vert(g2b(r * math.cos(t), y, r * math.sin(t)), part)
    w = (t1 - t0) * radius(y0) / cols
    for i in range(cols):
        for side in ("in", "out"):
            q = [grid[side, i, 0], grid[side, i + 1, 0], grid[side, i + 1, 1], grid[side, i, 1]]
            P.poly(q, material, ((i * w, y0), ((i + 1) * w, y0), ((i + 1) * w, y1), (i * w, y1)))
    for i in range(cols):
        for j in (0, 1):
            P.poly((grid["in", i, j], grid["in", i + 1, j], grid["out", i + 1, j], grid["out", i, j]), material,
                   ((0, 0), (w, 0), (w, lift), (0, lift)))
    for i in (0, cols):
        P.poly((grid["in", i, 0], grid["in", i, 1], grid["out", i, 1], grid["out", i, 0]), material,
               ((0, 0), (y1 - y0, 0), (y1 - y0, lift), (0, lift)))


def surface(t, y, lift=0.0):
    r = radius(y) + wobble(t, y) + lift
    return g2b(r * math.cos(t), y, r * math.sin(t))


def strap(a, b, normal_hint, width, thick, material):
    """A flat strap from a to b (Blender points), its face towards normal_hint."""
    along = b - a
    ax_len = along.normalized()
    ax_w = ax_len.cross(normal_hint).normalized()
    ax_n = ax_w.cross(ax_len).normalized()
    P.board((a + b) / 2, ax_len, ax_w, ax_n, along.length, width, thick, material, plank=0,
            v_off=float(rng.uniform(0, 0.3)))


def torus(centre, axis, major, minor, material, seg=14, tube_seg=6):
    part = P.new_part()
    axis = axis.normalized()
    e1 = axis.orthogonal().normalized()
    e2 = axis.cross(e1)
    rings = []
    for i in range(seg):
        a = 2 * math.pi * i / seg
        c = centre + (e1 * math.cos(a) + e2 * math.sin(a)) * major
        radial = (c - centre).normalized()
        rings.append([P.vert(c + (radial * math.cos(2 * math.pi * j / tube_seg) + axis * math.sin(2 * math.pi * j / tube_seg))
                             * minor, part) for j in range(tube_seg)])
    for i in range(seg):
        for j in range(tube_seg):
            i1, j1 = (i + 1) % seg, (j + 1) % tube_seg
            P.poly((rings[i][j], rings[i1][j], rings[i1][j1], rings[i][j1]), material,
                   ((i * 0.01, j * 0.005), ((i + 1) * 0.01, j * 0.005), ((i + 1) * 0.01, (j + 1) * 0.005),
                    (i * 0.01, (j + 1) * 0.005)))


# --- the tube: inner floor of the cavity, the inner wall up, the rolled rim, the outer wall down -----------
rim = cfg.get("rim", 0.007)
ys = set(np.round(np.linspace(-0.026, -L + 0.004, cfg.get("rings", 14)), 4))
for depth in cfg.get("hoops", []):  # rings on and beside each hoop, so its bulge is not lost between rings
    ys |= {-depth - 0.014, -depth, -depth + 0.014}
outer = [(radius(y), y) for y in sorted(ys, reverse=True)]
profile = ([(0.0, -CAVITY), (radius(-CAVITY) - WALL, -CAVITY)]
           + [(radius(y) - WALL, y) for y in np.linspace(-CAVITY * 0.66, -CAVITY * 0.33, 2)]
           + [(R0 - WALL, -0.012), (R0 - WALL + 0.001, 0.0), (R0 + rim * 0.35, 0.005), (R0 + rim, -0.004),
              (R0 + rim * 0.75, -0.016), (radius(-0.022) + 0.0005, -0.022)]
           + outer + [(0.0, -L + 0.004)])
# segments: floor and inner wall are the cavity (dark), the rest leather
inner_segments = 5  # floor, three wall segments, the wall's top under the rim
lathe(profile, mats=[INNER] * inner_segments + [LEATHER] * (len(profile) - 1 - inner_segments))

# the bottom cap: a stitched leather cup over the end, its open top tucked into the tube
cap_h, cr = cfg.get("cap_height", 0.045), radius(-L) + 0.003
lathe([(0.0, -L), (cr - 0.004, -L), (cr, -L + 0.004), (cr + 0.0005, -L + cap_h - 0.004),
       (cr - 0.001, -L + cap_h), (radius(-L + cap_h) - 0.003, -L + cap_h), (0.0, -L + cap_h)])

# --- the back seam (towards the body) and a patch on the outer side ------------------------------------------
seam_t = cfg.get("seam_angle", 0.0)
for y0, y1 in ((-0.03, -0.33), (-0.33, -L + cap_h)):
    a, b = surface(seam_t, y0, 0.0015), surface(seam_t, y1, 0.0015)
    strap(a, b, g2b(math.cos(seam_t), 0, math.sin(seam_t)), 0.007, 0.003, LEATHER)
pt, py = cfg.get("patch", [math.pi * 0.92, -0.30])
sector(pt - 0.85, pt + 0.65, py - 0.09, py, 0.0018, LEATHER)

# --- the hanger: a band under the mouth, a tab up to the ring, a strap from the ring under the belt -----------
by, bw = cfg.get("band", [-0.06, 0.04])
lathe([(radius(by) - 0.001, by + bw / 2), (radius(by) + 0.0035, by + bw / 2), (radius(by - bw / 2) + 0.0035, by - bw / 2),
       (radius(by - bw / 2) - 0.001, by - bw / 2)], loop=True, mats=[STRAP] * 4)
reach = cfg["reach"]
ring_c = g2b(R0 + 0.011, 0.022, 0.0)
ring_r, ring_w = 0.014, 0.0035
tab_from = g2b(radius(by) + 0.006, by - bw / 2 + 0.004, 0.0)
tab_to = ring_c - UP * (ring_r - 0.002)
strap(tab_from, tab_to, g2b(1, 0, 0), 0.026, 0.004, STRAP)
torus(ring_c, BACK, ring_r, ring_w, IRON)  # the ring stands in the plane of the tab and the belt strap
# the strap round the belt: from the ring towards the body, level in the world, a little lower as it doubles
belt_from = ring_c + LEFT * (ring_r * 0.6)
strap(belt_from, belt_from + LEFT * reach - UP * 0.004, -BACK, 0.03, 0.005, STRAP)  # face out, like the belt

# --- the tilt strap: from a stitched tab at a third of the length up to the belt behind the hip --------------
tt, ty = cfg.get("tilt_strap", [0.9, -0.23])
sector(tt - 0.32, tt + 0.32, ty - 0.035, ty + 0.01, 0.003, STRAP, cols=2)
t_from = surface(tt, ty + 0.005, 0.004)
t_to = Vector((0, 0, 0)) + LEFT * (R0 + reach) + BACK * cfg.get("tilt_back_reach", 0.07) - UP * 0.012
strap(t_from, t_to, -LEFT, 0.026, 0.004, STRAP)

metrics = P.finish(goal, out, lo, hi, extra={
    "mouth_gltf_m": [0.0, 0.0, 0.0], "axis_gltf": [0.0, -1.0, 0.0], "body_side_gltf": [1.0, 0.0, 0.0],
    "length_m": L, "mouth_d_m": 2 * R0, "bottom_d_m": 2 * R1, "cavity_m": CAVITY, "reach_m": reach,
    "world_up_blender": [round(c, 4) for c in UP], "world_left_blender": [round(c, 4) for c in LEFT]})
bpy.ops.wm.save_as_mainfile(filepath=str(out / "quiver.blend"))
