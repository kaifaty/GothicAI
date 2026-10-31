"""Asset-free contracts for the same-build, eval-only perception comparison."""
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest

SPEC = importlib.util.spec_from_file_location("nav_perception_eval", Path(__file__).resolve().parents[1] / "nav_perception_eval.py")
evaluation = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(evaluation)


def scenario(kind="point", index=0):
    value = dict(id=f"{kind}-{index}", goal_kind=kind, start=f"S{index}", goal=f"G{index}",
                 split="train", region=f"region-{index % 6}", seed=42 + index)
    if kind == "object":
        value["place_id"] = f"place-{index}"
    return value


def write_episode(directory, value, teacher, mode="gaze", memory_version=2):
    directory.mkdir(parents=True, exist_ok=True)
    metadata = dict(start=value["start"], goal=value["goal"], goal_kind=value["goal_kind"], seed=0,
                    teacher_version=2 if teacher == "v2" else 3, demo_version=2, success=True,
                    navigation_memory_version=memory_version, perception_mode=mode,
                    decisions=2, elapsed_ms=150, limit_ms=240000, detour_length_cm=50,
                    blocked_spans=0, blocked_ms=0, max_blocked_ms=0, failure="",
                    record=False, demonstration_recorded=False, oracle=teacher == "oracle",
                    observation_source={"memory": "learner_memory", "oracle": "detour_raster", "v2": "detour_teacher_v2"}[teacher])
    evaluation.v3.write_json(directory / "episode.json", metadata)
    actions = np.zeros((3, 9))
    actions[:, 0] = [0, 100, 150]
    actions[:, 4] = 1
    actions[:-1, 1] = 1
    np.savetxt(directory / "actions.txt", actions)
    trajectory = np.zeros((3, 5))
    trajectory[:, 0] = actions[:, 0]
    trajectory[:, 1] = [0, 100, 150]
    np.savetxt(directory / "trajectory.txt", trajectory)
    (directory / "teacher-paths.jsonl").write_text("{}\n")
    return metadata


def args_for(task):
    args = evaluation.perception.runner_args(task, task / "evaluation/workers/0")
    args.limit_ms = evaluation.LIMIT_MS
    return args


def write_witness(task, args, value, teacher, mode, output=None):
    directory = (output or task / "evaluation") / "workers/0/scenarios"
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / f"{evaluation.scenario_name(value, teacher)}.bench"
    path.write_text("\n".join([f"seed {value['seed']}", "cheat god", "set time 10 0", "navdemos physics cv",
                               *evaluation.command_sequence(args, value, teacher, mode), "label done", ""]))
    return path


def test_frozen_selection_keeps_exact_static_ids_and_never_writes_source(tmp_path):
    task = tmp_path / "nav-perception"
    source = task.parent / "nav-teacher-v3/expanded-scenarios.jsonl"
    values = [scenario(kind, i) for kind, count in evaluation.perception.EXPECTED.items() for i in range(count)]
    evaluation.v3.write_selection(source, values)
    original = source.read_bytes()
    path, selected = evaluation.frozen_cases(task)
    assert path == source
    assert len(selected) == 60
    assert {value["id"] for value in selected} == {value["id"] for value in values if value["goal_kind"] in ("point", "object")}
    assert source.read_bytes() == original


@pytest.mark.parametrize("teacher", evaluation.TEACHERS)
def test_every_teacher_uses_same_perception_cv_seed_and_disables_recording(tmp_path, teacher):
    task, value = tmp_path / "task", scenario()
    args = args_for(task)
    commands = evaluation.command_sequence(args, value, teacher, "gaze")
    assert commands[2:5] == ["navdemos record off", "navdemos perception-mode gaze",
                             "navdemos oracle " + ("on" if teacher == "oracle" else "off")]
    assert commands[-2] == f"navdemos start-v{2 if teacher == 'v2' else 3} BAU_954_MALETH S0 G0 240000"
    assert commands[-1] == "navdemos wait"
    witness = write_witness(task, args, value, teacher, "gaze")
    assert witness.read_text().splitlines()[0] == "seed 42"
    assert witness.read_text().splitlines()[3] == "navdemos physics cv"
    evaluation.validate_witness(task, args, value, teacher, "gaze")
    with pytest.raises(ValueError, match="different saved scenario"):
        evaluation.validate_witness(task, args, dict(value, seed=99), teacher, "gaze")


