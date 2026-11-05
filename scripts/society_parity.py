#!/usr/bin/env python3
"""Routine parity of the society stand's core with the engine's own day (docs/society-sim.md).

    scripts/society_parity.py --world society-world.json --core CORE_DIR --engine ENGINE_DIR
                              [--out report.md] [--json report.json] [--check]

CORE_DIR holds `society-marks.csv` and `society-day.journal` of `gothic-society-day` on the export WORLD; ENGINE_DIR holds
`npc-day.csv`, `npc-day-events.csv` and `npc-day.json` of the `npc-day-parity` bench on the same world
(`day-headless-full`). Marks are matched by NPC key and minute since the start; only people count
(`person` in the export, not the hero).

For every person the engine's day is cut into episodes of one routine entry (its `ta` events). An episode's
transition window runs from the entry's start until the engine's NPC first stands within 10 m of the entry's
point; when it never does, the window stays open for the whole episode. The report gives:

- the share of person-marks outside the windows where the core's ZS_* equals the engine's;
- the share of those marks where the core's position is within 10 m of the engine's, without the states that
  move by themselves (SELF_MOVING below: their place is reported on its own line);
- the same two shares inside the windows;
- travel time on transitions: for an episode whose window closes, the time from the entry's start until the
  NPC comes within 10 m of the point, for the engine and the core alike from their 5-minute positions (the
  first mark inside 10 m and the one before it, the crossing interpolated linearly). The place at the entry's
  start is the engine's from its `ta` event and the core's from its journal: the point it stands at, or on the
  way linearly by time between the walk's start and end points. The error is core minus engine;
- the worst NPCs and states with the reason; for information, the state of the NPCs that are not people.

With --check the exit code is 1 when a threshold (THRESHOLDS) fails.
"""

from __future__ import annotations

import argparse
import bisect
import collections
import csv
import json
import math
import struct
import sys
from pathlib import Path

PLACE_M = 10.0
THRESHOLDS = {
    "missing_marks": 0,  # person-marks of the day (the engine's minutes and period) absent from either run
    "state_outside": 0.95,  # share of person-marks outside the windows with the same ZS_*
    "place_outside": 0.90,  # share within 10 m outside the windows, without SELF_MOVING states
    "travel_median_abs_min": 2.0,  # median |core - engine| travel time on transitions, game minutes
}
# Routine states that move the NPC by themselves in the engine: their place is not the entry's point.
# ZS_*_FP states walk from free point to free point (AI_GotoNextFP), the smith's states move between the
# smithy's mobsi, ZS_PRACTICE_SWORD steps around; their marks count for the state but not for the place.
SELF_MOVING = {
    "ZS_PICK_FP": "picks from free point to free point (AI_GotoNextFP)",
    "ZS_RAKE_FP": "rakes from free point to free point (AI_GotoNextFP)",
    "ZS_SWEEP_FP": "sweeps from free point to free point (AI_GotoNextFP)",
    "ZS_PRAY_INNOS_FP": "goes between prayer free points (AI_GotoNextFP)",
    "ZS_MM_RTN_ROAM": "roams between FP_ROAM points",
}


def percentile(values, q):
    if not values:
        return None
    values = sorted(values)
    position = (len(values) - 1) * q
    low, high = math.floor(position), math.ceil(position)
    if values[low] == values[high]:
        return values[low]
    return values[low] + (values[high] - values[low]) * (position - low)


def finite(x):
    return x if x is not None and math.isfinite(x) else None


def share(hits, total):
    return hits / total if total else None


def load_world(path):
    world = json.loads(Path(path).read_text())
    points = [tuple(p["pos"]) for p in world["waypoints"]]
    names = {}
    for p in world["waypoints"]:
        names.setdefault(p["name"], tuple(p["pos"]))
    people = {}
    for n in world["npcs"]:
        if not n["person"]:
            continue
        people[n["key"]] = {
            "routine": n["routine"],
            "start": points[n["start_waypoint"]],
            "start_aistate": n.get("start_aistate"),
        }
    return world, points, names, people


