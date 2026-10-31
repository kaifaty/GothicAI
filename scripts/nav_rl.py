#!/usr/bin/env python3
"""Collect navigation populations and freeze a teacher-qualified final test set.

Runner commands belong inside one caller-owned gq batch request. Paths passed to
body-runner are absolute. This module does not start an optimizer or choose a
checkpoint from the final test set. Generation, teacher qualification and drive
measurement are separate commands so a measured test set cannot be reselected.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict, deque
from concurrent.futures import ThreadPoolExecutor
from copy import copy
import csv
import gzip
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import random
import shutil
import statistics
import sys
import tempfile
import threading
import time

REPO = Path(__file__).resolve().parent.parent
REGIONS = ("market", "harbour", "onar", "lobart", "port_streets", "upper_streets")
HEADS = ("forward", "strafe", "turn", "walk", "jump", "climb")
CLASSES = (3, 3, 3, 2, 2, 2)
REWARDS = ("progress_cm", "arrived", "blocked", "fall_damage", "death", "jump", "climb", "time_ms")
VALIDATION_FAMILIES = ("point", "object", "approach", "follow_below_gate", "jump")
DAGGER_FAMILIES = (*VALIDATION_FAMILIES, "follow", "water")
ACTION_SEEDS = (18102027, 18102028, 18102029)


def module(name):
    spec = importlib.util.spec_from_file_location(name, REPO / "scripts" / f"{name}.py")
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def rows(path):
    path = Path(path)
    opener = gzip.open if path.suffix == ".gz" else open
    with opener(path, "rt", encoding="utf-8") as stream:
        for number, line in enumerate(stream, 1):
            if line.strip():
                try:
                    yield json.loads(line)
                except ValueError as exc:
                    raise ValueError(f"Invalid JSON at {path}:{number}") from exc


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def write_pairs(path, pairs):
    """Freeze a selection: reruns may reuse it but cannot silently replace it."""
    path = Path(path)
    text = "".join(json.dumps(pair, ensure_ascii=False, sort_keys=True) + "\n" for pair in pairs)
    if path.exists() and path.read_text(encoding="utf-8") != text:
        raise ValueError(f"Refusing to replace a different pair selection: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def load_pairs(path, *, deduplicate=False):
    """Keep final selections strict; raw bench candidates may repeat point names."""
    pairs = []
    seen = set()
    for index, pair in enumerate(rows(path)):
        if not isinstance(pair, dict) or not all(
                isinstance(pair.get(key), str) and pair[key] and not any(c.isspace() for c in pair[key])
                for key in ("region", "start", "goal")):
            raise ValueError(f"Invalid named pair {index}: {pair}")
        key = pair["start"], pair["goal"]
        if key[0] == key[1]:
            raise ValueError(f"Duplicate or identical pair at {index}: {key}")
        if key in seen:
            if deduplicate:
                continue
            raise ValueError(f"Duplicate or identical pair at {index}: {key}")
        seen.add(key)
        pairs.append(pair)
    if not pairs:
        raise ValueError(f"No pairs in {path}")
    return pairs


def pair_keys(value):
    """Read historical selections, cache indices and metrics without split assumptions."""
    if isinstance(value, dict):
        start, goal = value.get("start"), value.get("goal")
        if isinstance(start, str) and isinstance(goal, str):
            yield start, goal
        pair = value.get("pair")
        if isinstance(pair, (list, tuple)) and len(pair) == 2 and all(isinstance(p, str) for p in pair):
            yield tuple(pair)
        for child in value.values():
            if isinstance(child, (dict, list)):
                yield from pair_keys(child)
    elif isinstance(value, list):
        for child in value:
            yield from pair_keys(child)


def historical_pairs(paths):
    """Exclude both directions, including all historical attempts, not only successes."""
    result = set()
    for path in paths:
        path = Path(path)
        if not path.exists():
            raise ValueError(f"Missing exclusion source: {path}")
        sources = sorted(p for p in path.rglob("*") if p.suffix in (".json", ".jsonl", ".csv")) if path.is_dir() else [path]
        for source in sources:
            if source.suffix == ".csv":
                with source.open(encoding="utf-8") as stream:
                    values = list(csv.DictReader(stream))
            elif source.suffix == ".jsonl":
                values = list(rows(source))
            elif source.suffix == ".json":
                values = read_json(source)
            else:
                raise ValueError(f"Unsupported exclusion source: {source}")
            for start, goal in pair_keys(values):
                result.update(((start, goal), (goal, start)))
    return result


def select_pairs(candidates, excluded, per_region, seed):
    if per_region < 1:
        raise ValueError("per-region must be positive")
    groups = defaultdict(dict)
    for pair in candidates:
        key = pair["start"], pair["goal"]
        if pair["region"] not in REGIONS:
            raise ValueError(f"Unknown region {pair['region']}")
        if key not in excluded and key[::-1] not in excluded:
            groups[pair["region"]].setdefault(key, pair)
    generator = random.Random(seed)
    selected = []
    used = set(excluded)
    for region in REGIONS:
        candidates = sorted(groups[region].values(), key=lambda pair: (pair["start"], pair["goal"]))
        generator.shuffle(candidates)
        chosen = []
        for pair in candidates:
            key = pair["start"], pair["goal"]
            if key in used or key[::-1] in used:
                continue
            chosen.append(dict(pair))
            used.update((key, key[::-1]))
            if len(chosen) == per_region:
                break
        if len(chosen) != per_region:
            raise ValueError(f"Need {per_region} unused pairs in {region}, found {len(chosen)}")
        selected.extend(chosen)
    for index, pair in enumerate(selected):
        pair.update(index=index, selection_seed=seed)
    return selected


def runner_pairs(path, pairs):
    path = Path(path).resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(f"{p['start']} {p['goal']}\n" for p in pairs), encoding="utf-8")
    return path


def iter_samples(path):
    """Yield one selected action and its unweighted reward components per decision."""
    path = Path(path)
    if path.is_dir():
        path = path / "samples.jsonl"
    for index, sample in enumerate(rows(path)):
        if not isinstance(sample, dict):
            raise ValueError(f"Sample {index} must be an object")
        yield sample


def categorical_log_probs(logits, actions):
    """Stable NumPy reference for independent policy heads; returns per-head log p."""
    import numpy as np
    actions = np.asarray(actions)
    if actions.shape != (len(HEADS),) or not np.issubdtype(actions.dtype, np.integer):
        raise ValueError("action_indices must contain six integers")
    if isinstance(logits, dict):
        heads = [np.asarray(logits[name], dtype=np.float64) for name in HEADS]
    else:
        flat = np.asarray(logits, dtype=np.float64)
        if flat.shape != (sum(CLASSES),):
            raise ValueError("logits must have 15 values or six named heads")
        heads = np.split(flat, np.cumsum(CLASSES)[:-1])
    result = []
    for name, classes, values, action in zip(HEADS, CLASSES, heads, actions):
        if values.shape != (classes,) or not np.isfinite(values).all() or not 0 <= action < classes:
            raise ValueError(f"Invalid {name} logits/action")
        shifted = values - values.max()
        result.append(float(shifted[action] - np.log(np.exp(shifted).sum())))
    return np.asarray(result)


def check_episode(path, tolerance=1e-4, progress_tolerance=.05):
    """Validate C++ sampling math, transition accounting and NAVDEMO alignment."""
    import numpy as np
    path = Path(path)
    episode = read_json(path / "episode.json")
    sums = Counter()
    maximum_error = 0.0
    count = 0
    last_time = None
    first_path = last_path = None
    stamps = []
    selections = []
    last_end = None
    invalid_paths = 0
    ended = False
    for sample in iter_samples(path):
        expected = categorical_log_probs(sample["logits"], sample["action_indices"])
        selections.append(sample["action_indices"])
        actual = np.asarray(sample["log_probs"], dtype=np.float64)
        total = float(sample["log_prob"])
        if actual.shape != expected.shape or not np.isfinite(actual).all() or not math.isfinite(total):
            raise ValueError(f"Invalid log-prob in {path} sample {count}")
        maximum_error = max(maximum_error, float(np.max(np.abs(expected - actual))), abs(float(expected.sum()) - total))
        if maximum_error > tolerance:
            raise ValueError(f"Log-prob error {maximum_error:g} > {tolerance:g} in {path}")
        if ended:
            raise ValueError(f"Sample follows terminal transition in {path}")
        stamp = sample["time_ms"]
        if not isinstance(stamp, int) or stamp < 0 or (last_time is not None and stamp <= last_time):
            raise ValueError(f"Nonincreasing sample time in {path}")
        end = sample["next_time_ms"]
        if not isinstance(end, int) or not 0 < end - stamp <= 100 or (last_end is not None and stamp != last_end):
            raise ValueError(f"Invalid transition interval in {path}")
        stamps.append(stamp)
        last_time, last_end = stamp, end
        if not isinstance(sample["terminated"], bool) or not isinstance(sample["truncated"], bool) or (sample["terminated"] and sample["truncated"]):
            raise ValueError(f"Invalid terminal flags in {path}")
        ended = sample["terminated"] or sample["truncated"]
        reward = sample["rewards"]
        if set(reward) != set(REWARDS) or any(not math.isfinite(float(v)) for v in reward.values()):
            raise ValueError(f"Invalid reward components in {path}")
        if reward["time_ms"] != end - stamp or any(reward[key] < 0 for key in REWARDS if key != "progress_cm"):
            raise ValueError(f"Invalid unsigned reward component in {path}")
        before, after = float(sample["path_before_cm"]), float(sample["path_after_cm"])
        if not math.isfinite(before) or not math.isfinite(after) or before < 0 or after < 0:
            raise ValueError(f"Invalid reward path lengths in {path}")
        valid = sample.get("path_valid", True)
        invalid_paths += not valid
        if valid and abs((before - after) - reward["progress_cm"]) > .1:
            raise ValueError(f"Progress is not the Detour path difference in {path}")
        if first_path is None:
            first_path = before
        elif valid and abs(last_path - before) > .1:
            raise ValueError(f"Discontinuous reward path lengths in {path}")
        last_path = after
        sums.update(reward)
        count += 1
    if count != episode["decisions"]:
        raise ValueError(f"Sample/decision count differs in {path}: {count}/{episode['decisions']}")
    if count and (not ended or last_end - stamps[0] != episode["elapsed_ms"]):
        raise ValueError(f"Final transition does not close episode time in {path}")
    progress_error = abs(sums["progress_cm"] - first_path) / first_path if first_path else None
    if episode.get("success") and (progress_error is None or progress_error > progress_tolerance):
        raise ValueError(f"Successful progress relative error {progress_error} > {progress_tolerance} in {path}")
    actions = np.loadtxt(path / "actions.txt", ndmin=2)
    if actions.shape != (count + 1, 9) or not np.isfinite(actions).all():
        raise ValueError(f"Invalid applied-action rows in {path}")
    selected_keys = np.asarray(selections, dtype=np.float64).reshape(count, 6)
    selected_keys[:, :3] -= 1
    if not np.array_equal(selected_keys, actions[:-1, 1:7]):
        raise ValueError(f"Selected action differs from applied keys in {path}")
    if count and (not np.array_equal(actions[:-1, 0], np.asarray(stamps) - stamps[0])
                  or actions[-1, 0] != episode["elapsed_ms"]):
        raise ValueError(f"Applied-action timestamps differ in {path}")
    demos = module("nav_demos")
    records = 0
    for data in demos.iter_chunks(path):
        if data["time_ms"].tolist() != stamps[records:records + len(data)]:
            raise ValueError(f"NAVDEMO/sample timestamps differ in {path}")
        records += len(data)
    if demos.episode_version(path) != 2 or records != count:
        raise ValueError(f"NAVDEMO v2/sample count differs in {path}")
    return dict(episode=str(path), decisions=count, max_log_prob_error=maximum_error,
                reward_sums=dict(sums), start_path_cm=first_path, final_path_cm=last_path, invalid_reward_paths=invalid_paths,
                successful_progress_relative_error=progress_error if episode.get("success") else None)


def check(args):
    paths = [path.parent for path in sorted(args.source.rglob("samples.jsonl"))] if not (args.source / "samples.jsonl").is_file() else [args.source]
    if not paths:
        raise ValueError(f"No population samples in {args.source}")
    results = [check_episode(path, args.logprob_tolerance) for path in paths]
    result = dict(episodes=len(results), decisions=sum(ep["decisions"] for ep in results),
                  max_log_prob_error=max(ep["max_log_prob_error"] for ep in results), results=results)
    write_json(args.output / "check.json", result)
    print(json.dumps(result, ensure_ascii=False))


def checked_paths(args, policy=False):
    if sys.platform != "linux":
        raise ValueError("Run heavy navigation work on Ryzen/Linux under gq batch")
    for name in ("runner", "assets", "save", "cache", "links", "roads", *(('weights',) if policy else ())):
        path = getattr(args, name)
        if path is None or (name != "cache" and not path.exists()):
            raise ValueError(f"--{name} must exist")
    if policy:
        with args.weights.open("rb") as stream:
            line = stream.readline(4 * 1024 * 1024)
        if not line.endswith(b"\n"):
            raise ValueError("Weight file has no bounded JSON header line")
        preprocessing = json.loads(line).get("preprocessing", {})
        args.policy_input_variant = preprocessing.get("input_variant")
        args.navigation_memory_version = preprocessing.get("navigation_memory_version", 3)
        journey_override = os.environ.get("OPENGOTHIC_NAV_JOURNEY_MEMORY")
        if journey_override is not None:
            if journey_override not in ("0", "1"):
                raise ValueError("OPENGOTHIC_NAV_JOURNEY_MEMORY must be 0 or 1")
            args.navigation_memory_version = 4 if journey_override == "1" else 3
        version = preprocessing.get("navdemo_version", 1)
        args.navdemo_version = 3 if version == 2 and args.navigation_memory_version == 4 else version
        if args.navigation_memory_version == 4 and (args.policy_input_variant == "hd-familiar"
                or "familiar" in preprocessing.get("map_order", ())):
            familiar = getattr(args, "familiar_map", None)
            if familiar is None or not familiar.is_file():
                raise ValueError("HD familiar policy input requires --familiar-map")
    args.output.mkdir(parents=True, exist_ok=True)
    args.cache.mkdir(parents=True, exist_ok=True)
    os.environ["OPENGOTHIC_BODY_TRACE"] = "0"


def new_runner(args, name):
    collector = module("collect_nav_demos")
    worker = copy(args)
    worker.output = args.output / "runner-work" / name
    worker.output.mkdir(parents=True, exist_ok=True)
    scenario = collector.scenario_file(args, f"prepare-{name}", [f"navmesh build {args.cache} {args.links}"])
    runner = collector.runner_type()(worker, scenario)
    try:
        runner.scenario(scenario, worker.output / "prepare", args.seed, timeout=720)
        collector.check_log(worker.output / "prepare")
    except BaseException:
        runner.close()
        raise
    return runner


def scenario(args, runner, name, commands, directory):
    collector = module("collect_nav_demos")
    path = collector.scenario_file(args, name, commands)
    runner.scenario(path, directory, args.seed, timeout=args.watchdog_seconds)
    collector.check_log(directory)


def generate(args):
    checked_paths(args)
    excluded = historical_pairs(args.exclude)
    if not excluded:
        raise ValueError("Generation requires nonempty --exclude historical pairs")
    runner = new_runner(args, "generate")
    directory = args.output / "candidate-world"
    try:
        scenario(args, runner, "generate", [f"navmesh pairs {args.seed} {args.candidates_per_region}"], directory)
    finally:
        runner.close()
    candidates = load_pairs(directory / "pairs.jsonl", deduplicate=True)
    candidate_file = runner_pairs(args.output / "candidate-pairs.txt", candidates)
    reach_directory = args.output / "reachability"
    runner = new_runner(args, "reachability")
    try:
        scenario(args, runner, "reachability", [f"navdemos reachable {candidate_file}"], reach_directory)
    finally:
        runner.close()
    reachable = list(rows(reach_directory / "reachable.jsonl"))
    if len(reachable) != len(candidates):
        raise ValueError("Reachability result count differs from candidates")
    checked = []
    for index, (pair, reach) in enumerate(zip(candidates, reachable)):
        if (reach.get("pair_index") != index or (reach.get("start"), reach.get("goal")) != (pair["start"], pair["goal"])):
            raise ValueError(f"Reachability pair mismatch at {index}")
        if reach.get("complete") is True:
            checked.append(dict(pair, detour_complete=True, detour_length_cm=reach["length_cm"]))
    selected = select_pairs(checked, excluded, args.per_region, args.seed)
    destination = args.output / "pairs.jsonl"
    write_pairs(destination, selected)
    print(json.dumps(dict(pairs=str(destination), n=len(selected), seed=args.seed,
                          regions=dict(Counter(p["region"] for p in selected)),
                          historical_directed_pairs=len(excluded), source=str(directory / "pairs.jsonl"))))


def qualify_region(args, candidates, region, publish):
    """Visit one region in source order, independently of other runners' clocks."""
    runner = new_runner(args, f"teacher-{region}")
    chosen = []
    visited = 0
    try:
        for index, pair in enumerate(candidates):
            if pair["region"] != region:
                continue
            if visited >= getattr(args, "max_teacher_candidates", 50):
                break
            visited += 1
            directory = args.output / "teacher" / f"{index:05d}"
            if (directory / "episode.json").is_file():
                if not args.resume:
                    raise ValueError(f"Existing teacher attempt: {directory}; use --resume")
            else:
                scenario(args, runner, f"teacher-{index:05d}", [f"roads load {args.roads}", f"roads on {args.npc}",
                         f"navdemos start-v2 {args.npc} {pair['start']} {pair['goal']} {args.limit_ms}", "navdemos wait"], directory)
            if not all((directory / name).is_file() for name in ("episode.json", "samples.bin.gz", "trajectory.txt", "actions.txt")):
                raise ValueError(f"Incomplete teacher attempt: {directory}")
            episode = read_json(directory / "episode.json")
            if episode.get("physics_mode") != getattr(args, "physics", "cv"):
                raise ValueError(f"Teacher physics mode differs from requested mode: {directory}")
            if (episode.get("demo_version") != 2 or episode.get("teacher_version") != 2
                    or (episode.get("start"), episode.get("goal")) != (pair["start"], pair["goal"])):
                raise ValueError(f"Teacher attempt is not the requested v2 pair: {directory}")
            attempt = dict(candidate=index, region=region, start=pair["start"], goal=pair["goal"],
                           success=bool(episode["success"]), elapsed_ms=episode["elapsed_ms"], episode=str(directory))
            publish(attempt)
            if episode["success"]:
                chosen.append(dict(pair, index=index, teacher_episode=str(directory), teacher_success=True,
                                   teacher_elapsed_ms=episode["elapsed_ms"], teacher_version=2, demo_version=2,
                                   npc=args.npc, seed=args.seed))
            print(json.dumps(attempt), flush=True)
            if len(chosen) == args.per_region:
                break
    finally:
        runner.close()
    return chosen


