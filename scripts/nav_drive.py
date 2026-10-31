#!/usr/bin/env python3
"""Select, run and measure the held-out navigation policy drives on Ryzen.

The runner holds its immutable navigation mesh between episodes, but resets the
saved world for each pair. Use nav-drive-batch.sh so other Ryzen sessions can
take the shared simulation lock between two-pair batches. Weights and NAVDEMO
recordings remain in --output, outside the repository.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from copy import copy
import importlib.util
import json
import os
from pathlib import Path
import random
import statistics
import subprocess
import sys
import time


REPO = Path(__file__).resolve().parent.parent
DEFAULT_OUTPUT = Path.home() / "gothic-ai/nav-drive"
HEADS = ("forward", "strafe", "turn", "walk", "jump", "climb")
ACTION_FIELDS = (*HEADS, "head_yaw_deg", "head_pitch_deg")


def module(name):
    spec = importlib.util.spec_from_file_location(name, REPO / "scripts" / f"{name}.py")
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def pairs_path(args):
    return args.pairs or args.output / "pairs.jsonl"


def teacher_references(root):
    """Match v2 teacher starts by pair, independently of the v1 cache IDs."""
    references = {}
    for path in sorted(root.rglob("episode.json")):
        episode = read_json(path)
        if episode.get("replay") or episode.get("drive") or "dagger" in episode:
            continue
        if episode.get("demo_version", episode.get("teacher_version")) != 2:
            continue
        if not (path.parent / "samples.bin.gz").is_file():
            continue
        pair = (episode["start"], episode["goal"])
        candidate = (path.parent, episode)
        # A successful ordinary start is the reference when the pair has repeats.
        if pair not in references or (not references[pair][1].get("success") and episode.get("success")):
            references[pair] = candidate
    if not references:
        raise ValueError(f"No NAVDEMO v2 teacher references in {root}")
    return references


def prepare(args):
    index = read_json(args.bc_cache / "index.json")
    unique = {}
    for episode in index["episodes"]:
        pair = tuple(episode["pair"])
        if pair in unique:
            if unique[pair]["split"] != episode["split"]:
                raise ValueError(f"Pair occurs in both splits: {pair}")
            continue
        unique[pair] = episode
    validation = sorted((ep for ep in unique.values() if ep["split"] == "validation"),
                        key=lambda ep: (ep["region"], ep["start"], ep["goal"]))
    if len(validation) != 31:
        raise ValueError(f"Expected the 31 held-out pairs, found {len(validation)}")
    train_regions = defaultdict(list)
    for episode in unique.values():
        if episode["split"] == "train":
            train_regions[episode["region"]].append(episode)
    if len(train_regions) != 6:
        raise ValueError(f"Expected six training regions, found {len(train_regions)}")
    rng = random.Random(args.selection_seed)
    train = []
    for region, group in sorted(train_regions.items()):
        group.sort(key=lambda ep: (ep["start"], ep["goal"]))
        if len(group) < 5:
            raise ValueError(f"Need five training pairs in {region}")
        train.extend(sorted(rng.sample(group, 5), key=lambda ep: (ep["start"], ep["goal"])))
    references = teacher_references(args.teacher_root)
    selected = []
    excluded_validation = []
    for split, episodes in (("validation", validation), ("train", train)):
        split_index = 0
        for source_index, episode in enumerate(episodes):
            pair = tuple(episode["pair"])
            if pair not in references:
                raise ValueError(f"Missing NAVDEMO v2 teacher pair: {pair}")
            teacher_dir, teacher = references[pair]
            if split == "validation" and not teacher.get("success"):
                excluded_validation.append(pair)
                continue
            selected.append(dict(index=len(selected), split=split, split_index=split_index,
                                 region=episode["region"], start=episode["start"], goal=episode["goal"],
                                 teacher_episode=str(teacher_dir), teacher_elapsed_ms=teacher["elapsed_ms"],
                                 teacher_success=bool(teacher.get("success")), teacher_version=2,
                                 source_cache_id=episode["id"], source_split_index=source_index,
                                 seed=teacher.get("seed", 7), npc=teacher.get("npc", "BAU_954_MALETH")))
            split_index += 1
    if not any(pair["split"] == "validation" for pair in selected):
        raise ValueError("Teacher v2 reached none of the held-out pairs")
    selected.append(dict(index=len(selected), split="control", split_index=0, region="lobart",
                         start="pasture_fence_current_pose", goal="FP_ROAM_FARM1_SHEEP_09",
                         source_control="reports/navigation/2026-10-16-stuck/intentional.bench",
                         seed=7, npc="BAU_954_MALETH"))
    path = pairs_path(args)
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text(encoding="utf-8") != "".join(json.dumps(p, ensure_ascii=False) + "\n" for p in selected):
        raise ValueError(f"Refusing to replace a different existing selection: {path}")
    path.write_text("".join(json.dumps(p, ensure_ascii=False) + "\n" for p in selected), encoding="utf-8")
    print(json.dumps(dict(pairs=str(path), source_validation=len(validation),
                          validation=sum(p["split"] == "validation" for p in selected),
                          excluded_validation=excluded_validation, train=len(train), control=1,
                          training_per_region=5, selection_seed=args.selection_seed), ensure_ascii=False))


def load_pairs(args):
    pairs = [json.loads(line) for line in pairs_path(args).read_text(encoding="utf-8").splitlines() if line.strip()]
    for index, pair in enumerate(pairs):
        if pair["index"] != index or pair["split"] not in ("validation", "train", "control"):
            raise ValueError(f"Invalid pair index/split at {index}")
    return pairs


def episode_dir(args, pair):
    return args.output / "dagger" / pair["split"] / f"{pair['split_index']:03d}"


def completed(path):
    return all((path / name).is_file() for name in ("episode.json", "trajectory.txt", "actions.txt", "samples.bin.gz"))


def indices(args, pairs):
    selected = [p["index"] for p in pairs if args.split == "all" or p["split"] == args.split]
    if args.indices is not None:
        selected = [int(item) for item in args.indices.split(",")]
        if len(selected) != len(set(selected)) or any(i < 0 or i >= len(pairs) for i in selected):
            raise ValueError("--indices must contain distinct valid pair indices")
        if any(args.split != "all" and pairs[i]["split"] != args.split for i in selected):
            raise ValueError("--indices contains a pair outside --split")
    else:
        selected = selected[args.start_index:]
        if args.count is not None:
            selected = selected[:args.count]
    if args.resume:
        for index in selected:
            directory = episode_dir(args, pairs[index])
            if completed(directory) and read_json(directory / "episode.json").get("action_mode", "argmax") != getattr(args, "action_mode", "argmax"):
                raise ValueError(f"--resume action mode differs from the completed drive: {directory}")
        selected = [i for i in selected if not completed(episode_dir(args, pairs[i]))]
    return selected


def policy_arguments(args, pair_index):
    """Keep the existing greedy command unchanged; sampling is an explicit suffix."""
    mode = getattr(args, "action_mode", "argmax")
    if mode not in ("argmax", "sample"):
        raise ValueError("--action-mode must be sample or argmax")
    seed = args.mixing_seed + pair_index
    result = f"{args.limit_ms} {args.weights} {args.teacher_beta:.9g} {seed}"
    return result + (" sample" if mode == "sample" else "")


def collect(args):
    if not 0 <= args.teacher_beta <= 1:
        raise ValueError("--teacher-beta must be in [0,1]")
    if args.teacher_beta and not args.dagger_collection:
        raise ValueError("Measured drives require --teacher-beta 0; use --dagger-collection for mixed control")
    action_mode = getattr(args, "action_mode", "argmax")
    if action_mode not in ("argmax", "sample"):
        raise ValueError("--action-mode must be sample or argmax")
    pairs = load_pairs(args)
    selected = indices(args, pairs)
    if args.list_indices:
        print(" ".join(map(str, selected)))
        return
    if not selected:
        print("All selected episodes are complete.")
        return
    if sys.platform != "linux":
        raise ValueError("Run policy drives on Ryzen/Linux")
    for name in ("runner", "assets", "save", "cache", "links", "roads", "weights"):
        path = getattr(args, name)
        if path is None or (name != "cache" and not path.exists()):
            raise ValueError(f"--{name} must exist")
    args.cache.mkdir(parents=True, exist_ok=True)
    args.output.mkdir(parents=True, exist_ok=True)
    os.environ["OPENGOTHIC_BODY_TRACE"] = "0"
    collector = module("collect_nav_demos")
    worker_args = copy(args)
    worker_args.output = args.output / "runner-work"
    worker_args.output.mkdir(parents=True, exist_ok=True)
    prepare_args = copy(args)
    prepare_args.seed = 7
    prepare_scenario = collector.scenario_file(prepare_args, "drive-prepare", [f"navmesh build {args.cache} {args.links}"])
    runner = collector.runner_type()(worker_args, prepare_scenario)
    try:
        runner.scenario(prepare_scenario, worker_args.output / "prepare", 7, timeout=720)
        collector.check_log(worker_args.output / "prepare")
        for i in selected:
            pair = pairs[i]
            directory = episode_dir(args, pair)
            if completed(directory):
                raise ValueError(f"Episode already exists: {directory}; use --resume")
            name = f"drive-{pair['split']}-{pair['split_index']:03d}"
            commands = [f"roads load {args.roads}", f"roads on {pair['npc']}"]
            mixing_seed = args.mixing_seed + pair["index"]
            policy_args = policy_arguments(args, pair["index"])
            if pair["split"] == "control":
                commands.extend(["goto NW_FARM1_ENTRANCE_03", f"put {pair['npc']} 0 0 -1400",
                                 "goto NW_FARM1_ENTRANCE_01", "wait 1000",
                                 f"navdemos drive-current {pair['npc']} {pair['goal']} {policy_args}"])
            else:
                commands.append(f"navdemos drive {pair['npc']} {pair['start']} {pair['goal']} {policy_args}")
            commands.append("navdemos wait")
            scenario_args = copy(args)
            scenario_args.seed = pair["seed"]
            scenario = collector.scenario_file(scenario_args, name, commands)
            started = time.monotonic()
            runner.scenario(scenario, directory, pair["seed"], timeout=args.watchdog_seconds)
            wall = time.monotonic() - started
            collector.check_log(directory)
            if not completed(directory):
                raise RuntimeError(f"Incomplete drive: {directory}")
            episode = read_json(directory / "episode.json")
            if episode.get("physics_mode", "native") != getattr(args, "physics", "native"):
                raise ValueError(f"Drive physics differs from requested mode: {directory}")
            if episode.get("demo_version") != 2 or episode.get("teacher_version") != 2:
                raise ValueError(f"Drive must use NAVDEMO v2 policy/teacher: {directory}")
            if episode.get("teacher_beta") != args.teacher_beta or episode.get("mixing_seed") != mixing_seed:
                raise ValueError(f"Runner did not apply the requested mixing settings: {directory}")
            if episode.get("action_mode", "argmax") != action_mode:
                raise ValueError(f"Runner did not apply the requested action mode: {directory}")
            episode.update(pair, drive=True, weights=str(args.weights), limit_ms=args.limit_ms,
                           collector_wall_s=wall, selection_seed=args.selection_seed,
                           dagger_collection=args.dagger_collection, requested_action_mode=action_mode,
                           requested_physics=getattr(args, "physics", "native"))
            write_json(directory / "episode.json", episode)
            print(f"{name}: success={episode['success']} elapsed_ms={episode['elapsed_ms']} "
                  f"distance_cm={episode.get('distance_cm')} wall_s={wall:.2f}", flush=True)
    finally:
        runner.close()


def speed(args):
    populations = [int(value) for value in args.populations.split(",")]
    if not populations or len(set(populations)) != len(populations) or any(p < 1 or p > 1000 for p in populations):
        raise ValueError("--populations must contain distinct integers in [1,1000]")
    if sys.platform == "darwin":
        if populations != [20]:
            raise ValueError("The short Mac speed measurement requires --populations 20")
        result = subprocess.run(["pgrep", "-f", "^[^ ]*Gothic2Notr -g"], stdout=subprocess.DEVNULL, check=False)
        if result.returncode != 1:
            raise ValueError("Mac speed measurement requires no running Gothic2Notr -g process")
    for name in ("runner", "assets", "save", "cache", "links", "roads", "weights"):
        path = getattr(args, name)
        if path is None or (name != "cache" and not path.exists()):
            raise ValueError(f"--{name} must exist")
    candidates = [pair for pair in load_pairs(args) if pair.get("teacher_elapsed_ms")]
    if not candidates:
        raise ValueError("No teacher reference pair is available for the speed measurement")
    pair = max(candidates, key=lambda value: (value["teacher_elapsed_ms"], -value["index"]))
    args.cache.mkdir(parents=True, exist_ok=True)
    args.output.mkdir(parents=True, exist_ok=True)
    os.environ["OPENGOTHIC_BODY_TRACE"] = "0"
    collector = module("collect_nav_demos")
    worker_args = copy(args)
    worker_args.output = args.output / "performance" / "runner-work"
    worker_args.output.mkdir(parents=True, exist_ok=True)
    scenario_args = copy(args)
    scenario_args.seed = pair["seed"]
    prepare_scenario = collector.scenario_file(scenario_args, "speed-prepare", [f"navmesh build {args.cache} {args.links}"])
    runner = collector.runner_type()(worker_args, prepare_scenario)
    measurements = []
    try:
        runner.scenario(prepare_scenario, worker_args.output / "prepare", pair["seed"], timeout=720)
        collector.check_log(worker_args.output / "prepare")
        for population in populations:
            directory = args.output / "performance" / f"p{population}"
            if completed(directory):
                if not args.resume:
                    raise ValueError(f"Speed output already exists: {directory}; use --resume")
                episode = read_json(directory / "episode.json")
            else:
                commands = [f"roads load {args.roads}", f"roads on {pair['npc']}",
                            f"navdemos speed {pair['npc']} {pair['start']} {pair['goal']} {args.limit_ms} {args.weights} {population}",
                            "navdemos wait"]
                scenario = collector.scenario_file(scenario_args, f"speed-p{population}", commands)
                started = time.monotonic()
                runner.scenario(scenario, directory, pair["seed"], timeout=args.watchdog_seconds)
                wall = time.monotonic() - started
                collector.check_log(directory)
                if not completed(directory):
                    raise RuntimeError(f"Incomplete speed output: {directory}")
                episode = read_json(directory / "episode.json")
                episode.update(pair, drive=True, speed=True, weights=str(args.weights), limit_ms=args.limit_ms,
                               collector_wall_s=wall, timing_scope="same_input_independent_gru_states_one_thread")
                write_json(directory / "episode.json", episode)
            if episode.get("policy_population") != population or episode.get("policy_measured_calls", 0) <= 0:
                raise ValueError(f"No warmed policy timing for population {population}: {directory}")
            measurements.append({key: episode.get(key) for key in (
                "policy_population", "policy_warmup_decisions", "policy_measured_calls", "population_measured_ticks",
                "policy_us_median", "policy_us_p95", "encode_us_median", "encode_us_p95",
                "population_us_median", "population_us_p95", "elapsed_ms", "decisions", "collector_wall_s")})
    finally:
        runner.close()
    result = dict(timing_scope="same_input_independent_gru_states_one_thread", weights=str(args.weights),
                  pair_index=pair["index"], start=pair["start"], goal=pair["goal"],
                  teacher_elapsed_ms=pair["teacher_elapsed_ms"], measurements=measurements)
    write_json(args.output / "performance" / "metrics.json", result)
    print(json.dumps(result, ensure_ascii=False, indent=2))


def trajectory(path):
    import numpy as np
    rows = np.loadtxt(path / "trajectory.txt", ndmin=2)
    if rows.shape[1] != 5 or not len(rows) or not np.isfinite(rows).all():
        raise ValueError(f"Invalid trajectory in {path}")
    return rows


def discrete_actions(values):
    """Use the same six categorical decisions as the policy's output heads."""
    import numpy as np
    values = np.asarray(values)
    return np.column_stack((np.sign(values[:, :3]), values[:, 3:6] != 0))


