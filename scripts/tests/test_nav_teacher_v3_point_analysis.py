"""Asset-free checks for point-failure distances and observation evidence."""

from copy import deepcopy
import importlib.util
from pathlib import Path
import sys

import numpy as np
import pytest


SPEC = importlib.util.spec_from_file_location(
    "nav_teacher_v3_point_analysis", Path(__file__).resolve().parents[1] / "nav_teacher_v3_point_analysis.py")
analysis = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = analysis
SPEC.loader.exec_module(analysis)


def trajectory(points):
    """Input XY pairs denote world X/Z metres, encoded in the runner's cm."""
    result = np.zeros((len(points), 5), dtype=np.float64)
    result[:, 0] = np.arange(len(points)) * 100
    result[:, 1] = np.asarray(points)[:, 0] * 100
    result[:, 3] = np.asarray(points)[:, 1] * 100
    return result


def test_distances_keep_travelled_path_separate_from_physical_endpoint_distance():
    poses = trajectory([(0, 0), (4, 0), (4, 3), (0, 3), (0, 0), (2, 0)])
    result = analysis.distance_summary(poses, np.array([False, True, True, True, True]),
                                       endpoint=[400, 0, 0], onset_index=3)
    assert result["last_handoff_index"] == 1
    assert result["after_handoff_path_m"] == 12
    assert result["onset_after_handoff_path_m"] == 7
    assert result["terminal_endpoint_distance_m"] == 2
    assert result["onset_endpoint_distance_m"] == 5
    assert result["endpoint_pass_index"] == 1
    assert result["after_endpoint_pass_path_m"] == 12
    assert result["onset_after_endpoint_pass_path_m"] == 7
    assert result["closest_endpoint_distance_m"] == 0


def test_never_observed_endpoint_pass_does_not_invent_path_beyond_the_node():
    poses = trajectory([(0, 0), (3, 0), (3, 3), (0, 3), (0, 0)])
    result = analysis.distance_summary(poses, np.array([False, True, True, True]),
                                       endpoint=[200, 0, 200], onset_index=2)
    assert result["after_handoff_path_m"] == 9
    assert result["terminal_endpoint_distance_m"] == pytest.approx(np.sqrt(8))
    assert result["onset_endpoint_distance_m"] == pytest.approx(np.sqrt(2))
    assert result["closest_endpoint_distance_m"] == pytest.approx(np.sqrt(2))
    assert result["endpoint_pass_index"] is None
    assert result["after_endpoint_pass_path_m"] is None
    assert result["onset_after_endpoint_pass_path_m"] is None


def test_final_held_interval_can_first_reach_endpoint_after_stuck_onset():
    poses = trajectory([(0, 0), (2, 0), (4, 0)])
    result = analysis.distance_summary(poses, np.array([True, True]), endpoint=[400, 0, 0], onset_index=0)
    assert result["last_handoff_index"] == 0
    assert result["after_handoff_path_m"] == 4
    assert result["endpoint_pass_index"] == 2
    assert result["after_endpoint_pass_path_m"] == 0
    assert result["onset_after_endpoint_pass_path_m"] is None
    assert result["onset_endpoint_distance_m"] == 4
    assert result["terminal_endpoint_distance_m"] == 0


def test_endpoint_pass_includes_the_one_metre_boundary():
    poses = trajectory([(0, 0), (2, 0), (4, 0)])
    result = analysis.distance_summary(poses, np.array([True, True]), endpoint=[300, 0, 0], onset_index=1)
    assert result["endpoint_pass_index"] == 1
    assert result["after_endpoint_pass_path_m"] == 2
    assert result["onset_after_endpoint_pass_path_m"] == 0


def test_last_handoff_is_latest_transition_and_does_not_create_negative_onset_path():
    poses = trajectory([(0, 0), (1, 0), (2, 0), (3, 0), (4, 0)])
    result = analysis.distance_summary(poses, np.array([False, True, False, True]), endpoint=None, onset_index=1)
    assert result["last_handoff_index"] == 3
    assert result["after_handoff_path_m"] == 1
    assert result["onset_after_handoff_path_m"] is None
    for field in ("terminal_endpoint_distance_m", "onset_endpoint_distance_m", "endpoint_pass_index",
                  "after_endpoint_pass_path_m", "onset_after_endpoint_pass_path_m", "closest_endpoint_distance_m"):
        assert result[field] is None


def test_no_beyond_known_segment_has_no_handoff_distance():
    poses = trajectory([(0, 0), (2, 0), (3, 0)])
    result = analysis.distance_summary(poses, np.array([False, False]), endpoint=None, onset_index=1)
    assert result["last_handoff_index"] is None
    assert result["after_handoff_path_m"] is None
    assert result["onset_after_handoff_path_m"] is None