def qualify(args):
    checked_paths(args)
    candidates = load_pairs(args.pairs)
    if any(pair["region"] not in REGIONS for pair in candidates):
        raise ValueError("Teacher qualification requires the six known regions")
    # No learner or weights are consulted before the complete selection is frozen.
    destination = args.output / "holdout.jsonl"
    if destination.exists():
        raise ValueError(f"Final test set already frozen: {destination}")
    attempts = {}
    lock = threading.Lock()
    def publish(attempt):
        with lock:
            attempts[attempt["candidate"]] = attempt
            write_json(args.output / "teacher-attempts.json", [attempts[index] for index in sorted(attempts)])
    workers = min(args.parallel, len(REGIONS))
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = [pool.submit(qualify_region, args, candidates, region, publish) for region in REGIONS]
        groups = [future.result() for future in futures]
    chosen = sorted((pair for group in groups for pair in group), key=lambda p: (REGIONS.index(p["region"]), p["index"]))
    for index, pair in enumerate(chosen):
        pair.update(index=index, split="validation", split_index=index, final_test=True)
    write_pairs(destination, chosen)
    counts = Counter(pair["region"] for pair in chosen)
    summary = dict(pairs=str(destination), n=len(chosen), regions={region: counts[region] for region in REGIONS},
                   requested_per_region=args.per_region, max_candidates_per_region=getattr(args, "max_teacher_candidates", 50),
                   shortages={region: args.per_region - counts[region] for region in REGIONS if counts[region] < args.per_region},
                   physics=getattr(args, "physics", "cv"), attempts=len(attempts), workers=workers)
    write_json(args.output / "qualification.json", summary)
    print(json.dumps(summary))


def drive(args):
    if getattr(args, "dagger_scenarios", None) is not None:
        return drive_dagger(args)
    if (getattr(args, "manifest", None) is not None or getattr(args, "detour_scenarios", None) is not None
            or getattr(args, "eval_screen", None) is not None):
        return drive_validation(args)
    pairs = load_pairs(args.pairs)
    if not all(pair.get("final_test") and pair.get("teacher_success") and pair.get("teacher_version") == 2 for pair in pairs):
        raise ValueError("drive requires a frozen teacher-qualified final test set")
    selected = list(range(len(pairs))) if args.indices is None else [int(value) for value in args.indices.split(",")]
    if len(selected) != len(set(selected)) or any(index < 0 or index >= len(pairs) for index in selected):
        raise ValueError("--indices must contain distinct valid pair indices")
    if not selected:
        raise ValueError("No final-test drive indices selected")
    workers = min(args.parallel, len(selected))
    def collect(worker_index):
        worker = copy(args)
        # Existing nav_drive.collect owns all mutable workdirs and scenario files
        # under its output. Each worker gets a disjoint subtree and original IDs.
        if args.parallel > 1:
            worker.output = args.output / "drive-workers" / f"w{worker_index}"
        worker.command = "collect"
        worker.action_mode = getattr(args, "action_mode", "sample")
        worker.teacher_beta = 0
        worker.dagger_collection = False
        worker.split = "validation"
        worker.list_indices = False
        worker.selection_seed = args.seed
        worker.mixing_seed = args.seed
        worker.indices = ",".join(map(str, selected[worker_index::workers]))
        worker.start_index = 0
        worker.count = None
        module("nav_drive").collect(worker)
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = [pool.submit(collect, index) for index in range(workers)]
        for future in futures:
            future.result()


def validation_scenarios(manifest, families=None):
    """Read only the frozen validation scenarios; never inspect train/test members."""
    value = read_json(manifest)
    if value.get("schema_version") != 1:
        raise ValueError("Unsupported demonstration manifest schema")
    groups = value.get("splits", {}).get("validation")
    if not isinstance(groups, dict):
        raise ValueError("Manifest must contain splits.validation scenarios")
    requested = VALIDATION_FAMILIES if families is None else tuple(families.split(","))
    if not requested or len(set(requested)) != len(requested) or set(requested) - set(VALIDATION_FAMILIES):
        raise ValueError("--families must contain distinct validation family names")
    result, seen = [], set()
    for family in requested:
        values = groups.get(family)
        if not isinstance(values, list) or not values:
            raise ValueError(f"Missing validation family: {family}")
        for original in values:
            if (not isinstance(original, dict) or original.get("split") != "validation"
                    or original.get("family") != family or original.get("recorded") is not False
                    or original.get("episode") is not None):
                raise ValueError(f"Invalid unrecorded validation scenario in {family}")
            value = dict(original)
            for key in ("id", "region", "start", "goal"):
                token = value.get(key)
                if not isinstance(token, str) or not token or any(c.isspace() for c in token):
                    raise ValueError(f"Invalid validation scenario token: {key}")
            identifier = value["id"]
            if Path(identifier).name != identifier or identifier in (".", "..") or identifier in seen:
                raise ValueError(f"Unsafe or duplicate validation scenario ID: {identifier}")
            if value.get("goal_kind") not in ("point", "object", "approach", "follow"):
                raise ValueError(f"Unknown validation goal kind: {identifier}")
            if type(value.get("seed")) is not int or not 0 <= value["seed"] <= 0xffffffff:
                raise ValueError(f"Invalid validation scenario seed: {identifier}")
            seen.add(identifier)
            result.append(value)
    return result


def detour_scenarios(path):
    """Read a frozen geometry-only validation cohort without inspecting test."""
    result, seen = [], set()
    for original in rows(path):
        if (not isinstance(original, dict) or original.get("split") != "validation"
                or original.get("family") != "detour" or original.get("goal_kind") not in ("point", "object")):
            raise ValueError("Detour drive requires validation point/object scenarios")
        for key in ("id", "region", "start", "goal"):
            token = original.get(key)
            if not isinstance(token, str) or not token or any(c.isspace() for c in token):
                raise ValueError(f"Invalid detour scenario token: {key}")
        identifier = original["id"]
        if Path(identifier).name != identifier or identifier in (".", "..") or identifier in seen:
            raise ValueError(f"Unsafe or duplicate detour scenario ID: {identifier}")
        if type(original.get("seed")) is not int or not 0 <= original["seed"] <= 0xffffffff:
            raise ValueError(f"Invalid detour scenario seed: {identifier}")
        geometry = original.get("geometry")
        if not isinstance(geometry, dict) or any(geometry.get(key) != original[key] for key in ("start", "goal")):
            raise ValueError(f"Detour geometry identity differs: {identifier}")
        if geometry.get("label") != identifier:
            raise ValueError(f"Detour geometry label differs: {identifier}")
        points = [geometry.get(key) for key in ("start_cm", "goal_cm")]
        length = geometry.get("length_cm")
        if (any(not isinstance(point, list) or len(point) != 3
                or any(type(value) not in (int, float) or not math.isfinite(value) for value in point) for point in points)
                or type(length) not in (int, float) or not math.isfinite(length)):
            raise ValueError(f"Invalid detour geometry lengths: {identifier}")
        direct = math.dist(*points)
        if (geometry.get("complete") is not True or geometry.get("straight_walkable") is not False
                or direct <= 0 or length < 1.3 * direct):
            raise ValueError(f"Unqualified detour geometry: {identifier}")
        if original["goal_kind"] == "object" and original.get("visibility") is not True:
            raise ValueError(f"Detour object was not prequalified as visible: {identifier}")
        seen.add(identifier)
        result.append(dict(original))
    if not result:
        raise ValueError("No frozen detour validation scenarios")
    return result


def action_seeds(value):
    try:
        seeds = list(ACTION_SEEDS) if value is None else [int(item) for item in value.split(",")]
    except ValueError as exc:
        raise ValueError("--action-seeds must contain distinct uint32 seeds") from exc
    if not seeds or len(set(seeds)) != len(seeds) or any(not 0 <= seed <= 0xffffffff for seed in seeds):
        raise ValueError("--action-seeds must contain distinct uint32 seeds")
    return seeds


def eval_screen_jobs(path):
    """Read the frozen validation screen without inspecting another split."""
    jobs, seen = [], set()
    for index, entry in enumerate(rows(path)):
        if not isinstance(entry, dict) or set(entry) != {"scenario", "action_seeds"}:
            raise ValueError("Invalid frozen evaluation screen entry")
        value, seeds = entry["scenario"], entry["action_seeds"]
        if (not isinstance(seeds, list) or seeds != list(ACTION_SEEDS)
                or any(type(seed) is not int for seed in seeds)):
            raise ValueError("Evaluation screen requires the three frozen action seeds")
        if not isinstance(value, dict) or value.get("split") != "validation":
            raise ValueError("Evaluation screen requires validation-only scenarios")
        family = value.get("family")
        kinds = {"point": ("point",), "object": ("object",), "approach": ("approach",),
                 "follow_below_gate": ("follow",), "jump": ("point",), "safety": ("point",),
                 "detour": ("point", "object")}
        if not isinstance(family, str) or family not in kinds or value.get("goal_kind") not in kinds[family]:
            raise ValueError("Invalid evaluation screen validation family or goal kind")
        for key in ("id", "region", "start", "goal"):
            token = value.get(key)
            if not isinstance(token, str) or not token or any(c.isspace() for c in token):
                raise ValueError(f"Invalid evaluation screen scenario token: {key}")
        identifier = value["id"]
        identity = family, identifier
        if Path(identifier).name != identifier or identifier in (".", "..") or identity in seen:
            raise ValueError(f"Unsafe or duplicate evaluation screen scenario ID: {identifier}")
        if type(value.get("seed")) is not int or not 0 <= value["seed"] <= 0xffffffff:
            raise ValueError(f"Invalid evaluation screen scenario seed: {identifier}")
        if family in VALIDATION_FAMILIES and (value.get("recorded") is not False or value.get("episode") is not None):
            raise ValueError(f"Evaluation screen requires unrecorded validation scenarios: {identifier}")
        seen.add(identity)
        jobs.extend((index, value, seed) for seed in seeds)
    if not jobs:
        raise ValueError("No frozen evaluation screen scenarios")
    return jobs


