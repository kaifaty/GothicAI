#!/usr/bin/env python3
"""Validate the moving-target fixture before collecting memory-v3 demos.

Run inside the caller's gq batch on Ryzen. Only the validation split is
evaluated; NAVDEMO recording and the oracle are explicitly disabled. Attempts
have separate frozen inputs and text-only output under TASK/validation.
"""
from __future__ import annotations

import argparse
from collections import Counter
import importlib.util
import os
from pathlib import Path
import time
from types import SimpleNamespace

import numpy as np


def module(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + ".py"))
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


oracle = module("nav_teacher_v3_oracle")
campaign, v3 = oracle.campaign, oracle.v3
TEACHERS = ("v3", "v2")
KINDS = ("follow", "approach")
MEMORY_VERSION = 3
PERCEPTION_MODE = "gaze"
SELECTION_SEED = 19102026


def atomic_json(path, value):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + ".tmp")
    v3.write_json(temporary, value)
    temporary.replace(path)


def output_for(task, attempt=1):
    if attempt not in (1, 2):
        raise ValueError("Only the original and final moving validation attempts are allowed")
    return Path(task) / "validation" / f"attempt-{attempt}"


def validation_cases(task, attempt=1, episodes_per_kind=None):
    source = Path(task) / "input/moving-scenarios.jsonl"
    values = list(v3.rows(source))
    moving = [value for value in values if value.get("goal_kind") in KINDS]
    if any(value.get("split") not in v3.SPLITS for value in moving):
        raise ValueError("Moving scenario has an unknown split")
    v3.validate_splits(moving)
    selected = [value for value in moving if value["split"] == "validation"]
    if not all(any(value["goal_kind"] == kind for value in selected) for kind in KINDS):
        raise ValueError("Validation needs both follow and approach scenarios")
    if attempt == 2:
        if episodes_per_kind != 10:
            raise ValueError("The final moving validation requires 10 episodes per kind")
        selected = expand_validation_cases(task, moving, selected, episodes_per_kind)
        source = output_for(task, attempt) / "cases.jsonl"
        if source.exists():
            if list(v3.rows(source)) != selected:
                raise ValueError("Final moving validation frozen selection or its inputs changed")
        else:
            v3.write_selection(source, selected)
    elif attempt != 1:
        raise ValueError("Only the original and final moving validation attempts are allowed")
    elif episodes_per_kind is not None and any(
            sum(value["goal_kind"] == kind for value in selected) != episodes_per_kind for kind in KINDS):
        raise ValueError("Original moving validation count differs from the requested count")
    if len({value["id"] for value in selected}) != len(selected):
        raise ValueError("Duplicate moving validation scenario ID")
    for value in selected:
        identifier = value["id"]
        if not isinstance(identifier, str) or Path(identifier).name != identifier or identifier in (".", ".."):
            raise ValueError(f"Unsafe scenario ID: {identifier!r}")
        for field in ("id", "start", "goal", "target", "region"):
            v3.token(value[field])
        route = value.get("route")
        if not isinstance(route, list) or len(route) < 2:
            raise ValueError(f"Missing moving route: {identifier}")
        for waypoint in route:
            v3.token(waypoint)
        if (not isinstance(value.get("seed"), int) or isinstance(value["seed"], bool)
                or not 0 <= value["seed"] <= 0xffffffff):
            raise ValueError(f"Moving scenario needs a uint32 seed: {identifier}")
    return source, selected


