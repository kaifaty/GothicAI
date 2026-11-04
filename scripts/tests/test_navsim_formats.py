"""Check packed engine formats against independently assembled byte streams."""
from pathlib import Path
import struct
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from navsim.formats import read_rays, read_static


def test_engine_snapshots_are_unaligned_little_endian(tmp_path):
    static = tmp_path / "static.bin"
    vertices = tuple(float(i * 100 - 500) for i in range(36))
    static.write_bytes(b"NAVSTAT1" + struct.pack("<II36f4B", 4, 0, *vertices, 1, 2, 3, 4))
    mesh = read_static(static)
    np.testing.assert_array_equal(mesh.vertices_cm.ravel(), vertices)
    np.testing.assert_array_equal(mesh.categories, [1, 2, 3, 4])

    rays = tmp_path / "rays.bin"
    payload = bytearray(b"NAVRAYS1" + struct.pack("<IIf", 238, 2, 1000))
    for update in range(2):
        payload += struct.pack("<Q3fB", 100 + update * 100, 101.25, -12.5, 2000, 1 - update)
        for index in range(238):
            payload += struct.pack("<5fBfBB", 1, 0, 0, 10000, index + .5, index % 6, -.75, 1, 1 - update)
    rays.write_bytes(payload)
    eye = read_rays(rays)
    assert rays.stat().st_size == 20 + 2 * (21 + 238 * 27)
    assert eye.near_range_cm == 1000
    np.testing.assert_array_equal(eye.updates["elapsed_ms"], [100, 200])
    np.testing.assert_array_equal(eye.updates["far_updated"], [1, 0])
    np.testing.assert_array_equal(eye.updates[1]["origin"], [101.25, -12.5, 2000])
    for update in range(2):
        ray = eye.updates[update]["rays"][237]
        np.testing.assert_array_equal(ray["direction"], [1, 0, 0])
        assert (ray["max_range_cm"], ray["distance_cm"], ray["kind"], ray["normal_y"],
                ray["hit"], ray["complete"]) == (10000, 237.5, 3, -.75, 1, 1 - update)