def validation_commands(args, value, action_seed):
    """Keep world/target randomness frozen while varying only policy sampling."""
    if value.get("split") != "validation":
        raise ValueError("Policy validation cannot run another split")
    return policy_commands(args, value, action_seed)


def policy_commands(args, value, action_seed, *, teacher_beta=0, dagger=False):
    kind, npc = value["goal_kind"], args.npc
    limit = value.get("limit_ms", args.limit_ms)
    if type(limit) is not int or limit <= 0:
        raise ValueError("Policy scenario limit must be positive")
    suffix = f"{limit} {args.weights} {teacher_beta:g} {action_seed} sample"
    record = "off" if not dagger and (getattr(args, "lean", False) or value.get("family") == "detour") else "on"
    roads_mode = "stranger" if getattr(args, "stranger", False) or (dagger and value.get("local") is False) else "on"
    commands = [f"roads load {args.roads}", f"roads {roads_mode} {npc}", f"navdemos record {record}",
                "navdemos perception-mode gaze", "navdemos oracle off"]
    if getattr(args, "familiar_map", None) is not None:
        commands.insert(1, f"roads familiar-load {args.familiar_map}")
    if dagger:
        teacher = 2 if value["family"] == "jump" else 3
        commands.extend([f"navdemos drive-teacher {teacher}", "navdemos dagger on"])
    if dagger or value.get("family") == "safety":
        commands.append("navdemos perception-monitor on")
    if value.get("fence_control"):
        commands.extend(["goto NW_FARM1_ENTRANCE_03", f"put {npc} 0 0 -1400",
                         "goto NW_FARM1_ENTRANCE_01", "wait 1000",
                         f"navdemos drive-current {npc} {value['goal']} {suffix}"])
    elif kind in ("follow", "approach"):
        route = value.get("route", [])
        if (not isinstance(route, list) or len(route) < (3 if kind == "follow" else 2)
                or any(not isinstance(name, str) or not name or any(c.isspace() for c in name) or "," in name for name in route)):
            raise ValueError("Invalid frozen moving-target route")
        target = value.get("target")
        if not isinstance(target, str) or not target or any(c.isspace() for c in target):
            raise ValueError("Invalid frozen moving-target symbol")
        commands.append(f"navdemos drive-moving {npc} {value['start']} {target} {','.join(route)} "
                        f"{kind} {limit} {value['seed']} {args.weights} {teacher_beta:g} {action_seed} sample")
    else:
        command = "drive-object" if kind == "object" else "drive"
        watch = ""
        if value.get("family") == "jump":
            identifier = value.get("transition_id")
            if type(identifier) is not int or identifier <= 0:
                raise ValueError("Jump validation needs its frozen transition ID")
            watch = f" {identifier}"
        commands.append(f"navdemos {command} {npc} {value['start']} {value['goal']} {suffix}{watch}")
    return [*commands, "navdemos wait"]


def dagger_scenarios(path):
    """Accept explicit train scenarios only, including unused frozen train pools."""
    result, seen = [], set()
    for original in rows(path):
        if (not isinstance(original, dict) or original.get("split") != "train"
                or original.get("family") not in DAGGER_FAMILIES
                or original.get("goal_kind") not in ("point", "object", "approach", "follow")
                or original.get("final_test") or original.get("fence_control")):
            raise ValueError("DAgger recording requires train point/object/approach/follow/jump/water scenarios")
        for key in ("id", "region", "start", "goal"):
            token = original.get(key)
            if not isinstance(token, str) or not token or any(c.isspace() for c in token):
                raise ValueError(f"Invalid DAgger scenario token: {key}")
        identifier = original["id"]
        if Path(identifier).name != identifier or identifier in (".", "..") or identifier in seen:
            raise ValueError(f"Unsafe or duplicate DAgger scenario ID: {identifier}")
        if type(original.get("seed")) is not int or not 0 <= original["seed"] <= 0xffffffff:
            raise ValueError(f"Invalid DAgger scenario seed: {identifier}")
        npc = original.get("npc")
        if npc is not None and (not isinstance(npc, str) or not npc or any(c.isspace() for c in npc)):
            raise ValueError(f"Invalid DAgger scenario NPC: {identifier}")
        seen.add(identifier)
        result.append(dict(original))
    if not result:
        raise ValueError("No train DAgger scenarios")
    return result


def dagger_control_metrics(root):
    """Count every visited decision, including masked labels and DART steps."""
    reasons = ("beta", "stall", "memory_danger", "heading")
    groups = defaultdict(lambda: Counter({reason: 0 for reason in reasons}))
    for path in sorted(Path(root).glob("dagger/*/*/seed-*/episode.json")):
        episode = read_json(path)
        family = episode["family"]
        counts = Counter(episodes=1, dagger_interventions=episode["dagger_interventions"])
        for sample in rows(path.parent / "samples.jsonl"):
            controlled, dart, masked = (sample[key] for key in ("teacher_control", "dart", "maneuver_mask"))
            reason = sample["takeover"]
            if (any(type(value) is not bool for value in (controlled, dart, masked))
                    or (controlled and reason not in reasons) or (not controlled and (reason or dart))):
                raise ValueError(f"Invalid DAgger control metadata: {path.parent}")
            counts["decisions"] += 1
            counts["teacher_control_decisions"] += controlled
            counts["dart_decisions"] += dart
            counts["maneuver_masked_decisions"] += masked
            if controlled:
                counts[reason] += 1
        for key in ("decisions", "teacher_control_decisions", "dart_decisions", "maneuver_masked_decisions"):
            if counts[key] != episode[key]:
                raise ValueError(f"DAgger {key} count differs: {path.parent}")
        for group in ("all", family):
            groups[group].update(counts)
    if not groups:
        raise ValueError(f"No DAgger episodes in {root}")

    def summary(counts):
        total, teacher = counts["decisions"], counts["teacher_control_decisions"]
        return dict(episodes=counts["episodes"], decisions=total,
            teacher_control_decisions=teacher, teacher_control_fraction=teacher / total if total else 0,
            dagger_interventions=counts["dagger_interventions"], dart_decisions=counts["dart_decisions"],
            maneuver_masked_decisions=counts["maneuver_masked_decisions"],
            by_reason={reason: dict(decisions=counts[reason],
                fraction_all_decisions=counts[reason] / total if total else 0,
                fraction_teacher_decisions=counts[reason] / teacher if teacher else 0) for reason in reasons})

    return dict(all=summary(groups["all"]),
                by_family={family: summary(counts) for family, counts in sorted(groups.items()) if family != "all"})


def dagger_threshold_replay(steps):
    """Replay new takeovers on fixed observations with recovered learner keys.

    The changed hold schedule cannot reuse recorded beta draws: beta steps are
    their expectation (0.1) on unheld decisions. DART hold extensions are omitted.
    """
    reasons = ("stall", "memory_danger", "heading")
    history, counts = deque(), Counter()
    previous_commanded, disagreement, held_until, held_reason = False, None, -1, ""

    def arc(position, path):
        best_distance, best_arc, offset = math.inf, 0., 0.
        for first, second in zip(path, path[1:]):
            dx, dz = second[0] - first[0], second[1] - first[1]
            length2 = dx * dx + dz * dz
            if not length2:
                continue
            fraction = max(0., min(1., ((position[0] - first[0]) * dx
                                       + (position[1] - first[1]) * dz) / length2))
            distance = ((position[0] - first[0] - fraction * dx) ** 2
                        + (position[1] - first[1] - fraction * dz) ** 2)
            if distance < best_distance:
                best_distance, best_arc = distance, offset + fraction * math.sqrt(length2)
            offset += math.sqrt(length2)
        return best_arc

    for step in steps:
        counts["decisions"] += 1
        counts["recovered_teacher_step_proposals"] += step["was_teacher_controlled"]
        now, position, movement = step["time_ms"], step["position"], step["movement"]
        moving = bool(movement[0] or movement[1])
        history.append((now, position, previous_commanded, step["teacher_path"]))
        previous_commanded = moving
        while len(history) > 1 and history[1][0] <= now - 2000:
            history.popleft()
        complete = now - history[0][0] >= 2000
        commanded = sum(second[0] - first[0] for first, second in zip(history, list(history)[1:]) if second[2])
        follow_band = step.get("goal_kind") == "follow" and 200 <= step["target_distance_cm"] <= 500
        stalled = not follow_band and moving and complete and commanded >= 2000 and math.dist(position, history[0][1]) < 30
        teacher = step["teacher_direction"]
        teacher_length = math.hypot(*teacher)
        disagree = moving and teacher_length > 0 and (movement[0] * teacher[0] + movement[1] * teacher[1]) < -.5 * teacher_length
        if not disagree:
            disagreement = None
        elif disagreement is None:
            disagreement = now
        progress = complete and arc(position, history[0][3]) > arc(history[0][1], history[0][3])
        candidates = (stalled, moving and step["danger"], disagree and now - disagreement > 2000 and not progress)
        for reason, candidate in zip(reasons, candidates):
            counts["candidate_" + reason] += candidate
        trigger = next((reason for reason, candidate in zip(reasons, candidates) if candidate), "")
        if now >= held_until and trigger:
            held_until, held_reason = now + 1000, trigger
            counts["interventions_" + trigger] += 1
        if now < held_until:
            counts["steps_" + held_reason] += 1
        else:
            counts["steps_beta"] += .1
    return counts


def dagger_calibrate(args):
    """Read recordings once in bounded chunks; never modify the source corpus."""
    import numpy as np
    import subprocess
    if sys.platform != "linux":
        raise ValueError("Run navigation calibration on Ryzen/Linux under gq")
    if args.runner is None or args.weights is None:
        raise ValueError("DAgger calibration requires --runner policy contract binary and --weights")
    demos = module("nav_demos")
    coordinates = (np.arange(64) + .5 - 32) * 25
    right, forward = np.meshgrid(coordinates, coordinates)
    groups = defaultdict(Counter)
    verification = Counter()
    episodes = 0
    with args.weights.open("rb") as stream:
        weights_sha256 = hashlib.file_digest(stream, "sha256").hexdigest()

    def steps(directory, episode):
        if episode["weights_sha256"] != weights_sha256:
            raise ValueError(f"DAgger calibration weights differ: {directory}")
        actions = np.loadtxt(directory / "actions.txt", ndmin=2)
        samples = list(rows(directory / "samples.jsonl"))
        if len(actions) != len(samples) + 1 or len(samples) != episode["decisions"]:
            raise ValueError(f"DAgger calibration action/sample counts differ: {directory}")
        replay_path = args.output / "replayed-actions" / episode["family"] / episode["id"] / (directory.name + ".jsonl")
        replay_path.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(args.runner), "--replay-actions", str(args.weights), str(directory / "samples.bin.gz"),
                        str(directory / "episode.json"), str(replay_path)], check=True)
        recovered = list(rows(replay_path))
        if len(recovered) != len(samples):
            raise ValueError(f"Native learner replay count differs: {directory}")
        plans = {value["elapsed_ms"]: value for value in rows(directory / "teacher-paths.jsonl")}
        target_distances = ({int(row[0]): float(row[4]) for row in np.loadtxt(
            directory / "target-trajectory.txt", ndmin=2)} if episode["goal_kind"] == "follow" else {})
        index = 0
        for chunk in demos.iter_chunks(directory, chunk_records=32):
            for record in chunk:
                sample = samples[index]
                if int(record["time_ms"]) != sample["time_ms"] or actions[index, 0] != sample["elapsed_ms"]:
                    raise ValueError(f"DAgger calibration timestamps differ: {directory}")
                x, _, z = (float(value) for value in record["position_cm"])
                yaw = math.radians(float(record["body_yaw_deg"]))
                cosine, sine = math.cos(yaw), math.sin(yaw)
                keys = record["keys"]
                def movement(forward_key, strafe):
                    return (float(np.sign(strafe)), 0.) if strafe else (0., float(np.sign(forward_key)))
                proposal = recovered[index]
                learner = proposal["learner_keys"]
                if proposal["time_ms"] != sample["time_ms"]:
                    raise ValueError(f"Native learner replay timestamp differs: {directory}")
                if not sample["teacher_control"]:
                    verification["compared_learner_steps"] += 1
                    mismatch = any(learner[head] != actions[index, column + 1] for column, head in enumerate(HEADS))
                    verification["mismatching_learner_steps"] += mismatch
                    if mismatch:
                        raise ValueError(f"Native learner replay differs on observed student step {index}: {directory}")
                direction = movement(learner["forward"], learner["strafe"])
                teacher = movement(keys["forward"], keys["strafe"])
                path = [(x, z)]
                if episode["teacher_version"] == 3:
                    plan = plans[sample["elapsed_ms"]]["local_plan_cm"]
                    path.extend((x + ahead * cosine - side * sine, z + ahead * sine + side * cosine) for side, ahead in plan)
                    if keys["forward"] >= 0 and keys["strafe"] == 0:
                        teacher = next(((side, ahead) for side, ahead in plan if math.hypot(side, ahead) > 1), teacher)
                else:
                    corner = record["teacher_hint"]["next_corner_cm"]
                    dx, dz = float(corner[0]) - x, float(corner[2]) - z
                    path.append((float(corner[0]), float(corner[2])))
                    if math.hypot(dx, dz) >= 1:
                        teacher = (-dx * sine + dz * cosine, dx * cosine + dz * sine)
                channels = record["memory_near"]["channels"]
                along = right * direction[0] + forward * direction[1]
                across = np.abs(right * direction[1] - forward * direction[0])
                velocity = record["body"]["velocity_cm_s"]
                danger = bool(np.any(((channels[:, :, 5] >= 128) | (channels[:, :, 4] >= 128))
                    & (along > 0) & (along <= 100) & (across <= 15 + 25 * .707107)
                    & (right * velocity[0] + forward * velocity[2] > 0)))
                yield dict(time_ms=sample["elapsed_ms"], position=(x, z), movement=direction,
                           teacher_direction=teacher, teacher_path=path, danger=danger,
                           goal_kind=episode["goal_kind"], target_distance_cm=(
                               target_distances[sample["elapsed_ms"]] if episode["goal_kind"] == "follow" else 0),
                           was_teacher_controlled=sample["teacher_control"])
                index += 1
        if index != len(samples):
            raise ValueError(f"DAgger calibration NAVDEMO count differs: {directory}")

    for source in sorted(args.source.glob("dagger/*/*/seed-*/episode.json")):
        episode = read_json(source)
        result = dagger_threshold_replay(steps(source.parent, episode))
        result["episodes"] = 1
        for family in ("all", episode["family"]):
            groups[family].update(result)
        episodes += 1
        print(json.dumps(dict(calibrated=episodes, family=episode["family"], id=episode["id"])), flush=True)
    if not episodes:
        raise ValueError(f"No DAgger episodes in {args.source}")

    def summary(counts):
        count = counts["decisions"]
        by_reason = {reason: dict(interventions=counts["interventions_" + reason],
            candidate_decisions=counts["candidate_" + reason], decisions=counts["steps_" + reason],
            fraction_all_decisions=counts["steps_" + reason] / count)
            for reason in ("stall", "memory_danger", "heading", "beta")}
        teacher = sum(value["decisions"] for value in by_reason.values())
        return dict(episodes=counts["episodes"], decisions=count,
            recovered_teacher_step_proposals=counts["recovered_teacher_step_proposals"],
            state_interventions=sum(value["interventions"] for value in by_reason.values()),
            state_teacher_decisions=sum(by_reason[reason]["decisions"] for reason in ("stall", "memory_danger", "heading")),
            estimated_teacher_decisions=teacher, estimated_teacher_fraction=teacher / count, by_reason=by_reason)

    output = dict(approximate=True, source=str(args.source),
        assumptions=["Fixed recorded trajectories and observations; changed control would produce different states.",
            "Frozen HD native inference recovers pre-mix learner proposals on all recorded states; GRU and sampling seed are replayed per episode.",
            "Beta is its expected 0.1 share of unheld steps; recorded beta draws do not apply to the changed hold schedule.",
            "DART-induced hold extensions are omitted.",
            "v2 teacher path progress uses recorded next corner only; v3 uses the full recorded local polyline."],
        baseline=dagger_control_metrics(args.source),
        native_replay_verification=dict(verification),
        estimate=dict(all=summary(groups["all"]), by_family={family: summary(counts)
            for family, counts in sorted(groups.items()) if family != "all"}))
    write_json(args.output / "calibration.json", output)
    print(json.dumps(output, ensure_ascii=False), flush=True)
    return output