def expand_validation_cases(task, moving, selected, count):
    """Retain every original validation trial, then add unmeasured cases.

    Whole-region assignment comes from the old frozen cohort. New routes use
    training-pairs endpoints, but may only enter an existing validation region;
    they never move a training/test region across the split boundary.
    """
    task = Path(task)
    if any(sum(value["goal_kind"] == kind for value in selected) != 3 for kind in KINDS):
        raise ValueError("Final validation must extend the original three cases per kind")
    region_splits = {value["region"]: value["split"] for value in moving}
    regions = {value["region"] for value in selected}
    training = list(v3.rows(task / "input/training-pairs.jsonl"))
    excluded = v3.exclusion_pairs(task / "input/holdout-cv.jsonl", task / "input/ppo-validation.jsonl")
    if any(not v3.isolated(value, excluded) for value in selected):
        raise ValueError("Original moving validation overlaps a PPO validation or holdout pair")
    pool = v3.build_candidates(v3.read_json(task / "input/places.json")["places"], training,
        excluded, SELECTION_SEED, moving_per_region=count * 2, point_count=24)
    selected = list(selected)
    existing = {value["id"] for value in moving}
    for kind in KINDS:
        # Sorted IDs make the selection independent of process scheduling and
        # teacher outcomes. Original validation cases always remain first.
        candidates = sorted((value for value in pool if value["goal_kind"] == kind
            and value["region"] in regions and value["id"] not in existing), key=lambda value: value["id"])
        needed = count - sum(value["goal_kind"] == kind for value in selected)
        if len(candidates) < needed:
            raise ValueError(f"Only {len(candidates)} new validation {kind} routes; need {needed}")
        for value in candidates[:needed]:
            if value["split"] != region_splits[value["region"]] or value["split"] != "validation":
                raise ValueError("Seeded moving candidates changed the frozen region split")
            selected.append(value)
    v3.validate_splits([*moving, *selected])
    return selected


def runner_args(options, index):
    task = options.task
    attempt = getattr(options, "attempt", 1)
    return SimpleNamespace(output=output_for(task, attempt) / "workers" / str(index), attempt=attempt,
        runner=task / "build/opengothic/gothic-body-runner", assets=Path(os.environ.get("GOTHIC_DIR", Path.home() / "gothic-ai/game-settings")),
        save=task / "input/save.sav", cache=task / "cache", links=task / "input/accepted-links.txt",
        roads=task / "input/places.json", npc="BAU_954_MALETH", ini=None, seed=20102026,
        physics="cv", watchdog_seconds=900, limit_ms=options.limit_ms)


def settings(options, source, selected):
    args = runner_args(options, 0)
    result = dict(schema_version=1, navigation_memory_version=MEMORY_VERSION, perception_mode=PERCEPTION_MODE,
        runner_sha256=campaign.binary_digest(args.runner), source=str(source),
        cohort_sha256=campaign.binary_digest(source),
        input_sha256={name: campaign.binary_digest(getattr(args, name)) for name in ("save", "links", "roads")},
        assets=str(args.assets), physics="cv", limit_ms=options.limit_ms, teachers=list(TEACHERS),
        expected_by_kind=dict(Counter(value["goal_kind"] for value in selected)),
        split="validation", record=False, oracle=False, demonstration_recorded=False)
    if getattr(options, "attempt", 1) == 2:
        result.update(schema_version=2, attempt=2, episodes_per_kind=options.episodes_per_kind,
            selection_seed=SELECTION_SEED, selection_generator="build_candidates: moving_per_region=20, point_count=24",
            selection_input_sha256={name: campaign.binary_digest(options.task / "input" / name) for name in
                ("moving-scenarios.jsonl", "training-pairs.jsonl", "holdout-cv.jsonl", "ppo-validation.jsonl")})
    return result


def directory_for(task, value, teacher, attempt=1):
    if teacher not in TEACHERS or value["goal_kind"] not in KINDS or value["split"] != "validation":
        raise ValueError("Only v3/v2 moving validation trials are allowed")
    return output_for(task, attempt) / teacher / value["goal_kind"] / value["id"]


def command_sequence(args, value, teacher):
    # directory_for checks the split even when only preparing a command.
    directory_for(Path("/unused"), value, teacher)
    return [f"roads load {args.roads}", f"roads on {args.npc}", "navdemos record off",
        f"navdemos perception-mode {PERCEPTION_MODE}", "navdemos oracle off",
        v3.start_command(value, 3 if teacher == "v3" else 2, args.npc, args.limit_ms), "navdemos wait"]


