#!/usr/bin/env python3
"""Animation LOD numbers from streaming-flythrough frame profiles.

    python3 anim_lod_summary.py <run dir>... > anim-lod.csv

Each run dir holds frameprofile.csv (scripts/bench.sh streaming-flythrough). Per run and
segment (run, fly): frames with an animation pass, parallel_animation mean / p50 / p99 (ms),
NPC updates per frame by frustum (the probe's plain sphere), and the LOD counters per frame
(deferred, caught up when entering the view, caught up on a logic read).
"""
import csv
import statistics
import sys
from pathlib import Path


def p(values, q):
    values = sorted(values)
    if not values:
        return float("nan")
    k = (len(values) - 1) * q
    lo = int(k)
    hi = min(lo + 1, len(values) - 1)
    return values[lo] + (values[hi] - values[lo]) * (k - lo)


def summary(run):
    rows = list(csv.DictReader(open(Path(run) / "frameprofile.csv")))
    out = []
    for seg in ("run", "fly"):
        sel = [r for r in rows if r["segment"].endswith("/" + seg) and int(r["parallel_animation_calls"] or 0) > 0]
        if not sel:
            continue
        ms = [int(r["parallel_animation_ns"]) / 1e6 for r in sel]
        n = len(sel)

        def per_frame(col):
            return sum(int(r.get(col) or 0) for r in sel) / n

        def calls(visible):
            return sum(int(r[k]) for r in sel for k in r
                       if k.startswith("npc_anim_") and k.endswith(f"_{visible}_n")) / n

        out.append({
            "run": Path(run).name, "segment": seg, "frames": n,
            "parallel_mean_ms": round(statistics.fmean(ms), 4),
            "parallel_p50_ms": round(p(ms, 0.5), 4),
            "parallel_p99_ms": round(p(ms, 0.99), 4),
            "npc_updates_in": round(calls("in"), 2),
            "npc_updates_out": round(calls("out"), 2),
            "deferred": round(per_frame("npc_anim_deferred"), 2),
            "catchup": round(per_frame("npc_anim_catchup"), 3),
            "sync_on_read": round(per_frame("npc_anim_sync_on_read"), 3),
        })
    return out


def main(argv):
    rows = [row for run in argv[1:] for row in summary(run)]
    if not rows:
        print("no frameprofile.csv rows", file=sys.stderr)
        return 1
    w = csv.DictWriter(sys.stdout, fieldnames=list(rows[0]))
    w.writeheader()
    w.writerows(rows)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
