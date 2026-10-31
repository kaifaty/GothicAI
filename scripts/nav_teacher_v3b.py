#!/usr/bin/env python3
"""Evaluate teacher-v3b on the frozen memory-v4 inputs inside one gq batch.

The binary and assets live on the local runtime disk. Inputs, caches and complete
raw outputs live under --task on shared storage. This adapter neither records
NAVDEMO demonstrations nor submits jobs, trains policies or retries episodes.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import time


_SPEC = importlib.util.spec_from_file_location("nav_memory_v4", Path(__file__).with_name("nav_memory_v4.py"))
memory = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(memory)

MODES = ("journey", "journey-stranger", "familiar-local", "familiar-stranger")
INPUTS = ("expanded-scenarios.jsonl", "detour-val.jsonl", "fixtures.json", "places.json", "accepted-links.txt", "save.sav")
DISCOVERY = ("roads-events.jsonl", "roads-graph.json", "points-v3.jsonl")
_ARGS_FOR = memory.args_for


def trace_cases(task):
    cases = memory.frozen_cases(task)
    return [value["id"] for cohort in ("beyond-roads", "detour-val")
            for value in [row for row in cases if row["cohort"] == cohort][:2]]


def configure(options):
    """Keep the original experiment, changing only runtime paths and identity."""
    task, runtime, assets = options.task, options.runtime, options.assets

    def args_for(current_task, output, npc="BAU_954_MALETH"):
        if current_task != task:
            raise ValueError("Unexpected task directory")
        args = _ARGS_FOR(current_task, output, npc)
        args.runner = runtime / "build/opengothic/gothic-body-runner"
        args.assets = assets
        return args

    def identity(current_task, mode):
        if current_task != task or mode not in MODES:
            raise ValueError("Unexpected teacher-v3b comparison identity")
        return dict(experiment="teacher-v3b", mode=mode, teacher_version=3,
            limit_ms=30000 if options.phase == "safety" else memory.LIMIT_MS, physics="cv", perception_mode="gaze",
            runner_sha256=memory.digest(runtime / "build/opengothic/gothic-body-runner"),
            input_sha256={name: memory.digest(task / "input" / name) for name in INPUTS},
            discovery_sha256={name: memory.digest(task / "discovery" / name) for name in DISCOVERY},
            driver_sha256={name: memory.digest(Path(__file__).with_name(name))
                           for name in ("nav_teacher_v3b.py", "nav_memory_v4.py")},
            memory_cost_trace_case_ids=trace_cases(task) if options.phase == "evaluate" and mode.startswith("familiar-") else [],
            familiar_sha256=memory.digest(task / "input/familiar.bin") if mode.startswith("familiar-") else None)

    memory.args_for = args_for
    memory.identity = identity


def validate_safety_payload(task, mode, current):
    """Retain the physical and observed-water evidence for before/after review."""
    for fixture in memory.frozen_safety(task):
        directory = task / "safety" / mode / fixture["label"]
        if not memory.oracle.complete_text(directory, "point"):
            raise ValueError(f"Incomplete safety action/trajectory/path evidence: {directory}")
        metadata = memory.v3.read_json(directory / "episode.json")
        if metadata.get("memory_v4_comparison") != current:
            raise ValueError(f"Safety trial uses different binary or frozen inputs: {directory}")
        frames = directory / "perception-safety-frames.jsonl"
        water = directory / "perception-water-observations.jsonl"
        if not frames.is_file() or not water.is_file():
            raise ValueError(f"Missing raw physical/water observation evidence: {directory}")
        observations = list(memory.v3.rows(water))
        if (not observations or observations[0].get("elapsed_ms") != 0
                or any(b.get("elapsed_ms", -1) <= a.get("elapsed_ms", -1)
                       for a, b in zip(observations, observations[1:]))
                or any(not all(key in row for key in ("position_cm", "water_hit_rays", "near_water_cells",
                                                       "far_water_cells", "goal_line")) for row in observations)):
            raise ValueError(f"Incomplete raw water observation evidence: {directory}")


def run(options):
    configure(options)
    if options.phase == "cost":
        return measure_cost(options)
    mode = options.mode[0]
    current = memory.identity(options.task, mode)
    selected = current["memory_cost_trace_case_ids"]
    os.environ["OPENGOTHIC_NAV_MEMORY_TRACE"] = ",".join(selected) if selected else "0"
    if options.phase == "evaluate":
        memory.evaluate(options)
        for value in memory.frozen_cases(options.task):
            if value["id"] in selected:
                trace = options.task / "evaluation" / mode / value["cohort"] / value["id"] / "memory-cost.bin"
                if not trace.is_file() or trace.stat().st_size == 0:
                    raise ValueError(f"Missing paired memory-cost replay trace: {trace}")
    else:
        memory.evaluate_safety(options)
        validate_safety_payload(options.task, mode, current)
    if memory.identity(options.task, mode) != current:
        raise ValueError("Binary or frozen inputs changed during teacher-v3b measurement")


def cost_traces(task):
    selected = trace_cases(task)
    cases = [value for value in memory.frozen_cases(task) if value["id"] in selected]
    traces = []
    for mode in ("familiar-local", "familiar-stranger"):
        expected = dict(memory.identity(task, mode), memory_cost_trace_case_ids=selected)
        for value in cases:
            directory = task / "evaluation" / mode / value["cohort"] / value["id"]
            trace = directory / "memory-cost.bin"
            if not trace.is_file() or trace.stat().st_size == 0:
                raise ValueError(f"Missing paired memory-cost replay trace: {trace}")
            metadata = memory.v3.read_json(directory / "episode.json")
            memory.validate_raw(directory, metadata, value, mode)
            if metadata.get("memory_v4_comparison") != expected:
                raise ValueError(f"Paired replay uses different binary or frozen inputs: {directory}")
            traces.append(trace)
    if len(traces) != 8:
        raise ValueError(f"Expected eight preselected paired traces; got {len(traces)}")
    return traces


def cost_contract(before, cached, traces):
    """Both implementations must process exactly the same events and outputs."""
    results = {}
    for name, data in (("before", before), ("cached", cached)):
        captures = data["captures"]
        expected = {str(path) for path in traces}
        if (data.get("implementation") != name or data.get("repetitions") != 4
                or data.get("warmup_passes_per_mode") != 1 or len(captures) != len(traces)
                or {row["path"] for row in captures} != expected):
            raise ValueError(f"Paired benchmark identity differs: {name}")
        results[name] = {row["path"]: row for row in captures}
    for path in results["before"]:
        first, second = results["before"][path], results["cached"][path]
        for key in ("atlas_tiles", "personal_mask_words", "duration_ms", "trajectory_updates", "trajectory_views"):
            if first[key] != second[key]:
                raise ValueError(f"Paired replay event/input mismatch: {path}: {key}")
        for mode in ("journey", "familiar"):
            for key in ("updates", "views", "checksum"):
                if first[mode][key] != second[mode][key]:
                    raise ValueError(f"Cached replay differs from uncached replay: {path}: {mode}/{key}")


def aggregate_cost(captures):
    result = {}
    for mode in ("journey", "familiar"):
        rows = [row[mode] for row in captures]
        counts = {key: sum(row[key] for row in rows) for key in ("updates", "views")}
        result[mode] = dict(counts,
            update_us=sum(row["update_us"] * row["updates"] for row in rows) / counts["updates"],
            view_us=sum(row["view_us"] * row["views"] for row in rows) / counts["views"])
    return result


def measure_cost(options):
    task = options.task
    traces = cost_traces(task)
    directory = task / "paired-cost"
    directory.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + options.budget_seconds
    binaries = dict(cached=options.runtime / "build/navmemory-cost", before=options.runtime / "build/navmemory-cost-before")
    results = {name: dict(implementation=name, repetitions=4, warmup_passes_per_mode=1, captures=[])
               for name in binaries}
    for index, trace in enumerate(traces):
        for implementation in (("cached", "before") if index % 2 == 0 else ("before", "cached")):
            command = [str(binaries[implementation]), "--implementation", implementation, "--repetitions", "4", str(trace)]
            completed = subprocess.run(command, capture_output=True, text=True, timeout=max(.1, deadline - time.monotonic()))
            (directory / f"{index:02d}-{implementation}.json").write_text(completed.stdout)
            (directory / f"{index:02d}-{implementation}.stderr.txt").write_text(completed.stderr)
            completed.check_returncode()
            data = json.loads(completed.stdout)
            if (data.get("implementation") != implementation or data.get("repetitions") != 4
                    or data.get("warmup_passes_per_mode") != 1 or len(data["captures"]) != 1
                    or data["captures"][0]["path"] != str(trace)):
                raise ValueError(f"Paired benchmark invocation differs: {trace}: {implementation}")
            results[implementation]["captures"].extend(data["captures"])
    cost_contract(results["before"], results["cached"], traces)
    by_residency = {}
    for residency in ("familiar-local", "familiar-stranger"):
        aggregates = {name: aggregate_cost([row for row in data["captures"] if Path(row["path"]).parents[2].name == residency])
                      for name, data in results.items()}
        before, cached = (aggregates[name] for name in ("before", "cached"))
        update_ratios = {mode: cached[mode]["update_us"] / before[mode]["update_us"] for mode in ("journey", "familiar")}
        view_ratio = cached["familiar"]["view_us"] / cached["journey"]["view_us"]
        by_residency[residency] = dict(cached=cached, before=before, familiar_view_over_journey=view_ratio,
            cached_update_over_before=update_ratios, view_gate_passed=view_ratio <= 1.15,
            update_gate_passed=all(value <= 1.10 for value in update_ratios.values()))
    summary = dict(traces=[dict(path=str(path), mode=path.parents[2].name, cohort=path.parents[1].name,
                               id=path.parent.name, sha256=memory.digest(path)) for path in traces],
        benchmark_sha256={name: memory.digest(binary) for name, binary in binaries.items()},
        repetitions=4, warmup_passes_per_mode=1, paired_contract_passed=True,
        by_residency=by_residency,
        view_gate_passed=all(row["view_gate_passed"] for row in by_residency.values()),
        update_gate_passed=all(row["update_gate_passed"] for row in by_residency.values()))
    memory.v3.write_json(directory / "summary.json", summary)
    memory.emit("teacher_v3b_paired_cost_complete", **summary)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("evaluate", "safety", "cost"))
    parser.add_argument("--task", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--mode", choices=MODES)
    parser.add_argument("--parallel", type=int, choices=(1, 2, 3), default=3)
    parser.add_argument("--budget-seconds", type=float, default=6600)
    options = parser.parse_args()
    if (any(not getattr(options, name).is_absolute() for name in ("task", "runtime", "assets"))
            or not 0 < options.budget_seconds <= 6900):
        parser.error("Absolute task, runtime and assets paths and a wall budget in (0,6900] seconds are required")
    if (options.phase == "cost") != (options.mode is None):
        parser.error("--mode is required for evaluate/safety and must be omitted for cost")
    for name in ("task", "runtime", "assets"):
        setattr(options, name, getattr(options, name).resolve())
    if options.runtime.is_relative_to(options.task) or options.task.is_relative_to(options.runtime):
        parser.error("Runtime and shared task storage must be separate directories")
    if shutil.disk_usage(options.task).free < 20 * 1024**3:
        raise ValueError("Less than 20 GiB free before teacher-v3b measurement")
    options.mode = [options.mode] if options.mode else []
    run(options)


if __name__ == "__main__":
    main()
