"""Read the reference GBuffer from the archived fir-bough-depth A/B pair."""

import csv
from pathlib import Path

import numpy as np


def load_capture(directory: Path, scene: str):
    """Return archive counters and depth planes after checking paired owner/depth parity."""
    with (directory / f"{scene}.csv").open() as stream:
        row, = csv.DictReader(stream)
    counters = {
        key: float(value) if key.startswith("cam_") else int(value)
        for key, value in row.items()
    }
    width, height = counters["width"], counters["height"]
    if (width, height) != (1280, 720) or counters["pixels"] != width * height:
        raise ValueError(f"{scene}: unexpected capture dimensions")

    # The archived planes.py and stand.patch specify six planar, little-endian
    # attachments in this order: owner, depth, diffuse, normal, ORM, early depth.
    a, b = (np.fromfile(directory / f"{scene}-{variant}.u32", dtype="<u4").reshape(6, height, width)
            for variant in ("A", "B"))
    if not np.array_equal(a[:2], b[:2]):
        raise ValueError(f"{scene}: archived pair has different owner IDs or final depth")
    final, early = a[1].view("<f4"), a[5].view("<f4")
    if not (final <= early).all():
        raise ValueError(f"{scene}: early depth is closer than final depth")
    return counters, final, early


def load_membership(analysis_dir: Path, scene: str):
    """Return the earlier offline replay's candidate crowns and their cut totals."""
    with (analysis_dir / f"{scene}-candidates.csv").open() as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != 1024 or len({row["key"] for row in rows}) != len(rows):
        raise ValueError(f"{scene}: expected one row per spruce in the fixture")

    # These are the archived final-depth replay's tests for frustum, impostor
    # transition and whole-crown rejection from the original early depth.
    return {
        row["key"]: {
            "compact": bool(int(row["compact"])),
            "a_tris": int(row["a_tris"]),
            "a_nodes": int(row["a_nodes"]),
        }
        for row in rows
        if float(row["frustum_distance"]) > -float(row["dynamic_radius"])
        and float(row["height_px"]) > 46 * 80 / 49
        and int(row["whole_reject"]) == 0
    }
