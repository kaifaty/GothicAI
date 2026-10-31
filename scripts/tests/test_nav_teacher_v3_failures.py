"""Geometry and timing checks for the offline first-take failure analysis."""
import gzip
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest

SPEC = importlib.util.spec_from_file_location("nav_teacher_v3_failures", Path(__file__).resolve().parents[1] / "nav_teacher_v3_failures.py")
analysis = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analysis)


def test_body_world_roundtrip_and_yaw_do_not_create_corridor_switches():
    world = np.array([[100, 200], [200, 400], [1000, 0]], dtype=float)
    rows = []
    for index, yaw in enumerate((0, 90, 180, -90)):
        local = analysis.world_to_local(world, [100, 0, 200], yaw)
        reconstructed = analysis.local_to_world(local, [100, 0, 200], yaw)
        np.testing.assert_allclose(reconstructed, world, atol=1.e-10)
        rows.append(dict(index=index, position_cm=[100, 0, 200], goal_cm=[1000, 0, 200], world_plan_xz_cm=reconstructed.tolist()))
    result = analysis.world_plan_metrics(rows)
    assert result["world_path_aba"] == 0
    assert result["corridor_side_switches"] == 0
    assert result["world_bearing_jumps_ge45"] == 0
    assert result["same_corridor_fraction_le50"] == 1


def test_world_shape_aba_requires_a_changed_corridor_then_return():
    first = [[0, 0], [100, 200], [1000, 0]]
    second = [[0, 0], [100, -200], [1000, 0]]
    rows = [dict(index=index, position_cm=[0, 0, 0], goal_cm=[1000, 0, 0], world_plan_xz_cm=path)
            for index, path in enumerate((first, second, first))]
    assert analysis.world_plan_metrics(rows)["world_path_aba"] == 1
    rows[-1]["position_cm"] = [200, 0, 0]
    assert analysis.world_plan_metrics(rows)["world_path_aba"] == 0


def test_turn_flip_same_corridor_is_measured_at_each_flip():
    a, b = [[0, 0], [100, 200], [1000, 0]], [[0, 0], [100, -200], [1000, 0]]
    rows = [dict(index=index, position_cm=[0, 0, 0], goal_cm=[1000, 0, 0], world_plan_xz_cm=path,
                 applied_actions=dict(turn=turn))
            for index, (path, turn) in enumerate(((a, 1), (a, -1), (b, 1), (b, 0), (b, -1)))]
    result = analysis.world_plan_metrics(rows)
    assert result["turn_flip_events"] == 3
    assert result["turn_flip_evaluable_events"] == 3
    assert result["turn_flip_same_corridor_events"] == 2
    rows[-1]["position_cm"] = [200, 0, 0]
    assert analysis.world_plan_metrics(rows)["turn_flip_same_corridor_events"] == 1


def test_polyline_distance_and_sampling_follow_segments_instead_of_vertices():
    path = [[0, 0], [100, 0], [100, 100]]
    np.testing.assert_allclose(analysis.sample_polyline(path, step=50), [[0, 0], [50, 0], [100, 0], [100, 50], [100, 100]])
    np.testing.assert_allclose(analysis.distances_to_path([[50, 10], [120, 50]], path), [10, 20])
    assert analysis.path_hausdorff(path, path) == 0


def test_net_stall_detects_commanded_jitter_that_travel_stationarity_does_not():
    times = np.arange(61) * 100
    positions = np.zeros((61, 3))
    positions[:, 0] = np.sin(np.arange(61) * np.pi / 2) * 15
    actions = np.zeros((61, 9))
    actions[:, 0] = times
    actions[:-1, 1] = 1
    assert analysis.stationary_spans(times, positions) == []
    spans = analysis.blocked_spans(times, positions, actions)
    assert spans[0]["start_ms"] == 0
    assert spans[0]["detected_ms"] == 3000
    actions[:, 1] = 0
    assert analysis.blocked_spans(times, positions, actions) == []


def test_selected_tail_is_100_decisions_and_diagnostics_include_last_at_1hz():
    actions = np.zeros((2401, 9))
    actions[:, 0] = np.arange(2401) * 100
    tail, diagnostic, onset, onset_indices = analysis.select_indices(2400, actions, [])
    assert tail == list(range(2300, 2400))
    assert 2399 in diagnostic and 2299 in diagnostic
    assert len([index for index in diagnostic if index >= 2299]) == 12  # 11 Hz frames plus exact fallback onset.
    assert onset is None
    assert onset_indices == [2270, 2280, 2290, 2300]


