#!/usr/bin/env python3
"""PSNR of every configuration's shot against the baseline configuration's shot (ffmpeg psnr filter).
Usage: imgdiff.py <survey dir> <baseline label> <shot> [shot ...]"""
import re, subprocess, sys
from pathlib import Path
root, base = Path(sys.argv[1]), sys.argv[2]
shots = sys.argv[3:]
print("config".ljust(12), *[s.ljust(10) for s in shots])
for d in sorted(p for p in root.iterdir() if p.is_dir()):
    vals = []
    for s in shots:
        a, b = root / base / f"{s}.png", d / f"{s}.png"
        if not (a.exists() and b.exists()):
            vals.append("-"); continue
        r = subprocess.run(["ffmpeg", "-hide_banner", "-i", str(a), "-i", str(b), "-lavfi", "psnr", "-f", "null", "-"],
                           capture_output=True, text=True).stderr
        m = re.search(r"average:([0-9.inf]+)", r)
        vals.append(m.group(1) if m else "?")
    print(d.name.ljust(12), *[v.ljust(10) for v in vals])
