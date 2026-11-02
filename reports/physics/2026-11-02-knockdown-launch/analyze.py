#!/usr/bin/env python3
"""Reduce paired events.jsonl files; reject an invalid or unsafe acceptance run."""

import argparse
import csv
import json
import statistics
from pathlib import Path


PARTS = ("pelvis", "spine", "head")
TARGET = "SLD_815_SOELDNER"
ATTACKER = "SLD_826_SOELDNER"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read(path):
    source = path / "events.jsonl" if path.is_dir() else path
    with source.open() as stream:
        return [json.loads(line) for line in stream if line.strip()]


def summarize(mode, events):
    weapons = [e for e in events if e.get("kind") == "launch_weapon"]
    require(len(weapons) == 70 and all(e["actor"] == ATTACKER and e["weapon"] == "ITMW_2H_SLD_AXE" and
                                      e["state"] == "W2H" for e in weapons),
            f"{mode}: expected 70 exact drawn axe checks, got {len(weapons)}")
    rows = []
    for cell in ("close", "medium", "far"):
        prefix = f"birth-{cell}-seed-"
        records = [e for e in events if e.get("kind") == "rec" and e.get("seg", "").startswith(prefix)]
        peaks = [e for e in events if e.get("kind") == "ragdoll_birth_peak" and
                 e.get("actor") == TARGET and e.get("seg", "").startswith(prefix)]
        require(len(records) == 20, f"{mode}/{cell}: expected 20 trials, got {len(records)}")
        missing = {e["seg"] for e in records} - {e["seg"] for e in peaks}
        require(len(peaks) == 20 and not missing,
                f"{mode}/{cell}: {len(peaks)}/20 births; missing segments: {sorted(missing)}")
        for e in peaks:
            require(e["culprit"] == ATTACKER, f"{e['seg']}: incorrect knockOff culprit")
            require(e["guard_enabled"] == (mode == "on"), f"{e['seg']}: wrong guard mode")
            require(299.99 <= e["sampled_ms"] <= 317, f"{e['seg']}: incorrect simulation window")
            low, high = {"close": (30, 40), "medium": (50, 90), "far": (100, 170)}[cell]
            require(low - .1 <= e["birth_distance_cm"] <= high + .1, f"{e['seg']}: birth outside distance cell")
        row = {"mode": mode, "distance": cell, "knockdowns": len(peaks)}
        for part in PARTS:
            values = [e[f"{part}_m_s"] for e in peaks]
            row[f"median_{part}_m_s"] = statistics.median(values)
            row[f"max_{part}_m_s"] = max(values)
        row["knockdowns_over_8_m_s"] = sum(max(e[f"{p}_m_s"] for p in PARTS) > 8 for e in peaks)
        times = [e["guard_release_ms"] for e in peaks if e["guard_release_ms"] is not None]
        row["median_guard_release_ms"] = statistics.median(times) if times else ""
        row["guards_remaining_at_300ms"] = sum(any(g["removed_ms"] is None for g in e["guards"]) for e in peaks)
        rows.append(row)
        print(f"{mode}/{cell}: n={len(peaks)}, medians=" +
              "/".join(f"{row[f'median_{p}_m_s']:.6f}" for p in PARTS) +
              f" m/s, >8={row['knockdowns_over_8_m_s']}, release_ms={row['median_guard_release_ms']}")
        if mode == "on" and cell == "medium":
            for e in peaks:
                if max(e[f"{p}_m_s"] for p in PARTS) > 8:
                    print("MEDIUM FAST CONTACTS " + json.dumps(e, ensure_ascii=False))

    for requested in (0, 700, 1500):
        samples = [e for e in events if e.get("kind") == "stun_drift" and e.get("requested_ms") == requested]
        require(len(samples) == 10, f"{mode}: {len(samples)}/10 drift samples at {requested} ms")
        require(all(e["actor"] == TARGET and e["attacker"] == ATTACKER for e in samples), "Wrong drift actors")
        require(all(requested <= e["elapsed_ms"] <= requested + 34 for e in samples), "Wrong drift timing")
        print(f"{mode}/drift {requested}ms: n=10, " + ", ".join(
            f"{key} median={statistics.median(e[key] for e in samples):.6f} "
            f"range=[{min(e[key] for e in samples):.6f},{max(e[key] for e in samples):.6f}]"
            for key in ("toward_cm", "horizontal_cm", "height_cm")))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("on", type=Path)
    parser.add_argument("off", type=Path)
    parser.add_argument("--output", type=Path, default=Path(__file__).parent / "summary.csv")
    args = parser.parse_args()
    on, off = read(args.on), read(args.off)
    recordings = lambda events: [e["seg"] for e in events if e.get("kind") == "rec"]
    require(recordings(on) == recordings(off), "ON/OFF recording order and seeds differ")
    rows = summarize("on", on) + summarize("off", off)
    with args.output.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    indexed = {(r["mode"], r["distance"]): r for r in rows}
    require(any(indexed["off", "close"][f"median_{p}_m_s"] > 8 for p in PARTS),
            "NEEDS_DECISION: OFF/close does not reproduce the launch")
    require(all(indexed["on", "close"][f"median_{p}_m_s"] <= 4 for p in PARTS) and
            indexed["on", "close"]["knockdowns_over_8_m_s"] == 0,
            "NEEDS_DECISION: ON/close does not meet acceptance")
    require(indexed["on", "medium"]["knockdowns_over_8_m_s"] == 0,
            "NEEDS_DECISION: ON/medium has a launch above 8 m/s; see contact details")
    require(all(abs(indexed["on", "far"][f"median_{p}_m_s"] -
                    indexed["off", "far"][f"median_{p}_m_s"]) <= .5 for p in PARTS),
            "NEEDS_DECISION: ON/OFF far medians differ by more than 0.5 m/s")
    print(f"Acceptance passed: 120 knockdowns, 20 drift probes; {args.output}")


if __name__ == "__main__":
    main()
