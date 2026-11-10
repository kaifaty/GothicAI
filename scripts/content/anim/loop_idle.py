"""Cut a standing loop (a stance such as S_RUN) out of a retargeted Kimodo move that stays in place.

    python3 scripts/content/anim/loop_idle.py <in.MAN.json> <models dir> <out.MAN.json> --name S_RUN \
        [--ref HUMANS-S_RUN.MAN.json] [--onto] [--window 0.5,9.7] [--length 5,9.5] [--frames a,b] [--pin 4] \
        [--report out.json]

The engine keeps a character's root node on the character's own place: every frame it takes the root's XZ out of the
pose (`Pose::mkBaseTranslation`), and a record whose root leaves its first frame by more than 0.4 cm moves the
character itself (`AnimData::setupMoveTr`), camera and collision with it. So a stance must not carry its sway in the
root. Gothic's own stances keep the root still and move `BIP01 PELVIS` under it; this script writes the loop the same
way.

1. Of all frame pairs (a, b) in the window that are `--length` seconds apart, the one whose poses and velocities
   differ least is the cycle (`--frames a,b` names it instead).
2. The seam's leftover - every node's local rotation and offset at the end against the start - is spread over the
   cycle with a smooth ramp; the last frame is the first again.
3. The cycle is turned about the vertical so that the shoulders face +Z on average: the character looks where the
   game aims it.
4. The root is put where `--ref` (Gothic's stance) has it against the ankles' midpoint and stays there; what it did
   goes into the pelvis node.
5. With `--onto` the cycle keeps only its motion: every node's turn about its own mean over the cycle (seen in its
   parent's axes) and the pelvis' sway about its mean are carried onto the pose of `--ref`'s first frame. The loop then
   holds Gothic's stance exactly and moves as the sample does, so the clips of the base that start from the stance or
   end in it meet it as they meet Gothic's own; Kimodo's standing habits (arched back, clavicles pulled back, bent
   wrists, toes out) go with its mean pose. The feet are then pinned where the reference has them.
6. A foot that stays within `--pin` cm over the whole cycle is pinned to its mean place and turn, and its leg is
   solved to it (two-bone IK, the knee stays in its plane): the ramp and Kimodo's own drift would creep it over the
   ground. A foot that steps is left alone and reported.

Prints and writes (`--report`) the seam before and after, the turn, and each foot's travel before and after.
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fix_layer as fl
import gothic_anim as ga
from loop_cycle import angle, qinv, qmul, qpow, world

FEET = {"L leg": "BIP01 L FOOT", "R leg": "BIP01 R FOOT"}


def seam_cost(P: np.ndarray, a: int, b: int) -> float:
    """Pose and velocity difference between frames a and b over every node (cm)."""
    dp = np.abs(P[b] - P[a]).max()
    dv = np.abs((P[b + 1] - P[b]) - (P[a + 1] - P[a])).max() if b + 1 < len(P) else 0.0
    return float(dp + 4.0 * dv)


def mean_rotation(Rs: np.ndarray) -> np.ndarray:
    """The mean of rotations that stay close to each other."""
    qs = np.array([fl.quat(R) for R in Rs])
    qs[(qs @ qs[0]) < 0] *= -1
    q = qs.mean(0)
    return ga.quat_matrix(q / np.linalg.norm(q))


def travel(P: np.ndarray) -> float:
    return float(np.linalg.norm(np.ptp(P, axis=0)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src", type=Path)
    ap.add_argument("models", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--name", required=True)
    ap.add_argument("--ref", type=Path, help="the stance whose root place against the ankles the loop takes; default "
                                             "HUMANS-S_RUN.MAN.json of the models dir")
    ap.add_argument("--onto", action="store_true", help="keep the cycle's motion only and carry it onto --ref's pose")
    ap.add_argument("--window", default="0.5,9.7", help="seconds of the source the cycle may come from")
    ap.add_argument("--length", default="5,9.5", help="shortest and longest cycle, seconds")
    ap.add_argument("--frames", help="a,b: the cycle's first and last source frames, instead of the search")
    ap.add_argument("--pin", type=float, default=4.0, help="cm: a foot that travels less over the cycle is pinned")
    ap.add_argument("--report", type=Path)
    a = ap.parse_args()

    skel = ga.read_skeleton(a.models / "HUMANS.MDH.json")
    idx = {n.name: i for i, n in enumerate(skel.nodes)}
    src = ga.read_animation(a.src)
    if src.node_indices[:2] != [idx["BIP01"], idx["BIP01 PELVIS"]]:
        raise SystemExit("the record must animate BIP01 and BIP01 PELVIS first")
    fps = src.fps
    P = world(skel, src)[:, :, :3, 3]
    if a.frames:
        s, e = (int(x) for x in a.frames.split(","))
        cost = seam_cost(P, s, e)
    else:
        t0, t1 = (float(x) for x in a.window.split(","))
        l0, l1 = (float(x) for x in a.length.split(","))
        f0, f1 = int(t0 * fps), min(src.frame_count - 2, int(t1 * fps))
        best = None
        for s in range(f0, f1):
            for e in range(s + int(l0 * fps), min(f1, s + int(l1 * fps)) + 1):
                c = seam_cost(P, s, e)
                if best is None or c < best[0]:
                    best = (c, s, e)
        if best is None:
            raise SystemExit("no cycle of that length fits the window")
        cost, s, e = best
    n = e - s

    pos = src.positions[s:e + 1].copy()
    rot = src.rotations[s:e + 1].copy()
    for f in range(1, len(rot)):
        flip = (rot[f] * rot[f - 1]).sum(-1) < 0
        rot[f][flip] *= -1
    seam_deg = float(angle(qmul(rot[n], qinv(rot[0]))).max())
    seam_cm = float(np.abs(P[e] - P[s]).max())
    feet_before = {limb: travel(P[s:e + 1, idx[foot]]) for limb, foot in FEET.items()}

    ramp = 0.5 - 0.5 * np.cos(np.pi * np.arange(n + 1) / n)          # 0..1, flat at both ends
    rot = qmul(rot, qpow(qmul(qinv(rot[n]), rot[0])[None], ramp[:, None]))
    pos += ramp[:, None, None] * (pos[0] - pos[n])[None]
    pos[n], rot[n] = pos[0], rot[0]
    loop = dataclasses.replace(src, name=a.name.upper(), next=a.name.upper(), frame_count=n + 1, positions=pos,
                               rotations=rot, events=[])

    # face +Z: the shoulders' line, right minus left, should run along +X on average
    W = world(skel, loop)
    line = (W[:n, idx["BIP01 R UPPERARM"], :3, 3] - W[:n, idx["BIP01 L UPPERARM"], :3, 3]).mean(0)
    yaw = 0.0
    for sign in (1.0, -1.0):
        th = sign * np.arctan2(line[2], line[0])
        qy = np.array([np.cos(th / 2), 0.0, np.sin(th / 2), 0.0])
        R = ga.quat_matrix(qy)
        turned = R @ line
        if abs(turned[2]) < 1e-6 * max(1.0, abs(line[0])) and turned[0] > 0:
            loop.positions[:, 0] = (R @ loop.positions[:, 0].T).T
            loop.rotations[:, 0] = qmul(qy[None], loop.rotations[:, 0])
            yaw = float(np.degrees(th))
            break

    # the root stands still where the reference stance has it against the ankles; the pelvis node carries the rest
    ref = ga.read_animation(a.ref or a.models / "HUMANS-S_RUN.MAN.json")
    Wr = ga.world_pose(skel, ref, 0)
    ref_mid = (Wr[idx["BIP01 L FOOT"]][:3, 3] + Wr[idx["BIP01 R FOOT"]][:3, 3]) / 2 - Wr[0][:3, 3]
    W = world(skel, loop)
    mid = ((W[:n, idx["BIP01 L FOOT"], :3, 3] + W[:n, idx["BIP01 R FOOT"], :3, 3]) / 2).mean(0)
    still = np.array([mid[0] - ref_mid[0], loop.positions[:n, 0, 1].mean(), mid[2] - ref_mid[2]])
    for f in range(n + 1):
        Rroot = ga.quat_matrix(loop.rotations[f, 0])
        loop.positions[f, 1] += Rroot.T @ (loop.positions[f, 0] - still)
    loop.positions[:, 0] = np.array([Wr[0][0, 3], still[1], Wr[0][2, 3]])

    if a.onto:
        if sorted(loop.node_indices) != sorted(ref.node_indices):
            raise SystemExit("--onto: the record and the reference animate different nodes")
        cols = [ref.node_indices.index(node) for node in loop.node_indices]
        mean = loop.rotations[:n].mean(0)                             # the frames are sign-continuous and close
        mean /= np.linalg.norm(mean, axis=-1, keepdims=True)
        loop.rotations = qmul(qmul(loop.rotations, qinv(mean)[None]), ref.rotations[0, cols][None])
        sway = loop.positions[:, 1] - loop.positions[:n, 1].mean(0)
        loop.positions[:] = ref.positions[0, cols][None]
        loop.positions[:, 1] += sway

    # pin the feet that stand
    clip = fl.Clip(skel, loop)
    W = world(skel, loop)
    pinned = {}
    for limb, foot in FEET.items():
        i = idx[foot]
        if travel(W[:n, i, :3, 3]) >= a.pin:
            continue
        target, turn = W[:n, i, :3, 3].mean(0), mean_rotation(W[:n, i, :3, :3])
        if a.onto:
            target, turn = Wr[i][:3, 3], Wr[i][:3, :3]
        for f in range(n + 1):
            Wf = clip.world(f)
            fl.two_bone(clip, f, Wf, limb, target, turn)
        pinned[limb] = True
    loop.positions[n], loop.rotations[n] = loop.positions[0], loop.rotations[0]
    ga.write_animation(a.out, loop, f"{a.src.name} frames {s}..{e}, standing loop")

    Wl = world(skel, loop)
    Pl = Wl[:, :, :3, 3]
    rep = {
        "source": str(a.src), "frames": [s, e], "cycle_frames": n, "fps": fps, "duration_s": round(n / fps, 3),
        "seam_cost_cm": round(cost, 2), "seam_before": {"deg": round(seam_deg, 2), "cm": round(seam_cm, 2)},
        "seam_after_cm": round(float(np.abs(Pl[n] - Pl[0]).max()), 4),
        "first_step_cm": round(float(np.abs(Pl[1] - Pl[0]).max()), 3),
        "median_step_cm": round(float(np.median(np.abs(Pl[1:] - Pl[:-1]).max(axis=(1, 2)))), 3),
        "yaw_deg": round(yaw, 2), "root_travel_cm": round(travel(Pl[:, 0]), 4),
        "feet": {limb: {"travel_before_cm": round(feet_before[limb], 2),
                        "travel_after_cm": round(travel(Pl[:, idx[foot]]), 3), "pinned": limb in pinned}
                 for limb, foot in FEET.items()},
    }
    print(json.dumps(rep, indent=1))
    if a.report:
        a.report.write_text(json.dumps(rep, indent=1))


if __name__ == "__main__":
    main()
