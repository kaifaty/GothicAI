"""Offline coverage-LOD estimate from archived per-crown censuses.

Goal: price demoting the main-view cut of spruce crowns whose winning-pixel
contribution is low, without any game run. Inputs are local copies of the
Ryzen archives (see the note for paths):

  trees.csv, placements.csv, census-frame.txt
      fir-view1-pixels-20261122T184646Z (census, tick 11644, inside hold)
  {inside,walk}.csv, {inside,walk}-candidates.csv
      fir-bough-depth-20261122T201811Z/abba comparison counters + the
      granularity replay cohort tree-8ms-cull-granularity/pair/analysis/
      final-depth. The pair archive does not preserve the owner-to-tree
      join, so pair scenes enter as aggregate context only.

Prices are the RTX 3080 calibrations of tree-cull-granularity.md:
0.45 ns per removed card triangle (B1 GBuffer delta), 0.20 ns per new
triangle (F model with margin), Bough-cut P50 0.610 ms inside over
992 190 replay reads. Mechanism cost is bounded by the crown-level test
price of the same note (0.001-0.011 ms) plus win counting; 0.05-0.10 ms
is used as a conservative bracket.

Usage: python3 analyze.py <artifact-dir> [--json out.json]
"""

from array import array
from collections import Counter
import argparse
import csv
import json
import math
import statistics
import struct
import sys
from pathlib import Path

REMOVED_NS = 0.45   # ms per million removed card triangles
NEW_NS = 0.20       # ms per million replacement triangles
BOUGH_CUT_MS = 0.610  # inside P50, census replay
MECH_LO, MECH_HI = 0.05, 0.10
TAUS = (1, 16, 100, 256, 1000)
KEPT = (0.0, 0.16, 0.4)  # impostor ceiling, two-level drop, one-level drop


def rows(path):
    with open(path, newline="") as fh:
        return list(csv.DictReader(fh))


def census_rows(folder):
    trees = rows(folder / "trees.csv")
    placements = rows(folder / "placements.csv")
    assert [t["key"] for t in trees] == [p["key"] for p in placements]
    total = int((folder / "census-frame.txt").read_text().split()[6])
    submitted = [t for t in trees if t["cut"] != "none"]
    assert sum(int(t["bough_triangles"]) for t in submitted) == total
    out = []
    for t in submitted:
        out.append(dict(key=t["key"], id=int(t["id"]), compact=t["cut"] == "compact",
                        tris=int(t["bough_triangles"]), units=int(t["bough_units"]),
                        wins=int(t["needle_3d_pixels"]), cut=t["cut"]))
    return out, total


def pair_context(folder, scene):
    """Aggregate counters only: the pair archive does not preserve the
    owner-to-tree join (see tree-cull-granularity.md), so per-crown wins
    cannot be decoded offline for these scenes."""
    counters = rows(folder / f"{scene}.csv")[0]
    candidates = {}
    for r in rows(folder / f"{scene}-candidates.csv"):
        # Same membership filter as the archived replay (paired_capture.py):
        # frustum sphere, impostor transition height, whole-crown early reject.
        if (float(r["frustum_distance"]) > -float(r["dynamic_radius"])
                and float(r["height_px"]) > 46 * 80 / 49
                and int(r["whole_reject"]) == 0):
            candidates[r["key"]] = r
    assert sum(int(c["a_tris"]) for c in candidates.values()) == int(counters["a_triangles"])
    return dict(scene=scene, tick=int(counters["tick"]), crowns=len(candidates),
                triangles=int(counters["a_triangles"]),
                whole_rejected=int(counters["occluded"]),
                compact=sum(1 for c in candidates.values() if c["compact"] == "1"))


def distribution(data, label):
    buckets = [("0", lambda w: w == 0), ("1-15", lambda w: 1 <= w < 16),
               ("16-99", lambda w: 16 <= w < 100), ("100-255", lambda w: 100 <= w < 256),
               ("256-999", lambda w: 256 <= w < 1000), (">=1000", lambda w: w >= 1000)]
    total_tris = sum(r["tris"] for r in data)
    total_wins = sum(r["wins"] for r in data)
    table = []
    for name, pred in buckets:
        group = [r for r in data if pred(r["wins"])]
        tris = sum(r["tris"] for r in group)
        wins = sum(r["wins"] for r in group)
        table.append(dict(bucket=name, crowns=len(group), triangles=tris,
                          triangle_pct=round(100 * tris / total_tris, 2),
                          wins=wins, win_pct=round(100 * wins / total_wins, 2) if total_wins else 0.0,
                          compact=sum(1 for r in group if r["compact"])))
    return dict(scene=label, crowns=len(data), triangles=total_tris, wins=total_wins, buckets=table)


