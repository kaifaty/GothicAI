"""Shared builders of the open-fire props (asset city-hearth, wave 1): fieldstones, an ember bed with
lumps, an ash ring, charred sticks and split logs. They add closed parts to a parts.Parts.

Stones are convex hulls of jittered points on a flattened ellipsoid: every stone has its own blunt,
broken shape (P2 of the intent sheet: not identical polyhedra, not smooth pebbles), sits on the ground
and is sunk a little into it. UVs are planar in metres along the stone's largest faces.
"""
import math

import bmesh
import numpy as np
from mathutils import Vector


def hull(P, points, material, scale_uv=1.0):
    """A closed convex hull of world points as one part; planar UVs by the dominant axis of each face."""
    part = P.new_part()
    tmp = bmesh.new()
    for p in points:
        tmp.verts.new(p)
    result = bmesh.ops.convex_hull(tmp, input=tmp.verts)
    geom = set(result["geom"]) - set(result.get("geom_interior", [])) - set(result.get("geom_unused", []))
    faces = [f for f in geom if isinstance(f, bmesh.types.BMFace)]
    lookup = {}
    for f in faces:
        verts = []
        for v in f.verts:
            if v not in lookup:
                lookup[v] = P.vert(v.co.copy(), part)
            verts.append(lookup[v])
        n = f.normal
        axis = max(range(3), key=lambda i: abs(n[i]))
        others = [i for i in range(3) if i != axis]
        P.poly(verts, material, [(v.co[others[0]] * scale_uv, v.co[others[1]] * scale_uv) for v in verts])
    tmp.free()
    return part


def stone(P, rng, c, size, material, sink=0.02, points=22, rough=0.22):
    """A broken fieldstone at ground point c: size (x, y, z) in m, yaw random, blunt and uneven."""
    yaw = rng.uniform(0, 2 * math.pi)
    cy, sy = math.cos(yaw), math.sin(yaw)
    pts = []
    for _ in range(points):
        d = rng.normal(size=3)
        d /= np.linalg.norm(d)
        d *= 1 - rough * rng.random()  # pull some points in: flat broken faces between them
        x, y, z = d[0] * size[0] / 2, d[1] * size[1] / 2, d[2] * size[2] / 2
        z = max(z, -size[2] * 0.35)  # a flat bottom on the ground: the top stands 0.85 of the size high
        pts.append(Vector((c.x + x * cy - y * sy, c.y + x * sy + y * cy, c.z + size[2] * 0.35 + z - sink)))
    return hull(P, pts, material)


def lump(P, rng, c, r, material):
    """A small coal or ember lump at c: a squat hull of 7 points."""
    pts = [Vector((c.x + rng.uniform(-r, r), c.y + rng.uniform(-r, r), c.z + rng.uniform(0, r * 0.9)))
           for _ in range(7)]
    pts.append(Vector((c.x, c.y, c.z - 0.003)))
    return hull(P, pts, material)


def dome(P, rng, c, radius, height, material, sides=16, rings=4):
    """A low ember bed: a radial grid whose height falls from the middle to the rim, jittered."""
    part = P.new_part()
    rows = []
    for k in range(1, rings + 1):
        t = k / rings
        h = height * (1 - t * t)
        row = []
        for i in range(sides):
            a = 2 * math.pi * (i + 0.5 * (k % 2)) / sides
            rr = radius * t * (1 + rng.uniform(-0.08, 0.08))
            row.append(P.vert(Vector((c.x + rr * math.cos(a), c.y + rr * math.sin(a),
                                      c.z + max(h + rng.uniform(-0.012, 0.012), 0.0) - (0.004 if k == rings else 0))), part))
        rows.append(row)
    top = P.vert(Vector((c.x, c.y, c.z + height)), part)
    uv = lambda v: (v.co.x - c.x, v.co.y - c.y)
    for i in range(sides):
        j = (i + 1) % sides
        P.poly([top, rows[0][i], rows[0][j]], material, [uv(top), uv(rows[0][i]), uv(rows[0][j])])
    for k in range(rings - 1):
        for i in range(sides):
            j = (i + 1) % sides
            quad = [rows[k][i], rows[k + 1][i], rows[k + 1][j], rows[k][j]]
            P.poly(quad, material, [uv(v) for v in quad])
    # close the bed underneath so the shell is closed
    bottom = P.vert(Vector((c.x, c.y, c.z - 0.006)), part)
    for i in range(sides):
        j = (i + 1) % sides
        P.poly([bottom, rows[-1][j], rows[-1][i]], material, [uv(bottom), uv(rows[-1][j]), uv(rows[-1][i])])
    return part


