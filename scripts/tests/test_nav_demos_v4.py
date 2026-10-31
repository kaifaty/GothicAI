"""Asset-free checks for moving validation; never launch a game or write NAVDEMO."""
from copy import deepcopy
import importlib.util
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest


SPEC = importlib.util.spec_from_file_location("nav_demos_v4", Path(__file__).resolve().parents[1] / "nav_demos_v4.py")
validation = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(validation)
v3 = validation.v3


def scenario(kind="follow", split="validation", index=0):
    return dict(id=f"{kind}-{split}-{index}", goal_kind=kind, split=split, region=f"region-{split}-{index}",
        start=f"S{index}", goal=f"G{index}", target="TARGET", route=[f"G{index}", f"T{index}", f"R{index}"], seed=42 + index)


def args_for(task):
    return validation.runner_args(SimpleNamespace(task=task, limit_ms=240000), 0)


def write_witness(task, args, value, teacher, worker="0"):
    path = validation.output_for(task) / "workers" / worker / "scenarios" / f"{validation.scenario_name(value, teacher)}.bench"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join([f"seed {value['seed']}", "cheat god", "set time 10 0", "navdemos physics cv",
        *validation.command_sequence(args, value, teacher), "label done", ""]))
    return path


def write_episode(directory, value, teacher="v3"):
    directory.mkdir(parents=True, exist_ok=True)
    metadata = dict(start=value["start"], goal="RUNTIME_WAYPOINT", goal_kind=value["goal_kind"], seed=value["seed"],
        teacher_version=3 if teacher == "v3" else 2, demo_version=2, navigation_memory_version=3,
        perception_mode="gaze", physics_mode="cv", success=True, limit_ms=240000, elapsed_ms=150, decisions=2,
        record=False, demonstration_recorded=False, oracle=False,
        observation_source="learner_memory" if teacher == "v3" else "detour_teacher_v2",
        failure="", blocked_spans=0, blocked_ms=0, max_blocked_ms=0, final_dead=False, final_down=False)
    if value["goal_kind"] == "approach":
        metadata.update(target_immortal=True, target_nonhostile=True, target_final_dead=False, target_final_down=False)
    v3.write_json(directory / "episode.json", metadata)
    actions = np.zeros((3, 9))
    actions[:, 0] = [0, 100, 150]
    actions[:, 4] = 1
    actions[:-1, 1] = 1
    np.savetxt(directory / "actions.txt", actions)
    trajectory = np.zeros((3, 5))
    trajectory[:, 0] = actions[:, 0]
    np.savetxt(directory / "trajectory.txt", trajectory)
    target = np.zeros((3, 6))
    target[:, 0] = actions[:, 0]
    target[:, 4] = [900, 400, 300]
    np.savetxt(directory / "target-trajectory.txt", target)
    (directory / "teacher-paths.jsonl").write_text("{}\n")
    return metadata


def test_validation_selection_excludes_train_test_and_static_cases(tmp_path):
    values = [scenario(kind, split) for split in ("train", "validation", "test") for kind in validation.KINDS]
    values.append(dict(id="point", goal_kind="point", split="validation"))
    source = tmp_path / "input/moving-scenarios.jsonl"
    v3.write_selection(source, values)
    before = source.read_bytes()
    selected_source, selected = validation.validation_cases(tmp_path)
    assert selected_source == source
    assert selected == [value for value in values if value.get("goal_kind") in validation.KINDS and value["split"] == "validation"]
    assert source.read_bytes() == before
    assert len(selected) == 2
    values[0]["region"] = values[2]["region"]
    source.unlink()
    v3.write_selection(source, values)
    with pytest.raises(ValueError, match="Split overlap"):
        validation.validation_cases(tmp_path)


@pytest.mark.parametrize("teacher,version", [("v3", 3), ("v2", 2)])
def test_validation_commands_explicitly_disable_recording_and_oracle(tmp_path, teacher, version):
    value = scenario()
    commands = validation.command_sequence(args_for(tmp_path), value, teacher)
    assert commands[2:5] == ["navdemos record off", "navdemos perception-mode gaze", "navdemos oracle off"]
    assert commands[-2] == f"navdemos moving-v3 {version} BAU_954_MALETH S0 TARGET G0,T0,R0 follow 240000 42"
    for split in ("train", "test"):
        with pytest.raises(ValueError, match="Only v3/v2 moving validation"):
            validation.command_sequence(args_for(tmp_path), scenario(split=split), teacher)
    assert args_for(tmp_path).assets == Path.home() / "gothic-ai/game-settings"


