#!/usr/bin/env python3
"""One seeded evaluation: v2, recorded memory-v3, and eval-only oracle-v3.

Execute inside one caller-owned gq batch. Existing DATA recordings are read-only.
Complete experimental outputs are reused; only missing v2 and oracle episodes
run. Outputs belong to TASK/oracle-experiment and never contain NAVDEMO samples.
"""
from __future__ import annotations

import argparse
from collections import Counter
from pathlib import Path
import statistics
import subprocess
import time

import numpy as np

import importlib.util


def module(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(f"{name}.py"))
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


campaign = module("nav_teacher_v3_campaign")
v3 = campaign.v3
MODES = ("v2", "memory", "oracle")
EXPECTED = dict(point=30, object=30, follow=18, approach=18)


def directory_for(task, data, value, mode):
    if mode == "memory":
        cohort = "v3-retry" if value["goal_kind"] == "point" else "v3-visible"
        return campaign.cohort_directory(data, value, cohort)
    if mode not in ("v2", "oracle"):
        raise ValueError(f"Unknown experimental mode: {mode}")
    return Path(task) / "oracle-experiment" / mode / value["goal_kind"] / value["split"] / value["id"]


def complete_text(directory, kind):
    names = ["episode.json", "actions.txt", "trajectory.txt", "teacher-paths.jsonl"]
    if kind in ("follow", "approach"):
        names.append("target-trajectory.txt")
    return all((Path(directory) / name).is_file() for name in names)


def text_metrics(directory):
    """Use the same 100-ms actions, XZ length and target-band math in all modes."""
    directory = Path(directory)
    metadata = v3.read_json(directory / "episode.json")
    count, elapsed = metadata["decisions"], metadata["elapsed_ms"]
    actions = np.loadtxt(directory / "actions.txt", ndmin=2)
    trajectory = np.loadtxt(directory / "trajectory.txt", ndmin=2)
    if (not isinstance(count, int) or count <= 0 or actions.shape != (count + 1, 9)
            or trajectory.shape != (count + 1, 5) or not np.isfinite(actions).all() or not np.isfinite(trajectory).all()):
        raise ValueError(f"Invalid evaluation action/trajectory shape: {directory}")
    times = actions[:, 0]
    if (times[0] != 0 or not np.array_equal(times, trajectory[:, 0]) or not np.array_equal(times, np.floor(times))
            or not (np.diff(times[:-1]) == 100).all() or not 0 < times[-1] - times[-2] <= 100 or times[-1] != elapsed):
        raise ValueError(f"Evaluation action/trajectory times do not align: {directory}")
    if not np.array_equal(actions[-1, 1:], [0, 0, 0, 1, 0, 0, 0, 0]):
        raise ValueError(f"Evaluation terminal action did not release keys: {directory}")
    for column in (1, 2, 3):
        if (np.abs(actions[:, column]) > 1).any():
            raise ValueError(f"Evaluation action axis outside [-1,1]: {directory}")
    for column in (4, 5, 6):
        if not np.isin(actions[:, column], [0, 1]).all():
            raise ValueError(f"Evaluation action bit is not binary: {directory}")
    applied = actions[:-1]
    length = float(np.linalg.norm(np.diff(trajectory[:, [1, 3]], axis=0), axis=1).sum())
    initial_detour = metadata.get("detour_length_cm")
    fields = ("id", "goal_kind", "split", "region", "place_id", "start", "goal", "teacher_version", "success", "elapsed_ms",
              "decisions", "blocked_spans", "blocked_ms", "max_blocked_ms", "failure", "limit_ms", "seed", "final_dead", "final_down")
    result = {key: metadata.get(key) for key in fields}
    result.update(episode=str(directory), travelled_cm=length, detour_length_cm=initial_detour,
                  length_over_detour=length / initial_detour if initial_detour and initial_detour > 0 else None,
                  backward=int(np.count_nonzero(applied[:, 1] < 0)), strafe=int(np.count_nonzero(applied[:, 2] != 0)),
                  strafe_left=int(np.count_nonzero(applied[:, 2] < 0)), strafe_right=int(np.count_nonzero(applied[:, 2] > 0)),
                  forward=int(np.count_nonzero(applied[:, 1] > 0)),
                  turn_only=int(np.count_nonzero((applied[:, 1:3] == 0).all(axis=1) & (applied[:, 3] != 0))),
                  walk=int(np.count_nonzero(applied[:, 4] != 0)), run=int(np.count_nonzero(applied[:, 4] == 0)),
                  jump=int(np.count_nonzero(applied[:, 5] != 0)), climb=int(np.count_nonzero(applied[:, 6] != 0)),
                  oracle_unsupported_decisions=metadata.get("oracle_unsupported_decisions"),
                  start_not_found_decisions=metadata.get("start_not_found_decisions"),
                  oracle_snapped_support_decisions=metadata.get("oracle_snapped_support_decisions"),
                  oracle_support_snap_max_cm=metadata.get("oracle_support_snap_max_cm"),
                  oracle_support_horizontal_max_cm=metadata.get("oracle_support_horizontal_max_cm"),
                  oracle_support_vertical_abs_max_cm=metadata.get("oracle_support_vertical_abs_max_cm"),
                  oracle=metadata.get("oracle", False), observation_source=metadata.get("observation_source", "learner_memory"),
                  contract_records=0, metric_source="text/actions+physical-trajectory; no experimental NAVDEMO")
    if result["goal_kind"] in ("follow", "approach"):
        trace = np.loadtxt(directory / "target-trajectory.txt", ndmin=2)
        if trace.shape[1] < 5 or not np.isfinite(trace).all():
            raise ValueError(f"Invalid target trajectory: {directory}")
        result.update(v3.follow_metrics(trace[:, 0], trace[:, 4], elapsed))
        result["approach_success"] = bool((trace[:, 4] <= 200).any() or
            (metadata["success"] and metadata.get("distance_cm", float("inf")) <= 200))
        result.update(native_follow_ms=metadata.get("follow_ms"), native_in_band_ms=metadata.get("in_band_ms"),
                      native_follow_fraction=metadata.get("follow_fraction"), native_first_contact_ms=metadata.get("first_contact_ms"))
    return result