@pytest.mark.parametrize("field,value", [("navigation_memory_version", 1), ("perception_mode", "rows"),
    ("record", True), ("demonstration_recorded", True), ("oracle", True), ("teacher_version", 2),
    ("observation_source", "detour_raster"), ("goal", "OTHER"), ("limit_ms", 1000), ("drive", True)])
def test_raw_identity_refuses_wrong_mode_version_goal_or_training_record(tmp_path, field, value):
    case = scenario()
    metadata = write_episode(tmp_path, case, "memory")
    metadata[field] = value
    with pytest.raises(ValueError, match="identity, representation or recording"):
        evaluation.validate_raw(tmp_path, metadata, case, "memory", "gaze")


def test_water_memory_version_requires_explicit_matching_native_representation(tmp_path):
    case = scenario()
    metadata = write_episode(tmp_path, case, "memory", memory_version=3)
    evaluation.validate_raw(tmp_path, metadata, case, "memory", "gaze", memory_version=3)
    with pytest.raises(ValueError, match="identity, representation or recording"):
        evaluation.validate_raw(tmp_path, metadata, case, "memory", "gaze")
    with pytest.raises(ValueError, match="identity, representation or recording"):
        evaluation.validate_raw(tmp_path, dict(metadata, navigation_memory_version=2), case,
                                "memory", "gaze", memory_version=3)


def test_completed_native_episode_is_recovered_once_then_strictly_resumed(tmp_path, monkeypatch):
    task, value = tmp_path / "task", scenario()
    args = args_for(task)
    identity = dict(perception_mode="gaze", runner_sha256="binary", cohort_sha256="cohort", navigation_memory_version=2)
    worker = evaluation.Worker(args, identity)
    calls = []

    def run(name, commands, directory, seed):
        calls.append((name, commands, seed))
        write_witness(task, args, value, "memory", "gaze")
        write_episode(directory, value, "memory")

    monkeypatch.setattr(worker, "scenario", run)
    first = worker.trial(task, value, "memory", "gaze")
    directory = evaluation.directory_for(task, value, "memory")
    original = (directory / "episode.json").read_bytes()
    second = worker.trial(task, value, "memory", "gaze")
    assert first == second
    assert len(calls) == 1
    assert (directory / "episode.json").read_bytes() == original
    metadata = json.loads(original)
    assert metadata["seed"] == 42 and metadata["runner_seed"] == 0
    assert metadata["perception_evaluation_identity"] == identity
    assert not list(task.rglob("samples.bin.gz"))
    worker.identity = dict(identity, runner_sha256="different")
    with pytest.raises(ValueError, match="different frozen trial"):
        worker.trial(task, value, "memory", "gaze")


def test_partial_output_is_not_silently_overwritten(tmp_path):
    task, value = tmp_path / "task", scenario()
    directory = evaluation.directory_for(task, value, "memory")
    directory.mkdir(parents=True)
    (directory / "trajectory.txt").write_text("partial")
    worker = evaluation.Worker(args_for(task), {})
    with pytest.raises(ValueError, match="refusing to overwrite"):
        worker.trial(task, value, "memory", "gaze")
    assert (directory / "trajectory.txt").read_text() == "partial"