def drive_dagger(args):
    if args.action_mode != "sample":
        raise ValueError("DAgger collection requires softmax sampling")
    values = dagger_scenarios(args.dagger_scenarios)
    if args.indices is not None:
        selected = [int(value) for value in args.indices.split(",")]
        if not selected or len(set(selected)) != len(selected) or any(not 0 <= index < len(values) for index in selected):
            raise ValueError("--indices must contain distinct valid DAgger indices")
        values = [values[index] for index in selected]
    if any(value["family"] == "jump" for value in values) and getattr(args, "jump_links", None) is None:
        raise ValueError("Jump DAgger requires --jump-links from the frozen corpus inputs")
    os.environ["OPENGOTHIC_NAV_JOURNEY_MEMORY"] = "1"
    checked_paths(args, policy=True)
    if args.navigation_memory_version != 4 or args.policy_input_variant != "hd":
        raise ValueError("DAgger requires the HD input without the familiar channel, on journey memory 4")
    beta = max(0.1, args.teacher_beta)
    with args.weights.open("rb") as stream:
        weights_sha256 = hashlib.file_digest(stream, "sha256").hexdigest()
    jobs = [(index, value, seed) for index, value in enumerate(values)
            for seed in action_seeds(getattr(args, "action_seeds", None))]
    workers = min(args.parallel, len(jobs))
    def collect(worker_index):
        assigned = jobs[worker_index::workers]
        for jumping in (False, True):
            group = [job for job in assigned if (job[1]["family"] == "jump") == jumping]
            if not group:
                continue
            worker = copy(args)
            worker.output = args.output / "dagger-workers" / str(worker_index) / ("jump" if jumping else "ordinary")
            worker.cache = args.cache / ("jump" if jumping else "ordinary")
            worker.cache.mkdir(parents=True, exist_ok=True)
            if jumping:
                worker.links = args.jump_links
            runner = new_runner(worker, "dagger")
            try:
                for index, value, sampling_seed in group:
                    # DART chooses an episode from the native mixing seed. Share
                    # a repeat seed across scenarios without correlating DART.
                    seed = int.from_bytes(hashlib.sha256(f"{sampling_seed}:{value['id']}".encode()).digest()[:4], "little")
                    directory = args.output / "dagger" / value["family"] / value["id"] / f"seed-{sampling_seed}"
                    identity = dict(dagger_scenario=value, scenario_seed=value["seed"], action_seed=seed,
                                    sampling_seed=sampling_seed,
                                    weights=str(args.weights), weights_sha256=weights_sha256, teacher_beta=beta)
                    existing = (directory / "episode.json").is_file()
                    if existing:
                        episode = read_json(directory / "episode.json")
                        if not args.resume or any(episode.get(key) != expected for key, expected in identity.items()):
                            raise ValueError(f"Existing or different DAgger episode: {directory}")
                    else:
                        if directory.exists() and any(directory.iterdir()):
                            raise ValueError(f"Incomplete DAgger episode: {directory}")
                        worker.seed = value["seed"]
                        worker.npc = value.get("npc", args.npc)
                        started = time.monotonic()
                        scenario(worker, runner, f"dagger-{value['id']}-{seed}",
                                 policy_commands(worker, value, seed, teacher_beta=beta, dagger=True), directory)
                        episode = read_json(directory / "episode.json")
                    if (episode.get("demo_version") != 3 or episode.get("navigation_memory_version") != 4
                            or episode.get("journey_memory") is not True or episode.get("physics_mode") != args.physics
                            or episode.get("oracle") is not False or episode.get("action_mode") != "sample"
                            or episode.get("teacher_beta") != beta or episode.get("mixing_seed") != seed
                            or episode.get("drive") is not True or episode.get("dagger") is not True
                            or episode.get("observation_source") != "learner_memory"
                            or episode.get("teacher_version") != (2 if jumping else 3)
                            or episode.get("record") is not True):
                        raise ValueError(f"Runner DAgger identity differs: {directory}")
                    count = 0
                    for sample in rows(directory / "samples.jsonl"):
                        if any(type(sample.get(key)) is not bool for key in ("teacher_control", "dart", "maneuver_mask")):
                            raise ValueError(f"Missing DAgger control/label metadata: {directory}")
                        count += 1
                    if count != episode.get("decisions"):
                        raise ValueError(f"DAgger sidecar decision count differs: {directory}")
                    if not existing:
                        for key in ("id", "family", "split", "region", "place_id", "dataset_family"):
                            if key in value:
                                episode[key] = value[key]
                        episode.update(index=index, **identity, collector_wall_s=time.monotonic() - started)
                        write_json(directory / "episode.json", episode)
                    print(json.dumps(dict(id=value["id"], family=value["family"], action_seed=seed,
                                          decisions=count, success=episode["success"], elapsed_ms=episode["elapsed_ms"])), flush=True)
            finally:
                runner.close()
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = [pool.submit(collect, index) for index in range(workers)]
        for future in futures:
            future.result()


def safety_scenarios(args):
    values = []
    fixtures_path = getattr(args, "safety_fixtures", None)
    if fixtures_path is not None:
        safety = module("nav_perception_safety")
        fixtures = read_json(fixtures_path)
        if not isinstance(fixtures, list) or not fixtures:
            raise ValueError("--safety-fixtures must contain qualified fixtures")
        for fixture in fixtures:
            if fixture.get("qualified") is not True or fixture.get("kind") not in safety.KINDS:
                raise ValueError("Safety fixture must be physically qualified")
            values.append(dict(id=fixture["label"], family="safety", goal_kind="point", split="validation",
                region=fixture["kind"], start=safety.token(fixture["projected_start_cm"]),
                goal=safety.token(fixture["goal_cm"]), seed=7, limit_ms=args.limit_ms, safety_fixture=fixture))
    if getattr(args, "fence_control", False):
        values.append(dict(id="maleth-fence", family="safety", goal_kind="point", split="validation",
            region="fence", start="pasture_fence_current_pose", goal="FP_ROAM_FARM1_SHEEP_09",
            seed=7, limit_ms=args.limit_ms, fence_control=True))
    for value in values:
        if Path(value["id"]).name != value["id"] or value["id"] in ("", ".", ".."):
            raise ValueError("Unsafe safety fixture label")
    if len({value["id"] for value in values}) != len(values):
        raise ValueError("Duplicate safety fixture label")
    return values


def drive_validation(args):
    if args.action_mode != "sample":
        raise ValueError("Frozen manifest validation requires softmax sampling")
    checked_paths(args, policy=True)
    detours = getattr(args, "detour_scenarios", None)
    screen = getattr(args, "eval_screen", None)
    lean = getattr(args, "lean", False) or detours is not None or screen is not None
    if screen is not None:
        if (getattr(args, "manifest", None) is not None or detours is not None
                or getattr(args, "pairs", None) is not None or getattr(args, "indices", None) is not None
                or getattr(args, "families", None) is not None or getattr(args, "action_seeds", None) is not None
                or getattr(args, "safety_fixtures", None) is not None or getattr(args, "fence_control", False)):
            raise ValueError("--eval-screen measures only its frozen validation cohort")
        args.lean = True
        jobs = eval_screen_jobs(screen)
        values = [value for _, value, _ in jobs[::len(ACTION_SEEDS)]]
    elif detours is not None:
        if (getattr(args, "families", None) is not None or getattr(args, "safety_fixtures", None) is not None
                or getattr(args, "fence_control", False)):
            raise ValueError("--detour-scenarios measures only its frozen validation cohort")
        values = detour_scenarios(detours)
    else:
        values = validation_scenarios(args.manifest, getattr(args, "families", None))
    if screen is None and args.indices is not None:
        selected = [int(value) for value in args.indices.split(",")]
        if not selected or len(set(selected)) != len(selected) or any(not 0 <= index < len(values) for index in selected):
            raise ValueError("--indices must contain distinct valid validation indices")
        values = [values[index] for index in selected]
    if screen is None:
        values += safety_scenarios(args)
        seeds = action_seeds(getattr(args, "action_seeds", None))
        jobs = [(index, value, seed) for index, value in enumerate(values) for seed in seeds]
    rejections = args.output.parent / "setup-rejections"
    if any(value["family"] == "jump" for value in values) and getattr(args, "jump_links", None) is None:
        raise ValueError("Jump validation requires --jump-links from the frozen corpus inputs")
    workers = min(args.parallel, len(jobs))
    def collect(worker_index):
        assigned = jobs[worker_index::workers]
        for jumping in (False, True):
            group = [job for job in assigned if (job[1]["family"] == "jump") == jumping]
            if not group:
                continue
            worker = copy(args)
            worker.output = args.output / "validation-workers" / str(worker_index) / ("jump" if jumping else "ordinary")
            worker.cache = args.cache / ("jump" if jumping else "ordinary") / str(worker_index)
            worker.cache.mkdir(parents=True, exist_ok=True)
            if jumping:
                worker.links = args.jump_links
            runner = new_runner(worker, "validation")
            try:
                for index, value, seed in group:
                    directory = args.output / "validation" / value["family"] / value["id"] / f"seed-{seed}"
                    rejection = rejections / f"{value['id']}.json"
                    if rejection.is_file():
                        if read_json(rejection)["validation_scenario"] != value:
                            raise ValueError(f"Different rejected validation scenario: {rejection}")
                        continue
                    if (directory / "episode.json").is_file():
                        existing = read_json(directory / "episode.json")
                        cached = (directory / "drive-metrics.json").is_file()
                        if (not args.resume or existing.get("validation_scenario", value) != value
                                or existing.get("action_seed", seed) != seed
                                or existing.get("weights", str(args.weights)) != str(args.weights)
                                or (lean and cached and (existing.get("validation_scenario") != value
                                    or existing.get("action_seed") != seed
                                    or existing.get("weights") != str(args.weights)))):
                            raise ValueError(f"Existing or different validation episode: {directory}")
                        episode = existing
                        started = None
                    else:
                        if lean and shutil.disk_usage(args.output).free < 20 * 1024**3 + 512 * 1024**2:
                            raise ValueError("Lean validation requires the 20-GiB disk reserve plus 512 MiB")
                        if directory.exists() and any(directory.iterdir()):
                            if not args.resume:
                                raise ValueError(f"Incomplete validation episode; use --resume: {directory}")
                            shutil.rmtree(directory)
                        worker.seed = value["seed"]
                        started = time.monotonic()
                        try:
                            scenario(worker, runner, f"validation-{value['id']}-{seed}",
                                     validation_commands(worker, value, seed), directory)
                        except RuntimeError as error:
                            reason = str(error)
                            if (value["goal_kind"] not in ("approach", "follow") or reason !=
                                    "Runner rejected advance: navdemos moving route needs distinct connected points"):
                                raise
                            process = getattr(runner, "process", None)
                            if process is not None and process.poll() is not None:
                                raise RuntimeError(f"Runner exited during setup refusal: {process.returncode}") from error
                            if any(path.name in ("episode.json", "actions.txt", "trajectory.txt", "target-trajectory.txt")
                                   or path.name.startswith("samples.bin") for path in directory.rglob("*")):
                                raise
                            rejected = dict(id=value["id"], family=value["family"],
                                            validation_scenario=value, reason=reason)
                            rejections.mkdir(parents=True, exist_ok=True)
                            with tempfile.NamedTemporaryFile(mode="w", dir=rejections, delete=False) as stream:
                                json.dump(rejected, stream, ensure_ascii=False, indent=2)
                                temporary = Path(stream.name)
                            temporary.replace(rejection)
                            print(json.dumps(dict(setup_rejected=True, **rejected)), flush=True)
                            # A rejected advance remains pending in the native
                            # protocol. Recreate this worker before another reset.
                            runner.close()
                            runner = new_runner(worker, "validation")
                            continue
                        episode = read_json(directory / "episode.json")
                    if (episode.get("demo_version") != getattr(args, "navdemo_version", 2)
                            or episode.get("navigation_memory_version") != getattr(args, "navigation_memory_version", 3)
                            or episode.get("physics_mode") != args.physics or episode.get("oracle") is not False
                            or episode.get("action_mode") != "sample" or episode.get("teacher_beta") != 0
                            or episode.get("mixing_seed") != seed or episode.get("drive") is not True
                            or episode.get("observation_source") != "learner_memory"):
                        raise ValueError(f"Runner validation identity differs: {directory}")
                    if lean:
                        start = "intentional" if value.get("fence_control") else value["start"]
                        moving = value["goal_kind"] in ("follow", "approach")
                        # Moving routes resolve freepoints to connected waypoints;
                        # native goal names do not identify the frozen target/route.
                        if (episode.get("start") != start
                                or (not moving and episode.get("goal") != value["goal"])
                                or (moving and episode.get("seed") != value["seed"])
                                or episode.get("goal_kind") != value["goal_kind"]
                                or episode.get("record") is not False or episode.get("teacher_control_decisions") != 0
                                or any(directory.glob("samples.bin*"))):
                            raise ValueError(f"Runner lean identity/recording/control differs: {directory}")
                    if (directory / "drive-metrics.json").is_file():
                        if lean:
                            clean_lean_diagnostics(directory)
                        else:
                            module("nav_ppo_final").retain_tail(directory)
                        continue
                    for key in ("id", "family", "split", "region", "place_id"):
                        if key in value:
                            episode[key] = value[key]
                    episode.update(index=index, validation_scenario=value, scenario_seed=value["seed"], action_seed=seed,
                                   weights=str(args.weights))
                    if started is not None:
                        episode["collector_wall_s"] = time.monotonic() - started
                    write_json(directory / "episode.json", episode)
                    measured = (detour_episode_metrics(directory) if value["family"] == "detour" else
                                lean_validation_episode_metrics(directory) if lean else validation_episode_metrics(directory))
                    write_json(directory / "drive-metrics.json", measured)
                    if lean:
                        clean_lean_diagnostics(directory)
                    else:
                        module("nav_ppo_final").retain_tail(directory)
                    print(json.dumps(dict(id=value["id"], family=value["family"], action_seed=seed,
                                          success=measured["success"], elapsed_ms=episode["elapsed_ms"])), flush=True)
            finally:
                runner.close()
            if lean and worker.output.exists():
                shutil.rmtree(worker.output)
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = [pool.submit(collect, index) for index in range(workers)]
        for future in futures:
            future.result()


