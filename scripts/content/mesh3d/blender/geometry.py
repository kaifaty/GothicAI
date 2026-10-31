"""Mesh pilot geometry shared by Blender and the Blender-free checks.

All coordinates and UV distances are metres in glTF space (+Y up). Functions
operate on NumPy arrays; importing this module does not import Blender.
"""

import numpy as np


def linear_to_oklab(rgb):
    """Convert *linear*, nonnegative RGB to OKLab (not gamma-encoded sRGB)."""
    rgb = np.asarray(rgb, dtype=np.float64)
    if rgb.shape[-1] != 3 or np.any(rgb < 0) or not np.all(np.isfinite(rgb)):
        raise ValueError("color must be finite nonnegative linear RGB")
    lms = rgb @ np.array([
        [0.4122214708, 0.2119034982, 0.0883024619],
        [0.5363325363, 0.6806995451, 0.2817188376],
        [0.0514459929, 0.1073969566, 0.6299787005],
    ])
    return np.cbrt(lms) @ np.array([
        [0.2104542553, 1.9779984951, 0.0259040371],
        [0.7936177850, -2.4285922050, 0.7827717662],
        [-0.0040720468, 0.4505937099, -0.8086757660],
    ])


def kmeans(values, k, weights=None, max_iterations=40):
    """Deterministic, area-weighted k-means; returns labels and sorted centres.

    Farthest-point initialisation has no random seed. Empty clusters are removed,
    so a constant colour never creates duplicate material zones.
    """
    values = np.asarray(values, dtype=np.float64)
    if values.ndim != 2 or not len(values) or not np.all(np.isfinite(values)):
        raise ValueError("kmeans needs a nonempty finite matrix")
    if not 1 <= k <= 4:
        raise ValueError("k must be between 1 and 4")
    weights = np.ones(len(values)) if weights is None else np.asarray(weights, dtype=float)
    if weights.shape != (len(values),) or np.any(weights < 0) or not weights.sum() > 0:
        raise ValueError("kmeans weights must be nonnegative with positive sum")
    mean = np.average(values, axis=0, weights=weights)
    centres = [values[np.argmin(np.sum((values - mean) ** 2, axis=1))]]
    distance = np.sum((values - centres[0]) ** 2, axis=1)
    for _ in range(1, min(k, len(values))):
        # Weight suppresses insignificant colour specks during initialisation.
        index = np.argmax(distance * weights)
        if distance[index] < 1e-14:
            break
        centres.append(values[index])
        distance = np.minimum(distance, np.sum((values - values[index]) ** 2, axis=1))
    centres = np.asarray(centres)
    for _ in range(max_iterations):
        distances = np.sum((values[:, None, :] - centres[None, :, :]) ** 2, axis=2)
        labels = distances.argmin(axis=1)
        updated = []
        for index in range(len(centres)):
            selected = labels == index
            if weights[selected].sum() > 0:
                updated.append(np.average(values[selected], axis=0, weights=weights[selected]))
        updated = np.asarray(updated)
        if updated.shape == centres.shape and np.max(np.abs(updated - centres)) < 1e-7:
            centres = updated
            break
        centres = updated
    order = np.lexsort(centres.T[::-1])
    centres = centres[order]
    labels = np.sum((values[:, None, :] - centres[None, :, :]) ** 2, axis=2).argmin(axis=1)
    return labels.astype(np.int32), centres


def choose_clusters(values, max_clusters=3, weights=None):
    """Choose 1–3 visible OKLab clusters without splitting small colour noise."""
    values = np.asarray(values, dtype=float)
    weights = np.ones(len(values)) if weights is None else np.asarray(weights, dtype=float)
    if not 1 <= max_clusters <= 3:
        raise ValueError("wood supports 1 to 3 clusters")
    best = None
    best_score = float("inf")
    for count in range(1, max_clusters + 1):
        labels, centres = kmeans(values, count, weights)
        residual = np.sum((values - centres[labels]) ** 2, axis=1)
        # A new zone must explain a visible OKLab difference, not generator noise.
        score = np.average(residual, weights=weights) + 0.0004 * len(centres)
        if score < best_score:
            best_score = score
            best = labels, centres
    return best