def load_engine(directory):
    directory = Path(directory)
    rows = collections.defaultdict(dict)
    with open(directory / "npc-day.csv", newline="") as stream:
        for r in csv.DictReader(stream):
            if r["hero"] == "hero":
                continue
            rows[r["key"]][int(float(r["t_min"]))] = (
                r["ta"],
                r["zs"],
                (float(r["x"]), float(r["y"]), float(r["z"])),
            )
    changes = collections.defaultdict(list)
    with open(directory / "npc-day-events.csv", newline="") as stream:
        for r in csv.DictReader(stream):
            if r["kind"] == "ta":
                changes[r["key"]].append(
                    (float(r["t_min"]), r["value"], (float(r["x"]), float(r["y"]), float(r["z"])))
                )
    header = json.loads((directory / "npc-day.json").read_text())
    return rows, changes, header


def load_core(directory, names):
    rows = collections.defaultdict(dict)
    with open(Path(directory) / "society-marks.csv", newline="") as stream:
        reader = csv.DictReader(stream)
        has_pos = "x" in reader.fieldnames
        for r in reader:
            if has_pos:
                pos = (float(r["x"]), float(r["y"]), float(r["z"]))
            else:  # marks v1: the point's position by name (a repeated name takes its first point)
                pos = names.get(r["point"])
            rows[r["key"]][int(r["t_min"])] = (r["routine"], r["state"], r["moving"] == "1", pos)
    return rows


# The core's journal (common/society/journal.h): its header and the 24-byte records.
JOURNAL_HEADER = struct.Struct("<8sIIQQqIII")
JOURNAL_RECORD = struct.Struct("<IIBBHIII")
NO_POINT = 0xFFFFFFFF
TRAVEL_STARTED, ARRIVED, ACTIVITY_STARTED, TELEPORTED = 2, 3, 4, 5


def load_journal(directory, world):
    """The core's start time and, by NPC key, its events that place it: (time s, kind, a, b, c)."""
    path = Path(directory) / "society-day.journal"
    data = path.read_bytes()
    magic, version, size, _, _, start, npcs, points, states = JOURNAL_HEADER.unpack_from(data)
    if magic != b"GSOCJRNL" or version not in (1, 2) or size != JOURNAL_RECORD.size:
        raise SystemExit(f"{path}: not a society journal v1/v2")
    if npcs != len(world["npcs"]) or points != len(world["waypoints"]):
        raise SystemExit(f"{path}: the journal is of another world ({npcs} NPCs, {points} points)")
    at = JOURNAL_HEADER.size
    for _ in range(states):
        at += 2 + struct.unpack_from("<H", data, at)[0]
    if (len(data) - at) % JOURNAL_RECORD.size:
        raise SystemExit(f"{path}: a cut record")
    keys = [n["key"] for n in world["npcs"]]
    events = collections.defaultdict(list)
    for t, npc, kind, _, _, a, b, c in JOURNAL_RECORD.iter_unpack(data[at:]):
        if kind in (TRAVEL_STARTED, ARRIVED, ACTIVITY_STARTED, TELEPORTED):
            events[keys[npc]].append((t, kind, a, b, c))
    return start, events


def core_place(events, t, points):
    """Where the core's NPC is at game second t by its journal: the point it stands at after its last arrival,
    teleport or activity start with a point; on the way, linearly by time between the walk's two points."""
    place, walk = None, None
    for time, kind, a, b, c in events:
        if time > t:
            break
        if kind == TRAVEL_STARTED:
            walk = (time, c, points[a], points[b])
        elif kind == ARRIVED or kind == TELEPORTED or (kind == ACTIVITY_STARTED and b != NO_POINT):
            place, walk = points[a if kind == ARRIVED else b], None
    if walk is None:
        return place
    t0, t1, p0, p1 = walk
    f = min(1.0, (t - t0) / (t1 - t0)) if t1 > t0 else 1.0
    return tuple(x + (y - x) * f for x, y in zip(p0, p1, strict=True))


def entry_index(ta):
    return None if ta in ("-", "") else int(ta.split()[0])


class Episode:
    __slots__ = ("begin", "entry", "origin", "marks", "arrival_mark", "target", "state")