def population_episode_counts(episodes, worker_index):
    """Count every closed episode without opening possibly rejected raw records."""
    count = valid = rejected_decisions = 0
    terminations = Counter()
    diagnostics = []
    for path, episode in episodes:
        count += 1
        termination = episode.get("termination") or episode.get("failure") or ("arrived" if episode.get("success") else "unknown")
        terminations[termination] += 1
        rejected = (termination == "nonfinite_observation"
                    or episode.get("training_valid") is False and "nonfinite_observation" in episode)
        valid += episode.get("training_valid", not rejected) is not False and not rejected
        if rejected:
            decisions = episode.get("decisions", 0)
            if not isinstance(decisions, int) or isinstance(decisions, bool) or decisions < 0:
                raise ValueError(f"Invalid rejected episode decision count in {path}")
            rejected_decisions += decisions
            # Keep all metadata, including the named diagnostic values and stage.
            # A zero-decision failure still occupies one episode in the denominator.
            diagnostics.append(dict(worker=worker_index, episode=str(Path(path).parent), metadata=episode))
    return dict(episodes=count, training_valid_episodes=valid, episode_terminations=dict(terminations),
                rejected_nonfinite_episodes=len(diagnostics), rejected_nonfinite_decisions=rejected_decisions,
                rejected_nonfinite_fraction=len(diagnostics) / count if count else 0.,
                rejected_nonfinite_diagnostics=diagnostics)


def population_worker(args, worker_index, barrier=None):
    worker = copy(args)
    worker.seed = (args.seed + worker_index) & 0xffffffff
    name = f"n{args.population}-r{args.parallel}-w{worker_index}"
    runner = None
    try:
        runner = new_runner(worker, name)
        pairs = load_pairs(args.pairs)
        # Separate initial slot populations between workers, preserving provenance.
        offset = (worker_index * args.population) % len(pairs)
        selected = pairs[offset:] + pairs[:offset]
        pair_file = runner_pairs(args.output / "runner-pairs" / f"{name}.txt", selected)
        directory = args.output / "population" / name
        if (directory / "population.json").exists():
            raise ValueError(f"Population output already exists: {directory}; choose a fresh --output")
        if barrier is not None:
            barrier.wait(timeout=720 + args.watchdog_seconds)
        command = (f"navdemos population {args.npc} {pair_file} {args.limit_ms} {args.weights} "
                   f"{args.population} {worker.seed} {args.action_mode} {args.episodes} {int(args.record)} {args.hero} {args.run_ms}")
        started = time.monotonic()
        scenario(worker, runner, name, [f"roads load {args.roads}", f"roads on {args.npc}", command, "navdemos wait"], directory)
        wall = time.monotonic() - started
        result = read_json(directory / "population.json")
        episode_records = []
        for episode_path in sorted((directory / "episodes").rglob("episode.json")):
            episode = read_json(episode_path)
            if args.record:
                pair_index = episode["pair_index"]
                if not isinstance(pair_index, int) or not 0 <= pair_index < len(selected):
                    raise ValueError(f"Invalid population pair index in {episode_path}")
                pair = selected[pair_index]
                episode.update(region=pair["region"], index=pair.get("index", pair_index),
                               split=pair.get("split", "population"), split_index=pair.get("split_index", pair_index),
                               weights=str(args.weights), limit_ms=args.limit_ms, drive=True)
                write_json(episode_path, episode)
            episode_records.append((episode_path, episode))
        health = population_episode_counts(episode_records, worker_index)
        for field, observed in (("episodes", health["episodes"]),
                                ("invalid_observation_episodes", health["rejected_nonfinite_episodes"]),
                                ("training_valid_episodes", health["training_valid_episodes"])):
            if field in result and result[field] != observed:
                raise ValueError(f"Population {field} count differs from episode metadata in {directory}: {result[field]}/{observed}")
        result.update(health)
        decisions = result.get("decisions", result.get("total_decisions"))
        if (not isinstance(decisions, int) or isinstance(decisions, bool) or decisions < 0
                or decisions == 0 and not result["rejected_nonfinite_episodes"]):
            raise ValueError(f"Population has no decisions: {directory}")
        if result["rejected_nonfinite_decisions"] > decisions:
            raise ValueError(f"Rejected decisions exceed population decisions in {directory}")
        result.update(worker=worker_index, population=args.population, parallel=args.parallel,
                      collector_wall_s=wall, directory=str(directory), weights=str(args.weights),
                      action_mode=args.action_mode, recording=args.record, hero=args.hero,
                      decisions=decisions)
        write_json(directory / "measurement.json", result)
        return result
    finally:
        if barrier is not None:
            barrier.abort()
        if runner is not None:
            runner.close()


def population(args):
    checked_paths(args, policy=True)
    load_pairs(args.pairs)
    if args.parallel > 1:
        # Build/read immutable cache once before concurrent readers enter it.
        warm = new_runner(args, "cache-warmup")
        warm.close()
    barrier = threading.Barrier(args.parallel) if args.parallel > 1 else None
    started = time.monotonic()
    with ThreadPoolExecutor(max_workers=args.parallel) as pool:
        futures = [pool.submit(population_worker, args, index, barrier) for index in range(args.parallel)]
        results = [future.result() for future in futures]
    all_wall = time.monotonic() - started
    wall = max(result["collector_wall_s"] for result in results)
    decisions = sum(result["decisions"] for result in results)
    episodes = sum(worker.get("episodes", 0) for worker in results)
    rejected = sum(worker.get("rejected_nonfinite_episodes", 0) for worker in results)
    terminations = Counter()
    for worker in results:
        terminations.update(worker.get("episode_terminations", {}))
    result = dict(population=args.population, parallel=args.parallel, decisions=decisions,
                  collection_wall_s=wall, total_wall_s=all_wall, decisions_per_second=decisions / wall,
                  timing_scope="concurrent_full_population_scenarios_excluding_initial_world_and_navmesh_preparation",
                  recording=args.record, action_mode=args.action_mode, workers=results,
                  episodes=episodes, training_valid_episodes=sum(worker.get("training_valid_episodes", 0) for worker in results),
                  episode_terminations=dict(terminations), rejected_nonfinite_episodes=rejected,
                  rejected_nonfinite_decisions=sum(worker.get("rejected_nonfinite_decisions", 0) for worker in results),
                  rejected_nonfinite_fraction=rejected / episodes if episodes else 0.,
                  rejected_nonfinite_diagnostics=[diagnostic for worker in results
                                                  for diagnostic in worker.get("rejected_nonfinite_diagnostics", [])])
    if all("measured_decisions" in worker and "collect_seconds" in worker for worker in results):
        warmed_decisions = sum(worker["measured_decisions"] for worker in results)
        warmed_wall = max(worker["collect_seconds"] for worker in results)
        result.update(warmed_decisions=warmed_decisions, warmed_collection_wall_s=warmed_wall,
                      warmed_decisions_per_second=warmed_decisions / warmed_wall,
                      warmed_timing_scope="C++ active population intervals excluding initial N sensor/policy decisions and startup")
    write_json(args.output / f"throughput-n{args.population}-r{args.parallel}.json", result)
    print(json.dumps(result, ensure_ascii=False))


def maneuver_press_metrics(times, teacher, applied, window_ms=1000):
    """Match rising button edges one-to-one by type within a fixed +/-1s window."""
    import numpy as np
    times, teacher, applied = np.asarray(times), np.asarray(teacher, dtype=bool), np.asarray(applied, dtype=bool)
    if teacher.shape != applied.shape or teacher.shape != (len(times), 2):
        raise ValueError("Maneuver labels/actions must align with decision timestamps")
    result = {}
    for column, name in enumerate(("jump", "climb")):
        reference = times[teacher[:, column] & ~np.r_[False, teacher[:-1, column]]]
        selected = times[applied[:, column] & ~np.r_[False, applied[:-1, column]]]
        teacher_index = selected_index = hits = 0
        while teacher_index < len(reference) and selected_index < len(selected):
            if selected[selected_index] < reference[teacher_index] - window_ms:
                selected_index += 1
            elif selected[selected_index] > reference[teacher_index] + window_ms:
                teacher_index += 1
            else:
                hits += 1
                teacher_index += 1
                selected_index += 1
        false = len(selected) - hits
        result[name] = dict(teacher_presses=len(reference), recalled_presses=hits,
                            learner_presses=len(selected), false_presses=false,
                            recall=hits / len(reference) if len(reference) else None,
                            false_presses_per_1000_decisions=1000 * false / len(times) if len(times) else None)
    return result


def applied_transition_trace(route, actions, frames):
    """Build passage witnesses from the frozen link and executed actions only.

    The watched topology is passive outcome evidence. A takeoff press counts
    only within 150cm of its frozen source; learner path labels are not actions.
    """
    import numpy as np
    if not frames:
        raise ValueError("Jump validation has no passive physical transition frames")
    source = np.asarray(route["transition_start_m"]) * 100
    frame_times = np.asarray([frame["elapsed_ms"] for frame in frames])
    trace = []
    for action in actions[:-1]:
        index = min(len(frames) - 1, max(0, int(np.searchsorted(frame_times, action[0], side="right")) - 1))
        frame = frames[index]
        if frame.get("transition_id") != route["transition_id"]:
            raise ValueError("Passive transition monitor differs from the frozen scheduled link")
        near_source = math.dist(frame["position_cm"], source) <= 150
        trace.append(dict(elapsed_ms=int(action[0]), position_cm=frame["position_cm"],
            transitions=[dict(id=route["transition_id"], type=route["type"])],
            active_transition_id=route["transition_id"], active_transition_type=route["type"],
            on_transition=near_source, jump=bool(action[5]), climb=bool(action[6])))
    return trace


def clean_lean_diagnostics(directory):
    for name in ("known-paths.jsonl", "teacher-paths.jsonl", "transition-frames.jsonl",
                 "perception-safety-frames.jsonl", "perception-water-observations.jsonl"):
        (directory / name).unlink(missing_ok=True)


