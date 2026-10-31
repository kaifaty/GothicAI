"""Asset-free checks for eval-only oracle collection and comparable metrics."""
import gzip
import importlib.util
import json
from collections import Counter
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest

SPEC = importlib.util.spec_from_file_location("nav_teacher_v3_oracle", Path(__file__).resolve().parents[1] / "nav_teacher_v3_oracle.py")
oracle = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(oracle)
v3 = oracle.v3


def scenario(kind="point", index=0):
    value = dict(id=f"{kind}-{index}", goal_kind=kind, start=f"S{index}", goal=f"G{index}",
                 split="train", region=f"region-{index % 6}", seed=42)
    if kind == "object":
        value["place_id"] = f"place-{index}"
    if kind in ("follow", "approach"):
        value.update(route=[f"G{index}", f"T{index}", f"R{index}"], target="TARGET")
    return value


def write_text_episode(directory, value, mode, success=True, experimental=False):
    directory.mkdir(parents=True, exist_ok=True)
    metadata = dict(value, teacher_version=2 if mode == "v2" else 3, demo_version=2, success=success,
                    decisions=5, elapsed_ms=450, limit_ms=240000, detour_length_cm=50,
                    blocked_spans=2, blocked_ms=100, max_blocked_ms=100,
                    distance_cm=150, failure="" if success else "path_timeout")
    if experimental:
        metadata.update(record=False, demonstration_recorded=False, oracle=mode == "oracle",
                        observation_source="detour_raster" if mode == "oracle" else "learner_memory",
                        start_not_found_decisions=1, oracle_unsupported_decisions=2 if mode == "oracle" else 0,
                        oracle_snapped_support_decisions=2 if mode == "oracle" else 0,
                        oracle_support_snap_max_cm=12, oracle_support_horizontal_max_cm=5, oracle_support_vertical_abs_max_cm=10)
    v3.write_json(directory / "episode.json", metadata)
    actions = np.zeros((6, 9))
    actions[:, 0] = [0, 100, 200, 300, 400, 450]
    actions[:, 4] = 1
    actions[:-1, 1] = [1, -1, 0, 1, 1]
    actions[:-1, 2] = [0, 1, -1, 0, 0]
    np.savetxt(directory / "actions.txt", actions)
    trajectory = np.zeros((6, 5))
    trajectory[:, 0] = actions[:, 0]
    trajectory[:, 1] = [0, 100, 100, 100, 100, 120]
    np.savetxt(directory / "trajectory.txt", trajectory)
    (directory / "teacher-paths.jsonl").write_text("{}\n")
    if value["goal_kind"] in ("follow", "approach"):
        trace = np.zeros((6, 6))
        trace[:, 0] = actions[:, 0]
        trace[:, 4] = [900, 400, 100, 600, 300, 300]
        np.savetxt(directory / "target-trajectory.txt", trace)
    return metadata, actions, trajectory


@pytest.mark.parametrize("kind", ["point", "follow", "approach"])
def test_text_metrics_match_existing_navdemo_metric_definitions(tmp_path, kind):
    demos = v3.module("nav_demos")
    value = scenario(kind)
    metadata, actions, trajectory = write_text_episode(tmp_path, value, "memory")
    metadata["eye_layout"] = [[0, 0, 1000]] * demos.RAY_COUNT
    v3.write_json(tmp_path / "episode.json", metadata)
    records = np.zeros(5, dtype=demos.RECORD_V2_DTYPE)
    records["time_ms"] = 1000 + np.arange(5) * 100
    records["position_cm"] = trajectory[:-1, 1:4]
    records["eye"]["distance_cm"] = 1000
    for column, field in enumerate(("forward", "strafe", "turn", "walk", "jump", "climb", "head_yaw_deg", "head_pitch_deg"), 1):
        records["keys"][field] = actions[:-1, column]
    records["previous_keys"][1:] = records["keys"][:-1]
    with gzip.open(tmp_path / "samples.bin.gz", "wb") as stream:
        stream.write(demos.HEADER.pack(demos.MAGIC, 2, demos.RECORD_BYTES, demos.RAY_COUNT, demos.NEAR_SIZE, demos.FAR_SIZE, demos.PERIOD_MS))
        stream.write(records.tobytes())
    v3.write_json(tmp_path / "contract.json", dict(records=5, mismatches=0, previous_mismatches=0))
    actual, expected = oracle.text_metrics(tmp_path), v3.episode_metrics(tmp_path)
    for key in ("success", "elapsed_ms", "decisions", "travelled_cm", "length_over_detour", "backward", "strafe",
                "strafe_left", "strafe_right", "blocked_spans", "blocked_ms"):
        assert actual[key] == expected[key]
    if kind in ("follow", "approach"):
        for key in ("follow_fraction", "first_contact_ms", "follow_observed_ms", "follow_in_band_ms", "approach_success"):
            assert actual[key] == expected[key]