def episodes(person, engine_rows, engine_changes, times):
    """The engine's routine episodes of a person: begin, entry index, position at the begin, marks."""
    routine = person["routine"]
    if not routine:
        return []
    first = engine_rows.get(times[0])
    if first is None:
        return []  # no start mark: its marks count as missing
    starts = [(0.0, entry_index(first[0]), first[2])]
    for t, value, pos in engine_changes:
        starts.append((t, entry_index(value), pos))
    result = []
    begins = [s[0] for s in starts]
    for t in times:
        row = engine_rows.get(t)
        if row is None:
            continue
        # The latest change at or before the mark (a change logged a moment before its mark belongs to it).
        k = bisect.bisect_right(begins, t + 0.01) - 1
        if not result or result[-1].begin != starts[k][0]:
            e = Episode()
            e.begin, e.entry, e.origin = starts[k]
            e.marks = []
            e.arrival_mark = None
            entry = routine[e.entry] if e.entry is not None and e.entry < len(routine) else None
            e.state = entry["state"] if entry else None
            e.target = None if entry is None or entry["waypoint"] is None else entry["waypoint"]
            result.append(e)
        result[-1].marks.append(t)
    return result


def crossing(samples, target):
    """First time a sequence of (t, pos) comes within PLACE_M of `target`, linearly; None if never."""
    previous = None
    for t, pos in samples:
        if pos is None:
            continue
        d = math.dist(pos, target) / 100.0
        if d <= PLACE_M:
            if previous is None:
                return t
            tp, dp = previous
            return tp + (t - tp) * (dp - PLACE_M) / (dp - d) if dp > d else t
        previous = (t, d)
    return None