def measured(kind, teacher, index=0, success=True, observed=100, in_band=100, contacted=True):
    return dict(scenario(kind, index=index), mode=teacher, success=success, failure="" if success else "path_timeout",
        first_contact_ms=100 if contacted else None, follow_observed_ms=observed,
        follow_in_band_ms=in_band, follow_fraction=in_band / observed if observed else None,
        elapsed_ms=240000, limit_ms=240000, final_dead=False, final_down=False,
        target_final_dead=False, target_final_down=False)


def threshold_episodes():
    episodes = []
    for teacher in validation.TEACHERS:
        episodes += [measured("follow", teacher), measured("follow", teacher, 1, observed=900, in_band=700)]
        episodes += [measured("approach", teacher, index, success=index < 4) for index in range(5)]
    return episodes


def test_follow_uses_total_time_after_contact_and_both_gates_include_80pct_boundary():
    result = validation.summarize_metrics(threshold_episodes())
    follow = result["by_kind"]["follow"]["v3"]
    assert follow["follow_fraction"] == .8  # unweighted per-case mean would be 8/9.
    assert follow["follow_observed_ms"] == 1000 and follow["follow_in_band_ms"] == 800
    assert result["by_kind"]["approach"]["v3"]["success_fraction"] == .8
    assert result["passed"] is True


def test_follow_retains_both_too_close_and_lost_contact_time_in_denominator():
    result = v3.follow_metrics([0, 100, 200, 300, 350], [900, 400, 100, 700, 400], 350)
    assert result == dict(first_contact_ms=100, follow_observed_ms=250, follow_in_band_ms=100, follow_fraction=.4)


def test_v2_success_rates_are_comparison_only_but_approach_target_deaths_are_never_ignored():
    episodes = threshold_episodes()
    for value in episodes:
        if value["mode"] == "v2":
            value["success"] = False
    assert validation.summarize_metrics(episodes)["passed"] is True
    episodes[-1]["target_final_dead"] = True
    result = validation.summarize_metrics(episodes)
    assert result["passed"] is False
    assert result["gates"]["approach_v2_no_target_death_or_down"] is False


@pytest.mark.parametrize("failure", ["lost_contact", "terminal_contact", "below_follow", "below_approach"])
def test_validation_gates_fail_for_missing_contact_or_below_threshold(failure):
    episodes = threshold_episodes()
    if failure == "lost_contact":
        episodes[0].update(first_contact_ms=None, follow_observed_ms=0, follow_in_band_ms=0)
    elif failure == "terminal_contact":
        episodes[0].update(follow_observed_ms=0, follow_in_band_ms=0)
    elif failure == "below_follow":
        episodes[0]["follow_in_band_ms"] -= 1
    else:
        next(value for value in episodes if value["goal_kind"] == "approach")["success"] = False
    assert validation.summarize_metrics(episodes)["passed"] is False


@pytest.mark.parametrize("teacher", validation.TEACHERS)
def test_raw_resume_uses_seeded_witness_then_preserves_complete_trial(tmp_path, monkeypatch, teacher):
    value, args, identity = scenario(), args_for(tmp_path), dict(limit_ms=240000, navigation_memory_version=3)
    directory = validation.directory_for(tmp_path, value, teacher)
    write_episode(directory, value, teacher)
    write_witness(tmp_path, args, value, teacher, worker="2")
    worker = validation.Worker(args, identity)
    monkeypatch.setattr(worker, "scenario", lambda *args: pytest.fail("Completed validation must not run again"))
    result = worker.trial(tmp_path, value, teacher)
    assert result["follow_fraction"] == 1 and result["first_contact_ms"] == 100
    enriched = v3.read_json(directory / "episode.json")
    assert enriched["goal"] == value["goal"] and enriched["runner_goal"] == "RUNTIME_WAYPOINT"
    assert enriched["seed"] == 42 and enriched["moving_validation_identity"] == identity
    before = {path: (path.read_bytes(), path.stat().st_mtime_ns) for path in directory.iterdir()}
    assert worker.trial(tmp_path, value, teacher) == result
    assert before == {path: (path.read_bytes(), path.stat().st_mtime_ns) for path in directory.iterdir()}
    (directory / "samples.bin.gz").write_bytes(b"forbidden; must be preserved")
    with pytest.raises(ValueError, match="forbidden NAVDEMO"):
        worker.trial(tmp_path, value, teacher)
    assert (directory / "samples.bin.gz").read_bytes() == b"forbidden; must be preserved"