def text_episode_metrics(directory):
    """Physical outcomes and applied actions only; no NAVDEMO/teacher labels."""
    import numpy as np
    directory = Path(directory)
    episode = read_json(directory / "episode.json")
    actions = np.loadtxt(directory / "actions.txt", ndmin=2)
    trajectory = np.loadtxt(directory / "trajectory.txt", ndmin=2)
    steps, elapsed = episode["decisions"], episode["elapsed_ms"]
    if (type(steps) is not int or steps <= 0 or actions.shape != (steps + 1, 9)
            or not np.isfinite(actions).all() or actions[0, 0] != 0 or actions[-1, 0] != elapsed
            or (np.diff(actions[:, 0]) <= 0).any()):
        raise ValueError(f"Invalid lean applied-action rows: {directory}")
    if (trajectory.shape[1] != 5 or not np.isfinite(trajectory).all() or trajectory[0, 0] != 0
            or trajectory[-1, 0] != elapsed or (np.diff(trajectory[:, 0]) < 0).any()):
        raise ValueError(f"Invalid lean trajectory rows: {directory}")
    applied = actions[:-1, 1:7]
    if (not np.isin(applied[:, :3], (-1, 0, 1)).all() or not np.isin(applied[:, 3:], (0, 1)).all()
            or (actions[-1, [1, 2, 3, 5, 6]] != 0).any()):
        raise ValueError(f"Invalid lean applied-action keys: {directory}")
    masks = {head: applied[:, index] != 0 for index, head in enumerate(HEADS)}
    masks.update(turn_only=(applied[:, :2] == 0).all(axis=1) & (applied[:, 2] != 0),
                 idle=~applied[:, [0, 1, 2, 4, 5]].any(axis=1))
    counts = {key: int(mask.sum()) for key, mask in masks.items()}
    presses = {}
    for column, name in ((4, "jump"), (5, "climb")):
        pressed = applied[:, column] != 0
        count = int(np.count_nonzero(pressed & ~np.r_[False, pressed[:-1]]))
        presses[name] = dict(presses=count, per_1000_decisions=1000 * count / steps)
    result = {key: episode[key] for key in ("id", "family", "goal_kind", "split", "region", "start", "goal",
        "action_seed", "scenario_seed", "success", "elapsed_ms", "decisions", "blocked_spans", "blocked_ms",
        "max_blocked_ms", "teacher_beta", "teacher_control_decisions", "failure", "distance_cm")}
    result.update(episode=str(directory), elapsed_s=elapsed / 1000, action_counts=counts,
        action_fraction={key: count / steps for key, count in counts.items()}, action_presses=presses,
        travelled_xz_cm=float(np.linalg.norm(np.diff(trajectory[:, [1, 3]], axis=0), axis=1).sum()),
        recording="text_only")
    for key in ("familiar_input_decisions", "familiar_input_nonzero_decisions",
                "familiar_near_input_nonzero_decisions", "familiar_far_input_nonzero_decisions"):
        if key in episode:
            result[key] = episode[key]
    return result


def detour_episode_metrics(directory):
    result = text_episode_metrics(directory)
    result["detour_geometry"] = read_json(Path(directory) / "episode.json")["validation_scenario"]["geometry"]
    return result


def text_group_metrics(episodes):
    decisions = sum(value["decisions"] for value in episodes)
    counts = {key: sum(value["action_counts"][key] for value in episodes) for key in episodes[0]["action_counts"]}
    return dict(n=len(episodes), arrived=sum(bool(value["success"]) for value in episodes),
        arrival_fraction=sum(bool(value["success"]) for value in episodes) / len(episodes), decisions=decisions,
        median_elapsed_s=statistics.median(value["elapsed_s"] for value in episodes),
        blocked_spans=sum(value["blocked_spans"] for value in episodes),
        blocked_ms=sum(value["blocked_ms"] for value in episodes), max_blocked_ms=max(value["max_blocked_ms"] for value in episodes),
        teacher_control_decisions=sum(value["teacher_control_decisions"] for value in episodes),
        action_counts=counts, action_fraction={key: count / decisions for key, count in counts.items()},
        turn_in_place_fraction=counts["turn_only"] / decisions, idle_fraction=counts["idle"] / decisions,
        action_presses={name: dict(presses=sum(value["action_presses"][name]["presses"] for value in episodes),
            per_1000_decisions=1000 * sum(value["action_presses"][name]["presses"] for value in episodes) / decisions)
            for name in ("jump", "climb")})


def detour_summary(episodes):
    """Pool lean detour outcomes without inventing unavailable teacher agreement."""
    return dict(all=text_group_metrics(episodes),
        by_seed={str(seed): text_group_metrics([ep for ep in episodes if ep["action_seed"] == seed])
                 for seed in sorted({ep["action_seed"] for ep in episodes})},
        by_goal_kind={kind: text_group_metrics([ep for ep in episodes if ep["goal_kind"] == kind])
                      for kind in sorted({ep["goal_kind"] for ep in episodes})},
        metric_definitions=dict(action_presses="applied rising edges per type per 1000 decisions; no teacher labels recorded",
            turn_in_place="nonzero turn, zero forward/strafe; fraction of decisions", recording="text_only; no NAVDEMO"),
        episodes=episodes)


def validation_episode_metrics(directory):
    import numpy as np
    directory = Path(directory)
    metadata = read_json(directory / "episode.json")
    demos = module("nav_demos")
    result = module("nav_drive").episode_metrics(directory, demos)
    actions = np.loadtxt(directory / "actions.txt", ndmin=2)
    labels = np.concatenate([np.column_stack((chunk["keys"]["jump"], chunk["keys"]["climb"]))
                             for chunk in demos.iter_chunks(directory)])
    result.update({key: metadata[key] for key in ("id", "family", "goal_kind", "action_seed", "scenario_seed")})
    result["maneuver_presses"] = maneuver_press_metrics(actions[:-1, 0], labels, actions[:-1, 5:7])
    return validation_outcome_metrics(directory, metadata, actions, result)


def lean_validation_episode_metrics(directory):
    import numpy as np
    directory = Path(directory)
    metadata = read_json(directory / "episode.json")
    result = text_episode_metrics(directory)
    actions = np.loadtxt(directory / "actions.txt", ndmin=2)
    return validation_outcome_metrics(directory, metadata, actions, result)


def validation_outcome_metrics(directory, metadata, actions, result):
    """Reuse passive physical witnesses for recorded and lean validation."""
    if metadata["goal_kind"] in ("follow", "approach"):
        moving = module("nav_demos_v4").text_metrics(directory)
        for key in ("first_contact_ms", "follow_observed_ms", "follow_in_band_ms", "follow_fraction",
                    "follow_full_horizon", "follow_failure", "follow_success", "approach_success"):
            if key in moving:
                result[key] = moving[key]
    if metadata["family"] == "jump":
        jumps = module("nav_jump_record")
        frames = list(rows(directory / "transition-frames.jsonl"))
        route = metadata["validation_scenario"]
        trace = applied_transition_trace(route, actions, frames)
        evidence, failure = jumps.transition_evidence(route, trace, frames=frames)
        evidence["action_source"] = "actions.txt; takeoff <=150cm from frozen scheduled transition source"
        result.update(goal_success=result["success"], success=bool(result["success"] and not failure),
                      transition_success=bool(result["success"] and not failure),
                      transition_failure=failure, transition_evidence=evidence)
        if failure:
            result["observed_reason"] = failure
    if metadata["family"] == "safety":
        safety = module("nav_perception_safety")
        frames = list(rows(directory / "perception-safety-frames.jsonl"))
        intervals = safety.fall_intervals(frames)
        result.update(falls=sum(value["descent_cm"] > safety.CV_STEP_CM for value in intervals),
            entered_deep_water=any(frame["water_depth_cm"] + safety.WATER_DEPTH_EPSILON_CM >= frame["deep_water_threshold_cm"]
                                   for frame in frames), swimming=any(frame["swimming"] for frame in frames))
        fixture = metadata["validation_scenario"].get("safety_fixture")
        if fixture is not None:
            result["safety"] = safety.assess(fixture, metadata, frames)
        elif metadata["validation_scenario"].get("fence_control"):
            if len(frames) < 2 or frames[0]["elapsed_ms"] != 0:
                raise ValueError("Fence control lacks initial physical safety frames")
            damages = []
            for previous, frame in zip(frames, frames[1:]):
                loss = previous["hitpoints"] - frame["hitpoints"]
                if loss > 0:
                    associated = any(value["descent_cm"] > safety.CV_STEP_CM and value["descent_witnessed_ms"]
                        <= frame["elapsed_ms"] <= value["end_ms"] + safety.LANDING_DAMAGE_WINDOW_MS for value in intervals)
                    damages.append(dict(elapsed_ms=frame["elapsed_ms"], hitpoints_lost=loss,
                                        associated_with_fall=associated))
            result["safety"] = dict(harmful_fall=any(value["associated_with_fall"] for value in damages),
                fall_intervals=intervals, damage_events=damages,
                unattributed_damage=sum(value["hitpoints_lost"] for value in damages if not value["associated_with_fall"]),
                initial_hitpoints=frames[0]["hitpoints"], final_hitpoints=frames[-1]["hitpoints"])
    return result


def validation_group_metrics(episodes):
    result = module("nav_drive").group_metrics(episodes)
    result.update(turn_in_place_fraction=result["action_fraction"]["turn_only"],
                  idle_fraction=result["action_fraction"]["idle"])
    presses = {}
    for name in ("jump", "climb"):
        counts = {key: sum(ep["maneuver_presses"][name][key] for ep in episodes)
                  for key in ("teacher_presses", "recalled_presses", "learner_presses", "false_presses")}
        counts.update(recall=counts["recalled_presses"] / counts["teacher_presses"] if counts["teacher_presses"] else None,
                      false_presses_per_1000_decisions=1000 * counts["false_presses"] / result["decisions"])
        presses[name] = counts
    result["maneuver_presses"] = presses
    return validation_outcome_group_metrics(episodes, result)


def validation_outcome_group_metrics(episodes, result):
    following = [ep for ep in episodes if ep["goal_kind"] == "follow"]
    if following:
        observed = sum(ep["follow_observed_ms"] for ep in following)
        band = sum(ep["follow_in_band_ms"] for ep in following)
        result.update(follow_observed_ms=observed, follow_in_band_ms=band,
                      follow_fraction=band / observed if observed else None,
                      follow_contact_episodes=sum(ep["first_contact_ms"] is not None for ep in following),
                      follow_full_horizon_episodes=sum(ep["follow_full_horizon"] for ep in following))
    safety = [ep for ep in episodes if ep["family"] == "safety"]
    if safety:
        result.update(falls=sum(ep["falls"] for ep in safety),
                      episodes_with_falls=sum(ep["falls"] > 0 for ep in safety),
                      deep_water_episodes=sum(ep["entered_deep_water"] for ep in safety),
                      swimming_episodes=sum(ep["swimming"] for ep in safety),
                      harmful_fall_episodes=sum(ep.get("safety", {}).get("harmful_fall", False) for ep in safety),
                      unattributed_damage=sum(ep.get("safety", {}).get("unattributed_damage", 0) for ep in safety))
    jumping = [ep for ep in episodes if ep["family"] == "jump"]
    if jumping:
        confirmed = sum(ep["transition_success"] for ep in jumping)
        result.update(transition_attempts=len(jumping), confirmed_transitions=confirmed,
                      transition_fraction=confirmed / len(jumping))
    return result


def validation_summary(episodes, lean=False):
    group = (lambda values: validation_outcome_group_metrics(values, text_group_metrics(values))) if lean else validation_group_metrics
    def families(values):
        return {family: group([ep for ep in values if ep["family"] == family])
                for family in sorted({ep["family"] for ep in values})}
    result = dict(all=group(episodes), by_family=families(episodes),
        by_seed={str(seed): dict(all=group([ep for ep in episodes if ep["action_seed"] == seed]),
                                by_family=families([ep for ep in episodes if ep["action_seed"] == seed]))
                 for seed in sorted({ep["action_seed"] for ep in episodes})},
        metric_definitions=dict(follow="sum in-band ms / sum post-contact ms; first <=500cm, band 200..500cm",
            maneuver_presses="rising edges, matched one-to-one by type within +/-1000ms of online teacher rising edge; unmatched learner edges per 1000 decisions",
            turn_in_place="nonzero turn, zero forward/strafe; fraction of decisions",
            idle="zero forward/strafe/turn/jump/climb; fraction of decisions",
            jump_success="goal reached plus scheduled transition, applied button, native dynamics and completed landing-side evidence",
            falls="physical unsupported descent >60cm; harmful falls require associated hitpoint loss within the original safety assessor window"), episodes=episodes)
    if lean:
        result["metric_definitions"].pop("maneuver_presses")
        result["metric_definitions"].update(recording="text_only; no NAVDEMO",
            action_presses="applied rising edges per type per 1000 decisions; no teacher labels recorded")
    return result


def object_detour_summary(episodes, probes_path):
    """Stratify objects with frozen Detour labels, independent of policy outcomes."""
    objects = defaultdict(list)
    for episode in episodes:
        if episode["family"] == "object":
            objects[episode["id"]].append(episode)
    probes = {}
    for probe in rows(probes_path):
        identifier = probe.get("label")
        if identifier in objects:
            if identifier in probes:
                raise ValueError(f"Duplicate object Detour probe: {identifier}")
            probes[identifier] = probe
    scenarios, ratios = [], {}
    for identifier, samples in sorted(objects.items()):
        probe = probes.get(identifier)
        if probe is None:
            raise ValueError(f"Missing object Detour probe: {identifier}")
        direct, detour = float(probe["direct_cm"]), float(probe["detour_length_cm"])
        if (probe.get("detour_complete") is not True or not math.isfinite(direct)
                or not math.isfinite(detour) or min(direct, detour) <= 0
                or any((sample["start"], sample["goal"]) != (probe["start"], probe["goal"])
                       for sample in samples)):
            raise ValueError(f"Invalid object Detour probe: {identifier}")
        ratio = detour / direct
        ratios[identifier] = ratio
        bucket = "<1.2" if ratio < 1.2 else "1.2-1.5" if ratio <= 1.5 else ">1.5"
        successes = sum(bool(sample["success"]) for sample in samples)
        scenarios.append(dict(id=identifier, start=probe["start"], goal=probe["goal"],
            detour_length_cm=detour, direct_cm=direct, detour_ratio=ratio, bucket=bucket,
            episodes=len(samples), successes=successes, success_fraction=successes / len(samples),
            by_seed={str(sample["action_seed"]): bool(sample["success"]) for sample in samples}))

    def aggregate(selected):
        values = [episode for identifier in selected for episode in objects[identifier]]
        successes = sum(bool(episode["success"]) for episode in values)
        by_seed = {}
        for seed in sorted({episode["action_seed"] for episode in values}):
            repeated = [episode for episode in values if episode["action_seed"] == seed]
            wins = sum(bool(episode["success"]) for episode in repeated)
            by_seed[str(seed)] = dict(episodes=len(repeated), successes=wins, success_fraction=wins / len(repeated))
        return dict(scenarios=len(selected), episodes=len(values), successes=successes,
                    success_fraction=successes / len(values) if values else None, by_seed=by_seed)

    return {"source": str(probes_path), "scenarios": scenarios,
        "by_bucket": {bucket: aggregate([value["id"] for value in scenarios if value["bucket"] == bucket])
                      for bucket in ("<1.2", "1.2-1.5", ">1.5")},
        "detours_over_1.2": aggregate([identifier for identifier, ratio in ratios.items() if ratio > 1.2])}


