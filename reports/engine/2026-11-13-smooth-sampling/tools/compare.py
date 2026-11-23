#!/usr/bin/env python3
"""Two runs of one bench side by side, to see that the curve leaves the feet and the blows as they were: per take
the flags of the report (how many of each kind and the worst), the contact of the planted feet with the ground and
the largest change of a joint's step.

  compare.py <run a> <run b> [<run c> ...]

The first run is the reference. A third run of the reference's kind shows how much two equal runs differ.
"""
import collections
import json
import sys
from pathlib import Path

STATS = ("contact_p50", "contact_p90", "contact_max", "anim_p99", "anim_max", "anim_switch_max")


def load(run):
    r = json.loads((Path(run) / "report.json").read_text())
    flags = collections.defaultdict(lambda: collections.defaultdict(list))
    for e in r["events"]:
        flags[e.get("seg")][e.get("flag")].append(float(e.get("worst", 0)))
    return r["segments"], flags


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    runs = [load(r) for r in argv]
    names = [Path(r).name for r in argv]
    print("| take | " + " | ".join(f"{n}: flags (worst)" for n in names) + " | "
          + " | ".join(f"{n}: contact p50 / p90 / max, cm; step p99 / max / at a switch, cm" for n in names) + " |")
    print("|---" * (1 + 2 * len(runs)) + "|")
    total = [collections.Counter() for _ in runs]
    for seg in runs[0][0]:
        cells, stats = [], []
        for i, (segments, flags) in enumerate(runs):
            f = flags.get(seg, {})
            for k, v in f.items():
                total[i][k] += len(v)
            cells.append(", ".join(f"{k} {len(v)} ({max(v):.1f})" for k, v in sorted(f.items())) or "—")
            s = segments.get(seg, {})
            num = lambda k: f"{s[k]:.1f}" if isinstance(s.get(k), (int, float)) else "—"  # noqa: E731
            stats.append(f"{num('contact_p50')} / {num('contact_p90')} / {num('contact_max')}; "
                         f"{num('anim_p99')} / {num('anim_max')} / {num('anim_switch_max')}")
        print(f"| {seg} | " + " | ".join(cells) + " | " + " | ".join(stats) + " |")
    for n, t in zip(names, total):
        print(f"{n}: " + (", ".join(f"{k} {c}" for k, c in sorted(t.items())) or "no flags"))


if __name__ == "__main__":
    main(sys.argv[1:])