def compare(world_path, core_dir, engine_dir):
    world, points, names, people = load_world(world_path)
    engine_rows, engine_changes, engine_header = load_engine(engine_dir)
    core_rows = load_core(core_dir, names)
    core_start_s, core_events = load_journal(core_dir, world)
    # Every mark of the requested day, so that a cut journal or CSV counts as missing marks, not as a shorter day.
    if "minutes" not in engine_header or "period_min" not in engine_header:
        raise SystemExit(f"{engine_dir}: npc-day.json has no minutes and period_min")
    times = list(range(0, int(engine_header["minutes"]) + 1, int(engine_header["period_min"])))

    tally = collections.Counter()
    per_npc = collections.defaultdict(collections.Counter)
    npc_pairs = collections.defaultdict(collections.Counter)
    npc_dist = collections.defaultdict(list)
    per_state = collections.defaultdict(collections.Counter)
    self_moving = collections.defaultdict(collections.Counter)
    open_windows = collections.defaultdict(collections.Counter)
    travel = []
    missing = 0
    state_pairs = collections.Counter()

    for key, person in sorted(people.items()):
        erows, crows = engine_rows.get(key), core_rows.get(key)
        if erows is None or crows is None:
            missing += 1
            tally["missing_marks"] += len(times)
            continue
        eps = episodes(person, erows, engine_changes.get(key, []), times)
        window = {}
        state_of = {}
        for e in eps:
            for t in e.marks:
                state_of[t] = e.state
            if e.target is None:
                continue
            target = points[e.target]
            inside = [t for t in e.marks if math.dist(erows[t][2], target) / 100.0 <= PLACE_M]
            if inside and inside[0] == e.marks[0] and math.dist(e.origin, target) / 100.0 <= PLACE_M:
                continue  # already there at the change: no transition
            arrival = inside[0] if inside else None
            for t in e.marks:
                if arrival is None or t < arrival:
                    window[t] = "open" if arrival is None else "walk"
            if arrival is None:
                open_windows[key][e.state] += len(e.marks)
                continue
            # Travel time: from the entry's start until within 10 m, both from their 5-minute positions.
            # The core's place at the entry's start, from its journal: it may stand or be on the way.
            core_start = core_place(core_events.get(key, []), core_start_s + e.begin * 60, points)
            engine_samples = [(e.begin, e.origin)] + [(t, erows[t][2]) for t in e.marks]
            t_engine = crossing(engine_samples, target)
            later = [t for t in times if t > e.begin]
            core_samples = [(e.begin, core_start)] + [(t, crows[t][3]) for t in later if t in crows]
            t_core = crossing(core_samples, target)
            travel.append(
                {
                    "key": key,
                    "begin": round(e.begin, 2),
                    "state": e.state,
                    "point": world["waypoints"][e.target]["name"],
                    "engine_min": round(t_engine - e.begin, 2),
                    "core_min": None if t_core is None else round(t_core - e.begin, 2),
                }
            )

        for t in times:
            er, cr = erows.get(t), crows.get(t)
            if er is None or cr is None:
                tally["missing_marks"] += 1
                continue
            where = window.get(t, "out")
            # The entry's state; without a routine the state the engine runs (its start_aistate).
            state = state_of.get(t) or er[1]
            same_state = er[1] == cr[1]
            dist = math.dist(er[2], cr[3]) / 100.0 if cr[3] is not None else float("inf")
            near = dist <= PLACE_M
            scope = "out" if where == "out" else "win"
            tally[scope + "_marks"] += 1
            tally[scope + "_same_state"] += same_state
            if where == "open":
                tally["open_marks"] += 1
                tally["open_same_state"] += same_state
                tally["open_near"] += near
            moves = state in SELF_MOVING
            if scope == "out":
                tally["out_all_near"] += near
            if scope == "out" and moves:
                self_moving[state]["marks"] += 1
                self_moving[state]["near"] += near
                self_moving[state]["same_state"] += same_state
            else:
                tally[scope + "_place_marks"] += 1
                tally[scope + "_near"] += near
            if scope != "out":
                continue
            c = per_npc[key]
            c["marks"] += 1
            c["state_diff"] += not same_state
            if not moves:
                c["place_marks"] += 1
                c["far"] += not near
                if not near:
                    npc_dist[key].append(dist)
            if not same_state:
                npc_pairs[key][(er[1], cr[1])] += 1
                state_pairs[(er[1], cr[1])] += 1
            s = per_state[state]
            s["marks"] += 1
            s["state_diff"] += not same_state
            if not moves:
                s["place_marks"] += 1
                s["far"] += not near

    # Not people (monsters and the like, without the hero): the state only, for information.
    others = collections.Counter()
    for n in world["npcs"]:
        key = n["key"]
        if n["person"] or key not in engine_rows or key not in core_rows:
            continue
        for t, er in engine_rows[key].items():
            cr = core_rows[key].get(t)
            if cr is None or er[1] in ("ZS_DEAD", "-"):
                continue
            others["marks"] += 1
            others["same_state"] += er[1] == cr[1]

    errors = [r["core_min"] - r["engine_min"] for r in travel if r["core_min"] is not None]
    # A transition the core never finished counts as an unbounded error, not as a missing one.
    abs_errors = [abs(x) for x in errors] + [math.inf] * (len(travel) - len(errors))
    result = {
        "world": str(world_path),
        "core": str(core_dir),
        "engine": str(engine_dir),
        "engine_header": {
            k: engine_header.get(k)
            for k in (
                "world",
                "tier_mode",
                "seed",
                "minutes",
                "period_min",
                "start_day",
                "start_hour",
                "start_minute",
                "end_reason",
            )
        },
        "people": len(people),
        "people_missing": missing,
        "missing_marks": tally["missing_marks"],
        "marks": len(times),
        "place_m": PLACE_M,
        "thresholds": THRESHOLDS,
        "self_moving_states": SELF_MOVING,
        "outside": {
            "marks": tally["out_marks"],
            "same_state": share(tally["out_same_state"], tally["out_marks"]),
            "place_marks": tally["out_place_marks"],
            "near": share(tally["out_near"], tally["out_place_marks"]),
            "near_with_self_moving": share(tally["out_all_near"], tally["out_marks"]),
        },
        "windows": {
            "marks": tally["win_marks"],
            "open_marks": tally["open_marks"],
            "same_state": share(tally["win_same_state"], tally["win_marks"]),
            "place_marks": tally["win_place_marks"],
            "near": share(tally["win_near"], tally["win_place_marks"]),
            "open_same_state": share(tally["open_same_state"], tally["open_marks"]),
            "open_near": share(tally["open_near"], tally["open_marks"]),
        },
        "self_moving": {
            s: {
                "marks": c["marks"],
                "same_state": share(c["same_state"], c["marks"]),
                "near": share(c["near"], c["marks"]),
                "why": SELF_MOVING.get(s, ""),
            }
            for s, c in sorted(self_moving.items())
        },
        "travel": {
            "transitions": len(travel),
            "core_missing": len(travel) - len(errors),
            "median_abs_min": finite(percentile(abs_errors, 0.5)),
            "p90_abs_min": finite(percentile(abs_errors, 0.9)),
            "median_signed_min": percentile(errors, 0.5),
            "engine_median_min": percentile([r["engine_min"] for r in travel], 0.5),
        },
        "open_windows": {
            k: dict(v) for k, v in sorted(open_windows.items(), key=lambda x: -sum(x[1].values()))
        },
        "state_pairs": [[a, b, n] for (a, b), n in state_pairs.most_common(15)],
        "not_people": {"marks": others["marks"], "same_state": share(others["same_state"], others["marks"])},
    }
    worst = sorted(per_npc.items(), key=lambda x: -(x[1]["state_diff"] + x[1]["far"]))
    result["worst_npcs"] = [
        {
            "key": k,
            "marks": c["marks"],
            "state_diff": c["state_diff"],
            "far": c["far"],
            "pairs": [[a, b, n] for (a, b), n in npc_pairs[k].most_common(3)],
            "far_median_m": percentile(npc_dist[k], 0.5),
        }
        for k, c in worst[:25]
        if c["state_diff"] + c["far"] > 0
    ]
    result["states"] = [
        {
            "state": s,
            "marks": c["marks"],
            "state_diff": c["state_diff"],
            "place_marks": c["place_marks"],
            "far": c["far"],
        }
        for s, c in sorted(per_state.items(), key=lambda x: -(x[1]["state_diff"] + x[1]["far"]))
    ]
    worst_travel = sorted(
        (r for r in travel),
        key=lambda r: -abs((r["core_min"] if r["core_min"] is not None else 1e9) - r["engine_min"]),
    )
    result["worst_travel"] = worst_travel[:15]
    result["travel_records"] = travel
    checks = {
        "missing_marks": (result["missing_marks"], "<="),
        "state_outside": (result["outside"]["same_state"], ">="),
        "place_outside": (result["outside"]["near"], ">="),
        "travel_median_abs_min": (result["travel"]["median_abs_min"], "<="),
    }
    result["checks"] = {}
    for name, (value, op) in checks.items():
        limit = THRESHOLDS[name]
        ok = value is not None and (value >= limit if op == ">=" else value <= limit)
        result["checks"][name] = {"value": value, "limit": limit, "op": op, "pass": ok}
    result["pass"] = all(c["pass"] for c in result["checks"].values())
    return result


