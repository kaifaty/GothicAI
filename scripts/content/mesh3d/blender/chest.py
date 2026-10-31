"""Build an arched-lid chest by parts from an image_gen concept and a numeric target.

blender -b --factory-startup -P chest.py -- chest.json OUT_DIR
config: {"id": "chest", "goal": {target with bbox, budget, zones}, "nodes": [body, lid],
         "hinge": [y, z], "seed": 1, "post": 0.06, "raise": 0.012, "plank": 0.022, "gap": 0.004,
         "planks": {"front": 4, "side": 4, "floor": 3}, "width_jitter": 0.12, "proud": 0.002,
         "band": 0.04, "arch_segments": 8, "lid_plank": 0.02, "lid_gap": 0.003, "recess": 0.004,
         "rim": [0.05, 0.045], "rim_step": 0.003, "end_board": 0.02, "strap": [0.045, 0.004], "straps_x": [-0.27, 0, 0.27], "body_strap": 0.17,
         "hinge_leaf": 0.14, "knuckle": [0.008, 0.05], "hasp": [0.065, 0.115, 0.006],
         "nail": 0.004, "nail_radius": 0.008, "stave_tile": 0.38, "planks_per_tile": 4}
The model is a MOB with two attachments, so it is exported as two GLB nodes named
after them: the body and the lid. The lid turns about the X axis of its node,
whose bind origin is "hinge" (Blender y, z), so the whole lid lies above and in
front of that line and opens without passing through the body. Faces inside the
closed chest (inner walls, floor top, lid underside) take the INNER zone: with the
lid shut their AO is near black, and their own zone target keeps them from
pulling the outside wood's measured mean down.
"""
import math
import sys
from pathlib import Path

import numpy as np
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parts import X, Y, Z, Parts, args, finish_nodes, load_goal  # noqa: E402

cfg, out = args()
goal, lo, hi = load_goal(cfg)
rng = np.random.default_rng(cfg.get("seed", 1))
keys = [z["key"] for z in goal["zones"]]
BODY, LID, INNER, BAND, LOCK = (keys.index(next(k for k in keys if k.endswith(s)))
                                for s in ("BODY", "LID", "INNER", "BAND", "LOCK"))
tile = cfg.get("stave_tile", 0.38)
plank_u = tile / cfg.get("planks_per_tile", 4)
body, lid = Parts(rng, plank_u), Parts(rng, plank_u)

cx = (lo.x + hi.x) / 2
hinge_y, hinge_z = cfg["hinge"]
zt = hinge_z                                      # the lid rests on the body's top at the hinge height
strap_w, strap_t = cfg.get("strap", [0.045, 0.004])
nail_h, nail_r = cfg.get("nail", 0.004), cfg.get("nail_radius", 0.008)
hx = (hi.x - lo.x) / 2 - nail_h                   # wood half-width: nail heads on the posts' ends meet the bbox
m = strap_t + nail_h                              # iron on the front, back and top stays inside the bbox
yf, yb = lo.y + m, hi.y - m                       # outer wood faces: front (-Y) and back (+Y)
post = cfg.get("post", 0.06)
raise_ = cfg.get("raise", 0.012)
plank_t = cfg.get("plank", 0.022)
gap = cfg.get("gap", 0.004)
jitter = cfg.get("width_jitter", 0.12)
proud = cfg.get("proud", 0.002)


def widths(span, count, avoid=()):
    """Plank widths; the gaps keep 1 cm from the opposite wall's gaps (``avoid``), so no gap sees through."""
    for _ in range(200):
        w = 1 + jitter * rng.uniform(-1, 1, count)
        w = w / w.sum() * (span - gap * (count - 1))
        seams = np.cumsum(w + gap)[:-1]
        if all(abs(s - a) > 0.01 for s in seams for a in avoid):
            return w
    raise SystemExit("no plank layout keeps its gaps away from the opposite wall")


def seams_of(w):
    return list(np.cumsum(w + gap)[:-1])


# ---------------- body ----------------
for sx in (-1, 1):
    for y, sy in ((yf + post / 2, -1), (yb - post / 2, 1)):
        c = Vector((cx + sx * (hx - post / 2), y, zt / 2))
        body.wood(c, Z, X, Y, zt, post, post, BODY, tile)
        for z in (0.06, zt - 0.06):           # square-headed nails into the planks behind, on both outer faces
            body.nail(Vector((c.x, y + sy * post / 2, z)), X, Z, Y * sy, nail_r, nail_h, BAND)
            body.nail(Vector((c.x + sx * post / 2, y, z)), Y, Z, X * sx, nail_r, nail_h, BAND)