def validate_identity(directory, value, mode, limit_ms, experimental=False):
    directory = Path(directory)
    version = 2 if mode == "v2" else 3
    metadata = v3.read_json(directory / "episode.json")
    if not v3.same_scenario(metadata, value, version) or metadata.get("limit_ms") != limit_ms:
        raise ValueError(f"Evaluation identity, seed or time limit differs: {directory}")
    if mode == "oracle":
        if metadata.get("oracle") is not True or metadata.get("observation_source") != "detour_raster":
            raise ValueError(f"Oracle adapter was not enabled: {directory}")
    elif metadata.get("oracle", False):
        raise ValueError(f"Oracle observation leaked into a reference mode: {directory}")
    if experimental:
        if ((directory / "samples.bin.gz").exists() or metadata.get("record") is not False
                or metadata.get("demonstration_recorded") is not False):
            raise ValueError(f"Experimental evaluation created training samples: {directory}")
    return metadata


def preflight(options):
    selected = list(v3.rows(options.task / "expanded-scenarios.jsonl"))
    if dict(Counter(value["goal_kind"] for value in selected)) != EXPECTED or len({value["id"] for value in selected}) != 96:
        raise ValueError("Oracle experiment requires exactly the frozen 30/30/18/18 scenario IDs")
    v3.validate_splits(selected)
    for value in selected:
        for mode in (("memory", "v2") if value["goal_kind"] in ("point", "object") else ("memory",)):
            directory = (v3.episode_directory(options.data, value, 2) if mode == "v2"
                         else directory_for(options.task, options.data, value, mode))
            if not complete_text(directory, value["goal_kind"]):
                raise ValueError(f"Missing unchanged {mode} reference: {directory}")
            metadata = validate_identity(directory, value, mode, options.limit_ms)
            text_metrics(directory)
            if mode == "v2":
                if Path(value["teacher_v2_episode"]).resolve() != directory.resolve() or metadata.get("success") is not True:
                    raise ValueError(f"Static v2 reference must be the existing successful DATA/v2 take: {directory}")
    return selected


