"""Read NAVSTAT1/NAVRAYS1 engine snapshots (little endian, cm, Y up).

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