def episode_metrics(directory, demos):
    import numpy as np
    episode = read_json(directory / "episode.json")
    actions = np.loadtxt(directory / "actions.txt", ndmin=2)
    applied = discrete_actions(actions[:-1, 1:7])
    counts = Counter()
    change_correct = Counter()
    teacher_change_steps = 0
    steps = 0
    last = None
    last_teacher = None
    for data in demos.iter_chunks(directory):
        actual = applied[steps:steps + len(data)]
        teacher = discrete_actions(np.column_stack([data["keys"][head] for head in HEADS]))
        disagreements = actual != teacher
        teacher_changes = np.zeros(len(data), dtype=bool)
        teacher_changes[1:] = (teacher[1:] != teacher[:-1]).any(axis=1)
        if last_teacher is not None:
            teacher_changes[0] = (teacher[0] != last_teacher).any()
        last_teacher = teacher[-1].copy()
        teacher_change_steps += int(teacher_changes.sum())
        for column, head in enumerate(HEADS):
            counts[head] += int(disagreements[:, column].sum())
            change_correct[head] += int((~disagreements[teacher_changes, column]).sum())
        counts["any"] += int(disagreements.any(axis=1).sum())
        change_correct["all_heads"] += int((~disagreements[teacher_changes].any(axis=1)).sum())
        steps += len(data)
        last = data[-1].copy()
    if steps != episode["decisions"] or len(actions) != steps + 1:
        raise ValueError(f"Decision/action mismatch in {directory}")
    action_masks = dict(forward=applied[:, 0] > 0, backward=applied[:, 0] < 0,
                        strafe_left=applied[:, 1] < 0, strafe_right=applied[:, 1] > 0,
                        turn_left=applied[:, 2] < 0, turn_right=applied[:, 2] > 0,
                        walk=applied[:, 3] != 0, jump=applied[:, 4] != 0, climb=applied[:, 5] != 0,
                        idle=~applied[:, [0, 1, 2, 4, 5]].any(axis=1),
                        turn_only=(applied[:, :2] == 0).all(axis=1) & (applied[:, 2] != 0))
    action_counts = {key: int(mask.sum()) for key, mask in action_masks.items()}
    changes = applied[1:] != applied[:-1]
    action_changes = {head: int(changes[:, column].sum()) for column, head in enumerate(HEADS)}
    action_changes["any"] = int(changes.any(axis=1).sum())
    run_bounds = np.r_[0, np.flatnonzero(changes.any(axis=1)) + 1, steps]
    run_lengths = np.diff(run_bounds)
    path = trajectory(directory)
    xz = path[:, [1, 3]]
    travelled = float(np.linalg.norm(np.diff(xz, axis=0), axis=1).sum())
    tail = path[path[:, 0] >= path[-1, 0] - 10000]
    tail_span = float(np.linalg.norm(np.ptp(tail[:, 1:4], axis=0)))
    tail_displacement = float(np.linalg.norm(tail[-1, 1:4] - tail[0, 1:4]))
    tail_path = float(np.linalg.norm(np.diff(tail[:, 1:4], axis=0), axis=1).sum())
    # Each maximal run of consecutive 3-second windows confined to a 25 cm
    # radius is one stationary spell; this also detects policies that turn in place.
    anchor = np.searchsorted(path[:, 0], path[:, 0] - 3000)
    stationary = (path[:, 0] - path[anchor, 0] >= 3000) & (np.linalg.norm(path[:, 1:4] - path[anchor, 1:4], axis=1) <= 25)
    stationary_spells = int(np.count_nonzero(stationary & ~np.r_[False, stationary[:-1]]))
    reason = episode.get("failure") or "path_timeout"
    if episode["success"]:
        reason = "arrived"
    elif reason.startswith("no_path:"):
        reason = "no_path"
    elif episode["elapsed_ms"] < episode.get("limit_ms", 240000):
        reason = "actor_down"
    elif tail_path <= 50:
        reason = "no_motion"
    elif tail_displacement <= 50:
        reason = "local_motion"
    else:
        reason = "moving_at_timeout"
    result = {key: episode.get(key) for key in ("index", "split", "split_index", "region", "start", "goal",
              "success", "elapsed_ms", "decisions", "distance_cm", "failure", "teacher_elapsed_ms",
              "teacher_episode", "teacher_success", "source_cache_id", "final_position", "target_position",
              "blocked_spans", "blocked_ms", "max_blocked_ms", "teacher_beta", "mixing_seed",
              "teacher_control_decisions", "dagger_collection")}
    result.update(episode=str(directory), elapsed_s=episode["elapsed_ms"] / 1000, observed_reason=reason,
                  travelled_xz_cm=travelled, last_10s_span_cm=tail_span, last_10s_path_cm=tail_path,
                  last_10s_displacement_cm=tail_displacement, stationary_spells_3s=stationary_spells,
                  disagreement_counts=dict(counts), disagreement_fraction={key: counts[key] / steps for key in (*HEADS, "any")},
                  action_change_steps=teacher_change_steps, action_change_correct_counts=dict(change_correct),
                  action_change_accuracy={key: change_correct[key] / teacher_change_steps if teacher_change_steps else None
                                          for key in (*HEADS, "all_heads")},
                  action_counts=action_counts, action_fraction={key: value / steps for key, value in action_counts.items()},
                  action_changes=action_changes,
                  action_repeat_fraction=1 - action_changes["any"] / (steps - 1) if steps > 1 else None,
                  action_runs=len(run_lengths), longest_action_run_steps=int(run_lengths.max()),
                  final_action_run_steps=int(run_lengths[-1]),
                  final_action={head: int(applied[-1, column]) for column, head in enumerate(HEADS)})
    if last is not None:
        result["last_body_flags"] = last["body"]["flags"].tolist()
        result["last_teacher_path_cm"] = float(last["teacher_hint"]["path_cm"])
    return result