class ExperimentBudget(campaign.StorageBudget):
    def used(self):
        total = super().used()
        experiment = self.task / "oracle-experiment"
        return total + (0 if experiment.is_relative_to(self.data) else campaign.tree_bytes(experiment))


def command_sequence(args, value, mode):
    if mode not in ("v2", "oracle"):
        raise ValueError("Only v2 and oracle evaluation episodes may run")
    return [f"roads load {args.roads}", f"roads on {args.npc}", "navdemos record off",
            f"navdemos oracle {'on' if mode == 'oracle' else 'off'}",
            v3.start_command(value, 2 if mode == "v2" else 3, args.npc, args.limit_ms), "navdemos wait"]


def validate_raw_identity(args, directory, metadata, value, mode):
    """Moving goals are runtime state; the saved command fixes their scenario.

    The runner resolves freepoints to connected waypoints and moves the target.
    Neither its goal name nor target_position identifies the moving experiment.
    Enriched metadata still goes through the unchanged, strict same_scenario.
    """
    version = 2 if mode == "v2" else 3
    moving = value["goal_kind"] in ("follow", "approach")
    fields = ("start", "goal_kind") if moving else ("start", "goal", "goal_kind")
    if (metadata.get("teacher_version") != version or metadata.get("demo_version") != 2
            or any(metadata.get(key) != value.get(key) for key in fields)
            or metadata.get("limit_ms") != args.limit_ms
            or (moving and metadata.get("seed") != value["seed"])):
        raise ValueError(f"Raw experimental episode identity differs: {directory}")
    if not moving:
        return
    # Raw native output normally lacks these fields. Never overwrite a
    # conflicting scenario if an interrupted collector already added some.
    for key in v3.SCENARIO_IDENTITY_FIELDS:
        if key != "goal" and key in metadata and metadata[key] != value.get(key):
            raise ValueError(f"Raw moving scenario {key} differs: {directory}")
    if (metadata.get("oracle") is not (mode == "oracle")
            or (mode == "oracle" and metadata.get("observation_source") != "detour_raster")
            or metadata.get("record") is not False or metadata.get("demonstration_recorded") is not False):
        raise ValueError(f"Raw moving experimental mode differs: {directory}")
    expected = [f"seed {value['seed']}", "cheat god", "set time 10 0",
                f"navdemos physics {args.physics}", *command_sequence(args, value, mode), "label done"]
    # A resumed take may be assigned to a different worker. All matching
    # command witnesses must agree, rather than selecting a convenient one.
    experiment = Path(args.output).parent.parent
    witnesses = sorted((experiment / "workers").glob(f"*/scenarios/oracle-experiment-{mode}-{value['id']}.bench"))
    if not witnesses or any(path.read_text(encoding="utf-8").splitlines() != expected for path in witnesses):
        raise ValueError(f"Missing or different saved moving scenario command: {directory}")


class Worker(campaign.Worker):
    def trial(self, value, directory, mode):
        directory = Path(directory)
        if (directory / "samples.bin.gz").exists():
            raise ValueError(f"Refusing a training recording in the oracle experiment: {directory}")
        if (directory / "episode.json").is_file():
            if not complete_text(directory, value["goal_kind"]):
                raise ValueError(f"Completed experimental episode has missing text files: {directory}")
            metadata = v3.read_json(directory / "episode.json")
            recovered_raw = not metadata.get("id")
            if recovered_raw:
                validate_raw_identity(self.args, directory, metadata, value, mode)
                if value["goal_kind"] in ("follow", "approach"):
                    metadata["runner_goal"] = metadata.get("goal")
                metadata.update(value, experiment_mode=mode)
                v3.write_json(directory / "episode.json", metadata)
            validate_identity(directory, value, mode, self.args.limit_ms, experimental=True)
            measured = text_metrics(directory)
            campaign.emit("oracle_trial_resumed", mode=mode, kind=value["goal_kind"], id=value["id"],
                          success=metadata["success"], recovered_raw=recovered_raw)
            return measured
        with self.budget.reserve(self.args.limit_ms):
            started = time.monotonic()
            campaign.emit("oracle_trial_started", mode=mode, kind=value["goal_kind"], id=value["id"])
            self.scenario(f"oracle-experiment-{mode}-{value['id']}", command_sequence(self.args, value, mode), directory, value["seed"])
            if not complete_text(directory, value["goal_kind"]):
                raise ValueError(f"Incomplete experimental evaluation: {directory}")
            metadata = v3.read_json(directory / "episode.json")
            validate_raw_identity(self.args, directory, metadata, value, mode)
            if value["goal_kind"] in ("follow", "approach"):
                metadata["runner_goal"] = metadata.get("goal")
            metadata.update(value, experiment_mode=mode, collector_wall_s=time.monotonic() - started)
            v3.write_json(directory / "episode.json", metadata)
            validate_identity(directory, value, mode, self.args.limit_ms, experimental=True)
            measured = text_metrics(directory)
            campaign.emit("oracle_trial_complete", mode=mode, kind=value["goal_kind"], id=value["id"], success=metadata["success"],
                          elapsed_ms=metadata["elapsed_ms"], wall_s=time.monotonic() - started)
            return measured


