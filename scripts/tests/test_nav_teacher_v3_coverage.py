"""Native-cell and original-input checks for evaluator-only memory coverage."""
import gzip
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest

SPEC = importlib.util.spec_from_file_location("nav_teacher_v3_coverage", Path(__file__).resolve().parents[1] / "nav_teacher_v3_coverage.py")
coverage = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(coverage)


def record():
    return np.zeros(1, dtype=coverage.demos.RECORD_V2_DTYPE)[0]


def cell(layer="near", row=32, column=32, passable=True, height=100):
    return dict(layer=layer, row=row, column=column, truth_passable=passable,
                ground_height_cm=height, height_valid=height is not None)


def frame():
    return dict(label="point-test-frame0", index=0, elapsed_ms=0, position_cm=[100, 200, 300], goal_cm=[1000, 400, 300], yaw_deg=90)


def truth(query=None):
    query = query or frame()
    return dict(label=query["label"], start_cm=query["position_cm"], goal_cm=query["goal_cm"], yaw_deg=query["yaw_deg"],
                complete=True, length_cm=1000, corridor_begin_cm=500, corridor_end_cm=1500, radius_cm=15,
                corridor_available=True, native_cells=[cell()], corridor_status="ok", raster_evaluated=True, raster_support_valid=True, transition_count=0)


@pytest.mark.parametrize("yaw", [0, 90, 180, -90, 47])
def test_current_goal_reconstruction_preserves_body_rotation_and_height(yaw):
    value = record()
    value["position_cm"] = [100, 200, 300]
    value["body_yaw_deg"] = yaw
    # A full 3D vector, matching NavDemos::direction normalization.
    delta = np.array([900., 200., -300.])
    angle = np.deg2rad(yaw)
    local = np.array([-delta[0] * np.sin(angle) + delta[2] * np.cos(angle), delta[1],
                      delta[0] * np.cos(angle) + delta[2] * np.sin(angle)])
    value["roads"]["goal_distance_cm"] = np.linalg.norm(delta)
    value["roads"]["goal_direction"] = local / np.linalg.norm(delta)
    np.testing.assert_allclose(coverage.goal_from_record(value), value["position_cm"] + delta, atol=.001)


def test_seeded_one_hz_phase_is_reproducible_and_always_includes_last_decision():
    first = coverage.sample_indices(101, "case", 42)
    assert first == coverage.sample_indices(101, "case", 42)
    assert first[-1] == 100
    assert all(b - a == 10 for a, b in zip(first[:-2], first[1:-1]))
    assert coverage.sample_indices(1, "case", 42) == [0]
    with pytest.raises(ValueError, match="empty"):
        coverage.sample_indices(0, "case", 42)


def test_categories_prioritize_any_nonzero_hazard_and_expose_unknown_traversability():
    value = record()
    value["memory_near"]["channels"][32, 32] = [255, 255, 1, 0, 0, 0, 0]
    value["memory_near"]["channels"][32, 33] = [0, 255, 0, 0, 0, 0, 0]
    value["memory_near"]["channels"][32, 34] = [10, 0, 0, 0, 0, 0, 0]
    value["memory_near"]["channels"][32, 35] = [1, 1, 0, 0, 0, 0, 0]
    counts, layers = coverage.classify_cells(value, [cell(column=c) for c in range(32, 36)])
    assert (counts["danger"], counts["unknown"], counts["free"], counts["cells"]) == (1, 2, 1, 4)
    assert counts["unknown_zero_confidence"] == counts["unknown_positive_confidence_zero_free"] == 1
    assert counts["danger_wall"] == 1
    assert layers["far"] == {}
    result = coverage.fractions(counts)
    assert result["fractions"] == dict(free=.25, unknown=.5, danger=.25)
    assert sum(result["truth_passable_fractions"].values()) == 1


def test_false_danger_has_distinct_all_cells_truth_passable_and_memory_danger_denominators():
    value = record()
    value["memory_near"]["channels"][32, 32, 2] = 1
    value["memory_near"]["channels"][32, 33, 2] = 1
    value["memory_near"]["channels"][32, 34, :2] = 255
    counts, _ = coverage.classify_cells(value, [cell(column=32), cell(column=33, passable=False), cell(column=34)])
    result = coverage.fractions(counts)
    assert result["false_danger_fraction_all_cells"] == 1 / 3
    assert result["false_danger_fraction_truth_passable"] == .5
    assert result["false_danger_fraction_memory_danger"] == .5
    assert result["truth_passable_fractions"] == dict(free=.5, unknown=0, danger=.5)