def group_metrics(episodes):
    success = [ep for ep in episodes if ep["success"]]
    steps = sum(ep["decisions"] for ep in episodes)
    counts = {key: sum(ep["disagreement_counts"].get(key, 0) for ep in episodes) for key in (*HEADS, "any")}
    teacher_change_steps = sum(ep["action_change_steps"] for ep in episodes)
    change_correct = {key: sum(ep["action_change_correct_counts"].get(key, 0) for ep in episodes)
                      for key in (*HEADS, "all_heads")}
    action_counts = {key: sum(ep["action_counts"][key] for ep in episodes) for key in episodes[0]["action_counts"]}
    action_changes = {key: sum(ep["action_changes"][key] for ep in episodes) for key in (*HEADS, "any")}
    transitions = sum(max(0, ep["decisions"] - 1) for ep in episodes)
    teacher_control = sum(ep.get("teacher_control_decisions") or 0 for ep in episodes)
    return dict(n=len(episodes), arrived=len(success), arrival_fraction=len(success) / len(episodes),
                teacher_beta_values=sorted({ep["teacher_beta"] for ep in episodes if ep.get("teacher_beta") is not None}),
                teacher_control_decisions=teacher_control, teacher_control_fraction=teacher_control / steps,
                dagger_collection=any(ep.get("dagger_collection") for ep in episodes),
                median_elapsed_s=statistics.median(ep["elapsed_s"] for ep in episodes),
                median_arrival_s=statistics.median(ep["elapsed_s"] for ep in success) if success else None,
                teacher_arrived=sum(bool(ep.get("teacher_success")) for ep in episodes),
                teacher_median_arrival_s=statistics.median(ep["teacher_elapsed_ms"] / 1000 for ep in episodes
                    if ep.get("teacher_success") and ep.get("teacher_elapsed_ms") is not None)
                    if any(ep.get("teacher_success") and ep.get("teacher_elapsed_ms") is not None for ep in episodes) else None,
                blocked_spans=sum(ep.get("blocked_spans") or 0 for ep in episodes),
                blocked_ms=sum(ep.get("blocked_ms") or 0 for ep in episodes),
                max_blocked_ms=max(ep.get("max_blocked_ms") or 0 for ep in episodes),
                stationary_spells_3s=sum(ep["stationary_spells_3s"] for ep in episodes),
                stationary_episodes=sum(ep["stationary_spells_3s"] > 0 for ep in episodes),
                failures=dict(Counter(ep["observed_reason"] for ep in episodes if not ep["success"])),
                decisions=steps, disagreement_counts=counts,
                disagreement_fraction={key: counts[key] / steps for key in counts},
                action_change_steps=teacher_change_steps, action_change_correct_counts=change_correct,
                action_change_accuracy={key: value / teacher_change_steps if teacher_change_steps else None
                                        for key, value in change_correct.items()},
                action_counts=action_counts, action_fraction={key: value / steps for key, value in action_counts.items()},
                action_changes=action_changes,
                action_repeat_fraction=1 - action_changes["any"] / transitions if transitions else None,
                action_runs=sum(ep["action_runs"] for ep in episodes),
                longest_action_run_steps=max(ep["longest_action_run_steps"] for ep in episodes),
                median_final_action_run_steps=statistics.median(ep["final_action_run_steps"] for ep in episodes))


