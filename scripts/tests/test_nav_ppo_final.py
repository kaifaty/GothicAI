"""Final mode selection must precede test access and pool every fixed seed."""
import gzip
from pathlib import Path
import sys

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import nav_demos
import nav_ppo_final as final


def metrics(arrived, turn=12):
    return dict(all=dict(n=60, arrived=arrived, decisions=600, teacher_control_decisions=0,
                         action_counts=dict(turn_only=turn), blocked_ms=3000),
                episodes=[dict(region=region, elapsed_ms=2000) for region in sorted(final.REGIONS) for _ in range(10)])


def test_modes_pool_three_seeds_and_never_use_test_results(tmp_path):
    for policy, mode, seed in final.configurations():
        # Round2's luckiest seed wins all routes, but pooled sampling loses.
        arrived = (60 if seed == final.SEEDS[0] else 30) if policy == "round2" else 44
        if mode == "argmax":
            arrived = 42
        path = final.measurement_path(tmp_path, "validation", policy, mode, seed)
        final.write(path / "metrics.json", metrics(arrived))
        # Test outcomes are deliberately contradictory and must be ignored.
        path = final.measurement_path(tmp_path, "test", policy, mode, seed)
        final.write(path / "metrics.json", metrics(0 if mode == "sample" else 60))
    selected = final.select_modes(tmp_path)
    assert selected["by_policy"]["round2"]["selected_mode"] == "argmax"
    assert selected["by_policy"]["round2"]["metrics"]["sample"]["arrived"] == 120
    assert selected["by_policy"]["iter35"]["selected_mode"] == selected["game_action_mode"] == "sample"
    assert selected["selection_source"] == "validation_only"
    assert selected["by_policy"]["iter35"]["metrics"]["sample"]["n"] == 180


def test_final_metrics_reject_incomplete_region_even_at_sixty_episodes(tmp_path):
    value = metrics(42)
    value["episodes"][0]["region"] = "unknown"
    final.write(tmp_path / "metrics.json", value)
    with pytest.raises(ValueError, match="regional"):
        final.checked_metrics(tmp_path)


def test_retained_tail_preserves_exact_sensor_bytes_and_absolute_timestamps(tmp_path):
    records = np.zeros(4, dtype=nav_demos.RECORD_V2_DTYPE)
    records["time_ms"] = [12345, 12445, 12545, 12645]
    records["body"]["head_yaw_deg"] = [10, 20, 30, 40]
    header = nav_demos.HEADER.pack(nav_demos.MAGIC, 2, nav_demos.RECORD_BYTES,
             nav_demos.RAY_COUNT, nav_demos.NEAR_SIZE, nav_demos.FAR_SIZE, 100)
    source = tmp_path / "samples.bin.gz"
    with gzip.open(source, "wb") as stream:
        stream.write(header)
        stream.write(records.tobytes())
    final.write(tmp_path / "episode.json", dict(decisions=4, elapsed_ms=400))
    (tmp_path / "actions.txt").write_text("all actions remain")
    final.retain_tail(tmp_path, tail_ms=200)
    assert not source.exists()
    with gzip.open(tmp_path / "tail-10s.bin.gz", "rb") as stream:
        assert stream.read() == header + records[2:].tobytes()
    assert (tmp_path / "actions.txt").read_text() == "all actions remain"
    assert final.read(tmp_path / "retained-sensors.json")["tail_decisions"] == 2
    final.retain_tail(tmp_path, tail_ms=200)  # Restart preserves the completed tail.