@pytest.mark.parametrize("field,bad", [("navigation_memory_version", 2), ("seed", 43), ("teacher_version", 2),
    ("record", True), ("oracle", True), ("limit_ms", 2000), ("physics_mode", "native"), ("perception_mode", "rows")])
def test_resume_rejects_changed_native_identity(tmp_path, field, bad):
    value = scenario()
    directory = validation.directory_for(tmp_path, value, "v3")
    metadata = write_episode(directory, value)
    metadata[field] = bad
    with pytest.raises(ValueError, match="identity, memory or recording mode"):
        validation.validate_metadata(directory, metadata, value, "v3", dict(limit_ms=240000))


@pytest.mark.parametrize("field,bad", [("target_immortal", False), ("target_nonhostile", False),
    ("target_final_dead", None), ("target_final_down", None)])
def test_approach_resume_requires_safe_fixture_and_explicit_death_evidence(tmp_path, field, bad):
    value = scenario("approach")
    directory = validation.directory_for(tmp_path, value, "v3")
    metadata = write_episode(directory, value)
    metadata[field] = bad
    with pytest.raises(ValueError, match="immortal/nonhostile or lacks death evidence"):
        validation.validate_metadata(directory, metadata, value, "v3", dict(limit_ms=240000))


def test_resume_rejects_changed_route_identity_and_every_conflicting_witness(tmp_path, monkeypatch):
    value, args, identity = scenario(), args_for(tmp_path), dict(limit_ms=240000)
    directory = validation.directory_for(tmp_path, value, "v3")
    write_episode(directory, value)
    write_witness(tmp_path, args, value, "v3")
    worker = validation.Worker(args, identity)
    monkeypatch.setattr(worker, "scenario", lambda *args: pytest.fail("Must resume"))
    worker.trial(tmp_path, value, "v3")
    altered = deepcopy(value)
    altered["route"][1] = "OTHER"
    metadata = v3.read_json(directory / "episode.json")
    with pytest.raises(ValueError, match="different frozen trial"):
        validation.validate_metadata(directory, metadata, altered, "v3", identity)
    witness = write_witness(tmp_path, args, value, "v3", worker="1")
    witness.write_text(witness.read_text().replace("seed 42", "seed 43"))
    with pytest.raises(ValueError, match="different saved moving validation command"):
        worker.trial(tmp_path, value, "v3")


def test_target_trace_must_align_with_actions_and_death_is_reported(tmp_path):
    value = scenario()
    metadata = write_episode(tmp_path, value)
    metadata.update(failure="target_down", success=False)
    v3.write_json(tmp_path / "episode.json", metadata)
    assert validation.text_metrics(tmp_path)["target_down"] is True
    trace = np.loadtxt(tmp_path / "target-trajectory.txt")
    trace[1, 0] = 101
    np.savetxt(tmp_path / "target-trajectory.txt", trace)
    with pytest.raises(ValueError, match="does not align"):
        validation.text_metrics(tmp_path)


def test_run_refuses_an_existing_output_with_changed_settings(tmp_path, monkeypatch):
    values = [scenario(kind) for kind in validation.KINDS]
    v3.write_selection(tmp_path / "input/moving-scenarios.jsonl", values)
    monkeypatch.setattr(validation, "settings", lambda *args: dict(navigation_memory_version=3))
    v3.write_json(validation.output_for(tmp_path) / "state.json", dict(settings=dict(navigation_memory_version=2)))
    with pytest.raises(ValueError, match="another binary, input, seed, memory or time limit"):
        validation.run(SimpleNamespace(task=tmp_path, parallel=3, limit_ms=240000))


def expanded_inputs(task, monkeypatch):
    originals = [scenario(kind, split, index) for split in ("train", "validation", "test")
                 for kind in validation.KINDS for index in range(3)]
    v3.write_selection(task / "input/moving-scenarios.jsonl", originals)
    v3.write_selection(task / "input/training-pairs.jsonl", [dict(region="region-validation-0", start="S", goal="G")])
    v3.write_json(task / "input/places.json", dict(places=[]))
    for name in ("holdout-cv.jsonl", "ppo-validation.jsonl"):
        (task / "input" / name).write_text("")
    monkeypatch.setattr(v3, "exclusion_pairs", lambda *args: set())
    pool = [dict(scenario(kind, index=index), region="region-validation-0")
            for kind in validation.KINDS for index in range(3, 14)]
    # These have attractive IDs, but belong to regions held for other splits.
    pool += [dict(scenario(kind, "test", 99), id=f"{kind}-000", region="region-test-0") for kind in validation.KINDS]
    calls = []

    def candidates(places, training, excluded, seed, **kwargs):
        calls.append((places, training, excluded, seed, kwargs))
        return pool

    monkeypatch.setattr(v3, "build_candidates", candidates)
    return originals, pool, calls