def analyze(args):
    demos = module("nav_demos")
    paths = sorted((args.output / "dagger").glob("*/*/episode.json"))
    if not paths:
        raise ValueError("No completed episodes")
    episodes = [episode_metrics(path.parent, demos) for path in paths]
    groups, regions = {}, {}
    for split in sorted({ep["split"] for ep in episodes}):
        group = [ep for ep in episodes if ep["split"] == split]
        groups[split] = group_metrics(group)
        regions[split] = {region: group_metrics([ep for ep in group if ep["region"] == region])
                          for region in sorted({ep["region"] for ep in group})}
    result = dict(stationary_definition="Consecutive 3 s 3D endpoint-displacement windows <=25 cm, merged into spells",
                  failure_definition="no_path first; early stop actor_down; last <=10 s 3D path <=50 cm no_motion; path >50 cm and 3D endpoint displacement <=50 cm local_motion; otherwise moving_at_timeout",
                  action_change_definition="Teacher's six categorical heads differ from the previous teacher decision; first decision excluded; axes use sign, flags use nonzero; accuracy compares applied and teacher heads",
                  action_repeat_definition="Identical consecutive applied six-head decisions, excluding the first decision and terminal reset; turn_only means turn with neither forward nor strafe",
                  groups=groups, by_region=regions, all=group_metrics(episodes), episodes=episodes)
    destination = args.metrics or args.output / "metrics.json"
    write_json(destination, result)
    table = ["| Выборка | Место | Дошло | Время успеха, медиана, с | Учитель, с | Застревания клавишами | Неподвижные эпизоды ≥3 с |",
             "|---|---|---:|---:|---:|---:|---:|"]
    for split in ("validation", "train", "control"):
        if split not in groups:
            continue
        for region, value in [("все", groups[split]), *regions[split].items()]:
            def number(v):
                return "—" if v is None else f"{v:.2f}"
            table.append(f"| {split} | {region} | {value['arrived']}/{value['n']} | {number(value['median_arrival_s'])} | "
                         f"{number(value['teacher_median_arrival_s'])} | {value['blocked_spans']} | {value['stationary_episodes']} |")
    destination.with_suffix(".md").write_text("\n".join(table) + "\n", encoding="utf-8")
    print(json.dumps(groups, ensure_ascii=False, indent=2))