def test_same_build_experiment_collects_all_three_modes_and_resumes_without_rerunning(tmp_path, monkeypatch):
    task = tmp_path / "task"
    args = args_for(task)
    for name in ("runner", "save", "links", "roads"):
        path = getattr(args, name)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(name.encode())
    source = task.parent / "nav-teacher-v3/expanded-scenarios.jsonl"
    selected = [scenario(kind) for kind in ("point", "object")]
    evaluation.v3.write_selection(source, selected)
    monkeypatch.setattr(evaluation, "EXPECTED", {"point": 1, "object": 1})
    monkeypatch.setattr(evaluation, "frozen_cases", lambda _: (source, selected))
    monkeypatch.setattr(evaluation.campaign, "phase_start", lambda _: None)
    calls = []

    def run(self, name, commands, directory, seed):
        teacher = next(teacher for teacher in evaluation.TEACHERS if name.startswith(f"perception-eval-{teacher}-"))
        value = next(value for value in selected if directory.name == value["id"])
        write_witness(task, self.args, value, teacher, "gaze")
        write_episode(directory, value, teacher)
        calls.append((value["id"], teacher, seed))

    monkeypatch.setattr(evaluation.Worker, "scenario", run)
    options = SimpleNamespace(task=task, mode="gaze", parallel=3, budget_seconds=60)
    before = source.read_bytes()
    result = evaluation.run(options)
    assert result["episodes"] == 6
    assert len(calls) == 6
    assert {teacher for _, teacher, _ in calls} == set(evaluation.TEACHERS)
    assert result["by_kind"]["point"]["memory"]["successes"] == 1
    assert result["comparisons"]["object"]["memory_vs_oracle"]["conditional_success_fraction"] == 1
    evaluation.run(options)
    assert len(calls) == 6
    assert source.read_bytes() == before
    assert not list(task.rglob("samples.bin.gz"))
    args.runner.write_bytes(b"changed binary")
    with pytest.raises(ValueError, match="another binary"):
        evaluation.run(options)
    assert len(calls) == 6


def test_memory_followup_keeps_parent_inputs_and_original_comparison_untouched(tmp_path, monkeypatch):
    task, output = tmp_path / "nav-perception", tmp_path / "nav-perception/evaluation-water-1"
    args = args_for(task)
    for name in ("runner", "save", "links", "roads"):
        path = getattr(args, name)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(name.encode())
    original = task / "evaluation/metrics.json"
    original.parent.mkdir(parents=True)
    original.write_text("old three-teacher comparison\n")
    source = task.parent / "nav-teacher-v3/expanded-scenarios.jsonl"
    selected = [scenario(kind) for kind in ("point", "object")]
    evaluation.v3.write_selection(source, selected)
    before_source, before_original = source.read_bytes(), original.read_bytes()
    monkeypatch.setattr(evaluation, "EXPECTED", {"point": 1, "object": 1})
    monkeypatch.setattr(evaluation, "frozen_cases", lambda _: (source, selected))
    monkeypatch.setattr(evaluation.campaign, "phase_start", lambda _: None)
    calls = []

    def run(self, name, commands, directory, seed):
        assert self.args.runner == args.runner and self.args.save == args.save
        assert self.args.output.is_relative_to(output)
        value = next(value for value in selected if directory.name == value["id"])
        write_witness(task, self.args, value, "memory", "gaze", output)
        write_episode(directory, value, "memory", memory_version=3)
        calls.append(value["id"])

    monkeypatch.setattr(evaluation.Worker, "scenario", run)
    options = SimpleNamespace(task=task, output=output, mode="gaze", teachers=["memory"], memory_version=3,
                              parallel=1, budget_seconds=60)
    result = evaluation.run(options)
    assert result["episodes"] == 2 and len(calls) == 2
    assert result["settings"]["teachers"] == ["memory"]
    assert result["settings"]["navigation_memory_version"] == 3
    assert result["comparisons"] == {"point": {}, "object": {}}
    assert set(result["by_kind"]["object"]) == {"memory"}
    evaluation.run(options)
    assert len(calls) == 2
    assert source.read_bytes() == before_source and original.read_bytes() == before_original
    options.memory_version = 2
    with pytest.raises(ValueError, match="another binary"):
        evaluation.run(options)
    options.memory_version = 3
    options.teachers = ["memory", "v2"]
    with pytest.raises(ValueError, match="another binary"):
        evaluation.run(options)
    assert len(calls) == 2