def scenario_name(value, teacher):
    return f"demos-v4-moving-validation-{teacher}-{value['id']}"


def reject_samples(directory):
    if any(Path(directory).rglob("samples.bin*")):
        raise ValueError(f"Moving validation contains forbidden NAVDEMO recording: {directory}")


def validate_witness(task, args, value, teacher):
    expected = [f"seed {value['seed']}", "cheat god", "set time 10 0", "navdemos physics cv",
                *command_sequence(args, value, teacher), "label done"]
    witnesses = sorted((output_for(task, getattr(args, "attempt", 1)) / "workers").glob(
        f"*/scenarios/{scenario_name(value, teacher)}.bench"))
    if not witnesses or any(path.read_text(encoding="utf-8").splitlines() != expected for path in witnesses):
        raise ValueError(f"Missing or different saved moving validation command: {value['id']}/{teacher}")


def validate_metadata(directory, metadata, value, teacher, identity):
    version = 3 if teacher == "v3" else 2
    source = "learner_memory" if teacher == "v3" else "detour_teacher_v2"
    reject_samples(directory)
    # A moving goal's current waypoint/position is runtime state. The frozen
    # route and target are witnessed by the exact saved scenario command.
    if (metadata.get("teacher_version") != version or metadata.get("demo_version") != 2
            or any(metadata.get(key) != value.get(key) for key in ("start", "goal_kind", "seed"))
            or metadata.get("navigation_memory_version") != MEMORY_VERSION
            or metadata.get("limit_ms") != identity["limit_ms"]
            or metadata.get("perception_mode") != PERCEPTION_MODE or metadata.get("physics_mode") != "cv"
            or metadata.get("oracle") is not False or metadata.get("record") is not False
            or metadata.get("demonstration_recorded") is not False
            or metadata.get("observation_source") != source
            or metadata.get("drive", False) or metadata.get("replay", False)):
        raise ValueError(f"Moving validation identity, memory or recording mode differs: {directory}")
    if value["goal_kind"] == "approach":
        if (metadata.get("target_immortal") is not True or metadata.get("target_nonhostile") is not True
                or any(not isinstance(metadata.get(key), bool) for key in ("target_final_dead", "target_final_down"))):
            raise ValueError(f"Approach target fixture is not immortal/nonhostile or lacks death evidence: {directory}")
    if "id" in metadata:
        if (not v3.same_scenario(metadata, value, version)
                or metadata.get("moving_validation_identity") != identity
                or metadata.get("experiment_mode") != teacher):
            raise ValueError(f"Completed moving validation belongs to a different frozen trial: {directory}")
    else:
        for key in v3.SCENARIO_IDENTITY_FIELDS:
            if key != "goal" and key in metadata and metadata[key] != value.get(key):
                raise ValueError(f"Raw moving validation scenario {key} differs: {directory}")


def full_follow_horizon(value):
    limit, elapsed = value.get("limit_ms"), value.get("elapsed_ms")
    return (isinstance(limit, (int, float)) and not isinstance(limit, bool) and limit > 0
        and isinstance(elapsed, (int, float)) and not isinstance(elapsed, bool) and elapsed >= limit)


def follow_terminal_failure(value):
    if any(value.get(key, False) for key in ("actor_down", "final_dead", "final_down")):
        return "actor_down"
    if any(value.get(key, False) for key in ("target_down", "target_final_dead", "target_final_down")):
        return "target_down"
    failure = value.get("failure")
    # Older full-horizon text trials called the observation interval a path
    # timeout. Keep that native evidence while applying the follow metric.
    if failure not in (None, "", "path_timeout", "follow_no_contact", "follow_band_below_threshold"):
        return failure
    return ""


