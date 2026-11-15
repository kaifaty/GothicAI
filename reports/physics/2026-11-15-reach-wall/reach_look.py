#!/usr/bin/env python3
"""Numbers of the hands and the stand per take of the step-3 scenario (reach_bench.py), from a bench run folder.

    python3 reports/physics/2026-11-15-reach-wall/reach_look.py <abs out dir> [<abs out dir of the other run>]

Reads metrics.csv (the hero's rc_* columns) and the `reach` events of run/melee-posture.jsonl. For every take: when
the root stopped, when the warning came and the palms stood on the wall, how the planted palm keeps its place (gap
to the surface, slip along it), the elbow and the wrist, when the stand is fully shown, how long the wall stop
held the hero and how far the root went on after it stopped. With a second folder: the
largest distance between the hero's root in the two runs, frame by frame of each take.
"""
import csv
import json
import math
import os
import sys


def load(run):
    rows = list(csv.DictReader(open(os.path.join(run, "metrics.csv"))))
    takes = {}
    for r in rows:
        if r["seg"]:
            takes.setdefault(r["seg"], []).append(r)
    return takes


run = sys.argv[1]
takes = load(run)
journal = os.path.join(run, "run", "melee-posture.jsonl")
events = []
if os.path.exists(journal):
    events = [json.loads(line) for line in open(journal) if '"event":"reach"' in line]
print(f"{len(events)} reach events:", ", ".join(f"{e['who']}:{e['phase']}" for e in events) or "none")

for take, frames in takes.items():
    t = [float(f["t"]) for f in frames]
    col = lambda name: [float(f[name]) for f in frames]
    press, warn, halt = col("rc_press"), col("rc_warn"), col("rc_halt")
    # the root's own speed, cm/s, from its place frame to frame
    x, z = col("x"), col("z")
    step = [0.0] + [math.hypot(x[i] - x[i - 1], z[i] - z[i - 1]) * 1000 / max(t[i] - t[i - 1], 1) for i in range(1, len(frames))]
    ran = [i for i in range(len(frames)) if step[i] > 200]
    if not ran:
        print(f"{take}: the hero never ran")
        continue
    # the stop: the first frame after a run where the root goes slower than 60 cm/s
    stop = next((i for i in range(ran[0] + 5, len(frames)) if step[i] < 60), None)
    line = [f"{take}:"]
    if stop is None:
        line.append("no stop")
        print(" ".join(line))
        continue
    first_warn = next((i for i in range(len(frames)) if warn[i] > 0), None)
    line.append(f"stop at {t[stop]:.0f} ms; warn {t[first_warn] - t[stop]:+.0f} ms" if first_warn is not None
                else f"stop at {t[stop]:.0f} ms; no warn")
    full = next((i for i in range(stop, len(frames)) if halt[i] >= 0.99), None)
    line.append(f"stand full {t[full] - t[stop]:+.0f} ms" if full is not None else f"stand max {max(halt):.2f}")
    for side, name in (("l", "left"), ("r", "right")):
        w, planted = col("rc_w" + side), col("rc_p" + side)
        gap, slip, elbow, wrist = col("rc_gap" + side), col("rc_slip" + side), col("rc_elb" + side), col("rc_wr" + side)
        on = [i for i in range(len(frames)) if planted[i] > 0 and w[i] >= 0.99 and i >= stop]
        if not on:
            line.append(f"{name}: not planted (weight max {max(w):.2f})")
            continue
        first = next(i for i in range(len(frames)) if planted[i] > 0)
        line.append(f"{name}: planted {t[first] - t[stop]:+.0f} ms, elbow {elbow[first]:.0f} then "
                    f"{min(elbow[i] for i in on):.0f}..{max(elbow[i] for i in on):.0f}, "
                    f"gap {min(gap[i] for i in on):+.1f}..{max(gap[i] for i in on):+.1f} cm, "
                    f"slip max {max(slip[i] for i in on):.1f} cm, wrist max {max(wrist[i] for i in on):.0f}")
    print(" ".join(line))
    # the wall stop: how long it held, how far the root went on after it stopped (a slide along the wall)
    held = [i for i in range(len(frames)) if "rc_stop" in frames[i] and float(frames[i]["rc_stop"]) > 0]
    crept = max(math.hypot(x[i] - x[stop], z[i] - z[stop]) for i in range(stop, len(frames)))
    print(f"    arm {float(frames[0]['rc_arm']):.1f} cm; press max {max(press):.2f}; stand max {max(halt):.2f}; "
          f"wall stop {'%.0f ms' % (t[held[-1]] - t[held[0]]) if held else 'none'}; "
          f"went on {crept:.0f} cm after the stop")

if len(sys.argv) > 2:
    other = load(sys.argv[2])
    for take, frames in takes.items():
        if take not in other:
            continue
        worst = 0.0
        for a, b in zip(frames, other[take]):
            worst = max(worst, math.hypot(float(a["x"]) - float(b["x"]), float(a["z"]) - float(b["z"])))
        print(f"root path {take}: {worst:.2f} cm apart at most ({len(frames)} / {len(other[take])} frames)")