def object_failure_summaries(episodes, count=10):
    failures = [ep for ep in episodes if ep["family"] == "object" and not ep["success"]]
    selected, seen = [], set()
    for episode in failures:
        if episode["id"] not in seen:
            selected.append(episode)
            seen.add(episode["id"])
    selected.extend(ep for ep in failures if ep not in selected)
    analyzer, results = module("nav_ppo_failures"), []
    for measured in selected[:count]:
        result, _, _, _ = analyzer.summarize_episode(Path(measured["episode"]), measured)
        results.append(dict(id=measured["id"], action_seed=measured["action_seed"],
            episode=measured["episode"], behaviour=result["behaviour"], eye_tail=result["eye_tail"],
            final_eye=result["final_eye"], final_memory=result["memory_snapshots"][-1],
            goal=result["carrot"], body_final=result["body_final"]))
    return results


def summarize(args):
    cached = sorted(args.source.rglob("drive-metrics.json"))
    if cached:
        episodes = [read_json(path) for path in cached]
        if any(ep.get("split") != "validation" for ep in episodes):
            raise ValueError("Manifest summary requires validation-only episodes")
        rejections = [read_json(path) for path in sorted((args.source.parent / "setup-rejections").glob("*.json"))]
        excluded = {value["id"] for value in rejections}
        episodes = [episode for episode in episodes if episode["id"] not in excluded]
        detours = bool(episodes) and all(ep.get("family") == "detour" for ep in episodes)
        lean = bool(episodes) and all(ep.get("recording") == "text_only" for ep in episodes)
        if any(ep.get("recording") == "text_only" for ep in episodes) and not lean:
            raise ValueError("Cannot combine recorded and lean validation metrics")
        result = detour_summary(episodes) if detours else validation_summary(episodes, lean=True) if lean else validation_summary(episodes)
        result["setup_rejections"] = rejections
        if getattr(args, "object_probes", None) is not None:
            result["object_detour"] = object_detour_summary(episodes, args.object_probes)
        result["object_failures_last_10s"] = [] if lean else object_failure_summaries(episodes)
        write_json(args.output / "metrics.json", result)
        print(json.dumps({key: value for key, value in result.items()
                          if key not in ("episodes", "object_failures_last_10s")}, ensure_ascii=False))
        return
    paths = sorted(args.source.rglob("episode.json"))
    episodes = []
    for path in paths:
        episode = read_json(path)
        if not (path.parent / "actions.txt").is_file() or not (path.parent / "trajectory.txt").is_file():
            continue
        if not episode.get("drive") or episode.get("decisions", 0) == 0:
            continue
        episodes.append(module("nav_drive").episode_metrics(path.parent, module("nav_demos")))
    if not episodes:
        raise ValueError(f"No recorded policy episodes in {args.source}")
    drive_module = module("nav_drive")
    result = dict(all=drive_module.group_metrics(episodes),
                  by_region={region: drive_module.group_metrics([ep for ep in episodes if ep["region"] == region])
                             for region in sorted({ep["region"] for ep in episodes})}, episodes=episodes)
    write_json(args.output / "metrics.json", result)
    print(json.dumps(result, ensure_ascii=False))


def history(args):
    excluded = historical_pairs(args.exclude)
    if not excluded:
        raise ValueError("No historical pairs found")
    pairs = [dict(start=start, goal=goal) for start, goal in sorted(excluded) if start < goal]
    destination = args.output / "historical-pairs.jsonl"
    write_pairs(destination, pairs)
    print(json.dumps(dict(pairs=str(destination), undirected_pairs=len(pairs))))


def comparable_episodes(root):
    result = {}
    for path in sorted(root.rglob("episode.json")):
        episode = read_json(path)
        if not episode.get("drive") or episode.get("decisions", 0) == 0:
            continue
        key = episode["start"], episode["goal"]
        if key in result:
            raise ValueError(f"Repeated comparison pair {key} in {root}")
        result[key] = dict(episode, directory=str(path.parent))
    if not result:
        raise ValueError(f"No policy episodes in {root}")
    return result


def comparison_metrics(reference, repeated):
    if reference.keys() != repeated.keys():
        raise ValueError("Comparison needs identical route sets")
    comparisons = []
    for key, episode in reference.items():
        other = repeated[key]
        elapsed = episode["elapsed_ms"]
        relative = abs(other["elapsed_ms"] - elapsed) / elapsed if elapsed else float("inf")
        comparisons.append(dict(start=key[0], goal=key[1],
                                reference_success=episode["success"], repeated_success=other["success"],
                                reference_elapsed_ms=elapsed, repeated_elapsed_ms=other["elapsed_ms"],
                                elapsed_relative_error=relative,
                                passed=episode["success"] == other["success"] and relative <= .05))
    result = dict(passed=all(pair["passed"] for pair in comparisons), n=len(comparisons),
                  tolerance=.05, reference_arrived=sum(p["reference_success"] for p in comparisons),
                  repeated_arrived=sum(p["repeated_success"] for p in comparisons),
                  median_reference_ms=statistics.median(p["reference_elapsed_ms"] for p in comparisons),
                  median_repeated_ms=statistics.median(p["repeated_elapsed_ms"] for p in comparisons),
                  pairs=comparisons)
    return result


def compare(args):
    result = comparison_metrics(comparable_episodes(args.reference), comparable_episodes(args.source))
    write_json(args.output / "comparison.json", result)
    print(json.dumps(result, ensure_ascii=False))
    if not result["passed"]:
        raise ValueError("Population comparison exceeds success/time tolerance; see comparison.json")


def equivalence_commands(args, pair, pair_file, mode):
    """Drive and population use identical stand physics and explicit hero placement."""
    if mode not in ("drive-near", "pop-near", "drive-far", "pop-far"):
        raise ValueError(f"Unknown equivalence mode: {mode}")
    controller, distance = mode.split("-")
    commands = [f"roads load {args.roads}", f"roads on {args.npc}",
                f"navdemos hero {'near' if distance == 'near' else 'keep'}"]
    if distance == "far":
        commands.append(f"goto {args.far_waypoint}")
    if controller == "drive":
        commands.append(f"navdemos drive {args.npc} {pair['start']} {pair['goal']} {args.limit_ms} {args.weights} 0 {args.seed}")
    else:
        commands.append(f"navdemos population {args.npc} {pair_file} {args.limit_ms} {args.weights} 1 {args.seed} argmax 1 1 {distance} 0")
    return [*commands, "navdemos wait"]


def record_equivalence(args, runner, index, pair, pair_file, mode):
    worker = copy(args)
    worker.seed = pair.get("seed", args.seed)
    directory = args.output / mode / f"{index:05d}"
    episode_path = directory / "episode.json" if mode.startswith("drive-") else directory / "episodes" / "0" / "episode.json"
    if episode_path.is_file():
        if not args.resume:
            raise ValueError(f"Existing equivalence output: {episode_path}; use --resume")
    else:
        scenario(worker, runner, f"equivalence-{mode}-{index:05d}",
                 equivalence_commands(worker, pair, pair_file, mode), directory)
    episode = read_json(episode_path)
    if ((episode.get("start"), episode.get("goal")) != (pair["start"], pair["goal"])
            or episode.get("demo_version") != 2 or episode.get("physics_mode") != getattr(args, "physics", "cv")):
        raise ValueError(f"Equivalence route/schema/physics differs from requested setup: {episode_path}")
    episode.update(region=pair["region"], index=index, split="equivalence", split_index=index,
                   drive=True, weights=str(args.weights), seed=worker.seed, limit_ms=args.limit_ms)
    write_json(episode_path, episode)
    print(json.dumps(dict(pair=index, mode=mode, success=episode["success"],
                          elapsed_ms=episode["elapsed_ms"], episode=str(episode_path.parent))), flush=True)


def trajectory_divergence(reference, repeated):
    """Locate the first geometric/action difference without calling it a cause."""
    import numpy as np
    paths = [Path(episode["directory"]) for episode in (reference, repeated)]
    if not all((path / "trajectory.txt").is_file() and (path / "actions.txt").is_file() for path in paths):
        return dict(available=False)
    traces = [np.loadtxt(path / "trajectory.txt", ndmin=2) for path in paths]
    actions = [np.loadtxt(path / "actions.txt", ndmin=2) for path in paths]
    if any(trace.shape[1] != 5 or not np.isfinite(trace).all() for trace in traces):
        raise ValueError("Invalid equivalence trajectory")
    if any(keys.shape[1] != 9 or not np.isfinite(keys).all() for keys in actions):
        raise ValueError("Invalid equivalence action trace")
    common, first, second = np.intersect1d(traces[0][:, 0], traces[1][:, 0], return_indices=True)
    if not len(common):
        raise ValueError("Equivalence trajectories have no common timestamps")
    delta = np.linalg.norm(traces[0][first, 1:4] - traces[1][second, 1:4], axis=1)
    result = dict(available=True, compared_rows=len(common), max_position_error_cm=float(delta.max()),
                  rms_position_error_cm=float(np.sqrt(np.mean(delta ** 2))))
    for threshold in (.1, 1., 25.):
        found = np.flatnonzero(delta > threshold)
        row = int(found[0]) if len(found) else None
        result[f"first_over_{threshold:g}_cm"] = None if row is None else dict(
            time_ms=float(common[row]), error_cm=float(delta[row]),
            reference_position_cm=traces[0][first[row], 1:4].tolist(), repeated_position_cm=traces[1][second[row], 1:4].tolist())
    times, ia, ib = np.intersect1d(actions[0][:-1, 0], actions[1][:-1, 0], return_indices=True)
    changed = np.flatnonzero((actions[0][ia, 1:7] != actions[1][ib, 1:7]).any(axis=1))
    result["first_action_difference"] = None if not len(changed) else dict(
        time_ms=float(times[changed[0]]), reference_keys=actions[0][ia[changed[0]], 1:7].tolist(),
        repeated_keys=actions[1][ib[changed[0]], 1:7].tolist())
    return result


def controlled_comparison(reference, repeated):
    result = comparison_metrics(reference, repeated)
    for pair in result["pairs"]:
        key = pair["start"], pair["goal"]
        first, second = reference[key], repeated[key]
        compared = first["success"] and second["success"]
        pair["timing_compared"] = compared
        pair["same_outcome"] = first["success"] == second["success"]
        pair["passed"] = pair["same_outcome"] and pair["elapsed_relative_error"] <= .05
        pair["trajectory"] = trajectory_divergence(first, second)
    result["passed"] = all(pair["passed"] for pair in result["pairs"])
    result["timing_evidence_routes"] = sum(pair["timing_compared"] for pair in result["pairs"])
    return result


def distance_comparison_metrics(near, far):
    """Far simulation must retain reachability and avoid slowing common successes."""
    symmetric = comparison_metrics(near, far)
    lost = [dict(start=start, goal=goal) for (start, goal), episode in near.items()
            if episode["success"] and not far[start, goal]["success"]]
    common = []
    for key, episode in near.items():
        other = far[key]
        if not (episode["success"] and other["success"]):
            continue
        common.append(dict(start=key[0], goal=key[1], near_elapsed_ms=episode["elapsed_ms"],
                           far_elapsed_ms=other["elapsed_ms"],
                           passed=other["elapsed_ms"] <= episode["elapsed_ms"] * 1.05))
    reachability_passed = symmetric["repeated_arrived"] >= symmetric["reference_arrived"]
    return dict(passed=reachability_passed and not lost and all(pair["passed"] for pair in common),
                criterion="no_lost_near_success_and_no_more_than_5pct_slower_on_each_common_success",
                time_tolerance=.05, reachability_passed=reachability_passed,
                near_arrived=symmetric["reference_arrived"], far_arrived=symmetric["repeated_arrived"],
                lost_success_routes=lost, common_successes=common, symmetric_diagnostic=symmetric)


def equivalence(args):
    checked_paths(args, policy=True)
    pairs = load_pairs(args.pairs)
    if args.count > len(pairs):
        raise ValueError(f"Need {args.count} routes, found {len(pairs)}")
    chosen = pairs[:args.count]
    write_pairs(args.output / "selected-pairs.jsonl", chosen)
    pair_files = [runner_pairs(args.output / "runner-pairs" / f"equivalence-{index:05d}.txt", [pair])
                  for index, pair in enumerate(chosen)]
    modes = ("drive-near", "pop-near", "drive-far", "pop-far")
    if args.parallel == 1:
        runner = new_runner(args, "equivalence")
        try:
            for index, pair in enumerate(chosen):
                for mode in modes:
                    record_equivalence(args, runner, index, pair, pair_files[index], mode)
        finally:
            runner.close()
    else:
        def record_mode(mode):
            runner = new_runner(args, f"equivalence-{mode}")
            try:
                for index, pair in enumerate(chosen):
                    record_equivalence(args, runner, index, pair, pair_files[index], mode)
            finally:
                runner.close()
        with ThreadPoolExecutor(max_workers=min(args.parallel, len(modes))) as pool:
            futures = [pool.submit(record_mode, mode) for mode in modes]
            for future in futures:
                future.result()
    return summarize_equivalence(args, chosen)


