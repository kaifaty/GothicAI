"""Read engine snapshots and nav-sim episode packages (little endian, Y up).

NAVSTAT1/NAVRAYS1 use centimetres; OGNAV001 tiles use metres.

Arrays are read-only memory maps. Ray records follow NavigationEye.samples();
near-only misses are incomplete, not observations of the uncast far segment.
"""
from pathlib import Path
import struct
from typing import NamedTuple

import numpy as np


STATIC_HEADER = struct.Struct("<8sII")
RAYS_HEADER = struct.Struct("<8sIIf")
RAY_DTYPE = np.dtype([
    ("direction", "<f4", (3,)), ("max_range_cm", "<f4"),
    ("distance_cm", "<f4"), ("kind", "u1"), ("normal_y", "<f4"),
    ("hit", "u1"), ("complete", "u1"),
], align=False)
KINDS = ("none", "world", "static_vob", "movable_vob", "npc", "water")
CATEGORIES = ("landscape", "static_object", "water", "kinematic_object")


class Static(NamedTuple):
    vertices_cm: np.ndarray  # (triangles, vertices=3, xyz=3)
    categories: np.ndarray  # 1..4, see CATEGORIES


class Rays(NamedTuple):
    near_range_cm: float
    updates: np.ndarray  # elapsed_ms, origin, far_updated, rays


def read_static(path: str | Path) -> Static:
    path = Path(path)
    with path.open("rb") as stream:
        magic, count, reserved = STATIC_HEADER.unpack(stream.read(STATIC_HEADER.size))
    if magic != b"NAVSTAT1" or reserved != 0:
        raise ValueError("Invalid NAVSTAT1 header")
    if path.stat().st_size != STATIC_HEADER.size + count * 37:
        raise ValueError("Invalid NAVSTAT1 file size")
    if not count:
        return Static(np.empty((0, 3, 3), dtype="<f4"), np.empty(0, dtype="u1"))
    return Static(
        np.memmap(path, mode="r", dtype="<f4", offset=STATIC_HEADER.size, shape=(count, 3, 3)),
        np.memmap(path, mode="r", dtype="u1", offset=STATIC_HEADER.size + count * 36, shape=(count,)),
    )


def read_rays(path: str | Path) -> Rays:
    path = Path(path)
    with path.open("rb") as stream:
        magic, count, updates, near_range = RAYS_HEADER.unpack(stream.read(RAYS_HEADER.size))
    if magic != b"NAVRAYS1" or count != 238:
        raise ValueError("Invalid NAVRAYS1 header (expected layout 250 / 238 rays)")
    dtype = np.dtype([
        ("elapsed_ms", "<u8"), ("origin", "<f4", (3,)), ("far_updated", "u1"),
        ("rays", RAY_DTYPE, (count,)),
    ], align=False)
    if path.stat().st_size != RAYS_HEADER.size + updates * dtype.itemsize:
        raise ValueError("Invalid NAVRAYS1 file size")
    data = (np.memmap(path, mode="r", dtype=dtype, offset=RAYS_HEADER.size, shape=(updates,))
            if updates else np.empty(0, dtype=dtype))
    return Rays(near_range, data)


class NavMeshTile(NamedTuple):
    reference: int
    data: np.ndarray  # packed Detour tile, read-only bytes


class NavMeshCache(NamedTuple):
    key: int
    build_seconds: float
    origin_m: tuple[float, float, float]
    tile_width_m: float
    tile_height_m: float
    max_tiles: int
    max_polygons: int
    tiles: tuple[NavMeshTile, ...]


class Package(NamedTuple):
    directory: Path
    manifest: dict
    navmesh: NavMeshCache
    navmesh_params: dict
    links: list[dict]
    roads: dict
    actors: list[dict]
    places: dict
    pairs: dict[str, list[dict]]
    path_checks: list[dict]


def read_navmesh(path: str | Path) -> NavMeshCache:
    """Read the engine's OGNAV001 v2 cache with 64-bit Detour references.

    Tile bytes retain Detour's metre coordinates, polygon areas and flags.
    ``NavMesh::loadCache`` consumes the same file in the stand's native process.
    """
    path = Path(path)
    header = struct.Struct("<8sIIQd3f2f2iQ")
    tile_header = struct.Struct("<QIQ")
    with path.open("rb") as stream:
        magic, version, endian, key, seconds, x, y, z, width, height, max_tiles, max_polys, count = header.unpack(
            stream.read(header.size))
    if magic != b"OGNAV001" or version != 2 or endian != 0x01020304:
        raise ValueError("Invalid OGNAV001 v2 header")
    if not 0 <= count <= max_tiles or max_polys <= 0:
        raise ValueError("Invalid OGNAV001 tile counts")
    data = np.memmap(path, mode="r", dtype="u1")
    offset = header.size
    tiles = []
    for _ in range(count):
        if offset + tile_header.size > len(data):
            raise ValueError("Truncated OGNAV001 tile header")
        reference, size, _checksum = tile_header.unpack_from(data, offset)
        offset += tile_header.size
        if size == 0 or offset + size > len(data):
            raise ValueError("Invalid OGNAV001 tile size")
        tiles.append(NavMeshTile(reference, data[offset:offset + size]))
        offset += size
    if offset != len(data):
        raise ValueError("Invalid OGNAV001 file size")
    return NavMeshCache(key, seconds, (x, y, z), width, height, max_tiles, max_polys, tuple(tiles))


def read_package(directory: str | Path) -> Package:
    """Verify a package's file sizes/SHA-256 and read its episode inputs.

    Pair rows and place/road identifiers are retained exactly as exported;
    actor knowledge is the engine's node/edge ID lists, built at ``home_cm``.
    The native cache loader additionally verifies each Detour tile checksum.
    """
    import hashlib
    import json

    directory = Path(directory)
    manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
    if manifest["format"] != "navsim-package-v1":
        raise ValueError("Unsupported nav-sim package format")
    for entry in manifest["files"]:
        path = directory / entry["path"]
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        if path.stat().st_size != entry["size"] or digest != entry["sha256"]:
            raise ValueError(f"Package size/SHA-256 mismatch: {entry['path']}")

    def document(name):
        return json.loads((directory / name).read_text(encoding="utf-8"))

    def rows(name):
        return [json.loads(line) for line in (directory / name).read_text(encoding="utf-8").splitlines()
                if line.strip()]

    return Package(directory, manifest, read_navmesh(directory / "navmesh.navmesh"),
                   document("navmesh-params.json"), document("links.json"), document("roads-graph.json"),
                   rows("actors.jsonl"), document("places.json"),
                   {split: rows(f"{split}.jsonl") for split in ("train", "validation", "test")},
                   rows("path-checks.jsonl"))