def triangle_areas(vertices, faces):
    triangle = np.asarray(vertices, dtype=float)[np.asarray(faces, dtype=np.int64)]
    return np.linalg.norm(np.cross(triangle[:, 1] - triangle[:, 0], triangle[:, 2] - triangle[:, 0]), axis=1) * 0.5


def classify_zones(vertices, faces, color, metallic, zones):
    """Return face material-slot indices. Explicit metallic takes precedence.

    WOOD zones are assigned from dark to light OKLab clusters, in target order.
    Without metallic the darkest neutral cluster (L < .55, chroma < .06) is iron.
    Metal uses the first METAL zone; metallic and color do not identify its role.
    A single other nonmetal zone is supported for targets with no WOOD slot.
    """
    faces = np.asarray(faces, dtype=np.int64)
    color = np.asarray(color, dtype=float)
    if color.shape != (len(vertices), 3):
        raise ValueError("color must have one linear RGB value per vertex")
    metal_slots = [i for i, zone in enumerate(zones) if zone["group"].upper() == "METAL"]
    wood_slots = [i for i, zone in enumerate(zones) if zone["group"].upper() == "WOOD"]
    if not wood_slots:
        wood_slots = [i for i in range(len(zones)) if i not in metal_slots]
        if len(wood_slots) != 1:
            raise ValueError("non-WOOD material classification needs one nonmetal zone")
    elif any(i not in metal_slots + wood_slots for i in range(len(zones))):
        raise ValueError("color classification only supports WOOD and METAL together")
    areas = triangle_areas(vertices, faces)
    if not areas.sum() > 0:
        raise ValueError("mesh has no surface area")
    lab = linear_to_oklab(color[faces].mean(axis=1))
    is_metal = np.zeros(len(faces), dtype=bool)
    if metallic is not None:
        metallic = np.asarray(metallic, dtype=float)
        if metallic.shape != (len(vertices),) or not np.all(np.isfinite(metallic)):
            raise ValueError("metallic must have one finite scalar per vertex")
        is_metal = metallic[faces].mean(axis=1) > 0.5
    elif metal_slots:
        clusters, centres = kmeans(lab, 4, areas)
        neutral = (centres[:, 0] < 0.55) & (np.linalg.norm(centres[:, 1:], axis=1) < 0.06)
        candidates = np.flatnonzero(neutral)
        if len(candidates):
            is_metal = clusters == candidates[np.argmin(centres[candidates, 0])]
    if is_metal.any() and not metal_slots:
        raise ValueError("mesh has metallic faces but target has no METAL zone")
    labels = np.full(len(faces), wood_slots[0], dtype=np.int32)
    if is_metal.any():
        labels[is_metal] = metal_slots[0]
    wood = ~is_metal
    if wood.any():
        clusters, _ = choose_clusters(lab[wood], min(3, len(wood_slots)), areas[wood])
        labels[wood] = np.asarray(wood_slots)[clusters]
    return labels


def face_adjacency(faces):
    """Pairs of faces sharing a manifold edge; nonmanifold edges do not vote."""
    faces = np.asarray(faces, dtype=np.int64)
    edges = np.concatenate((faces[:, [0, 1]], faces[:, [1, 2]], faces[:, [2, 0]]))
    edges.sort(axis=1)
    owners = np.tile(np.arange(len(faces)), 3)
    order = np.lexsort((edges[:, 1], edges[:, 0]))
    edges, owners = edges[order], owners[order]
    starts = np.r_[0, np.flatnonzero(np.any(edges[1:] != edges[:-1], axis=1)) + 1]
    counts = np.diff(np.r_[starts, len(edges)])
    paired = starts[counts == 2]
    return np.column_stack((owners[paired], owners[paired + 1]))


