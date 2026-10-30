"""Numeric collision-cache checks in source-visual centimetres."""
from __future__ import annotations

from collections import defaultdict, deque

import numpy as np
from scipy.spatial import ConvexHull


def mesh_volume(vertices, triangles):
    """Sum oriented component volumes; keep open/nonmanifold diagnostics visible.

    The signed volume of an open surface is not a proof of enclosed volume. It
    is still reported, together with the boundary count, instead of silently
    replacing the source with its convex hull or filling its missing faces.
    """
    vertices = np.asarray(vertices, dtype=np.float64)
    triangles = np.asarray(triangles, dtype=np.int64)
    welded, indices = np.unique(np.round(vertices, 7), axis=0, return_inverse=True)
    faces = indices[triangles]
    points = welded[faces]
    areas = np.linalg.norm(np.cross(points[:, 1] - points[:, 0],
                                    points[:, 2] - points[:, 0]), axis=1)
    faces = faces[areas > 1e-9]
    edges = defaultdict(list)
    for index, face in enumerate(faces):
        for a, b in zip(face, np.roll(face, -1)):
            edges[(min(a, b), max(a, b))].append((index, 1 if a < b else -1))
    adjacent = defaultdict(list)
    for entries in edges.values():
        # A nonmanifold junction does not provide a unique surface orientation.
        # Its pairwise constraints still expose inconsistencies in diagnostics.
        first, first_sign = entries[0]
        for other, other_sign in entries[1:]:
            relation = -first_sign * other_sign
            adjacent[first].append((other, relation))
            adjacent[other].append((first, relation))
    signs = np.zeros(len(faces), dtype=np.int8)
    components, conflicts, volume = 0, 0, 0.0
    for start in range(len(faces)):
        if signs[start]:
            continue
        components += 1
        queue = deque([start])
        signs[start] = 1
        component = []
        while queue:
            index = queue.popleft()
            component.append(index)
            for other, relation in adjacent[index]:
                expected = signs[index] * relation
                if signs[other] == 0:
                    signs[other] = expected
                    queue.append(other)
                elif signs[other] != expected:
                    conflicts += 1
        component = np.asarray(component)
        surface = welded[faces[component]]
        origin = surface.mean(axis=(0, 1))
        relative = surface - origin
        signed = np.einsum("ij,ij->i", relative[:, 0],
                           np.cross(relative[:, 1], relative[:, 2])) / 6.0
        volume += abs(float(np.dot(signed, signs[component])))
    boundary = sum(len(entries) == 1 for entries in edges.values())
    nonmanifold = sum(len(entries) > 2 for entries in edges.values())
    return {
        "volume_cm3": volume,
        "watertight": boundary == 0 and nonmanifold == 0 and conflicts == 0,
        "boundary_edges": boundary,
        "nonmanifold_edges": nonmanifold,
        "winding_conflicts": conflicts // 2,
        "components": components,
        "degenerate_triangles": int(len(triangles) - len(faces)),
        "volume_method": "oriented-component-surfaces",
    }


def _triangle_distances(point, triangles):
    """Exact point-to-triangle Euclidean distances, including edge regions."""
    a, b, c = triangles[:, 0], triangles[:, 1], triangles[:, 2]
    ab, ac = b - a, c - a
    normal = np.cross(ab, ac)
    normal2 = np.einsum("ij,ij->i", normal, normal)
    ap = point - a
    dot = np.einsum("ij,ij->i", ap, normal)
    safe = np.maximum(normal2, 1e-30)
    projected = ap - (dot / safe)[:, None] * normal
    d00 = np.einsum("ij,ij->i", ab, ab)
    d01 = np.einsum("ij,ij->i", ab, ac)
    d11 = np.einsum("ij,ij->i", ac, ac)
    d20 = np.einsum("ij,ij->i", projected, ab)
    d21 = np.einsum("ij,ij->i", projected, ac)
    denominator = d00 * d11 - d01 * d01
    denom_safe = np.maximum(denominator, 1e-30)
    u = (d11 * d20 - d01 * d21) / denom_safe
    v = (d00 * d21 - d01 * d20) / denom_safe
    inside = (u >= -1e-10) & (v >= -1e-10) & (u + v <= 1 + 1e-10) & (normal2 > 1e-20)
    distance2 = np.full(len(triangles), np.inf)
    distance2[inside] = dot[inside] ** 2 / normal2[inside]
    for start, end in ((a, b), (b, c), (c, a)):
        segment = end - start
        length2 = np.einsum("ij,ij->i", segment, segment)
        t = np.clip(np.einsum("ij,ij->i", point - start, segment) /
                    np.maximum(length2, 1e-30), 0, 1)
        delta = point - start - t[:, None] * segment
        distance2 = np.minimum(distance2, np.einsum("ij,ij->i", delta, delta))
    return np.sqrt(np.maximum(distance2, 0))


def _hulls(parts):
    result = []
    for points in parts:
        points = np.asarray(points, dtype=np.float64)
        hull = ConvexHull(points)
        result.append((hull.equations, points[hull.simplices]))
    return result


def _distance(point, hull):
    equations, surface = hull
    inside = np.max(equations[:, :3] @ point + equations[:, 3]) <= 1e-7
    distance = float(np.min(_triangle_distances(point, surface)))
    return -distance if inside else distance