def test_action_alignment_uses_motion_after_the_current_command():
    trajectory = np.zeros((2, 5))
    trajectory[:, 0] = [0, 100]
    trajectory[1, 3] = -10
    actions = np.zeros((2, 9))
    actions[:, 0] = [0, 100]
    actions[0, 2] = 1  # Requested right, physical next motion is left.
    result = analysis.execution_metrics(dict(index=0), [[0, 0], [100, 0]], trajectory, actions)
    assert result["action_interval_ms"] == 100
    assert result["next_delta_local_right_forward_cm"] == [-10, 0]
    assert result["next_delta_command_cosine"] == -1


def test_corridor_uses_any_nonzero_hazard_and_separates_height_mismatch():
    record = np.zeros(1, dtype=analysis.demos.RECORD_V2_DTYPE)[0]
    record["memory_far"]["channels"][55, 50] = [3, 255, 1, 0, 0, 0, 0]
    record["memory_far"]["height_valid"][55, 50] = 1
    record["memory_far"]["ground_height_cm"][55, 50] = 0
    result = analysis.memory_corridor(record, [[1000, 200, 0], [1010, 200, 0]])
    far = result["far"]
    assert far["danger_samples"] == 2
    assert far["danger_bins"]["q1_25"] == 2
    assert far["danger_channel_samples"]["wall"] == 2
    assert far["hit_confidence_median"] == 3
    assert far["hit_free_median"] == 255
    assert far["hit_height_mismatch_gt100_samples"] == 2
    assert far["hit_height_within100_samples"] == 0
    assert result["far_boundary_8_12m"]["danger_samples"] == 2


def test_unknown_exposure_uses_full_quantized_confidence_and_free():
    record = np.zeros(1, dtype=analysis.demos.RECORD_V2_DTYPE)[0]
    record["memory_near"]["channels"][32, 32, :2] = [255, 255]
    assert analysis.path_memory_exposure(record, [[0, 0]])["unknown_exposure_mean"] == 0
    result = analysis.path_memory_exposure(record, [[0, 1000]])
    assert result["unknown_exposure_mean"] == 1
    assert result["zero_confidence_or_free_fraction"] == 1


def diagnostic_variant(target_kind, reached):
    return dict(target_kind=target_kind, options=dict(body_radius_cm=30, unknown_cost=4, near_only=False, known_free_only=False),
                actual_target_reached=reached, plan_complete=True, target_outside_map=False, target_clamped=False, start_found=True)


def test_goal_and_carrot_connectivity_are_never_combined():
    goal = diagnostic_variant("goal", False)
    steering = diagnostic_variant("steering", True)
    assert analysis.select_variant([goal, steering]) is steering
    assert analysis.select_variant([goal, steering], target_kind="goal") is goal
    frames = [dict(beyond_known=False, production=steering, goal_production=goal, variants=[goal, steering])]
    assert analysis.connectivity_summary(frames, "steering")["exact_target_reached"] == 1
    assert analysis.connectivity_summary(frames, "goal")["exact_target_reached"] == 0
    assert analysis.connectivity_summary(frames, "goal")["snapped_node_only"] == 1


def test_diagnostics_replay_original_source_not_the_cropped_npz(tmp_path, monkeypatch):
    case_output = tmp_path / "failed"
    case_output.mkdir()
    binary = tmp_path / "diagnostics"
    binary.touch()
    (case_output / "diagnostic-indices.txt").write_text("3\n")
    source = "/original/episode/samples.bin.gz"
    calls = []

    def fake_run(command, **kwargs):
        calls.append(command)
        kwargs["stdout"].write(json.dumps(dict(type="record", index=3)) + "\n")
        kwargs["stdout"].write(json.dumps(dict(type="summary", records=10, mismatches=0, previous_mismatches=0)) + "\n")
        return SimpleNamespace(returncode=0, stderr="")

    monkeypatch.setattr(analysis.subprocess, "run", fake_run)
    records, summary = analysis.diagnostic_rows(SimpleNamespace(output=tmp_path, diagnostics=binary, reuse_diagnostics=False),
                                               dict(id="failed", source_samples=source, decisions=10, diagnostic_indices=[3]))
    assert calls[0] == [str(binary), source, "--indices", str(case_output / "diagnostic-indices.txt"), "--paths"]
    assert list(records) == [3]
    assert summary["records"] == 10


