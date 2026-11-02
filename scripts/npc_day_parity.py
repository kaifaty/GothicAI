#!/usr/bin/env python3
"""Compare two NPC day journals (bench `npcday`, common/npcdayjournal.h).

    scripts/npc_day_parity.py RUN_A RUN_B [--out report.md] [--json report.json]
                              [--tolerances scripts/bench/npc-day-parity.tolerances.json] [--max-pos-p95 30 ...]

RUN_A/RUN_B are bench output directories (or their npc-day.csv). Rows are matched by
sample time and NPC key; the hero is left out. The report gives, per metric: the share
of NPC-samples in the same ZS state and the same routine (TA) entry, the straight-line
position error (p50/p95/max, metres), the offset of matched ZS starts (game minutes),
alive/dead, HP and inventory differences, a per-hour divergence curve, the NPCs that
diverge most, and the same metrics split by run B's simulation tier. With tolerances
the exit code is 1 when a check fails; without them the report is informational.

The completeness gates fail a pair whose runs did not both finish (`end_reason`), stop
early or at different times, have samples present in only one run, or end with an NPC
that the other run does not have: metrics over the shared samples alone cannot see that.
"""
from __future__ import annotations

import argparse
import bisect
import csv
import gzip
import json
import math
from pathlib import Path
import sys

# name -> (kind, report label); "min" checks value >= limit, "max" checks value <= limit,
# "flag" checks that the value is true when the tolerance is true (false skips it).
CHECKS = {
    "require_done": ("flag", "both runs finished the journal (end_reason done)"),
    "require_full_length": ("flag", "both runs reach their requested minutes and end at the same time"),
    "max_only_samples": ("max", "NPC-samples present in only one run"),
    "max_missing_end": ("max", "NPCs present at the end of one run and missing in the other"),
    "min_same_zs": ("min", "same ZS state, share of NPC-samples"),
    "min_same_ta": ("min", "same routine (TA) entry, share of NPC-samples"),
    "max_pos_p50": ("max", "position error p50, m"),
    "max_pos_p95": ("max", "position error p95, m"),
    "max_pos_max": ("max", "position error max, m"),
    "min_zs_matched": ("min", "ZS starts matched within the window, share"),
    "max_zs_offset_p95": ("max", "matched ZS start offset p95, game min"),
    "max_alive_diff": ("max", "NPCs alive in one run and dead (or missing) in the other at the end"),
    "max_inventory_diff_end": ("max", "NPCs whose inventory differs (or who are missing) at the end"),
    "min_same_inventory": ("min", "same inventory, share of NPC-samples"),
    "min_same_hp": ("min", "same HP, share of NPC-samples"),
}


def opener(path):
    return gzip.open(path, "rt", newline="") if str(path).endswith(".gz") else open(path, newline="")


def find(run, name):
    run = Path(run)
    if run.is_file():
        run = run.parent
    for candidate in (run / name, run / (name + ".gz")):
        if candidate.exists():
            return candidate
    return None


def load(run):
    samples = find(run, "npc-day.csv")
    if samples is None:
        raise SystemExit(f"{run}: no npc-day.csv")
    rows = {}
    keys = {}
    hero = {}
    hero_keys = set()
    times = set()
    with opener(samples) as stream:
        for row in csv.DictReader(stream):
            t = int(float(row["t_min"]))
            times.add(t)
            if row.get("hero") == "hero":
                hero[t] = (float(row["x"]), float(row["y"]), float(row["z"]))
                hero_keys.add(row["key"])
                continue
            rows[(t, row["key"])] = (
                row["alive"] == "1", int(row["hp"]), row["ta"], row["zs"],
                float(row["x"]), float(row["y"]), float(row["z"]), row["inv_hash"], row["tier"], row["head"])
            keys[row["key"]] = row["symbol"]
    events = {}
    path = find(run, "npc-day-events.csv")
    if path is not None:
        with opener(path) as stream:
            for row in csv.DictReader(stream):
                if row["key"] in hero_keys:
                    continue  # the hero is left out of events too
                events.setdefault(row["kind"], {}).setdefault(row["key"], []).append((float(row["t_min"]), row["value"]))
    header = {}
    path = find(run, "npc-day.json")
    if path is not None:
        try:
            header = json.loads(path.read_text())
        except (OSError, ValueError):
            header = {}
    last = max(times) if times else None
    return {"rows": rows, "keys": keys, "hero": hero, "events": events, "header": header, "path": str(samples.parent),
            "last": last, "end_keys": {key for t, key in rows if t == last}}