def test_height_mismatch_is_separate_and_does_not_hide_teacher_visible_false_hazard():
    value = record()
    memory = value["memory_near"][32, 32]
    memory["channels"][2] = 1
    memory["height_valid"] = 1
    memory["ground_height_cm"] = 350
    counts, _ = coverage.classify_cells(value, [cell(height=100)])
    assert counts["height_comparable"] == 1
    assert counts["height_mismatch_gt100"] == counts["false_danger_height_mismatch_gt100"] == counts["false_danger"] == 1
    value["memory_near"][32, 32]["height_valid"] = 0
    assert coverage.classify_cells(value, [cell(height=100)])[0]["height_comparable"] == 0


def test_duplicate_or_overlapping_native_cells_are_rejected():
    value = record()
    with pytest.raises(ValueError, match="Duplicate"):
        coverage.classify_cells(value, [cell(), cell()])
    with pytest.raises(ValueError, match="duplicates the near"):
        coverage.classify_cells(value, [cell("far", 50, 50)])
    # Native far centers at+/-900cm begin outside the exact800cm near extent.
    counts, _ = coverage.classify_cells(value, [cell("far", 54, 50), cell("far", 45, 50), cell()])
    assert counts["cells"] == 3
    assert counts["area_cm2"] == 2 * 40000 + 625


@pytest.mark.parametrize("mutate", [lambda value: value.update(yaw_deg=float("nan")),
                                    lambda value: value.update(start_cm=[1010, 200, 300]),
                                    lambda value: value.update(radius_cm=30),
                                    lambda value: value.update(transition_count=1),
                                    lambda value: value.update(raster_support_valid=False),
                                    lambda value: value.update(corridor_status="offmesh_transition")])
def test_truth_query_rejects_wrong_pose_or_unsupported_corridor(mutate):
    value = truth()
    mutate(value)
    with pytest.raises(ValueError):
        coverage.validate_truth(frame(), value)


@pytest.mark.parametrize("status", ["incomplete_path", "offmesh_transition", "short_path", "unsupported_pose"])
def test_unavailable_corridor_is_a_frame_denominator_with_no_native_cells(status):
    value = truth()
    value.update(corridor_available=False, native_cells=[], corridor_status=status)
    if status == "incomplete_path":
        value["complete"] = False
    if status == "short_path":
        value["length_cm"] = 400
    if status == "offmesh_transition":
        value["transition_count"] = 1
    if status == "unsupported_pose":
        value["raster_support_valid"] = False
    coverage.validate_truth(frame(), value)
    assert coverage.fractions({})["fractions"] == dict(free=None, unknown=None, danger=None)


def test_original_record_validation_checks_the_current_moving_goal():
    value = record()
    value["time_ms"] = 500
    value["position_cm"] = [100, 200, 300]
    value["body_yaw_deg"] = 90
    value["roads"]["goal_direction"] = [0, 0, 1]
    value["roads"]["goal_distance_cm"] = 1000
    query = dict(frame(), goal_cm=[100, 200, 1300])
    coverage.validate_record(value, query, 500)
    query["goal_cm"][2] += 2
    with pytest.raises(ValueError, match="current moving goal"):
        coverage.validate_record(value, query, 500)