def write_episode(task, data):
    value = dict(id="failed", goal_kind="point", start="A", goal="B", region="area0", split="train", seed=7)
    task.mkdir()
    (task / "expanded-scenarios.jsonl").write_text(json.dumps(value) + "\n")
    directory = analysis.v3.episode_directory(data, value, 3)
    directory.mkdir(parents=True)
    count = 120
    records = np.zeros(count, dtype=analysis.demos.RECORD_V2_DTYPE)
    records["time_ms"] = 1000 + np.arange(count) * 100
    records["position_cm"][:, 0] = np.minimum(np.arange(count), 20) * 10
    records["keys"]["forward"] = 1
    records["memory_near"]["channels"][:, 32, 32, 0] = 255
    records["memory_near"]["channels"][:, 32, 32, 1] = 255
    with gzip.open(directory / "samples.bin.gz", "wb") as stream:
        d = analysis.demos
        stream.write(d.HEADER.pack(d.MAGIC, 2, d.RECORD_BYTES, d.RAY_COUNT, d.NEAR_SIZE, d.FAR_SIZE, d.PERIOD_MS))
        stream.write(records.tobytes())
    actions = np.zeros((count + 1, 9))
    actions[:, 0] = np.arange(count + 1) * 100
    actions[:-1, 1] = 1
    actions[:, 4] = 1
    np.savetxt(directory / "actions.txt", actions)
    trajectory = np.zeros((count + 1, 5))
    trajectory[:, 0] = actions[:, 0]
    trajectory[:-1, 1:4] = records["position_cm"]
    trajectory[-1, 1:4] = records["position_cm"][-1]
    np.savetxt(directory / "trajectory.txt", trajectory)
    plans = [dict(elapsed_ms=index * 100, position_cm=record["position_cm"].tolist(), yaw_deg=0, goal_cm=[1000, 0, 0],
                  carrot_cm=[1000, 0, 0], beyond_known=True, local_plan_cm=[[0, 0], [0, 500]]) for index, record in enumerate(records)]
    (directory / "teacher-paths.jsonl").write_text("".join(json.dumps(plan) + "\n" for plan in plans))
    analysis.v3.write_json(directory / "episode.json", dict(value, teacher_version=3, demo_version=2, decisions=count,
                           success=False, elapsed_ms=count * 100, target_position=[1000, 0, 0], failure="path_timeout", final_dead=False, final_down=False))
    return directory


def test_prepare_extracts_all_failures_with_lossless_full_resolution_frames(tmp_path):
    task, data = tmp_path / "task", tmp_path / "data"
    source = write_episode(task, data)
    output = task / "failures"
    analysis.prepare(SimpleNamespace(task=task, data=data, output=output, expected_points=1, expected_successes=0))
    extraction = analysis.v3.read_json(output / "extraction.json")
    assert extraction["failures"] == 1
    case = extraction["cases"][0]
    assert case["source_samples"] == str(source / "samples.bin.gz")
    assert case["onset_kind"] == "net_displacement_blocked"
    archive = np.load(output / "failed/memory-frames.npz", allow_pickle=False)
    assert archive["records"]["memory_near"].shape[1:] == (64, 64)
    assert archive["records"]["memory_far"].shape[1:] == (100, 100)
    assert archive["records"]["memory_near"]["channels"][0, 32, 32, 0] == 255
    assert len(list(analysis.v3.rows(output / "failed/tails.jsonl"))) == 100
    queries = (output / "detour-queries.txt").read_text().splitlines()
    assert queries[0].startswith("failed-reference cm:0,0,0 cm:1000,0,0")
    assert len(queries) == 1 + len(case["diagnostic_indices"])


def test_preflight_rejects_a_different_first_take(tmp_path):
    task, data = tmp_path / "task", tmp_path / "data"
    write_episode(task, data)
    with pytest.raises(ValueError, match="points=1/30, successes=0/8"):
        analysis.failure_selection(task, data)


