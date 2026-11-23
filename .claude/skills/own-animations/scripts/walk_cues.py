"""What makes a walk loop read as weighty and a man's own rather than a passer-by's or a model's, on BIP01.

    python3 .claude/skills/own-animations/scripts/walk_cues.py <base models> LABEL=<loop.MAN.json> ...

One cycle of each loop (the last frame is the first again, moved by the travel); the model looks along +Z, Y is up,
centimetres. The cues come from reports/content/2026-11-hero-locomotion, «Ходьба отвергнута»:
- steps a minute, the step's length and frames a step at 30 fps: 120 a minute is an average walk, 100 a deliberate
  one (Theodore), 90 a stroll (Williams);
- feet apart: the sideways distance between the ankles, each taken while it stands; legs "well apart" read as a
  man, feet on one line as a woman or a model (Williams);
- arm off the body: the upper arm's angle off the trunk's down axis, seen from the front, mean of both arms - the
  strongest measured cue of a dominant walk (10.9 +- 3.7 degrees in 52 men, r = 0.67), and the hand's distance
  outside its hip joint;
- trunk sway: the chest's sideways travel about the path, mm (14.6 mean, 5-26, r = 0.51);
- bob: the pelvis' up-and-down travel - "the up and down of your masses gives the feeling of weight";
- turn of the hips and of the shoulders about the vertical (ranges) and how they go together: -1 is full opposition;
- shoulder roll: the range of the shoulder line's tilt seen from the front (a swagger is the shoulders', not the hips').
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "scripts" / "content" / "anim"))
import gothic_anim as ga


def cues(skel: ga.Skeleton, path: str) -> dict[str, float]:
    idx = {n.name: i for i, n in enumerate(skel.nodes)}
    a = ga.read_animation(path)
    n = a.frame_count - 1
    P = np.array([ga.world_pose(skel, a, f)[:, :3, 3] for f in range(n + 1)])
    travel = P[n, 0, 2] - P[0, 0, 2]
    drift = np.linspace(0.0, P[n, 0, 0] - P[0, 0, 0], n + 1)
    P[:, :, 0] -= drift[:, None]
    P = P[:n]
    j = lambda name: P[:, idx[name]]
    T = n / a.fps
    out = {"steps/min": 120.0 / T, "step cm": travel / 2, "frames/step @30": T * 30 / 2, "speed cm/s": travel / T}
    stand = {}
    for s in "LR":
        A = j(f"BIP01 {s} FOOT")
        stand[s] = A[A[:, 1] < A[:, 1].min() + 3.0][:, 0].mean()
    out["feet apart cm"] = abs(stand["L"] - stand["R"])
    mid = (j("BIP01 L THIGH") + j("BIP01 R THIGH")) / 2
    down = mid - j("BIP01 NECK")
    ab, hand = [], []
    for s, sign in (("L", 1.0), ("R", -1.0)):
        arm = j(f"BIP01 {s} FOREARM") - j(f"BIP01 {s} UPPERARM")
        side = np.sign((j(f"BIP01 {s} UPPERARM") - j("BIP01 NECK"))[:, 0].mean())
        ang = np.degrees(np.arctan2(arm[:, 0], -arm[:, 1]) - np.arctan2(down[:, 0], -down[:, 1])) * side
        ab.append(ang.mean())
        hand.append(((j(f"BIP01 {s} HAND") - j(f"BIP01 {s} THIGH"))[:, 0] * side).mean())
    out["arm off body °"] = float(np.mean(ab))
    out["hand outside hip cm"] = float(np.mean(hand))
    out["trunk sway mm"] = float(np.ptp(j("BIP01 SPINE2")[:, 0]) * 10)
    out["pelvis sway mm"] = float(np.ptp(mid[:, 0]) * 10)
    out["bob cm"] = float(np.ptp(mid[:, 1]))
    hips = j("BIP01 L THIGH") - j("BIP01 R THIGH")
    sh = j("BIP01 L UPPERARM") - j("BIP01 R UPPERARM")
    yaw = lambda v: np.degrees(np.unwrap(np.arctan2(v[:, 2], v[:, 0])))
    roll = lambda v: np.degrees(np.arctan2(v[:, 1], np.hypot(v[:, 0], v[:, 2])))
    hy, sy = yaw(hips), yaw(sh)
    out["hips turn °"], out["shoulders turn °"] = float(np.ptp(hy)), float(np.ptp(sy))
    out["hips x shoulders"] = float(np.corrcoef(hy, sy)[0, 1])
    out["hips roll °"], out["shoulder roll °"] = float(np.ptp(roll(hips))), float(np.ptp(roll(sh)))
    return out


def main():
    skel = ga.read_skeleton(Path(sys.argv[1]) / "HUMANS.MDH.json")
    rows = [(item.partition("=")[0], cues(skel, item.partition("=")[2])) for item in sys.argv[2:]]
    keys = list(rows[0][1])
    width = max(len(k) for k in keys) + 2
    print(" " * width + "".join(f"{label:>10}" for label, _ in rows))
    for k in keys:
        print(f"{k:<{width}}" + "".join(f"{r[k]:>10.1f}" if abs(r[k]) >= 10 or "x" not in k else f"{r[k]:>10.2f}" for _, r in rows))


if __name__ == "__main__":
    main()
