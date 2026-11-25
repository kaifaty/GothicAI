#!/usr/bin/env python3
"""Summarize per-world-tick NPC CPU profiles and a Society cohort release."""

import argparse
from collections import defaultdict
import csv
from itertools import product
import json
from pathlib import Path
import re

from perf_report import describe, read_profile

TICK_TIMERS = ("npc_full_tick_ns", "npc_simplified_tick_ns")
ANIMATION_WORKER = tuple(f"npc_anim_{tier}_{policy}_{view}_ns"
                         for tier, policy, view in product(("full", "simplified"),
                                                           ("player", "normal", "far", "far2"), ("in", "out")))
ANIMATION_TAIL = ("melee_shadow_ns", "melee_contacts_ns", "logical_body_sync_ns")
PROFILE_FIELDS = {
    "frame", "dt", "overflow", "world_logic_ns", "world_logic_calls", "animation_npc_pass_ns", "npc_perception_ns",
    *TICK_TIMERS, *ANIMATION_TAIL,
}
COHORT_COUNTS = ("selected", "held", "admitted", "released", "skipped", "gesture_repasses",
                 "unique_held", "unique_released", "checked", "blocked_stage", "blocked_lock",
                 "blocked_near", "blocked_camera")
SOCIETY_FIELDS = {
    "tick_ms", *COHORT_COUNTS[:8], "advance_ns", "eligibility_ns", "sample_ns",
}
TRANSITION = re.compile(r"Society group routine (held|released): \S+ actor=(\d+) .*?tick=(\d+)")
MIN_HELD = 16
METRICS = (
    ("npc_tick", "NPC tick wall"),
    ("full_tick", "Full NPC tick wall"),
    ("simplified_tick", "Simplified NPC tick wall"),
    ("animation_wall", "NPC animation pass wall"),
    ("animation_cpu_path", "NPC animation pass, tail and late catch-up wall"),
    ("animation_catchup", "NPC animation catch-up after camera update wall"),
    ("animation_worker", "NPC animation update worker sum"),
    ("animation_worker_out", "NPC animation update worker sum outside frustum"),
    ("animation_sync", "Forced animation sync elapsed (overlapping)"),
    ("animation_tail", "NPC animation tail wall"),
    ("perception", "NPC perception wall"),
    ("society", "Society measured total"),
    ("society_advance", "Society advance"),
    ("society_bridge", "Society native binding"),
    ("society_wake", "Society native return preparation (overlapping)"),
    ("society_eligibility", "Society eligibility (inside native NPC tick)"),
    ("society_sample", "Society sample"),
    ("npc_wall", "Measured NPC path wall subtotal"),
    ("world_logic", "World logic wall"),
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_society(path):
    if not path.exists():
        return None
    with path.open(newline="") as stream:
        reader = csv.DictReader(stream)
        missing = SOCIETY_FIELDS - set(reader.fieldnames or ())
        require(not missing, f"{path}: missing columns {', '.join(sorted(missing))}")
        result = {}
        for line, plain in enumerate(reader, 2):
            row = {name: int(value) for name, value in plain.items()}
            tick = row["tick_ms"]
            require(all(value >= 0 for value in row.values()), f"{path}:{line}: negative count or timing")
            require(tick not in result, f"{path}:{line}: duplicate tick {tick}")
            result[tick] = row
        require(bool(result), f"{path}: no Society samples")
        return result


def read_marks(path):
    if not path.exists():
        return {}
    markers = {}
    with path.open() as stream:
        for line in stream:
            row = json.loads(line)
            if row["kind"] != "mark":
                continue
            if "t_game_ms" in row:
                require(row["text"] not in markers, f"{path}: duplicate mark {row['text']}")
                markers[row["text"]] = row["t_game_ms"]
    return markers


def read_transitions(path):
    if not path.exists():
        return []
    transitions = []
    with path.open() as stream:
        for line in stream:
            match = TRANSITION.search(line)
            if match:
                operation, actor, tick = match.groups()
                transitions.append((int(tick), operation, actor))
    require(all(a[0] <= b[0] for a, b in zip(transitions, transitions[1:])), f"{path}: transition clock regressed")
    return transitions


def load_run(directory, args):
    profile = directory / "frameprofile.csv"
    rows, _, _, _ = read_profile(profile)
    require(bool(rows), f"{profile}: no frame profile records")
    missing = PROFILE_FIELDS - set(rows[0])
    require(not missing, f"{profile}: missing columns {', '.join(sorted(missing))}")
    require(all(row["overflow"] == 0 for row in rows), f"{profile}: recorder overflow")
    society = read_society(directory / args.society_file)
    worker_available = set(ANIMATION_WORKER).issubset(rows[0]) and any(
        any(row.get(f"{column[:-3]}_n", 0) for column in ANIMATION_WORKER) for row in rows
    )
    result = {}
    for row in rows:
        if row["dt"] == 0 or row["world_logic_calls"] == 0:
            continue
        tick = row["frame"]
        require(row["world_logic_calls"] == 1, f"{profile}: more than one world tick at {tick} ms")
        require(tick not in result, f"{profile}: clock does not identify a single step at {tick} ms")
        timed = society.get(tick) if society is not None else None
        complete_society = society is None or timed is not None
        advance = timed["advance_ns"] if timed is not None else 0
        eligibility = timed["eligibility_ns"] if timed is not None else 0
        sample = timed["sample_ns"] if timed is not None else 0
        bridge_outside_tick = timed.get("bridge_outside_tick_ns", 0) if timed is not None else 0
        bridge = timed.get("bridge_ns", eligibility + sample) if timed is not None else 0
        tick_wall = sum(row[column] for column in TICK_TIMERS)
        animation_wall = row["animation_npc_pass_ns"]
        animation_tail = sum(row[column] for column in ANIMATION_TAIL)
        catchup = row.get("npc_animation_catchup_ns", 0)
        perception = row["npc_perception_ns"]
        worker_sum = sum(row[column] for column in ANIMATION_WORKER) if worker_available else None
        worker_out = (sum(row[column] for column in ANIMATION_WORKER if column.endswith("_out_ns"))
                      if worker_available else None)
        result[tick] = {
            "complete_society": complete_society,
            **{name: timed.get(name, 0) if timed is not None else 0
               for name in ("wake_count", "sleeping")},
            **{name: timed.get(name, 0) if timed is not None else 0 for name in COHORT_COUNTS},
            "npc_full": row.get("npc_full", 0), "npc_simplified": row.get("npc_simplified", 0),
            "npc_ticked": row.get("npc_simplified_ticked", 0), "npc_deferred": row.get("npc_simplified_deferred", 0),
            "npc_anim_deferred": row.get("npc_anim_deferred", 0), "npc_anim_catchup": row.get("npc_anim_catchup", 0),
            "npc_anim_worker_updates": sum(row.get(f"{column[:-3]}_n", 0) for column in ANIMATION_WORKER),
            "society_npc_tick_skipped": row.get("society_npc_tick_skipped", 0),
            "society_anim_deferred": row.get("society_anim_deferred", 0),
            "society_anim_catchup": row.get("society_anim_catchup", 0),
            "cost": {
                "npc_tick": tick_wall, "full_tick": row["npc_full_tick_ns"],
                "simplified_tick": row["npc_simplified_tick_ns"], "animation_wall": animation_wall,
                "animation_cpu_path": animation_wall + animation_tail + catchup, "animation_catchup": catchup,
                "animation_worker": worker_sum, "animation_worker_out": worker_out,
                "animation_sync": row.get("forced_sync_animation_ns"),
                "animation_tail": animation_tail, "perception": perception,
                "society": advance + bridge, "society_advance": advance,
                "society_bridge": bridge,
                "society_wake": timed.get("wake_ns", 0) if timed is not None else 0,
                "society_eligibility": eligibility, "society_sample": sample,
                # Eligibility is timed inside the native NPC routine; the worker sum overlaps the animation pass.
                "npc_wall": tick_wall + animation_wall + animation_tail + catchup + perception + advance + sample + bridge_outside_tick,
                "world_logic": row["world_logic_ns"],
            },
        }
    require(result and tuple(result) == tuple(sorted(result)), f"{profile}: ticks absent or out of order")
    return result, read_marks(directory / "events.jsonl"), read_transitions(directory / "game.log")


def window(run, phase):
    rows, marks, _ = run
    label, begin, end = phase

    def boundary(value):
        if value.isdecimal():
            return int(value)
        require(value in marks, f"no world tick for mark {value!r}")
        return marks[value]

    left, right = boundary(begin), boundary(end)
    require(min(rows) <= left < right <= max(rows), f"{label}: phase {left}–{right} lies outside the profile")
    selected = {tick: row for tick, row in rows.items() if left < tick <= right}
    require(len(selected) >= 2, f"{label}: need at least two world ticks")
    require(all(row["complete_society"] for row in selected.values()), f"{label}: missing Society rows for profiled ticks")
    return left, right, selected


def actor_counts(transitions, left, right, phase):
    present = set()
    unique_held, unique_released, unique_changed = set(), set(), set()
    seen = dict(admitted=defaultdict(int), released=defaultdict(int))
    prior = [(tick, operation, actor) for tick, operation, actor in transitions if tick <= left]
    inside = [(tick, operation, actor) for tick, operation, actor in transitions if left < tick <= right]

    def apply_transition(tick, operation, actor):
        if operation == "held":
            require(actor not in present, f"duplicate hold for actor {actor} at tick {tick}")
            present.add(actor)
        else:
            require(actor in present, f"unmatched release for actor {actor} at tick {tick}")
            present.remove(actor)

    for change in prior:
        apply_transition(*change)
    unique_held.update(present)
    for tick, operation, actor in inside:
        apply_transition(tick, operation, actor)
        unique_changed.add(actor)
        if operation == "held":
            unique_held.add(actor)
            seen["admitted"][tick] += 1
        else:
            unique_released.add(actor)
            seen["released"][tick] += 1
    holds = [row["held"] for row in phase.values()]
    changed_total = sum(row["admitted"] + row["released"] for row in phase.values())
    require(not (any(holds) or changed_total) or transitions,
            "Society counts are nonzero but the run lacks per-actor transition logs")
    for tick, row in phase.items():
        for key in seen:
            require(row[key] == seen[key][tick], f"{key} counter disagrees with actor transitions at tick {tick}")
    last = phase[max(phase)]
    require(last["held"] == len(present), "active holds disagree with per-actor transitions at the end of the phase")
    require(last["unique_held"] == len({actor for _, operation, actor in prior + inside if operation == "held"})
            and last["unique_released"] == len({actor for _, operation, actor in prior + inside if operation == "released"}),
            "cumulative distinct actors disagree with per-actor transitions")
    return {
        "selected_max": max(row["selected"] for row in phase.values()),
        "held_max": max(holds), "held_p50": describe(holds)["p50"], "unique_held": len(unique_held),
        "sleeping_p50": describe([row["sleeping"] for row in phase.values()])["p50"],
        "wake_sum": sum(row["wake_count"] for row in phase.values()),
        "wake_max": max(row["wake_count"] for row in phase.values()),
        "unique_released": len(unique_released), "unique_transferred": len(unique_changed),
        "admitted_max": max(row["admitted"] for row in phase.values()),
        "released_max": max(row["released"] for row in phase.values()),
        "released_ticks": sum(row["released"] != 0 for row in phase.values()),
        "transferred_max": max(row["admitted"] + row["released"] for row in phase.values()),
        "skipped_sum": sum(row["skipped"] for row in phase.values()),
        "gesture_repasses_sum": sum(row["gesture_repasses"] for row in phase.values()),
        "eligibility_counts": {key: sum(row[key] for row in phase.values())
                               for key in ("checked", "blocked_stage", "blocked_lock", "blocked_near", "blocked_camera")},
        "npc_full_max": max(row["npc_full"] for row in phase.values()),
        "npc_simplified_max": max(row["npc_simplified"] for row in phase.values()),
        "npc_deferred_p50": describe([row["npc_deferred"] for row in phase.values()])["p50"],
        "npc_animation_deferred_p50": describe([row["npc_anim_deferred"] for row in phase.values()])["p50"],
        "npc_animation_catchup_max": max(row["npc_anim_catchup"] for row in phase.values()),
        "npc_animation_worker_updates_sum": sum(row["npc_anim_worker_updates"] for row in phase.values()),
        "society_npc_tick_skipped_sum": sum(row["society_npc_tick_skipped"] for row in phase.values()),
        "society_npc_tick_skipped_p50": describe([row["society_npc_tick_skipped"] for row in phase.values()])["p50"],
        "society_animation_deferred_sum": sum(row["society_anim_deferred"] for row in phase.values()),
        "society_animation_deferred_p50": describe([row["society_anim_deferred"] for row in phase.values()])["p50"],
        "society_animation_catchup_sum": sum(row["society_anim_catchup"] for row in phase.values()),
        "society_animation_catchup_max": max(row["society_anim_catchup"] for row in phase.values()),
    }


def describe_window(rows):
    stats = {}
    for name, _ in METRICS:
        samples = {tick: row["cost"][name] for tick, row in rows.items() if row["cost"][name] is not None}
        if not samples:
            stats[name] = None
            continue
        stats[name] = describe([ns / 1e6 for ns in samples.values()])
        stats[name]["peak_tick_ms"] = max(samples, key=samples.__getitem__)
        stats[name]["samples"] = len(samples)
    return {"ticks": len(rows), "first_tick_ms": min(rows), "last_tick_ms": max(rows), "elapsed_ms": stats}


def compare(runs, args):
    phase_windows = {}
    phases = {}
    for phase in args.phase:
        label, _, _ = phase
        require(label not in phases, f"duplicate phase {label}")
        aligned = {name: window(run, phase) for name, run in runs.items()}
        offsets = {name: tuple(tick - left for tick in rows) for name, (left, _, rows) in aligned.items()}
        require(len(set(offsets.values())) == 1, f"{label}: world-tick windows are not aligned across variants")
        phase_windows[label] = aligned
        phases[label] = {}
        for name, (left, right, rows) in aligned.items():
            summary = describe_window(rows)
            summary.update(actor_counts(runs[name][2], left, right, rows))
            phases[label][name] = summary
    pilot = args.release_run or next(reversed(runs))
    require(pilot in runs, f"unknown release run {pilot}")
    held_max = max(variants[pilot]["held_max"] for variants in phases.values())
    report = {"phases": phases, "release": None, "cohort": {"run": pilot, "held_max": held_max, "minimum_held": MIN_HELD}}
    if args.release_phase is None:
        return report
    require(args.release_phase in phase_windows, "--release-phase must name one of the specified phases")
    aligned = phase_windows[args.release_phase]
    left, _, rows = aligned[pilot]
    releases = [tick for tick, row in rows.items() if row["released"] > 0]
    if not releases:
        report["release"] = {"phase": args.release_phase, "pilot": pilot, "runs": {}}
        return report
    cohort_peak = max(releases, key=lambda tick: rows[tick]["released"])
    ticks = tuple(runs[pilot][0])
    position = ticks.index(cohort_peak)
    indices = ticks[max(position - args.radius, 0):position + args.radius + 1]
    require(len(indices) == 2 * args.radius + 1, "recording begins or ends inside the requested release window")
    offsets = tuple(tick - left for tick in indices)
    result = {
        "phase": args.release_phase, "pilot": pilot, "tick_ms": cohort_peak,
        "released_at_peak": rows[cohort_peak]["released"], "released_ticks": releases,
        "release_step_offset_ms": cohort_peak - left, "radius_ticks": args.radius, "runs": {},
    }
    for name, (left, _, _) in aligned.items():
        all_rows = runs[name][0]
        require(all(offset + left in all_rows and all_rows[offset + left]["complete_society"] for offset in offsets),
                f"{name}: incomplete profile for the matched release window")
        group = {offset + left: all_rows[offset + left] for offset in offsets}
        tick = left + result["release_step_offset_ms"]
        summary = describe_window(group)
        summary["anchor_tick_ms"] = tick
        summary["at_anchor_ms"] = {key: value / 1e6 if value is not None else None for key, value in group[tick]["cost"].items()}
        summary["held_before"] = group[min(group)]["held"]
        summary["held_after"] = group[max(group)]["held"]
        result["runs"][name] = summary
    report["release"] = result
    return report


def format_ms(value):
    return "—" if value is None else f"{value:.3f}"


def print_report(report):
    cohort = report["cohort"]
    if cohort["held_max"] < cohort["minimum_held"]:
        print(f"Below cohort target in {cohort['run']}: peak {cohort['held_max']} held, target {cohort['minimum_held']}.")
    for phase, variants in report["phases"].items():
        print(f"\n### {phase}\n")
        print("| Elapsed ms per world tick | " + " | ".join(f"{name} P50 / P95" for name in variants) + " |")
        print("|---|" + "---:|" * len(variants))
        for key, label in METRICS:
            parts = []
            for variant in variants.values():
                data = variant["elapsed_ms"][key]
                parts.append(" / ".join(format_ms(data[p] if data else None) for p in ("p50", "p95")))
            print("| " + label + " | " + " | ".join(parts) + " |")
        for name, data in variants.items():
            print(f"\n- {name}, {data['ticks']} ticks, held P50/max {data['held_p50']:.1f}/{data['held_max']}, "
                  f"unique held/released/transferred "
                  f"{data['unique_held']}/{data['unique_released']}/{data['unique_transferred']}, "
                  f"peak transfers {data['transferred_max']} (admissions {data['admitted_max']}, releases {data['released_max']}), "
                  f"releases on {data['released_ticks']} ticks, "
                  f"skipped loops {data['skipped_sum']}. NPC Full/Simplified max "
                  f"{data['npc_full_max']}/{data['npc_simplified_max']}.")
            if data["sleeping_p50"] or data["wake_sum"]:
                print(f"  Persistently held NPCs P50 {data['sleeping_p50']:.1f}, "
                      f"{data['wake_sum']} wake events, peak {data['wake_max']} in one tick.")
            if data["npc_animation_worker_updates_sum"]:
                print(f"  NPC animation worker updates {data['npc_animation_worker_updates_sum']}.")
            if data["society_npc_tick_skipped_sum"]:
                print(f"  Native NPC ticks skipped before dispatch {data['society_npc_tick_skipped_sum']}, "
                      f"P50 {data['society_npc_tick_skipped_p50']} per world tick.")
            blocked = data["eligibility_counts"]
            if data["society_animation_deferred_sum"] or data["society_animation_catchup_sum"]:
                print(f"  Society animation deferrals {data['society_animation_deferred_sum']}, "
                      f"P50 {data['society_animation_deferred_p50']:.1f} NPCs per tick; "
                      f"restored poses {data['society_animation_catchup_sum']}, "
                      f"peak {data['society_animation_catchup_max']} in one tick.")
            if blocked["checked"]:
                print(f"  Candidate checks {blocked['checked']}; blocked by routine stage {blocked['blocked_stage']}, "
                      f"lock {blocked['blocked_lock']}, hero {blocked['blocked_near']}, camera {blocked['blocked_camera']}.")
    release = report["release"]
    if release is None:
        return
    print(f"\n### Cohort release, {release['phase']}\n")
    if not release["runs"]:
        print(f"No cohort release in {release['pilot']} during this phase.")
        return
    print(f"{release['pilot']} released {release['released_at_peak']} NPCs at {release['tick_ms']} ms. "
          f"Each variant uses the same ±{release['radius_ticks']}-tick window relative to its phase start.\n")
    variants = release["runs"]
    print("| Elapsed ms | " + " | ".join(f"{name} at release / peak" for name in variants) + " |")
    print("|---|" + "---:|" * len(variants))
    for key, label in METRICS:
        values = []
        for data in variants.values():
            metric = data["elapsed_ms"][key]
            values.append(f"{format_ms(data['at_anchor_ms'][key])} / {format_ms(metric['max'] if metric else None)}")
        print("| " + label + " | " + " | ".join(values) + " |")


def parse_phase(value):
    parts = value.split(":")
    if len(parts) != 3 or any(not field for field in parts):
        raise argparse.ArgumentTypeError("expected NAME:BEGIN_MARK_OR_TICK:END_MARK_OR_TICK")
    return tuple(parts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runs", nargs="+", metavar="LABEL=OUTPUT_DIR")
    parser.add_argument("--phase", action="append", required=True, type=parse_phase)
    parser.add_argument("--society-file", default="society.csv", help="filename of optional Society per-tick CSV")
    parser.add_argument("--release-phase", help="phase in which to compare the peak cohort release")
    parser.add_argument("--release-run", help="run with the cohort enabled, defaults to the last run")
    parser.add_argument("--radius", type=int, default=12, help="world ticks on either side of the peak release")
    parser.add_argument("--json", type=Path, help="also save machine-readable statistics")
    args = parser.parse_args()
    try:
        require(args.radius >= 1, "--radius must be positive")
        runs = {}
        for arg in args.runs:
            name, sep, directory = arg.partition("=")
            require(sep and name and directory and name not in runs, f"invalid or duplicate run {arg!r}")
            runs[name] = load_run(Path(directory), args)
        report = compare(runs, args)
        if args.json is not None:
            args.json.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
        print_report(report)
    except (KeyError, OSError, TypeError, ValueError) as error:
        raise SystemExit(f"ERROR: {error}") from None


if __name__ == "__main__":
    main()