def test_final_validation_retains_originals_extends_only_validation_and_freezes_before_runs(tmp_path, monkeypatch):
    originals, _, calls = expanded_inputs(tmp_path, monkeypatch)
    sentinel = validation.output_for(tmp_path) / "metrics.json"
    v3.write_json(sentinel, dict(original_attempt="untouched"))
    before = {path: (path.read_bytes(), path.stat().st_mtime_ns) for path in tmp_path.rglob("*") if path.is_file()}
    source, selected = validation.validation_cases(tmp_path, attempt=2, episodes_per_kind=10)
    assert source == tmp_path / "validation/attempt-2/cases.jsonl"
    expected_originals = [value for value in originals if value["split"] == "validation"]
    assert selected[:6] == expected_originals
    assert all(sum(value["goal_kind"] == kind for value in selected) == 10 for kind in validation.KINDS)
    assert all(value["split"] == "validation" and value["region"].startswith("region-validation-") for value in selected)
    assert calls[0][3:] == (19102026, dict(moving_per_region=20, point_count=24))
    assert before == {path: (path.read_bytes(), path.stat().st_mtime_ns) for path in before}
    frozen = source.read_bytes(), source.stat().st_mtime_ns
    assert validation.validation_cases(tmp_path, 2, 10) == (source, selected)
    assert (source.read_bytes(), source.stat().st_mtime_ns) == frozen
    assert not list(tmp_path.rglob("samples.bin*"))


def test_final_validation_rejects_changed_frozen_cases_and_short_pool(tmp_path, monkeypatch):
    _, pool, _ = expanded_inputs(tmp_path, monkeypatch)
    source, _ = validation.validation_cases(tmp_path, 2, 10)
    before = source.read_bytes()
    pool[0]["seed"] += 1
    with pytest.raises(ValueError, match="frozen selection or its inputs changed"):
        validation.validation_cases(tmp_path, 2, 10)
    assert source.read_bytes() == before
    pool.clear()
    with pytest.raises(ValueError, match="new validation follow routes"):
        validation.validation_cases(tmp_path, 2, 10)
    assert source.read_bytes() == before


def test_final_validation_rejects_changed_region_split_and_wrong_counts(tmp_path, monkeypatch):
    _, pool, _ = expanded_inputs(tmp_path, monkeypatch)
    with pytest.raises(ValueError, match="10 episodes per kind"):
        validation.validation_cases(tmp_path, 2, 3)
    for value in pool:
        if value["split"] == "validation":
            value["split"] = "train"
    with pytest.raises(ValueError, match="changed the frozen region split"):
        validation.validation_cases(tmp_path, 2, 10)
    assert not validation.output_for(tmp_path, 2).exists()


def test_final_validation_identity_binds_original_selection_training_exclusions_and_runner(tmp_path, monkeypatch):
    expanded_inputs(tmp_path, monkeypatch)
    source, selected = validation.validation_cases(tmp_path, 2, 10)
    options = SimpleNamespace(task=tmp_path, attempt=2, episodes_per_kind=10, limit_ms=240000)
    args = validation.runner_args(options, 0)
    for path in (args.runner, args.save, args.links):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("input")
    identity = validation.settings(options, source, selected)
    assert identity["attempt"] == 2 and identity["expected_by_kind"] == dict(follow=10, approach=10)
    assert args.output == tmp_path / "validation/attempt-2/workers/0"
    assert validation.directory_for(tmp_path, selected[0], "v3", 2).is_relative_to(validation.output_for(tmp_path, 2))
    for path in (args.runner, args.save, args.links, args.roads, source,
                 *(tmp_path / "input" / name for name in identity["selection_input_sha256"])):
        original = path.read_bytes()
        path.write_bytes(original + b" ")
        assert validation.settings(options, source, selected) != identity
        path.write_bytes(original)
    assert validation.settings(options, source, selected) == identity