def test_text_metrics_reject_action_timing_corruption_and_count_transition_actions(tmp_path):
    _, actions, _ = write_text_episode(tmp_path, scenario(), "v2")
    actions[0, 5] = 1
    actions[1, 6] = 1
    np.savetxt(tmp_path / "actions.txt", actions)
    assert oracle.text_metrics(tmp_path)["jump"] == oracle.text_metrics(tmp_path)["climb"] == 1
    actions[1, 0] = 101
    np.savetxt(tmp_path / "actions.txt", actions)
    with pytest.raises(ValueError, match="times do not align"):
        oracle.text_metrics(tmp_path)


def test_eval_commands_disable_writer_and_reset_oracle_per_scenario():
    args = SimpleNamespace(roads=Path("/places.json"), npc="NPC", limit_ms=240000)
    commands = oracle.command_sequence(args, scenario("point"), "oracle")
    assert commands[2:4] == ["navdemos record off", "navdemos oracle on"]
    assert commands[-2] == "navdemos start-v3 NPC S0 G0 240000"
    commands = oracle.command_sequence(args, scenario("follow"), "v2")
    assert commands[2:4] == ["navdemos record off", "navdemos oracle off"]
    assert commands[-2].startswith("navdemos moving-v3 2 NPC S0 TARGET G0,T0,R0 follow 240000 42")
    assert oracle.command_sequence(args, scenario("point"), "v2")[-2] == "navdemos start-v2 NPC S0 G0 240000"
    with pytest.raises(ValueError):
        oracle.command_sequence(args, scenario("point"), "memory")


def experiment_fixture(tmp_path):
    task, data = tmp_path / "task", tmp_path / "data"
    task.mkdir()
    selected = [scenario(kind, index) for kind, count in oracle.EXPECTED.items() for index in range(count)]
    for value in selected:
        memory = oracle.directory_for(task, data, value, "memory")
        success = value["goal_kind"] != "point" or int(value["id"].split("-")[-1]) < 10
        write_text_episode(memory, value, "memory", success=success)
        if value["goal_kind"] in ("point", "object"):
            baseline = v3.episode_directory(data, value, 2)
            value["teacher_v2_episode"] = str(baseline)
            write_text_episode(baseline, value, "v2")
    v3.write_selection(task / "expanded-scenarios.jsonl", selected)
    return selected, SimpleNamespace(task=task, data=data, parallel=3, limit_ms=240000, budget_seconds=60)


def test_preflight_requires_exact_existing_memory_and_v2_static_references(tmp_path):
    selected, options = experiment_fixture(tmp_path)
    assert oracle.preflight(options) == selected
    memory = oracle.directory_for(options.task, options.data, selected[0], "memory")
    metadata = v3.read_json(memory / "episode.json")
    metadata["seed"] = 43
    v3.write_json(memory / "episode.json", metadata)
    with pytest.raises(ValueError, match="identity, seed or time limit"):
        oracle.preflight(options)


