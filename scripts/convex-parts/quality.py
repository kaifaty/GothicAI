"""Numeric collision-cache checks in source-visual centimetres."""
from __future__ import annotations

import numpy as np
from scipy.spatial import ConvexHull


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
    closed_candidates = 0
    gap_footprints = []
    for index, (shelf, above) in enumerate(zip(boards, boards[1:])):
        gaps += 1
        shelf_points = surface[shelf["triangles"]].reshape(-1, 3)
        footprint_lower, footprint_upper = shelf_points.min(axis=0), shelf_points.max(axis=0)
        center = (footprint_lower + footprint_upper) * .5
        center[1] = (shelf["top"] + above["bottom"]) * .5
        # The supporting board can extend beyond the cabinet body. Classify
        # its outer ledges independently of the requested sphere radius.
        edge_a = surface.reshape(-1, 3)
        edge_b = np.roll(surface, -1, axis=1).reshape(-1, 3)
        dy = edge_b[:, 1] - edge_a[:, 1]
        crossing = (np.abs(dy) > 1e-9) & ((edge_a[:, 1] - center[1]) * (edge_b[:, 1] - center[1]) <= 0)
        t = (center[1] - edge_a[crossing, 1]) / dy[crossing]
        section = edge_a[crossing] + t[:, None] * (edge_b[crossing] - edge_a[crossing])
        coplanar = (np.abs(edge_a[:, 1] - center[1]) <= 1e-7) & (np.abs(edge_b[:, 1] - center[1]) <= 1e-7)
        section = np.concatenate((section, edge_a[coplanar], edge_b[coplanar]))
        section = np.unique(np.round(section[:, [0, 2]], 7), axis=0)
        if len(section) < 3 or np.linalg.matrix_rank(section - section.mean(axis=0)) < 2:
            result.append(check(center, f"shelf-{index + 1}-detector-no-cross-section", detector_error=True, passed=False))
            continue
        section_hull = ConvexHull(section)
        axes = []
        for axis, count in ((0, 9), (2, 5)):
            if footprint_upper[axis] - footprint_lower[axis] >= 20:
                axes.append(np.linspace(footprint_lower[axis] + 10, footprint_upper[axis] - 10, count))
            else:
                axes.append([center[axis]])
        candidates = list(dict.fromkeys((round(float(x), 9), round(float(z), 9)) for x in axes[0] for z in axes[1]))
        center_xz = (round(float(center[0]), 9), round(float(center[2]), 9))
        if center_xz not in candidates:
            candidates.append(center_xz)
        source_open = []
        supported = 0
        exterior = 0
        boundary = 0
        for x, z in candidates:
            point = np.asarray((x, center[1], z))
            if not _ray_hits(point, np.asarray((0., -1., 0.)), surface[shelf["triangles"]]):
                continue
            if np.max(section_hull.equations[:, :2] @ (x, z) + section_hull.equations[:, 2]) >= -1e-5:
                exterior += 1
                continue
            source_clearance = float(np.min(_triangle_distances(point, surface)))
            if source_clearance <= 1e-5:
                boundary += 1
                continue
            supported += 1
            directions = [direction for direction in ((1., 0., 0.), (-1., 0., 0.), (0., 0., 1.), (0., 0., -1.))
                          if not _ray_hits(point, np.asarray(direction), surface)]
            if directions:
                source_open.append((source_clearance, point, directions))
        gap_footprints.append({"shelf_y_cm": float(shelf["top"]), "source_footprint_area_cm2": float(section_hull.volume),
                               "source_interior_candidates": supported, "source_exterior_candidates": exterior,
                               "source_boundary_candidates": boundary})
        if source_open:
            # Choose from source geometry alone; output hulls never influence
            # the selected cavity or turn a closed opening into a better probe.
            source_clearance, point, directions = max(source_open, key=lambda candidate: candidate[0])
            entry = check(point, f"open-shelf-{index + 1}", shelf_y_cm=float(shelf["top"]),
                          gap_cm=float(above["bottom"] - shelf["top"]),
                          source_clearance_cm=source_clearance, source_candidates_checked=supported,
                          source_footprint_area_cm2=float(section_hull.volume), source_exterior_candidates=exterior,
                          source_boundary_candidates=boundary,
                          source_open_candidates=len(source_open),
                          source_open_directions=[list(direction) for direction in directions])
            entry["passed"] = entry["passed"] and source_clearance >= 10 - 1e-5
            result.append(entry)
        elif supported:
            closed_candidates += supported
        else:
            result.append(check(center, f"shelf-{index + 1}-detector-no-support", detector_error=True,
                                source_candidates_checked=0, passed=False))
    if not result:
        if gaps:
            # Every supported source candidate in every detected gap is blocked
            # in all four horizontal directions: no open shelf was detected.
            result.append({"label": "closed-cupboard", "applicable": False,
                           "source_gaps_checked": gaps, "source_candidates_checked": closed_candidates,
                           "source_gap_footprints": gap_footprints,
                           "passed": True})
        else:
            result.append(check((lower + upper) * .5, "shelf-detector-no-gaps", detector_error=True, passed=False))
    return result
