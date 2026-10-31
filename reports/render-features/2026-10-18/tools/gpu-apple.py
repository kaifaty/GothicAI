#!/usr/bin/env python3
"""Sample Apple GPU utilisation from ioreg every 200 ms; stamped with the monotonic clock of the frame profile."""
import re, subprocess, sys, time
out = open(sys.argv[1], "w", buffering=1)
out.write("mono_ns,device_util,renderer_util,tiler_util,in_use_mem\n")
keys = ["Device Utilization %", "Renderer Utilization %", "Tiler Utilization %", "In use system memory"]
while True:
    t = time.monotonic_ns()
    s = subprocess.run(["ioreg", "-r", "-d", "1", "-w", "0", "-c", "IOAccelerator"], capture_output=True, text=True).stdout
    vals = []
    for k in keys:
        m = re.search(r'"' + re.escape(k) + r'"=(\d+)', s)
        vals.append(m.group(1) if m else "")
    out.write(f"{t}," + ",".join(vals) + "\n")
    time.sleep(0.2)
