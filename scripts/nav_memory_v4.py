#!/usr/bin/env python3
"""Bounded teacher-v3 comparison of journey memory and eye-baked familiarity.

Run inside the caller's gq batch. Frozen input files are read-only, all outputs
belong to --task, and no NAVDEMO demonstrations or learner updates are produced.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import time


def module(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + ".py"))
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


oracle = module("nav_teacher_v3_oracle")
campaign, v3 = oracle.campaign, oracle.v3
perception = module("nav_perception")
safety = module("nav_perception_safety")
MODES = ("baseline", "baseline-stranger", "journey", "journey-stranger", "familiar-local", "familiar-stranger")
LIMIT_MS = 240000


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def emit(event, **values):
    print(json.dumps(dict(event=event, **values)), flush=True)


def frozen_cases(task):
    points = [v for v in perception.frozen_cases(task / "input") if v["goal_kind"] == "point"]
    detours = list(v3.rows(task / "input/detour-val.jsonl"))
    if len(points) != 30 or len(detours) != 121:
        raise ValueError(f"Expected frozen 30 beyond-road points and 121 detour-val cases; got {len(points)}/{len(detours)}")
    selected = [dict(value, cohort=name) for name, values in (("beyond-roads", points), ("detour-val", detours)) for value in values]
    for value in selected:
        if (value["goal_kind"] not in ("point", "object") or not isinstance(value.get("seed"), int)
                or Path(value["id"]).name != value["id"] or value["id"] in (".", "..")):
            raise ValueError(f"Unsupported frozen scenario: {value['id']}")
    if len({value["id"] for value in selected}) != len(selected):
        raise ValueError("Duplicate frozen scenario IDs")
    return selected


def args_for(task, output, npc="BAU_954_MALETH"):
    from types import SimpleNamespace
    return SimpleNamespace(output=output, runner=task / "build/opengothic/gothic-body-runner",
        assets=Path(os.environ.get("GOTHIC_DIR", Path.home() / "gothic-ai/game-settings")), save=task / "input/save.sav", cache=task / "cache",
        links=task / "input/accepted-links.txt", roads=task / "input/places.json", ini=None,
        npc=npc, seed=22102026, physics="cv", watchdog_seconds=900, limit_ms=LIMIT_MS)


def mode_environment(mode):
    if mode not in MODES:
        raise ValueError(f"Unknown memory comparison mode: {mode}")
    os.environ["OPENGOTHIC_NAV_JOURNEY_MEMORY"] = "0" if mode.startswith("baseline") else "1"
    os.environ["OPENGOTHIC_NAV_FAMILIAR_MAP"] = "1" if mode.startswith("familiar-") else "0"


def initial_commands(task, npc, mode):
    commands = [f"roads load {task / 'input/places.json'}"]
    if mode.startswith("familiar-"):
        commands.append(f"roads familiar-load {task / 'input/familiar.bin'}")
    commands.append(f"roads {'stranger' if mode.endswith('-stranger') else 'on'} {npc}")
    if mode.startswith("familiar-"):
        commands.append(f"roads familiar-stats {npc} 300")
    return commands


def command_sequence(task, value, npc, mode):
    return [*initial_commands(task, npc, mode), "navdemos record off", "navdemos oracle off",
            "navdemos perception-mode gaze", v3.start_command(value, 3, npc, LIMIT_MS), "navdemos wait"]


def stand(task, directory, commands):
    args = args_for(task, directory / "worker")
    if directory.name == "bake":
        args.watchdog_seconds = 6500
    worker = campaign.Worker(args, 3, None)
    try:
        worker.scenario(directory.name, commands, directory, args.seed)
    finally:
        worker.close()


def bake(task):
    if (task / "input/familiar.bin").exists():
        raise ValueError("Refusing to overwrite an existing familiar atlas")
    stand(task, task / "bake", [f"roads load {task / 'input/places.json'}",
          f"roads bake {task / 'input/familiar.bin'}"])
    if not (task / "input/familiar.bin").is_file():
        raise ValueError("Eye baking did not produce the familiar atlas")
    bake_npc_stats(task)


def frozen_safety(task):
    fixtures = v3.read_json(task / "input/fixtures.json")
    if len(fixtures) != 9 or any(not value.get("qualified") for value in fixtures):
        raise ValueError("Expected 9 frozen qualified safety fixtures")
    return fixtures


def bake_npc_stats(task):
    cases = frozen_cases(task)
    safety_starts = [dict(start=safety.token(value["projected_start_cm"])) for value in frozen_safety(task)]
    actors, nodes, positions = discovery(task)
    npcs = sorted({local_actor(value, actors, nodes, positions)["npc"] for value in [*cases, *safety_starts]})
    commands = [f"roads load {task / 'input/places.json'}", f"roads familiar-load {task / 'input/familiar.bin'}"]
    for npc in npcs:
        for residency in ("on", "stranger"):
            commands.extend((f"roads {residency} {npc}", f"roads familiar-stats {npc} 300"))
    directory = task / "bake-npcs"
    stand(task, directory, commands)
    stats = [row for row in v3.rows(directory / "roads-events.jsonl") if row.get("kind") == "familiar_stats"]
    if (len(stats) != len(npcs) * 2 or any(type(row.get("stranger")) is not bool for row in stats)
            or {(row.get("actor"), row["stranger"]) for row in stats} != {(npc, stranger) for npc in npcs for stranger in (False, True)}):
        raise ValueError("Missing or duplicate native familiar stats for scenario NPCs")
    for row in stats:
        if (row["npc_count"] != 300 or row["max_bytes"] != 50_000_000
                or not 0 < row["total_bytes_for_npcs"] <= 50_000_000
                or not 0 <= row["known_2m_tiles_xz"] <= row["eligible_2m_tiles"]
                or not 0 <= row["coverage_fraction"] <= 1):
            raise ValueError(f"Invalid native familiar coverage or 300-NPC memory budget: {row['actor']}")
        if row["stranger"] and any(row[key] != 0 for key in (
                "known_tiles", "known_2m_tiles_xz", "eligible_2m_tiles", "familiar_source_poses", "coverage_fraction")):
            raise ValueError(f"Stranger unexpectedly knows familiar terrain: {row['actor']}")
    scenario_starts = len(cases) + len(safety_starts)
    total_bytes = max(row["total_bytes_for_npcs"] for row in stats)
    v3.write_json(directory / "memory-cost.json", dict(scenario_starts=scenario_starts, npc_count=300,
        unique_npc_count=len(npcs), total_bytes_for_300_npcs=total_bytes, measurements=stats))
    emit("memory_bake_npcs_complete", npcs=len(npcs), scenario_starts=scenario_starts,
         total_bytes_for_300_npcs=total_bytes)


def discovery(task):
    directory = task / "discovery"
    if not (directory / "roads-events.jsonl").exists():
        stand(task, directory, [f"roads load {task / 'input/places.json'}", "roads actors", "navdemos points-v3"])
    actors = [row for row in v3.rows(directory / "roads-events.jsonl") if row.get("kind") == "roads_actors"]
    if len(actors) != 1:
        raise ValueError("Expected one native routine-actor export")
    graph = v3.read_json(directory / "roads-graph.json")
    positions = v3.load_start_positions(directory / "points-v3.jsonl")
    return actors[0]["actors"], graph["nodes"], positions


def start_position(value, positions):
    start = value["start"]
    if start.startswith("cm:"):
        result = [float(v) for v in start[3:].split(",")]
    else:
        result = positions[start]
    if len(result) != 3 or not all(math.isfinite(v) for v in result):
        raise ValueError(f"Invalid frozen start: {start}")
    return result


def local_actor(value, actors, nodes, positions):
    """Select a resident by routine distance to START, never by the goal."""
    start = start_position(value, positions)
    candidates = []
    for actor in actors:
        routine_nodes = {r["node"] for r in actor["routines"] if r["node"] is not None}
        if not routine_nodes or actor["instance"] == "PC_HERO":
            continue
        distance = min(math.hypot(nodes[n][1] - start[0], nodes[n][3] - start[2]) for n in routine_nodes)
        candidates.append((distance, actor["instance"]))
    if not candidates:
        raise ValueError(f"No routine resident for start {value['start']}")
    distance, npc = min(candidates)
    return dict(npc=npc, nearest_routine_to_start_cm=distance,
                selection="nearest routine waypoint to frozen start; goal ignored")


def identity(task, mode):
    files = ("expanded-scenarios.jsonl", "detour-val.jsonl", "fixtures.json", "places.json", "accepted-links.txt", "save.sav")
    return dict(mode=mode, teacher_version=3, limit_ms=LIMIT_MS, physics="cv", perception_mode="gaze",
                runner_sha256=digest(task / "build/opengothic/gothic-body-runner"),
                input_sha256={name: digest(task / "input" / name) for name in files},
                familiar_sha256=digest(task / "input/familiar.bin") if mode.startswith("familiar-") else None)


def validate_raw(directory, metadata, value, mode, limit_ms=LIMIT_MS):
    if (metadata.get("teacher_version") != 3 or metadata.get("demo_version") != (2 if mode.startswith("baseline") else 3)
            or metadata.get("navigation_memory_version") != (3 if mode.startswith("baseline") else 4)
            or metadata.get("journey_memory") is not (not mode.startswith("baseline"))
            or any(metadata.get(k) != value[k] for k in ("start", "goal", "goal_kind"))
            or metadata.get("limit_ms") != limit_ms or metadata.get("perception_mode") != "gaze"
            or metadata.get("oracle") is not False or metadata.get("observation_source") != "learner_memory"
            or metadata.get("record") is not False or metadata.get("demonstration_recorded") is not False
            or metadata.get("drive", False) or (directory / "samples.bin.gz").exists()):
        raise ValueError(f"Memory evaluation identity or no-recording contract differs: {directory}")


class Worker(campaign.Worker):
    def trial(self, task, value, mode, selection, current):
        directory = task / "evaluation" / mode / value["cohort"] / value["id"]
        commands = command_sequence(task, value, selection["npc"], mode)
        saved = directory / "episode.json"
        if not saved.exists():
            if directory.exists() and any(directory.iterdir()):
                raise ValueError(f"Refusing to overwrite incomplete trial: {directory}")
            self.scenario(f"memory-{mode}-{value['id']}", commands, directory, value["seed"])
        if not oracle.complete_text(directory, value["goal_kind"]):
            raise ValueError(f"Incomplete evaluation text: {directory}")
        metadata = v3.read_json(saved)
        validate_raw(directory, metadata, value, mode)
        if "memory_v4_comparison" in metadata:
            if metadata["memory_v4_comparison"] != current or metadata.get("resident_selection") != selection:
                raise ValueError(f"Existing trial uses different binary, inputs or resident: {directory}")
        else:
            metadata.update({key: value[key] for key in v3.SCENARIO_IDENTITY_FIELDS if key in value})
            metadata.update(cohort=value["cohort"], memory_v4_comparison=current, resident_selection=selection)
            v3.write_json(saved, metadata)
        measured = dict(oracle.text_metrics(directory), cohort=value["cohort"], mode=mode, resident_selection=selection)
        measured.update({key: metadata[key] for key in ("memory_update_us", "memory_view_us", "memory_updates", "memory_views") if key in metadata})
        stats = [row for row in v3.rows(directory / "roads-events.jsonl") if row.get("kind") == "familiar_stats"]
        if mode.startswith("familiar-"):
            if len(stats) != 1:
                raise ValueError(f"Missing native familiarity coverage and memory stats: {directory}")
            measured["familiar"] = stats[0]
        emit("memory_trial_complete", mode=mode, id=value["id"], success=measured["success"], npc=selection["npc"])
        return measured


def evaluate(options):
    task = options.task
    cases = frozen_cases(task)
    actors, nodes, positions = discovery(task)
    residents = {value["id"]: local_actor(value, actors, nodes, positions) for value in cases}
    deadline = time.monotonic() + options.budget_seconds
    results, settings = [], {}
    for mode in options.mode or MODES:
        mode_environment(mode)
        current = identity(task, mode)
        settings[mode] = current
        workers = [Worker(args_for(task, task / "evaluation/workers" / mode / str(i)), 3, None) for i in range(options.parallel)]
        mode_results = []
        try:
            campaign.run_parallel(cases, workers, lambda worker, value: mode_results.append(
                worker.trial(task, value, mode, residents[value["id"]], current)), deadline)
        finally:
            for worker in workers:
                worker.close()
        if identity(task, mode) != current:
            raise ValueError("Binary or frozen inputs changed during comparison")
        results.extend(mode_results)
    grouped = {cohort: {mode: v3.group_metrics([r for r in results if r["cohort"] == cohort and r["mode"] == mode])
               for mode in settings} for cohort in ("beyond-roads", "detour-val")}
    costs = {}
    for mode in settings:
        episodes = [row for row in results if row["mode"] == mode]
        costs[mode] = {part: dict(samples=sum(row["memory_" + part + "s"] for row in episodes),
            mean_us=sum(row["memory_" + part + "_us"] * row["memory_" + part + "s"] for row in episodes)
            / sum(row["memory_" + part + "s"] for row in episodes)) for part in ("update", "view")}
    result = dict(settings=settings, by_cohort=grouped, memory_cost=costs,
                  episodes=sorted(results, key=lambda r: (r["mode"], r["id"])),
                  completed=True, demonstration_recorded=False)
    filename = "metrics-" + "-".join(options.mode) + ".json" if options.mode else "metrics.json"
    v3.write_json(task / "evaluation" / filename, result)
    emit("memory_evaluation_complete", by_cohort=grouped)


def evaluate_safety(options):
    task = options.task
    fixtures = frozen_safety(task)
    actors, nodes, positions = discovery(task)
    deadline = time.monotonic() + options.budget_seconds
    results = []
    for mode in options.mode or MODES:
        mode_environment(mode)
        current = identity(task, mode)
        args = args_for(task, task / "safety/workers" / mode)
        args.limit_ms = 30000
        worker = campaign.Worker(args, 3, None)
        try:
            for fixture in fixtures:
                if time.monotonic() >= deadline:
                    raise TimeoutError("Safety comparison wall budget reached")
                value = dict(start=safety.token(fixture["projected_start_cm"]), goal=safety.token(fixture["goal_cm"]), goal_kind="point")
                selection = local_actor(value, actors, nodes, positions)
                args.npc = selection["npc"]
                commands = safety.command_sequence(args, fixture, "memory", "gaze")
                commands[:2] = initial_commands(task, args.npc, mode)
                directory = task / "safety" / mode / fixture["label"]
                episode = directory / "episode.json"
                if not episode.exists():
                    if directory.exists() and any(directory.iterdir()):
                        raise ValueError(f"Refusing to overwrite incomplete safety trial: {directory}")
                    worker.scenario(f"memory-safety-{mode}-{fixture['label']}", commands, directory, args.seed)
                metadata = v3.read_json(episode)
                validate_raw(directory, metadata, value, mode, args.limit_ms)
                if "memory_v4_comparison" in metadata and metadata["memory_v4_comparison"] != current:
                    raise ValueError(f"Existing safety trial uses different binary or inputs: {directory}")
                metadata.update(memory_v4_comparison=current, resident_selection=selection)
                v3.write_json(episode, metadata)
                measured = dict(safety.assess(fixture, metadata, list(v3.rows(directory / "perception-safety-frames.jsonl"))),
                                mode=mode, resident_selection=selection)
                results.append(measured)
                emit("memory_safety_complete", mode=mode, label=fixture["label"], safe=measured["safe"])
        finally:
            worker.close()
    grouped = {kind: {mode: safety.group_metrics([r for r in results if r["kind"] == kind and r["mode"] == mode])
               for mode in options.mode or MODES} for kind in sorted({v["kind"] for v in fixtures})}
    filename = "metrics-" + "-".join(options.mode) + ".json" if options.mode else "metrics.json"
    v3.write_json(task / "safety" / filename, dict(by_kind=grouped, episodes=results, completed=True, demonstration_recorded=False))
    emit("memory_safety_comparison_complete", by_kind=grouped)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("bake", "evaluate", "safety"))
    parser.add_argument("--task", type=Path, required=True)
    parser.add_argument("--mode", choices=MODES, action="append")
    parser.add_argument("--parallel", type=int, choices=(1, 2, 3), default=3)
    parser.add_argument("--budget-seconds", type=float, default=6600)
    options = parser.parse_args()
    os.environ.update(module("ryzen_game").game_environment())
    if not options.task.is_absolute() or not 0 < options.budget_seconds <= 6900:
        parser.error("An absolute task path and a wall budget in (0,6900] seconds are required")
    options.task = options.task.resolve()
    if shutil.disk_usage(options.task).free < 20 * 1024**3:
        raise ValueError("Less than 20 GiB free before measurement")
    if options.phase == "bake":
        bake(options.task)
    elif options.phase == "evaluate":
        evaluate(options)
    else:
        evaluate_safety(options)


if __name__ == "__main__":
    main()
