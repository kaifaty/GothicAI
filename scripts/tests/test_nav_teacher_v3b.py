"""Contracts for the teacher-v3b measurement adapter and retained evidence."""
import importlib.util
import json
from pathlib import Path
import subprocess
from types import SimpleNamespace

import pytest


SPEC = importlib.util.spec_from_file_location("nav_teacher_v3b", Path(__file__).resolve().parents[1] / "nav_teacher_v3b.py")
measurement = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(measurement)


def selected_cases():
    return [dict(id=f"{cohort}-{index}", cohort=cohort) for cohort in ("beyond-roads", "detour-val") for index in range(3)]


def test_runtime_binary_and_assets_are_separate_from_shared_inputs_and_outputs(tmp_path, monkeypatch):
    task, runtime, assets = (tmp_path / name for name in ("shared", "runtime", "game"))
    for name in measurement.INPUTS + ("familiar.bin",):
        path = task / "input" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(name)
    for name in measurement.DISCOVERY:
        path = task / "discovery" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(name)
    binary = runtime / "build/opengothic/gothic-body-runner"
    binary.parent.mkdir(parents=True)
    binary.write_bytes(b"teacher-v3b")
    monkeypatch.setattr(measurement.memory, "frozen_cases", lambda _: selected_cases())
    options = SimpleNamespace(task=task, runtime=runtime, assets=assets, phase="evaluate")
    measurement.configure(options)
    args = measurement.memory.args_for(task, task / "evaluation/worker", "LOCAL")
    assert args.runner == binary and args.assets == assets
    assert args.output == task / "evaluation/worker" and args.npc == "LOCAL"
    assert args.cache == task / "cache" and args.save == task / "input/save.sav"
    identity = measurement.memory.identity(task, "familiar-local")
    assert identity["memory_cost_trace_case_ids"] == ["beyond-roads-0", "beyond-roads-1", "detour-val-0", "detour-val-1"]
    assert set(identity["driver_sha256"]) == {"nav_teacher_v3b.py", "nav_memory_v4.py"}
    binary.write_bytes(b"changed teacher")
    assert measurement.memory.identity(task, "familiar-local")["runner_sha256"] != identity["runner_sha256"]
    (task / "input/detour-val.jsonl").write_text("changed input")
    assert measurement.memory.identity(task, "familiar-local")["input_sha256"] != identity["input_sha256"]
    (task / "discovery/roads-graph.json").write_text("changed resident selection")
    assert measurement.memory.identity(task, "familiar-local")["discovery_sha256"] != identity["discovery_sha256"]
    journey = measurement.memory.identity(task, "journey")
    assert journey["familiar_sha256"] is None and journey["memory_cost_trace_case_ids"] == []


@pytest.mark.parametrize("mode", measurement.MODES)
def test_trace_allowlist_is_fixed_before_parallel_evaluation(tmp_path, monkeypatch, mode):
    cases = selected_cases()
    selected = [row["id"] for row in cases if not row["id"].endswith("2")] if mode.startswith("familiar-") else []
    monkeypatch.setattr(measurement, "configure", lambda _: None)
    monkeypatch.setattr(measurement.memory, "identity", lambda *_: dict(memory_cost_trace_case_ids=selected))
    monkeypatch.setattr(measurement.memory, "frozen_cases", lambda _: cases)
    monkeypatch.setenv("OPENGOTHIC_NAV_MEMORY_TRACE", "inherited setting")
    invoked = []

    def evaluate(options):
        invoked.append(options)
        assert measurement.os.environ["OPENGOTHIC_NAV_MEMORY_TRACE"] == (",".join(selected) if selected else "0")
        for row in cases:
            if row["id"] in selected:
                trace = tmp_path / "evaluation" / mode / row["cohort"] / row["id"] / "memory-cost.bin"
                trace.parent.mkdir(parents=True)
                trace.write_bytes(b"native replay trace")

    monkeypatch.setattr(measurement.memory, "evaluate", evaluate)
    options = SimpleNamespace(task=tmp_path, mode=[mode], phase="evaluate")
    measurement.run(options)
    assert invoked == [options]


def test_missing_selected_trace_stops_without_replaying_evaluation(tmp_path, monkeypatch):
    monkeypatch.setattr(measurement, "configure", lambda _: None)
    monkeypatch.setattr(measurement.memory, "identity", lambda *_: dict(memory_cost_trace_case_ids=["beyond-roads-0"]))
    monkeypatch.setattr(measurement.memory, "frozen_cases", lambda _: selected_cases())
    invoked = []
    monkeypatch.setattr(measurement.memory, "evaluate", invoked.append)
    options = SimpleNamespace(task=tmp_path, mode=["familiar-local"], phase="evaluate")
    with pytest.raises(ValueError, match="Missing paired memory-cost replay trace"):
        measurement.run(options)
    assert invoked == [options]


