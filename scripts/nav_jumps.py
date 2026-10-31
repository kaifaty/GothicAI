#!/usr/bin/env python3
"""Bounded native CV transition collection. Run heavy commands in gq batch.

Each probe batch resets the saved world; its completed outcomes can be resumed.
Unverified candidates are never loaded as off-mesh links. No learner is run.
"""
from __future__ import annotations

import argparse
from collections import Counter
import importlib.util
import json
import math
from pathlib import Path
import sys

REPO = Path(__file__).resolve().parent.parent
TYPES = {1: "jump", 2: "climb", 3: "drop"}
# Three original failed locations remain in the pilot alongside spatially spread sites.
PILOT_CASES = (2708710634, 2071474113, 3225746972)


def module(name):
    spec = importlib.util.spec_from_file_location(name, REPO / "scripts" / f"{name}.py")
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def links(path):
    result = []
    for number, line in enumerate(Path(path).read_text().splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        fields = line.split()
        if len(fields) != 8:
            raise ValueError(f"Expected 8 fields: {path}:{number}")
        coordinates = tuple(float(v) for v in fields[:6])
        kind, identifier = int(fields[6]), int(fields[7])
        if not all(math.isfinite(v) for v in coordinates) or kind not in TYPES or identifier <= 0:
            raise ValueError(f"Invalid link: {path}:{number}")
        result.append(dict(start=coordinates[:3], end=coordinates[3:], type=kind, id=identifier))
    if len({r["id"] for r in result}) != len(result):
        raise ValueError(f"Duplicate link id: {path}")
    return result


def write_links(path, values):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text("".join(" ".join([*(f"{v:.9g}" for v in (*r["start"], *r["end"])),
                                                    str(r["type"]), str(r["id"])]) + "\n" for r in values))


def json_rows(path):
    return [json.loads(line) for line in Path(path).read_text().splitlines() if line.strip()]


def scenario(args, name, commands):
    path = args.output / "scenarios" / f"{name}.bench"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join([f"seed {args.seed}", "set time 10 0", "navdemos physics cv",
                                *commands, "label done", ""]))
    return path


def summary(outcomes):
    counts = {}
    for kind in TYPES.values():
        selected = [r for r in outcomes if r["type"] == kind]
        counts[kind] = dict(attempts=len(selected), accepted=sum(bool(r["accepted"]) for r in selected),
                            rejected_reasons=dict(Counter(r["reason"] for r in selected if not r["accepted"])))
    return counts


def novel_links(values, excluded=()):
    """Do not count repeated native attempts at the same directed transition."""
    kept = []
    for row in values:
        if any(row["type"] == other["type"] and math.dist(row["start"], other["start"]) < .75
               and math.dist(row["end"], other["end"]) < .75 for other in (*excluded, *kept)):
            continue
        kept.append(row)
    return kept


def pilot_links(values, per_type=16):
    """A deterministic spatial sample of new candidates, without success filtering."""
    selected = {}
    for kind in TYPES:
        group = sorted((row for row in values if row["type"] == kind),
                       key=lambda row: (*row["start"], *row["end"], row["id"]))
        if len(group) < per_type:
            raise ValueError(f"Pilot needs {per_type} candidates of type {TYPES[kind]}")
        fixed = [row for row in group if row["id"] in PILOT_CASES]
        remaining = [row for row in group if row["id"] not in PILOT_CASES]
        count = per_type - len(fixed)
        spread = [remaining[round(i * (len(remaining) - 1) / max(1, count - 1))] for i in range(count)]
        selected[kind] = fixed + spread
    return [selected[kind][index] for index in range(per_type) for kind in TYPES]


def verify_batch(directory, expected):
    outcomes = json_rows(directory / "transitions.jsonl")
    if Counter(r["link_id"] for r in outcomes) != Counter(r["id"] for r in expected):
        raise RuntimeError(f"Incomplete/duplicate outcomes: {directory}")
    accepted = links(directory / "accepted-links.txt")
    if {r["id"] for r in accepted} != {r["link_id"] for r in outcomes if r["accepted"]}:
        raise RuntimeError(f"Accepted links disagree with outcomes: {directory}")
    kinds = {row["id"]: row["type"] for row in expected}
    if (any(row["type"] != TYPES[kinds[row["link_id"]]] for row in outcomes)
            or any(row["type"] != kinds[row["id"]] for row in accepted)):
        raise RuntimeError(f"Runner changed a candidate link type: {directory}")
    for row in outcomes:
        # Preparation failures are ordinary candidate outcomes. They may have no
        # CV frames at all: only successful transitions need complete CV evidence.
        if row["accepted"] and (row.get("frames", 0) == 0 or not row.get("expect_cv")
                or row.get("forced_cv_frames") != row["frames"]
                or row.get("preparation_frames", 0) == 0
                or row.get("preparation_cv_frames") != row["preparation_frames"]
                or (row.get("candidate_preparation") and row.get("preparation_qualified_ms", 0) < 400)):
            row.update(probe_accepted=True, accepted=False, probe_reason=row["reason"],
                       reason="accepted_without_cv_support_evidence")
    accepted_ids = {row["link_id"] for row in outcomes if row["accepted"]}
    accepted = [row for row in accepted if row["id"] in accepted_ids]
    return outcomes, accepted