def max_coverage_error(vertices, parts):
    hulls = _hulls(parts)
    if not hulls:
        return float("inf")
    worst = 0.0
    for point in np.unique(np.asarray(vertices, dtype=np.float64), axis=0):
        if any(np.max(equations[:, :3] @ point + equations[:, 3]) <= 1e-7 for equations, _ in hulls):
            continue
        worst = max(worst, min(float(np.min(_triangle_distances(point, surface))) for _, surface in hulls))
    return worst


def _ray_hits(origin, direction, triangles):
    edge1 = triangles[:, 1] - triangles[:, 0]
    edge2 = triangles[:, 2] - triangles[:, 0]
    p = np.cross(np.broadcast_to(direction, edge2.shape), edge2)
    determinant = np.einsum("ij,ij->i", edge1, p)
    valid = np.abs(determinant) > 1e-9
    inverse = np.divide(1.0, determinant, out=np.zeros_like(determinant), where=valid)
    delta = origin - triangles[:, 0]
    u = np.einsum("ij,ij->i", delta, p) * inverse
    q = np.cross(delta, edge1)
    v = q @ direction * inverse
    t = np.einsum("ij,ij->i", edge2, q) * inverse
    return bool(np.any(valid & (u >= -1e-8) & (v >= -1e-8) & (u + v <= 1 + 1e-8) & (t > 1e-5)))


def void_checks(vertices, triangles, parts, cls):
    vertices = np.asarray(vertices, dtype=np.float64)
    surface = vertices[np.asarray(triangles, dtype=np.int64)]
    hulls = _hulls(parts)
    lower, upper = vertices.min(axis=0), vertices.max(axis=0)

    def check(center, label, **extra):
        clearance = min(_distance(np.asarray(center), hull) for hull in hulls)
        return {"label": label, "center_cm": list(map(float, center)),
                "radius_cm": 10.0, "clearance_cm": float(clearance),
                "passed": bool(clearance >= 10 - 1e-5), **extra}

    normals = np.cross(surface[:, 1] - surface[:, 0], surface[:, 2] - surface[:, 0])
    area2 = np.linalg.norm(normals, axis=1)
    horizontal = (np.abs(normals[:, 1]) >= .98 * area2) & (area2 > 1e-8)
    if cls == "table":
        # Fix the centre from the highest broad tabletop plane, independently
        # of output. A small raised trim must not change the specified height.
        planes = []
        for index in sorted(np.flatnonzero(horizontal), key=lambda i: float(surface[i, :, 1].mean())):
            height = float(surface[index, :, 1].mean())
            if not planes or height - planes[-1]["y"] > 1:
                planes.append({"y": height, "triangles": [int(index)], "area": float(area2[index])})
            else:
                planes[-1]["triangles"].append(int(index))
                planes[-1]["area"] += float(area2[index])
        if not planes:
            return [check((lower + upper) * .5, "tabletop-detector-failed", detector_error=True, passed=False)]
        broadest = max(plane["area"] for plane in planes)
        plane = max((plane for plane in planes if plane["area"] >= broadest * .25), key=lambda p: p["y"])
        extent = surface[plane["triangles"]].reshape(-1, 3)
        center = (extent.min(axis=0) + extent.max(axis=0)) * .5
        center[1] = (lower[1] + plane["y"]) * .5
        return [check(center, "under-tabletop", tabletop_y_cm=plane["y"])]
    if cls != "cupboard":
        return []

    indices = np.flatnonzero(horizontal)
    # Group coplanar faces, then the two faces of each shelf board (<= 5 cm).
    levels = []
    for index in sorted(indices, key=lambda i: float(surface[i, :, 1].mean())):
        height = float(surface[index, :, 1].mean())
        if not levels or height - levels[-1]["y"] > 1:
            levels.append({"y": height, "triangles": [int(index)], "area": float(area2[index])})
        else:
            levels[-1]["triangles"].append(int(index))
            levels[-1]["area"] += float(area2[index])
    if not levels:
        return [check((lower + upper) * .5, "shelf-detector-failed", detector_error=True, passed=False)]
    largest = max(level["area"] for level in levels)
    levels = [level for level in levels if level["area"] >= largest * .15]
    boards = []
    for level in levels:
        if not boards or level["y"] - boards[-1]["top"] > 5:
            boards.append({"bottom": level["y"], "top": level["y"],
                           "triangles": list(level["triangles"])})
        else:
            boards[-1]["top"] = level["y"]
            boards[-1]["triangles"].extend(level["triangles"])
    result = []
    gaps = 0
    for index, (shelf, above) in enumerate(zip(boards, boards[1:])):
        gaps += 1
        shelf_points = surface[shelf["triangles"]].reshape(-1, 3)
        center = (shelf_points.min(axis=0) + shelf_points.max(axis=0)) * .5
        center[1] = (shelf["top"] + above["bottom"]) * .5
        directions = [direction for direction in ((1., 0., 0.), (-1., 0., 0.), (0., 0., 1.), (0., 0., -1.))
                      if not _ray_hits(center, np.asarray(direction), surface)]
        if directions:
            result.append(check(center, f"open-shelf-{index + 1}", shelf_y_cm=float(shelf["top"]),
                                gap_cm=float(above["bottom"] - shelf["top"]),
                                source_open_directions=[list(direction) for direction in directions]))
    if not result:
        if gaps:
            # Every detected gap is blocked in all four horizontal directions
            # by the source surfaces, so the open-shelf condition is inapplicable.
            result.append({"label": "closed-cupboard", "applicable": False,
                           "source_gaps_checked": gaps, "passed": True})
        else:
            result.append(check((lower + upper) * .5, "shelf-detector-no-gaps", detector_error=True, passed=False))
    return result