front_planks = []
avoid = []
for face_y, sign in ((yf, -1), (yb, 1)):          # planks between the posts, recessed behind them
    span_x = 2 * hx - 2 * post                    # ends flush with the posts: a top face inside one z-fights with its top
    z = 0.0
    layout = widths(zt, cfg["planks"]["front"], avoid)
    avoid = seams_of(layout)
    for i, w in enumerate(layout):
        depth = raise_ + plank_t / 2 + rng.uniform(-proud, proud)
        part = body.parts
        body.wood(Vector((cx, face_y - sign * depth, z + w / 2)), X, Z, Y * sign, span_x, w, plank_t, BODY, tile)
        if sign < 0:
            front_planks.append(part)
        z += w + gap
avoid = []
for sx in (-1, 1):
    span_y = (yb - yf) - 2 * post
    z = 0.0
    layout = widths(zt, cfg["planks"]["side"], avoid)
    avoid = seams_of(layout)
    for w in layout:
        depth = raise_ + plank_t / 2 + rng.uniform(-proud, proud)
        body.wood(Vector((cx + sx * (hx - depth), (yf + yb) / 2, z + w / 2)), Y, Z, X * sx, span_y, w, plank_t, BODY, tile)
        z += w + gap

inner_x = hx - raise_ - plank_t                   # inner faces of the walls
inner_lo, inner_hi = yf + raise_ + plank_t, yb - raise_ - plank_t
floor_t = plank_t
y = inner_lo
for w in widths(inner_hi - inner_lo, cfg["planks"]["floor"]):  # 1 mm up: its bottom stays out of the posts' plane
    body.wood(Vector((cx, y + w / 2, floor_t / 2 + 0.001)), X, Y, Z, 2 * inner_x, w, floor_t, BODY, tile)
    y += w + gap

straps_x = [cx + s for s in cfg.get("straps_x", [-0.27, 0.0, 0.27])]
body_strap = cfg.get("body_strap", 0.17)
leaf = cfg.get("hinge_leaf", 0.14)
k_r, k_len = cfg.get("knuckle", [0.008, 0.05])
for x in (straps_x[0], straps_x[-1]):
    # front: a short strap under each outer lid strap; back: the body leaf of the hinge
    face = yf + raise_ - proud
    body.strap(Vector((x, face, zt - 0.003 - body_strap / 2)), Z, X, -Y, body_strap, strap_w, strap_t, BAND)
    for f in (0.2, 0.55, 0.9):
        body.nail(Vector((x, face - strap_t, zt - 0.003 - body_strap * f)), X, Z, -Y, nail_r, nail_h, BAND)
    face = yb - raise_ + proud
    top = zt - k_r - 0.004
    body.strap(Vector((x, face, top - leaf / 2)), Z, X, Y, leaf, strap_w, strap_t, BAND)
    for f in (0.3, 0.8):
        body.nail(Vector((x, face + strap_t, top - leaf * f)), X, Z, Y, nail_r, nail_h, BAND)
    # the knuckle: a hexagonal pin on the hinge axis, so it turns in place
    part = body.new_part()
    rings = [[body.vert(Vector((x + s * k_len / 2, hinge_y + k_r * math.cos(a), hinge_z + k_r * math.sin(a))), part)
              for a in (math.pi / 3 * i for i in range(6))] for s in (-1, 1)]
    for i in range(6):
        j = (i + 1) % 6
        body.poly((rings[0][i], rings[0][j], rings[1][j], rings[1][i]), BAND,
                  ((i * k_r, 0), ((i + 1) * k_r, 0), ((i + 1) * k_r, k_len), (i * k_r, k_len)))
    for ring in rings:
        body.poly(ring, BAND, [(k_r * math.cos(a), k_r * math.sin(a)) for a in (math.pi / 3 * i for i in range(6))])

# ---------------- lid ----------------
lid.parts = body.parts                            # part numbers stay unique after bake_atlas joins the nodes
band = cfg.get("band", 0.04)
top = hi.z - m
zc = zt + band
a, b = (yb - yf) / 2, top - zc
yc = (yf + yb) / 2
n_arch = cfg.get("arch_segments", 9)