def smooth_labels(faces, labels, areas, iterations=2, locked_labels=()):
    """Area-weighted face-neighbour votes, retaining explicit metal boundaries.

    Locked labels neither change nor propagate. A face itself casts one vote;
    ties retain its existing label. This removes islands without eroding iron.
    """
    labels = np.asarray(labels, dtype=np.int32).copy()
    areas = np.asarray(areas, dtype=float)
    if len(labels) != len(faces) or areas.shape != labels.shape or np.any(labels < 0):
        raise ValueError("labels and areas must match the faces")
    adjacency = face_adjacency(faces)
    source = np.r_[adjacency[:, 0], adjacency[:, 1]]
    destination = np.r_[adjacency[:, 1], adjacency[:, 0]]
    locked = np.isin(labels, locked_labels)
    for _ in range(iterations):
        votes = np.zeros((len(labels), int(labels.max()) + 1))
        votes[np.arange(len(labels)), labels] = areas
        eligible = ~locked[source] & ~locked[destination]
        np.add.at(votes, (destination[eligible], labels[source[eligible]]), areas[source[eligible]])
        winner = votes.argmax(axis=1)
        current = votes[np.arange(len(labels)), labels]
        change = ~locked & (votes[np.arange(len(labels)), winner] > current + 1e-12)
        labels[change] = winner[change]
    return labels


def budget_ratio(current, budget):
    """Collapse ratio with a small rounding allowance; never request > 1."""
    if current <= 0 or budget < 4:
        raise ValueError("positive triangle count and budget >= 4 required")
    return min(1.0, (budget - 1) / current) if current > budget else 1.0


def simplify_loop(points, tolerance):
    """Keep sorted indices of a closed 3D polyline within a metric tolerance.

    Two far-apart points split the ring into open Ramer–Douglas–Peucker halves.
    Each discarded point lies within ``tolerance`` of its retained chord. The
    result retains at least three points when the input has at least three.
    A repeated final copy of the first point is omitted from the output.
    """
    points = np.asarray(points, dtype=float)
    if points.ndim != 2 or points.shape[1] != 3 or not np.isfinite(points).all():
        raise ValueError("loop points must be a finite (N,3) matrix")
    if not np.isfinite(tolerance) or tolerance < 0:
        raise ValueError("loop tolerance must be finite and nonnegative")
    count = len(points)
    if count > 1 and np.array_equal(points[0], points[-1]):
        count -= 1
        points = points[:count]
    if count <= 3:
        return np.arange(count, dtype=np.int32)
    first = int(np.argmax(np.sum((points - points.mean(axis=0)) ** 2, axis=1)))
    second = int(np.argmax(np.sum((points - points[first]) ** 2, axis=1)))
    if first == second:
        return np.arange(3, dtype=np.int32)
    first, second = sorted((first, second))
    retained = {first, second}

    def chord_distance(candidates, start, finish):
        delta = finish - start
        length_squared = float(delta @ delta)
        if length_squared == 0:
            return np.sum((candidates - start) ** 2, axis=1)
        projection = np.clip((candidates - start) @ delta / length_squared, 0, 1)
        return np.sum((candidates - start - projection[:, None] * delta) ** 2, axis=1)

    halves = (np.arange(first, second + 1),
              np.r_[np.arange(second, count), np.arange(first + 1)])
    for half in halves:
        pending = [(0, len(half) - 1)]
        while pending:
            start, finish = pending.pop()
            if finish - start <= 1:
                continue
            candidates = half[start + 1:finish]
            distances = chord_distance(points[candidates], points[half[start]], points[half[finish]])
            furthest = int(np.argmax(distances))
            if distances[furthest] > tolerance ** 2:
                pivot = start + 1 + furthest
                retained.add(int(half[pivot]))
                pending.extend(((start, pivot), (pivot, finish)))
    if len(retained) < 3:
        candidates = np.array([i for i in range(count) if i not in retained])
        distances = chord_distance(points[candidates], points[first], points[second])
        retained.add(int(candidates[np.argmax(distances)]))
    return np.asarray(sorted(retained), dtype=np.int32)


def _horizontal_hull(points):
    points = np.unique(np.asarray(points, dtype=float), axis=0)
    if len(points) < 3:
        raise ValueError("mesh has no horizontal extent")
    def cross(a, b, c):
        return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
    lower, upper = [], []
    for point in points:
        while len(lower) > 1 and cross(lower[-2], lower[-1], point) <= 0:
            lower.pop()
        lower.append(point)
    for point in points[::-1]:
        while len(upper) > 1 and cross(upper[-2], upper[-1], point) <= 0:
            upper.pop()
        upper.append(point)
    return np.asarray(lower[:-1] + upper[:-1])


