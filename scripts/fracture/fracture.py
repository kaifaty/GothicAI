#!/usr/bin/env python3
"""Fixed-seed Voronoi fracture of source shells, in Gothic centimetres.

Stock meshes and pieces contain game assets and belong in the local cache,
never in git. Content mode reads only our indexed GLBs, without game archives,
and writes adjacent version 2 sidecars. Requires numpy and scipy (fracture.sh).
"""
from __future__ import annotations

import argparse
import csv
import fnmatch
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tempfile
import time
import zlib

import numpy as np
from scipy.spatial import ConvexHull, HalfspaceIntersection, cKDTree


def read_source(path: Path):
    lines = iter(path.read_text().splitlines())
    triangles, textures = [], []
    for line in lines:
        tag, texture = line.split(maxsplit=1)
        if tag != "tri":
            raise ValueError(f"{path}: expected tri")
        textures.append(texture)
        triangles.append([[float(x) for x in next(lines).split()] for _ in range(3)])
    vertices = np.asarray(triangles, dtype=np.float64)
    if vertices.shape[1:] != (3, 8) or not np.isfinite(vertices).all():
        raise ValueError(f"{path}: malformed source vertices")
    return vertices, textures


def number(value):
    return "0" if abs(value) < 1e-12 else format(float(value), ".12g")


def outward_triangles(points):
    """Stable ordering, outward winding and no duplicate/interior vertices."""
    # Many original faces meet at one corner. Halfspace arithmetic can produce
    # several nearly identical copies; weld them before triangulating, so those
    # copies cannot become zero-area triangles after runtime float conversion.
    points = np.unique(points, axis=0)
    parents = list(range(len(points)))

    def root(index):
        while parents[index] != index:
            parents[index] = parents[parents[index]]
            index = parents[index]
        return index

    for a, b in sorted(cKDTree(points).query_pairs(0.0001)):
        a, b = root(a), root(b)
        parents[max(a, b)] = min(a, b)
    groups = {}
    for index, point in enumerate(points):
        groups.setdefault(root(index), []).append(point)
    points = np.asarray([np.mean(group, axis=0) for _, group in sorted(groups.items())])
    # Hull topology must be computed in the precision used by the engine. Doing
    # the float conversion later can flip a very thin, almost coplanar facet.
    points = np.unique(points.astype(np.float32).astype(np.float64), axis=0)
    hull = ConvexHull(points)
    faces = []
    for indices, plane in zip(hull.simplices, hull.equations):
        indices = list(map(int, indices))
        triangle = points[indices]
        if np.dot(np.cross(triangle[1] - triangle[0], triangle[2] - triangle[0]), plane[:3]) < 0:
            indices[1], indices[2] = indices[2], indices[1]
        start = indices.index(min(indices))
        indices = indices[start:] + indices[:start]
        faces.append((tuple(indices), plane[:3]))
    faces.sort(key=lambda face: face[0])
    return points, hull, faces


