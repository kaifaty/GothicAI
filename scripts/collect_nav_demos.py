#!/usr/bin/env python3
"""Collect keyboard teacher episodes with fresh seeded body-runner worlds.

Run through nav-demos-batch.sh on Ryzen: the shell releases the shared simulation
lock between small batches. Outputs stay outside git in ~/gothic-ai/nav-demos.
This orchestration uses the standard library; dataset loading is independent.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import importlib.util
import json
import math
import os
from pathlib import Path
import random
import re
import statistics
import sys
import threading
import time


REPO = Path(__file__).resolve().parent.parent
DEFAULT_OUTPUT = Path.home() / "gothic-ai/nav-demos"
NPC = "BAU_954_MALETH"
DAGGER_MAX_ATTEMPTS = 32


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def load_pairs(path):
    with path.open(encoding="utf-8") as stream:
        pairs = [json.loads(line) for line in stream if line.strip()]
    for index, pair in enumerate(pairs):
        if not all(isinstance(pair.get(key), str) and pair[key] for key in ("region", "start", "goal")):
            raise ValueError(f"Invalid pair {index}: {pair}")
    return pairs


def numeric_lines(path, columns=None):
    values = []
    with path.open(encoding="utf-8") as stream:
        for number, line in enumerate(stream, 1):
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            try:
                row = tuple(float(value) for value in line.split())
            except ValueError as exc:
                raise ValueError(f"Invalid numeric line {path}:{number}") from exc
            if columns is not None and len(row) != columns:
                raise ValueError(f"Expected {columns} columns at {path}:{number}, got {len(row)}")
            if not row or not all(math.isfinite(value) for value in row):
                raise ValueError(f"Empty/nonfinite numeric line {path}:{number}")
            values.append(row)
    return values


def decision_count(directory):
    # The terminal row contains neutral keys and closes the final held interval.
    actions = numeric_lines(directory / "actions.txt")
    return max(0, len(actions) - 1)


def episode_directory(args, index):
    if args.command == "collect":
        return args.output / "episodes" / f"{index:03d}"
    if args.command == "replay":
        return args.output / "replays" / f"{index:03d}"
    if args.command == "dagger":
        return args.output / "dagger" / f"{index:03d}"
    return args.output / "performance" / f"p{args.parallel}" / f"{index:03d}"


def unsupported_preparation(directory):
    episode = directory / "episode.json"
    if not episode.is_file():
        return False
    value = read_json(episode)
    return (value.get("failure") == "dagger_start_not_supported"
            and value.get("elapsed_ms") == 0 and value.get("decisions") == 0)


def completed(directory, replay=False):
    if unsupported_preparation(directory):
        return False
    required = ("episode.json", "trajectory.txt")
    if not replay:
        required += ("actions.txt", "samples.bin.gz")
    return all((directory / name).is_file() for name in required)


def select_indices(args, pairs):
    if args.indices:
        indices = [int(value) for value in args.indices.split(",")]
        if len(indices) != len(set(indices)) or any(index < 0 for index in indices):
            raise ValueError("--indices must contain distinct nonnegative integers")
    elif args.command == "replay":
        candidates = []
        for index in range(len(pairs)):
            directory = args.output / "episodes" / f"{index:03d}"
            if completed(directory) and bool(read_json(directory / "episode.json").get("success")):
                candidates.append(index)
        indices = candidates[args.start_index:args.start_index + (args.count or 20)]
        if len(indices) < (args.count or 20):
            raise ValueError(f"Need {args.count or 20} successful recorded episodes, found {len(indices)}")
    else:
        default_count = {"collect": len(pairs), "dagger": 50, "performance": 4}[args.command]
        count = args.count if args.count is not None else default_count - args.start_index
        indices = list(range(args.start_index, args.start_index + count))
    limit = {"collect": len(pairs), "replay": len(pairs), "dagger": 50}.get(args.command)
    if limit is not None and any(index >= limit for index in indices):
        raise ValueError(f"Index outside {args.command} range [0, {limit})")
    if args.resume:
        indices = [index for index in indices if not completed(episode_directory(args, index), args.command == "replay")]
    return indices


def scenario_file(args, name, commands):
    path = args.output / "scenarios" / f"{name}.bench"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join([f"seed {args.seed}", "cheat god", "set time 10 0", f"navdemos physics {getattr(args, 'physics', 'native')}", *commands,
                                "label done", ""]), encoding="utf-8")
    return path


def runner_type():
    # Explicit file import also works with python3 -I, without changing sys.path.
    specification = importlib.util.spec_from_file_location("navmesh_measure", REPO / "scripts/navmesh_measure.py")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module.Runner


def check_log(output):
    log = output / "bench.log"
    if not log.is_file():
        raise RuntimeError(f"Runner did not write {log}")
    text = log.read_text(encoding="utf-8", errors="replace")
    bad = re.search(r"navdemos_error|unknown command|roads:.*(?:error|failed|expected)|navmesh:.*(?:error|expected)", text, re.I)
    if bad:
        raise RuntimeError(f"Bench preparation failed in {output}: {bad.group(0)}")


def dagger_states(args, pairs):
    generator = random.Random(args.seed ^ 0xDA66E2)
    return [dict(pair=generator.randrange(len(pairs)),
                 lateral_cm=generator.uniform(100, 500) * generator.choice((-1, 1)),
                 yaw_deg=generator.uniform(-180, 180), fraction=0.5) for _ in range(50)]


def dagger_candidate(args, pairs, states, index, attempt):
    if attempt == 0:
        return states[index]  # Keep all original first candidates unchanged.
    generator = random.Random((args.seed << 32) ^ index ^ 0xDA66E2A1)
    for _ in range(attempt):
        state = dict(pair=generator.randrange(len(pairs)),
                     lateral_cm=generator.uniform(100, 500) * generator.choice((-1, 1)),
                     yaw_deg=generator.uniform(-180, 180), fraction=0.5)
    return state


def dagger_rejections(args, index):
    paths = sorted((args.output / "dagger-rejected").glob(f"{index:03d}-attempt-*"),
                   key=lambda path: int(path.name.rsplit("-", 1)[1]))
    for attempt, path in enumerate(paths):
        if path.name != f"{index:03d}-attempt-{attempt}" or not unsupported_preparation(path):
            raise RuntimeError(f"Invalid DAgger preparation history: {path}")
    return paths


def archive_dagger_rejection(args, index, directory, rejected):
    destination = args.output / "dagger-rejected" / f"{index:03d}-attempt-{len(rejected)}"
    destination.parent.mkdir(parents=True, exist_ok=True)
    directory.rename(destination)
    rejected.append(destination)
    print(f"dagger {index:03d}: rejected zero-step preparation -> {destination}", flush=True)


def dagger_preparation_metadata(args, rejected, attempt):
    return dict(dagger_preparation_attempt=attempt, dagger_rejected_preparation_count=len(rejected),
                dagger_rejected_preparations=[dict(path=str(path.relative_to(args.output)),
                                                   failure="dagger_start_not_supported",
                                                   elapsed_ms=0, decisions=0) for path in rejected])


def compare_replay(recorded, replayed, tolerance_cm):
    reference = numeric_lines(recorded / "trajectory.txt", 5)
    replay = numeric_lines(replayed / "trajectory.txt", 5)
    if not reference or not replay:
        raise ValueError("Replay comparison requires nonempty trajectories")
    reference_by_time = {row[0]: row for row in reference}
    replay_by_time = {row[0]: row for row in replay}
    if len(reference_by_time) != len(reference) or len(replay_by_time) != len(replay):
        raise ValueError("Duplicate trajectory timestamps")
    common = sorted(reference_by_time.keys() & replay_by_time.keys())
    if not common:
        raise ValueError("Replay has no shared trajectory timestamps")
    distances = [math.dist(reference_by_time[t][1:4], replay_by_time[t][1:4]) for t in common]
    yaw_errors = [abs((reference_by_time[t][4] - replay_by_time[t][4] + 180) % 360 - 180) for t in common]
    original = read_json(recorded / "episode.json")
    repeated = read_json(replayed / "episode.json")
    first_position = original.get("final_position", reference[-1][1:4])
    last_position = repeated.get("final_position", replay[-1][1:4])
    if isinstance(first_position, dict):
        first_position = [first_position[key] for key in ("x", "y", "z")]
    if isinstance(last_position, dict):
        last_position = [last_position[key] for key in ("x", "y", "z")]
    endpoint = math.dist(first_position, last_position)
    time_equal = [row[0] for row in reference] == [row[0] for row in replay]
    ordered = sorted(distances)
    result = dict(passed=time_equal and endpoint <= tolerance_cm and max(distances) <= tolerance_cm,
                  tolerance_cm=tolerance_cm, endpoint_cm=endpoint, trajectory_max_cm=max(distances),
                  trajectory_mean_cm=statistics.fmean(distances),
                  trajectory_rms_cm=math.sqrt(statistics.fmean(value * value for value in distances)),
                  trajectory_p95_cm=ordered[max(0, math.ceil(0.95 * len(ordered)) - 1)],
                  yaw_max_deg=max(yaw_errors), timestamps_equal=time_equal,
                  reference_rows=len(reference), replay_rows=len(replay), compared_rows=len(common),
                  missing_replay_times_ms=sorted(reference_by_time.keys() - replay_by_time.keys()),
                  extra_replay_times_ms=sorted(replay_by_time.keys() - reference_by_time.keys()),
                  first_divergence_over_1mm_ms=next((t for t, error in zip(common, distances) if error > 0.1), None),
                  first_divergence_over_tolerance_ms=next((t for t, error in zip(common, distances) if error > tolerance_cm), None))
    write_json(replayed / "replay-comparison.json", result)
    return result


def record_episode(args, runner, pairs, states, index, state=None, preparation=None):
    directory = episode_directory(args, index)
    if completed(directory, args.command == "replay"):
        raise ValueError(f"Completed output exists: {directory}; use --resume or a fresh output")
    state = state if state is not None else (states[index] if args.command == "dagger" else None)
    pair_index = state["pair"] if state else index % len(pairs)
    pair = pairs[pair_index]
    # Known roads are enabled on the actor before the teacher moves it. This keeps
    # Maleth's genuine familiarity even for city/Onar starts where he is a stranger.
    commands = [f"roads load {args.roads}", f"roads on {args.npc}"]
    limit_ms = args.limit_ms or (10000 if args.command == "performance" else 240000)
    if args.command == "replay":
        recorded = args.output / "episodes" / f"{index:03d}"
        commands.append(f"navdemos replay {args.npc} {pair['start']} {pair['goal']} {recorded / 'actions.txt'}")
    else:
        start = "start-v2" if getattr(args, "teacher_version", 1) == 2 else "start"
        command = f"navdemos {start} {args.npc} {pair['start']} {pair['goal']} {limit_ms}"
        if state:
            command += f" {state['lateral_cm']:.9g} {state['yaw_deg']:.9g} {state['fraction']:.9g}"
        commands.append(command)
    commands.append("navdemos wait")
    scenario = scenario_file(args, f"{args.command}-p{args.parallel}-{index:03d}", commands)
    started = time.monotonic()
    runner.scenario(scenario, directory, args.seed, timeout=args.watchdog_seconds)
    elapsed = time.monotonic() - started
    check_log(directory)
    if not completed(directory, args.command == "replay"):
        # Only zero-step unsupported placements can be retried by run_episode.
        if not unsupported_preparation(directory):
            raise RuntimeError(f"Incomplete navdemos output: {directory}")
    episode = read_json(directory / "episode.json")
    if args.command == "replay":
        count = decision_count(recorded)
    else:
        count = decision_count(directory) if (directory / "actions.txt").is_file() else 0
    episode.update(pair=pair_index, region=pair["region"], start=pair["start"], goal=pair["goal"],
                   seed=args.seed, npc=args.npc, limit_ms=episode.get("limit_ms", limit_ms), collector_wall_s=elapsed,
                   collector_decisions=count, collector_decisions_per_second=count / elapsed)
    if state:
        episode.update(dagger=index, dagger_lateral_cm=state["lateral_cm"],
                       dagger_yaw_deg=state["yaw_deg"], dagger_fraction=state["fraction"])
    if preparation is not None:
        episode.update(preparation)
    write_json(directory / "episode.json", episode)
    if args.command == "replay":
        comparison = compare_replay(recorded, directory, args.tolerance_cm)
        print(f"replay {index:03d}: passed={comparison['passed']} endpoint_cm={comparison['endpoint_cm']:.6g} "
              f"trajectory_max_cm={comparison['trajectory_max_cm']:.6g}", flush=True)
    else:
        print(f"{args.command} {index:03d}: success={episode.get('success')} "
              f"elapsed_ms={episode.get('elapsed_ms')} decisions={count} "
              f"wall_s={elapsed:.3f} decisions/s={count / elapsed:.2f}", flush=True)
    return count


def run_episode(args, runner, pairs, states, index):
    if args.command != "dagger":
        return record_episode(args, runner, pairs, states, index)
    directory = episode_directory(args, index)
    rejected = dagger_rejections(args, index)
    if unsupported_preparation(directory):
        # Resume old collections that counted an unsupported placement among 50.
        archive_dagger_rejection(args, index, directory, rejected)
    elif (directory / "episode.json").is_file():
        raise ValueError(f"Existing physical DAgger episode is preserved: {directory}; use --resume")
    for attempt in range(len(rejected), DAGGER_MAX_ATTEMPTS):
        state = dagger_candidate(args, pairs, states, index, attempt)
        count = record_episode(args, runner, pairs, states, index, state,
                               dagger_preparation_metadata(args, rejected, attempt))
        if not unsupported_preparation(directory):
            return count  # Any real failure after control began remains in the 50.
        archive_dagger_rejection(args, index, directory, rejected)
    raise RuntimeError(f"DAgger {index:03d}: preparation exhausted {DAGGER_MAX_ATTEMPTS} attempts; "
                       f"no physical episode collected. Rejected starts: {args.output / 'dagger-rejected'}")


def worker(args, pairs, states, indices, worker_index=0, barrier=None):
    from copy import copy
    worker_args = copy(args)
    # Runner's mutable workdir/stderr must never be shared by concurrent processes.
    worker_args.output = args.output / "runner-work" / args.command / f"p{args.parallel}-w{worker_index}"
    worker_args.output.mkdir(parents=True, exist_ok=True)
    prepare = scenario_file(args, f"prepare-{args.command}-p{args.parallel}-w{worker_index}",
                            [f"navmesh build {args.cache} {args.links}"])
    runner = runner_type()(worker_args, prepare)
    try:
        runner.scenario(prepare, worker_args.output / "prepare", args.seed, timeout=720)
        check_log(worker_args.output / "prepare")
        # Process/world preparation is excluded from episode collection timing.
        if barrier is not None:
            barrier.wait(timeout=720 + args.watchdog_seconds)
        started = time.monotonic()
        total = sum(run_episode(args, runner, pairs, states, index) for index in indices)
        return dict(worker=worker_index, episodes=len(indices), decisions=total,
                    wall_s=time.monotonic() - started)
    finally:
        if barrier is not None:
            barrier.abort()
        runner.close()


def run(args, pairs, indices):
    if sys.platform != "linux":
        raise RuntimeError("Run heavy nav-demos collection on Ryzen/Linux")
    for name in ("runner", "assets", "save", "cache", "links", "roads"):
        path = getattr(args, name)
        if path is None:
            raise ValueError(f"--{name} is required")
        if name != "cache" and not path.exists():
            raise ValueError(f"Missing {name}: {path}")
    if args.parallel != 1 and args.command != "performance":
        raise ValueError("--parallel is reserved for short performance measurements")
    args.output.mkdir(parents=True, exist_ok=True)
    args.cache.mkdir(parents=True, exist_ok=True)
    os.environ["OPENGOTHIC_BODY_TRACE"] = "0"
    states = dagger_states(args, pairs)
    if not indices:
        print("All selected episodes are already complete.")
        return
    if args.parallel == 1:
        measurements = [worker(args, pairs, states, indices)]
    else:
        # Warm the immutable cache sequentially once before concurrent readers.
        worker(args, pairs, states, [], args.parallel)
        actual_workers = min(args.parallel, len(indices))
        barrier = threading.Barrier(actual_workers)
        with ThreadPoolExecutor(max_workers=args.parallel) as pool:
            futures = [pool.submit(worker, args, pairs, states, indices[i::args.parallel], i, barrier)
                       for i in range(actual_workers)]
            measurements = [future.result() for future in futures]
    if args.command == "performance":
        directory = args.output / "performance" / f"p{args.parallel}"
        directory.mkdir(parents=True, exist_ok=True)
        # max worker time is the simultaneous collection span; preparation is
        # separately visible in runner logs and excluded from these throughput figures.
        total = sum(item["decisions"] for item in measurements)
        wall = max(item["wall_s"] for item in measurements)
        write_json(directory / f"throughput-{indices[0]:03d}-{indices[-1]:03d}.json",
                   dict(parallel=args.parallel, actual_workers=len(measurements), decisions=total,
                        collection_wall_s=wall, decisions_per_second=total / wall, workers=measurements))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("collect", "replay", "dagger", "performance"))
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--pairs", type=Path, default=REPO / "reports/navigation/2026-10-16-navmesh/raw/pairs.jsonl")
    parser.add_argument("--roads", type=Path, default=REPO / "server/src/gothic_ai/gamedata/places.json")
    parser.add_argument("--links", type=Path, default=REPO / "reports/navigation/2026-10-16-teacher/raw/accepted-links.txt")
    for name in ("runner", "assets", "save", "cache", "ini"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--npc", default=NPC)
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--teacher-version", type=int, choices=(1, 2), default=1)
    parser.add_argument("--physics", choices=("native", "cv"), default="native")
    parser.add_argument("--limit-ms", type=int)
    parser.add_argument("--parallel", type=int, default=1)
    parser.add_argument("--start-index", type=int, default=0)
    parser.add_argument("--count", type=int)
    parser.add_argument("--indices", help="Explicit comma-separated original pair/state indices")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--list-indices", action="store_true", help="Print pending indices without launching a runner")
    parser.add_argument("--tolerance-cm", type=float, default=10)
    parser.add_argument("--watchdog-seconds", type=float, default=300)
    args = parser.parse_args()
    for key, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, key, value.expanduser().resolve())
    if args.parallel not in (1, 2, 4) or args.start_index < 0 or (args.count is not None and args.count < 1):
        parser.error("parallel must be 1/2/4, start-index >=0 and count >=1")
    if not 0 <= args.seed <= 0xffffffff or (args.limit_ms is not None and args.limit_ms < 1000):
        parser.error("seed must be uint32 and limit >=1000ms")
    if args.parallel > 1 and args.command != "performance":
        parser.error("parallel runners are only supported in performance mode")
    try:
        pairs = load_pairs(args.pairs)
        indices = select_indices(args, pairs)
        if args.list_indices:
            print(" ".join(str(index) for index in indices))
        else:
            run(args, pairs, indices)
    except (ValueError, OSError, RuntimeError) as exc:
        parser.exit(1, f"nav-demos: {exc}\n")


if __name__ == "__main__":
    main()