def simulate(data, tau, kept):
    demoted = [r for r in data if r["wins"] < tau]
    kept_rows = [r for r in data if r["wins"] >= tau]
    dem_tris = sum(r["tris"] for r in demoted)
    dem_units = sum(r["units"] for r in demoted)
    all_units = sum(r["units"] for r in data)
    removed_m = dem_tris * (1 - kept) / 1e6
    new_m = dem_tris * kept / 1e6
    gpu = REMOVED_NS * removed_m - NEW_NS * new_m
    # Traversal bonus upper bound: reads scale with compact node count; a
    # coarser cut still walks its retained nodes, so scale by (1 - kept).
    bonus = BOUGH_CUT_MS * dem_units / all_units * (1 - kept) if all_units else 0.0
    return dict(tau=tau, kept=kept, demoted_crowns=len(demoted),
                kept_crowns=len(kept_rows),
                kept_triangles=sum(r["tris"] for r in kept_rows),
                removed_m=round(removed_m, 3), new_m=round(new_m, 3),
                gpu_ms=round(gpu, 3),
                net_lo_ms=round(gpu - MECH_HI, 3),
                net_hi_ms=round(gpu - MECH_LO + bonus, 3),
                walk_bonus_ms=round(bonus, 3))


def read_frame(folder, placements, profile, next_slot):
    attempt, read_attempt, slot, tick, width, height, total = map(
        int, (folder / "frame.txt").read_text().split())
    if (attempt != int(folder.name) or (width, height) != (1280, 720)
            or next_slot.get(attempt) != read_attempt or int(profile[attempt]["cmd_id"]) != slot
            or int(profile[read_attempt]["tree_bough_triangles"]) != total):
        raise ValueError(f"{folder}: invalid extent or frame-profile join")
    return decode_owners(folder, placements, width, height, total), tick, total


def decode_owners(folder, placements, width, height, total):
    owners = {}
    lookup = {}
    for owner in rows(folder / "owners.csv"):
        if owner.get("crown", "1") != "1":
            continue
        pos = tuple(round(float(owner[k]) * 10) for k in "xyz")
        key = placements[pos]
        cluster = int(owner["cluster"])
        if cluster in owners:
            raise ValueError(f"{folder}: duplicate crown cluster {cluster}")
        owners[cluster] = key
        for prefix in ("", "band_"):
            instance = int(owner[prefix + "instance"]) + 1
            for value in (int(owner[prefix + "cluster"]) + 1, 1 << 29 | instance, 2 << 29 | instance):
                if value in lookup:
                    raise ValueError(f"{folder}: duplicate winner ID {value}")
                lookup[value] = key
    if set(owners.values()) != set(placements.values()) or len(owners) != 1024:
        raise ValueError(f"{folder}: incomplete placement join")
    data = {}
    for cluster, mode, tris, units in struct.iter_unpack("<4I", (folder / "bough.u32").read_bytes()):
        key = owners[cluster]
        if key in data or mode not in (0, 1, 2) or (mode > 0) != (tris > 0) or (tris > 0) != (units > 0):
            raise ValueError(f"{folder}: invalid cut for {key}")
        data[key] = dict(tris=tris, compact=mode == 2, wins=0)
    if len(data) != 1024 or sum(v["tris"] for v in data.values()) != total:
        raise ValueError(f"{folder}: per-crown triangles differ from the stock counter")
    plane = array("I")
    plane.frombytes((folder / "winners.u32").read_bytes())
    if sys.byteorder != "little":
        plane.byteswap()
    if plane.itemsize != 4 or len(plane) != width * height:
        raise ValueError(f"{folder}: incomplete R32U plane")
    for value, count in Counter(plane).items():
        tag = value >> 29
        if tag not in (0, 1, 2, 4, 5) or (tag == 2 and value not in lookup):
            raise ValueError(f"{folder}: unknown winner {value}")
        if value in lookup:
            data[lookup[value]]["wins"] += count
    return data


def write_table(path, records):
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=records[0])
        writer.writeheader()
        writer.writerows(records)


def describe(values):
    ordered = sorted(values)
    return dict(min=ordered[0], mean=statistics.mean(values), p50=statistics.median(values),
                p95=ordered[math.ceil(.95 * len(values)) - 1], max=ordered[-1])


