#!/usr/bin/env python3
"""Summarize the four placed-item census/probe runs and enforce their fixed gate."""

import argparse
import collections
import json
import math
from pathlib import Path


REASONS = ("quest", "hangs", "static", "furniture", "neighbour", "ok")
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


def summarize(runs):
    worlds = []
    outliers = []
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
        shifts = []
        for zen_id, probe in measured.items():
            shift = probe["max_shift_cm"]
            if not isinstance(shift, (int, float)) or not math.isfinite(shift) or shift < 0:
                raise ValueError(f"{world}/{zen_id}: invalid maximum displacement")
            if probe["elapsed_ms"] < 5000 or probe["native_steps"] < 300:
                raise ValueError(f"{world}/{zen_id}: probe shorter than five native seconds")
            shifts.append(shift)
            if shift > 10:
                source = admitted[zen_id]
                support_index = source.get("support_item_index")
                outliers.append({**source, "max_shift_cm": shift,
                                 "final_shift_cm": probe["final_shift_cm"],
                                 "npc_contact": probe["npc_contact"],
                                 "support_item_zen_id": census[support_index]["zen_id"]
                                 if isinstance(support_index, int) and 0 <= support_index < len(census) else None})
        log_recovered = (run / "game.log").read_text().count("item recovered")
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
    args = parser.parse_args()
    result = verify_saves(args.runs) if args.save_chain else summarize(args.runs)
    text = json.dumps(result, indent=2, ensure_ascii=False, allow_nan=False) + "\n"
    if args.output:
        args.output.write_text(text)
    print(text, end="")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
