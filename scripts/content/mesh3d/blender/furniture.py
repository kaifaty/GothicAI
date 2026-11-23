"""Build one piece of the house furniture set (asset city-furniture) over its original's measured form.

blender -b --factory-startup -P furniture.py -- furniture-<item>.json OUT_DIR   (run from the repository root)
config: {"id": target id in data/content/mesh3d-furniture.json, "target": that file, "kind": builder name, "seed": n, ...}
kinds: stool, stool-rich, armchair, bed, bed-rich, bed-master, dresser, dresser-rich, sideboard, lectern, table,
table-rich, bench.

reports/content/remaster/city-furniture: the user's decision of 2026-11-02 («Да, по мешу»). The concepts give the
material and the character (concept-<item> in asset.json); sizes, profiles, tier heights and the places of iron come
from the original's mesh, read as a height profile (intent.md, «Главная деталь»): numbers here are those readings.
glTF metres, the front is -z (where the npc stands at ZS_POS0), y up. No vertex of the original is copied.

Detail (the user, 2026-11-02: the first build was as coarse as the original, «сделать более современными»): every
hard edge of wood, iron and stone is rounded (round_edges) and the normals are weighted by face area, so the big
faces stay flat and the rounded edges catch the light; seats, shields and panels are separate boards; tops,
cornices and plinths are swept mouldings; rivets are domes; strap hinges are forged ribbons ending in curls;
cloth and leather are dense enough to sag and fold.
"""
import math
import sys
from pathlib import Path

import bmesh
import numpy as np
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parts import Parts, args, load_goal  # noqa: E402
from hearth_parts import turned  # noqa: E402

cfg, out = args()
goal, lo, hi = load_goal(cfg)
rng = np.random.default_rng(cfg.get("seed", 5))
keys = [z["key"] for z in goal["zones"]]
Z = {k.replace("OWN_FURN_", ""): i for i, k in enumerate(keys)}
P = Parts(rng, 0.1)
SOFT = set()  # parts already smooth (cushions, piping, beads): round_edges leaves them alone
FRONT = Vector((0, 1, 0))  # glTF -z in Blender axes: the face toward the npc
UP = Vector((0, 0, 1))

# Edge rounding per zone: (radius m, segments). Wood is planed by hand to about 5 mm; forged iron 1.5 mm; dressed
# stone 12 mm. A part thinner than four radii gets a quarter of its thickness.
EDGE = {"WOOD": (0.005, 3), "DARKWOOD": (0.005, 3), "OAK": (0.005, 3), "IRON": (0.0015, 2), "BRASS": (0.0015, 2),
        "STONE": (0.012, 2), "LEATHER": (0.01, 3), "TRIM": (0.002, 2), "PAPER": (0.0008, 1)}


def b(x, y, z):
    """glTF metres to Blender axes."""
    return Vector((x, -z, y))


def d(x, y, z):
    """A glTF direction in Blender axes."""
    return Vector((x, -z, y))


def zone(name):
    return Z[name]


# --- primitives ----------------------------------------------------------------------------------------------------