def test_segment_summary_includes_final_held_interval_and_keeps_separate_runs():
    poses = trajectory([(0, 0), (3, 0), (3, 4), (8, 4), (8, 10)])
    result = analysis.segment_summary(poses, np.array([True, False, True, True]))
    assert result["total_path_m"] == 18
    assert result["beyond_path_m"] == 14
    assert result["beyond_path_fraction"] == pytest.approx(14 / 18)
    runs = result["beyond_segments"]
    assert [(run["start_index"], run["end_index"], run["path_m"]) for run in runs] == [(0, 1, 3), (2, 4, 11)]


def test_path_lengths_use_world_xz_and_do_not_count_vertical_motion_or_yaw():
    poses = trajectory([(0, 0), (3, 4), (3, 4)])
    poses[:, 2] = [0, 5000, -9000]
    poses[:, 4] = [170, -170, 800]
    result = analysis.segment_summary(poses, np.array([True, True]))
    assert result["total_path_m"] == result["beyond_path_m"] == 5
    assert result["beyond_path_fraction"] == 1


def frame(near=False, frontier=False, role="tail", evaluable=True):
    return dict(near_blocking=near, frontier_supported=frontier, role=role, evaluable=evaluable)


@pytest.mark.parametrize(("frames", "classification", "near_count", "frontier_count"), [
    ([frame(near=True), frame(near=True), frame(), frame()], "near_visible_obstacle", 2, 0),
    ([frame(frontier=True), frame(frontier=True), frame(), frame()], "memory_frontier", 0, 2),
    ([frame(near=True, frontier=True), frame(near=True, frontier=True)], "mixed", 2, 2),
    ([frame(near=True), frame(near=True), frame(frontier=True), frame(frontier=True)], "mixed", 2, 2),
    ([frame(near=True, role="onset"), frame(near=True), frame(frontier=True)], "near_visible_obstacle", 2, 1),
    ([frame(frontier=True, role="onset"), frame(frontier=True), frame(near=True)], "memory_frontier", 1, 2),
    ([frame(near=True)], "undetermined", 1, 0),
    ([frame(frontier=True), frame(), frame()], "undetermined", 0, 1),
    ([frame(), frame()], "undetermined", 0, 0),
])
def test_classification_requires_two_frames_and_half_the_evaluable_stuck_frames(
        frames, classification, near_count, frontier_count):
    original = deepcopy(frames)
    result = analysis.classify_evidence(frames)
    assert result["classification"] == classification
    assert result["frames"] == len(frames)
    assert result["near_visible_frames"] == near_count
    assert result["frontier_frames"] == frontier_count
    assert result["near_fraction"] == pytest.approx(near_count / len(frames))
    assert result["frontier_fraction"] == pytest.approx(frontier_count / len(frames))
    assert frames == original


def test_classification_excludes_handoff_and_unevaluable_snapshots_from_both_counts():
    frames = [frame(frontier=True, role="handoff"), frame(frontier=True, role="handoff"),
              frame(near=True, frontier=True, evaluable=False), frame(role="onset"), frame()]
    result = analysis.classify_evidence(frames)
    assert result["classification"] == "undetermined"
    assert result["frames"] == 2
    assert result["near_visible_frames"] == result["frontier_frames"] == 0
    assert result["near_fraction"] == result["frontier_fraction"] == 0


def test_classification_does_not_treat_unknown_memory_as_frontier_evidence():
    result = analysis.classify_evidence([frame(evaluable=False), frame(evaluable=False)])
    assert result["classification"] == "undetermined"
    assert result["frames"] == result["near_visible_frames"] == result["frontier_frames"] == 0


def snapshot():
    record = np.zeros((), dtype=analysis.demos.RECORD_V2_DTYPE)
    record["roads"]["goal_direction"] = [0, 0, 1]
    record["roads"]["goal_distance_cm"] = 500
    record["roads"]["carrot_direction"] = [0, 0, 1]
    record["roads"]["carrot_distance_cm"] = 500
    record["roads"]["beyond_known"] = 1
    return record