def average(values):
    present = [value for value in values if value is not None]
    return statistics.mean(present) if present else None


def paired_comparison(reference, trials):
    refs = {value["id"]: value for value in reference}
    measured = {value["id"]: value for value in trials}
    if refs.keys() != measured.keys() or len(refs) != len(reference) or len(measured) != len(trials):
        raise ValueError("Experimental paired comparison needs identical, unique scenario IDs")
    eligible = [key for key in refs if refs[key]["success"]]
    joint = [key for key in eligible if measured[key]["success"]]
    return dict(paired=len(refs), reference_successes=len(eligible), trial_successes=sum(value["success"] for value in trials),
                trial_successes_where_reference_succeeded=len(joint),
                conditional_success_fraction=len(joint) / len(eligible) if eligible else None,
                elapsed_ratio_on_joint_success=average([measured[key]["elapsed_ms"] / refs[key]["elapsed_ms"] for key in joint if refs[key]["elapsed_ms"] > 0]),
                travelled_ratio_on_joint_success=average([measured[key]["travelled_cm"] / refs[key]["travelled_cm"] for key in joint if refs[key]["travelled_cm"] > 0]))


def summarize(selected, options, metrics_fn=None):
    metrics_fn = metrics_fn or text_metrics
    episodes, groups, comparisons = [], {}, {}
    for kind in v3.KINDS:
        groups[kind] = {}
        by_mode = {}
        for mode in MODES:
            values = []
            for value in selected:
                if value["goal_kind"] != kind:
                    continue
                directory = directory_for(options.task, options.data, value, mode)
                if not complete_text(directory, kind):
                    raise ValueError(f"Incomplete {mode}/{kind} experimental comparison: {directory}")
                validate_identity(directory, value, mode, options.limit_ms, experimental=mode in ("oracle", "v2"))
                measured = dict(metrics_fn(directory), mode=mode)
                values.append(measured)
            by_mode[mode] = values
            episodes.extend(values)
            groups[kind][mode] = v3.group_metrics(values)
            groups[kind][mode].update(jump=sum(value["jump"] for value in values), climb=sum(value["climb"] for value in values),
                                      episodes_with_jump_or_climb=sum(value["jump"] > 0 or value["climb"] > 0 for value in values))
            for field in ("oracle_unsupported_decisions", "start_not_found_decisions", "oracle_snapped_support_decisions"):
                available = [value for value in values if value.get(field) is not None]
                groups[kind][mode][field] = sum(value[field] for value in available) if available else None
                groups[kind][mode][field + "_episodes_available"] = len(available)
                groups[kind][mode][field + "_decisions_available"] = sum(value["decisions"] for value in available)
            for field in ("oracle_support_snap_max_cm", "oracle_support_horizontal_max_cm", "oracle_support_vertical_abs_max_cm"):
                available = [value[field] for value in values if value.get(field) is not None]
                groups[kind][mode][field] = max(available) if available else None
        comparisons[kind] = {"memory_vs_v2": paired_comparison(by_mode["v2"], by_mode["memory"]),
                             "oracle_vs_v2": paired_comparison(by_mode["v2"], by_mode["oracle"]),
                             "oracle_vs_memory": paired_comparison(by_mode["memory"], by_mode["oracle"])}
        if kind == "follow":
            for comparison, numerator, denominator in (("memory_vs_v2", "memory", "v2"), ("oracle_vs_v2", "oracle", "v2"),
                                                        ("oracle_vs_memory", "oracle", "memory")):
                a, b = groups[kind][numerator]["follow_fraction"], groups[kind][denominator]["follow_fraction"]
                comparisons[kind][comparison]["follow_fraction_ratio"] = a / b if a is not None and b is not None and b > 0 else None
    transition_slice = {kind: dict(v2_episodes_with_jump_or_climb=groups[kind]["v2"]["episodes_with_jump_or_climb"])
                        for kind in v3.KINDS}
    archived = {kind: v3.group_metrics([metrics_fn(v3.episode_directory(options.data, value, 2)) for value in selected
                                       if value["goal_kind"] == kind]) for kind in ("point", "object")}
    return dict(scenarios=len(selected), episodes=len(episodes), expected_by_kind=EXPECTED, by_kind=groups, comparisons=comparisons,
                transition_action_slice=transition_slice, archived_v2_static=archived,
                episode_metrics=episodes, complete=True,
                interpretation="Oracle changes only the teacher observation map. Eval-only outputs are not a demonstration corpus; no learner was trained.")