def box(x0, x1, y0, y1, z0, z1, mat, bevel=0.0):
    """An axis-aligned box in glTF metres; bevel > 0 cuts its vertical edges at 45 degrees (a chamfer)."""
    x0, x1 = sorted((x0, x1))
    z0, z1 = sorted((z0, z1))
    if bevel <= 0:
        P.block(b((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2), (x1 - x0, z1 - z0, y1 - y0), mat, 1.0)
        return
    c = bevel
    prism([(x0 + c, z0), (x1 - c, z0), (x1, z0 + c), (x1, z1 - c), (x1 - c, z1), (x0 + c, z1), (x0, z1 - c), (x0, z0 + c)], y0, y1, mat)


def prism(outline, y0, y1, mat, top=True, bottom=True):
    """A closed vertical prism over a convex outline [(x, z)] in glTF metres, from y0 up to y1. UV in metres."""
    part = P.new_part()
    lo_ = [P.vert(b(x, y0, z), part) for x, z in outline]
    hi_ = [P.vert(b(x, y1, z), part) for x, z in outline]
    n = len(outline)
    run = 0.0
    for i in range(n):
        j = (i + 1) % n
        dd = math.dist(outline[i], outline[j])
        P.poly([lo_[i], lo_[j], hi_[j], hi_[i]], mat, [(run, y0), (run + dd, y0), (run + dd, y1), (run, y1)])
        run += dd
    # wind the caps by the outline's orientation (counter-clockwise seen from +y means clockwise in x,z)
    area = sum(outline[i][0] * outline[(i + 1) % n][1] - outline[(i + 1) % n][0] * outline[i][1] for i in range(n))
    cap_hi = hi_ if area < 0 else hi_[::-1]
    cap_lo = lo_[::-1] if area < 0 else lo_
    if top:
        P.poly(cap_hi, mat, [(v.co.x, v.co.y) for v in cap_hi])
    if bottom:
        P.poly(cap_lo, mat, [(v.co.x, v.co.y) for v in cap_lo])
    return part


def slab(profile, axis, a0, a1, mat):
    """A closed slab extruded along glTF x (axis 'x') or z (axis 'z') from a0 to a1 over a convex profile
    [(u, y)], u being z (axis x) or x (axis z): a bed's shield, a crest."""
    part = P.new_part()

    def at(u, y, a):
        return b(a, y, u) if axis == "x" else b(u, y, a)
    s0 = [P.vert(at(u, y, a0), part) for u, y in profile]
    s1 = [P.vert(at(u, y, a1), part) for u, y in profile]
    n = len(profile)
    run = 0.0
    for i in range(n):
        j = (i + 1) % n
        dd = math.dist(profile[i], profile[j])
        P.poly([s0[i], s0[j], s1[j], s1[i]], mat, [(run, 0), (run + dd, 0), (run + dd, a1 - a0), (run, a1 - a0)])
        run += dd
    P.poly(s0[::-1], mat, [p for p in profile[::-1]])
    P.poly(s1, mat, [p for p in profile])
    return part


def clip(poly, k, a, c):
    """The part of a convex polygon [(p0, p1)] whose coordinate k lies between a and c (a board cut from a seat)."""
    def cut(pts, val, keep_ge):
        res = []
        for i in range(len(pts)):
            p, q = pts[i], pts[(i + 1) % len(pts)]
            ip = p[k] >= val if keep_ge else p[k] <= val
            iq = q[k] >= val if keep_ge else q[k] <= val
            if ip:
                res.append(p)
            if ip != iq:
                t = (val - p[k]) / (q[k] - p[k])
                res.append((p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t))
        return res
    return cut(cut(poly, a, True), c, False)


def soft(cx, cy, cz, rx, ry, rz, mat, e=0.25, nu=16, nv=10, sag=None):
    """A soft closed superellipsoid (cushion, mattress, roll) centred at (cx, cy, cz) with half sizes r; e near 0
    makes it boxy with rounded edges. sag(x, z) may lower the top (a mattress dented by use)."""
    part = P.new_part()
    SOFT.add(part)

    def f(w, m):
        return math.copysign(abs(w) ** m, w)
    rings = []
    for j in range(1, nv):
        v = -math.pi / 2 + math.pi * j / nv
        ring = []
        for i in range(nu):
            u = -math.pi + 2 * math.pi * i / nu
            x = rx * f(math.cos(v), e) * f(math.cos(u), e)
            z = rz * f(math.cos(v), e) * f(math.sin(u), e)
            y = ry * f(math.sin(v), e)
            if sag is not None and y > 0:
                y -= sag(x, z) * (y / ry)
            ring.append(P.vert(b(cx + x, cy + y, cz + z), part))
        rings.append(ring)
    top_y = ry - (sag(0, 0) if sag is not None else 0)
    bottom, top = P.vert(b(cx, cy - ry, cz), part), P.vert(b(cx, cy + top_y, cz), part)
    span = 2 * (rx + rz)
    for j in range(len(rings) - 1):
        for i in range(nu):
            k = (i + 1) % nu
            P.poly([rings[j][i], rings[j][k], rings[j + 1][k], rings[j + 1][i]], mat,
                   [(i / nu * span, j / nv * 2 * ry), ((i + 1) / nu * span, j / nv * 2 * ry),
                    ((i + 1) / nu * span, (j + 1) / nv * 2 * ry), (i / nu * span, (j + 1) / nv * 2 * ry)])
    for i in range(nu):
        k = (i + 1) % nu
        P.poly([bottom, rings[0][k], rings[0][i]], mat, [(0, 0), (0.05, 0.05), (0, 0.05)])
        P.poly([top, rings[-1][i], rings[-1][k]], mat, [(0, 0), (0.05, 0.05), (0, 0.05)])
    return part


def superellipse(rx, rz, e, n):
    """Points of the equator of soft(..., e) with half sizes rx, rz: where its piping runs."""
    f = lambda w: math.copysign(abs(w) ** e, w)  # noqa: E731
    return [(rx * f(math.cos(t)), rz * f(math.sin(t))) for t in (-math.pi + 2 * math.pi * i / n for i in range(n))]


def bead(c, ax, ay, az, r, mat, nu=6, nv=4):
    """A small ellipsoid at Blender point c with unit axes ax, ay, az and radii r: a bead of a carved rope."""
    part = P.new_part()
    SOFT.add(part)
    rings = []
    for j in range(1, nv):
        v = -math.pi / 2 + math.pi * j / nv
        rings.append([P.vert(c + ax * (r[0] * math.cos(v) * math.cos(u)) + ay * (r[1] * math.cos(v) * math.sin(u))
                             + az * (r[2] * math.sin(v)), part) for u in (2 * math.pi * i / nu for i in range(nu))])
    poles = P.vert(c - az * r[2], part), P.vert(c + az * r[2], part)
    for j in range(len(rings) - 1):
        for i in range(nu):
            k = (i + 1) % nu
            P.poly([rings[j][i], rings[j][k], rings[j + 1][k], rings[j + 1][i]], mat, [(0, 0), (0.01, 0), (0.01, 0.01), (0, 0.01)])
    for i in range(nu):
        k = (i + 1) % nu
        P.poly([poles[0], rings[0][k], rings[0][i]], mat, [(0, 0), (0.01, 0), (0, 0.01)])
        P.poly([poles[1], rings[-1][i], rings[-1][k]], mat, [(0, 0), (0.01, 0), (0, 0.01)])


def tube(points, r, mat, ref, sides=8, closed=True):
    """A round cord through Blender points (piping, a tie, a ring handle); ref is a direction never along the cord."""
    part = P.new_part()
    SOFT.add(part)
    n = len(points)
    rings = []
    for i, p in enumerate(points):
        if closed:
            t = points[(i + 1) % n] - points[i - 1]
        else:
            t = points[min(i + 1, n - 1)] - points[max(i - 1, 0)]
        t.normalize()
        a = t.cross(ref).normalized()
        c = t.cross(a)
        rings.append([P.vert(p + (a * math.cos(u) + c * math.sin(u)) * r, part)
                      for u in (2 * math.pi * k / sides for k in range(sides))])
    run = 0.0
    for i in range(n if closed else n - 1):
        j = (i + 1) % n
        ln = (points[j] - points[i]).length
        for k in range(sides):
            m = (k + 1) % sides
            P.poly([rings[i][k], rings[i][m], rings[j][m], rings[j][k]], mat,
                   [(k * 0.01, run), ((k + 1) * 0.01, run), ((k + 1) * 0.01, run + ln), (k * 0.01, run + ln)])
        run += ln
    if not closed:
        P.poly(rings[0][::-1], mat, [(0, 0)] * sides)
        P.poly(rings[-1], mat, [(0, 0)] * sides)


def circle(cx, cz, r, n=16):
    return [(cx + r * math.cos(2 * math.pi * k / n), cz + r * math.sin(2 * math.pi * k / n)) for k in range(n)]


def disc(x, y, z, r, depth, mat, n=16):
    """A round boss, knob or rivet plate on a front face: a short cylinder along glTF z, its face toward -z."""
    outline = [(r * math.cos(2 * math.pi * k / n), r * math.sin(2 * math.pi * k / n)) for k in range(n)]
    part = P.new_part()
    back = [P.vert(b(x + u, y + v, z), part) for u, v in outline]
    front = [P.vert(b(x + u, y + v, z - depth), part) for u, v in outline]
    for i in range(n):
        j = (i + 1) % n
        P.poly([back[i], back[j], front[j], front[i]], mat, [(0, 0), (0.01, 0), (0.01, depth), (0, depth)])
    P.poly(front[::-1], mat, [(u, v) for u, v in outline[::-1]])
    P.poly(back, mat, [(u, v) for u, v in outline])
    return part


def dome(p, nrm, r, mat, h=None, sides=8):
    """A domed head (rivet, nail, peg end, boss) on a surface at Blender point p, bulging along nrm; its open base
    sits 1 mm in the surface."""
    h = r * 0.55 if h is None else h
    nrm = nrm.normalized()
    a = nrm.cross(UP)
    if a.length < 1e-3:
        a = nrm.cross(Vector((1, 0, 0)))
    a.normalize()
    c = nrm.cross(a)  # a, c, nrm right-handed: the rings run counter-clockwise seen from the tip
    part = P.new_part(wound=True)
    rings = [[P.vert(p + nrm * lift + (a * math.cos(t) + c * math.sin(t)) * (r * scale), part)
              for t in (2 * math.pi * k / sides for k in range(sides))]
             for lift, scale in ((-0.001, 1.0), (0.55 * h, 0.82), (0.88 * h, 0.45))]
    tip = P.vert(p + nrm * h, part)
    for j in range(2):
        for k in range(sides):
            m = (k + 1) % sides
            P.poly([rings[j][k], rings[j][m], rings[j + 1][m], rings[j + 1][k]], mat, [(0, 0), (r, 0), (r, h), (0, h)])
    for k in range(sides):
        P.poly([rings[2][k], rings[2][(k + 1) % sides], tip], mat, [(0, 0), (r, 0), (r / 2, h)])


def rivets(points, normals, mat, r=0.007):
    for p, n in zip(points, normals):
        dome(p, n, r, mat, sides=6 if r < 0.008 else 8)


def octagon(flat, rot=math.pi / 8):
    """An octagon whose flats face the axes at distance flat."""
    r = flat / math.cos(math.pi / 8)
    return [(r * math.cos(rot + k * math.pi / 4), r * math.sin(rot + k * math.pi / 4)) for k in range(8)]


def ribbon(path, widths, at, thick, mat):
    """A forged flat strap along path [(u, v)] in a surface; widths per station; at(u, v, lift) maps to Blender
    space. Closed, so its edges are rounded like the rest of the iron."""
    part = P.new_part()
    n = len(path)
    st = []
    for i in range(n):
        p0, p1 = path[max(i - 1, 0)], path[min(i + 1, n - 1)]
        tu, tv = p1[0] - p0[0], p1[1] - p0[1]
        ln = math.hypot(tu, tv)
        su, sv = -tv / ln, tu / ln
        w = widths[i] / 2
        u, v = path[i]
        st.append([P.vert(at(u + su * w * s, v + sv * w * s, lift), part) for s, lift in ((1, -0.001), (-1, -0.001), (-1, thick), (1, thick))])
    run = 0.0
    for i in range(n - 1):
        ln = math.dist(path[i], path[i + 1])
        for k in range(4):
            j = (k + 1) % 4
            P.poly([st[i][k], st[i][j], st[i + 1][j], st[i + 1][k]], mat,
                   [(k * 0.01, run), ((k + 1) * 0.01, run), ((k + 1) * 0.01, run + ln), (k * 0.01, run + ln)])
        run += ln
    P.poly(st[0], mat, [(0, 0), (0.01, 0), (0.01, 0.01), (0, 0.01)])
    P.poly(st[-1][::-1], mat, [(0, 0), (0.01, 0), (0.01, 0.01), (0, 0.01)])


def hinge(xk, xf, y, z, mat, w=0.035, curl=True):
    """A forged strap hinge on a door front (plane z, front -z): a knuckle at xk on the door's hinge edge, the strap
    tapering toward xf and ending in two curls, one up and one down; domed rivets along it."""
    at = lambda u, v, lift: b(u, v, z - lift)  # noqa: E731
    sx = 1 if xf > xk else -1
    span = abs(xf - xk)
    ts = np.linspace(0, 1, 12)
    end_w = w * (0.55 if curl else 0.45)
    ribbon([(xk + sx * span * t, y) for t in ts], [w + (end_w - w) * t for t in ts], at, 0.005, mat)
    if curl:
        R = w * 0.55
        for up in (1, -1):
            pts, ws = [], []
            for t in np.linspace(0, 1, 18):
                ang = -up * math.pi / 2 + sx * up * t * 1.6 * math.pi
                r = R * (1 - 0.6 * t)
                pts.append((xf + r * math.cos(ang), y + up * R + r * math.sin(ang)))
                ws.append(end_w * 0.5 * (1 - 0.45 * t))
            ribbon(pts, ws, at, 0.0045, mat)
    prism(circle(xk, z - 0.011, 0.011), y - 0.6 * w, y + 0.6 * w, mat)  # the knuckle
    rivets([at(xk + sx * span * t, y, 0.005) for t in (0.18, 0.5, 0.8)], [FRONT] * 3, mat, 0.0055)


def ring_pull(x, y, z, mat, r=0.032):
    """A ring handle hanging from a domed rose on a door front (plane z)."""
    disc(x, y, z, 0.022, 0.004, mat)
    dome(b(x, y, z - 0.004), FRONT, 0.016, mat, h=0.01)
    tube([b(x + r * math.sin(t), y - r + r * math.cos(t), z - 0.016) for t in np.linspace(0, 2 * math.pi, 18, endpoint=False)],
         0.0045, mat, FRONT, sides=6)


def knob(x, y, z, mat):
    """A small turned knob on a door front."""
    disc(x, y, z, 0.010, 0.016, mat)
    dome(b(x, y, z - 0.016), FRONT, 0.017, mat, h=0.014)


def moulding(outline, profile, mat):
    """Sweep a profile [(dd, y)] round a convex plan outline [(x, z)], dd the offset outward from the outline:
    cornices, plinths, the rounded edge of a top. The first and last rows are capped flat."""
    part = P.new_part()
    n = len(outline)
    area = sum(outline[i][0] * outline[(i + 1) % n][1] - outline[(i + 1) % n][0] * outline[i][1] for i in range(n))
    sgn = 1 if area > 0 else -1
    normals = []
    for i in range(n):
        (x0, z0), (x1, z1) = outline[i], outline[(i + 1) % n]
        ln = math.hypot(x1 - x0, z1 - z0)
        normals.append((sgn * (z1 - z0) / ln, -sgn * (x1 - x0) / ln))
    miter = []
    for i in range(n):
        a, c = normals[i - 1], normals[i]
        k = 1 + a[0] * c[0] + a[1] * c[1]
        miter.append(((a[0] + c[0]) / k, (a[1] + c[1]) / k))
    rings = [[P.vert(b(x + mx * dd, y, z + mz * dd), part) for (x, z), (mx, mz) in zip(outline, miter)] for dd, y in profile]
    runs = [0.0]
    for i in range(n):
        runs.append(runs[-1] + math.dist(outline[i], outline[(i + 1) % n]))
    pv = 0.0
    for r in range(len(profile) - 1):
        step = math.dist(profile[r], profile[r + 1])
        for i in range(n):
            j = (i + 1) % n
            P.poly([rings[r][i], rings[r][j], rings[r + 1][j], rings[r + 1][i]], mat,
                   [(runs[i], pv), (runs[i + 1], pv), (runs[i + 1], pv + step), (runs[i], pv + step)])
        pv += step
    for ring in (rings[0], rings[-1]):
        P.poly(ring, mat, [(v.co.x, v.co.y) for v in ring])
    return part


def roundover(D, y0, y1, r):
    """A moulding profile: a top or ledge standing D proud, its upper outer edge rounded with radius r."""
    return ([(0, y0), (D, y0), (D, y1 - r)] + [(D - r + r * math.cos(a), y1 - r + r * math.sin(a)) for a in np.linspace(0, math.pi / 2, 5)[1:]]
            + [(0, y1)])


def cornice(D, y0, y1):
    """A moulding profile: a fillet, a cove, a flat band and a rounded top edge, D proud of the outline."""
    rc = min(0.6 * D, 0.45 * (y1 - y0))
    f = (D - rc) / 2
    yc = y0 + 0.1 * (y1 - y0)
    cove = [(f + rc - rc * math.cos(a), yc + rc * math.sin(a)) for a in np.linspace(0, math.pi / 2, 5)]
    r = min(0.01, 0.2 * (y1 - y0))
    top = [(D - r + r * math.cos(a), y1 - r + r * math.sin(a)) for a in np.linspace(0, math.pi / 2, 4)]
    return [(0, y0), (f, y0), (f, yc)] + cove + [(D, yc + rc)] + top + [(0, y1)]


def plinth(D, y0, y1):
    """A moulding profile: a plinth D proud, its upper edge a quarter round back to the outline."""
    r = min(D, 0.5 * (y1 - y0))
    return [(0, y0), (D, y0), (D, y1 - r)] + [(r * math.cos(a) + D - r, y1 - r + r * math.sin(a)) for a in np.linspace(0, math.pi / 2, 5)[1:]] + [(0, y1)]


def band_around(outline, y0, y1, mat, grow=0.006, nails=2):
    """A forged iron band bound round a prism edge (a stool seat, a table top) with domed rivets on each side."""
    big = [(x * (1 + grow / max(math.hypot(x, z), 1e-6)), z * (1 + grow / max(math.hypot(x, z), 1e-6))) for x, z in outline]
    prism(big, y0, y1, mat)
    pts, ns = [], []
    n = len(big)
    for i in range(n):
        a, c = big[i], big[(i + 1) % n]
        mid_y = (y0 + y1) / 2
        nx, nz = (c[1] - a[1]), -(c[0] - a[0])
        ln = math.hypot(nx, nz)
        if (a[0] + c[0]) * nx + (a[1] + c[1]) * nz < 0:  # outward: away from the centre
            nx, nz = -nx, -nz
        for t in np.linspace(0.25, 0.75, nails) if nails > 1 else (0.5,):
            x, z = a[0] + (c[0] - a[0]) * t, a[1] + (c[1] - a[1]) * t
            pts.append(b(x, mid_y, z))
            ns.append(d(nx / ln, 0, nz / ln))
    rivets(pts, ns, mat)


def ball_feet(points, y0, r, mat):
    for x, z in points:
        turned(P, b(x, y0, z), [(0, r * 0.5), (r * 0.4, r * 0.95), (r, r), (r * 1.6, r * 0.95), (r * 2, r * 0.55)], [], 16, mat)


def plank_door(x0, x1, y0, y1, z, mat, boards=4, arch=0.0, nails=None):
    """A door of vertical boards standing proud of the carcass front plane z (front is -z); arch > 0 raises its
    middle top into an arch of that height. nails: the material of two nail heads at each board's top and foot,
    where the boards are nailed to the ledges behind."""
    w = (x1 - x0) / boards
    for k in range(boards):
        a, c = x0 + k * w + 0.002, x0 + (k + 1) * w - 0.002
        mid = (a + c) / 2 - (x0 + x1) / 2
        rise = arch * max(0.0, 1 - (2 * mid / (x1 - x0)) ** 2) if arch else 0.0
        top = y1 - arch + rise
        if arch:  # the board's top follows the arch: a slab whose profile is cut by the arc
            half = (x1 - x0) / 2
            cx = (x0 + x1) / 2
            arc = lambda u: y1 - arch + arch * max(0.0, 1 - ((u - cx) / half) ** 2)  # noqa: E731
            prof = [(a, y0), (c, y0)] + [(u, arc(u)) for u in np.linspace(c, a, 4)]
            slab(prof, "z", z - 0.022, z, mat)
        else:
            box(a, c, y0, top, z - 0.022, z, mat)
        if nails is not None:
            for yy in (y0 + 0.06, top - 0.07):
                rivets([b((a + c) / 2 + s * w * 0.22, yy, z - 0.022) for s in (-1, 1)], [FRONT] * 2, nails, 0.005)


def face_frame(x0, x1, y0, y1, z, rail, stile, mat, proud=0.012):
    """Stiles and rails round a door opening on a carcass front (plane z, front -z)."""
    box(x0 - stile, x0, y0 - rail, y1 + rail, z - proud, z, mat)
    box(x1, x1 + stile, y0 - rail, y1 + rail, z - proud, z, mat)
    box(x0, x1, y0 - rail, y0, z - proud, z, mat)
    box(x0, x1, y1, y1 + rail, z - proud, z, mat)


def side_boards(x, y0, y1, z0, z1, mat, boards=4, proud=0.005):
    """The side of a carcass shown as boards (x the side face; the boards stand proud toward the outside)."""
    s = 1 if x > 0 else -1
    edges = np.linspace(z0, z1, boards + 1)
    for k in range(boards):
        box(x, x + s * proud, y0, y1 - rng.uniform(0, 0.004), edges[k] + 0.0015, edges[k + 1] - 0.0015, mat)


def rope(outline, y, mat, step=0.03, grow=0.003):
    """A carved rope round a convex outline [(x, z)] at height y: beads slanted at 45 degrees."""
    n = len(outline)
    for i in range(n):
        (x0, z0), (x1, z1) = outline[i], outline[(i + 1) % n]
        ln = math.hypot(x1 - x0, z1 - z0)
        t = d((x1 - x0) / ln, 0, (z1 - z0) / ln)
        nrm = d((z1 - z0) / ln, 0, -(x1 - x0) / ln)
        if nrm.dot(d((x0 + x1) / 2, 0, (z0 + z1) / 2)) < 0:
            nrm = -nrm
        ax = (t + UP).normalized()
        az = (UP - t).normalized()
        for k in range(int(ln / step)):
            s = (k + 0.5) * ln / int(ln / step)
            c = b(x0 + (x1 - x0) * s / ln, y, z0 + (z1 - z0) * s / ln) + nrm * grow
            bead(c, ax, nrm, az, (0.024, 0.009, 0.011), mat)


def sheet(grid, out, mat, thick=0.004, du=0.05, dv=0.05):
    """Two-sided cloth from a grid of Blender points [row][col] (curtains, a valance); out is the side of the first
    layer. Each layer is wound by hand: a lone cloth sheet has no inside for the recalculation to find."""
    R, C = len(grid), len(grid[0])
    for side in (1, -1):
        part = P.new_part(wound=True)
        vs = [[P.vert(p + out * (side * thick / 2), part) for p in row] for row in grid]
        for r in range(R - 1):
            for k in range(C - 1):
                q = [vs[r][k], vs[r][k + 1], vs[r + 1][k + 1], vs[r + 1][k]]
                nrm = (q[1].co - q[0].co).cross(q[3].co - q[0].co)
                if nrm.dot(out) * side < 0:
                    q = q[::-1]
                P.poly(q, mat, [(k * du, r * dv), ((k + 1) * du, r * dv), ((k + 1) * du, (r + 1) * dv), (k * du, (r + 1) * dv)])


def pyramid_cap(x, z, y, half, rise, mat):
    part = P.new_part()
    base = [P.vert(b(x + dx * half, y, z + dz * half), part) for dx, dz in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
    tip = P.vert(b(x, y + rise, z), part)
    for i in range(4):
        P.poly([base[i], base[(i + 1) % 4], tip], mat, [(0, 0), (2 * half, 0), (half, rise)])
    P.poly(base, mat, [(0, 0), (1, 0), (1, 1), (0, 1)])


def square(x, z, h):
    return [(x - h, z - h), (x + h, z - h), (x + h, z + h), (x - h, z + h)]


# --- finishing -----------------------------------------------------------------------------------------------------

def round_edges():
    """Round every hard edge of the closed hard parts (EDGE radii); then rough up the dressed stone."""
    bm = P.bm
    layer = P.part_layer
    pid = lambda f: round(f.verts[0][layer])  # noqa: E731
    bmesh.ops.recalc_face_normals(bm, faces=[f for f in bm.faces if pid(f) not in P.wound])
    bm.normal_update()
    ext = {}
    for v in bm.verts:
        lo_, hi_ = ext.setdefault(round(v[layer]), ([1e9] * 3, [-1e9] * 3))
        for i in range(3):
            lo_[i] = min(lo_[i], v.co[i])
            hi_[i] = max(hi_[i], v.co[i])
    groups = {}
    for e in bm.edges:
        if len(e.link_faces) != 2:
            continue
        f0, f1 = e.link_faces
        k = pid(f0)
        if k in P.wound or k in SOFT or f0.material_index != f1.material_index:
            continue
        if f0.normal.angle(f1.normal, 0.0) < math.radians(25):
            continue
        name = keys[f0.material_index].replace("OWN_FURN_", "")
        if name not in EDGE:
            continue
        w, seg = EDGE[name]
        size = min(h - l for l, h in zip(*ext[k]))
        if size < 4 * w:
            w, seg = size / 4, (min(seg, 2) if size >= 0.01 else 1)
        groups.setdefault((round(w, 5), seg), []).append(e)
    for (w, seg), edges in groups.items():
        edges = [e for e in edges if e.is_valid]
        bmesh.ops.bevel(bm, geom=edges, offset=w, offset_type="OFFSET", profile_type="SUPERELLIPSE", segments=seg,
                        profile=0.5, affect="EDGES", clamp_overlap=True)
    # Wind every part by the sign of its volume and freeze it: the export's own recalculation judges a part by
    # its outermost face, which after rounding is often a sliver of a bevel corner, and turned the rich dresser's
    # carcass and half its door boards inside out (the engine culled them: the lower tier was see-through).
    faces = {}
    for f in bm.faces:
        faces.setdefault(pid(f), []).append(f)
    for k, fs in faces.items():
        if k in P.wound:
            continue
        vol = sum(sum(f.verts[0].co.dot(f.verts[i].co.cross(f.verts[i + 1].co)) for i in range(1, len(f.verts) - 1)) for f in fs)
        if vol < 0:
            bmesh.ops.reverse_faces(bm, faces=fs)
        P.wound.add(k)
    if "STONE" in Z:
        stone = {f for f in bm.faces if f.material_index == Z["STONE"]}
        for v in {v for f in stone for v in f.verts}:
            v.co += Vector(rng.normal(0, 0.0025, 3))


def prepare(obj):
    """Smooth the rounded edges and weight the normals by face area: big faces flat, edges catch the light."""
    obj.data.set_sharp_from_angle(angle=math.radians(50))
    mod = obj.modifiers.new("weighted", "WEIGHTED_NORMAL")
    mod.mode = "FACE_AREA"
    mod.keep_sharp = True
    mod.weight = 50


# --- builders ------------------------------------------------------------------------------------------------------

def stool():
    W, I = zone("WOOD"), zone("IRON")
    seat = octagon(0.300)
    cuts = np.linspace(-0.33, 0.33, 5)
    for k in range(4):  # four boards across the seat, butted: the rounded edges part them (an open gap aliases in game)
        prism(clip(seat, 1, cuts[k] + 0.0005, cuts[k + 1] - 0.0005), 0.40, 0.50 + rng.uniform(-0.0015, 0.0015), W)
    for x in (-0.12, 0.12):  # cleats under the boards
        box(x - 0.025, x + 0.025, 0.37, 0.40, -0.24, 0.24, W)
    band_around(seat, 0.425, 0.475, I, nails=2)
    for ang in (90, 210, 330):  # three legs splayed outward, one at the back (+z)
        a = math.radians(ang)
        top = (0.13 * math.cos(a), 0.13 * math.sin(a))
        foot = (0.25 * math.cos(a), 0.25 * math.sin(a))
        P.post(b(top[0], 0.46, top[1]), b(foot[0], 0.0, foot[1]), 0.065, 0.055, Vector((1, 0, 0)), Vector((0, 1, 0)), W, 0.6)
        prism(circle(top[0], top[1], 0.024), 0.49, 0.5025, W)  # the tenon end through the seat
        box(top[0] - 0.003, top[0] + 0.003, 0.495, 0.505, top[1] - 0.022, top[1] + 0.022, W)  # its wedge


def stool_rich():
    D, I = zone("DARKWOOD"), zone("IRON")
    seat = octagon(0.300)
    prism(seat, 0.41, 0.49, D)
    band_around(seat, 0.43, 0.47, I, nails=2)
    # a low faceted top, as the original's (0.08 m over the edge at the centre)
    part = P.new_part()
    ring = [P.vert(b(x * 0.98, 0.49, z * 0.98), part) for x, z in seat]
    tip = P.vert(b(0, 0.566, 0), part)
    for i in range(8):
        P.poly([ring[i], ring[(i + 1) % 8], tip], D, [(0, 0), (0.23, 0), (0.11, 0.3)])
    P.poly(ring[::-1], D, [(v.co.x, v.co.y) for v in ring[::-1]])
    box(-0.07, 0.07, 0.10, 0.41, -0.075, 0.075, D, bevel=0.012)  # the post
    for ang in (0, 90, 180, 270):  # a cross of flat feet, joints in iron brackets
        a = math.radians(ang + 45)
        end = (0.29 * math.cos(a), 0.29 * math.sin(a))
        P.board(b(end[0] / 2, 0.05, end[1] / 2), Vector((math.cos(a), -math.sin(a), 0)), Vector((math.sin(a), math.cos(a), 0)),
                Vector((0, 0, 1)), 0.29, 0.07, 0.10, D)
    for y0, y1 in ((0.06, 0.16), (0.34, 0.40)):  # brackets round the foot of the post and under the seat
        box(-0.08, 0.08, y0, y1, -0.085, 0.085, I)
        for nx, nz in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            rivets([b(nx * 0.08 + nz * s * 0.045, (y0 + y1) / 2, nz * 0.085 + nx * s * 0.045) for s in (-1, 1)],
                   [d(nx, 0, nz)] * 2, I, 0.007)


def armchair():
    L, B = zone("LEATHER"), zone("BRASS")
    # all leather, no visible wood: a skirt to the floor, a seat cushion, a tall back whose top dips between two
    # rolls curling backward, wings, roll arms with brass lion bosses on their front ends (front is -z)
    soft(0, 0.26, 0.05, 0.45, 0.26, 0.40, L, e=0.12, nu=40, nv=20)                       # base and skirt
    sag = lambda x, z: 0.015 * (1 - (x / 0.33) ** 2)  # noqa: E731
    soft(0, 0.55, -0.06, 0.33, 0.07, 0.31, L, e=0.3, nu=32, nv=12, sag=sag)              # seat cushion
    tube([b(x, 0.55, -0.06 + z) for x, z in superellipse(0.335, 0.315, 0.3, 64)], 0.006, L, UP)  # its piping
    dip = lambda x, z: 0.05 * max(0.0, 1 - (x / 0.30) ** 2)  # noqa: E731
    soft(0, 0.98, 0.46, 0.40, 0.48, 0.10, L, e=0.15, nu=40, nv=24, sag=dip)               # back
    for sx in (-1, 1):
        soft(sx * 0.44, 0.92, 0.18, 0.06, 0.38, 0.30, L, e=0.15, nu=24, nv=20)            # wing
        soft(sx * 0.44, 0.70, -0.06, 0.075, 0.075, 0.33, L, e=0.45, nu=24, nv=12)         # roll arm
        disc(sx * 0.44, 0.70, -0.383, 0.05, 0.006, B, 24)
        dome(b(sx * 0.44, 0.70, -0.389), FRONT, 0.042, B, h=0.02, sides=16)
        # the roll at the top of the back: a leather-covered board curling backward over the top, down the back
        # and round inward (a spiral seen from the side)
        zc, yc = 0.47, 1.42
        pts = [(zc - 0.10, yc - 0.10)]
        for t in np.linspace(0, 1, 40):
            ang = math.pi - t * 2.1 * math.pi
            r = 0.10 - 0.06 * t
            pts.append((zc + r * math.cos(ang), yc + r * math.sin(ang)))
        scroll(pts, sx * 0.27, sx * 0.48, 0.03, L)


def scroll(path, x0, x1, thick, mat):
    """A board of thickness thick bent along path [(z, y)] and running from x0 to x1 (the armchair's curled ears)."""
    part = P.new_part()
    n = len(path)
    rows = []
    for i in range(n):
        p0, p1 = path[max(i - 1, 0)], path[min(i + 1, n - 1)]
        tz, ty = p1[0] - p0[0], p1[1] - p0[1]
        ln = math.hypot(tz, ty)
        nz, ny = -ty / ln, tz / ln
        z, y = path[i]
        rows.append([P.vert(b(x, y + ny * s * thick / 2, z + nz * s * thick / 2), part) for x, s in ((x0, 1), (x1, 1), (x1, -1), (x0, -1))])
    run = 0.0
    for i in range(n - 1):
        ln = math.dist(path[i], path[i + 1])
        for k in range(4):
            j = (k + 1) % 4
            P.poly([rows[i][k], rows[i][j], rows[i + 1][j], rows[i + 1][k]], mat,
                   [(k * 0.1, run), ((k + 1) * 0.1, run), ((k + 1) * 0.1, run + ln), (k * 0.1, run + ln)])
        run += ln
    P.poly(rows[0], mat, [(0, 0), (0.2, 0), (0.2, 0.03), (0, 0.03)])
    P.poly(rows[-1][::-1], mat, [(0, 0), (0.2, 0), (0.2, 0.03), (0, 0.03)])


def bed_box(W, length, width, frame_top):
    """The plank box of a bed: side rails pegged into the end shields (the original's frame 0.15-0.35 m)."""
    for sz in (-1, 1):
        box(-length / 2, length / 2, 0.10, frame_top, sz * width / 2, sz * (width / 2 - 0.04), W)
        rivets([b(sx * (length / 2 - 0.05), yy, sz * width / 2) for sx in (-1, 1) for yy in (0.17, 0.33)],
               [d(0, 0, sz)] * 4, W, 0.013)


def bed():
    W, S = zone("WOOD"), zone("STRAW")
    L, Wd = 2.70, 1.188
    prof = [(Wd / 2, 0.0)] + [((Wd / 2) * math.cos(t), 0.98 - (Wd / 2) * (1 - math.sin(t)) * 0.9)
                              for t in np.linspace(0, math.pi, 25)] + [(-Wd / 2, 0.0)]
    cuts = np.linspace(-Wd / 2, Wd / 2, 6)
    for sx in (-1, 1):  # thick semicircular shields of five boards at both ends, the same height 0.98 m
        a0, a1 = sx * 1.35 - (0.10 if sx > 0 else 0), sx * 1.35 + (0.0 if sx > 0 else 0.10)
        for k in range(5):
            slab(clip(prof, 0, cuts[k] + 0.002, cuts[k + 1] - 0.002), "x", a0, a1, W)
        inner = sx * 1.25
        box(inner, inner - sx * 0.03, 0.60, 0.70, -Wd / 2 + 0.06, Wd / 2 - 0.06, W)  # the batten holding the boards
        rivets([b(inner - sx * 0.03, 0.65, (cuts[k] + cuts[k + 1]) / 2) for k in range(5)], [d(-sx, 0, 0)] * 5, W, 0.012)
    bed_box(W, 2.50, Wd, 0.42)
    box(-1.25, 1.25, 0.12, 0.16, -Wd / 2 + 0.04, Wd / 2 - 0.04, W)  # bottom boards
    lumps = lambda x, z: 0.025 * math.cos(x * 2.1) + 0.015 * math.sin(z * 7) + 0.008 * math.sin(5.3 * x + 2) * math.cos(4.1 * z)  # noqa: E731
    soft(0, 0.44, 0, 1.22, 0.10, 0.53, S, e=0.35, nu=56, nv=14, sag=lumps)
    tube([b(x, 0.44, z) for x, z in superellipse(1.222, 0.532, 0.35, 96)], 0.007, S, UP)  # the seam


def post_with_cap(x, z, half, top, cap, mat, collar=True):
    box(x - half, x + half, 0.0, top, z - half, z + half, mat, bevel=0.01)
    if collar:
        moulding(square(x, z, half), roundover(0.012, top - 0.07, top - 0.035, 0.01), mat)
    pyramid_cap(x, z, top, half, cap, mat)


def posts_and_panels(D, length, width, post, post_top, panel_top, cap):
    for sx in (-1, 1):
        for sz in (-1, 1):
            post_with_cap(sx * (length / 2 - post / 2), sz * (width / 2 - post / 2), post / 2, post_top, cap, D)
        # a solid panel of boards in a frame between the posts at each end
        x0 = sx * (length / 2 - post / 2)
        cuts = np.linspace(-width / 2 + post, width / 2 - post, 5)
        for k in range(4):
            box(x0 - 0.03, x0 + 0.03, 0.17, panel_top - 0.06, cuts[k] + 0.002, cuts[k + 1] - 0.002, D)
        box(x0 - 0.045, x0 + 0.045, panel_top - 0.06, panel_top, -width / 2 + post, width / 2 - post, D)
        box(x0 - 0.045, x0 + 0.045, 0.11, 0.17, -width / 2 + post, width / 2 - post, D)


def linen_mattress(C, T, x0, x1, width, y, depth=0.10):
    cx, rx = (x0 + x1) / 2, (x1 - x0) / 2
    sag = lambda x, z: 0.012 * math.cos(x * 3) + 0.004 * math.sin(6 * z)  # noqa: E731
    soft(cx, y, 0, rx, depth, width / 2, C, e=0.3, nu=48, nv=12, sag=sag)
    for end in (x0 + 0.25, x1 - 0.25):  # the woven green border near each end: a thin band round the whole section
        soft(end, y, 0, 0.05, depth * 1.012, width / 2 * 1.006, T, e=0.3, nu=40, nv=12,
             sag=lambda x, z, e=end - cx: sag(x + e, z) * 1.0)


def bed_rich():
    D, C, T = zone("DARKWOOD"), zone("LINEN"), zone("TRIM")
    posts_and_panels(D, 2.578, 1.278, 0.10, 0.90, 0.80, 0.04)
    for sz in (-1, 1):
        box(-1.19, 1.19, 0.11, 0.30, sz * (0.639 - 0.05), sz * (0.639 - 0.01), D)  # side rails
        rivets([b(sx * 1.17, yy, sz * 0.639) for sx in (-1, 1) for yy in (0.16, 0.25)], [d(0, 0, sz)] * 4, D, 0.012)
    box(-1.19, 1.19, 0.30, 0.34, -0.58, 0.58, D)
    linen_mattress(C, T, -1.18, 1.18, 1.14, 0.42, 0.09)


def bed_master():
    D, C, T = zone("DARKWOOD"), zone("LINEN"), zone("TRIM")
    # foot: two posts with caps and a plank board (x 1.01-1.19); head: an arched board under a canopy box
    for sz in (-1, 1):
        post_with_cap(1.139, sz * 0.56, 0.05, 0.90, 0.04, D)
    cuts = np.linspace(-0.51, 0.51, 5)
    for k in range(4):
        box(1.11, 1.17, 0.15, 0.80, cuts[k] + 0.002, cuts[k + 1] - 0.002, D)
    prof = [(0.637, 0.0)] + [(0.637 * math.cos(t), 0.78 + 0.25 * math.sin(t)) for t in np.linspace(0, math.pi, 25)] + [(-0.637, 0.0)]
    cuts = np.linspace(-0.637, 0.637, 6)
    for k in range(5):  # the arched head board of five boards
        slab(clip(prof, 0, cuts[k] + 0.002, cuts[k + 1] - 0.002), "x", -1.38, -1.30, D)
    for sz in (-1, 1):
        box(-1.30, 1.05, 0.11, 0.30, sz * (0.60 - 0.04), sz * 0.60, D)
        rivets([b(xx, yy, sz * 0.60) for xx in (-1.27, 1.02) for yy in (0.16, 0.25)], [d(0, 0, sz)] * 4, D, 0.012)
    box(-1.30, 1.05, 0.30, 0.34, -0.56, 0.56, D)
    linen_mattress(C, T, -1.28, 1.03, 1.12, 0.42, 0.09)
    # canopy box over the head third: a frame wrapped in linen, a pleated valance round it, a back curtain to the
    # floor, front curtains tied at its corners
    box(-1.382, -0.283, 2.03, 2.15, -0.709, 0.709, C)
    pleat = lambda s, period, amp: amp * math.sin(2 * math.pi * s / period)  # noqa: E731
    vrows = np.linspace(0, 1, 6)
    for sz in (-1, 1):  # side valances along x
        xs = np.arange(-1.382, -0.283, 0.012)
        grid = [[b(x, 2.035 - t * (0.11 + 0.012 * math.cos(2 * math.pi * (x + 1.382) / 0.22)),
                   sz * (0.704 + pleat(x, 0.07, 0.004))) for x in xs] for t in vrows]
        sheet(grid, d(0, 0, sz), C, du=0.012, dv=0.022)
    zs = np.arange(-0.704, 0.705, 0.012)  # front valance along z
    sheet([[b(-0.288 + pleat(z, 0.07, 0.004), 2.035 - t * (0.11 + 0.012 * math.cos(2 * math.pi * (z + 0.704) / 0.22)), z) for z in zs]
           for t in vrows], d(1, 0, 0), C, du=0.012, dv=0.022)
    zs = np.linspace(-0.70, 0.70, 48)  # the back curtain behind the head board, to the floor, in soft pleats
    sheet([[b(-1.377 + pleat(z, 0.12, 0.012) * (0.5 + 0.5 * y / 2.03), y, z) for z in zs] for y in np.linspace(2.03, 0.0, 14)],
          d(1, 0, 0), C, du=0.03, dv=0.15)
    for sz in (-1, 1):  # front curtains hanging down to the mattress, gathered by a cord at 1.2 m
        rows = []
        for y in np.linspace(2.03, 0.62, 22):
            g = 1.0 - 0.7 * math.exp(-((y - 1.2) / 0.18) ** 2)  # gathered at the tie
            rows.append([b(-0.30 - 0.40 * t * g, y, sz * (0.62 - 0.018 * math.sin(t * math.pi * 5) * g * (0.6 + 0.4 * t)))
                         for t in np.linspace(0, 1, 17)])
        sheet(rows, d(0, 0, sz), C, du=0.025, dv=0.065)
        g = 0.3
        tube([b(-0.36 + 0.075 * math.cos(a), 1.2, sz * 0.62 + 0.04 * math.sin(a)) for a in np.linspace(0, 2 * math.pi, 20, endpoint=False)],
             0.009, T, UP, sides=6)  # the tie


def dresser():
    W, I = zone("WOOD"), zone("IRON")
    for sx in (-1, 1):  # square block feet
        box(sx * 0.85 - 0.06 * (sx > 0), sx * 0.85 + 0.06 * (sx < 0), -1.235, -1.14, -0.35, -0.27, W)
        box(sx * 0.85 - 0.06 * (sx > 0), sx * 0.85 + 0.06 * (sx < 0), -1.235, -1.14, 0.30, 0.375, W)
    box(-0.895, 0.895, -1.14, -0.09, -0.395, 0.405, W)       # lower cupboard, 0.85 deep with its doors
    for sx in (-1, 1):
        side_boards(sx * 0.895, -1.14, -0.09, -0.395, 0.405, W)
    moulding([(-0.88, -0.405), (0.88, -0.405), (0.88, 0.405), (-0.88, 0.405)], roundover(0.02, -0.09, 0.01, 0.015), W)  # the ledge
    box(-0.775, 0.775, 0.01, 1.14, -0.195, 0.395, W)        # upper cupboard, 0.65 deep with its doors, set back
    for sx in (-1, 1):
        side_boards(sx * 0.775, 0.01, 1.14, -0.195, 0.395, W)
    moulding([(-0.753, -0.195), (0.753, -0.195), (0.753, 0.395), (-0.753, 0.395)], cornice(0.03, 1.14, 1.235), W)
    for y0, y1, x0, x1, z in ((-1.10, -0.13, -0.86, 0.86, -0.397), (0.05, 1.10, -0.74, 0.74, -0.197)):
        face_frame(x0, x1, y0, y1, z, 0.04, 0.035, W)
        mid = (x0 + x1) / 2
        plank_door(x0 + 0.004, mid - 0.006, y0 + 0.004, y1 - 0.004, z, W, boards=3, nails=I)
        plank_door(mid + 0.006, x1 - 0.004, y0 + 0.004, y1 - 0.004, z, W, boards=3, nails=I)
        for sx in (-1, 1):  # small knobs and small forged hinges
            knob(mid + sx * 0.05, (y0 + y1) / 2, z - 0.022, I)
            edge = x0 if sx < 0 else x1
            for yy in (y0 + 0.12, y1 - 0.12):
                hinge(edge, edge - sx * 0.11, yy, z - 0.022, I, w=0.024, curl=False)


def chamfered(x, zf, zb, c):
    """A plan outline with its two front corners cut at 45 degrees by c (front at zf < zb)."""
    return [(-x + c, zf), (x - c, zf), (x, zf + c), (x, zb), (-x, zb), (-x, zf + c)]


def carved_corner(sx, y_mid, height, D):
    """A carved panel on a cut corner of the carcass: a raised board with a rope down its middle."""
    c = Vector((sx * (0.85 - 0.06), 0, -0.40 + 0.06))
    n = Vector((sx * 0.7071, 0, -0.7071))
    t = Vector((0.7071, 0, sx * 0.7071))
    P.board(b(c.x + n.x * 0.008, y_mid, c.z + n.z * 0.008), d(0, 1, 0), d(t.x, 0, t.z), d(n.x, 0, n.z), height, 0.10, 0.016, D)
    nb = d(n.x, 0, n.z)
    ax, az = (d(t.x, 0, t.z) + UP).normalized(), (UP - d(t.x, 0, t.z)).normalized()
    for y in np.arange(y_mid - height / 2 + 0.04, y_mid + height / 2 - 0.03, 0.032):
        bead(b(c.x + n.x * 0.016, y, c.z + n.z * 0.016), ax, nb, az, (0.022, 0.008, 0.01), D)


def rich_lower(D, I, y_base):
    """The lower cupboard of the rich dresser and the sideboard: polygonal, arched plank doors with strap hinges,
    carved panels on the cut corners, an overhanging top, a moulded plinth on ball feet."""
    ball_feet([(-0.70, -0.30), (0.70, -0.30), (-0.70, 0.15), (0.70, 0.15)], y_base, 0.025, D)
    moulding(chamfered(0.86, -0.41, 0.21, 0.114), plinth(0.015, y_base + 0.05, y_base + 0.10), D)
    prism(chamfered(0.85, -0.40, 0.45, 0.12), y_base + 0.10, y_base + 1.15, D)          # carcass
    moulding(chamfered(0.88, -0.43, 0.43, 0.122), roundover(0.02, y_base + 1.15, y_base + 1.25, 0.018), D)  # top
    y0, y1 = y_base + 0.18, y_base + 1.06
    plank_door(-0.70, -0.01, y0, y1, -0.40, D, boards=4, arch=0.12)
    plank_door(0.01, 0.70, y0, y1, -0.40, D, boards=4, arch=0.12)
    for x0, x1 in ((-0.70, -0.06), (0.70, 0.06)):
        for yy in (y0 + 0.15, y1 - 0.25):
            hinge(x0, x1, yy, -0.422, I)
        ring_pull(x1 + (-0.03 if x0 < 0 else 0.03), (y0 + y1) / 2, -0.422, I)
    for sx in (-1, 1):
        carved_corner(sx, y_base + 0.62, 0.75, D)


def dresser_rich():
    D, I = zone("DARKWOOD"), zone("IRON")
    rich_lower(D, I, -1.266)
    yb = -0.02
    prism(chamfered(0.692, -0.25, 0.45, 0.10), yb, yb + 1.00, D)          # upper cupboard, set back
    moulding(chamfered(0.712, -0.27, 0.42, 0.105), cornice(0.03, yb + 1.00, yb + 1.10), D)
    plank_door(-0.56, -0.01, yb + 0.08, yb + 0.92, -0.25, D, boards=3, nails=I)
    plank_door(0.01, 0.56, yb + 0.08, yb + 0.92, -0.25, D, boards=3, nails=I)
    for x0, x1 in ((-0.56, -0.08), (0.56, 0.08)):
        for yy in (yb + 0.22, yb + 0.78):
            hinge(x0, x1, yy, -0.272, I)
    prof = [(0.28, 0.0)] + [(0.28 * math.cos(t), 0.19 * math.sin(t)) for t in np.linspace(0, math.pi, 21)] + [(-0.28, 0.0)]
    slab([(u, yb + 1.10 + v) for u, v in prof], "z", -0.25, -0.20, D)    # the carved crest
    dome(b(0, yb + 1.18, -0.25), FRONT, 0.05, D, h=0.02, sides=16)         # its rosette
    for k in range(8):
        a = math.pi * (k + 0.5) / 8
        bead(b(0.17 * math.cos(a), yb + 1.10 + 0.115 * math.sin(a), -0.252), FRONT.cross(Vector((math.cos(a), 0, math.sin(a)))).normalized(),
             FRONT, Vector((math.cos(a), 0, math.sin(a))), (0.028, 0.008, 0.012), D)


def sideboard():
    D, I = zone("DARKWOOD"), zone("IRON")
    rich_lower(D, I, -1.266)


def lectern():
    D, I, L, Pp = zone("DARKWOOD"), zone("IRON"), zone("LEATHER"), zone("PAPER")
    moulding([(-0.60, 0.385), (0.60, 0.385), (0.60, 1.112), (-0.60, 1.112)], plinth(0.024, 0.0, 0.16), D)  # plinth
    rope([(-0.62, 0.364), (0.62, 0.364)], 0.08, D, grow=0.006)               # its carving
    box(-0.60, 0.60, 0.16, 1.09, 0.385, 1.12, D)                           # cupboard
    # the desk: a slope from the front at 1.15 m up to 1.50 m at z 0.70, then flat to the back
    part = P.new_part()
    prof = [(0.385, 1.09), (1.12, 1.09), (1.12, 1.50), (0.704, 1.50), (0.385, 1.15)]
    s0 = [P.vert(b(-0.62, y, z), part) for z, y in prof]
    s1 = [P.vert(b(0.62, y, z), part) for z, y in prof]
    for i in range(5):
        j = (i + 1) % 5
        P.poly([s0[i], s1[i], s1[j], s0[j]], D, [(0, 0), (1.24, 0), (1.24, 0.3), (0, 0.3)])
    P.poly(s0, D, [(z, y) for z, y in prof])
    P.poly(s1[::-1], D, [(z, y) for z, y in prof[::-1]])
    box(-0.60, 0.60, 1.14, 1.18, 0.36, 0.385, D)                            # the lip
    face_frame(-0.54, 0.54, 0.24, 1.02, 0.385, 0.03, 0.03, D)
    plank_door(-0.536, -0.008, 0.244, 1.016, 0.385, D, boards=3, nails=I)
    plank_door(0.008, 0.536, 0.244, 1.016, 0.385, D, boards=3, nails=I)
    for x0, x1 in ((-0.536, -0.08), (0.536, 0.08)):
        for yy in (0.36, 0.90):
            hinge(x0, x1, yy, 0.363, I, w=0.03)
        ring_pull(x1 + (-0.03 if x0 < 0 else 0.03), 0.63, 0.363, I, r=0.026)
    # the open book on the slope: two page blocks curving up to the spine, and a cover
    ang = math.atan2(1.50 - 1.15, 0.704 - 0.395)
    along, across = d(0, math.sin(ang), math.cos(ang)), d(0, math.cos(ang), -math.sin(ang))
    for sx in (-1, 1):
        cx = sx * 0.20
        cz, cy = 0.55, 1.15 + (0.55 - 0.395) * math.tan(ang) + 0.02
        P.board(b(cx, cy - 0.02, cz + 0.002), d(1, 0, 0), along, across, 0.39, 0.32, 0.008, L)
        part = P.new_part()
        base = b(cx, cy - 0.016, cz)
        grid = []
        for u in np.linspace(-0.18, 0.18, 10):
            spine = (0.18 - sx * u) / 0.36  # 0 at the fore edge, 1 at the spine
            rise = 0.008 + 0.014 * math.sin(math.pi * 0.85 * spine)
            grid.append([(base + d(1, 0, 0) * u + along * v, rise) for v in np.linspace(-0.15, 0.15, 3)])
        top = [[P.vert(p + across * h, part) for p, h in row] for row in grid]
        bot = [[P.vert(p, part) for p, h in row] for row in grid]
        for i in range(len(grid) - 1):
            for j in range(2):
                P.poly([top[i][j], top[i + 1][j], top[i + 1][j + 1], top[i][j + 1]], Pp, [(i * 0.04, j * 0.15), ((i + 1) * 0.04, j * 0.15), ((i + 1) * 0.04, (j + 1) * 0.15), (i * 0.04, (j + 1) * 0.15)])
                P.poly([bot[i][j + 1], bot[i + 1][j + 1], bot[i + 1][j], bot[i][j]], Pp, [(0, 0), (0.04, 0), (0.04, 0.15), (0, 0.15)])
        for i in range(len(grid) - 1):  # the page edges round the block
            for j in (0, 2):
                P.poly([bot[i][j], bot[i + 1][j], top[i + 1][j], top[i][j]], Pp, [(0, 0), (0.04, 0), (0.04, 0.01), (0, 0.01)])
        for i in (0, len(grid) - 1):
            for j in range(2):
                P.poly([bot[i][j], bot[i][j + 1], top[i][j + 1], top[i][j]], Pp, [(0, 0), (0.15, 0), (0.15, 0.01), (0, 0.01)])
    # The numbers above were read with the root translation unturned (z 0.33-1.14); the original's root node is
    # turned 180 degrees, so in the engine's bind space the lectern stands 1.10 m further forward: move it there.
    dz = goal["bbox_max"][2] - 1.13565
    for v in P.bm.verts:
        v.co.y -= dz


def table(top_mat, rich=False):
    T = zone(top_mat)
    D = zone("DARKWOOD") if rich else zone("WOOD")
    I = zone("IRON")
    for sx in (-1, 1):  # square legs (the original's 0.15 m)
        for sz in (-1, 1):
            x, z = sx * 0.85, sz * 0.45
            box(x - 0.075, x + 0.075, -0.45, 0.35, z - 0.075, z + 0.075, D, bevel=0.012)
            if not rich:  # iron corner brackets on the tops of the legs
                box(x - 0.08, x + 0.08, 0.22, 0.30, z - 0.08, z + 0.08, I)
                rivets([b(x + sx * 0.08, 0.26, z + s * 0.04) for s in (-1, 1)] + [b(x + s * 0.04, 0.26, z + sz * 0.08) for s in (-1, 1)],
                       [d(sx, 0, 0)] * 2 + [d(0, 0, sz)] * 2, I, 0.007)
    for sz in (-1, 1):  # apron, pegged into the legs
        box(-0.78, 0.78, 0.15, 0.35, sz * 0.45 - 0.03, sz * 0.45 + 0.03, D)
    for sx in (-1, 1):
        box(sx * 0.85 - 0.03, sx * 0.85 + 0.03, 0.15, 0.35, -0.38, 0.38, D)
    for sx in (-1, 1):  # the peg ends in the legs where the aprons are tenoned in
        for sz in (-1, 1):
            rivets([b(sx * 0.85, yy, sz * 0.525) for yy in (0.20, 0.30)], [d(0, 0, sz)] * 2, D, 0.011)
            rivets([b(sx * 0.925, yy, sz * 0.45) for yy in (0.20, 0.30)], [d(sx, 0, 0)] * 2, D, 0.011)
    widths = rng.uniform(0.85, 1.15, 5)
    widths = widths / widths.sum() * 1.2
    z = -0.6
    for w in widths:  # boards of uneven width, butted (a 6 mm open gap read in game as a dotted line), each at its own height
        y1 = 0.45 + rng.uniform(-0.002, 0.002)
        box(-0.99, 0.99, 0.35, y1, max(z + 0.0005, -0.59), min(z + w - 0.0005, 0.59), T)
        z += w
    outline = [(-0.99, -0.59), (0.99, -0.59), (0.99, 0.59), (-0.99, 0.59)]
    if rich:  # a carved rope round the edge of the top, and a dark inlaid frame on it
        rope(outline, 0.40, D)
        for (x0, z0, x1, z1) in ((-0.92, -0.53, 0.92, -0.50), (-0.92, 0.50, 0.92, 0.53), (-0.92, -0.53, -0.89, 0.53), (0.89, -0.53, 0.92, 0.53)):
            box(x0, x1, 0.45, 0.452, z0, z1, D)
    else:
        band_around(outline, 0.37, 0.43, I, grow=0.004, nails=6)


def bench():
    W, S = zone("WOOD"), zone("STONE")
    for sx in (-1, 1):  # trapezoid pedestals of dressed stone, wider at the foot: a footing of two blocks, two courses
        x = sx * 0.70
        box(x - 0.175, x + 0.175, 0.0, 0.08, -0.275, -0.003, S)
        box(x - 0.175, x + 0.175, 0.0, 0.08, 0.003, 0.275, S)
        for y0, y1 in ((0.083, 0.214), (0.217, 0.35)):
            f0, f1 = (y0 - 0.08) / 0.27, (y1 - 0.08) / 0.27
            hx = lambda f: 0.17 - 0.05 * f  # noqa: E731
            hz = lambda f: 0.27 - 0.07 * f  # noqa: E731
            part = P.new_part()
            lv = [P.vert(b(x + px * hx(f0), y0, pz * hz(f0)), part) for px, pz in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
            tv = [P.vert(b(x + px * hx(f1), y1, pz * hz(f1)), part) for px, pz in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
            for i in range(4):
                j = (i + 1) % 4
                P.poly([lv[i], lv[j], tv[j], tv[i]], S, [(0, 0), (0.34, 0), (0.24, 0.27), (0.1, 0.27)])
            P.poly(tv, S, [(v.co.x, v.co.y) for v in tv])
            P.poly(lv[::-1], S, [(v.co.x, v.co.y) for v in lv[::-1]])
    # the plank: 0.15 m thick, bevelled long edges
    prof = [(-0.378, 0.35), (0.378, 0.35), (0.378, 0.42), (0.295, 0.50), (-0.295, 0.50), (-0.378, 0.42)]
    slab(prof, "x", -1.0, 1.0, W)


BUILDERS = {"stool": stool, "stool-rich": stool_rich, "armchair": armchair, "bed": bed, "bed-rich": bed_rich,
            "bed-master": bed_master, "dresser": dresser, "dresser-rich": dresser_rich, "sideboard": sideboard,
            "lectern": lectern, "table": lambda: table("WOOD"), "table-rich": lambda: table("OAK", rich=True), "bench": bench}
BUILDERS[cfg["kind"]]()
round_edges()
P.finish(goal, out, lo, hi, {"kind": cfg["kind"]}, prepare)