def fracture(source, textures, seed, count):
    positions = np.unique(source[:, :, :3].reshape(-1, 3), axis=0)
    outer = ConvexHull(positions)
    # Sorting and deduplicating coplanar hull equations avoids input-order effects.
    planes = np.unique(np.round(outer.equations, 11), axis=0)
    lower, upper = positions.min(axis=0), positions.max(axis=0)
    rng = np.random.Generator(np.random.PCG64(seed))
    sites = []
    while len(sites) < count:
        site = rng.uniform(lower, upper)
        if np.max(planes[:, :3] @ site + planes[:, 3]) < -1e-6:
            sites.append(site)
    sites = np.asarray(sites)

    source_positions = source[:, :, :3]
    cross = np.cross(source_positions[:, 1] - source_positions[:, 0],
                     source_positions[:, 2] - source_positions[:, 0])
    areas = np.linalg.norm(cross, axis=1)
    normals = cross / np.maximum(areas[:, None], 1e-30)
    texture_areas = {}
    for texture, area in zip(textures, areas):
        texture_areas[texture] = texture_areas.get(texture, 0) + area
    cut_texture = max(sorted(texture_areas), key=texture_areas.get)

    out = io.StringIO()
    out.write(f"GOTHIC_FRACTURE 1\npieces {count}\n")
    piece_volumes = []
    for index, site in enumerate(sites):
        # The site lies strictly inside every bisector and the source hull.
        bisectors = []
        for j, other in enumerate(sites):
            if index == j:
                continue
            normal = 2 * (other - site)
            distance = np.dot(site, site) - np.dot(other, other)
            norm = np.linalg.norm(normal)
            bisectors.append(np.r_[normal / norm, distance / norm])
        halfspaces = np.vstack((planes, bisectors)) if bisectors else planes
        intersections = HalfspaceIntersection(halfspaces, site).intersections
        points, hull, faces = outward_triangles(intersections)
        centroid = np.zeros(3)
        volume = 0.0
        for indices, _ in faces:
            a, b, c = points[list(indices)]
            tetrahedron = np.dot(a - site, np.cross(b - site, c - site)) / 6
            centroid += tetrahedron * (site + a + b + c) / 4
            volume += tetrahedron
        centroid /= volume
        piece_volumes.append(volume)
        out.write("piece " + " ".join(number(x) for x in [volume, *centroid]) + f" {len(faces)}\n")
        for indices, normal in faces:
            triangle = points[list(indices)]
            # Keep source UV on an exterior face. A triangulated convex hull can
            # merge coplanar source triangles, so use their affine UV mapping.
            plane_distance = np.abs((source_positions - triangle[0]) @ normal).max(axis=1)
            alignment = np.abs(normals @ normal)
            candidates = np.flatnonzero((plane_distance < 0.001) & (alignment > 0.999))
            if len(candidates):
                middle = triangle.mean(axis=0)
                chosen = min(candidates, key=lambda i: float(np.linalg.norm(source_positions[i].mean(axis=0) - middle)))
                original = source_positions[chosen]
                basis = np.column_stack((original[1] - original[0], original[2] - original[0]))
                coordinates = np.linalg.lstsq(basis, (triangle - original[0]).T, rcond=None)[0].T
                uv = source[chosen, 0, 6:] + coordinates @ (source[chosen, 1:, 6:] - source[chosen, 0, 6:])
                texture = textures[chosen]
            else:
                uv = None
                texture = cut_texture
            if uv is None or abs(np.linalg.det(uv[1:] - uv[0])) < 1e-15:
                # A material-space planar projection: 128 cm per texture tile,
                # orthogonal to the cut normal, with no collapsed UV triangles.
                axis = np.eye(3)[int(np.argmin(np.abs(normal)))]
                tangent = np.cross(axis, normal)
                tangent /= np.linalg.norm(tangent)
                bitangent = np.cross(normal, tangent)
                uv = np.column_stack((triangle @ tangent, triangle @ bitangent)) / 128
            out.write(f"tri {texture}\n")
            for position, texcoord in zip(triangle, uv):
                out.write(" ".join(number(x) for x in [*position, *normal, *texcoord]) + "\n")
    ratio = sum(piece_volumes) / outer.volume
    if not 0.9 <= ratio <= 1.1:
        raise ValueError(f"fracture volume ratio {ratio} outside [0.9, 1.1]")
    return out.getvalue().encode("ascii"), outer.volume, ratio


def fracture_boards(source, textures, seed, count):
    """Cut each connected fence board separately, keeping the gaps empty."""
    positions, indices = np.unique(source[:, :, :3].reshape(-1, 3), axis=0, return_inverse=True)
    triangles = indices.reshape(-1, 3)
    parents = list(range(len(positions)))

    def root(index):
        while parents[index] != index:
            parents[index] = parents[parents[index]]
            index = parents[index]
        return index

    for triangle in triangles:
        first = root(int(triangle[0]))
        for index in triangle[1:]:
            parents[root(int(index))] = first
    groups = {}
    for index, triangle in enumerate(triangles):
        groups.setdefault(root(int(triangle[0])), []).append(index)
    boards = [source[group] for _, group in sorted(groups.items())]
    materials = [[textures[i] for i in group] for _, group in sorted(groups.items())]
    if len(boards) > count:
        raise ValueError(f"fence has {len(boards)} boards, exceeding {count} pieces")
    volumes = [ConvexHull(board[:, :, :3].reshape(-1, 3)).volume for board in boards]
    allocation = [1] * len(boards)
    # Stable, volume-weighted allocation, with at least one piece per board.
    for _ in range(count - len(boards)):
        largest = max(range(len(boards)), key=lambda i: volumes[i] / (allocation[i] + 1))
        allocation[largest] += 1
    pieces = []
    for index, (board, material, amount) in enumerate(zip(boards, materials, allocation)):
        data, _, _ = fracture(board, material, seed + index, amount)
        pieces.extend(data.decode("ascii").splitlines()[2:])
    data = (f"GOTHIC_FRACTURE 1\npieces {count}\n" + "\n".join(pieces) + "\n").encode("ascii")
    return data, sum(volumes), 1.0