def test_analyze_case_scans_original_gzip_once_and_serializes_native_counts(tmp_path, monkeypatch):
    source, output = tmp_path / "source", tmp_path / "output"
    directory = output / "cases" / "case"
    source.mkdir()
    directory.mkdir(parents=True)
    values = np.zeros(3, dtype=coverage.demos.RECORD_V2_DTYPE)
    values["time_ms"] = [10000, 10100, 10200]
    values["roads"]["goal_direction"][:, 2] = 1
    values["roads"]["goal_distance_cm"] = 1000
    values["memory_near"]["channels"][:, 32, 32, :2] = 255
    with gzip.open(source / "samples.bin.gz", "wb") as stream:
        stream.write(coverage.demos.HEADER.pack(coverage.demos.MAGIC, 2, coverage.demos.RECORD_BYTES, 238, 64, 100, 100))
        stream.write(values.tobytes())
    queries, truths = [], []
    for index in (0, 2):
        query = dict(label=f"case-frame{index}", index=index, elapsed_ms=index * 100,
                     position_cm=[0, 0, 0], goal_cm=[1000, 0, 0], yaw_deg=0)
        queries.append(query)
        truths.append(truth(query))
    (directory / "queries.jsonl").write_text("".join(json.dumps(value) + "\n" for value in queries))
    (directory / "truth.jsonl").write_text("".join(json.dumps(value) + "\n" for value in truths))
    original = coverage.demos.iter_chunks
    calls = []

    def counted(*args, **kwargs):
        calls.append(args[0])
        yield from original(*args, **kwargs)

    monkeypatch.setattr(coverage.demos, "iter_chunks", counted)
    case = dict(id="case", source=str(source), decisions=3, sampled_frames=2, goal_kind="follow")
    result = coverage.analyze_case(SimpleNamespace(output=output), case)
    assert calls == [str(source)]
    assert result["status"]["sampled_frames"] == 2
    assert result["coverage"]["fractions"] == dict(free=1, unknown=0, danger=0)
    assert result["coverage"]["denominator_cells"] == 2
    assert json.loads((directory / "summary.json").read_text())["coverage"] == result["coverage"]


def test_truth_distribution_rejects_missing_or_duplicate_queries(tmp_path):
    output = tmp_path / "coverage"
    directory = output / "cases" / "case"
    directory.mkdir(parents=True)
    query = frame()
    (directory / "queries.jsonl").write_text(json.dumps(query) + "\n")
    source = tmp_path / "truth.jsonl"
    args = SimpleNamespace(output=output, truth=source)
    source.write_text("")
    with pytest.raises(ValueError, match="Incomplete true corridors"):
        coverage.distribute_truth(args, [dict(id="case")])
    source.write_text((json.dumps(truth()) + "\n") * 2)
    with pytest.raises(ValueError, match="duplicate truth"):
        coverage.distribute_truth(args, [dict(id="case")])


def test_aggregate_weights_native_cell_observations_and_serializes_missing_denominators():
    case = dict(status=dict(sampled_frames=1), coverage=coverage.fractions(dict(cells=2, free=1, danger=1, truth_passable=2, truth_passable_free=1,
                                                                            truth_passable_danger=1, false_danger=1)),
                layers=dict(near=coverage.fractions(dict(cells=2, free=1, danger=1)), far=coverage.fractions({})))
    result = coverage.aggregate([case, case])
    assert result["coverage"]["denominator_cells"] == 4
    assert result["coverage"]["false_danger_fraction_truth_passable"] == .5
    assert json.loads(json.dumps(result))["layers"]["far"]["denominator_cells"] == 0


@pytest.mark.parametrize("kind", ["point", "object", "approach"])
@pytest.mark.parametrize("success", [True, False])
def test_static_and_approach_outcomes_use_native_metadata_success(tmp_path, kind, success):
    result = coverage.episode_outcome(kind, dict(success=success, failure="path_timeout"), tmp_path)
    assert result["outcome_success"] is success
    assert result["outcome"] == ("success" if success else "failure")
    assert result["metadata_success"] is success


@pytest.mark.parametrize("times,distances,elapsed,native_success,expected_fraction,expected_reason", [
    ([0, 800, 1000], [300, 600, 300], 1000, False, .8, None),
    ([0, 1000, 2000], [600, 300, 100], 3000, True, .5, "band_fraction_below_80_percent"),
    ([0, 1000], [700, 600], 2000, True, None, "no_first_contact"),
    ([0, 1000], [600, 300], 1000, True, None, "no_post_contact_time"),
])
def test_follow_outcome_uses_weighted_band_after_contact_and_rejects_no_contact(tmp_path, times, distances, elapsed,
                                                                            native_success, expected_fraction, expected_reason):
    trace = np.column_stack((times, np.zeros((len(times), 3)), distances))
    np.savetxt(tmp_path / "target-trajectory.txt", trace)
    result = coverage.episode_outcome("follow", dict(success=native_success, elapsed_ms=elapsed), tmp_path)
    assert result["metadata_success"] is native_success
    assert result["follow_fraction"] == expected_fraction
    assert result["outcome_failure_reason"] == expected_reason
    assert result["outcome_success"] is (expected_reason is None)


