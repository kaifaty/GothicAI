#!/usr/bin/env python3
"""Sample the NVIDIA GPU every 100 ms; each line is stamped with CLOCK_MONOTONIC ns (same clock as the frame profile)."""
import subprocess, sys, time
out = open(sys.argv[1], "w", buffering=1)
out.write("mono_ns,util_gpu,clock_gr,power_w,mem_used_mib,pstate\n")
p = subprocess.Popen(["nvidia-smi", "--query-gpu=utilization.gpu,clocks.gr,power.draw,memory.used,pstate",
                      "--format=csv,noheader,nounits", "-lms", "100"], stdout=subprocess.PIPE, text=True)
for line in p.stdout:
    out.write(f"{time.monotonic_ns()},{line.strip().replace(' ', '')}\n")