def transitions(frames, threshold, output):
    # A demotion requires a submitted cut. A newly submitted crown is reported separately.
    below, zero, longest = {}, {}, {}
    previous = {}
    changes = []
    for attempt, data in frames:
        up, down, entrants = 0, 0, 0
        for key, row in data.items():
            active = row["tris"] > 0
            was = previous.get(key)
            if active and row["wins"] >= threshold:
                if was and was["tris"] and was["wins"] < threshold:
                    up += 1
                    output.append(dict(attempt=attempt, threshold=threshold, key=key, wins=row["wins"],
                                       low_frames=below[key], preceding_zero_frames=zero[key],
                                       longest_zero_in_low_spell=longest[key]))
                elif was and not was["tris"]:
                    entrants += 1
            if active and was and was["tris"] and was["wins"] >= threshold > row["wins"]:
                down += 1
            if active and row["wins"] < threshold:
                below[key] = below.get(key, 0) + 1
                zero[key] = zero.get(key, 0) + 1 if not row["wins"] else 0
                longest[key] = max(longest.get(key, 0), zero[key])
            else:
                below[key] = zero[key] = longest[key] = 0
        if previous:
            changes.append(dict(attempt=attempt, threshold=threshold, promotions=up,
                                demotions=down, new_submitted_winners=entrants))
        previous = data
    return changes


def analyze_capture(root, out):
    out.mkdir(parents=True, exist_ok=True)
    placement_rows = rows(root / "placements.csv")
    placements = {tuple(round(float(r[k]) * 10) for k in "xyz"):r["key"] for r in placement_rows}
    if len(placements) != len(set(placements.values())) or len(placements) != 1024:
        raise ValueError("Expected 1,024 distinct placements")
    all_attempts = rows(root / "frameprofile.csv")
    if any(int(r["overflow"]) for r in all_attempts):
        raise ValueError("Frame profile overflow")
    profile = {int(r["attempt"]): r for r in all_attempts if r["outcome"] == "1"}
    next_slot = {}
    last = {}
    for attempt, row in profile.items():
        slot = row["cmd_id"]
        if slot in last:
            next_slot[last[slot]] = attempt
        last[slot] = attempt
    result = {}
    for scene in ("inside", "walk"):
        folders = sorted((root / "capture" / scene).iterdir(), key=lambda p: int(p.name))
        expected = [a for a, row in profile.items() if row["segment"] == "perf-coverage/" + scene]
        if len(expected) < 2 or [int(p.name) for p in folders] != expected:
            raise ValueError(f"{scene}: missing or extra submitted frames")
        frames, summaries, buckets = [], [], []
        for folder, attempt in zip(folders, expected):
            data, tick, total = read_frame(folder, placements, profile, next_slot)
            frames.append((attempt, data))
            dist = distribution([r for r in data.values() if r["tris"]], scene)
            for row in dist["buckets"]:
                buckets.append(dict(attempt=attempt, **row))
            front = dist["buckets"][-1]
            zero = dist["buckets"][0]["triangles"]
            summaries.append(dict(attempt=attempt, tick=tick, triangles=total, crowns=dist["crowns"],
                                  wins=dist["wins"], zero_triangles=zero, zero_pct=100*zero/total,
                                  front_crowns=front["crowns"], front_triangles=front["triangles"],
                                  front_wins=front["wins"], front_pct=100*front["wins"]/dist["wins"],
                                  wins_without_bough_cut=sum(r["wins"] for r in data.values() if not r["tris"])))
        if any(a["tick"] > b["tick"] for a, b in zip(summaries, summaries[1:])):
            raise ValueError(f"{scene}: scene tick went backwards")
        stats = {k: describe([r[k] for r in summaries]) for k in summaries[0] if k not in ("attempt", "tick")}
        if scene == "inside" and not 13080308 <= stats["triangles"]["p50"] <= 13198353:
            raise ValueError("Inside median differs from the crown-fix reference")
        totals = {k: sum(r[k] for r in summaries) for k in ("triangles", "zero_triangles", "wins", "front_wins")}
        groups = []
        for name in [r["bucket"] for r in buckets[:6]]:
            group = [r for r in buckets if r["bucket"] == name]
            groups.append(dict(bucket=name, crowns_p50=statistics.median(r["crowns"] for r in group),
                               triangles_p50=statistics.median(r["triangles"] for r in group),
                               triangle_pct=100*sum(r["triangles"] for r in group)/totals["triangles"],
                               win_pct=100*sum(r["wins"] for r in group)/totals["wins"]))
        changes, events, stability = [], [], {}
        for threshold in (1, 16, 256):
            new = []
            per_frame = transitions(frames, threshold, new)
            changes.extend(per_frame)
            events.extend(new)
            stability[str(threshold)] = dict(promotions_per_frame=describe([r["promotions"] for r in per_frame]),
                promotions=len(new), demotions=sum(r["demotions"] for r in per_frame),
                new_submitted_winners=sum(r["new_submitted_winners"] for r in per_frame),
                max_low_frames=max((r["low_frames"] for r in new), default=0),
                max_zero_frames=max((r["longest_zero_in_low_spell"] for r in new), default=0),
                max_consecutive_zero_immediately_before=max((r["preceding_zero_frames"] for r in new), default=0))
        for suffix, table in (("frames", summaries), ("buckets", buckets), ("transitions", changes),
                              ("bucket-summary", groups)):
            write_table(out / f"{scene}-{suffix}.csv", table)
        with (out / f"{scene}-promotions.csv").open("w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=("attempt", "threshold", "key", "wins", "low_frames",
                                                   "preceding_zero_frames", "longest_zero_in_low_spell"))
            writer.writeheader()
            writer.writerows(events)
        result[scene] = dict(frames=len(frames), first_tick=summaries[0]["tick"], last_tick=summaries[-1]["tick"],
                             stats=stats, buckets=groups, stability=stability,
                             interval_s=(int(profile[expected[-1]]["begin_ns"]) -
                                         int(profile[expected[0]]["begin_ns"]))/1e9)
    return result


