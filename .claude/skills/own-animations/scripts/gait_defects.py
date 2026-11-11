"""Find the flaws of a walk or run loop, or of a start or stop, that the user's eye caught in clips - before a clip
is filmed. Every check is a number on BIP01 with a limit; a clip is judged beside a reference, Gothic's own loop.

    python3 .claude/skills/own-animations/scripts/gait_defects.py <base models> [--loop] [--stance REC] \
        [--ref LABEL=<clip.MAN.json>] LABEL=<clip.MAN.json> ...

`--loop`: the last frame is the first again, moved by the travel (it is dropped and the frames wrap). The floor is
the stance's toes (`--stance`, Gothic's S_RUN by default). The model looks along +Z, Y is up, centimetres.
A foot **stands** while its ankle moves under 1.5 cm a frame at 30 fps over the ground; it **rolls** while the ankle
moves and the toe does not; else it **swings**; the **reach** is the last 40 % of a swing. The **sole** is a stand from
the frame its toe has come down to the floor.

What is checked, the user's words it came from (reports/content/2026-11-hero-locomotion) and the limit:
- toe under floor: the toe node under the floor while the foot stands or rolls - «проваливается сквозь поверхность», 1 cm;
- foot floats: the standing ankle over the stance's own ankle - a foot that stands on its toes or in the air, 2 cm;
- toe off floor: the toe over the floor once the sole is down - a sole that does not lie flat, 1.5 cm;
- sole rocks: the range of the foot's pitch once the sole is down - «ступни болтаются», 6 degrees;
- foot slides / turns: the standing ankle's way on the ground, 2 cm, and the toe's sweep about the vertical, 6 degrees;
- toe up in reach: the toe's height and the foot's pitch over flat at the end of the swing - «тянет носок, как
  строевым шагом», 8 cm and 12 degrees;
- overreach: the ankle's furthest place ahead of the hips over its place at the strike (the leg is thrown out and
  pulled back), 4 cm; hang: frames at 30 fps the swinging ankle stays within 2 cm of that furthest place, 3;
- knee back: the least bend of the hip-knee-foot chain under the bind pose's 13.5 degrees (`knee_floor.py`), 3 under;
- knee snap: the largest change of the knee's bend in a frame at 30 fps - «колени дёргаются», 30 degrees;
- stiff knee: the standing knee's give, from its least bend to its most - «как в штаны насрал», at least 8 degrees;
- straight arm: the elbow's least bend - «руки вниз, как по стойке смирно», at least 10 degrees;
- hand pumps: the hand's travel up and down - «поднимает кулаки, как оловянный солдатик», 15 cm;
- joint snaps: the largest change of a joint's step from one frame to the next, at 30 fps - a foot or hand that
  jerks (a start squeezed in time threw the foot 27 cm in a frame; a run's strike is 13), 16 cm;
- seam (`--loop`): a joint's change of step over the seam beyond what it does inside the cycle anyway (its 95th
  percentile), 3 cm.
Each value is printed for every clip; a value over its limit is marked `!` and listed below with its frames. With
`--ref` a value is also marked when it is over the limit and the reference is not: the reference's own habits (Gothic's
walk bends the knee back for a frame) are printed, not hidden. The numbers start the list; the clip decides.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "scripts" / "content" / "anim"))
import gothic_anim as ga

LIMITS = {"toe under floor cm": 1.0, "foot floats cm": 2.0, "toe off floor cm": 1.5, "sole rocks °": 6.0, "foot slides cm": 2.0,
          "foot turns °": 6.0, "toe up in reach cm": 8.0, "toes up in reach °": 12.0, "overreach cm": 4.0,
          "hang frames": 3.0, "knee back °": 3.0, "knee snap °": 30.0, "stiff knee: give °": -8.0,
          "straight arm: elbow °": -10.0, "hand pumps cm": 15.0, "joint snaps cm": 16.0, "seam cm": 3.0}


def ang(u: np.ndarray, v: np.ndarray) -> np.ndarray:
    u = u / np.linalg.norm(u, axis=-1, keepdims=True)
    v = v / np.linalg.norm(v, axis=-1, keepdims=True)
    return np.degrees(np.arccos(np.clip((u * v).sum(-1), -1, 1)))


def spans(mask: np.ndarray, wrap: bool) -> list[list[int]]:
    """Runs of true frames as frame lists; with `wrap` a run over the seam is one run."""
    n = len(mask)
    if mask.all():
        return [list(range(n))]
    out, start = [], 0
    if wrap:
        while mask[start]:
            start += 1
    f = 0
    while f < n:
        i = (start + f) % n if wrap else f
        if mask[i]:
            run = []
            while f < n and mask[(start + f) % n if wrap else f]:
                run.append((start + f) % n if wrap else f)
                f += 1
            out.append(run)
        else:
            f += 1
    return out


def check(skel: ga.Skeleton, path: str, floor: float, ankle: float, bind_knee: float, flat: float, loop: bool) -> tuple[dict, dict]:
    idx = {n.name: i for i, n in enumerate(skel.nodes)}
    a = ga.read_animation(path)
    n = a.frame_count - 1 if loop else a.frame_count
    P = np.array([ga.world_pose(skel, a, f)[:, :3, 3] for f in range(a.frame_count)])
    travel = P[-1, 0] - P[0, 0] if loop else np.zeros(3)
    rate = a.fps / 30.0
    j = lambda name: P[:n, idx[name]]

    def step(X):                                    # the way to the next frame, wrapping a loop over its seam
        nxt = np.r_[X[1:], [X[0] + travel]] if loop else np.r_[X[1:], [X[-1]]]
        return nxt - X

    val = {k: 0.0 for k in LIMITS}
    val["stiff knee: give °"], val["straight arm: elbow °"] = 1e9, 1e9
    at: dict[str, str] = {}

    def worst(key, v, where, low=False):
        if (v < val[key]) if low else (v > val[key]):
            val[key], at[key] = float(v), where

    for s in "LR":
        A, T = j(f"BIP01 {s} FOOT"), j(f"BIP01 {s} TOE0")
        H, K = j(f"BIP01 {s} THIGH"), j(f"BIP01 {s} CALF")
        vA = np.linalg.norm(step(A)[:, [0, 2]], axis=1) * rate
        vT = np.linalg.norm(step(T)[:, [0, 2]], axis=1) * rate
        d = T - A
        pitch = np.degrees(np.arctan2(d[:, 1], np.hypot(d[:, 0], d[:, 2]))) - flat      # + toes up
        yaw = np.degrees(np.unwrap(np.arctan2(d[:, 0], d[:, 2])))
        knee = ang(K - H, A - K)
        ahead = A[:, 2] - j("BIP01 PELVIS")[:, 2]
        stands = vA < 1.5                                 # the ankle does not move over the ground
        rolls = ~stands & (vT < 1.5)                      # the heel is up, the toe still lies where it was
        swings = ~stands & ~rolls
        for run in spans(stands, loop):
            f0 = f"{s} f{run[0]}-{run[-1]}"
            seam = [i < run[0] for i in run]              # the frames after a loop's seam: a cycle further on
            ground = A[run][:, [0, 2]] + np.outer(seam, travel[[0, 2]])
            worst("foot slides cm", np.linalg.norm(np.ptp(ground, axis=0)), f0)
            worst("foot turns °", np.ptp(np.degrees(np.unwrap(np.radians(yaw[run])))), f0)
            if len(run) >= 4 * rate:                      # a stand of a frame or two is a strike, not a stance
                worst("stiff knee: give °", np.ptp(knee[run]), f0, low=True)
            worst("foot floats cm", A[run, 1].min() - ankle, f0)
            # the sole: from the frame the toe has come down (a strike on the heel lands the toe a frame or two later)
            down = [k for k, i in enumerate(run) if T[i, 1] - floor < 1.5]
            sole = run[down[0]:] if down else run
            worst("toe off floor cm", (T[sole, 1] - floor).max(), f"{s} f{sole[int(np.argmax(T[sole, 1]))]}")
            worst("sole rocks °", np.ptp(pitch[sole]), f"{s} f{sole[0]}-{sole[-1]}")
        for run in spans(stands | rolls, loop):
            worst("toe under floor cm", (floor - T[run, 1]).max(), f"{s} f{run[int(np.argmin(T[run, 1]))]}")
        for run in spans(swings, loop):
            if len(run) < 3:
                continue
            reach = run[int(len(run) * 0.6):]
            worst("toe up in reach cm", (T[reach, 1] - floor).max(), f"{s} f{reach[int(np.argmax(T[reach, 1]))]}")
            worst("toes up in reach °", pitch[reach].max(), f"{s} f{reach[int(np.argmax(pitch[reach]))]}")
            far = int(np.argmax(ahead[run]))
            worst("overreach cm", ahead[run][far] - ahead[run[-1]], f"{s} f{run[far]}")
            near = (ahead[run] > ahead[run][far] - 2.0) & (A[run, 1] > A[:, 1].min() + 2.0)
            worst("hang frames", near.sum() / rate, f"{s} f{run[far]}")
        worst("knee back °", bind_knee - knee.min(), f"{s} f{int(np.argmin(knee))}")
        dk = np.abs((np.r_[knee[1:], knee[:1]] if loop else np.r_[knee[1:], knee[-1:]]) - knee) / rate
        worst("knee snap °", dk[:-1].max() if not loop else dk.max(), f"{s} f{int(np.argmax(dk))}")
        el = ang(j(f"BIP01 {s} FOREARM") - j(f"BIP01 {s} UPPERARM"), j(f"BIP01 {s} HAND") - j(f"BIP01 {s} FOREARM"))
        worst("straight arm: elbow °", el.min(), f"{s} f{int(np.argmin(el))}", low=True)
        hand = j(f"BIP01 {s} HAND")[:, 1] - j("BIP01 PELVIS")[:, 1]
        worst("hand pumps cm", np.ptp(hand), f"{s} hand")
    B = [i for i, nd in enumerate(skel.nodes) if nd.name.startswith("BIP01")]
    inner = np.linalg.norm(P[2:n, B] - 2 * P[1:n - 1, B] + P[:n - 2, B], axis=2) * rate * rate    # [n - 2, joints]
    f, k = np.unravel_index(int(np.argmax(inner)), inner.shape)
    val["joint snaps cm"], at["joint snaps cm"] = float(inner[f, k]), f"{skel.nodes[B[k]].name[6:]} f{f + 1}"
    if loop:
        X = np.concatenate([P[:n, B], P[:2, B] + travel])   # the cycle and two frames of the next one
        acc = np.linalg.norm(X[2:] - 2 * X[1:-1] + X[:-2], axis=2) * rate * rate     # [n, joints], centred on 1..n
        usual = np.percentile(acc[:n - 2], 95, axis=0)       # what each joint does inside the cycle anyway
        extra = (acc[n - 2:] - usual).max(axis=0)            # the two frames about the seam
        val["seam cm"], at["seam cm"] = float(max(extra.max(), 0.0)), skel.nodes[B[int(np.argmax(extra))]].name
    else:
        val["seam cm"] = 0.0
    for k in ("stiff knee: give °", "straight arm: elbow °"):
        if val[k] > 1e8:
            val[k] = 0.0
    return val, at


def over(key: str, v: float) -> bool:
    lim = LIMITS[key]
    return v < -lim if lim < 0 else v > lim


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("models", type=Path)
    ap.add_argument("clips", nargs="+", help="LABEL=<clip.MAN.json>")
    ap.add_argument("--ref", help="LABEL=<clip.MAN.json>: the reference loop, printed first")
    ap.add_argument("--stance", type=Path)
    ap.add_argument("--loop", action="store_true")
    a = ap.parse_args()
    skel = ga.read_skeleton(a.models / "HUMANS.MDH.json")
    idx = {n.name: i for i, n in enumerate(skel.nodes)}
    stance = ga.read_animation(a.stance or a.models / "HUMANS-S_RUN.MAN.json")
    S = ga.world_pose(skel, stance, 0)[:, :3, 3]
    floor = float(min(S[idx["BIP01 L TOE0"], 1], S[idx["BIP01 R TOE0"], 1]))
    bind = ga.world_pose(skel, None, 0)[:, :3, 3]
    bind_knee = float(min(ang(bind[idx[f"BIP01 {s} CALF"]] - bind[idx[f"BIP01 {s} THIGH"]],
                              bind[idx[f"BIP01 {s} FOOT"]] - bind[idx[f"BIP01 {s} CALF"]]) for s in "LR"))
    d = bind[idx["BIP01 L TOE0"]] - bind[idx["BIP01 L FOOT"]]
    flat = float(np.degrees(np.arctan2(d[1], np.hypot(d[0], d[2]))))
    items = ([a.ref] if a.ref else []) + a.clips
    ankle = float(min(S[idx["BIP01 L FOOT"], 1], S[idx["BIP01 R FOOT"], 1]))
    rows = [(it.partition("=")[0], *check(skel, it.partition("=")[2], floor, ankle, bind_knee, flat, a.loop)) for it in items]
    ref = rows[0][1] if a.ref else None
    width = max(len(k) for k in LIMITS) + 9
    print(" " * width + "".join(f"{label:>11}" for label, _, _ in rows))
    found = []
    for k, lim in LIMITS.items():
        cells = ""
        for i, (label, val, at) in enumerate(rows):
            bad = over(k, val[k])
            mark = "!" if bad and not (a.ref and i == 0) else ("~" if bad else " ")
            cells += f"{val[k]:>10.1f}{mark}"
            if mark == "!":
                found.append((label, k, val[k], at.get(k, ""), ref is not None and over(k, ref[k])))
        print(f"{k + ' (' + ('min ' if lim < 0 else 'max ') + f'{abs(lim):g})':<{width}}" + cells)
    print()
    for label in dict.fromkeys(f[0] for f in found):
        print(f"{label}:")
        for _, k, v, where, also in (f for f in found if f[0] == label):
            print(f"  {k} {v:.1f} at {where}" + (" (the reference too)" if also else ""))
    if not found:
        print("nothing over the limits")


if __name__ == "__main__":
    main()