def test_collection_follow_gate_is_independent_of_approach_and_v2_failure():
    episodes = threshold_episodes()
    for value in episodes:
        if value["goal_kind"] == "approach":
            value["success"] = False
        elif value["mode"] == "v2":
            value["follow_in_band_ms"] = 0
    result = validation.summarize_metrics(episodes)
    assert result["passed"] is False
    assert result["collection"] == dict(follow=True, approach=True, roads=True, objects=True, jumps=True)
    assert result["teacher_follow_gates"] == dict(follow_v3_passed=True, follow_v2_passed=False)
    episodes[0]["follow_in_band_ms"] -= 1
    result = validation.summarize_metrics(episodes)
    assert result["collection"]["follow"] is False
    assert result["collection"]["approach"] is True


def test_follow_summary_uses_timed_occupancy_and_keeps_native_timeout_as_evidence():
    episodes = threshold_episodes()
    episodes[0].update(success=False, failure="path_timeout")
    result = validation.summarize_metrics(episodes)
    follow = result["by_kind"]["follow"]["v3"]
    assert follow["successes"] == 1 and follow["native_successes"] == 1
    assert follow["native_failures"] == dict(path_timeout=1)
    assert follow["failures"] == dict(follow_band_below_threshold=1)
    assert result["collection"]["follow"] is True


@pytest.mark.parametrize("changed,expected", [
    (dict(elapsed_ms=1000), "follow_incomplete_horizon"),
    (dict(limit_ms=None), "follow_incomplete_horizon"),
    (dict(elapsed_ms=None), "follow_incomplete_horizon"),
    (dict(failure="actor_down", elapsed_ms=1000), "actor_down"),
    (dict(failure="target_down", elapsed_ms=1000), "target_down"),
    (dict(final_dead=True), "actor_down"),
    (dict(final_down=True), "actor_down"),
    (dict(target_final_dead=True), "target_down"),
    (dict(target_final_down=True), "target_down"),
    (dict(failure="blocked"), "blocked"),
])
def test_follow_never_passes_truncated_or_terminal_trials_even_with_perfect_band(changed, expected):
    episodes = threshold_episodes()
    episodes[0].update(changed)
    assert validation.follow_outcome(episodes[0]) == expected
    result = validation.summarize_metrics(episodes)
    assert result["by_kind"]["follow"]["v3"]["failures"][expected] == 1
    assert result["by_kind"]["follow"]["v3"]["passed"] is False
    assert result["passed"] is False
    assert result["collection"]["follow"] is False


def test_follow_no_contact_failure_uses_native_reason_and_full_horizon_boundary():
    value = measured("follow", "v3", contacted=False, observed=0, in_band=0)
    assert validation.full_follow_horizon(value) is True
    assert validation.follow_outcome(value) == "follow_no_contact"
    value.update(first_contact_ms=100, follow_observed_ms=100, follow_in_band_ms=80)
    assert validation.follow_outcome(value) == ""
    value["elapsed_ms"] -= 1
    assert validation.follow_outcome(value) == "follow_incomplete_horizon"


def test_follow_diagnostics_distinguish_too_far_too_close_and_measure_physical_speed(tmp_path):
    value = scenario()
    write_episode(tmp_path, value)
    metadata = v3.read_json(tmp_path / "episode.json")
    metadata.update(elapsed_ms=350, decisions=4)
    v3.write_json(tmp_path / "episode.json", metadata)
    times = [0, 100, 200, 300, 350]
    actions = np.zeros((5, 9))
    actions[:, 0] = times
    actions[:, 4] = 1
    actions[1, 4] = 0
    np.savetxt(tmp_path / "actions.txt", actions)
    trajectory = np.zeros((5, 5))
    trajectory[:, 0] = times
    trajectory[:, 1] = np.asarray(times) * .2
    np.savetxt(tmp_path / "trajectory.txt", trajectory)
    target = np.zeros((5, 6))
    target[:, 0] = times
    target[:, 1] = np.asarray(times) * .3
    target[:, 4] = [900, 400, 100, 700, 400]
    np.savetxt(tmp_path / "target-trajectory.txt", target)
    result = validation.text_metrics(tmp_path)
    assert result["follow_observed_ms"] == 250 and result["follow_in_band_ms"] == 100
    assert result["follow_too_close_ms"] == 100 and result["follow_too_far_ms"] == 50
    assert result["follow_actor_speed_m_s"] == pytest.approx(2)
    assert result["follow_target_speed_m_s"] == pytest.approx(3)
    assert result["follow_walk_ms"] == 150 and result["follow_run_ms"] == 100