def bind_source(data, glb):
    """Version 2 binds the pieces to the exact GLB bytes that were converted."""
    header, body = data.split(b"\n", 1)
    if header != b"GOTHIC_FRACTURE 1":
        raise ValueError("expected version 1 fracture")
    return (f"GOTHIC_FRACTURE 2\nsource {len(glb)} {zlib.crc32(glb):08x}\n".encode("ascii") + body)


def validate(data, glb=None):
    """Check the actual serialized geometry consumed by the game."""
    lines = iter(data.decode("ascii").splitlines())
    header = next(lines)
    if header == "GOTHIC_FRACTURE 2":
        source = next(lines).split()
        if (len(source) != 3 or source[0] != "source" or glb is None or
                int(source[1]) != len(glb) or int(source[2], 16) != zlib.crc32(glb)):
            raise ValueError("fracture source does not match GLB size and CRC32")
    elif header != "GOTHIC_FRACTURE 1" or glb is not None:
        raise ValueError("invalid header")
    count = int(next(lines).split()[1])
    if not 8 <= count <= 20:
        raise ValueError(f"invalid piece count {count}")
    volume_sum = 0
    for _ in range(count):
        header = next(lines).split()
        if header[0] != "piece":
            raise ValueError("missing piece")
        declared_volume = float(header[1])
        triangles = []
        edge_counts = {}
        edge_directions = {}
        for _ in range(int(header[5])):
            if not next(lines).startswith("tri "):
                raise ValueError("missing triangle texture")
            vertices = np.asarray([[float(x) for x in next(lines).split()] for _ in range(3)],
                                  dtype=np.float32).astype(np.float64)
            if vertices.shape != (3, 8) or not np.isfinite(vertices).all():
                raise ValueError("invalid triangle")
            triangle = vertices[:, :3]
            if abs(np.linalg.det(vertices[1:, 6:] - vertices[0, 6:])) < 1e-15:
                raise ValueError("degenerate texture coordinates")
            triangles.append(triangle)
            for i in range(3):
                a, b = tuple(triangle[i]), tuple(triangle[(i + 1) % 3])
                edge = tuple(sorted((a, b)))
                edge_counts[edge] = edge_counts.get(edge, 0) + 1
                edge_directions[edge] = edge_directions.get(edge, 0) + (1 if a < b else -1)
        if any(n != 2 for n in edge_counts.values()) or any(edge_directions.values()):
            raise ValueError("piece is not a closed, consistently wound two-manifold")
        triangles = np.asarray(triangles)
        points = np.unique(triangles.reshape(-1, 3), axis=0)
        for triangle in triangles:
            normal = np.cross(triangle[1] - triangle[0], triangle[2] - triangle[0])
            normal /= np.linalg.norm(normal)
            if np.max((points - triangle[0]) @ normal) > 1e-4:
                raise ValueError("piece is not convex")
        volume = ConvexHull(points).volume
        if not np.isclose(volume, declared_volume, rtol=1e-5):
            raise ValueError("piece volume differs from serialized geometry")
        volume_sum += volume
    if next(lines, None) is not None:
        raise ValueError("trailing data")
    return count, volume_sum


def content_meshes(root):
    """Select indexed destructibles without consulting game archives or MRM."""
    table = json.loads((root / "meshes/meshes.json").read_text())
    if not isinstance(table, dict):
        raise ValueError("meshes.json must contain an object")
    rules = []
    listing = Path(__file__).resolve().parents[2] / "OpenGothic/data/destructible-vobs.csv"
    with listing.open() as stream:
        for row in csv.reader(line for line in stream if not line.startswith("#")):
            if row and row[1] in ("crate", "barrel", "pot", "fence"):
                rules.append((row[0].upper(), row[1]))
    selected = {}
    for visual, entry in table.items():
        if not visual or "\0" in visual or "/" in visual or "\\" in visual or Path(visual).suffix:
            raise ValueError(f"invalid indexed visual: {visual!r}")
        visual = visual.upper()
        cls = next((cls for pattern, cls in rules if fnmatch.fnmatchcase(visual, pattern)), None)
        if cls is None:
            continue
        value = entry.get("glb") if isinstance(entry, dict) else None
        if not isinstance(value, str) or not value or "\0" in value:
            raise ValueError(f"{visual}: missing GLB path")
        relative = Path(value)
        if relative.is_absolute() or ".." in relative.parts or relative.suffix.lower() != ".glb":
            raise ValueError(f"{visual}: invalid relative GLB path")
        selected[visual] = (root / "meshes" / relative, cls)
    return sorted(selected.items())