def percentile(values, q):
    if not values:
        return None
    values = sorted(values)
    position = (len(values) - 1) * q
    low = math.floor(position)
    high = math.ceil(position)
    if low == high:
        return values[low]
    return values[low] + (values[high] - values[low]) * (position - low)


def share(hits, total):
    return hits / total if total else None


class Tally:
    def __init__(self):
        self.samples = self.same_zs = self.same_ta = self.same_inventory = self.same_hp = 0
        self.alive_mismatch = 0
        self.errors = []

    def add(self, a, b):
        self.samples += 1
        self.same_zs += a[3] == b[3]
        self.same_ta += a[2] == b[2]
        self.same_hp += a[1] == b[1]
        self.same_inventory += a[7] == b[7]
        self.alive_mismatch += a[0] != b[0]
        if a[0] and b[0]:
            self.errors.append(math.dist(a[4:7], b[4:7]) / 100)

    def summary(self):
        return {
            "samples": self.samples,
            "same_zs": share(self.same_zs, self.samples),
            "same_ta": share(self.same_ta, self.samples),
            "same_hp": share(self.same_hp, self.samples),
            "same_inventory": share(self.same_inventory, self.samples),
            "alive_mismatch_samples": self.alive_mismatch,
            "pos_p50": percentile(self.errors, 0.5),
            "pos_p95": percentile(self.errors, 0.95),
            "pos_max": max(self.errors) if self.errors else None,
        }


def match_events(a, b, window):
    """Greedy nearest matching of (time, value) events with the same value, per NPC."""
    offsets, total, matched = [], 0, 0
    for key in sorted(set(a) | set(b)):
        pool = {}
        for t, value in b.get(key, []):
            pool.setdefault(value, []).append(t)
        for times in pool.values():
            times.sort()
        total += len(a.get(key, [])) + len(b.get(key, []))
        for t, value in a.get(key, []):
            times = pool.get(value)
            if not times:
                continue
            index = bisect.bisect_left(times, t)
            best = None
            for candidate in (index - 1, index):
                if 0 <= candidate < len(times) and (best is None or abs(times[candidate] - t) < abs(times[best] - t)):
                    best = candidate
            if best is not None and abs(times[best] - t) <= window:
                offsets.append(abs(times.pop(best) - t))
                matched += 1
    if not total:
        # No events in either run agree completely; None stays for events that never matched.
        return {"events": 0, "matched_pairs": 0, "matched": 1.0, "offset_p50": 0.0, "offset_p95": 0.0, "offset_max": 0.0}
    return {"events": total, "matched_pairs": matched, "matched": share(2 * matched, total),
            "offset_p50": percentile(offsets, 0.5), "offset_p95": percentile(offsets, 0.95),
            "offset_max": max(offsets) if offsets else None}


def near_hero(run, t, position, metres):
    hero = run["hero"].get(t)
    return hero is not None and math.dist(hero, position) < metres * 100