def select_uv_projection(vertices, faces, target):
    """Select a target override or detect round sections around the +Y axis.

    Inspect the mesh in oriented glTF coordinates (+Y up). Face
    intersections work for a low-poly cylinder whose vertices occur only at its
    ends, as well as a densely sampled bowed barrel. Section circularity uses
    edges as well as corners, so a cube's four equidistant corners cannot pass.
    Neither the target id nor its asset kind controls the projection.
    """
    if "uv_projection" in target:
        projection = target["uv_projection"]
        if projection not in ("cylinder", "box"):
            raise ValueError("uv_projection must be cylinder or box")
        return projection
    vertices = np.asarray(vertices, dtype=float)
    faces = np.asarray(faces, dtype=np.int64)
    if not len(vertices) or not len(faces):
        return "box"
    triangles = vertices[faces]
    minimum, maximum = triangles[:, :, 1].min(axis=1), triangles[:, :, 1].max(axis=1)
    bottom, top = minimum.min(), maximum.max()
    if top - bottom <= 1e-12:
        return "box"
    centers, radii = [], []
    for fraction in (.15, .325, .5, .675, .85):
        level = bottom + (top - bottom) * fraction
        section = triangles[(minimum <= level) & (maximum > level)]
        points = []
        for a, b in ((0, 1), (1, 2), (2, 0)):
            crossing = (section[:, a, 1] <= level) != (section[:, b, 1] <= level)
            start, end = section[crossing, a], section[crossing, b]
            fraction_y = (level - start[:, 1]) / (end[:, 1] - start[:, 1])
            points.append((start + fraction_y[:, None] * (end - start))[:, [0, 2]])
        points = np.unique(np.concatenate(points), axis=0)
        if len(points) < 3:
            return "box"
        hull = _horizontal_hull(points)
        if len(hull) < 3:
            return "box"
        following = np.roll(hull, -1, axis=0)
        area = abs(np.sum(hull[:, 0] * following[:, 1] - hull[:, 1] * following[:, 0])) * .5
        perimeter = np.linalg.norm(following - hull, axis=1).sum()
        if perimeter <= 1e-12 or 4 * np.pi * area / perimeter ** 2 < .93:
            return "box"
        centers.append((hull.min(axis=0) + hull.max(axis=0)) * .5)
        radii.append(np.sqrt(area / np.pi))
    centers = np.asarray(centers)
    drift = np.linalg.norm(centers - np.median(centers, axis=0), axis=1).max()
    return "cylinder" if drift <= np.median(radii) * .08 else "box"


def fit_bbox(vertices, meta, target):
    """Rotate declared up to +Y, orient minimum horizontal OBB, fit target bbox.

    The target's bbox minimum is the anchor; a base at Y=0 stays on the ground.
    Independent axis scaling matches the requested item dimensions exactly.
    """
    vertices = np.asarray(vertices, dtype=float)
    up_axis = meta["up_axis"].upper()
    if up_axis not in ("+X", "-X", "+Y", "-Y", "+Z", "-Z"):
        raise ValueError("up_axis must be signed X, Y or Z")
    units = float(meta["meters_per_unit"])
    if not np.isfinite(units) or units <= 0:
        raise ValueError("meters_per_unit must be positive")
    up = np.eye(3)["XYZ".index(up_axis[1])] * (1 if up_axis[0] == "+" else -1)
    destination = np.array([0., 1., 0.])
    axis = np.cross(up, destination)
    cosine = float(up @ destination)
    if cosine < -0.999:
        rotation = np.diag([1., -1., -1.])
    else:
        skew = np.array([[0., -axis[2], axis[1]], [axis[2], 0., -axis[0]], [-axis[1], axis[0], 0.]])
        rotation = np.eye(3) + skew + skew @ skew / (1 + cosine)
    points = (vertices * units) @ rotation.T
    lower = np.asarray(target["bbox_min"], dtype=float)
    upper = np.asarray(target["bbox_max"], dtype=float)
    size = upper - lower
    if lower.shape != (3,) or not np.all(np.isfinite(size)) or np.any(size <= 0):
        raise ValueError("target bbox must have positive finite dimensions")
    hull = _horizontal_hull(points[:, [0, 2]])
    edges = np.roll(hull, -1, axis=0) - hull
    angles = np.unique(np.round(np.mod(np.arctan2(edges[:, 1], edges[:, 0]), np.pi / 2), 10))
    best = None
    for angle in angles:
        basis = np.array([[np.cos(angle), -np.sin(angle)], [np.sin(angle), np.cos(angle)]])
        extent = np.ptp(hull @ basis, axis=0)
        area = np.prod(extent)
        # Equal-area orientations choose the target-compatible width/depth.
        distortion = abs(np.log((extent[0] / extent[1]) / (size[0] / size[2])))
        alternative = abs(np.log((extent[1] / extent[0]) / (size[0] / size[2])))
        if alternative < distortion:
            basis = basis @ np.array([[0., -1.], [1., 0.]])
            distortion = alternative
        candidate = (area, distortion, basis)
        if best is None or area < best[0] - max(1e-12, best[0] * 1e-8) or (np.isclose(area, best[0], rtol=1e-8, atol=1e-12) and distortion < best[1]):
            best = candidate
    points[:, [0, 2]] = points[:, [0, 2]] @ best[2]
    extent = np.ptp(points, axis=0)
    if np.any(extent <= 1e-12):
        raise ValueError("mesh must have three-dimensional extent")
    return ((points - points.min(axis=0)) * (size / extent) + lower).astype(np.float32)