def cut_content(args):
    rows = []
    for visual, (glb_path, cls) in content_meshes(args.cache):
        glb = glb_path.read_bytes()
        target = glb_path.with_suffix(".fracture")
        if args.verify:
            # Reject a stale sidecar before recutting or invoking the converter.
            validate(target.read_bytes(), glb)
        started = time.monotonic()
        with tempfile.TemporaryDirectory(prefix="gothic-fracture-") as directory:
            source_path = Path(directory) / "source.mesh"
            snapshot = Path(directory) / "source.glb"
            # The converter reads exactly the bytes identified by the source row.
            snapshot.write_bytes(glb)
            subprocess.run([str(args.extractor), str(snapshot), str(source_path)], check=True)
            source, textures = read_source(source_path)
        cutter = fracture_boards if cls == "fence" else fracture
        data, shell_volume, _ = cutter(source, textures, args.seed, args.pieces)
        data = bind_source(data, glb)
        count, actual_volume = validate(data, glb)
        ratio = actual_volume / shell_volume
        if not 0.9 <= ratio <= 1.1:
            raise ValueError(f"{visual}: serialized volume ratio {ratio}")
        if args.verify:
            if target.read_bytes() != data:
                raise ValueError(f"{target}: repeated fracture differs byte-for-byte")
        else:
            with tempfile.TemporaryDirectory(dir=target.parent, prefix=f".{target.name}.") as directory:
                temporary = Path(directory) / target.name
                temporary.write_bytes(data)
                temporary.replace(target)
        rows.append((visual, count))
        print(f"{visual}: {count} pieces, {time.monotonic() - started:.3f} s, "
              f"shell {shell_volume:.6f} cm3, pieces {actual_volume:.6f} cm3, "
              f"volume {ratio:.9f}, closed + convex" +
              (", identical bytes" if args.verify else ""), flush=True)
    if not rows:
        raise ValueError("no indexed destructible GLB meshes")
    print(f"Validated {len(rows)} visuals, {sum(row[1] for row in rows)} convex closed pieces.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cache", type=Path)
    parser.add_argument("--seed", type=int, default=601)
    parser.add_argument("--pieces", type=int, default=12, choices=range(8, 21))
    parser.add_argument("--verify", action="store_true", help="re-cut and compare bytes without overwriting")
    parser.add_argument("--content", action="store_true", help="cut indexed GLB files into adjacent version 2 sidecars")
    parser.add_argument("--extractor", type=Path, help="CPU GLB-to-source converter for --content")
    args = parser.parse_args()
    if args.content:
        if args.extractor is None:
            parser.error("--content requires --extractor")
        cut_content(args)
        return
    source_dir = args.cache / "source"
    classes = {}
    listing = source_dir / "visuals.csv"
    if listing.exists():
        with listing.open() as stream:
            classes = {row["visual"]: row["class"] for row in csv.DictReader(stream)}
    rows = []
    for path in sorted(source_dir.glob("*.mesh")):
        source, textures = read_source(path)
        started = time.monotonic()
        cutter = fracture_boards if classes.get(path.stem) == "fence" else fracture
        data, shell_volume, ratio = cutter(source, textures, args.seed, args.pieces)
        elapsed = time.monotonic() - started
        count, actual_volume = validate(data)
        ratio = actual_volume / shell_volume
        if not 0.9 <= ratio <= 1.1:
            raise ValueError(f"{path.stem}: serialized volume ratio {ratio}")
        target = args.cache / (path.stem + ".fracture")
        if args.verify:
            if target.read_bytes() != data:
                raise ValueError(f"{target}: repeated fracture differs byte-for-byte")
        else:
            temporary = target.with_suffix(".fracture.tmp")
            temporary.write_bytes(data)
            temporary.replace(target)
        digest = hashlib.sha256(data).hexdigest()
        rows.append([path.stem, count, f"{elapsed:.6f}", f"{shell_volume:.6f}", f"{ratio:.9f}", len(data), digest])
        print(f"{path.stem}: {count} pieces, {elapsed:.3f} s, volume {ratio:.9f}, closed + convex" +
              (", identical bytes" if args.verify else ""), flush=True)
    if not rows:
        raise ValueError(f"no source meshes in {source_dir}; run scripts/fracture.sh extract")
    if not args.verify:
        with (args.cache / "validation.csv").open("w") as stream:
            writer = csv.writer(stream)
            writer.writerow(["visual", "pieces", "seconds", "shell_volume_cm3", "volume_ratio", "bytes", "sha256"])
            writer.writerows(rows)
    print(f"Validated {len(rows)} visuals, {sum(row[1] for row in rows)} convex closed pieces.")


if __name__ == "__main__":
    main()