def test_snapshot_unknown_goal_and_connected_optimistic_path_support_frontier():
    record = snapshot()
    local_plan = np.array([[0, 0], [0, 500]])
    diagnostic = dict(actual_target_reached=True, plan_complete=True, target_clamped=False)
    original = record.copy()
    original_plan = local_plan.copy()
    original_diagnostic = deepcopy(diagnostic)
    result = analysis.snapshot_evidence(record, local_plan=local_plan, diagnostic=diagnostic)
    assert result["evaluable"] is True
    assert result["beyond_known"] is True
    assert result["goal_currently_unknown"] is True
    assert result["goal_confidence"] == 0
    assert result["plan_unknown_fraction"] == 1
    assert result["optimistic_goal_or_proxy_connected"] is True
    assert result["frontier_supported"] is True
    assert result["near_blocking"] is False
    np.testing.assert_array_equal(record, original)
    np.testing.assert_array_equal(local_plan, original_plan)
    assert diagnostic == original_diagnostic


def test_snapshot_known_free_goal_is_not_frontier_even_when_most_plan_is_unknown():
    record = snapshot()
    record["memory_near"]["channels"][52, 32, :2] = 255
    result = analysis.snapshot_evidence(record, local_plan=[[0, 0], [0, 500]],
                                        diagnostic=dict(actual_target_reached=True))
    assert result["goal_currently_unknown"] is False
    assert result["goal_confidence"] == 255
    assert result["plan_unknown_fraction"] >= .5
    assert result["optimistic_goal_or_proxy_connected"] is True
    assert result["frontier_supported"] is False


def test_snapshot_remote_near_hazard_and_unknown_connected_goal_are_distinct_mixed_evidence():
    record = snapshot()
    record["memory_near"]["channels"][40, 32, 0] = 255
    record["memory_near"]["channels"][40, 32, 2] = 255
    original = record.copy()
    evidence = analysis.snapshot_evidence(record, local_plan=[[0, 0], [0, 500]],
                                          diagnostic=dict(actual_target_reached=True))
    assert evidence["near_blocking"] is True
    assert evidence["direct_near_corridor"]["occupied_cells"] == 1
    assert evidence["planned_near_corridor"]["occupied_cells"] == 1
    assert evidence["goal_currently_unknown"] is True
    assert evidence["frontier_supported"] is True
    result = analysis.classify_evidence([dict(evidence, role="onset"), dict(evidence, role="tail")])
    assert result["classification"] == "mixed"
    assert result["near_visible_frames"] == result["frontier_frames"] == 2
    np.testing.assert_array_equal(record, original)


def test_sparse_v2_trace_reconstructs_every_beyond_label_without_assuming_per_decision_rows(tmp_path):
    import json
    value = dict(start="A", goal="B")
    metadata = dict(teacher_version=2, success=True, start="A", goal="B", decisions=4,
                    elapsed_ms=400, target_position=[400, 0, 0], detour_length_cm=400)
    (tmp_path / "episode.json").write_text(json.dumps(metadata))
    trajectory = np.zeros((5, 5))
    trajectory[:, 0] = np.arange(5)*100
    trajectory[:, 1] = np.arange(5)*100
    np.savetxt(tmp_path / "trajectory.txt", trajectory)
    sparse = [dict(elapsed_ms=0, beyond_known=False, remaining_known_cm=800),
              dict(elapsed_ms=200, beyond_known=True, remaining_known_cm=600)]
    (tmp_path / "teacher-paths.jsonl").write_text("".join(json.dumps(row)+"\n" for row in sparse))
    result = analysis.reference_stats(tmp_path, value)
    assert result["total_path_m"] == 4
    assert result["beyond_path_m"] == 2
    assert result["beyond_segments"] == [dict(start_index=2, end_index=4, path_m=2)]
    assert result["goal_distance_at_last_handoff_m"] == 2
    assert result["remaining_known_at_last_handoff_m"] == 6


def test_local_start_failure_is_preserved_and_never_becomes_pure_frontier():
    record = np.zeros((), dtype=analysis.demos.RECORD_V2_DTYPE)
    record["roads"]["goal_direction"] = [0, 0, 1]
    record["roads"]["goal_distance_cm"] = 2000
    record["roads"]["beyond_known"] = 1
    diagnostic = dict(actual_target_reached=True, start_found=False, start_escape_required=True, overlap_danger_cells=1)
    evidence = analysis.snapshot_evidence(record, [[0, 0], [0, 2000]], diagnostic)
    assert evidence["local_start_problem"] is True
    assert evidence["start_found"] is False and evidence["start_escape_required"] is True
    assert evidence["overlap_danger_cells"] == 1
    assert evidence["frontier_supported"] is False
    frames = [dict(role="tail", frontier_supported=True), dict(role="tail", frontier_supported=True),
              dict(role="onset", **evidence)]
    assert analysis.classify_evidence(frames)["classification"] == "mixed"