def summarize_equivalence(args, chosen):
    """Aggregate existing four-mode recordings; never prepare or restart a world."""
    modes = ("drive-near", "pop-near", "drive-far", "pop-far")
    recorded = {mode: comparable_episodes(args.output / mode) for mode in modes}
    comparisons = {}
    for label, first, second in (("drive_vs_population_near", "drive-near", "pop-near"),
                                 ("drive_vs_population_far", "drive-far", "pop-far"),
                                 ("drive_near_vs_far", "drive-near", "drive-far"),
                                 ("population_near_vs_far", "pop-near", "pop-far")):
        comparisons[label] = controlled_comparison(recorded[first], recorded[second])
    distances = [dict(mode=mode, start=start, goal=goal, min_hero_distance_cm=episode["min_hero_distance_cm"],
                      max_hero_distance_cm=episode["max_hero_distance_cm"],
                      far_enough=episode["min_hero_distance_cm"] >= args.min_far_cm)
                 for mode in ("drive-far", "pop-far") for (start, goal), episode in recorded[mode].items()]
    raw_outliers = defaultdict(list)
    for label, comparison in comparisons.items():
        for pair in comparison["pairs"]:
            if not pair["passed"]:
                raw_outliers[pair["start"], pair["goal"]].append(label)
    outliers = defaultdict(list)
    for label in ("drive_vs_population_near", "drive_vs_population_far"):
        for pair in comparisons[label]["pairs"]:
            if not pair["passed"]:
                outliers[pair["start"], pair["goal"]].append(label)
    distance_noninferiority = {}
    for controller, near, far, label in (("drive", "drive-near", "drive-far", "drive_near_vs_far"),
                                         ("population", "pop-near", "pop-far", "population_near_vs_far")):
        metric = distance_comparison_metrics(recorded[near], recorded[far])
        distance_noninferiority[controller] = metric
        for pair in metric["lost_success_routes"]:
            outliers[pair["start"], pair["goal"]].append(label)
        for pair in metric["common_successes"]:
            if not pair["passed"]:
                outliers[pair["start"], pair["goal"]].append(label)
    preparation_passed = all(pair["far_enough"] for pair in distances)
    result = dict(passed=preparation_passed and len(outliers) <= 2,
                  strict_passed=preparation_passed and not raw_outliers, preparation_passed=preparation_passed,
                  n=len(chosen), physics=getattr(args, "physics", "cv"), time_tolerance=.05,
                  max_tolerated_outlier_routes=2, outlier_routes=[dict(start=key[0], goal=key[1], comparisons=labels)
                      for key, labels in sorted(outliers.items())],
                  selection="first_count_routes_in_supplied_order_before_policy_results",
                  world_reset="same_save_and_seed_before_every_episode_in_each_of_four_modes",
                  far_waypoint=args.far_waypoint, min_required_hero_distance_cm=args.min_far_cm,
                  comparisons=comparisons, distance_noninferiority=distance_noninferiority,
                  raw_symmetric_outlier_routes=[dict(start=key[0], goal=key[1], comparisons=labels)
                      for key, labels in sorted(raw_outliers.items())], far_distances=distances)
    write_json(args.output / "equivalence.json", result)
    print(json.dumps(result, ensure_ascii=False))
    if not result["passed"]:
        raise ValueError("Navigation equivalence failed setup or more than two unique routes; see equivalence.json")
    return result


def death_reset_metrics(episodes):
    if len(episodes) != 2:
        raise ValueError("Death smoke requires exactly two consecutive episodes")
    first, second = episodes
    initial = second["initial_state"]
    expected = initial.get("expected_start_position_cm", second.get("expected_start_position_cm"))
    if expected is None:
        raise ValueError("Death reset metadata lacks expected start pose")
    pose_error = math.dist(initial["position_cm"], expected)
    held = initial["held_keys"]
    checks = dict(actual_death=bool(first.get("kill_injected")) and first["final_dead"] and first["final_hp"] <= 0,
                  actual_ragdoll=bool(first["final_ragdoll"]),
                  death_state_ran=bool(first["final_state_started"]),
                  single_injection=not second.get("kill_injected", False),
                  reset_from_actual_death=bool(second["reset_after_death"]),
                  recurrent_state_exercised=first["final_gru_l2"] > 1e-8,
                  health_restored=initial["hp"] == initial["hp_max"] and initial["hp"] > 0,
                  live_pose=not any(initial[key] for key in ("down", "dead", "ragdoll", "in_air", "climbing")),
                  start_pose_restored=pose_error <= 1.,
                  recurrent_state_cleared=abs(initial["gru_l2"]) <= 1e-8,
                  memory_cleared=abs(initial["memory_traveled_cm"]) <= 1e-8,
                  guidance_cleared=initial["guidance_points"] == 0,
                  eye_cleared=not initial["eye_observed"],
                  keys_released=len(held) == 6 and all(held[index] == 0 for index in (0, 1, 2, 4, 5)) and held[3] in (0, 1))
    return dict(passed=all(checks.values()), checks=checks, reset_position_error_cm=pose_error,
                initial_state=initial, first_final_hp=first["final_hp"], first_kill_at_ms=first["kill_at_ms"])


def death_smoke(args):
    checked_paths(args, policy=True)
    pairs = load_pairs(args.pairs)
    selected = pairs[:2] if len(pairs) > 1 else pairs
    pair_file = runner_pairs(args.output / "death-pairs.txt", selected)
    directory = args.output / "death-population"
    if (directory / "population.json").exists():
        raise ValueError(f"Death smoke output exists: {directory}; choose a fresh output")
    runner = new_runner(args, "death-smoke")
    try:
        scenario(args, runner, "death-smoke", [f"roads load {args.roads}", f"roads on {args.npc}",
                 "navdemos kill-slot 0 400",
                 f"navdemos population {args.npc} {pair_file} 1000 {args.weights} 1 {args.seed} argmax 2 1 far 0",
                 "navdemos wait"], directory)
    finally:
        runner.close()
    episodes = [read_json(path) for path in sorted((directory / "episodes").glob("*/episode.json"),
                                                 key=lambda p: int(p.parent.name))]
    if any(episode.get("physics_mode") != getattr(args, "physics", "cv") for episode in episodes):
        raise ValueError("Death smoke did not use requested stand physics")
    result = death_reset_metrics(episodes)
    recorded = [check_episode(directory / "episodes" / str(episode["episode_index"])) for episode in episodes]
    result.update(physics=getattr(args, "physics", "cv"), recording_checks=recorded,
                  death_reward=sum(record["reward_sums"].get("death", 0) for record in recorded))
    result["passed"] = result["passed"] and result["death_reward"] == 1
    write_json(args.output / "death-smoke.json", result)
    print(json.dumps(result, ensure_ascii=False))
    if not result["passed"]:
        raise ValueError("Death/reset smoke failed; see death-smoke.json")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("generate", "qualify", "drive", "population", "check", "summarize", "history", "compare", "equivalence", "death-smoke", "dagger-calibrate"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--pairs", type=Path)
    parser.add_argument("--manifest", type=Path, help="Frozen v4 corpus manifest; drive reads splits.validation only")
    parser.add_argument("--detour-scenarios", type=Path, help="Frozen detour-val JSONL; drive only, text recording without NAVDEMO")
    parser.add_argument("--dagger-scenarios", type=Path, help="Explicit train-only JSONL; drive records learner memory and online teacher labels")
    parser.add_argument("--teacher-beta", type=float, default=0.1, help="DAgger baseline teacher probability, clamped to at least 0.1")
    parser.add_argument("--eval-screen", type=Path, help="Frozen validation screen JSONL; drive only, always lean")
    parser.add_argument("--lean", action="store_true", help="Manifest validation with text-only physical outcomes; no NAVDEMO")
    parser.add_argument("--families", help="Comma-separated validation families; default all five")
    parser.add_argument("--object-probes", type=Path, help="Frozen object Detour probes for validation summary")
    parser.add_argument("--action-seeds", help="Fixed sampling seeds; default 18102027,18102028,18102029")
    parser.add_argument("--jump-links", type=Path, help="Frozen jump links, used only for the jump family")
    parser.add_argument("--safety-fixtures", type=Path, help="Existing physically qualified nav-perception-safety fixtures")
    parser.add_argument("--fence-control", action="store_true", help="Include the existing Maleth fence control")
    parser.add_argument("--source", type=Path)
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--exclude", type=Path, action="append", default=[])
    parser.add_argument("--seed", type=int, default=18102026)
    parser.add_argument("--per-region", type=int, default=10)
    parser.add_argument("--max-teacher-candidates", type=int, default=50)
    parser.add_argument("--physics", choices=("cv", "native"), default="cv")
    parser.add_argument("--count", type=int, default=10, help="First routes for independent equivalence episodes")
    parser.add_argument("--far-waypoint", default="NW_FARM1_ENTRANCE_01")
    parser.add_argument("--min-far-cm", type=float, default=10000)
    parser.add_argument("--candidates-per-region", type=int, default=200)
    parser.add_argument("--npc", default="BAU_954_MALETH")
    parser.add_argument("--population", type=int, default=20)
    parser.add_argument("--parallel", type=int, default=1)
    parser.add_argument("--episodes", type=int, help="Global episode cap; default N, or 1000000 with --run-ms")
    parser.add_argument("--record", action="store_true")
    parser.add_argument("--action-mode", choices=("sample", "argmax"),
                        help="Action selection, default sample; manifest validation requires sample")
    parser.add_argument("--hero", choices=("near", "far"), default="far")
    parser.add_argument("--limit-ms", type=int, default=240000)
    parser.add_argument("--run-ms", type=int, default=0, help="Population game-time span; 0 runs all episodes")
    parser.add_argument("--watchdog-seconds", type=float, default=3600)
    parser.add_argument("--logprob-tolerance", type=float, default=1e-4)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--indices")
    parser.add_argument("--roads", type=Path, default=REPO / "server/src/gothic_ai/gamedata/places.json")
    parser.add_argument("--familiar-map", type=Path, help="Eye-baked familiar atlas; required for the hd-familiar policy input")
    parser.add_argument("--stranger", action="store_true", help="Evaluate validation drive without the resident's known roads or familiar map")
    parser.add_argument("--links", type=Path, default=REPO / "reports/navigation/2026-10-16-teacher/raw/accepted-links.txt")
    for name in ("runner", "assets", "save", "cache", "ini", "weights"):
        parser.add_argument("--" + name, type=Path)
    args = parser.parse_args()
    if args.parallel < 1:
        parser.error("--parallel must be a positive integer")
    if args.action_mode is None:
        args.action_mode = "sample"
    for key, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, key, value.expanduser().resolve())
    args.exclude = [path.expanduser().resolve() for path in args.exclude]
    if args.episodes is None:
        args.episodes = 1000000 if args.run_ms else args.population
    if not 0 <= args.seed <= 0xffffffff or args.per_region < 1 or args.max_teacher_candidates < 1 or args.count < 1 or args.min_far_cm < 0 or args.population < 1 or args.episodes < 1 or args.limit_ms < 1 or args.run_ms < 0:
        parser.error("seed must be uint32; per-region, population, episodes and limit-ms must be positive")
    if args.manifest is not None and (args.command != "drive" or args.pairs is not None):
        parser.error("--manifest is exclusive with --pairs and only supports validation drive")
    if args.detour_scenarios is not None and (args.command != "drive" or args.pairs is not None or args.manifest is not None):
        parser.error("--detour-scenarios is exclusive with --pairs/--manifest and only supports validation drive")
    if args.dagger_scenarios is not None and (args.command != "drive" or args.pairs is not None
            or args.manifest is not None or args.detour_scenarios is not None or args.lean
            or args.eval_screen is not None or args.families is not None or args.safety_fixtures is not None or args.fence_control):
        parser.error("--dagger-scenarios requires an exclusive train recording drive")
    if not math.isfinite(args.teacher_beta) or not 0 <= args.teacher_beta <= 1:
        parser.error("--teacher-beta must be a probability")
    if args.eval_screen is not None:
        if (args.command != "drive" or any(getattr(args, name) is not None for name in
                ("pairs", "manifest", "detour_scenarios", "dagger_scenarios", "indices", "families", "action_seeds", "safety_fixtures"))
                or args.fence_control):
            parser.error("--eval-screen is exclusive with other cohort selection options and only supports validation drive")
        args.lean = True
    if args.lean and (args.command != "drive" or (args.manifest is None and args.detour_scenarios is None and args.eval_screen is None)):
        parser.error("--lean requires manifest, detour or screen validation drive")
    if args.stranger and (args.command != "drive" or (args.manifest is None and args.detour_scenarios is None and args.eval_screen is None)):
        parser.error("--stranger requires manifest, detour or screen validation drive")
    if (args.command in ("qualify", "drive", "population", "equivalence", "death-smoke")
            and args.pairs is None and args.manifest is None and args.detour_scenarios is None
            and args.eval_screen is None and args.dagger_scenarios is None):
        parser.error("--pairs is required")
    if args.command in ("check", "summarize", "compare", "dagger-calibrate") and args.source is None:
        parser.error("--source is required")
    if args.command == "compare" and args.reference is None:
        parser.error("--reference is required")
    if args.command == "population" and (args.episodes < args.population or (args.hero == "near" and args.population != 1)):
        parser.error("population requires episodes >= N; near hero mode requires N=1")
    try:
        {"generate": generate, "qualify": qualify, "drive": drive, "population": population,
         "check": check, "summarize": summarize, "history": history, "compare": compare, "equivalence": equivalence, "death-smoke": death_smoke, "dagger-calibrate": dagger_calibrate}[args.command](args)
    except (ValueError, OSError, RuntimeError, KeyError) as exc:
        parser.exit(1, f"nav-rl: {exc}\n")


if __name__ == "__main__":
    main()
