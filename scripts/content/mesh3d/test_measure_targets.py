"""Offline synthetic checks: no game files, GPU, model or network required."""
import importlib.util
from pathlib import Path
import os
import subprocess

import pytest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("measure_targets", HERE / "measure_targets.py")
measure_targets = importlib.util.module_from_spec(spec)
spec.loader.exec_module(measure_targets)


def test_gltf_reflection_units_and_origin():
    raw = {
        "visual": "NW_HARBOUR_BARREL_01.MRM", "resolved_visual": "NW_HARBOUR_BARREL_01.MRM",
        "source_bbox_min_cm": [-40, -50, -30], "source_bbox_max_cm": [60, 150, 70],
        "source_tris": 12, "source_pose": "static", "newworld_instances": 7,
    }
    record = measure_targets.target_record(measure_targets.TARGETS[0], raw)
    assert record["bbox_min"] == [-0.6, -0.5, -0.3]
    assert record["bbox_max"] == [0.4, 1.5, 0.7]
    assert record["origin_in_bbox"] == [0.6, 0.25, 0.3]
    assert record["source_tris"] == 12
    assert record["budget_tris"] == 1500
    assert record["zones"][2]["group"] == "METAL"


@pytest.mark.parametrize("lo,hi", [([0, 0], [1, 1]), ([0, 0, 0], [1, 0, 1]),
                                      ([0, 0, 0], [-1, 1, 1]), ([0, 0, 0], [float("inf"), 1, 1]),
                                      ([0, float("nan"), 0], [1, 1, 1])])
def test_invalid_bounds(lo, hi):
    with pytest.raises(ValueError):
        measure_targets.gltf_bounds(lo, hi)


@pytest.mark.parametrize("tris", [0, -1, 1.5, True])
def test_invalid_triangle_count(tris):
    with pytest.raises(ValueError, match="source_tris"):
        measure_targets.target_record(measure_targets.TARGETS[0], {"visual": "NW_HARBOUR_BARREL_01.MRM", "source_tris": tris})


def test_scalar_cpp_mesh_count_and_mob_hierarchy(tmp_path):
    executable = tmp_path / "target-self-test"
    # Only checkout headers are needed; this mode does not link the ZenKit library.
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++20", "-O1", "-DZKNO_REM=1", "-DTARGET_MEASURES_SELF_TEST",
        f"-I{measure_targets.ROOT / 'OpenGothic/lib/ZenKit/include'}",
        str(HERE / "target-measures.cpp"), "-o", str(executable),
    ], check=True, capture_output=True, text=True)
    result = subprocess.run([str(executable), "--self-test"], check=True, capture_output=True, text=True)
    assert result.stdout.strip() == "target-measures: 7 synthetic checks passed"


def test_recorded_pilot_is_numeric_and_complete():
    import json
    records = json.loads((measure_targets.ROOT / "data/content/mesh3d-pilot.json").read_text())
    assert [r["id"] for r in records] == ["barrel", "crate", "table", "chest", "bench"]
    for record in records:
        assert all(isinstance(v, (int, float)) for field in ("bbox_min", "bbox_max", "origin_in_bbox") for v in record[field])
        assert record["source_tris"] > 0
        assert record["budget_tris"] <= (1500 if record["id"] in ("barrel", "crate") else 3000)
        assert {z["group"] for z in record["zones"]} == {"WOOD", "METAL"}
        assert record["newworld_instances"] >= 40
