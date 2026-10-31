#!/usr/bin/env python3
"""Evaluate the frozen 30 points and 30 objects without recording demonstrations.

Run inside the caller's gq batch on Ryzen. Memory-v3, oracle-v3 and v2 use
the same binary, perception mode, CV physics, seeds and original 240-second
limit. Only the selected output directory is written; completed trials are
strictly resumed. A follow-up may select memory alone without touching the
original three-teacher comparison.
"""
from __future__ import annotations

import argparse
from collections import Counter
import importlib.util
from pathlib import Path
import time


def module(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + ".py"))
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


perception = module("nav_perception")
oracle = module("nav_teacher_v3_oracle")
campaign, v3 = oracle.campaign, oracle.v3
TEACHERS = ("memory", "oracle", "v2")
EXPECTED = {"point": 30, "object": 30}
LIMIT_MS = 240000
MEMORY_VERSION = 2


def atomic_json(path, value):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + ".tmp")
    v3.write_json(temporary, value)
    temporary.replace(path)


def frozen_cases(task):
    source = task.parent / "nav-teacher-v3/expanded-scenarios.jsonl"
    values = perception.frozen_cases(source.parent)
    selected = [value for value in values if value["goal_kind"] in EXPECTED]
    if dict(Counter(value["goal_kind"] for value in selected)) != EXPECTED:
        raise ValueError("Evaluation needs the frozen 30 points and 30 objects")
    for value in selected:
        for key in ("id", "split"):
            if Path(value[key]).name != value[key] or value[key] in (".", ".."):
                raise ValueError(f"Unsafe scenario {key}: {value[key]!r}")
        if not isinstance(value.get("seed"), int):
            raise ValueError(f"Missing frozen scenario seed: {value['id']}")
    v3.validate_splits(selected)
    return source, selected


def directory_for(task, value, teacher, output=None):
    if teacher not in TEACHERS:
        raise ValueError(f"Unknown teacher: {teacher}")
    return (output or task / "evaluation") / teacher / value["goal_kind"] / value["split"] / value["id"]


def settings(task, mode, source, teachers=TEACHERS, memory_version=MEMORY_VERSION):
    if mode not in ("gaze", "rows"):
        raise ValueError("Select gaze or rows perception")
    if not teachers or len(set(teachers)) != len(teachers) or any(teacher not in TEACHERS for teacher in teachers):
        raise ValueError("Select unique known evaluation teachers")
    args = perception.runner_args(task, task / "evaluation/workers/0")
    return dict(perception_mode=mode, navigation_memory_version=memory_version,
                runner_sha256=campaign.binary_digest(args.runner),
                cohort_sha256=campaign.binary_digest(source), source=str(source),
                input_sha256={name: campaign.binary_digest(getattr(args, name)) for name in ("save", "links", "roads")},
                assets=str(args.assets), physics="cv", limit_ms=LIMIT_MS,
                teachers=list(teachers), expected_by_kind=EXPECTED, demonstration_recorded=False)


def command_sequence(args, value, teacher, mode):
    if teacher not in TEACHERS or mode not in ("gaze", "rows"):
        raise ValueError("Unknown teacher or perception mode")
    return [f"roads load {args.roads}", f"roads on {args.npc}", "navdemos record off",
            f"navdemos perception-mode {mode}", f"navdemos oracle {'on' if teacher == 'oracle' else 'off'}",
            v3.start_command(value, 2 if teacher == "v2" else 3, args.npc, LIMIT_MS), "navdemos wait"]


def scenario_name(value, teacher):
    return f"perception-eval-{teacher}-{value['id']}"