def follow_outcome(value):
    terminal = follow_terminal_failure(value)
    if terminal:
        return terminal
    if not full_follow_horizon(value):
        return "follow_incomplete_horizon"
    if value["first_contact_ms"] is None or value["follow_observed_ms"] <= 0:
        return "follow_no_contact"
    if value["follow_in_band_ms"] / value["follow_observed_ms"] < .8:
        return "follow_band_below_threshold"
    return ""


def text_metrics(directory):
    measured = oracle.text_metrics(directory)
    metadata = v3.read_json(Path(directory) / "episode.json")
    actions = np.loadtxt(Path(directory) / "actions.txt", ndmin=2)
    trace = np.loadtxt(Path(directory) / "target-trajectory.txt", ndmin=2)
    if trace.shape != (len(actions), 6) or not np.array_equal(trace[:, 0], actions[:, 0]):
        raise ValueError(f"Moving target trace does not align with physical actions: {directory}")
    if measured["goal_kind"] == "follow":
        trajectory = np.loadtxt(Path(directory) / "trajectory.txt", ndmin=2)
        first = measured["first_contact_ms"]
        durations = np.diff(trace[:, 0])
        post_contact = np.zeros(len(durations), dtype=bool) if first is None else trace[:-1, 0] >= first
        distances = trace[:-1, 4]
        observed = float(durations[post_contact].sum())
        actor_steps = np.linalg.norm(np.diff(trajectory[:, [1, 3]], axis=0), axis=1)
        target_steps = np.linalg.norm(np.diff(trace[:, [1, 3]], axis=0), axis=1)
        measured.update(follow_too_close_ms=float(durations[post_contact & (distances < 200)].sum()),
            follow_too_far_ms=float(durations[post_contact & (distances > 500)].sum()),
            follow_actor_speed_m_s=float(actor_steps[post_contact].sum() * 10 / observed) if observed else None,
            follow_target_speed_m_s=float(target_steps[post_contact].sum() * 10 / observed) if observed else None,
            follow_walk_ms=float(durations[post_contact & (actions[:-1, 4] != 0)].sum()),
            follow_run_ms=float(durations[post_contact & (actions[:-1, 4] == 0)].sum()))
    measured.update(navigation_memory_version=metadata["navigation_memory_version"],
        target_down=metadata.get("failure") == "target_down", actor_down=metadata.get("failure") == "actor_down")
    for key in ("target_final_dead", "target_final_down", "target_immortal", "target_nonhostile",
                "target_attitude", "target_temp_attitude", "target_external_control"):
        if key in metadata:
            measured[key] = metadata[key]
    if measured["goal_kind"] == "follow":
        measured.update(follow_full_horizon=full_follow_horizon(measured),
            follow_terminal_failure=follow_terminal_failure(measured),
            follow_failure=follow_outcome(measured), follow_success=not follow_outcome(measured))
    return measured


class Worker(campaign.Worker):
    def __init__(self, args, identity):
        super().__init__(args, 3, None)
        self.identity = identity

    def trial(self, task, value, teacher):
        directory = directory_for(task, value, teacher, getattr(self.args, "attempt", 1))
        episode = directory / "episode.json"
        resumed = episode.is_file()
        started = time.monotonic()
        reject_samples(directory)
        if not resumed:
            if directory.exists() and any(directory.iterdir()):
                raise ValueError(f"Incomplete prior validation; refusing to overwrite: {directory}")
            campaign.emit("moving_validation_trial_started", teacher=teacher, id=value["id"], kind=value["goal_kind"])
            self.scenario(scenario_name(value, teacher), command_sequence(self.args, value, teacher), directory, value["seed"])
        if not oracle.complete_text(directory, value["goal_kind"]):
            raise ValueError(f"Incomplete moving validation text files: {directory}")
        metadata = v3.read_json(episode)
        validate_metadata(directory, metadata, value, teacher, self.identity)
        validate_witness(task, self.args, value, teacher)
        if "id" not in metadata:
            metadata["runner_goal"] = metadata.get("goal")
            metadata.update({key: value[key] for key in v3.SCENARIO_IDENTITY_FIELDS if key in value})
            metadata.update(experiment_mode=teacher, moving_validation_identity=self.identity,
                            collector_wall_s=time.monotonic() - started)
            atomic_json(episode, metadata)
        measured = dict(text_metrics(directory), mode=teacher)
        campaign.emit("moving_validation_trial_complete", teacher=teacher, id=value["id"], kind=value["goal_kind"],
            success=metadata["success"], failure=metadata.get("failure"), resumed=resumed)
        return measured