def profile(a, b, yb, yf):
    """Outer lid outline (y, z) from the back bottom over the arch to the front bottom."""
    pts = [(yb, zt)]
    pts += [(yc + a * math.cos(t), zc + b * math.sin(t)) for t in np.linspace(0, math.pi, n_arch + 1)]
    pts.append((yf, zt))
    return pts


outer = profile(a, b, yb, yf)
lid_t = cfg.get("lid_plank", 0.02)
lid_gap = cfg.get("lid_gap", 0.003)
centre = Vector((0, yc, zt))


def normal_of(p, q):
    d = (q - p).normalized()
    n = Vector((0, d.z, -d.y))
    return n if n.dot((p + q) / 2 - centre) > 0 else -n


V = [Vector((0, y, z)) for y, z in outer]
rim_w, rim_t = cfg.get("rim", [0.05, 0.045])
recess = cfg.get("recess", 0.004)
seg_normals = [normal_of(p, q) for p, q in zip(V[:-1], V[1:])]


def seated(p, q, short):
    """Midpoint of a segment shortened by ``short``; a segment on the seam keeps its seam end, so the
    closed lid leaves no slit to see through."""
    d = (q - p).normalized()
    if abs(p.z - zt) < 1e-6:          # mathutils stores float32
        return (p + q) / 2 - d * (short / 2)
    if abs(q.z - zt) < 1e-6:
        return (p + q) / 2 + d * (short / 2)
    return (p + q) / 2
# end rims: a segmented arched frame at each end, flush with the outline. Neighbours overlap inside
# each joint, so every other segment is "rim_step" narrower on both faces: two end faces in one
# plane z-fight there (black wedges in Cycles).
rim_step = cfg.get("rim_step", 0.003)
for sx in (-1, 1):
    for k, ((p, q), n) in enumerate(zip(zip(V[:-1], V[1:]), seg_normals)):
        chord = q - p
        mid = seated(p, q, 0.002) - n * (rim_t / 2) + X * (cx + sx * (hx - rim_w / 2))
        lid.wood(mid, chord.normalized(), X, n, chord.length - 0.002, rim_w - 2 * rim_step * (k % 2), rim_t, LID, tile)
# planks between the rims, a little below their outer face, their ends hidden in the rims
for (p, q), n in zip(zip(V[:-1], V[1:]), seg_normals):
    chord = q - p
    mid = seated(p, q, lid_gap) - n * (recess + lid_t / 2) + X * cx
    lid.wood(mid, X, chord.normalized(), n, 2 * (hx - rim_w) + 0.02, chord.length - lid_gap, lid_t, LID, tile)

# end panels: a slab filling each rim's opening, set back from its outer face
inner_outline = profile(a - rim_t, b - rim_t, yb - rim_t, yf + rim_t)
panel_t = cfg.get("end_board", 0.02)
panel_x = hx - rim_w / 2
for sx in (-1, 1):
    part = lid.new_part()
    caps = [[lid.vert(Vector((cx + sx * (panel_x + d), y, z)), part) for y, z in inner_outline]
            for d in (panel_t / 2, -panel_t / 2)]
    n = len(inner_outline)
    for cap in caps:
        lid.poly(cap, LID, [(y, z) for y, z in inner_outline])
    run = 0.0
    for i in range(n):
        j = (i + 1) % n
        step = (Vector(inner_outline[j]) - Vector(inner_outline[i])).length
        lid.poly((caps[0][i], caps[0][j], caps[1][j], caps[1][i]), LID,
                 ((run, 0), (run + step, 0), (run + step, panel_t), (run, panel_t)))
        run += step

# iron straps over the arch, a ribbon on the planks' outer faces, rivets at every other joint
normals = []
for i, p in enumerate(V):
    ns = [normal_of(V[j], V[j + 1]) for j in (i - 1, i) if 0 <= j < len(V) - 1]
    normals.append(sum(ns, Vector()).normalized())