def completeness(run):
    """Whether one run finished its journal: its end reason, last sample and requested minutes."""
    header = run["header"]
    minutes, period = header.get("minutes"), header.get("period_min")
    expected = None
    if isinstance(minutes, (int, float)) and isinstance(period, (int, float)) and period > 0:
        expected = int(minutes // period * period)
    return {"end_reason": header.get("end_reason", "unfinished"), "last_t_min": run["last"],
            "requested_min": minutes, "expected_last_t_min": expected,
            "full_length": expected is not None and run["last"] is not None and run["last"] >= expected}


def end_differences(a, b):
    """The end of each run against the other's end: NPCs missing in one, alive and inventory."""
    missing, alive, inventory = [], [], []
    for key in sorted(a["end_keys"] | b["end_keys"]):
        ra, rb = a["rows"].get((a["last"], key)), b["rows"].get((b["last"], key))
        if ra is None or rb is None:
            missing.append(key)
            alive.append(key)
            inventory.append(key)
            continue
        if ra[0] != rb[0]:
            alive.append(key)
        if ra[7] != rb[7]:
            inventory.append(key)
    return missing, alive, inventory


def compare(a, b, window=60.0, min_hero_distance=0.0):
    common = sorted(set(a["rows"]) & set(b["rows"]))
    excluded = 0
    if min_hero_distance > 0:
        kept = [(t, key) for t, key in common
                if not near_hero(a, t, a["rows"][(t, key)][4:7], min_hero_distance)
                and not near_hero(b, t, b["rows"][(t, key)][4:7], min_hero_distance)]
        excluded = len(common) - len(kept)
        common = kept
    if not common:
        raise SystemExit("no common NPC-samples: different worlds or empty journals")
    total, by_tier, by_hour, per_npc = Tally(), {}, {}, {}
    for t, key in common:
        ra, rb = a["rows"][(t, key)], b["rows"][(t, key)]
        total.add(ra, rb)
        by_tier.setdefault(rb[8], Tally()).add(ra, rb)
        by_hour.setdefault(t // 60, Tally()).add(ra, rb)
        npc = per_npc.setdefault(key, {"zs_diff": 0, "pos_max": 0.0, "first_zs_diff": None, "samples": 0})
        npc["samples"] += 1
        if ra[3] != rb[3]:
            npc["zs_diff"] += 1
            if npc["first_zs_diff"] is None:
                npc["first_zs_diff"] = t
        if ra[0] and rb[0]:
            npc["pos_max"] = max(npc["pos_max"], math.dist(ra[4:7], rb[4:7]) / 100)
    last = max(t for t, _ in common)
    missing_end, alive_diff, inventory_diff = end_differences(a, b)
    # Events count only up to the last common sample: a longer run must not add unmatched ones.
    def kind(run, name, value=None):
        return {key: [(t, value or v) for t, v in events if t <= last]
                for key, events in run["events"].get(name, {}).items()}
    deaths = match_events(kind(a, "death", "death"), kind(b, "death", "death"), window)
    zs = match_events(kind(a, "zs_start"), kind(b, "zs_start"), window)
    diverged = [key for key, npc in per_npc.items() if npc["zs_diff"] or npc["pos_max"] > 5]
    first = [npc["first_zs_diff"] for npc in per_npc.values() if npc["first_zs_diff"] is not None]
    return {
        "a": {"path": a["path"], "header": a["header"]},
        "b": {"path": b["path"], "header": b["header"]},
        "common_samples": len(common),
        "min_hero_distance_m": min_hero_distance,
        "excluded_near_hero": excluded,
        "only_a": len(set(a["rows"]) - set(b["rows"])),
        "only_b": len(set(b["rows"]) - set(a["rows"])),
        "complete_a": completeness(a),
        "complete_b": completeness(b),
        "missing_end": missing_end,
        "npcs": len({key for _, key in common}),
        "last_t_min": last,
        "total": total.summary(),
        "by_tier": {tier: tally.summary() for tier, tally in sorted(by_tier.items())},
        "by_hour": {int(hour): tally.summary() for hour, tally in sorted(by_hour.items())},
        "zs_starts": zs,
        "deaths": deaths,
        "deaths_a": sum(len(v) for v in kind(a, "death").values()),
        "deaths_b": sum(len(v) for v in kind(b, "death").values()),
        "alive_diff_end": alive_diff,
        "inventory_diff_end": inventory_diff,
        "diverged_npcs": len(diverged),
        "first_zs_divergence_min": min(first) if first else None,
        "worst_npcs": sorted(({"key": key, **npc} for key, npc in per_npc.items() if npc["pos_max"] > 0 or npc["zs_diff"]),
                             key=lambda item: (-item["pos_max"], -item["zs_diff"], item["key"]))[:15],
        "zs_window_min": window,
    }


def check_values(result):
    total = result["total"]
    ca, cb = result["complete_a"], result["complete_b"]
    return {
        "require_done": ca["end_reason"] == "done" and cb["end_reason"] == "done",
        "require_full_length": ca["full_length"] and cb["full_length"] and ca["last_t_min"] == cb["last_t_min"],
        "max_only_samples": result["only_a"] + result["only_b"],
        "max_missing_end": len(result["missing_end"]),
        "min_same_zs": total["same_zs"], "min_same_ta": total["same_ta"],
        "max_pos_p50": total["pos_p50"], "max_pos_p95": total["pos_p95"], "max_pos_max": total["pos_max"],
        "min_zs_matched": result["zs_starts"]["matched"], "max_zs_offset_p95": result["zs_starts"]["offset_p95"],
        "max_alive_diff": len(result["alive_diff_end"]), "min_same_inventory": total["same_inventory"],
        "max_inventory_diff_end": len(result["inventory_diff_end"]),
        "min_same_hp": total["same_hp"],
    }


def evaluate(result, tolerances):
    values = check_values(result)
    checks = []
    for name, limit in tolerances.items():
        if limit is None:
            continue
        kind, label = CHECKS[name]
        value = values[name]
        if kind == "flag":
            if not limit:
                continue
            passed = bool(value)
        else:
            passed = value is not None and (value >= limit if kind == "min" else value <= limit)
        checks.append({"name": name, "label": label, "value": value, "limit": limit, "kind": kind, "pass": passed})
    return checks


def fmt(value, digits=3):
    if value is None:
        return "—"
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value)


def pct(value):
    return "—" if value is None else f"{value * 100:.2f}%"


def markdown(result, checks, label_a, label_b):
    total = result["total"]
    lines = [f"# NPC day parity: {label_a} vs {label_b}", ""]
    for side, label in (("a", label_a), ("b", label_b)):
        header = result[side]["header"]
        lines.append(f"- {label}: `{result[side]['path']}` — tier `{header.get('tier_mode', '?')}`, "
                     f"seed {header.get('seed', '?')}, isolate {header.get('isolate', '?')}, "
                     f"{header.get('minutes', '?')} min every {header.get('period_min', '?')}, "
                     f"wall {fmt(header.get('wall_s'), 0)} s, end `{header.get('end_reason', 'unfinished')}`")
    lines += [f"- NPC-samples compared: {result['common_samples']} ({result['npcs']} NPCs, up to t={result['last_t_min']} min); "
              f"only in {label_a}: {result['only_a']}, only in {label_b}: {result['only_b']}"]
    for side, label in (("complete_a", label_a), ("complete_b", label_b)):
        item = result[side]
        lines.append(f"- {label} ends at t={fmt(item['last_t_min'])} of {fmt(item['requested_min'])} requested minutes, "
                     f"`{item['end_reason']}`")
    if result["min_hero_distance_m"]:
        lines.append(f"- left out: {result['excluded_near_hero']} NPC-samples within {fmt(result['min_hero_distance_m'], 0)} m "
                     "of the hero in either run (ZS-start and death events are not filtered)")
    lines.append("")
    lines += ["| Metric | Value |", "|---|---|",
              f"| same ZS state | {pct(total['same_zs'])} |",
              f"| same routine (TA) entry | {pct(total['same_ta'])} |",
              f"| same HP | {pct(total['same_hp'])} |",
              f"| same inventory | {pct(total['same_inventory'])} |",
              f"| position error p50 / p95 / max, m | {fmt(total['pos_p50'], 2)} / {fmt(total['pos_p95'], 2)} / {fmt(total['pos_max'], 1)} |",
              f"| ZS starts matched (±{fmt(result['zs_window_min'], 0)} min) | {pct(result['zs_starts']['matched'])} of {result['zs_starts']['events']} |",
              f"| matched ZS start offset p50 / p95 / max, game min | {fmt(result['zs_starts']['offset_p50'], 2)} / {fmt(result['zs_starts']['offset_p95'], 2)} / {fmt(result['zs_starts']['offset_max'], 1)} |",
              f"| deaths {label_a} / {label_b}; matched pairs | {result['deaths_a']} / {result['deaths_b']}; {result['deaths']['matched_pairs']} |",
              f"| NPCs missing in one run at the end | {len(result['missing_end'])} |",
              f"| alive differs at the end (missing counts) | {len(result['alive_diff_end'])} |",
              f"| inventory differs at the end (missing counts) | {len(result['inventory_diff_end'])} |",
              f"| NPCs with any ZS difference or >5 m apart | {result['diverged_npcs']} of {result['npcs']} |",
              f"| first ZS difference, game min | {fmt(result['first_zs_divergence_min'])} |", ""]
    if checks:
        lines += ["## Tolerances", "", "| Check | Value | Limit | |", "|---|---|---|---|"]
        for check in checks:
            if check["kind"] == "flag":
                value, limit = ("yes" if check["value"] else "no"), "required"
            else:
                sign = "≥" if check["kind"] == "min" else "≤"
                value, limit = fmt(check["value"], 4), f"{sign} {fmt(check['limit'], 4)}"
            lines.append(f"| {check['label']} | {value} | {limit} | {'pass' if check['pass'] else '**FAIL**'} |")
        lines.append("")
    lines += [f"## By tier in {label_b}", "", "| Tier | Samples | Same ZS | Same TA | Position p50 / p95 / max, m |",
              "|---|---|---|---|---|"]
    for tier, item in result["by_tier"].items():
        lines.append(f"| {tier} | {item['samples']} | {pct(item['same_zs'])} | {pct(item['same_ta'])} | "
                     f"{fmt(item['pos_p50'], 2)} / {fmt(item['pos_p95'], 2)} / {fmt(item['pos_max'], 1)} |")
    lines += ["", "## By game hour since the start", "", "| Hour | Same ZS | Same TA | Position p95, m | Max, m |",
              "|---|---|---|---|---|"]
    for hour, item in result["by_hour"].items():
        lines.append(f"| {hour} | {pct(item['same_zs'])} | {pct(item['same_ta'])} | {fmt(item['pos_p95'], 2)} | "
                     f"{fmt(item['pos_max'], 1)} |")
    lines += ["", "## NPCs that diverge most", "", "| NPC | Max distance, m | ZS differs, samples | First ZS difference, min |",
              "|---|---|---|---|"]
    for npc in result["worst_npcs"]:
        lines.append(f"| `{npc['key']}` | {fmt(npc['pos_max'], 2)} | {npc['zs_diff']} of {npc['samples']} | "
                     f"{fmt(npc['first_zs_diff'])} |")
    if not result["worst_npcs"]:
        lines.append("| none | 0 | 0 | — |")
    for name, keys in (("Missing in one run at the end", result["missing_end"]),
                       ("Alive differs at the end", result["alive_diff_end"]),
                       ("Inventory differs at the end", result["inventory_diff_end"])):
        if keys:
            shown = ", ".join(f"`{key}`" for key in keys[:20])
            lines += ["", f"{name}: {shown}{' …' if len(keys) > 20 else ''}"]
    return "\n".join(lines) + "\n"


def tolerances_from(args):
    tolerances = {}
    if args.tolerances:
        data = json.loads(Path(args.tolerances).read_text())
        data = data.get("tolerances", data)
        unknown = set(data) - set(CHECKS)
        if unknown:
            raise SystemExit(f"{args.tolerances}: unknown tolerance {sorted(unknown)}")
        tolerances.update(data)
    for name in CHECKS:
        value = getattr(args, name)
        if value is not None:
            tolerances[name] = value
    return tolerances


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("run_a", metavar="RUN_A")
    parser.add_argument("run_b", metavar="RUN_B")
    parser.add_argument("--label-a", default="A")
    parser.add_argument("--label-b", default="B")
    parser.add_argument("--out", help="write the Markdown report here (default: stdout)")
    parser.add_argument("--json", help="write the numbers as JSON here")
    parser.add_argument("--window", type=float, default=60.0, help="ZS start / death matching window, game minutes")
    parser.add_argument("--min-hero-distance", type=float, default=0.0,
                        help="leave out NPC-samples closer to the hero than this many metres in either run")
    parser.add_argument("--tolerances", help="JSON file of tolerances (keys as the options below, without --)")
    for name, (kind, label) in CHECKS.items():
        if kind == "flag":
            parser.add_argument("--" + name.replace("_", "-"), dest=name, type=lambda v: v.lower() in ("1", "true", "yes"),
                                help=f"true or false: {label}")
        else:
            parser.add_argument("--" + name.replace("_", "-"), dest=name, type=float, help=f"{kind} {label}")
    args = parser.parse_args(argv)
    tolerances = tolerances_from(args)
    result = compare(load(args.run_a), load(args.run_b), args.window, args.min_hero_distance)
    checks = evaluate(result, tolerances)
    report = markdown(result, checks, args.label_a, args.label_b)
    if args.out:
        Path(args.out).write_text(report)
    else:
        sys.stdout.write(report)
    if args.json:
        Path(args.json).write_text(json.dumps({"result": result, "checks": checks}, indent=1, ensure_ascii=False))
    failed = [check["name"] for check in checks if not check["pass"]]
    if failed:
        print("npc_day_parity: FAIL " + ", ".join(failed), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
