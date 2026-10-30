#!/usr/bin/env python3
"""Offline furniture collision; copyrighted geometry stays outside git.

V-HACD 4.1 single header: kmammou/v-hacd commit
f900e42361491f525262d4825e758845e4969897 (BSD-3-Clause, see VHACD.h).
The native reader never runs this decomposition.
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import time
import zlib

import numpy as np
from scipy.spatial import ConvexHull

from quality import max_coverage_error, mesh_volume, void_checks


def read_mesh(path):
    with path.open() as source:
        vertices, triangles = map(int, source.readline().split())
        points = np.asarray([list(map(float, source.readline().split())) for _ in range(vertices)])
        faces = np.asarray([list(map(int, source.readline().split())) for _ in range(triangles)])
    if points.shape != (vertices, 3) or faces.shape != (triangles, 3) or not np.isfinite(points).all():
        raise ValueError(f"{path}: invalid source mesh")
    # Wedges are separate for UV seams; geometric connectivity uses positions.
    welded, inverse = np.unique(points, axis=0, return_inverse=True)
    return welded, inverse[faces]


def read_parts(path, source):
    lines = iter(path.read_text().splitlines())
    if next(lines) != "GOTHIC_CONVEX_PARTS 1":
        raise ValueError(f"{path}: unsupported cache format")
    tag, size, crc = next(lines).split()
    if tag != "source" or int(size) != len(source) or int(crc, 16) != zlib.crc32(source):
        raise ValueError(f"{path}: stale source identity")
    tag, count = next(lines).split()
    if tag != "parts" or not 1 <= int(count) <= 16:
        raise ValueError(f"{path}: invalid part count")
    parts = []
    for _ in range(int(count)):
        tag, points = next(lines).split()
        if tag != "part" or not 4 <= int(points) <= 256:
            raise ValueError(f"{path}: invalid hull point count")
        part = np.asarray([list(map(float, next(lines).split())) for _ in range(int(points))])
        if part.shape != (int(points), 3) or not np.isfinite(part).all():
            raise ValueError(f"{path}: invalid hull points")
        parts.append(part)
    if next(lines, None) is not None:
        raise ValueError(f"{path}: trailing cache data")
    return parts


def write_parts(path, source, parts):
    text = f"GOTHIC_CONVEX_PARTS 1\nsource {len(source)} {zlib.crc32(source):08x}\nparts {len(parts)}\n"
    for part in parts:
        if not 4 <= len(part) <= 256:
            raise ValueError("hull must contain 4..256 points")
        text += f"part {len(part)}\n"
        text += "".join(" ".join(format(float(v), ".9g") for v in point) + "\n" for point in part)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(text)
    temporary.replace(path)


def decompose(points, triangles, mesh_file, output, executable):
    subprocess.run([str(executable), str(mesh_file), str(output)], check=True)
    lines = iter(output.read_text().splitlines())
    parts = []
    for _ in range(int(next(lines))):
        parts.append(np.asarray([list(map(float, next(lines).split())) for _ in range(int(next(lines)))]))
    if not 1 <= len(parts) <= 16:
        raise ValueError("V-HACD returned an invalid part count")
    # Shrink wrapping may round a source corner. Assign uncovered source vertices
    # to the closest output hull before rebuilding its convex envelope. This is
    # deterministic and makes vertex coverage exact; the volume/void gates below
    # still reject a repair that closes a required gap.
    planes = [ConvexHull(part).equations for part in parts]
    distances = np.asarray([np.maximum(0, np.max(points @ p[:, :3].T + p[:, 3], axis=1)) for p in planes])
    nearest = np.argmin(distances, axis=0)
    for i, part in enumerate(parts):
        outside = points[(nearest == i) & (distances.min(axis=0) > 1e-6)]
        if len(outside):
            part = np.concatenate((part, outside))
        part = np.unique(part.astype(np.float32).astype(np.float64), axis=0)
        hull = ConvexHull(part)
        parts[i] = part[hull.vertices]
    return parts


def visuals(args):
    rules = []
    for line in args.rules.read_text().splitlines():
        if line and not line.startswith("#"):
            pattern, cls, *_ = line.split(",")
            if cls != "fence":
                rules.append((pattern, cls))
    root = args.content if args.content else args.base
    index = json.loads((root / "meshes/meshes.json").read_text())
    table = index if args.content else index["meshes"]
    for visual, entry in sorted(table.items()):
        key = Path(visual).stem.upper()
        cls = next((cls for pattern, cls in rules if fnmatch.fnmatchcase(key, pattern)), None)
        if cls is None:
            continue
        relative = entry["glb"] if args.content else entry
        file = (root / "meshes" / relative).resolve()
        if not file.is_relative_to((root / "meshes").resolve()) or file.suffix.lower() != ".glb":
            raise ValueError(f"invalid indexed GLB path: {relative!r}")
        cache = file.with_suffix(".convex-parts") if args.content else args.cache / (key + ".convex-parts")
        yield key, cls, file, cache


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--base", type=Path, required=True)
    parser.add_argument("--content", type=Path)
    parser.add_argument("--rules", type=Path, required=True)
    parser.add_argument("--extractor", type=Path, required=True)
    parser.add_argument("--decomposer", type=Path, required=True)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    args.cache.mkdir(parents=True, exist_ok=True)
    rows = []
    for visual, cls, source, cache in visuals(args):
        start = time.monotonic()
        glb = source.read_bytes()
        with tempfile.TemporaryDirectory(prefix="convex-parts-", dir=args.cache) as temporary:
            mesh = Path(temporary) / "source.mesh"
            subprocess.run([str(args.extractor), "content" if args.content else "base", str(source), str(mesh)], check=True)
            points, triangles = read_mesh(mesh)
            topology = mesh_volume(points, triangles)
            volume = topology["volume_cm3"]
            hull = ConvexHull(points)
            if not topology["watertight"]:
                # These stock surfaces have no defined enclosed volume. Do not
                # guess a hole-filling rule or publish an uncertified cache.
                row = dict(visual=visual, cls=cls, parts=0, decision="rejected_open_mesh", passed=False,
                           reason="Source mesh is open or nonmanifold; enclosed volume needs a decision",
                           mesh_volume_cm3=None, surface_volume_estimate_cm3=volume,
                           hull_volume_cm3=hull.volume, topology=topology,
                           source_size=len(glb), source_crc32=f"{zlib.crc32(glb):08x}",
                           cache=str(cache.resolve()), elapsed_s=time.monotonic() - start)
                rows.append(row)
                print(json.dumps(row, sort_keys=True), flush=True)
                continue
            if not volume > 0:
                raise ValueError(f"{visual}: source mesh has no measurable volume")
            excess = hull.volume / volume - 1
            if args.verify:
                parts = read_parts(cache, glb)
            elif excess < .15:
                parts = [points[hull.vertices]]
            else:
                parts = decompose(points, triangles, mesh, Path(temporary) / "parts.mesh", args.decomposer)
            candidate = cache if args.verify else Path(temporary) / "candidate.convex-parts"
            if not args.verify:
                write_parts(candidate, glb, parts)
            # Validate precisely the float coordinates that will be loaded.
            parts = read_parts(candidate, glb)
            parts_volume = sum(ConvexHull(part).volume for part in parts)
            coverage = max_coverage_error(points, parts)
            voids = void_checks(points, triangles, parts, cls) if len(parts) > 1 else []
            passed = (len(parts) <= 16 and coverage <= 2 and
                      (len(parts) == 1 or parts_volume / volume <= 1.25) and
                      all(v["passed"] for v in voids) and
                      (len(parts) == 1 or cls not in ("table", "cupboard") or bool(voids)) and
                      (excess < .15 or len(parts) > 1))
            if passed and not args.verify:
                write_parts(cache, glb, parts)
            row = dict(visual=visual, cls=cls, parts=len(parts), mesh_volume_cm3=volume,
                       hull_volume_cm3=hull.volume, parts_volume_cm3=parts_volume,
                       hull_excess_percent=excess * 100, parts_excess_percent=(parts_volume / volume - 1) * 100,
                       coverage_error_cm=coverage, void_spheres=voids, topology=topology,
                       decision="compound" if len(parts) > 1 else "one_hull", passed=passed,
                       source_size=len(glb), source_crc32=f"{zlib.crc32(glb):08x}", cache=str(cache.resolve()),
                       cache_sha256=hashlib.sha256(candidate.read_bytes()).hexdigest(), elapsed_s=time.monotonic() - start)
            rows.append(row)
            print(json.dumps(row, sort_keys=True), flush=True)
    if not rows:
        raise ValueError("no indexed movable GLB meshes matched")
    report = args.cache / ("content-report.json" if args.content else "report.json")
    report.write_text(json.dumps(dict(algorithm="V-HACD 4.1", deterministic_async=False,
                                    parameters=dict(max_hulls=16, resolution=2000000, error_percent=.1, depth=12),
                                    visuals=rows, passed=all(row["passed"] for row in rows)), indent=2) + "\n")
    return 0 if all(row["passed"] for row in rows) else 1


if __name__ == "__main__":
    raise SystemExit(main())