def _uv_patches(vertices, faces):
    """Connected surface patches, stopping at a normal turn of twenty degrees."""
    triangles = vertices[faces]
    normal = np.cross(triangles[:, 1] - triangles[:, 0], triangles[:, 2] - triangles[:, 0])
    area = np.linalg.norm(normal, axis=1)
    normal /= area[:, None]
    pairs = face_adjacency(faces)
    pairs = pairs[np.einsum("ij,ij->i", normal[pairs[:, 0]], normal[pairs[:, 1]]) > np.cos(np.deg2rad(20))]
    parents = np.arange(len(faces))

    def root(face):
        while parents[face] != face:
            parents[face] = parents[parents[face]]
            face = parents[face]
        return face

    for a, b in pairs:
        a, b = root(a), root(b)
        if a != b:
            parents[b] = a
    labels = np.array([root(face) for face in range(len(faces))])
    _, labels = np.unique(labels, return_inverse=True)
    return labels.astype(np.int32), normal, area


def _planar_patch_uv(points, normal):
    """One metric OBB plane and origin for the complete connected patch."""
    centre = points.mean(axis=0)
    _, axes = np.linalg.eigh((points - centre).T @ (points - centre))
    vertical = axes[:, -1] - normal * (axes[:, -1] @ normal)
    vertical /= np.linalg.norm(vertical)
    horizontal = np.cross(vertical, normal)
    return np.column_stack(((points - centre) @ horizontal, (points - centre) @ vertical))


def box_uv(vertices, faces, return_patches=False):
    """Metre-scale box charts on connected, similarly oriented surface patches."""
    vertices = np.asarray(vertices, dtype=float)
    faces = np.asarray(faces, dtype=np.int64)
    labels, normals, areas = _uv_patches(vertices, faces)
    uv = np.empty((len(faces), 3, 2), dtype=np.float32)
    for label in range(int(labels.max()) + 1):
        selected = labels == label
        used, corners = np.unique(faces[selected], return_inverse=True)
        normal = np.sum(normals[selected] * areas[selected, None], axis=0)
        normal /= np.linalg.norm(normal)
        uv[selected] = _planar_patch_uv(vertices[used], normal)[corners].reshape(-1, 3, 2)
    types = np.zeros(len(faces), dtype=np.int32)
    return (uv, labels, types) if return_patches else uv


