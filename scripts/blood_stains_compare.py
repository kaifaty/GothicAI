#!/usr/bin/env python3
"""Compare blood stains across a save and a fresh process (physics stage 8, step 4c).

python3 scripts/blood_stains_compare.py <blood-stains out> <blood-stains-reload out>

The first run's `blood_stains save created` records every stain and `blood` trace;
the second run (SAVE=<first>/session-created.sav, blood-stains-reload.bench) samples the
loaded world. Ids, kinds, positions, sizes, birth times and body links must survive;
trace ids must be the same and their age must continue from the saved clock.
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
        if event.get("kind") == "blood_stains_error":
            raise SystemExit(f"{directory}: {event.get('reason')}")
        if event.get("kind") == "blood_stains":
            rows.append(event["sample"])
    return rows


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    created, loaded = (Path(arg).resolve() for arg in sys.argv[1:])
    saved = next(s for s in samples(created) if s.get("archive") == "session-created.sav")
    sample = samples(loaded)[-1]
    failures = []
    before = {s["id"]: s for s in saved["stains"]}
    after = {s["id"]: s for s in sample["stains"]}
    if not before and saved["blood"]:
        failures.append("the saved world has no stains")
    if set(before) != set(after):
        failures.append(f"stain ids differ: saved {sorted(before)}, loaded {sorted(after)}")
    for sid, a in before.items():
        b = after.get(sid)
        if b is None:
            continue
        for key in ("kind", "blood", "indoor", "born_at_ms", "body_id", "radius_cm"):
            if a[key] != b[key]:
                failures.append(f"stain {sid}.{key}: saved {a[key]!r}, loaded {b[key]!r}")
        if math.dist(a["position_cm"], b["position_cm"]) > 0.05:
            failures.append(f"stain {sid}: loaded {math.dist(a['position_cm'], b['position_cm']):.2f} cm away")
    clock = sample["game_minute"] - saved["game_minute"]
    if not 0 <= clock <= 2:
        failures.append(f"the loaded clock is {clock} minutes from the saved one")
    traces_before = {t["id"]: t for t in saved["traces"]}
    traces_after = {t["id"]: t for t in sample["traces"]}
    if set(traces_before) != set(traces_after):
        failures.append(f"trace ids differ: saved {sorted(traces_before)}, loaded {sorted(traces_after)}")
    for tid, a in traces_before.items():
        b = traces_after.get(tid)
        if b is not None and abs((b["age"] - a["age"]) - clock) > 1:
            failures.append(f"{tid}: age {a['age']} saved, {b['age']} loaded, clock moved {clock}")
    result = {
        "pass": not failures,
        "failures": failures,
        "stains": len(before),
        "traces": sorted(traces_before),
        "saved_minute": saved["game_minute"],
        "loaded_minute": sample["game_minute"],
        "ages": {tid: [a["age"], traces_after.get(tid, {}).get("age")] for tid, a in traces_before.items()},
    }
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
