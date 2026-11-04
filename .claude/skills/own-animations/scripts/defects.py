"""Measured defect list of one dodge record: pace, pelvis and bone snaps, parts under the floor, the blade against the
floor and the body, feet sliding on the ground. The numbers start the list; the frame sheet decides.

    python3 defects.py <base models> <record.MAN.json> <stance.MAN.json>

- pace: speed between frames 0 and 1, the largest acceleration in the first 0.24 s, the peak pelvis speed;
- bone turns > 60° a frame are local (child against parent) and overstate a whip the child undoes: check with
  worldturn.py; "max turn" lists the sword arm, the left hand, the neck and the head whatever their size;
- under the floor: toes, feet, knees, hands, elbows, head, pelvis; the floor is the stance's own toe minus 3 cm;
- blade: 90 cm along −X of ZS_RIGHTHAND; the body is capsules of the torso, head, thighs and shins;
- sliding: a foot or toe under 8 cm in two frames running and moving over 8 cm between them (a step's first arc
  frame also counts, it is only a few cm up).
"""
import sys
from pathlib import Path

import numpy as np

from common import blade, floor_of, ga, runs, skeleton, turn

B = Path(sys.argv[1])
skel = skeleton(B)
a, st = ga.read_animation(Path(sys.argv[2])), ga.read_animation(Path(sys.argv[3]))
ix, n, fps = skel.index, a.frame_count, a.fps
W = np.array([ga.world_pose(skel, a, f) for f in range(n)])
P = W[:, :, :3, 3]
floor = floor_of(skel, st)
p = lambda nm: P[:, ix(nm)]  # noqa: E731

pel, head = p("BIP01 PELVIS"), p("BIP01 HEAD")
v = np.linalg.norm(np.diff(pel[:, [0, 2]], axis=0), axis=1) * fps / 100
acc = np.abs(np.diff(v)) * fps / 9.81
print(f"{(n - 1) / fps:.2f} s, {n} frames | speed f0→1 {v[0]:.1f} m/s, accel in 0.24 s up to {acc[:int(round(0.24 * fps))].max():.1f} g, "
      f"peak {v.max():.1f} m/s at {np.argmax(v)}")
pt = [turn(W[f, ix("BIP01 PELVIS")][:3, :3], W[f + 1, ix("BIP01 PELVIS")][:3, :3]) for f in range(n - 1)]
print(f"pelvis turn a frame max {max(pt):.0f}° at {int(np.argmax(pt))}")

ALWAYS = ("BIP01 R UPPERARM", "BIP01 R FOREARM", "BIP01 R HAND", "BIP01 L HAND", "BIP01 NECK", "BIP01 HEAD")
by = {}
for i, nd in enumerate(skel.nodes):
    if not nd.name.startswith("BIP01") or "FINGER" in nd.name or "TOE" in nd.name:
        continue
    for f in range(n - 1):
        L0 = np.linalg.inv(W[f, nd.parent]) @ W[f, i] if nd.parent >= 0 else W[f, i]
        L1 = np.linalg.inv(W[f + 1, nd.parent]) @ W[f + 1, i] if nd.parent >= 0 else W[f + 1, i]
        d = turn(L0[:3, :3], L1[:3, :3])
        if d > 60 or nd.name in ALWAYS:
            by.setdefault(nd.name, []).append((f, d))
big = [f"{nm[6:]} {runs([f for f, d in fd if d > 60])} up to {max(d for _, d in fd):.0f}°" for nm, fd in by.items() if max(d for _, d in fd) > 60]
print("bone turns > 60° a frame:", "; ".join(big) or "none")
print("max turn a frame:", ", ".join(f"{nm[6:]} {max(d for _, d in by[nm]):.0f}° at {max(by[nm], key=lambda x: x[1])[0]}" for nm in ALWAYS))

under = []
for nm in ("L TOE0", "R TOE0", "L FOOT", "R FOOT", "L CALF", "R CALF", "L HAND", "R HAND", "L FOREARM", "R FOREARM", "HEAD", "PELVIS"):
    h = p(f"BIP01 {nm}")[:, 1] - floor
    fs = [f for f in range(n) if h[f] < 0]
    if fs:
        under.append(f"{nm} {h.min():.0f} cm at {runs(fs)}")
print("under the floor:", "; ".join(under) or "none")


def seg(x, a_, b_):
    t = np.clip(np.dot(x - a_, b_ - a_) / max(np.dot(b_ - a_, b_ - a_), 1e-9), 0, 1)
    return float(np.linalg.norm(x - (a_ + t * (b_ - a_))))


low, gap, tips = [], [], []
for f in range(n):
    bl = blade(W[f], skel)
    low.append(bl[:, 1].min() - floor)
    hd = p("BIP01 HEAD")[f] + [0, 8, 0]
    caps = [(p("BIP01 PELVIS")[f], p("BIP01 NECK")[f], 14), (hd, hd, 12)]
    caps += [(p(f"BIP01 {s} THIGH")[f], p(f"BIP01 {s} CALF")[f], 8) for s in "LR"]
    caps += [(p(f"BIP01 {s} CALF")[f], p(f"BIP01 {s} FOOT")[f], 6) for s in "LR"]
    gap.append(min(seg(x, c0, c1) - r for x in bl for c0, c1, r in caps))
    tips.append(bl[-1])
low, gap = np.array(low), np.array(gap)
tv = np.linalg.norm(np.diff(np.array(tips), axis=0), axis=1)
print(f"blade lowest {low.min():.0f} cm over the floor at {int(np.argmin(low))}, under it at {runs([f for f in range(n) if low[f] < 0])}; "
      f"closest to the body {gap.min():.0f} cm at {int(np.argmin(gap))}, in it (over 2 cm) at {runs([f for f in range(n) if gap[f] < -2])}; "
      f"tip up to {tv.max():.0f} cm a frame at {int(np.argmax(tv))}→{int(np.argmax(tv)) + 1}")

slides = {}
for s in "LR":
    for nm in (f"BIP01 {s} FOOT", f"BIP01 {s} TOE0"):
        x = p(nm)
        h = x[:, 1] - floor
        for f in range(n - 1):
            d = np.linalg.norm(x[f + 1, [0, 2]] - x[f, [0, 2]])
            if h[f] < 8 and h[f + 1] < 8 and d > 8:
                slides.setdefault(s, {})[f] = max(slides.get(s, {}).get(f, 0), d)
print("feet sliding on the ground > 8 cm a frame:",
      "; ".join(f"{s} at {','.join(map(str, sorted(fd)))} up to {max(fd.values()):.0f} cm" for s, fd in slides.items()) or "none")
hp = pel[:, 1] - head[:, 1]
print(f"inverted (pelvis over head) at {runs([f for f in range(n) if hp[f] > 10])}; pelvis lowest {pel[:, 1].min() - floor:.0f} cm at {int(np.argmin(pel[:, 1]))}")
