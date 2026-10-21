"""List overlapping coplanar faces of different parts in a builder GLB (parts.py props).

blender -b --factory-startup -P coplanar.py -- model.glb [model.glb ...] [--json out.json] [--all]

Takes the builder's model.glb, which keeps the "_part" attribute (the atlas GLB does not). A model
without it (barrel.py) falls back to connected pieces. Two triangles of different parts that lie in
one plane (within 0.1 mm) and overlap by more than 0.1 mm^2 make a pair:
- same facing: both faces draw at one depth. The game z-fights there, Cycles shades it black;
- opposite facing: two parts touch face to face. Neither face is seen, nothing to fix.

Each same-facing pair is sampled just in front of the overlap. It is hidden when every sample is
inside another closed part (a plank end inside a post, a face covered by a touching part) or below
the model's lowest point (a bottom on the ground). The rest are listed as "look": decide by eye
whether a player sees them (a face inside a closed lid, a back against the wall). --all also lists
the hidden and the opposite-facing pairs.
"""
import json
import sys
from collections import defaultdict

import bpy
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree

GAP = 0.0001        # m: faces closer than this to one plane count as coplanar (offsets by design are >= 0.5 mm)
COS = 0.9999        # normals within ~0.8 degrees
MIN_AREA = 1e-7     # m^2: 0.1 mm^2
FRONT = 0.0003      # m: how far in front of the overlap the samples sit
RAY = Vector((0.2672, 0.5345, 0.8018))  # oblique, so parity rays miss edges and axis-aligned faces


def load(glb):
    """Triangles of a GLB: corners (N,3,3), part key per triangle, material name per triangle."""
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=glb)
    corners, keys, mats = [], [], []
    for oi, obj in enumerate(o for o in bpy.context.scene.objects if o.type == "MESH"):
        me = obj.data
        me.calc_loop_triangles()
        co = np.array([obj.matrix_world @ v.co for v in me.vertices])
        attr = me.attributes.get("_PART") or me.attributes.get("_part")
        if attr is not None and attr.domain == "POINT":
            part = np.array([round(d.value) for d in attr.data])
        else:
            part = pieces(co, [lt.vertices for lt in me.loop_triangles])
        names = [s.material.name if s.material else "" for s in obj.material_slots]
        for lt in me.loop_triangles:
            idx = list(lt.vertices)
            corners.append(co[idx])
            keys.append(f"{obj.name}#{part[idx[0]]}")
            mats.append(names[lt.material_index] if names else "")
    return np.array(corners), keys, mats


