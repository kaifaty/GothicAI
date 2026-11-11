#!/usr/bin/env python3
"""gpumon.py <out.jsonl> <stopfile>: every second, monotonic ns and the accumulated GPU time per process (gputime.py)."""
import json, os, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from gputime import snap
out, stop = sys.argv[1], sys.argv[2]
with open(out, 'w') as f:
    while not os.path.exists(stop):
        t = time.monotonic_ns(); s = snap()
        f.write(json.dumps({"t": t, "v": 2, "gpu": s}) + "\n"); f.flush()
        time.sleep(max(0.0, 1.0 - (time.monotonic_ns() - t) / 1e9))