def settings(options):
    return dict(selection_sha256=campaign.binary_digest(options.task / "expanded-scenarios.jsonl"),
                runner_sha256=campaign.binary_digest(options.task / "build/opengothic/gothic-body-runner"),
                limit_ms=options.limit_ms, modes=list(MODES), expected_by_kind=EXPECTED)


def run(options):
    selected = preflight(options)
    experiment = options.task / "oracle-experiment"
    experiment.mkdir(parents=True, exist_ok=True)
    state_path = experiment / "state.json"
    current = settings(options)
    if state_path.exists() and v3.read_json(state_path).get("settings") != current:
        raise ValueError("Oracle experiment belongs to another binary, time limit or frozen selection")
    v3.write_json(state_path, dict(settings=current, status="collecting"))
    deadline = time.monotonic() + options.budget_seconds
    budget = ExperimentBudget(options.task, options.data)
    workers = []
    for index in range(options.parallel):
        args = campaign.worker_args(options, 3, index)
        args.output = experiment / "workers" / str(index)
        workers.append(Worker(args, 3, budget))
    try:
        phases = [("oracle", "point"), ("v2", "point"), ("oracle", "object"), ("v2", "object"), ("v2", "follow"),
                  ("oracle", "follow"), ("v2", "approach"), ("oracle", "approach")]
        for mode, kind in phases:
            campaign.phase_start(f"oracle-experiment-{mode}-{kind}")
            values = [value for value in selected if value["goal_kind"] == kind]
            campaign.emit("oracle_phase_started", mode=mode, kind=kind, scenarios=len(values))
            campaign.run_parallel(values, workers, lambda worker, value: worker.trial(value, directory_for(options.task, options.data, value, mode), mode), deadline)
            v3.write_json(state_path, dict(settings=current, status=f"completed_{mode}_{kind}"))
    finally:
        for worker in workers:
            worker.close()
    result = summarize(selected, options)
    result["settings"] = current
    result["storage_bytes"] = budget.used()
    v3.write_json(experiment / "metrics.json", result)
    v3.write_json(state_path, dict(settings=current, status="complete"))
    campaign.emit("oracle_experiment_complete", scenarios=result["scenarios"], episodes=result["episodes"], by_kind=result["by_kind"])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--task", type=Path, required=True)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--parallel", type=int, choices=(1, 2, 3), default=3)
    parser.add_argument("--limit-ms", type=int, default=240000)
    parser.add_argument("--budget-seconds", type=float, default=6900)
    options = parser.parse_args()
    if not options.task.is_absolute() or not options.data.is_absolute() or options.limit_ms <= 0 or not 0 < options.budget_seconds <= 7200:
        parser.error("Absolute task/data paths, positive limit and a wall budget <=7200s are required")
    options.task, options.data = options.task.resolve(), options.data.resolve()
    try:
        run(options)
    except Exception as exc:
        v3.write_json(options.task / "oracle-experiment/error.json", dict(error=type(exc).__name__, detail=str(exc)))
        raise


if __name__ == "__main__":
    main()