def ash(P, rng, c, r0, r1, material, sides=28, lift=0.004):
    """A flat ring of ash and burnt ground from r0 to r1 with a ragged outer edge, one face up."""
    part = P.new_part(wound=True)
    inner, outer = [], []
    for i in range(sides):
        a = 2 * math.pi * i / sides
        ro = r1 * (1 + rng.uniform(-0.18, 0.12))
        inner.append(P.vert(Vector((c.x + r0 * math.cos(a), c.y + r0 * math.sin(a), c.z + lift)), part))
        outer.append(P.vert(Vector((c.x + ro * math.cos(a), c.y + ro * math.sin(a), c.z + lift * 0.5)), part))
    uv = lambda v: (v.co.x - c.x, v.co.y - c.y)
    for i in range(sides):
        j = (i + 1) % sides
        quad = [inner[i], outer[i], outer[j], inner[j]]
        P.poly(quad, material, [uv(v) for v in quad])
    return part


def stick(P, rng, a, b, radius, material, burnt=None, sides=6):
    """A round stick or log from a to b, sides faces, closed; burnt: (material, share from b) charred end.
    UV: u around in metres, v along the length (grain along the stick)."""
    part = P.new_part()
    axis = (b - a)
    length = axis.length
    d = axis.normalized()
    u = d.cross(Vector((0, 0, 1)))
    if u.length < 1e-3:
        u = d.cross(Vector((1, 0, 0)))
    u.normalize()
    w = d.cross(u)
    off = rng.uniform(0, 1)
    cuts = [0.0, 1.0] if burnt is None else [0.0, 1.0 - burnt[1], 1.0]
    rings = []
    for t in cuts:
        r = radius * (1 - 0.25 * t if burnt is not None and t == 1.0 else 1)
        rings.append([P.vert(a + axis * t + (u * math.cos(2 * math.pi * k / sides) + w * math.sin(2 * math.pi * k / sides))
                             * r * (1 + rng.uniform(-0.08, 0.08)), part) for k in range(sides)])
    for s in range(len(rings) - 1):
        mat = burnt[0] if burnt is not None and s == len(rings) - 2 else material
        for k in range(sides):
            j = (k + 1) % sides
            u0, u1 = 2 * math.pi * radius * k / sides, 2 * math.pi * radius * (k + 1) / sides
            v0, v1 = off + cuts[s] * length, off + cuts[s + 1] * length
            P.poly([rings[s][k], rings[s][j], rings[s + 1][j], rings[s + 1][k]], mat, [(u0, v0), (u1, v0), (u1, v1), (u0, v1)])
    for ring, mat in ((rings[0], material), (rings[-1], burnt[0] if burnt is not None else material)):
        P.poly(ring, mat, [((v.co - ring[0].co).dot(u), (v.co - ring[0].co).dot(w)) for v in ring])
    return part


def turned(P, c, outer, inner, sides, material, radius_at=None):
    """A closed turned vessel round the vertical axis at c: the outer profile [(h, r)] from the bottom up
    to the rim, then the inner profile from the rim down; caps close the bottom and the inner floor.
    radius_at(h, t, r) may bend a radius (hammer dents, an oval). UV: u = arc length, v = run along
    the profile. Returns the part."""
    part = P.new_part()
    rows = list(outer) + list(inner)
    bend = radius_at or (lambda h, t, r: r)
    rings = []
    for h, r in rows:
        ring = []
        for i in range(sides):
            t = 2 * math.pi * i / sides
            rr = bend(h, t, r)
            ring.append(P.vert(Vector((c.x + rr * math.cos(t), c.y + rr * math.sin(t), c.z + h)), part))
        rings.append(ring)
    run = 0.0
    for k in range(len(rings) - 1):
        (h0, r0), (h1, r1) = rows[k], rows[k + 1]
        step = math.hypot(h1 - h0, r1 - r0)
        for i in range(sides):
            j = (i + 1) % sides
            a, b = 2 * math.pi * r0 * i / sides, 2 * math.pi * r0 * (i + 1) / sides
            P.poly([rings[k][i], rings[k][j], rings[k + 1][j], rings[k + 1][i]], material,
                   [(a, run), (b, run), (b, run + step), (a, run + step)])
        run += step
    for ring, up in ((rings[0], False), (rings[-1], True)):
        centre = P.vert(sum((v.co for v in ring), Vector()) / len(ring), part)
        for i in range(sides):
            a, b = ring[i], ring[(i + 1) % sides]
            tri = [centre, a, b] if up else [centre, b, a]
            P.poly(tri, material, [(v.co.x, v.co.y) for v in tri])
    return part