def _angular_profile(angle, radius, weights):
    """Area-weighted mean section and its physical arc, without ellipse fitting."""
    circular = np.mod(angle, 2 * np.pi)
    ordered = np.sort(circular)
    gaps = np.diff(np.r_[ordered, ordered[0] + 2 * np.pi])
    first = ordered[(int(np.argmax(gaps)) + 1) % len(ordered)]
    covered = 2 * np.pi - gaps.max()
    periodic = covered > np.pi
    if periodic:
        # A decimated pilot has only a few samples per 128-bin sector.
        # Keep angular resolution broad enough to average their height mix.
        count = 32
        grid = -np.pi + (np.arange(count) + .5) * 2 * np.pi / count
        samples = np.mod(angle + np.pi, 2 * np.pi) - np.pi
        # atan2 of points on the same ray can differ by a few ulps as their
        # radius changes. Keep those samples in one bin at exact boundaries.
        scaled = np.round((samples + np.pi) / (2 * np.pi) * count, 12)
        bins = np.floor(scaled).astype(int) % count
        samples = np.where((bins == 0) & (samples > 0), samples - 2 * np.pi, samples)
    else:
        count = max(3, int(np.ceil(covered / (2 * np.pi / 32))) + 1)
        grid = np.linspace(first, first + covered, count)
        samples = np.mod(circular - first, 2 * np.pi) + first
        bins = np.rint((samples - first) / covered * (count - 1)).astype(int)
    totals = np.bincount(bins, weights=weights, minlength=count)
    occupied = totals > 0
    sample_angle = np.bincount(bins, weights=weights * samples, minlength=count)[occupied] / totals[occupied]
    sample_radius = np.bincount(bins, weights=weights * radius, minlength=count)[occupied] / totals[occupied]
    profile = np.interp(grid, sample_angle, sample_radius,
                        period=2 * np.pi if periodic else None)
    if periodic:
        smooth = (np.roll(profile, 2) + 4 * np.roll(profile, 1) + 6 * profile
                  + 4 * np.roll(profile, -1) + np.roll(profile, -2)) / 16
        derivative = (np.roll(smooth, -1) - np.roll(smooth, 1)) / (2 * (grid[1] - grid[0]))
        speed = np.hypot(smooth, derivative)
        grid = np.r_[grid, grid[0] + 2 * np.pi]
        speed = np.r_[speed, speed[0]]
        smooth = np.r_[smooth, smooth[0]]
    else:
        profile[0] = sample_radius[0] + (grid[0] - sample_angle[0]) * (sample_radius[1] - sample_radius[0]) / (sample_angle[1] - sample_angle[0])
        profile[-1] = sample_radius[-1] + (grid[-1] - sample_angle[-1]) * (sample_radius[-1] - sample_radius[-2]) / (sample_angle[-1] - sample_angle[-2])
        padded = np.r_[2 * profile[0] - profile[1], profile, 2 * profile[-1] - profile[-2]]
        smooth = np.convolve(padded, [.25, .5, .25], mode="valid")
        speed = np.hypot(smooth, np.gradient(smooth, grid))
    arc = np.r_[0., np.cumsum((speed[:-1] + speed[1:]) * np.diff(grid) * .5)]
    return grid, arc, periodic, smooth