def test_collects_oracle_points_first_and_all_v2_with_no_memory_reruns_or_data_changes(tmp_path, monkeypatch):
    selected, options = experiment_fixture(tmp_path)
    original = {path: path.read_bytes() for path in options.data.rglob("*") if path.is_file()}
    original[options.task / "expanded-scenarios.jsonl"] = (options.task / "expanded-scenarios.jsonl").read_bytes()
    calls = []

    class Worker:
        def __init__(self, args, version, budget):
            assert args.output.is_relative_to(options.task / "oracle-experiment/workers")
            assert isinstance(budget, oracle.ExperimentBudget)

        def trial(self, value, directory, mode):
            assert directory.is_relative_to(options.task / "oracle-experiment")
            assert mode in ("oracle", "v2")
            write_text_episode(directory, value, mode, experimental=True)
            calls.append((mode, value["goal_kind"], value["id"]))

        def close(self):
            pass

    monkeypatch.setattr(oracle, "Worker", Worker)
    monkeypatch.setattr(oracle, "settings", lambda _: dict(binary="fixed", frozen=True))
    monkeypatch.setattr(oracle.campaign, "phase_start", lambda _: None)
    result = oracle.run(options)
    assert all(mode == "oracle" and kind == "point" for mode, kind, _ in calls[:30])
    assert Counter((mode, kind) for mode, kind, _ in calls) == {
        ("oracle", "point"): 30, ("oracle", "object"): 30, ("oracle", "follow"): 18, ("oracle", "approach"): 18,
        ("v2", "point"): 30, ("v2", "object"): 30, ("v2", "follow"): 18, ("v2", "approach"): 18}
    assert all(mode == "v2" and kind == "point" for mode, kind, _ in calls[30:60])
    assert result["scenarios"] == 96 and result["episodes"] == 288 and result["complete"] is True
    assert result["comparisons"]["point"]["memory_vs_v2"]["conditional_success_fraction"] == pytest.approx(10 / 30)
    assert result["comparisons"]["point"]["oracle_vs_v2"]["conditional_success_fraction"] == 1
    assert result["by_kind"]["point"]["oracle"]["start_not_found_decisions"] == 30
    assert result["by_kind"]["point"]["oracle"]["oracle_unsupported_decisions"] == 60
    assert result["by_kind"]["point"]["oracle"]["oracle_snapped_support_decisions"] == 60
    assert result["by_kind"]["point"]["oracle"]["oracle_support_snap_max_cm"] == 12
    assert result["transition_action_slice"]["point"]["v2_episodes_with_jump_or_climb"] == 0
    assert result["archived_v2_static"]["point"]["episodes"] == 30
    assert not list((options.task / "oracle-experiment").rglob("samples.bin.gz"))
    assert all(path.read_bytes() == content for path, content in original.items())


def test_experimental_resume_is_text_only_and_never_reruns_a_complete_take(tmp_path, monkeypatch):
    value = scenario("follow")
    directory = tmp_path / "episode"
    write_text_episode(directory, value, "v2", experimental=True)
    before = {path: path.read_bytes() for path in directory.iterdir()}
    worker = oracle.Worker(SimpleNamespace(limit_ms=240000), 3, None)
    monkeypatch.setattr(worker, "scenario", lambda *args: pytest.fail("A complete eval-only take must be reused"))
    assert worker.trial(value, directory, "v2")["success"] is True
    assert all(path.read_bytes() == content for path, content in before.items())
    (directory / "samples.bin.gz").write_bytes(b"must not be deleted")
    with pytest.raises(ValueError, match="training recording"):
        worker.trial(value, directory, "v2")
    assert (directory / "samples.bin.gz").read_bytes() == b"must not be deleted"