def test_full_analysis_serializes_goal_and_steering_counterfactuals(tmp_path, monkeypatch):
    task, data = tmp_path / "task", tmp_path / "data"
    write_episode(task, data)
    output = task / "failures"
    args = SimpleNamespace(task=task, data=data, output=output, expected_points=1, expected_successes=0,
                           detour_plans=output / "detour.jsonl", parallel=1, figures=1)
    analysis.prepare(args)
    case = analysis.v3.read_json(output / "extraction.json")["cases"][0]
    diagnostics = {}
    for index in case["diagnostic_indices"]:
        variants = []
        options = [("steering", radius, cost, False, False) for radius in (30, 15, 0) for cost in (1, 2, 4, 8, 16)]
        options += [("steering", 30, 4, False, True), ("steering", 30, 4, True, False), ("steering", 30, 4, True, True)]
        options += [("goal", radius, 4, False, False) for radius in (30, 15, 0)] + [("goal", 30, 4, False, True)]
        for target_kind, radius, cost, near, known in options:
            variants.append(dict(name=f"{target_kind}-{radius}-{cost}-{near}-{known}", target_kind=target_kind,
                options=dict(body_radius_cm=radius, unknown_cost=cost, near_only=near, known_free_only=known),
                actual_target_reached=target_kind == "steering" or radius == 0, plan_complete=True,
                target_outside_map=False, target_clamped=False, start_found=True, target=[0, 500], local_target=[0, 500],
                target_clear=True, start_clear=True, snapped_target=[12.5, 487.5], snapped_target_distance_cm=18,
                endpoint_distance_to_actual_target_cm=0, plan_length_cm=500, plan_cost=500 * cost,
                path=[[0, 0], [0, 500]]))
        diagnostics[index] = dict(type="record", index=index, diagnostics=variants)
    replay = dict(type="summary", records=case["decisions"], mismatches=0, previous_mismatches=0)
    monkeypatch.setattr(analysis, "diagnostic_rows", lambda *_: (diagnostics, replay))
    detours = [dict(label=line.split()[0], complete=True, length_cm=1000,
                    start_cm=analysis.parse_coordinate(line.split()[1]).tolist(), goal_cm=[1000, 0, 0],
                    corners_cm=[[0, 0, 0], [1000, 0, 0]])
               for line in (output / "detour-queries.txt").read_text().splitlines()]
    args.detour_plans.write_text("".join(json.dumps(value) + "\n" for value in detours))
    analysis.analyze(args)
    result = analysis.v3.read_json(output / "metrics.json")
    assert result["analyzed_failures"] == 1
    assert result["connectivity"]["steering"]["exact_target_reached"] > 0
    assert result["connectivity"]["goal"]["exact_target_reached"] == 0
    assert result["hypotheses"]["H2_body_margin"]["goal_r0_open_r30_closed"] > 0
    assert result["cases"][0]["unknown_cost_sensitivity"]["1"]["frames"] == len(case["diagnostic_indices"])
    assert (output / "failed.svg").is_file()


def test_detour_results_must_cover_exact_queries_with_correct_endpoints(tmp_path):
    query, result = tmp_path / "queries.txt", tmp_path / "detour.jsonl"
    query.write_text("A cm:0,0,0 cm:100,0,0\nB cm:0,0,0 cm:200,0,0\n")
    with pytest.raises(ValueError, match="Missing Detour"):
        analysis.checked_detours(query, result)
    value = dict(label="A", start_cm=[0, 0, 0], goal_cm=[100, 0, 0], complete=False, reason="no_path")
    result.write_text(json.dumps(value) + "\n")
    with pytest.raises(ValueError, match="label mismatch"):
        analysis.checked_detours(query, result)
    second = dict(value, label="B", goal_cm=[200, 0, 0])
    result.write_text(json.dumps(value) + "\n" + json.dumps(second) + "\n")
    assert len(analysis.checked_detours(query, result)) == 2  # Incomplete paths remain valid measured facts.
    result.write_text(json.dumps(value) + "\n" + json.dumps(value) + "\n")
    with pytest.raises(ValueError, match="Duplicate Detour"):
        analysis.checked_detours(query, result)
    result.write_text(json.dumps(value) + "\n" + json.dumps(dict(second, goal_cm=[205, 0, 0])) + "\n")
    with pytest.raises(ValueError, match="goal_cm mismatch"):
        analysis.checked_detours(query, result)
