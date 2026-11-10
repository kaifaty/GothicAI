"""How a body is carried in a run, its start and its stops on BIP01: how low, how far it leans, knees and arms.

    python3 .claude/skills/own-animations/scripts/posture_cues.py <base models> [--series] LABEL=<clip.MAN.json>[:a,b] ...

Give the stance (S_RUN) as one of the clips: the run is read against it. The model looks along +Z, Y is up, cm.
A table per clip, or with --series every frame of each clip (speed, pelvis height, lean, knees, elbows, arms):
- pelvis y: a runner is lower than he stands on the whole cycle (about 9-12 cm at mid-stance) and never above;
  Gothic's run and Kimodo's samples peak above the stance - the body reads as carried too high;
- lean: the trunk (pelvis -> neck) from the vertical, + forward. Runners at 3.4 m/s: 7.3 ± 3.6°; a start leans
  with its acceleration (about 5.8° per m/s², Gothic's own start peaks at 16°), a braking step leans 3° back;
- knee at strike and at mid-stance (0 = straight): runners 18° and 46°;
- elbow flex (0 = straight): about 90° in a run; 115-137° with the hands 21-40 cm over the pelvis is a boxer's
  carriage, fists at the chest (rejected 2026-11-10);
- upper arm from hanging down (+ forward), hands over the pelvis, ahead of the shoulder and off the body's axis.
Sources: reports/content/2026-11-hero-locomotion, «Бег отвергнут: осанка и разгон».
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "scripts" / "content" / "anim"))
import gothic_anim as ga


def ang(u, v):
    u = u / np.linalg.norm(u, axis=-1, keepdims=True); v = v / np.linalg.norm(v, axis=-1, keepdims=True)
    return np.degrees(np.arccos(np.clip((u * v).sum(-1), -1, 1)))

def measure(skel, path, win=None):
    idx = {n.name: i for i, n in enumerate(skel.nodes)}
    a = ga.read_animation(Path(path))
    f0, f1 = win if win else (0, a.frame_count - 1)
    P = np.array([ga.world_pose(skel, a, f)[:, :3, 3] for f in range(f0, f1 + 1)])
    g = lambda n: P[:, idx["BIP01 " + n] if n else idx["BIP01"]]
    out = {"fps": a.fps, "n": len(P)}
    root = g("")
    out["speed"] = np.concatenate([[np.nan], np.diff(root[:, 2]) * a.fps])
    out["pelvis_y"] = g("PELVIS")[:, 1]
    out["head_y"] = g("HEAD")[:, 1]
    up = g("NECK") - g("PELVIS")
    out["lean"] = np.degrees(np.arctan2(up[:, 2], up[:, 1]))
    for s in "LR":
        th, ca, fo = g(f"{s} THIGH"), g(f"{s} CALF"), g(f"{s} FOOT")
        out[f"knee{s}"] = 180 - ang(th - ca, fo - ca)
        ua, fa, ha = g(f"{s} UPPERARM"), g(f"{s} FOREARM"), g(f"{s} HAND")
        out[f"elbow{s}"] = 180 - ang(ua - fa, ha - fa)
        out[f"hand{s}_y"] = ha[:, 1] - g("PELVIS")[:, 1]
        out[f"hand{s}_z"] = ha[:, 2] - ua[:, 2]
        out[f"hand{s}_x"] = np.abs(ha[:, 0] - root[:, 0])
        d = fa - ua
        out[f"arm{s}"] = np.degrees(np.arctan2(d[:, 2], -d[:, 1]))   # upper arm from hanging down, + forward
        out[f"foot{s}_y"] = fo[:, 1]; out[f"foot{s}_z"] = fo[:, 2] - g("PELVIS")[:, 2]
    return out

def main():
    args = sys.argv[1:]
    series = "--series" in args
    args = [x for x in args if x != "--series"]
    skel = ga.read_skeleton(Path(args[0]) / "HUMANS.MDH.json")
    rows = []
    for item in args[1:]:
        label, _, spec = item.partition("=")
        path, _, w = spec.partition(":")
        m = measure(skel, path, tuple(int(x) for x in w.split(",")) if w else None)
        if series:
            print(label)
            print("  f  speed pelvis  lean  kneeL kneeR elbL elbR  armL armR handLy handRy")
            for f in range(m["n"]):
                print(f"{f:3d} {m['speed'][f]:6.0f} {m['pelvis_y'][f]:6.1f} {m['lean'][f]:5.1f}  {m['kneeL'][f]:5.0f} {m['kneeR'][f]:5.0f} {m['elbowL'][f]:4.0f} {m['elbowR'][f]:4.0f} {m['armL'][f]:5.0f} {m['armR'][f]:4.0f} {m['handL_y'][f]:6.1f} {m['handR_y'][f]:6.1f}")
            continue
        r = {}
        r["pelvis y mean"] = m["pelvis_y"].mean(); r["pelvis y min"] = m["pelvis_y"].min(); r["pelvis y max"] = m["pelvis_y"].max()
        r["head y mean"] = m["head_y"].mean()
        r["lean mean"] = m["lean"].mean(); r["lean min"] = m["lean"].min(); r["lean max"] = m["lean"].max()
        # support knee: the knee of the foot that is low and under the pelvis
        sup = []
        strike = []
        for s in "LR":
            low = m[f"foot{s}_y"] < m[f"foot{s}_y"].min() + 4
            if low.any():
                k = m[f"knee{s}"][low]; z = m[f"foot{s}_z"][low]
                sup.append(k[np.argmin(np.abs(z))]); strike.append(k[np.argmax(z)])
        r["knee midstance"] = np.mean(sup); r["knee at strike"] = np.mean(strike)
        r["knee max"] = max(m["kneeL"].max(), m["kneeR"].max())
        e = np.concatenate([m["elbowL"], m["elbowR"]])
        r["elbow flex mean"] = e.mean(); r["elbow flex min"] = e.min(); r["elbow flex max"] = e.max()
        arm = np.concatenate([m["armL"], m["armR"]])
        r["upper arm back"] = arm.min(); r["upper arm fwd"] = arm.max()
        hy = np.concatenate([m["handL_y"], m["handR_y"]])
        r["hand over pelvis min"] = hy.min(); r["hand over pelvis max"] = hy.max()
        hz = np.concatenate([m["handL_z"], m["handR_z"]])
        r["hand vs shoulder back"] = hz.min(); r["hand vs shoulder fwd"] = hz.max()
        r["hand off axis mean"] = np.concatenate([m["handL_x"], m["handR_x"]]).mean()
        rows.append((label, r))
    if rows:
        keys = list(rows[0][1]); w = max(map(len, keys)) + 2
        print(" " * w + "".join(f"{l:>11}" for l, _ in rows))
        for k in keys:
            print(f"{k:<{w}}" + "".join(f"{r[k]:>11.1f}" for _, r in rows))
if __name__ == "__main__":
    main()
