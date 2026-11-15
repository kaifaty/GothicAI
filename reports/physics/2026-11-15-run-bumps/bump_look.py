#!/usr/bin/env python3
"""Numbers of the bump answer per take of the step-2 scenario (was_bench.py), from a bench run folder.

    python3 reports/physics/2026-11-15-run-bumps/bump_look.py <abs out dir>

Reads metrics.csv (the hero's b_* columns: the bump's throw of the trunk and of the head, degrees) and the `bump`
events of run/melee-posture.jsonl. For every answer of the hero it prints when the root stopped, when the answer
began, its peak, and what is left of it 300 ms after the start — the guard rails of the note's «Приёмка».
"""
import csv
import json
import math
import os
import sys

run = sys.argv[1]
rows = list(csv.DictReader(open(os.path.join(run, "metrics.csv"))))
events = []
journal = os.path.join(run, "run", "melee-posture.jsonl")
if os.path.exists(journal):
    events = [json.loads(line) for line in open(journal) if '"event":"bump"' in line]

takes = {}
for r in rows:
    takes.setdefault(r["seg"], []).append(r)

print(f"{len(events)} bump events, {sum(e['strength'] > 0 for e in events)} with strength")
for e in events:
    if e["strength"] > 0:
        print(f"  {e['who']:>18} {e['kind']:6} s={e['strength']:.2f} u={e['speed_cm_s']:.0f} angle={e['angle_deg']:.0f}")

for take, frames in takes.items():
    if not take:
        continue
    t = [float(f["t"]) for f in frames]
    pitch = [float(f["b_pitch"]) for f in frames]
    roll = [float(f["b_roll"]) for f in frames]
    twist = [float(f["b_twist"]) for f in frames]
    head = [float(f["b_head"]) for f in frames]
    hits = [int(f["b_hits"]) for f in frames]
    lean = [float(f["ln_pitch"]) for f in frames]
    speed = [float(f["speed"]) for f in frames]
    starts = [i for i in range(1, len(frames)) if hits[i] > hits[i - 1]]
    if not starts:
        print(f"{take}: no answer; trunk max {max(math.hypot(p, r) for p, r in zip(pitch, roll)):.1f} deg")
        continue
    for i in starts:
        # the frame the root lost its speed: the last frame before the answer where it still ran
        ran = max((j for j in range(max(0, i - 6), i + 1) if speed[j] > 150), default=i)
        end = next((j for j in range(i, len(frames)) if t[j] - t[i] >= 300), len(frames) - 1)
        window = range(i, min(len(frames), i + 40))
        peak = max(window, key=lambda j: math.hypot(pitch[j], roll[j]))
        head_peak = max(window, key=lambda j: abs(head[j]))
        print(f"{take}: answer at t={t[i]:.0f} ms, {t[i] - t[ran]:.0f} ms after the last running frame; "
              f"trunk peak {pitch[peak]:+.1f} pitch {roll[peak]:+.1f} roll at +{t[peak] - t[i]:.0f} ms, "
              f"twist {max(twist[j] for j in window):+.1f}/{min(twist[j] for j in window):+.1f}; "
              f"head {head[head_peak]:+.1f} at +{t[head_peak] - t[i]:.0f} ms; "
              f"at +300 ms trunk {math.hypot(pitch[end], roll[end]):.1f}, head {abs(head[end]):.1f}; "
              f"lean_accel {min(lean[j] for j in window):+.1f}..{max(lean[j] for j in window):+.1f}")