def summarize_metrics(episodes):
    groups = {}
    for kind in KINDS:
        groups[kind] = {}
        for teacher in TEACHERS:
            values = [value for value in episodes if value["goal_kind"] == kind and value["mode"] == teacher]
            if not values:
                raise ValueError(f"Missing {teacher}/{kind} validation metrics")
            group = dict(episodes=len(values), successes=sum(bool(value["success"]) for value in values),
                failures=dict(Counter(value.get("failure") or "unspecified" for value in values if not value["success"])),
                target_down=sum(bool(value.get("target_down")) for value in values),
                target_final_dead=sum(bool(value.get("target_final_dead")) for value in values),
                target_final_down=sum(bool(value.get("target_final_down")) for value in values),
                actor_down=sum(bool(value.get("actor_down")) for value in values),
                final_dead=sum(bool(value.get("final_dead")) for value in values),
                final_down=sum(bool(value.get("final_down")) for value in values))
            group["success_fraction"] = group["successes"] / len(values)
            if kind == "follow":
                observed = sum(value["follow_observed_ms"] for value in values)
                in_band = sum(value["follow_in_band_ms"] for value in values)
                group.update(contact_episodes=sum(value["first_contact_ms"] is not None for value in values),
                    observed_episodes=sum(value["follow_observed_ms"] > 0 for value in values),
                    follow_observed_ms=observed, follow_in_band_ms=in_band,
                    follow_fraction=in_band / observed if observed > 0 else None)
                group["native_successes"] = group["successes"]
                group["native_failures"] = group["failures"]
                # Following is an observation interval, not an endpoint route.
                # Keep native outcomes as evidence, while the follow result is
                # determined by timed occupancy after first contact.
                outcomes = [follow_outcome(value) for value in values]
                group.update(successes=sum(not outcome for outcome in outcomes),
                    failures=dict(Counter(outcome for outcome in outcomes if outcome)),
                    full_horizon_episodes=sum(full_follow_horizon(value) for value in values),
                    nonterminal_episodes=sum(not follow_terminal_failure(value) for value in values))
                group["success_fraction"] = group["successes"] / len(values)
                for key in ("follow_too_close_ms", "follow_too_far_ms", "follow_walk_ms", "follow_run_ms"):
                    group[key] = sum(value.get(key, 0) for value in values)
                group["passed"] = (group["contact_episodes"] == len(values)
                    and group["observed_episodes"] == len(values)
                    and group["full_horizon_episodes"] == len(values)
                    and group["nonterminal_episodes"] == len(values)
                    and group["follow_fraction"] is not None and group["follow_fraction"] >= .8)
            groups[kind][teacher] = group
    follow, approach = groups["follow"]["v3"], groups["approach"]["v3"]
    gates = dict(follow_all_contacted=follow["contact_episodes"] == follow["episodes"],
        follow_all_observed=follow["observed_episodes"] == follow["episodes"],
        follow_all_full_horizon=follow["full_horizon_episodes"] == follow["episodes"],
        follow_all_nonterminal=follow["nonterminal_episodes"] == follow["episodes"],
        follow_at_least_80pct=follow["follow_fraction"] is not None and follow["follow_fraction"] >= .8,
        approach_at_least_80pct=approach["success_fraction"] >= .8)
    for teacher in TEACHERS:
        target = groups["approach"][teacher]
        gates[f"approach_{teacher}_no_target_death_or_down"] = not any(target[key] for key in
            ("target_down", "target_final_dead", "target_final_down"))
        # Comparison gates are explicit, but v2's follow result does not gate
        # v3 collection or the overall validation verdict.
    comparison = {f"follow_{teacher}_passed": groups["follow"][teacher]["passed"] for teacher in TEACHERS}
    return dict(by_kind=groups, gates=gates, passed=all(gates.values()), episode_metrics=episodes,
        teacher_follow_gates=comparison, collection=dict(follow=groups["follow"]["v3"]["passed"],
            approach=True, roads=True, objects=True, jumps=True),
        scenarios=len(episodes) // len(TEACHERS), episodes=len(episodes), complete=True,
        demonstration_recorded=False, navigation_memory_version=MEMORY_VERSION,
        follow_metric="sum in-band ms / sum post-contact ms; first <=500cm, band 200..500cm; every case must contact")


