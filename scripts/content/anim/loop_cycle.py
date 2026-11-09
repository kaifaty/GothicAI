"""Cut a locomotion loop out of a steady stretch of a retargeted move (walk, run) and close its seam.

    python3 scripts/content/anim/loop_cycle.py <in.MAN.json> <models dir> <out.MAN.json> --name S_RUNL \
        --speed 345 [--window 1.0,5.8] [--strides 1] [--start left] [--report out.json]

The engine plays a moving loop the way Gothic's own loops are made: the last frame is the first frame of the next
cycle, moved by the cycle's travel (`Animation::AnimData::setupMoveTr`). So the loop is written with N+1 frames,
frame N = frame 0 + travel, rotations equal.

1. The stride period comes from the autocorrelation of the toes' heights in the window.
2. Of all frame pairs (a, a + period ± 2) in the window, the one whose in-place poses and velocities differ least
   is the cycle (`--strides` periods long).
3. The seam's leftover - every node's local rotation and the root's in-place offset at the end against the start -
   is spread over the cycle with a smooth ramp, so the last frame is the first again and nothing jumps.
4. The cycle starts at the strike of the `--start` foot: the start and stop clips meet the loop there.
5. The frame rate is set so the cycle travels at exactly `--speed` cm/s: the frames stay Kimodo's, the time
   changes by the few percent the generator missed the speed by (feet stay planted; a stretched root would slide
   them).

Prints and writes (`--report`) the seam before and after, the speed, cadence, step and the strike frames, which
become the footstep sounds (`sfx_ground` RUN) of the script entry.
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gothic_anim as ga

TOES = ("BIP01 L TOE0", "BIP01 R TOE0")


def qmul(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    w1, x1, y1, z1 = np.moveaxis(a, -1, 0)
    w2, x2, y2, z2 = np.moveaxis(b, -1, 0)
    return np.stack([w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2, w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2,
                     w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2, w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2], -1)


def qinv(q: np.ndarray) -> np.ndarray:
    return q * np.array([1.0, -1.0, -1.0, -1.0])


def qpow(q: np.ndarray, t: np.ndarray) -> np.ndarray:
    """q^t for unit quaternions (shortest arc), t broadcast against q[..., 0]."""
    q = np.where(q[..., :1] < 0, -q, q)
    w = np.clip(q[..., 0], -1.0, 1.0)
    ang = np.arccos(w)
    s = np.sin(ang)
    axis = np.where(s[..., None] > 1e-9, q[..., 1:] / np.maximum(s, 1e-9)[..., None], 0.0)
    a = ang * t
    return np.concatenate([np.cos(a)[..., None], axis * np.sin(a)[..., None]], -1)


def angle(q: np.ndarray) -> np.ndarray:
    return np.degrees(2 * np.arccos(np.clip(np.abs(q[..., 0]), 0.0, 1.0)))


def world(skel: ga.Skeleton, anim: ga.Animation) -> np.ndarray:
    return np.array([ga.world_pose(skel, anim, f) for f in range(anim.frame_count)])


def in_place(W: np.ndarray) -> np.ndarray:
    """Node positions relative to the root, per frame (frames, nodes, 3)."""
    return W[:, :, :3, 3] - W[:, :1, :3, 3]


def toe_heights(W: np.ndarray, idx: dict) -> np.ndarray:
    """Per foot (left, right): how far the ankle or the toe, whichever is nearer, is above its own lowest height. A heel
    strike has the ankle down and the toe up, a forefoot strike the other way round."""
    out = []
    for t in TOES:
        toe, ank = W[:, idx[t], 1, 3], W[:, idx[t.replace("TOE0", "FOOT")], 1, 3]
        out.append(np.minimum(toe - np.percentile(toe, 3), ank - np.percentile(ank, 3)))
    return np.stack(out, 1)


def period(h: np.ndarray, fps: float) -> float:
    best = []
    for k in range(h.shape[1]):
        x = h[:, k] - h[:, k].mean()
        ac = np.correlate(x, x, "full")[len(x) - 1:]
        ac /= ac[0]
        lo, hi = int(0.4 * fps), min(len(ac) - 2, int(2.0 * fps))
        lag = lo + int(np.argmax(ac[lo:hi]))
        a, b, c = ac[lag - 1], ac[lag], ac[lag + 1]
        den = a - 2 * b + c
        best.append(lag + (0.5 * (a - c) / den if abs(den) > 1e-9 else 0.0))
    return float(np.mean(best))


def cycle_strikes(Wc: np.ndarray, idx: dict) -> list[list[int]]:
    """Strikes of a closed cycle (world matrices of n + 1 frames, the last one the first moved by the travel), frames
    0..n-1, left then right. A foot is down while its ankle or toe (whichever is slower) moves over the ground at under
    a third of the body's speed; a strike is where it comes down (heel and forefoot strikes alike). The cycle's
    per-frame speeds are periodic, so they are read over three passes and the middle pass counts."""
    n = len(Wc) - 1
    body = np.linalg.norm(np.diff(Wc[:, 0, :3, 3][:, [0, 2]], axis=0), axis=1).mean()
    out = []
    for t in TOES:
        sp = []
        for name in (t, t.replace("TOE0", "FOOT")):
            p = Wc[:, idx[name], :3, 3]
            sp.append(np.linalg.norm(np.diff(p[:, [0, 2]], axis=0), axis=1))
        v = np.minimum(*sp)                                   # n speeds, periodic
        down = np.tile(v < body / 3.0, 3)
        hits = [f for f in range(1, len(down)) if down[f] and not down[f - 1]]
        out.append(sorted({f - n for f in hits if n <= f < 2 * n}))
    return out

def seam_cost(P: np.ndarray, a: int, b: int) -> float:
    """In-place pose and velocity difference between frames a and b (cm)."""
    dp = np.abs(P[b] - P[a]).max()
    dv = np.abs((P[b + 1] - P[b]) - (P[a + 1] - P[a])).max() if b + 1 < len(P) else 0.0
    return float(dp + 2.0 * dv)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src", type=Path)
    ap.add_argument("models", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--name", required=True)
    ap.add_argument("--speed", type=float, required=True, help="cm/s the loop must travel")
    ap.add_argument("--window", default="1.0,5.8")
    ap.add_argument("--strides", type=int, default=1)
    ap.add_argument("--start", choices=("left", "right"), default="left")
    ap.add_argument("--report", type=Path)
    a = ap.parse_args()

    skel = ga.read_skeleton(a.models / "HUMANS.MDH.json")
    idx = {n.name: i for i, n in enumerate(skel.nodes)}
    src = ga.read_animation(a.src)
    if src.node_indices[0] != 0:
        raise SystemExit("the record does not animate the root: no travel to loop")
    fps = src.fps
    W = world(skel, src)
    P = in_place(W)
    h = toe_heights(W, idx)
    t0, t1 = (float(x) for x in a.window.split(","))
    f0, f1 = int(t0 * fps), min(src.frame_count - 2, int(t1 * fps))
    T = period(h[f0:f1], fps) * a.strides
    n0 = round(T)
    best = None
    for n in range(n0 - 2, n0 + 3):
        for s in range(f0, f1 - n):
            c = seam_cost(P, s, s + n)
            if best is None or c < best[0]:
                best = (c, s, n)
    cost, s, n = best

    # the cycle, frames s..s+n; rotate it to start at a strike of the chosen foot (in the extended range)
    st = cycle_strikes(W[s:s + n + 1], idx)
    foot = 0 if a.start == "left" else 1
    shift = st[foot][0] if st[foot] else 0

    pos = src.positions[s:s + n + 1].copy()
    rot = src.rotations[s:s + n + 1].copy()
    # make rotations continuous in sign
    for f in range(1, len(rot)):
        flip = (rot[f] * rot[f - 1]).sum(-1) < 0
        rot[f][flip] *= -1
    # turn the whole cycle about the vertical so it travels straight ahead (+Z): a rigid turn keeps the feet planted
    d = pos[n, 0] - pos[0, 0]
    yaw_deg = 0.0
    for sign in (1.0, -1.0):
        th = sign * np.arctan2(d[0], d[2])
        qy = np.array([np.cos(th / 2), 0.0, np.sin(th / 2), 0.0])
        R = ga.quat_matrix(qy)[:3, :3]
        if abs((R @ d)[0]) < 1e-3 * max(1.0, abs(d[2])):
            pos[:, 0] = (R @ (pos[:, 0] - pos[0, 0]).T).T + pos[0, 0]
            rot[:, 0] = qmul(qy[None], rot[:, 0])
            yaw_deg = float(np.degrees(th))
            break
    travel = pos[n, 0] - pos[0, 0]
    travel[1] = 0.0
    seam_before_deg = float(angle(qmul(rot[n], qinv(rot[0]))).max())
    seam_before_cm = float(np.abs(pos[n, 1:] - pos[0, 1:]).max())

    # spread the leftover: local rotations of every node, the root's height and sideways drift, other nodes' offsets
    ramp = 0.5 - 0.5 * np.cos(np.pi * np.arange(n + 1) / n)          # 0..1, flat at both ends
    err = qmul(qinv(rot[n]), rot[0])                                  # what the end needs to become the start
    rot = qmul(rot, qpow(err[None], ramp[:, None]))
    root_err = (pos[0, 0] + travel) - pos[n, 0]
    pos[:, 0] += ramp[:, None] * root_err[None]
    pos[:, 1:] += ramp[:, None, None] * (pos[0, 1:] - pos[n, 1:])[None]
    pos[n] = pos[0].copy()
    pos[n, 0] = pos[0, 0] + travel
    rot[n] = rot[0]

    if shift:
        body = np.concatenate([pos[shift:n], pos[:shift + 1]])
        body[n - shift:, 0] += travel
        rbody = np.concatenate([rot[shift:n], rot[:shift + 1]])
        pos, rot = body, rbody
    pos[:, 0] -= pos[0, 0] * np.array([1.0, 0.0, 1.0])               # start the root at x = z = 0, keep its height

    dist = float(np.linalg.norm(travel[[0, 2]]))
    speed_src = dist * fps / n
    out_fps = fps * a.speed / speed_src
    loop = dataclasses.replace(src, name=a.name.upper(), next=a.name.upper(), fps=float(out_fps), frame_count=n + 1,
                               positions=pos, rotations=rot, events=[])
    ga.write_animation(a.out, loop, f"{a.src.name} frames {s}..{s + n}, loop")

    # checks on the written loop
    Wl = world(skel, loop)
    Pl = in_place(Wl)
    st_l = cycle_strikes(Wl, idx)
    seam_after = float(np.abs(Pl[n] - Pl[0]).max())
    step_seam = float(np.abs(Pl[1] - Pl[0]).max())
    step_mid = float(np.median(np.abs(Pl[1:] - Pl[:-1]).max(axis=(1, 2))))
    cad = 120.0 * out_fps / n
    rep = {
        "source": str(a.src), "frames": [s, s + n], "cycle_frames": n, "fps": round(out_fps, 3),
        "duration_s": round(n / out_fps, 4), "speed_cm_s": round(dist * out_fps / n, 2),
        "time_scale": round(fps / out_fps, 4), "cadence_spm": round(cad, 1), "step_m": round(dist / 200.0, 3),
        "seam_cost_cm": round(cost, 2), "seam_before": {"deg": round(seam_before_deg, 2), "cm": round(seam_before_cm, 2)},
        "seam_after_cm": round(seam_after, 3), "first_step_cm": round(step_seam, 2), "median_step_cm": round(step_mid, 2),
        "strikes": {"left": st_l[0], "right": st_l[1]}, "start_shift": shift, "yaw_deg": round(yaw_deg, 2),
    }
    print(json.dumps(rep, indent=1))
    if a.report:
        a.report.write_text(json.dumps(rep, indent=1))


if __name__ == "__main__":
    main()
