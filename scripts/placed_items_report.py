#!/usr/bin/env python3
"""Summarize the four placed-item census/probe runs and enforce their fixed gate."""

import argparse
import collections
import json
import math
import re
from pathlib import Path


REASONS = ("quest", "hangs", "static", "furniture", "neighbour", "rooted", "slope", "irregular", "on_excluded", "ok")
IDENTITY = ("instance", "visual", "zen_id", "placed", "owner", "owner_guild", "flags", "count")


def read_rows(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def verify_saves(runs):
    if len(runs) != 3:
        raise ValueError("save verification needs the original run and two cold reload runs")
    cycles = []
    for before, after in zip(runs, runs[1:]):
        saved = read_rows(before / "placed-items-saved.jsonl")
        loaded = read_rows(after / "placed-items-initial.jsonl")
        if not saved or len(saved) != len(loaded):
            raise ValueError(f"{after}: saved/loaded item counts differ")
        maximum = 0.0
        linked = moved = 0
        census = {row["zen_id"]: row for row in read_rows(before / "placed-items-census.jsonl")}
        for old, new in zip(saved, loaded):
            if old["index"] != new["index"] or any(old[key] != new[key] for key in IDENTITY):
                raise ValueError(f"{after}/{old['index']}: identity, ownership, flags or count changed")
            if len(old["transform"]) != 16 or len(new["transform"]) != 16 or not all(
                    math.isfinite(value) for row in (old, new) for value in row["transform"]):
                raise ValueError(f"{after}/{old['index']}: invalid observed transform")
            distance = math.dist(old["position_cm"], new["position_cm"])
            matrix_error = max(abs(a - b) for a, b in zip(old["transform"], new["transform"]))
            maximum = max(maximum, distance)
            if not math.isfinite(distance) or distance > .01 or matrix_error > .01:
                raise ValueError(f"{after}/{old['index']}: saved pose error exceeds .01 cm")
            if new["placed"]:
                linked += 1
                if new["dynamic"] and not new["sleeping"]:
                    raise ValueError(f"{after}/{old['index']}: linked item entered awake")
                source = census[new["zen_id"]]
                moved += math.dist(old["position_cm"], source["origin_cm"]) > .01
                if not source["enabled"] and new["dynamic"]:
                    raise ValueError(f"{after}/{old['index']}: excluded item has a native body")
        initial = [row for row in read_rows(after / "events.jsonl")
                   if row.get("kind") == "placed_items_snapshot" and row.get("label") == "initial"]
        if len(initial) != 1 or initial[0]["initial_dt"] != 0:
            raise ValueError(f"{after}: missing snapshot before the first native step")
        cycles.append({"items": len(saved), "linked": linked, "moved_linked": moved,
                       "maximum_position_error_cm": maximum})
    return {"save_cycles": cycles, "pass": all(row["moved_linked"] > 0 for row in cycles)}


def slope_distribution(rows):
    bins = {f"{low}–{low + 5}": 0 for low in range(0, 90, 5)}
    missing = non_static = 0
    for row in rows:
        angle = row.get("support_slope_deg")
        if angle is None or not math.isfinite(angle):
            missing += 1
            non_static += row["support_category"] not in (2, 4)
        else:
            low = min(85, int(max(0, angle) // 5) * 5)
            bins[f"{low}–{low + 5}"] += 1
    return {"bins_deg": bins, "missing": missing, "non_static_support": non_static,
            "static_ray_miss": missing - non_static}


def summarize(runs, previous_runs=()):
    worlds = []
    outliers = []
    previous_outliers = {}
    for run in previous_runs:
        for row in read_rows(run / "placed-items-probe.jsonl"):
            if row["max_shift_cm"] > 10:
                previous_outliers[(row["world"], row["zen_id"])] = row
    total = under_five = recovered = 0
    maximum = 0.0
    for run in runs:
        census = read_rows(run / "placed-items-census.jsonl")
        probes = read_rows(run / "placed-items-probe.jsonl")
        events = read_rows(run / "events.jsonl")
        summaries = [row for row in events if row.get("kind") == "placed_items_census"]
        if len(summaries) != 1:
            raise ValueError(f"{run}: expected one census summary, found {len(summaries)}")
        summary = summaries[0]
        world = summary["world"]
        counts = collections.Counter(row["disposition"] for row in census)
        if sum(counts.values()) != summary["total"] or set(counts) - set(REASONS):
            raise ValueError(f"{world}: invalid census counts")
        admitted = {row["zen_id"]: row for row in census if row["enabled"]}
        measured = {row["zen_id"]: row for row in probes}
        if len(measured) != len(probes) or measured.keys() != admitted.keys():
            raise ValueError(f"{world}: admitted/probed identities differ ({len(admitted)}/{len(probes)})")
        recovery_lines = [line for line in (run / "game.log").read_text().splitlines()
                          if "item recovered" in line]
        recovery_records = []
        recovery_shifts = {}
        for line in recovery_lines:
            identity = re.search(r"zen_id=(\d+)", line)
            zen_id = int(identity[1]) if identity else None
            source = next((row for row in census if row["zen_id"] == zen_id), {})
            position = re.search(r"\bfrom=([^ ]+) rest=", line)
            failed = list(map(float, position[1].split(","))) if position else None
            distance = math.dist(failed, source["origin_cm"]) if failed and source else None
            if distance is not None and math.isfinite(distance):
                recovery_shifts[zen_id] = max(recovery_shifts.get(zen_id, 0), distance)
            else:
                failed = distance = None
            recovery_records.append({"zen_id": zen_id, "visual": source.get("visual"),
                                     "origin_cm": source.get("origin_cm"), "failed_position_cm": failed,
                                     "shift_before_recovery_cm": distance, "message": line})
        shifts = []
        for zen_id, probe in measured.items():
            shift = probe["max_shift_cm"]
            if not isinstance(shift, (int, float)) or not math.isfinite(shift) or shift < 0:
                raise ValueError(f"{world}/{zen_id}: invalid maximum displacement")
            if probe["elapsed_ms"] < 5000 or probe["native_steps"] < 300:
                raise ValueError(f"{world}/{zen_id}: probe shorter than five native seconds")
            # Recovery can reset the pose inside a native tick, before the
            # regular post-tick sample. Its logged pre-reset pose is also evidence.
            shift = max(shift, recovery_shifts.get(zen_id, 0))
            shifts.append(shift)
            if shift > 10:
                source = admitted[zen_id]
                support_index = source.get("support_item_index")
                outliers.append({**source, "max_shift_cm": shift,
                                 "final_shift_cm": probe["final_shift_cm"],
                                 "npc_contact": probe["npc_contact"],
                                 "support_item_zen_id": census[support_index]["zen_id"]
                                 if isinstance(support_index, int) and 0 <= support_index < len(census) else None})
        log_recovered = len(recovery_lines)
        recovered += log_recovered
        within = sum(shift < 5 for shift in shifts)
        high = max(shifts, default=0)
        total += len(shifts)
        under_five += within
        maximum = max(maximum, high)
        worlds.append({"world": world, "total": len(census), "reasons": dict(counts),
                       "eligible": len(admitted), "classification_ms": summary["classification_ms"],
                       "gaps_cm": [summary["gaps_0_1"], summary["gaps_1_2"], summary["gaps_2_4"]],
                       "under_5cm": within, "max_shift_cm": high, "recovered": log_recovered,
                       "sampled_under_5cm": sum(row["max_shift_cm"] < 5 for row in probes),
                       "recoveries": recovery_records,
                       "admitted_slopes": slope_distribution(admitted.values()),
                       "previous_outlier_slopes": slope_distribution(
                           row for row in census if (world, row["zen_id"]) in previous_outliers),
                       "previous_outlier_reasons": dict(collections.Counter(
                           row["disposition"] for row in census if (world, row["zen_id"]) in previous_outliers)),
                       "densest_center_cm": summary["densest_center_cm"],
                       "densest_eligible_within_20m": summary["densest_eligible_within_20m"]})
    if len({row["world"] for row in worlds}) != len(worlds):
        raise ValueError("repeated world in census inputs")
    return {"worlds": worlds, "total": total, "under_5cm": under_five,
            "under_5cm_percent": 100 * under_five / total if total else 0,
            "max_shift_cm": maximum, "recovered": recovered, "outliers": outliers,
            "pass": total > 0 and under_five >= .95 * total and maximum <= 100 and recovered == 0}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runs", nargs="+", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--save-chain", action="store_true", help="verify two successive cold reloads")
    parser.add_argument("--previous-runs", nargs="*", type=Path, default=[],
                        help="previous probe runs; report new facet slopes of their >10 cm outliers")
    args = parser.parse_args()
    result = verify_saves(args.runs) if args.save_chain else summarize(args.runs, args.previous_runs)
    text = json.dumps(result, indent=2, ensure_ascii=False, allow_nan=False) + "\n"
    if args.output:
        args.output.write_text(text)
    print(text, end="")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