def run(options):
    attempt = getattr(options, "attempt", 1)
    source, selected = validation_cases(options.task, attempt, getattr(options, "episodes_per_kind", None))
    identity = settings(options, source, selected)
    output = output_for(options.task, attempt)
    output.mkdir(parents=True, exist_ok=True)
    state = output / "state.json"
    if state.exists():
        if v3.read_json(state).get("settings") != identity:
            raise ValueError("Moving validation belongs to another binary, input, seed, memory or time limit")
    elif any(path.name != "cases.jsonl" for path in output.iterdir()):
        raise ValueError("Existing moving validation has no identity state")
    reject_samples(output)
    atomic_json(state, dict(settings=identity, status="running"))
    workers = [Worker(runner_args(options, index), identity) for index in range(options.parallel)]
    try:
        for teacher in TEACHERS:
            for kind in KINDS:
                values = [value for value in selected if value["goal_kind"] == kind]
                campaign.run_parallel(values, workers,
                    lambda worker, value: worker.trial(options.task, value, teacher), float("inf"))
                atomic_json(state, dict(settings=identity, status=f"completed_{teacher}_{kind}"))
    finally:
        for worker in workers:
            worker.close()
    if settings(options, source, selected) != identity:
        raise ValueError("Moving validation binary or frozen inputs changed during collection")
    episodes = []
    for teacher in TEACHERS:
        for value in selected:
            directory = directory_for(options.task, value, teacher, attempt)
            validate_metadata(directory, v3.read_json(directory / "episode.json"), value, teacher, identity)
            validate_witness(options.task, runner_args(options, 0), value, teacher)
            episodes.append(dict(text_metrics(directory), mode=teacher))
    result = dict(summarize_metrics(episodes), settings=identity)
    atomic_json(output / "metrics.json", result)
    atomic_json(state, dict(settings=identity, status="passed" if result["passed"] else "failed"))
    campaign.emit("moving_validation_complete", passed=result["passed"], gates=result["gates"], by_kind=result["by_kind"],
        teacher_follow_gates=result["teacher_follow_gates"], collection=result["collection"])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    validate = commands.add_parser("validate-moving", help="text-only moving-target validation, v3 against v2")
    validate.add_argument("--task", type=Path, required=True)
    validate.add_argument("--parallel", type=int, choices=(1, 2, 3), default=3)
    validate.add_argument("--limit-ms", type=int, default=240000)
    validate.add_argument("--attempt", type=int, choices=(1, 2), default=1)
    validate.add_argument("--episodes-per-kind", type=int, help="Final attempt requires exactly 10 of each kind")
    options = parser.parse_args()
    os.environ.update(module("ryzen_game").game_environment())
    if not options.task.is_absolute() or options.limit_ms <= 0:
        parser.error("An absolute task path and a positive limit are required")
    if options.attempt == 2 and options.episodes_per_kind != 10:
        parser.error("--attempt 2 requires --episodes-per-kind 10")
    options.task = options.task.resolve()
    try:
        result = run(options)
    except Exception as exc:
        atomic_json(output_for(options.task, options.attempt) / "error.json", dict(error=type(exc).__name__, detail=str(exc)))
        raise
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