def raw_moving_fixture(tmp_path, kind="follow", mode="v2"):
    value = scenario(kind)
    experiment = tmp_path / "oracle-experiment"
    directory = experiment / mode / kind / value["split"] / value["id"]
    metadata, _, _ = write_text_episode(directory, value, mode, experimental=True)
    for key in v3.SCENARIO_IDENTITY_FIELDS:
        if key not in ("start", "goal", "goal_kind", "seed"):
            metadata.pop(key, None)
    metadata.update(goal="RESOLVED_OR_CURRENT_WAYPOINT", target_position=[12000, 1500, -4000])
    v3.write_json(directory / "episode.json", metadata)
    args = SimpleNamespace(output=experiment / "workers" / "2", roads=tmp_path / "places.json",
                           physics="cv", npc="NPC", limit_ms=240000, seed=value["seed"])
    # The old worker differs from the worker assigned after resume.
    witness_args = SimpleNamespace(**vars(args))
    witness_args.output = experiment / "workers" / "0"
    collector = v3.module("collect_nav_demos")
    witness = collector.scenario_file(witness_args, f"oracle-experiment-{mode}-{value['id']}",
                                      oracle.command_sequence(args, value, mode))
    return value, directory, metadata, args, witness


@pytest.mark.parametrize("kind", ["follow", "approach"])
@pytest.mark.parametrize("mode", ["v2", "oracle"])
def test_raw_moving_resume_uses_frozen_command_not_target_goal_and_preserves_payload(tmp_path, monkeypatch, kind, mode):
    value, directory, raw, args, _ = raw_moving_fixture(tmp_path, kind, mode)
    payload = {path: (path.read_bytes(), path.stat().st_mtime_ns) for path in directory.iterdir()
               if path.name != "episode.json"}
    events = []
    monkeypatch.setattr(oracle.campaign, "emit", lambda event, **fields: events.append((event, fields)))
    worker = oracle.Worker(args, 3, None)
    monkeypatch.setattr(worker, "scenario", lambda *args: pytest.fail("Raw completed moving take must not run again"))
    assert worker.trial(value, directory, mode)["success"] is True
    promoted = v3.read_json(directory / "episode.json")
    assert promoted["goal"] == value["goal"]
    assert promoted["runner_goal"] == raw["goal"]
    assert promoted["target_position"] == raw["target_position"]
    assert promoted["route"] == value["route"] and promoted["target"] == value["target"]
    assert events == [("oracle_trial_resumed", dict(mode=mode, kind=kind, id=value["id"], success=True, recovered_raw=True))]
    for path, (content, modified) in payload.items():
        assert (path.read_bytes(), path.stat().st_mtime_ns) == (content, modified)
    metadata_bytes = (directory / "episode.json").read_bytes()
    metadata_time = (directory / "episode.json").stat().st_mtime_ns
    worker.trial(value, directory, mode)
    assert (directory / "episode.json").read_bytes() == metadata_bytes
    assert (directory / "episode.json").stat().st_mtime_ns == metadata_time
    assert events[-1][1]["recovered_raw"] is False


@pytest.mark.parametrize("change", ["seed", "start", "version", "limit", "route", "target", "mode", "bench-route", "bench-seed", "bench-weights", "missing-bench"])
def test_raw_moving_resume_rejects_changed_identity_before_promoting(tmp_path, monkeypatch, change):
    value, directory, metadata, args, witness = raw_moving_fixture(tmp_path)
    if change in ("seed", "start", "route", "target"):
        metadata[change] = dict(seed=99, start="OTHER", route=["OTHER", "ROUTE"], target="OTHER_TARGET")[change]
    elif change == "version":
        metadata["teacher_version"] = 3
    elif change == "limit":
        metadata["limit_ms"] = 1000
    elif change == "mode":
        metadata["oracle"] = True
    elif change == "bench-route":
        witness.write_text(witness.read_text().replace("G0,T0,R0", "G0,OTHER,R0"))
    elif change == "bench-seed":
        witness.write_text(witness.read_text().replace("seed 42", "seed 99"))
    elif change == "bench-weights":
        witness.write_text(witness.read_text().replace("navdemos wait", "navdemos drive NPC S0 G0 240000 /other.navbc 0 42\nnavdemos wait"))
    else:
        witness.unlink()
    v3.write_json(directory / "episode.json", metadata)
    before = (directory / "episode.json").read_bytes()
    worker = oracle.Worker(args, 3, None)
    monkeypatch.setattr(worker, "scenario", lambda *args: pytest.fail("Invalid completed take must not be rerun"))
    with pytest.raises(ValueError, match="identity differs|scenario .* differs|mode differs|saved moving scenario"):
        worker.trial(value, directory, "v2")
    assert (directory / "episode.json").read_bytes() == before