def plot(args):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np
    metrics = read_json(args.metrics or args.output / "metrics.json")
    candidates = [ep for ep in metrics["episodes"] if ep.get("teacher_episode")]
    if args.indices:
        wanted = [int(i) for i in args.indices.split(",")]
        lookup = {ep["index"]: ep for ep in candidates}
        selected = [lookup[i] for i in wanted]
    else:
        selected, used_regions = [], set()
        # Include both outcomes when there are successful drives, then favour
        # failed held-out routes from different places.
        for success, cap in ((True, 2), (False, args.plot_count)):
            group = sorted((ep for ep in candidates if bool(ep["success"]) == success),
                           key=lambda ep: (ep["split"] != "validation", ep["region"], ep["index"]))
            kept = 0
            for ep in group:
                if ep["region"] in used_regions or len(selected) >= args.plot_count or kept >= cap:
                    continue
                selected.append(ep)
                used_regions.add(ep["region"])
                kept += 1
        for ep in candidates:
            if len(selected) >= args.plot_count:
                break
            if ep not in selected:
                selected.append(ep)
    if not selected:
        raise ValueError("No teacher/learner trajectory comparisons available")
    fig, axes = plt.subplots(len(selected), 2, figsize=(9, 3.8 * len(selected)), squeeze=False)
    for row, ep in enumerate(selected):
        teacher = trajectory(Path(ep["teacher_episode"]))[:, [1, 3]] / 100
        learner = trajectory(Path(ep["episode"]))[:, [1, 3]] / 100
        target = ep.get("target_position")
        bounds = [teacher, learner]
        if target is not None:
            bounds.append(np.array([[target[0], target[2]]]) / 100)
        combined = np.concatenate(bounds)
        lo, hi = combined.min(axis=0), combined.max(axis=0)
        center = (lo + hi) / 2
        span = max(float(np.max(hi - lo)), 5) * 1.12
        for column, (path, title, color) in enumerate(((teacher, "Teacher", "#278246"), (learner, "Learner", "#bc4a32"))):
            ax = axes[row, column]
            ax.plot(path[:, 0], path[:, 1], color=color, lw=1.6)
            ax.scatter(*path[0], color="#26354a", marker="o", s=22, label="start", zorder=4)
            ax.scatter(*path[-1], color=color, marker="s", s=22, label="finish", zorder=4)
            if target is not None:
                ax.scatter(target[0] / 100, target[2] / 100, color="#d6a11a", marker="*", s=90, label="goal", zorder=5)
            ax.set_xlim(center[0] - span / 2, center[0] + span / 2)
            ax.set_ylim(center[1] - span / 2, center[1] + span / 2)
            ax.set_aspect("equal")
            ax.set_xlabel("World X, m")
            ax.set_ylabel("World Z, m")
            ax.grid(alpha=.2)
            if column == 0:
                detail = f"{'arrived' if ep.get('teacher_success') else 'failed'}, {ep['teacher_elapsed_ms'] / 1000:.1f} s"
            else:
                detail = f"{'arrived' if ep['success'] else ep['observed_reason']}, {ep['elapsed_s']:.1f} s"
            ax.set_title(f"{title}: {detail}", fontsize=10)
            ax.legend(fontsize=7, loc="best")
        axes[row, 0].text(0, 1.16, f"{ep['split']} #{ep['split_index']:03d} · {ep['region']}\n{ep['start']} → {ep['goal']}",
                          transform=axes[row, 0].transAxes, fontsize=9)
    fig.tight_layout(h_pad=3.8)
    destination = args.figure or args.output / "trajectories.png"
    destination.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(destination, dpi=160, bbox_inches="tight")
    plt.close(fig)
    print(json.dumps(dict(figure=str(destination), indices=[ep["index"] for ep in selected])))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "collect", "speed", "analyze", "plot"))
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--pairs", type=Path)
    parser.add_argument("--bc-cache", type=Path, default=Path.home() / "gothic-ai/nav-bc/cache")
    parser.add_argument("--teacher-root", type=Path, default=Path.home() / "gothic-ai/nav-demos-v2")
    parser.add_argument("--selection-seed", type=int, default=17)
    parser.add_argument("--roads", type=Path, default=REPO / "server/src/gothic_ai/gamedata/places.json")
    parser.add_argument("--links", type=Path, default=REPO / "reports/navigation/2026-10-16-teacher/raw/accepted-links.txt")
    parser.add_argument("--weights", type=Path, default=Path.home() / "gothic-ai/nav-bc/run/policy.navbc")
    for name in ("runner", "assets", "save", "cache", "ini", "metrics", "figure"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--limit-ms", type=int, help="default: 13000 for speed, otherwise 240000")
    parser.add_argument("--populations", default="20,50", help="Comma-separated policy populations for speed")
    parser.add_argument("--watchdog-seconds", type=float, default=900)
    parser.add_argument("--split", choices=("all", "validation", "train", "control"), default="all")
    parser.add_argument("--indices")
    parser.add_argument("--start-index", type=int, default=0)
    parser.add_argument("--count", type=int)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--list-indices", action="store_true")
    parser.add_argument("--teacher-beta", type=float, default=0,
                        help="Per-decision probability of applying teacher keys; measured drives require 0")
    parser.add_argument("--mixing-seed", type=int, default=1701,
                        help="Action sampling and mixed-control RNG seed, offset by the pair index")
    parser.add_argument("--action-mode", choices=("argmax", "sample"), default="argmax",
                        help="Drive policy action selection; argmax preserves the existing measured drive")
    parser.add_argument("--dagger-collection", action="store_true", help="Mark this run as DAgger collection")
    parser.add_argument("--plot-count", type=int, default=6)
    parser.add_argument("--physics", choices=("native", "cv"), default="native")
    args = parser.parse_args()
    if args.limit_ms is None:
        args.limit_ms = 13000 if args.command == "speed" else 240000
    for key, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, key, value.expanduser().resolve())
    if args.start_index < 0 or (args.count is not None and args.count < 1) or args.limit_ms < 1000 or not 4 <= args.plot_count <= 6 or args.mixing_seed < 0:
        parser.error("start-index >=0, count >=1, limit-ms >=1000 and plot-count 4..6 required")
    try:
        {"prepare": prepare, "collect": collect, "speed": speed, "analyze": analyze, "plot": plot}[args.command](args)
    except (ValueError, OSError, RuntimeError, KeyError) as exc:
        parser.exit(1, f"nav-drive: {exc}\n")


if __name__ == "__main__":
    main()