def pieces(co, tris):
    """Connected pieces by welded position, for a model without "_part"."""
    weld = {}
    vid = [weld.setdefault(tuple(np.round(c, 5)), len(weld)) for c in co]
    parent = list(range(len(weld)))

    def root(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    for t in tris:
        a = root(vid[t[0]])
        for v in t[1:]:
            parent[root(vid[v])] = a
    return np.array([root(v) for v in vid])


def clip(a, b, u, v):
    """Overlap of two coplanar triangles as a 2D polygon in the (u, v) basis (Sutherland-Hodgman)."""
    A = [(float(p @ u), float(p @ v)) for p in a]
    B = [(float(p @ u), float(p @ v)) for p in b]
    if area2(A) < 0:
        A.reverse()
    if area2(B) < 0:
        B.reverse()
    out = A
    for i in range(3):
        c, d = B[i], B[(i + 1) % 3]
        inp, out = out, []

        def side(p):
            return (d[0] - c[0]) * (p[1] - c[1]) - (d[1] - c[1]) * (p[0] - c[0])

        for j in range(len(inp)):
            p, q = inp[j], inp[(j + 1) % len(inp)]
            sp, sq = side(p), side(q)
            if sq >= 0:
                if sp < 0:
                    out.append(cut(p, q, sp, sq))
                out.append(q)
            elif sp >= 0:
                out.append(cut(p, q, sp, sq))
        if not out:
            break
    return out


def cut(p, q, sp, sq):
    t = sp / (sp - sq)
    return (p[0] + t * (q[0] - p[0]), p[1] + t * (q[1] - p[1]))


def area2(P):
    return sum(P[i][0] * P[(i + 1) % len(P)][1] - P[(i + 1) % len(P)][0] * P[i][1] for i in range(len(P)))


def closed_parts(corners, keys):
    """Parts whose every edge is shared by two of their triangles (welded at 0.01 mm)."""
    edges = defaultdict(lambda: defaultdict(int))
    for t, k in zip(corners, keys):
        q = [tuple(np.round(c, 5)) for c in t]
        for i in range(3):
            edges[k][tuple(sorted((q[i], q[(i + 1) % 3])))] += 1
    return {k for k, e in edges.items() if all(n == 2 for n in e.values())}


def inside(trees, point):
    """Parts (closed ones only) that contain the point: ray parity per part."""
    hits = set()
    for key, tree in trees.items():
        n, o = 0, Vector(point)
        while True:
            loc, _, _, _ = tree.ray_cast(o, RAY)
            if loc is None:
                break
            n += 1
            o = loc + RAY * 1e-6
        if n % 2:
            hits.add(key)
    return hits


def check(glb):
    corners, keys, mats = load(glb)
    n = np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0])
    size = np.linalg.norm(n, axis=1)
    good = size > 1e-12
    n[good] /= size[good, None]
    d = np.einsum("ij,ij->i", n, corners[:, 0])
    lo, hi = corners.min(axis=1), corners.max(axis=1)
    kid = np.unique(keys, return_inverse=True)[1]
    zmin = float(corners[:, :, 2].min())

    pairs = []
    for s in range(0, len(corners), 256):
        e = min(s + 256, len(corners))
        dot = n[s:e] @ n.T
        m = (np.abs(dot) >= COS) & good[s:e, None] & good[None, :]
        m &= np.abs(d[s:e, None] - np.sign(dot) * d[None, :]) <= 4 * GAP
        m &= kid[s:e, None] != kid[None, :]
        m &= np.arange(s, e)[:, None] < np.arange(len(corners))[None, :]
        m &= np.all(lo[s:e, None] <= hi[None, :] + GAP, axis=2) & np.all(lo[None, :] <= hi[s:e, None] + GAP, axis=2)
        pairs += [(s + i, j) for i, j in zip(*np.nonzero(m))]

    closed = closed_parts(corners, keys)
    trees = {}
    for k in closed:
        tri = [t for t, kk in zip(corners, keys) if kk == k]
        verts = [tuple(c) for t in tri for c in t]
        trees[k] = BVHTree.FromPolygons(verts, [(3 * i, 3 * i + 1, 3 * i + 2) for i in range(len(tri))])

    groups = {}
    for i, j in pairs:
        ni = n[i]
        if np.abs((corners[j] - corners[i][0]) @ ni).max() > GAP:   # the other triangle off this plane
            continue
        u = np.cross(ni, [1, 0, 0] if abs(ni[0]) < 0.9 else [0, 1, 0])
        u /= np.linalg.norm(u)
        v = np.cross(ni, u)
        poly = clip(corners[i], corners[j], u, v)
        if len(poly) < 3:
            continue
        area = abs(area2(poly)) / 2
        if area < MIN_AREA:
            continue
        same = bool(n[i] @ n[j] > 0)
        a, b = sorted((keys[i], keys[j]))
        axis = "".join(f"{'+' if c > 0 else '-'}{'XYZ'[k]}" for k, c in enumerate(ni) if abs(c) > 0.5) or "tilted"
        g = groups.setdefault((a, b, same, axis, round(float(d[i]), 3)), {
            "parts": [a, b], "materials": sorted({mats[i], mats[j]}), "same_facing": same,
            "normal": axis, "area_mm2": 0.0, "samples": [], "lo": [9e9] * 3, "hi": [-9e9] * 3})
        g["area_mm2"] += area * 1e6
        plane = float(ni @ corners[i][0])
        pts = [ni * plane + u * p[0] + v * p[1] for p in poly]
        c = np.mean(pts, axis=0)
        g["samples"] += [c] + [c + 0.8 * (p - c) for p in pts]
        g["front"] = ni
        g["lo"] = np.minimum(g["lo"], np.min(pts, axis=0)).tolist()
        g["hi"] = np.maximum(g["hi"], np.max(pts, axis=0)).tolist()

    for g in groups.values():
        front = g.pop("front")
        samples = g.pop("samples")
        g["area_mm2"] = round(g["area_mm2"], 1)
        g["lo"] = [round(x, 4) for x in g["lo"]]
        g["hi"] = [round(x, 4) for x in g["hi"]]
        if not g["same_facing"]:
            g["verdict"] = "touching"
            continue
        why = set()
        for p in samples:
            q = p + front * FRONT
            if q[2] < zmin:
                why.add("ground")
                continue
            covers = inside(trees, q) - set(g["parts"])
            if not covers:
                why = None
                break
            why.add("inside " + min(covers))
        g["verdict"] = "look" if why is None else "hidden: " + ", ".join(sorted(why))
    out = sorted(groups.values(), key=lambda g: (g["verdict"] != "look", not g["same_facing"], -g["area_mm2"]))
    return {"glb": glb, "triangles": len(corners), "parts": len(set(keys)), "closed_parts": len(closed),
            "look": sum(g["verdict"] == "look" for g in out),
            "hidden": sum(g["verdict"].startswith("hidden") for g in out),
            "touching": sum(g["verdict"] == "touching" for g in out), "pairs": out}


def main():
    rest = sys.argv[sys.argv.index("--") + 1:]
    report = rest[rest.index("--json") + 1] if "--json" in rest else None
    every = "--all" in rest
    glbs = [a for i, a in enumerate(rest) if not a.startswith("--") and (i == 0 or rest[i - 1] != "--json")]
    results = []
    for glb in glbs:
        r = check(glb)
        results.append(r)
        print(f"COPLANAR {glb}: {r['triangles']} tris, {r['parts']} parts ({r['closed_parts']} closed); "
              f"same facing: {r['look']} to look at, {r['hidden']} hidden; touching: {r['touching']}")
        for g in r["pairs"]:
            if every or g["verdict"] == "look":
                print(f"  {g['verdict']:<24} {g['normal']:<6} {g['area_mm2']:>9.1f} mm2  {g['parts'][0]} | {g['parts'][1]}"
                      f"  {'/'.join(g['materials'])}  lo {g['lo']} hi {g['hi']}")
    if report:
        with open(report, "w") as f:
            json.dump(results, f, indent=1)


main()
