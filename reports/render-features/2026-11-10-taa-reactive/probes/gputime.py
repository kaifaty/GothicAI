#!/usr/bin/env python3
"""Accumulated Metal GPU time per process from ioreg (AGX user clients): prints {"pid name": ns}."""
import json, re, subprocess, sys, time
def snap():
    # ioreg prints a node's properties sorted by name: "AppUsage" comes before its "IOUserClientCreator"
    out = subprocess.run(['ioreg', '-r', '-c', 'AGXAccelerator', '-l', '-w0'], capture_output=True, text=True).stdout
    res = {}; usage = 0
    for line in out.splitlines():
        if '+-o ' in line:
            usage = 0
        if '"AppUsage"' in line:
            usage = sum(int(x) for x in re.findall(r'"accumulatedGPUTime"=(\d+)', line))
        m = re.search(r'"IOUserClientCreator" = "pid (\d+), ([^"]*)"', line)
        if m:
            key = f"{m.group(1)} {m.group(2)}"
            res[key] = res.get(key, 0) + usage
            usage = 0
    return res
if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == 'top':
        dt = float(sys.argv[2]) if len(sys.argv) > 2 else 5
        a = snap(); time.sleep(dt); b = snap()
        d = sorted(((b[k] - a.get(k, 0)) / 1e9 / dt * 100, k) for k in b)
        for pct, k in reversed(d[-8:]): print(f"{pct:6.1f} % {k}")
    else:
        print(json.dumps(snap()))