def probe(args):
    if sys.platform != "linux":
        raise RuntimeError("Run collection on Ryzen in gq batch")
    args.output.mkdir(parents=True, exist_ok=True)
    args.cache.mkdir(parents=True, exist_ok=True)
    expected = links(args.links)
    if args.mode == "pilot":
        expected = pilot_links(expected)
        write_links(args.output / "pilot-links.txt", expected)
    excluded = links(args.exclude_links) if args.exclude_links else []
    if args.mode == "recheck" and len(expected) != 99:
        raise ValueError(f"Expected exactly 99 historical links, got {len(expected)}")
    prepare = scenario(args, "prepare", [f"navmesh build {args.cache}"])
    runner = module("navmesh_measure").Runner(args, prepare)
    all_outcomes, all_accepted = [], []
    try:
        runner.scenario(prepare, args.output / "prepare", args.seed, timeout=720)
        for first in range(0, len(expected), args.batch_size):
            group = expected[first:first + args.batch_size]
            directory = args.output / "batches" / f"{first:05d}"
            source = args.output / "inputs" / f"{first:05d}.txt"
            if (directory / "probe.json").exists():
                if links(source) != group:
                    raise RuntimeError(f"Resume input changed: {source}")
            else:
                if (directory / "transitions.jsonl").exists():
                    raise RuntimeError(f"Partial native attempt needs inspection before retry: {directory}")
                write_links(source, group)
                command = "probe-links" if args.mode == "recheck" else "probe-candidates"
                task = scenario(args, f"probe-{first:05d}",
                                [f"navmesh {command} {args.npc} {source} {args.attempt_ms}"])
                runner.scenario(task, directory, args.seed, timeout=args.watchdog_seconds)
            outcomes, accepted = verify_batch(directory, group)
            all_outcomes.extend(outcomes)
            all_accepted.extend(accepted)
            print(json.dumps(dict(first=first, attempts=len(all_outcomes), accepted=len(all_accepted),
                                  types=summary(all_outcomes))), flush=True)
        unique = novel_links(all_accepted, excluded) if args.mode in ("discover", "pilot") else all_accepted
        write_links(args.output / "accepted-links-cv.txt", unique)
        (args.output / "transitions.jsonl").write_text("".join(json.dumps(r) + "\n" for r in all_outcomes))
        (args.output / "counts.json").write_text(json.dumps(summary(all_outcomes), indent=2) + "\n")
        if args.mode == "discover" and len(unique) < 30:
            raise RuntimeError(f"STOP: probe budget exhausted; only {len(unique)} new links, need diagnosis")
    finally:
        runner.close()


def candidates(args):
    if sys.platform != "linux":
        raise RuntimeError("Run discovery on Ryzen in gq batch")
    args.output.mkdir(parents=True, exist_ok=True)
    args.cache.mkdir(parents=True, exist_ok=True)
    destination = args.output / "candidates.txt"
    prepare = scenario(args, "candidates", [f"navmesh build {args.cache}", "navmesh coverage",
                                           f"navmesh shortcuts {destination} {args.site_budget}"])
    runner = module("navmesh_measure").Runner(args, prepare)
    try:
        runner.scenario(prepare, args.output / "geometry", args.seed, timeout=720)
        selected = links(destination)
        print(json.dumps(dict(site_budget=args.site_budget, scheduled_attempts=len(selected),
                              candidate_types=dict(Counter(TYPES[r["type"]] for r in selected)))), flush=True)
    finally:
        runner.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("candidates", "recheck", "discover", "pilot"))
    for name in ("runner", "assets", "save", "cache", "output"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    parser.add_argument("--links", type=Path)
    parser.add_argument("--exclude-links", type=Path)
    parser.add_argument("--ini", type=Path)
    parser.add_argument("--npc", default="BAU_954_MALETH")
    parser.add_argument("--seed", type=int, default=19)
    parser.add_argument("--site-budget", type=int, default=1200)
    parser.add_argument("--batch-size", type=int, default=16)
    parser.add_argument("--attempt-ms", type=int, default=6000)
    parser.add_argument("--watchdog-seconds", type=int, default=600)
    args = parser.parse_args()
    for name in ("runner", "assets", "save", "cache", "output", "links", "exclude_links", "ini"):
        value = getattr(args, name)
        if value is not None and not value.is_absolute():
            parser.error(f"--{name} must be absolute")
    if not 1 <= args.batch_size <= 32 or not 1 <= args.site_budget <= 10000 or args.attempt_ms <= 0:
        parser.error("Invalid bounded probe budget")
    if args.mode != "candidates" and args.links is None:
        parser.error("--links is required for probes")
    candidates(args) if args.mode == "candidates" else probe(args)


if __name__ == "__main__":
    main()