def bar(P, a, b, w, h, material):
    """A flat rectangular iron bar from a to b, w wide and h thick, lying with its width horizontal."""
    part = P.new_part()
    d = (b - a).normalized()
    side = d.cross(Vector((0, 0, 1)))
    if side.length < 1e-3:
        side = Vector((1, 0, 0))
    side.normalize()
    up = side.cross(d)
    corners = {}
    for i, p in enumerate((a, b)):
        for sw in (-1, 1):
            for sh in (-1, 1):
                corners[i, sw, sh] = P.vert(p + side * (sw * w / 2) + up * (sh * h / 2), part)
    L = (b - a).length
    quads = [((0, -1, -1), (1, -1, -1), (1, 1, -1), (0, 1, -1)), ((0, -1, 1), (0, 1, 1), (1, 1, 1), (1, -1, 1)),
             ((0, -1, -1), (0, -1, 1), (1, -1, 1), (1, -1, -1)), ((0, 1, -1), (1, 1, -1), (1, 1, 1), (0, 1, 1)),
             ((0, -1, -1), (0, 1, -1), (0, 1, 1), (0, -1, 1)), ((1, -1, -1), (1, -1, 1), (1, 1, 1), (1, 1, -1))]
    for q in quads:
        P.poly([corners[k] for k in q], material, [(k[0] * L, (k[1] + 1) * w / 2) for k in q])
    return part


def chain(P, a, b, material, link=0.07, wire=0.006, width=0.035):
    """An iron chain hanging straight from a to b: oval links of round wire, each turned 90 degrees
    to the last. Each link is a closed loop of 6 segments with a 3-sided wire (36 triangles)."""
    d = b - a
    length = d.length
    axis = d.normalized()
    side = axis.cross(Vector((0, 0, 1)))
    if side.length < 1e-3:
        side = axis.cross(Vector((1, 0, 0)))
    side.normalize()
    other = axis.cross(side)
    n = max(1, round(length / (link * 0.8)))
    step = length / n
    for k in range(n):
        part = P.new_part()
        c = a + axis * (step * (k + 0.5))
        across = side if k % 2 == 0 else other
        normal = other if k % 2 == 0 else side
        loop = []
        for i in range(6):
            t = 2 * math.pi * i / 6
            loop.append((c + axis * (math.cos(t) * link / 2) + across * (math.sin(t) * width / 2),
                         (axis * math.cos(t) * (width / link) + across * math.sin(t)).normalized()))
        rings = []
        for p, out in loop:
            rings.append([P.vert(p + (out * math.cos(s) + normal * math.sin(s)) * wire, part)
                          for s in (2 * math.pi * j / 3 for j in range(3))])
        for i in range(6):
            i1 = (i + 1) % 6
            for j in range(3):
                j1 = (j + 1) % 3
                P.poly([rings[i][j], rings[i1][j], rings[i1][j1], rings[i][j1]], material,
                       [(i * 0.02, j * 0.01), ((i + 1) * 0.02, j * 0.01), ((i + 1) * 0.02, (j + 1) * 0.01), (i * 0.02, (j + 1) * 0.01)])


def octagon(r, t, round_share=0.25):
    """Radius of an eight-sided outline at angle t with rounded corners: flat faces, soft edges."""
    phase = (t + math.pi / 8) % (math.pi / 4) - math.pi / 8
    flat = r * math.cos(math.pi / 8) / math.cos(phase)
    return flat * (1 - round_share) + r * math.cos(math.pi / 16) * round_share