def validate_raw(directory, metadata, value, teacher, mode, memory_version=MEMORY_VERSION):
    version = 2 if teacher == "v2" else 3
    expected_source = {"memory": "learner_memory", "oracle": "detour_raster", "v2": "detour_teacher_v2"}[teacher]
    if (metadata.get("teacher_version") != version or metadata.get("demo_version") != 2
            or any(metadata.get(key) != value.get(key) for key in ("start", "goal", "goal_kind"))
            or metadata.get("limit_ms") != LIMIT_MS or metadata.get("navigation_memory_version") != memory_version
            or metadata.get("perception_mode") != mode
            or metadata.get("oracle") is not (teacher == "oracle")
            or metadata.get("observation_source") != expected_source
            or metadata.get("record") is not False or metadata.get("demonstration_recorded") is not False
            or metadata.get("drive", False) or metadata.get("replay", False)
            or (directory / "samples.bin.gz").exists()):
        raise ValueError(f"Evaluation identity, representation or recording mode differs: {directory}")


def validate_witness(task, args, value, teacher, mode, output=None):
    expected = [f"seed {value['seed']}", "cheat god", "set time 10 0", "navdemos physics cv",
                *command_sequence(args, value, teacher, mode), "label done"]
    witnesses = sorted(((output or task / "evaluation") / "workers").glob(
        f"*/scenarios/{scenario_name(value, teacher)}.bench"))
    if not witnesses or any(path.read_text(encoding="utf-8").splitlines() != expected for path in witnesses):
        raise ValueError(f"Missing or different saved scenario command: {value['id']}/{teacher}")


class Worker(campaign.Worker):
    def __init__(self, args, identity):
        super().__init__(args, 3, None)
        self.identity = identity

    def trial(self, task, value, teacher, mode, output=None):
        directory = directory_for(task, value, teacher, output)
        episode = directory / "episode.json"
        resumed = episode.is_file()
        started = time.monotonic()
        campaign.emit("perception_eval_trial_started", teacher=teacher, perception_mode=mode,
                      kind=value["goal_kind"], id=value["id"], resumed=resumed)
        if not resumed:
            if directory.exists() and any(directory.iterdir()):
                raise ValueError(f"Incomplete prior trial; refusing to overwrite: {directory}")
            self.scenario(scenario_name(value, teacher), command_sequence(self.args, value, teacher, mode), directory, value["seed"])
        if not oracle.complete_text(directory, value["goal_kind"]):
            raise ValueError(f"Incomplete evaluation text files: {directory}")
        metadata = v3.read_json(episode)
        validate_raw(directory, metadata, value, teacher, mode,
                     self.identity.get("navigation_memory_version", MEMORY_VERSION))
        validate_witness(task, self.args, value, teacher, mode, output)
        if "id" in metadata:
            if (not v3.same_scenario(metadata, value, 2 if teacher == "v2" else 3)
                    or metadata.get("perception_evaluation_identity") != self.identity
                    or metadata.get("experiment_mode") != teacher):
                raise ValueError(f"Completed evaluation belongs to a different frozen trial: {directory}")
        else:
            # Native static output keeps scenarioSeed=0; the saved bench above
            # witnesses the frozen seed. Preserve it before attaching identity.
            metadata["runner_seed"] = metadata.get("seed")
            metadata.update({key: value[key] for key in v3.SCENARIO_IDENTITY_FIELDS if key in value})
            metadata.update(experiment_mode=teacher, perception_evaluation_identity=self.identity,
                            collector_wall_s=time.monotonic() - started)
            atomic_json(episode, metadata)
        measured = dict(oracle.text_metrics(directory), mode=teacher)
        campaign.emit("perception_eval_trial_complete", teacher=teacher, perception_mode=mode,
                      kind=value["goal_kind"], id=value["id"], success=metadata["success"],
                      elapsed_ms=metadata["elapsed_ms"], wall_s=time.monotonic() - started, resumed=resumed)
        return measured


