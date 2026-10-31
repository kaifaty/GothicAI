"""Physical follow-speed diagnosis, including time-weighting and missing contact."""
import importlib.util
import json
from pathlib import Path

import numpy as np
import pytest


spec = importlib.util.spec_from_file_location("follow_analysis", Path(__file__).resolve().parents[1] / "nav_follow_analysis.py")
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)


def trace(path, distances=(900, 400, 600, 100)):
    (path / "episode.json").write_text(json.dumps(dict(teacher_version=3, start_not_found_decisions=2)))
    times = [0, 100, 200, 500]
    actions = np.zeros((4, 9))
    actions[:, 0] = times
    actions[:, 4] = 1
    actor = np.zeros((4, 5))
    actor[:, 0] = times
    actor[:, 1] = [0, 10, 20, 20]
    target = np.zeros((4, 6))
    target[:, 0] = times
    target[:, 1] = [0, 30, 60, 150]
    target[:, 4] = distances
    for name, values in (("actions.txt", actions), ("trajectory.txt", actor), ("target-trajectory.txt", target)):
        np.savetxt(path / name, values)


def test_partial_interval_far_neutral_uses_actual_speed_and_time(tmp_path):
    trace(tmp_path)
    result = analysis.analyze(tmp_path)
    assert result["observed_ms"] == 400
    assert result["bands"]["far"]["fraction"] == .75
    assert result["bands"]["far"]["actor_cm_s"] == 0
    assert result["bands"]["far"]["target_cm_s"] == 300
    assert result["conditions_ms"]["far_neutral"] == 300
    assert result["conditions_ms"]["far_commanded_but_slow"] == 0
    assert result["longest_excursions"][0]["duration_ms"] == 300


def test_no_contact_does_not_invent_postcontact_occupancy(tmp_path):
    trace(tmp_path, (900, 800, 700, 600))
    result = analysis.analyze(tmp_path)
    assert result["first_contact_ms"] is None
    assert result["observed_ms"] == 0
    assert analysis.summarize([result])["by_teacher"]["v3"]["band_fractions"]["band"] is None


def test_terminal_contact_has_no_observation_interval(tmp_path):
    trace(tmp_path, (900, 800, 700, 400))
    result = analysis.analyze(tmp_path)
    assert result["first_contact_ms"] == 500
    assert result["observed_ms"] == 0


def test_summary_weights_different_episode_durations(tmp_path):
    trace(tmp_path)
    result = analysis.analyze(tmp_path)
    small = dict(result, observed_ms=100, bands={name: dict(values, ms=100 if name == "band" else 0)
                                             for name, values in result["bands"].items()})
    assert analysis.summarize([result, small])["by_teacher"]["v3"]["band_fractions"]["band"] == .4


def test_rejects_unaligned_clocks(tmp_path):
    trace(tmp_path)
    values = np.loadtxt(tmp_path / "trajectory.txt")
    values[1, 0] += 1
    np.savetxt(tmp_path / "trajectory.txt", values)
    with pytest.raises(ValueError, match="Unaligned"):
        analysis.analyze(tmp_path)
