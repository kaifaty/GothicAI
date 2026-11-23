#!/usr/bin/env python3
"""Record S4a population parity and the 60-route stand reference on Ryzen/gq."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import nav_rl as rl

SEED = 18102027
POPULATION = 6
LIMIT_MS = 240000
NPC = "BAU_954_MALETH"


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def comparison(first, second):
    """Exact ordered reward/path comparisons; timing costs and logits are irrelevant."""
    first, second = Path(first), Path(second)
    paths = [sorted((root / "episodes").glob("*/samples.jsonl"), key=lambda p: int(p.parent.name))
             for root in (first, second)]
    if not all(paths):
        raise ValueError("Population comparison needs recorded samples in both runs")
    identities = [[path.parent.name for path in values] for values in paths]
    if identities[0] != identities[1]:
        raise ValueError("Population comparison episode sequences differ")
    differences, samples, examples, trajectory_differences, metadata_differences = 0, 0, [], 0, 0
    for left, right in zip(*paths):
        metadata = [rl.read_json(path.parent / "episode.json") for path in (left, right)]
        identity = ("start", "goal", "population_index", "pair_index", "episode_index")
        changed_identity = [key for key in identity if metadata[0][key] != metadata[1][key]]
        if changed_identity:
            metadata_differences += 1
            if len(examples) < 5:
                examples.append(dict(episode=int(left.parent.name), fields=changed_identity,
                                     first={key: metadata[0][key] for key in changed_identity},
                                     second={key: metadata[1][key] for key in changed_identity}))
        rows = [list(rl.rows(path)) for path in (left, right)]
        samples += len(rows[0])
        different = abs(len(rows[0]) - len(rows[1]))
        for index, (a, b) in enumerate(zip(*rows)):
            fields = {"time_ms", "next_time_ms", "elapsed_ms", "rewards", "terminated", "truncated", "action_indices"}
            fields.update(key for row in (a, b) for key in row if key.startswith("path_"))
            changed = sorted(key for key in fields if a.get(key) != b.get(key))
            if changed:
                different += 1
                if len(examples) < 5:
                    examples.append(dict(episode=int(left.parent.name), decision=index,
                                         fields=changed, first={key: a.get(key) for key in changed},
                                         second={key: b.get(key) for key in changed}))
        trajectory_equal = ((left.parent / "trajectory.txt").read_bytes()
                            == (right.parent / "trajectory.txt").read_bytes())
        trajectory_differences += not trajectory_equal
        differences += different
    return dict(deterministic=differences == 0 and trajectory_differences == 0 and metadata_differences == 0,
                episodes=len(paths[0]), compared_samples=samples, differing_samples=differences,
                differing_trajectories=trajectory_differences, differing_episode_metadata=metadata_differences, examples=examples,
                fields="rewards, all path_*, terminated, truncated, time_ms, next_time_ms, elapsed_ms, action_indices; trajectory.txt")


def record(args, directory, pairs, *, reference=False):
    directory = Path(directory).resolve()
    if (directory / "population.json").exists():
        raise ValueError(f"Population output exists: {directory}")
    directory.mkdir(parents=True, exist_ok=True)
    worker = SimpleNamespace(runner=args.runner, assets=Path(os.environ["GOTHIC_DIR"]),
        save=args.inputs / "save.sav", links=args.inputs / "accepted-links.txt", roads=args.inputs / "places.json",
        cache=args.cache, ini=None, output=directory, physics="cv", seed=SEED, watchdog_seconds=1800)
    pair_file = rl.runner_pairs(directory / "pairs.txt", pairs)
    rl.write_json(directory / "validation-scenarios.json", pairs)
    old_trace = os.environ.pop("OPENGOTHIC_NAV_PASSAGE_TRACE", None)
    if reference:
        os.environ["OPENGOTHIC_NAV_PASSAGE_TRACE"] = "1"
    process = None
    try:
        process = rl.new_runner(worker, "population")
        command = (f"navdemos population {NPC} {pair_file} {LIMIT_MS} {args.weights} "
                   f"{POPULATION} {SEED} sample {len(pairs)} 1 far 0" + (" run" if reference else ""))
        rl.scenario(worker, process, "population", [f"roads load {worker.roads}", f"roads on {NPC}",
                    command, "navdemos wait"], directory)
    finally:
        try:
            if process is not None:
                process.close()
        finally:
            if old_trace is None:
                os.environ.pop("OPENGOTHIC_NAV_PASSAGE_TRACE", None)
            else:
                os.environ["OPENGOTHIC_NAV_PASSAGE_TRACE"] = old_trace
    population = rl.read_json(directory / "population.json")
    if (population["episodes"] != len(pairs) or population["population"] != POPULATION
            or population["seed"] != SEED or population["sampling"] is not True or population["record"] is not True):
        raise ValueError(f"Population settings/count differ: {directory}")
    decisions = passages = passage_calls = 0
    demos = rl.module("nav_demos")
    for index, pair in enumerate(pairs):
        episode_dir = directory / "episodes" / str(index)
        episode = rl.read_json(episode_dir / "episode.json")
        if ((episode["start"], episode["goal"]) != (pair["start"], pair["goal"])
                or episode["episode_index"] != index or episode["pair_index"] != index
                or episode.get("training_valid") is not True or episode["decisions"] <= 0):
            raise ValueError(f"Population route/recording invalid: {episode_dir}")
        sample_rows = list(rl.rows(episode_dir / "samples.jsonl"))
        if len(sample_rows) != episode["decisions"]:
            raise ValueError(f"Population sample/decision count differs: {episode_dir}")
        if demos.episode_version(episode_dir) != episode["demo_version"]:
            raise ValueError(f"NAVDEMO header differs from episode metadata: {episode_dir}")
        offset = 0
        first_time = sample_rows[0]["time_ms"]
        for chunk in demos.iter_chunks(episode_dir, chunk_records=16):
            if offset + len(chunk) > len(sample_rows):
                raise ValueError(f"NAVDEMO has extra decisions: {episode_dir}")
            for time_ms, sample in zip(chunk["time_ms"], sample_rows[offset:offset + len(chunk)]):
                if int(time_ms) != sample["time_ms"] or int(time_ms) - first_time != sample["elapsed_ms"]:
                    raise ValueError(f"NAVDEMO/sample time alignment differs: {episode_dir}/{offset}")
                offset += 1
        if offset != len(sample_rows):
            raise ValueError(f"NAVDEMO/sample decision count differs: {episode_dir}: {offset}/{len(sample_rows)}")
        decisions += len(sample_rows)
        if reference:
            if episode.get("pace") != "run" or episode.get("pace_input") is not True:
                raise ValueError(f"Reference is not v5/run: {episode_dir}")
            trace = list(rl.rows(episode_dir / "roads-passages.jsonl"))
            guidance = [line.split() for line in (episode_dir / "roads-guidance.txt").read_text().splitlines()
                        if line.startswith("d ")]
            if len(trace) != len(sample_rows) or len(guidance) != len(trace):
                raise ValueError(f"Reference passage/guidance count differs: {episode_dir}")
            for decision, (sample, road, passage) in enumerate(zip(sample_rows, guidance, trace)):
                if (passage["decision_index"] != decision or passage["elapsed_ms"] != sample["elapsed_ms"]
                        or int(road[1]) != sample["elapsed_ms"]):
                    raise ValueError(f"Reference decision alignment differs: {episode_dir}/{decision}")
            passages += len(trace)
            passage_calls += sum(len(row["calls"]) for row in trace)
        elif any((episode_dir / name).exists() for name in ("roads-passages.jsonl", "roads-guidance.txt")):
            raise ValueError(f"Parity recording unexpectedly enabled road traces: {episode_dir}")
    if decisions != population["decisions"]:
        raise ValueError(f"Population total decision count differs: {directory}")
    return dict(directory=str(directory), episodes=len(pairs), decisions=decisions, navdemo_records=decisions,
                passage_decisions=passages, passage_calls=passage_calls,
                arrived=sum(rl.read_json(directory / "episodes" / str(i) / "episode.json")["success"]
                            for i in range(len(pairs))))


def provenance(args):
    return dict(engine_revision=args.revision, runner=str(args.runner), runner_sha256=digest(args.runner),
                weights=str(args.weights), weights_sha256=digest(args.weights),
                validation_manifest=str(args.manifest), validation_manifest_sha256=digest(args.manifest),
                base_fingerprint=os.environ["OPENGOTHIC_BASE_FINGERPRINT"],
                seed=SEED, population=POPULATION, limit_ms=LIMIT_MS, action_mode="sample", physics="cv",
                npc=NPC, hero="far", recording_enabled=True)


def finish_reference(directory, metadata):
    rl.write_json(directory / "reference.json", metadata)
    (directory / "README.md").write_text(
        "# S4a population reference\n\n"
        f"Engine `{metadata['engine_revision']}`, v5 weights `{metadata['weights_sha256']}`, "
        f"runner `{metadata['runner_sha256']}`. Fixed seed {SEED}, six population slots, "
        f"60 frozen validation point pairs, sampled actions, CV physics, run pace, {LIMIT_MS} ms limit. "
        "Source paths, hashes and base fingerprint: `reference.json`. This is a recording for replay on the same poses, "
        "not an assertion that repeated world simulations are deterministic.\n\n"
        "- `validation-scenarios.json` preserves the selected validation scenarios; `pairs.txt` holds their FROM TO order.\n"
        "- `episodes/N/episode.json`: N is `episode_index`; `pair_index = N % 60`; `population_index` identifies the reused slot.\n"
        "- `episodes/N/samples.bin.gz`: NAVDEMO header and ordered decisions. Read with `scripts/nav_demos.py`. "
        "Fields: `time_ms`, `position_cm`, `body_yaw_deg`, `body.velocity_cm_s`, `body.flags`, "
        "`previous_keys`, teacher `keys`, `teacher_hint.next_corner_cm`/`teacher_hint.path_cm`; "
        "road input `roads.carrot_direction`, `carrot_distance_cm`, `goal_direction`, `goal_distance_cm`, "
        "`remaining_known_cm`, `beyond_known`; eye rays `eye`; maps `memory_near`/`memory_far`. "
        "All positions and distances are centimetres, Y up; map channel bytes are quantized values.\n"
        "- `samples.jsonl`: row i is the transition after NAVDEMO observation i; `time_ms` matches that record, "
        "`next_time_ms` closes its held action. `action_indices` are executed sampled actions; NAVDEMO `keys` "
        "remain teacher labels. `rewards` holds unweighted components; `path_*`, termination and truncation accompany them.\n"
        "- `roads-guidance.txt`: the ith `d` row belongs to decision i, identified by episode-relative elapsed ms. "
        "Its columns are elapsed_ms, progress_cm, rebuilt, caught_up, beyond_known, start_skipped, "
        "start_fallback + start_departure, carrot_x_cm, carrot_y_cm, carrot_z_cm, remaining_known_cm, projection_rejected. "
        "A following `p K x y z ...` row contains K rebuilt-polyline points.\n"
        "- `roads-passages.jsonl`: one row per decision, `decision_index = i`; `elapsed_ms` matches the reward "
        "row and guidance. `calls` retains every PassageCheck in call order, including an empty list; "
        "each call has `from_cm`, `to_cm`, verdict Free/Closed/Unknown. No passage query is reconstructed.\n"
        "- `trajectory.txt` and `actions.txt`: applied body poses/actions, including the extra final row. "
        "`bootstrap.bin.gz` is the continuing endpoint observation for truncated episodes.\n\n"
        "Within each episode, join NAVDEMO record i, samples row i, guidance d row i and passage decision_index i. "
        "Absolute `time_ms` minus the first NAVDEMO time equals `elapsed_ms`. "
        "`SHA256SUMS` covers every saved file except itself; verify with `sha256sum -c SHA256SUMS`.\n",
        encoding="utf-8")
    with (directory / "SHA256SUMS").open("w", encoding="utf-8") as stream:
        for path in sorted(directory.rglob("*")):
            if path.is_file() and path.name != "SHA256SUMS":
                stream.write(f"{digest(path)}  {path.relative_to(directory).as_posix()}\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=("determinism", "before", "reference"))
    for name in ("runner", "inputs", "cache", "weights", "manifest", "output"):
        parser.add_argument(f"--{name}", type=lambda value: Path(value).resolve(), required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--after", type=lambda value: Path(value).resolve())
    args = parser.parse_args()
    if sys.platform != "linux":
        raise ValueError("Run population recording on Ryzen/Linux through gq")
    args.output.mkdir(parents=True, exist_ok=True)
    pairs = rl.validation_scenarios(args.manifest, "point")
    if len(pairs) != 60 or any(pair["goal_kind"] != "point" for pair in pairs):
        raise ValueError(f"Reference requires exactly 60 validation point routes, got {len(pairs)}")
    meta = provenance(args)
    if args.stage == "determinism":
        first = record(args, args.output / "first", pairs[:12])
        second = record(args, args.output / "second", pairs[:12])
        result = dict(**meta, first=first, second=second,
                      comparison=comparison(args.output / "first", args.output / "second"))
        result["deterministic"] = result["comparison"]["deterministic"]
        result["acceptance"] = ("before/after comparison required" if result["deterministic"] else
                                "population is not deterministic; reward acceptance uses gothic-navreward-contract")
    elif args.stage == "before":
        if args.after is None:
            parser.error("before requires --after DETERMINISM_FIRST_DIRECTORY")
        result = dict(**meta, recording=record(args, args.output / "before", pairs[:12]),
                      comparison=comparison(args.after, args.output / "before"))
    else:
        result = dict(**meta, recording=record(args, args.output, pairs, reference=True), pace="run")
        finish_reference(args.output, result)
    if args.stage != "reference":
        rl.write_json(args.output / f"{args.stage}.json", result)
    print(json.dumps(result), flush=True)
    if args.stage == "before" and not result["comparison"]["deterministic"]:
        raise ValueError("Before/after population reward/path/pose comparison differs")


if __name__ == "__main__":
    main()