def cylinder_uv(vertices, faces, return_patches=False):
    """Shared cap, cylindrical and OBB charts around the object's +Y axis.

    A chart never changes its projection, origin or scale between triangles.
    Cylinder U integrates the arc of a smoothed, angle-binned mean section.
    V integrates a smoothed, height-binned physical radius profile shared by
    every vertex at that height. Two half charts put the wrapping seam
    on patch boundaries. Returned face types are 0=box, 1=cylinder and 2=cap.
    """
    vertices = np.asarray(vertices, dtype=float)
    faces = np.asarray(faces, dtype=np.int64)
    labels, normals, areas = _uv_patches(vertices, faces)
    centre = (vertices.min(axis=0) + vertices.max(axis=0)) * .5
    local = vertices - centre
    axes = np.ptp(vertices[:, [0, 2]], axis=0) * .5
    theta = np.arctan2(local[:, 2], local[:, 0])
    radius = np.linalg.norm(local[:, [0, 2]], axis=1)
    uv = np.empty((len(faces), 3, 2), dtype=np.float32)
    patch_ids = np.empty(len(faces), dtype=np.int32)
    types = np.empty(len(faces), dtype=np.int32)
    next_id = 0
    for label in range(int(labels.max()) + 1):
        selected = np.flatnonzero(labels == label)
        planar = np.all(normals[selected] @ normals[selected[0]] > 1 - 1e-6)
        cap = np.average(np.abs(normals[selected, 1]), weights=areas[selected]) > .98
        radial = local[faces[selected]].mean(axis=1)[:, [0, 2]] / axes ** 2
        radial /= np.linalg.norm(radial, axis=1)[:, None]
        alignment = np.abs(np.einsum("ij,ij->i", normals[selected][:, [0, 2]], radial))
        cylindrical = not planar and not cap and np.average(alignment, weights=areas[selected]) > .85
        if cylindrical:
            patch_vertices, patch_corners = np.unique(faces[selected], return_inverse=True)
            vertex_weights = np.bincount(patch_corners.ravel(), weights=np.repeat(areas[selected], 3))
            angular_grid, angular_arc, periodic, angular_radius = _angular_profile(
                theta[patch_vertices], radius[patch_vertices], vertex_weights)
            mean_radius = np.sum((angular_radius[:-1] + angular_radius[1:]) * np.diff(angular_grid) * .5) / np.ptp(angular_grid)
        # Separate half charts before assigning coordinates; no internal edge
        # crosses the angular branch cut. Flat patches need no artificial seam.
        halves = (local[faces[selected], 2].mean(axis=1) >= 0).astype(int) if cylindrical else np.zeros(len(selected), dtype=int)
        for half in np.unique(halves):
            group = selected[halves == half]
            used, corners = np.unique(faces[group], return_inverse=True)
            points = local[used]
            if cap:
                coordinates = points[:, [0, 2]]
                projection = 2
            elif not cylindrical:
                normal = np.sum(normals[group] * areas[group, None], axis=0)
                normal /= np.linalg.norm(normal)
                coordinates = _planar_patch_uv(points, normal)
                projection = 0
            else:
                angle = theta[used].copy()
                if half:
                    angle[angle < -np.pi / 2] += 2 * np.pi
                else:
                    angle[angle > np.pi / 2] -= 2 * np.pi
                wrapped = np.mod(np.mod(angle, 2 * np.pi) - angular_grid[0], 2 * np.pi) + angular_grid[0]
                horizontal = np.interp(wrapped, angular_grid, angular_arc)
                if periodic:
                    horizontal += np.floor((angle - angular_grid[0]) / (2 * np.pi)) * angular_arc[-1]
                height = points[:, 1]
                bottom, top = height.min(), height.max()
                span = top - bottom
                if span <= 1e-12:
                    raise ValueError("cylindrical UV patch needs a vertical extent")
                # Remove the section's angular radius before height binning:
                # an ellipse sampled unevenly must not invent meridian slopes.
                section_radius = np.interp(wrapped, angular_grid, angular_radius)
                height_radius = radius[used] / section_radius * mean_radius
                # One radius profile for the chart. Smoothing retains a linear
                # cone exactly, with one V coordinate for every shared height.
                bins = np.rint((height - bottom) / span * 32).astype(int)
                counts = np.bincount(bins, minlength=33)
                occupied = counts > 0
                sample_height = np.bincount(bins, weights=height, minlength=33)[occupied] / counts[occupied]
                sample_radius = np.bincount(bins, weights=height_radius, minlength=33)[occupied] / counts[occupied]
                grid_height = np.linspace(bottom, top, 33)
                grid_radius = np.interp(grid_height, sample_height, sample_radius)
                # Extrapolate to the true ends instead of clamping V in the
                # first/last bin. Linear padding avoids flattening end slopes.
                grid_radius[0] = sample_radius[0] + (bottom - sample_height[0]) * (sample_radius[1] - sample_radius[0]) / (sample_height[1] - sample_height[0])
                grid_radius[-1] = sample_radius[-1] + (top - sample_height[-1]) * (sample_radius[-1] - sample_radius[-2]) / (sample_height[-1] - sample_height[-2])
                padding = np.r_[2 * grid_radius[0] - grid_radius[1], grid_radius,
                                2 * grid_radius[-1] - grid_radius[-2]]
                smooth_radius = np.convolve(padding, [.25, .5, .25], mode="valid")
                meridian = np.r_[0., np.cumsum(np.hypot(np.diff(grid_height), np.diff(smooth_radius)))]
                vertical = np.interp(height, grid_height, meridian)
                coordinates = np.column_stack((horizontal, vertical))
                projection = 1
            uv[group] = coordinates[corners].reshape(-1, 3, 2)
            patch_ids[group] = next_id
            types[group] = projection
            next_id += 1
    return (uv, patch_ids, types) if return_patches else uv