def pct(x):
    return "—" if x is None else f"{100 * x:.2f}%"


def num(x, digits=2):
    return "—" if x is None else f"{x:.{digits}f}"


def markdown(r):
    o, w, t = r["outside"], r["windows"], r["travel"]
    lines = [
        "# Society core vs engine day: routine parity",
        "",
        f"- world: `{r['world']}`",
        f"- core: `{r['core']}`; engine: `{r['engine']}` (tier `{r['engine_header']['tier_mode']}`, "
        f"seed {r['engine_header']['seed']}, {r['engine_header']['minutes']} min every {r['engine_header']['period_min']})",
        f"- people: {r['people']} (missing in a run: {r['people_missing']}), marks: {r['marks']}; place radius {r['place_m']:.0f} m",
        "",
        "| Check | Value | Limit | |",
        "|---|---|---|---|",
    ]
    labels = {
        "missing_marks": "person-marks of the day missing in a run",
        "state_outside": "same ZS_* outside the windows",
        "place_outside": "within 10 m outside the windows, without self-moving states",
        "travel_median_abs_min": "travel time error on transitions, median \\|core − engine\\|, game min",
    }
    for name, c in r["checks"].items():
        if name == "missing_marks":
            value, limit = str(c["value"]), str(c["limit"])
        else:
            value = num(c["value"]) if name.endswith("_min") else pct(c["value"])
            limit = num(c["limit"]) if name.endswith("_min") else pct(c["limit"])
        lines.append(f"| {labels[name]} | {value} | {c['op']} {limit} | {'pass' if c['pass'] else 'FAIL'} |")
    lines += [
        "",
        "| Marks | Count | Same ZS_* | Within 10 m (count) |",
        "|---|---|---|---|",
        f"| outside the windows | {o['marks']} | {pct(o['same_state'])} | {pct(o['near'])} ({o['place_marks']}) |",
        f"| inside the windows | {w['marks']} | {pct(w['same_state'])} | {pct(w['near'])} ({w['place_marks']}) |",
        f"| of them in open windows | {w['open_marks']} | {pct(w['open_same_state'])} | {pct(w['open_near'])} |",
        f"| outside, self-moving states included | {o['marks']} | {pct(o['same_state'])} | "
        f"{pct(o['near_with_self_moving'])} |",
    ]
    for s, c in r["self_moving"].items():
        lines.append(
            f"| self-moving `{s}`, outside | {c['marks']} | {pct(c['same_state'])} | {pct(c['near'])} — "
            f"not in the place check: {c['why']} |"
        )
    lines += [
        "",
        f"Travel time on transitions: {t['transitions']} (the core never came within 10 m: {t['core_missing']}); "
        f"|core − engine| median {num(t['median_abs_min'])} min, p90 {num(t['p90_abs_min'])} min; "
        f"signed median {num(t['median_signed_min'])} min; engine's median travel {num(t['engine_median_min'])} min.",
        "",
        "## NPCs that differ most (outside the windows)",
        "",
        "| NPC | Marks | ZS differs | Farther than 10 m (median, m) | Most common engine / core ZS |",
        "|---|---|---|---|---|",
    ]
    for n in r["worst_npcs"]:
        pairs = "; ".join(f"{a} / {b} ×{c}" for a, b, c in n["pairs"]) or "—"
        lines.append(
            f"| {n['key']} | {n['marks']} | {n['state_diff']} | {n['far']} ({num(n['far_median_m'], 1)}) | {pairs} |"
        )
    lines += [
        "",
        "## States (outside the windows)",
        "",
        "| Entry state | Marks | ZS differs | Farther than 10 m |",
        "|---|---|---|---|",
    ]
    for s in r["states"][:20]:
        lines.append(
            f"| {s['state']} | {s['marks']} | {s['state_diff']} | {s['far']} of {s['place_marks']} |"
        )
    lines += [
        "",
        "## Engine / core ZS_* that differ most (outside the windows)",
        "",
        "| Engine | Core | Marks |",
        "|---|---|---|",
    ]
    for a, b, n in r["state_pairs"]:
        lines.append(f"| {a} | {b} | {n} |")
    lines += [
        "",
        f"Not people (monsters, without the hero and the dead or the moment of a state change in the engine): "
        f"same ZS_* in {pct(r['not_people']['same_state'])} of {r['not_people']['marks']} marks, for information.",
        "",
        "## Open windows (the engine never came within 10 m of the entry's point)",
        "",
        "| NPC | Marks by entry state |",
        "|---|---|",
    ]
    for k, v in list(r["open_windows"].items())[:20]:
        lines.append(f"| {k} | " + ", ".join(f"{s} {n}" for s, n in v.items()) + " |")
    lines += [
        "",
        "## Transitions with the largest travel error",
        "",
        "| NPC | Start, min | State | Point | Engine, min | Core, min |",
        "|---|---|---|---|---|---|",
    ]
    for x in r["worst_travel"]:
        lines.append(
            f"| {x['key']} | {x['begin']} | {x['state']} | {x['point']} | {x['engine_min']} | "
            f"{'—' if x['core_min'] is None else x['core_min']} |"
        )
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--world", required=True, help="society-world export (v1 or v2)")
    parser.add_argument(
        "--core", required=True, help="directory with society-marks.csv of gothic-society-day"
    )
    parser.add_argument(
        "--engine", required=True, help="npc-day-parity bench directory (npc-day.csv, events, json)"
    )
    parser.add_argument("--out", help="Markdown report")
    parser.add_argument("--json", help="JSON report")
    parser.add_argument("--check", action="store_true", help="exit 1 when a threshold fails")
    args = parser.parse_args(argv)
    result = compare(args.world, args.core, args.engine)
    text = markdown(result)
    if args.out:
        Path(args.out).write_text(text)
    if args.json:
        Path(args.json).write_text(json.dumps(result, indent=1, ensure_ascii=False) + "\n")
    sys.stdout.write(text)
    return 1 if args.check and not result["pass"] else 0


if __name__ == "__main__":
    sys.exit(main())
