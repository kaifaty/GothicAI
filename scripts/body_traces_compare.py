#!/usr/bin/env python3
"""Compare body traces across a save and a fresh process (physics stage 8, step 4b).

python3 scripts/body_traces_compare.py <body-traces out> <body-traces-reload out>

The first run's `body_traces save created` records each fixture body; the second
run (SAVE=<first>/session-created.sav, body-traces-reload.bench) samples the loaded
world and the production npc_view. Ids, death times, lifetime starts, engine
culprits and symbols must survive; npc_view age must continue from the saved clock.
"""

from __future__ import annotations

import json
import math
import sys
from pathlib import Path


def samples(directory: Path) -> list[dict]:
    rows = []
    for line in (directory / "events.jsonl").read_text(encoding="utf-8").splitlines():
        event = json.loads(line)
        if event.get("kind") == "body_traces_error":
            raise SystemExit(f"{directory}: {event.get('reason')}")
        if event.get("kind") == "body_traces":
            rows.append(event["sample"])
    return rows


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    created, loaded = (Path(arg).resolve() for arg in sys.argv[1:])
    saved = next(s for s in samples(created) if s.get("archive") == "session-created.sav")
    reload_rows = samples(loaded)
    sample = next(s for s in reload_rows if "load_minute" in s)
    view = next(s for s in reload_rows if s.get("label") == "reload")
    before = {a["role"]: a for a in saved["actors"]}
    after = {a["role"]: a for a in sample["actors"]}
    failures = []
    if set(before) != set(after):
        failures.append(f"roles differ: saved {sorted(before)}, loaded {sorted(after)}")
    for role, a in before.items():
        b = after.get(role)
        if b is None:
            continue
        for key in ("npc", "id", "trace_id", "died_at", "clock_from", "culprit_truth", "dead", "present"):
            if a[key] != b[key]:
                failures.append(f"{role}.{key}: saved {a[key]!r}, loaded {b[key]!r}")
        moved = math.dist(a["position_cm"], b["position_cm"])
        if moved > 5:
            failures.append(f"{role}: loaded {moved:.1f} cm from the saved position")
    if sample["game_minute"] - saved["game_minute"] > 2:
        failures.append("the loaded clock is not the saved one")
    victim = before.get("victim", {})
    if sample["body_traces"]:
        if view.get("expected_id") != victim.get("trace_id"):
            failures.append(f"npc_view id {view.get('expected_id')} != saved {victim.get('trace_id')}")
        expected_age = view["game_minute"] - victim.get("died_at", 0)
        if view.get("age") is None or abs(view["age"] - expected_age) > 1:
            failures.append(f"npc_view age {view.get('age')} after loading, the clock says {expected_age}")
    result = {
        "pass": not failures,
        "failures": failures,
        "bodies": len(before),
        "victim_trace_id": victim.get("trace_id"),
        "victim_died_at": victim.get("died_at"),
        "saved_minute": saved["game_minute"],
        "loaded_minute": sample["game_minute"],
        "loaded_view_age": view.get("age"),
    }
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