@pytest.fixture
def safety_payload(tmp_path, monkeypatch):
    directory = tmp_path / "safety/journey/water-shore"
    directory.mkdir(parents=True)
    identity = dict(experiment="teacher-v3b", runner_sha256="frozen binary")
    (directory / "episode.json").write_text(json.dumps(dict(memory_v4_comparison=identity)))
    for name in ("actions.txt", "trajectory.txt", "teacher-paths.jsonl", "perception-safety-frames.jsonl"):
        (directory / name).write_text("retained raw payload\n")
    observations = [dict(elapsed_ms=elapsed, position_cm=[0, 0, 0], water_hit_rays=[], near_water_cells=[],
                         far_water_cells=0, goal_line=[]) for elapsed in (0, 100)]
    (directory / "perception-water-observations.jsonl").write_text("".join(json.dumps(row) + "\n" for row in observations))
    monkeypatch.setattr(measurement.memory, "frozen_safety", lambda _: [dict(label="water-shore")])
    return tmp_path, directory, identity


def test_safety_retains_every_raw_evidence_file_without_modification(safety_payload):
    task, directory, identity = safety_payload
    original = {path.name: path.read_bytes() for path in directory.iterdir()}
    measurement.validate_safety_payload(task, "journey", identity)
    assert {path.name: path.read_bytes() for path in directory.iterdir()} == original


@pytest.mark.parametrize("missing", ("actions.txt", "trajectory.txt", "teacher-paths.jsonl",
                                    "perception-safety-frames.jsonl", "perception-water-observations.jsonl"))
def test_safety_requires_physical_water_and_planner_evidence(safety_payload, missing):
    task, directory, identity = safety_payload
    (directory / missing).unlink()
    with pytest.raises(ValueError, match="evidence"):
        measurement.validate_safety_payload(task, "journey", identity)


def test_safety_rejects_missing_observation_fields_and_changed_identity(safety_payload):
    task, directory, identity = safety_payload
    with pytest.raises(ValueError, match="different binary or frozen inputs"):
        measurement.validate_safety_payload(task, "journey", dict(identity, runner_sha256="changed"))
    (directory / "perception-water-observations.jsonl").write_text('{"elapsed_ms":0}\n')
    with pytest.raises(ValueError, match="raw water observation evidence"):
        measurement.validate_safety_payload(task, "journey", identity)


def test_cost_uses_identical_traces_alternates_implementations_and_keeps_local_gate_separate(tmp_path, monkeypatch):
    traces = []
    for mode in ("familiar-local", "familiar-stranger"):
        for cohort in ("beyond-roads", "detour-val"):
            for index in range(2):
                path = tmp_path / "evaluation" / mode / cohort / f"case-{index}" / "memory-cost.bin"
                path.parent.mkdir(parents=True)
                path.write_bytes(b"frozen observed inputs")
                traces.append(path)
    runtime = tmp_path / "runtime"
    (runtime / "build").mkdir(parents=True)
    for name in ("navmemory-cost", "navmemory-cost-before"):
        (runtime / "build" / name).write_bytes(name.encode())
    monkeypatch.setattr(measurement, "cost_traces", lambda _: traces)
    calls = []

    def replay(command, **kwargs):
        implementation, trace = command[2], Path(command[-1])
        calls.append((implementation, trace))
        assert command[3:5] == ["--repetitions", "4"] and kwargs["timeout"] > 0
        count = traces.index(trace) + 1
        local = trace.parents[2].name == "familiar-local"
        timing = dict(update_us=10.5 if implementation == "cached" else 10., updates=count * 4,
                      view_us=100., views=count * 8, checksum=count * 123)
        familiar = dict(timing, view_us=(120. if local else 90.) if implementation == "cached" else 200.)
        capture = dict(path=str(trace), atlas_tiles=100 if local else 0, personal_mask_words=2 if local else 0,
                       duration_ms=1000, trajectory_updates=count, trajectory_views=count * 2,
                       journey=timing, familiar=familiar)
        payload = dict(implementation=implementation, repetitions=4, warmup_passes_per_mode=1, captures=[capture])
        return subprocess.CompletedProcess(command, 0, json.dumps(payload), "")

    monkeypatch.setattr(measurement.subprocess, "run", replay)
    options = SimpleNamespace(task=tmp_path, runtime=runtime, budget_seconds=60)
    summary = measurement.measure_cost(options)
    assert calls == [(name, trace) for index, trace in enumerate(traces)
                     for name in (("cached", "before") if index % 2 == 0 else ("before", "cached"))]
    assert summary["paired_contract_passed"] and summary["update_gate_passed"]
    assert summary["by_residency"]["familiar-local"]["familiar_view_over_journey"] == pytest.approx(1.2)
    assert not summary["view_gate_passed"]
    assert summary["by_residency"]["familiar-stranger"]["view_gate_passed"]
    assert len(summary["traces"]) == 8
    assert (tmp_path / "paired-cost/summary.json").is_file()
    assert len(list((tmp_path / "paired-cost").glob("??-*.json"))) == 16


def test_cost_rejects_cache_output_differences(tmp_path):
    trace = tmp_path / "memory-cost.bin"
    timing = dict(updates=4, views=4, checksum=123)
    capture = dict(path=str(trace), atlas_tiles=1, personal_mask_words=1, duration_ms=100,
                   trajectory_updates=1, trajectory_views=1, journey=timing, familiar=timing)
    before = dict(implementation="before", repetitions=4, warmup_passes_per_mode=1, captures=[capture])
    cached = dict(implementation="cached", repetitions=4, warmup_passes_per_mode=1,
                  captures=[dict(capture, familiar=dict(timing, checksum=124))])
    with pytest.raises(ValueError, match="Cached replay differs"):
        measurement.cost_contract(before, cached, [trace])