def summarize(task, selected, identity, output=None):
    groups, comparisons, episodes = {}, {}, []
    for kind in EXPECTED:
        by_teacher = {}
        for teacher in identity["teachers"]:
            values = []
            for value in selected:
                if value["goal_kind"] != kind:
                    continue
                directory = directory_for(task, value, teacher, output)
                metadata = v3.read_json(directory / "episode.json")
                validate_raw(directory, metadata, value, teacher, identity["perception_mode"],
                             identity["navigation_memory_version"])
                if (not v3.same_scenario(metadata, value, 2 if teacher == "v2" else 3)
                        or metadata.get("perception_evaluation_identity") != identity
                        or metadata.get("experiment_mode") != teacher):
                    raise ValueError(f"Summary encountered a different trial: {directory}")
                values.append(dict(oracle.text_metrics(directory), mode=teacher))
            by_teacher[teacher] = values
            episodes.extend(values)
        groups[kind] = {teacher: v3.group_metrics(values) for teacher, values in by_teacher.items()}
        comparisons[kind] = {f"{trial}_vs_{reference}": oracle.paired_comparison(by_teacher[reference], by_teacher[trial])
                             for trial, reference in (("memory", "v2"), ("oracle", "v2"), ("memory", "oracle"))
                             if trial in by_teacher and reference in by_teacher}
    return dict(settings=identity, scenarios=len(selected), episodes=len(episodes), by_kind=groups,
                comparisons=comparisons, episode_metrics=episodes, complete=True, demonstration_recorded=False)


def run(options):
    task = options.task
    source, selected = frozen_cases(task)
    teachers = tuple(getattr(options, "teachers", TEACHERS))
    memory_version = getattr(options, "memory_version", MEMORY_VERSION)
    current = settings(task, options.mode, source, teachers, memory_version)
    output = getattr(options, "output", None) or task / "evaluation"
    output.mkdir(parents=True, exist_ok=True)
    state_path = output / "state.json"
    if state_path.exists():
        if v3.read_json(state_path).get("settings") != current:
            raise ValueError("Evaluation belongs to another binary, input, cohort, mode or memory representation")
    elif any(output.iterdir()):
        raise ValueError("Existing evaluation output has no identity state")
    if list(output.rglob("samples.bin.gz")):
        raise ValueError("Evaluation output contains forbidden NAVDEMO training recordings")
    atomic_json(state_path, dict(settings=current, status="collecting"))
    deadline = time.monotonic() + options.budget_seconds
    workers = []
    for index in range(options.parallel):
        args = perception.runner_args(task, output / "workers" / str(index))
        args.limit_ms = LIMIT_MS
        args.watchdog_seconds = 900
        workers.append(Worker(args, current))
    try:
        for teacher in teachers:
            for kind in EXPECTED:
                campaign.phase_start(f"perception-eval-{teacher}-{kind}")
                values = [value for value in selected if value["goal_kind"] == kind]
                campaign.run_parallel(values, workers,
                    lambda worker, value: worker.trial(task, value, teacher, options.mode, output), deadline)
                atomic_json(state_path, dict(settings=current, status=f"completed_{teacher}_{kind}"))
    finally:
        for worker in workers:
            worker.close()
    if settings(task, options.mode, source, teachers, memory_version) != current:
        raise ValueError("Evaluation binary or frozen inputs changed during collection")
    result = summarize(task, selected, current, output)
    atomic_json(output / "metrics.json", result)
    atomic_json(state_path, dict(settings=current, status="complete"))
    campaign.emit("perception_evaluation_complete", episodes=result["episodes"], by_kind=result["by_kind"])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--task", type=Path, required=True)
    parser.add_argument("--output", type=Path, help="Absolute output directory; default TASK/evaluation")
    parser.add_argument("--teachers", nargs="+", choices=TEACHERS, default=list(TEACHERS),
                        help="Teachers to evaluate with the same frozen cases (default: all three)")
    parser.add_argument("--memory-version", type=int, choices=(2, 3), default=MEMORY_VERSION,
                        help="Required native navigation memory representation (default: historical 2)")
    parser.add_argument("--mode", choices=("gaze", "rows"), required=True)
    parser.add_argument("--parallel", type=int, choices=(1, 2, 3), default=3)
    parser.add_argument("--budget-seconds", type=float, default=6900)
    options = parser.parse_args()
    if not options.task.is_absolute() or not 0 < options.budget_seconds <= 7200:
        parser.error("An absolute task path and a wall budget in (0,7200] seconds are required")
    options.task = options.task.resolve()
    if options.output is not None:
        if not options.output.is_absolute():
            parser.error("Evaluation output path must be absolute")
        options.output = options.output.resolve()
    run(options)


if __name__ == "__main__":
    main()