for x in straps_x:
    part = lid.new_part()
    rings = []
    for p, n in zip(V, normals):
        base = p + X * x - n * recess
        rings.append([lid.vert(base + X * (s * strap_w / 2) + n * d, part)
                      for s, d in ((-1, 0), (1, 0), (1, strap_t), (-1, strap_t))])
    run = 0.0
    for i in range(len(rings) - 1):
        step = (V[i + 1] - V[i]).length
        for k in range(4):
            l = (k + 1) % 4
            lid.poly((rings[i][k], rings[i][l], rings[i + 1][l], rings[i + 1][k]), BAND,
                     ((k * strap_w, run), ((k + 1) * strap_w, run), ((k + 1) * strap_w, run + step), (k * strap_w, run + step)))
        run += step
    for ring in (rings[0], rings[-1]):
        lid.poly(ring, BAND, [(0, 0), (strap_w, 0), (strap_w, strap_t), (0, strap_t)])
    for i in range(1, len(V) - 1, 2):
        lid.nail(V[i] + X * x + normals[i] * (strap_t - recess), X, (V[i + 1] - V[i - 1]).normalized(), normals[i], nail_r, nail_h, BAND)

# hasp: a plate hanging from the front lid edge over the middle of the body front
hasp_w, hasp_h, hasp_t = cfg.get("hasp", [0.065, 0.115, 0.006])
hasp_top = zt + 0.015
lid.board(Vector((straps_x[1], yf - hasp_t / 2, hasp_top - hasp_h / 2)), Z, X, -Y, hasp_h, hasp_w, hasp_t, LOCK)
lid.board(Vector((straps_x[1], yf - hasp_t - 0.0005, hasp_top - hasp_h * 0.62)), Z, X, -Y, 0.022, 0.007, 0.002, INNER)  # keyhole
for f in (0.25, 0.75):
    lid.nail(Vector((straps_x[1] - hasp_w / 2 + hasp_w * f, yf - hasp_t, hasp_top - 0.012)), X, Z, -Y, nail_r * 0.8, nail_h * 0.8, LOCK)


# ---------------- interior faces ----------------
def mark_inner(P, inside):
    eps = 0.002
    count = 0
    for face in P.bm.faces:
        c = face.calc_center_median()
        if face.material_index in (BODY, LID) and inside(c, eps):
            face.material_index = INNER
            count += 1
    return count


def in_body(c, eps):
    return (abs(c.x - cx) < inner_x + eps and inner_lo - eps < c.y < inner_hi + eps
            and floor_t - eps < c.z < zt + eps)


inset = recess + lid_t
inner_poly = [Vector((y, z)) for y, z in profile(a - inset, b - inset, yb - inset, yf + inset)]


def in_lid(c, eps):
    eps = 2.5 * eps                      # plank inner faces are chords, the test outline an arc
    if abs(c.x - cx) > panel_x - panel_t / 2 + eps or c.z < zt - eps:
        return False
    # inside the convex inner outline, widened by eps (the outline runs one way round)
    p = Vector((c.y, c.z))
    sign = 0
    for i in range(len(inner_poly)):
        e0, e1 = inner_poly[i], inner_poly[(i + 1) % len(inner_poly)]
        d, r = e1 - e0, p - e0
        cross = d.x * r.y - d.y * r.x
        dist = cross / max(d.length, 1e-9)
        if sign == 0:
            sign = 1 if (inner_poly[(i + 2) % len(inner_poly)] - e0).x * d.y - (inner_poly[(i + 2) % len(inner_poly)] - e0).y * d.x < 0 else -1
        if sign * dist < -eps:
            return False
    return True


inner_faces = mark_inner(body, in_body) + mark_inner(lid, in_lid)

# ---------------- checks: the lid opens about the hinge without entering the body ----------------
lid_co = np.array([v.co[:] for v in lid.bm.verts])
rel_y, rel_z = lid_co[:, 1] - hinge_y, lid_co[:, 2] - hinge_z
angle = math.radians(cfg.get("open_angle", 35.2))
open_z = rel_z * math.cos(angle) - rel_y * math.sin(angle)   # front (negative rel_y) goes up
extra = {"front_planks_parts": front_planks, "inner_faces": inner_faces,
         "lid_behind_hinge_m": round(float(rel_y.max()), 4), "lid_below_hinge_m": round(float(-rel_z.min()), 4),
         "lid_open_lowest_m": round(float(open_z.min()), 4)}
finish_nodes(goal, out, lo, hi, [(cfg["nodes"][0], body), (cfg["nodes"][1], lid)], extra)
