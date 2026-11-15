#!/usr/bin/env python3
"""Check ragdoll-brace-death's three cells in run/melee-posture.jsonl."""
import argparse
import json
from pathlib import Path


CELLS = ("death", "unconscious", "control")
TARGET = "SLD_815_SOELDNER"


def check(rows: list[dict]) -> tuple[list[str], list[str]]:
    failures, results = [], []
    falls = [i for i, row in enumerate(rows)
             if row.get("event") == "knockdown" and row.get("target", {}).get("name") == TARGET]
    if len(falls) != len(CELLS):
        return [f"expected 3 knockdowns of {TARGET}, got {len(falls)}"], []
    for cell, start, stop in zip(CELLS, falls, falls[1:] + [len(rows)]):
        fall = rows[start]
        actor_id = fall["target"]["id"]
        body = [row for row in rows[start:stop] if row.get("target", {}).get("id") == actor_id]
        ends = [row for row in body if row.get("event") == "brace"]
        if fall.get("brace", "none") == "none":
            failures.append(f"{cell}: knockdown has no active brace")
        if len(ends) != 1:
            failures.append(f"{cell}: expected exactly 1 brace event, got {len(ends)}")
            continue
        end = ends[0]
        reason = end.get("end")
        if cell != "control" and reason != cell:
            failures.append(f"{cell}: expected end={cell}, got {reason!r}")
        if cell == "control" and (not reason or reason in CELLS[:2]):
            failures.append(f"control: expected ordinary brace end, got {reason!r}")
        if end.get("fall_ms") != fall.get("tick_ms"):
            failures.append(f"{cell}: brace event belongs to another fall: {end.get('fall_ms')}")
        elapsed = end["tick_ms"] - end["fall_ms"]
        if cell != "control" and not 0 <= elapsed <= 300:
            failures.append(f"{cell}: brace ended {elapsed} ms after fall; expected 0..300 ms")
        if end.get("pose", "none") == "none":
            failures.append(f"{cell}: brace event has no protective pose")
        # Reject supplied post-end brace rows, including the end's tick. Production
        # currently has none, so this does not independently observe pose inactivity.
        after = body[body.index(end) + 1:]
        for row in after:
            if "brace" in row and row["brace"] != "none":
                failures.append(f"{cell}: active brace {row['brace']} after end at tick {row.get('tick_ms')}")
        results.append(f"{cell}: end={reason} after {elapsed} ms; " + json.dumps(end, separators=(",", ":")))
    return failures, results


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("journal", type=Path, help="run/melee-posture.jsonl from ragdoll-brace-death")
    args = parser.parse_args(argv)
    try:
        rows = [json.loads(line) for line in args.journal.read_text().splitlines() if line.strip()]
        failures, results = check(rows)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"FAIL: {args.journal}: {error}")
        return 1
    for result in results:
        print(result)
    for failure in failures:
        print(f"FAIL: {failure}")
    if not failures:
        print("PASS: death, unconscious, control (brace termination events)")
        print("LIMIT: post-end pose is not sampled; pose inactivity is not independently verified")
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())