def test_outcome_aggregation_preserves_native_cell_and_truth_passable_denominators_for_each_layer():
    def case(outcome, near, far):
        totals = coverage.Counter(near)
        totals.update(far)
        return dict(outcome=outcome, outcome_success=outcome == "success", status=dict(sampled_frames=2),
                    coverage=coverage.fractions(totals), layers=dict(near=coverage.fractions(near), far=coverage.fractions(far)))

    successful = case("success", dict(cells=2, free=2, truth_passable=2, truth_passable_free=2), {})
    failed = case("failure", dict(cells=2, unknown=2, truth_passable=1, truth_passable_unknown=1, truth_unpassable=1),
                  dict(cells=1, danger=1, false_danger=1, truth_passable=1, truth_passable_danger=1))
    strata = coverage.aggregate_outcomes([successful, failed])
    assert strata["success"]["scenarios"] == strata["failure"]["scenarios"] == 1
    assert strata["success"]["coverage"]["fractions"] == dict(free=1, unknown=0, danger=0)
    assert strata["failure"]["coverage"]["fractions"] == dict(free=0, unknown=2 / 3, danger=1 / 3)
    assert strata["failure"]["coverage"]["false_danger_fraction_truth_passable"] == .5
    assert strata["failure"]["coverage"]["false_danger_fraction_all_cells"] == 1 / 3
    assert strata["failure"]["layers"]["near"]["truth_blocked_fraction_all_cells"] == .5
    assert strata["failure"]["layers"]["far"]["false_danger_fraction_truth_passable"] == 1
    pooled = coverage.aggregate([successful, failed])
    assert sum(value["coverage"]["denominator_cells"] for value in strata.values()) == pooled["coverage"]["denominator_cells"]
    assert coverage.aggregate_outcomes([successful])["failure"]["coverage"]["denominator_cells"] == 0


def test_outcome_aggregation_rejects_missing_or_contradictory_labels():
    with pytest.raises(ValueError, match="Missing"):
        coverage.aggregate_outcomes([{}])
    with pytest.raises(ValueError, match="Contradictory"):
        coverage.aggregate_outcomes([dict(outcome="success", outcome_success=False)])


def test_old_preparation_is_enriched_with_follow_outcome_without_mutating_sampling_or_raw_source(monkeypatch):
    original = dict(id="follow-case", goal_kind="follow", source="/readonly/v3-visible/follow/train/follow-case",
                    success=True, sampled_frames=51)
    current = dict(original, outcome="failure", outcome_success=False, follow_fraction=.28,
                   outcome_failure_reason="band_fraction_below_80_percent")
    current.pop("sampled_frames")
    monkeypatch.setattr(coverage, "source_cases", lambda task, data: [current])
    enriched = coverage.refresh_outcomes(SimpleNamespace(task=Path("/task"), data=Path("/data")), [original])
    assert enriched[0]["sampled_frames"] == 51
    assert enriched[0]["success"] is True
    assert enriched[0]["outcome_success"] is False
    assert "outcome" not in original
    assert enriched[0]["source"] == original["source"]


def test_moving_goal_validation_tracks_two_current_goal_positions_not_only_initial_target():
    first = record()
    first["time_ms"] = 1000
    first["roads"]["goal_direction"] = [0, 0, 1]
    first["roads"]["goal_distance_cm"] = 1000
    second = first.copy()
    second["time_ms"] = 2000
    second["roads"]["goal_direction"] = [1, 0, 0]
    second["roads"]["goal_distance_cm"] = 500
    initial_query = dict(label="follow-frame0", index=0, elapsed_ms=0, position_cm=[0, 0, 0], goal_cm=[1000, 0, 0], yaw_deg=0)
    current_query = dict(initial_query, label="follow-frame10", index=10, elapsed_ms=1000, goal_cm=[0, 0, 500])
    coverage.validate_record(first, initial_query, 1000)
    coverage.validate_record(second, current_query, 1000)
    with pytest.raises(ValueError, match="current moving goal"):
        coverage.validate_record(second, dict(current_query, goal_cm=initial_query["goal_cm"]), 1000)
