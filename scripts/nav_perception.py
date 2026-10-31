#!/usr/bin/env python3
"""Paired eye/memory measurements on the frozen v3 poses; no demonstrations.

Run on Ryzen inside the caller's gq batch. All writes belong to --task.
The source experiment and its recordings are opened read-only.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import importlib.util
import json
import os
from pathlib import Path
from types import SimpleNamespace


def module(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + ".py"))
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


v3 = module("nav_teacher_v3")
coverage = module("nav_teacher_v3_coverage")
MODES = ("baseline", "gaze", "rows")
EXPECTED = {"point": 30, "object": 30, "follow": 18, "approach": 18}


def frozen_cases(source):
    path = source / "expanded-scenarios.jsonl"
    cases = list(v3.rows(path))
    if dict(Counter(case["goal_kind"] for case in cases)) != EXPECTED:
        raise ValueError("Expected the frozen 30/30/18/18 v3 cohort")
    if len({case["id"] for case in cases}) != len(cases):
        raise ValueError("Duplicate frozen scenario")
    return cases


def prepare(task, source, data):
    cases = frozen_cases(source)
    output = task / "replay"
    output.mkdir(parents=True, exist_ok=True)
    count = samples = 0
    with (output / "poses.txt").open("w", encoding="utf-8") as stream:
        for case in cases:
            cohort = "v3-retry" if case["goal_kind"] == "point" else "v3-visible"
            directory = data / cohort / case["goal_kind"] / case["split"] / case["id"]
            meta = v3.read_json(directory / "episode.json")
            if not v3.same_scenario(meta, case, 3):
                raise ValueError(f"Frozen episode identity mismatch: {directory}")
            wanted = set(coverage.sample_indices(meta["decisions"], case["id"], 20102026))
            written = 0
            previous = None
            for index, row in enumerate(v3.rows(directory / "teacher-paths.jsonl")):
                elapsed = int(row["elapsed_ms"])
                if previous is not None and elapsed - previous != 100:
                    raise ValueError(f"Non-100ms source decision interval: {directory}")
                previous = elapsed
                numbers = [elapsed, *row["position_cm"], row["yaw_deg"], *row["goal_cm"], int(index in wanted)]
                stream.write(case["id"] + " " + " ".join(format(value, ".12g") for value in numbers) + "\n")
                written += 1
                samples += index in wanted
            if written != meta["decisions"]:
                raise ValueError(f"Incomplete source trajectory: {directory}")
            count += written
    result = dict(scenarios=len(cases), updates=count, sampled_frames=samples,
                  cohort_sha256=hashlib.sha256((source / "expanded-scenarios.jsonl").read_bytes()).hexdigest(),
                  poses_sha256=hashlib.sha256((output / "poses.txt").read_bytes()).hexdigest(),
                  sampling="seed20102026 one decision in10 plus final; full100ms pose history",
                  recordings_created=False)
    v3.write_json(output / "preparation.json", result)
    print(json.dumps(dict(event="perception_prepared", **result)), flush=True)
    return result


def runner_args(task, output):
    return SimpleNamespace(output=output, runner=task / "build/opengothic/gothic-body-runner",
        assets=Path(os.environ.get("GOTHIC_DIR", Path.home() / "gothic-ai/game-settings")), save=task / "input/save.sav", cache=task / "cache",
        links=task / "input/accepted-links.txt", roads=task / "input/places.json", ini=None,
        npc="BAU_954_MALETH", seed=20102026, physics="cv", watchdog_seconds=5400)


def replay(task, modes=MODES, output=None):
    output = output or task / "replay"
    for mode in modes:
        directory = output / mode
        if (directory / "perception-timing.json").is_file():
            raise ValueError(f"Refusing to overwrite completed replay: {directory}")
        options = runner_args(task, output / (mode + "-worker"))
        v3.checked_paths(options)
        runner = v3.new_runner(options)
        try:
            print(json.dumps(dict(event="perception_replay_started", mode=mode)), flush=True)
            v3.runner_scenario(options, runner, "perception-" + mode,
                [f"navdemos perception-replay {task / 'replay/poses.txt'} {mode}"], directory, options.seed)
        finally:
            runner.close()
        if not (directory / "perception-timing.json").is_file():
            raise ValueError(f"Replay did not produce timings: {directory}")
        print(json.dumps(dict(event="perception_replay_finished", mode=mode)), flush=True)


def ratios(counts):
    def divide(numerator, denominator):
        return numerator / denominator if denominator else None
    return dict(unknown_passable=divide(counts.get("truth_passable_unknown", 0), counts.get("truth_passable", 0)),
                false_among_danger=divide(counts.get("false_danger", 0), counts.get("danger", 0)),
                detected_impassable=divide(counts.get("true_danger", 0), counts.get("truth_unpassable", 0)))


def summarize(task):
    """Aggregate only the frozen 1-Hz cohort; exclude extra diagnostic frames."""
    result = dict(modes={}, preparation=v3.read_json(task / "replay/preparation.json"))
    expected_frames = result["preparation"]["sampled_frames"]
    reference = None
    for mode in MODES:
        directory = task / "replay" / mode
        timing = v3.read_json(directory / "perception-timing.json")
        signatures, status, counts, band = [], Counter(), Counter(), Counter()
        for row in v3.rows(directory / "perception-frames.jsonl"):
            if not row["sampled"]:
                continue
            signatures.append((row["label"], row["elapsed_ms"], row["corridor_status"],
                               row["corridor"].get("cells", 0), row["corridor"].get("truth_passable", 0)))
            status[row["corridor_status"]] += 1
            counts.update(row["corridor"])
            band.update(row["forward_band"])
        if len(signatures) != expected_frames or timing["sampled_frames"] != expected_frames:
            raise ValueError(f"Incomplete frozen sampling: {mode}")
        if reference is not None and signatures != reference:
            raise ValueError(f"Unpaired truth geometry or sampled poses: {mode}")
        reference = signatures
        result["modes"][mode] = dict(status=dict(status), corridor=dict(counts), fractions=ratios(counts),
            forward_band=dict(band), timing=timing,
            ray_increase=timing["ray_count"] / 238 - 1)
    eligible = [name for name in ("gaze", "rows") if result["modes"][name]["ray_increase"] <= .25]
    if not eligible:
        raise ValueError("Neither candidate satisfies the ray budget")
    selected = min(eligible, key=lambda name: (result["modes"][name]["fractions"]["unknown_passable"],
        result["modes"][name]["fractions"]["false_among_danger"],
        -result["modes"][name]["fractions"]["detected_impassable"],
        result["modes"][name]["timing"]["eye"]["mean_us"]))
    result["selected"] = selected
    old = v3.read_json(task.parent / "nav-teacher-v3/oracle-experiment/coverage/metrics.json")["all"]["coverage"]["counts"]
    old["true_danger"] = old["danger"] - old["false_danger"]
    result["archived_memory_v1"] = dict(corridor=old, fractions=ratios(old))
    chosen = result["modes"][selected]["fractions"]
    result["acceptance"] = dict(unknown=chosen["unknown_passable"] <= .25,
        false_danger=chosen["false_among_danger"] <= .30,
        detected_impassable=chosen["detected_impassable"] >= ratios(old)["detected_impassable"])
    v3.write_json(task / "replay/summary.json", result)
    print(json.dumps(dict(event="perception_summarized", selected=selected, acceptance=result["acceptance"])), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("prepare", "replay", "summarize"))
    parser.add_argument("--task", type=Path, required=True)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--data", type=Path)
    parser.add_argument("--mode", choices=MODES, action="append")
    parser.add_argument("--output", type=Path, help="Separate absolute replay output; original poses remain read-only")
    args = parser.parse_args()
    if args.phase == "replay":
        os.environ.update(module("ryzen_game").game_environment())
    task = args.task.resolve()
    if args.output is not None and not args.output.is_absolute():
        parser.error("Replay output must be absolute")
    if args.phase == "prepare":
        prepare(task, (args.source or task.parent / "nav-teacher-v3").resolve(),
                (args.data or task.parent / "nav-demos-v3").resolve())
    elif args.phase == "replay":
        replay(task, args.mode or MODES, args.output)
    else:
        summarize(task)


if __name__ == "__main__":
    main()