def analyze_saved_frames(root, out):
    """Recover complete GPU readbacks after an abort. No independent profile or window validation."""
    placements = {tuple(round(float(r[k])*10) for k in "xyz"): r["key"] for r in rows(root / "placements.csv")}
    out.mkdir(parents=True, exist_ok=True)
    frames, buckets, crowns, incomplete = [], [], [], []
    adjacent = []
    previous = None
    for folder in sorted((root / "capture").glob("*/*"), key=lambda p: (p.parent.name, int(p.name))):
        scene = folder.parent.name
        if not (folder / "frame.txt").is_file():
            incomplete.append(f"{scene}/{folder.name}")
            continue
        attempt, read_attempt, slot, tick, w, h, total = map(int, (folder / "frame.txt").read_text().split())
        if str(attempt) != folder.name or read_attempt <= attempt or (w, h) != (1280, 720):
            raise ValueError(f"{folder}: inconsistent capture metadata")
        data = decode_owners(folder, placements, w, h, total)
        dist = distribution([r for r in data.values() if r["tris"]], scene)
        identity = dict(scene=scene, attempt=attempt)
        frames.append(dict(**identity, read_attempt=read_attempt, slot=slot, tick=tick, triangles=total,
                           crowns=dist["crowns"], wins=dist["wins"],
                           wins_without_cut=sum(r["wins"] for r in data.values() if not r["tris"])))
        buckets.extend(dict(**identity, **r) for r in dist["buckets"])
        crowns.extend(dict(**identity, key=k, **r) for k, r in sorted(data.items()))
        if previous and previous[0] == scene and attempt == previous[1] + 1:
            for threshold in (1, 16, 256):
                events = []
                pair, = transitions([(previous[1], previous[2]), (attempt, data)], threshold, events)
                adjacent.append(dict(scene=scene, previous_attempt=previous[1], **pair,
                                     from_zero=sum(r["preceding_zero_frames"] > 0 for r in events)))
        previous = scene, attempt, data
    if frames:
        write_table(out / "saved-frames.csv", frames)
        write_table(out / "saved-buckets.csv", buckets)
        write_table(out / "saved-crowns.csv", crowns)
    if adjacent:
        write_table(out / "saved-adjacent.csv", adjacent)
    return dict(status="partial", adjacent=adjacent, frameprofile_present=(root / "frameprofile.csv").is_file(),
                incomplete=incomplete, frames=frames, buckets=buckets)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    parser.add_argument("--json", type=Path, default=None)
    parser.add_argument("--capture-tables", type=Path, help="analyze two captured windows and write per-frame CSVs")
    parser.add_argument("--saved-frames", type=Path, help="recover individual completed frames from an aborted run")
    args = parser.parse_args()
    if args.saved_frames:
        result = analyze_saved_frames(args.folder, args.saved_frames)
        (args.json or args.saved_frames / "partial.json").write_text(json.dumps(result, indent=2) + "\n")
        return
    if args.capture_tables:
        result = analyze_capture(args.folder, args.capture_tables)
        text = json.dumps(result, indent=2) + "\n"
        (args.json or args.capture_tables / "summary.json").write_text(text)
        return
    folder = args.folder

    census, census_total = census_rows(folder)

    result = {
        "inputs": dict(census_tick=11644, census_triangles=census_total),
        "distributions": [distribution(census, "census-inside")],
        "pair_context": [pair_context(folder, "inside"), pair_context(folder, "walk")],
        "simulation": {name: [simulate(data, tau, kept) for tau in TAUS for kept in KEPT]
                       for name, data in (("census-inside", census),)},
    }
    text = json.dumps(result, indent=2)
    print(text)
    if args.json:
        args.json.write_text(text)


if __name__ == "__main__":
    main()