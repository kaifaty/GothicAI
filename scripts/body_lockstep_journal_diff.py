#!/usr/bin/env python3
"""Compare lockstep episode journals, ignoring wall clocks and session ids."""
import argparse
import glob
import json
import os
import re

# Generation and episode counters belong to the fork template: they follow which episodes it served.
VOLATILE = re.compile(r"wall|epoch|monotonic|heartbeat|_us$|_ns$|^t$|elapsed_real|pid|generation|episode_id")
FILES = ("observations.jsonl", "events.jsonl", "arena-events.jsonl", "body-control-events.jsonl")


def norm(v):
    if isinstance(v, dict):
        return {k: norm(x) for k, x in v.items() if not VOLATILE.search(k)}
    if isinstance(v, list):
        return [norm(x) for x in v]
    return v


def episodes(run):
    return {os.path.basename(d): d for d in glob.glob(os.path.join(run, "worker-*", "ep-*")) if os.path.isdir(d)}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_a", metavar="RUN_A", help="first collector run directory")
    parser.add_argument("run_b", metavar="RUN_B", help="second collector run directory")
    args = parser.parse_args(argv)
    a, b = episodes(args.run_a), episodes(args.run_b)
    common = sorted(set(a) & set(b))
    print(f"episodes: {len(a)} vs {len(b)}, common {len(common)}")
    first = {}
    same = {f: 0 for f in FILES}
    for ep in common:
        for f in FILES:
            pa, pb = os.path.join(a[ep], f), os.path.join(b[ep], f)
            la = open(pa).read().splitlines() if os.path.exists(pa) else []
            lb = open(pb).read().splitlines() if os.path.exists(pb) else []
            ok = len(la) == len(lb)
            if ok:
                for i, (x, y) in enumerate(zip(la, lb)):
                    if norm(json.loads(x)) != norm(json.loads(y)):
                        ok = False
                        first.setdefault(f, (ep, i, x[:400], y[:400]))
                        break
            else:
                first.setdefault(f, (ep, -1, f"{len(la)} lines", f"{len(lb)} lines"))
            same[f] += ok
    for f in FILES:
        print(f"{f}: identical {same[f]}/{len(common)}")
    for f, (ep, i, x, y) in first.items():
        print(f"first difference {f} {ep} line {i}:\n  A {x}\n  B {y}")


if __name__ == "__main__":
    main()