@pytest.mark.parametrize("field", ["goal", "route", "target", "seed"])
def test_enriched_moving_identity_remains_strict(tmp_path, field):
    value = scenario("follow")
    metadata, _, _ = write_text_episode(tmp_path, value, "v2", experimental=True)
    metadata[field] = ["OTHER", "ROUTE"] if field == "route" else (99 if field == "seed" else "OTHER")
    v3.write_json(tmp_path / "episode.json", metadata)
    with pytest.raises(ValueError, match="identity, seed or time limit"):
        oracle.validate_identity(tmp_path, value, "v2", 240000, experimental=True)


@pytest.mark.parametrize("kind", ["point", "object"])
def test_raw_static_identity_still_requires_exact_goal(tmp_path, kind):
    value = scenario(kind)
    metadata, _, _ = write_text_episode(tmp_path, value, "v2", experimental=True)
    metadata["goal"] = "OTHER_GOAL"
    with pytest.raises(ValueError, match="identity differs"):
        oracle.validate_raw_identity(SimpleNamespace(limit_ms=240000), tmp_path, metadata, value, "v2")


def test_oracle_identity_requires_adapter_and_no_navdemo(tmp_path):
    value = scenario()
    metadata, _, _ = write_text_episode(tmp_path, value, "oracle", experimental=True)
    assert oracle.validate_identity(tmp_path, value, "oracle", 240000, True)["oracle"] is True
    metadata["oracle"] = False
    v3.write_json(tmp_path / "episode.json", metadata)
    with pytest.raises(ValueError, match="adapter was not enabled"):
        oracle.validate_identity(tmp_path, value, "oracle", 240000, True)
    metadata["oracle"] = True
    v3.write_json(tmp_path / "episode.json", metadata)
    (tmp_path / "samples.bin.gz").write_bytes(b"unexpected")
    with pytest.raises(ValueError, match="created training samples"):
        oracle.validate_identity(tmp_path, value, "oracle", 240000, True)


def test_experiment_storage_budget_combines_data_existing_attempts_and_own_outputs(tmp_path, monkeypatch):
    task, data = tmp_path / "task", tmp_path / "data"
    data.mkdir()
    (data / "raw").write_bytes(b"12345")
    (data / "alias").symlink_to(data / "raw")
    attempts = task / "v2-attempts"
    attempts.mkdir(parents=True)
    (attempts / "raw").write_bytes(b"123")
    experiment = task / "oracle-experiment"
    experiment.mkdir()
    (experiment / "text").write_bytes(b"1234567")
    monkeypatch.setattr(oracle.campaign.shutil, "disk_usage", lambda _: SimpleNamespace(free=100 * 1024**3))
    budget = oracle.ExperimentBudget(task, data, max_bytes=15 + v3.ESTIMATED_DECISION_BYTES)
    assert budget.used() == 15
    with budget.reserve(100):
        with pytest.raises(ValueError, match="Disk budget"):
            with budget.reserve(100):
                pytest.fail("Concurrent experimental reservations share the data budget")


def test_pairing_uses_exact_ids_and_conditions_on_successful_v2():
    reference = [dict(id="A", success=True, elapsed_ms=10, travelled_cm=20), dict(id="B", success=False, elapsed_ms=20, travelled_cm=30)]
    trials = [dict(id="A", success=True, elapsed_ms=20, travelled_cm=30), dict(id="B", success=False, elapsed_ms=20, travelled_cm=30)]
    result = oracle.paired_comparison(reference, trials)
    assert result["conditional_success_fraction"] == 1
    assert result["elapsed_ratio_on_joint_success"] == 2
    assert result["travelled_ratio_on_joint_success"] == 1.5
    with pytest.raises(ValueError, match="identical, unique"):
        oracle.paired_comparison(reference, trials[:-1])
