"""A transition between a stance and a moving loop on BIP01: how it meets both ends and what the feet do.

    python3 .claude/skills/own-animations/scripts/trans_cues.py <base models> [--from REC[:FRAME]] [--to REC[:FRAME]] \
        [--frames a,b] [--to-speed CM_S] LABEL=<clip.MAN.json> ...

A start (T_RUN_2_RUNL) begins on the stance and ends where the loop begins; a stop (T_RUNL_2_RUN, T_RUNR_2_RUN)
begins on a frame of the loop and ends on the stance. `--from` and `--to` name those frames (a record and a frame,
0 by default); `--frames` reads a window of a longer sample instead of the whole clip. The model looks along +Z,
Y is up, centimetres. Printed per clip:
- time and travel: the window's length, the root's way, when the root has come to rest (under 15 cm/s for good) and
  the way it made by then; for a start, when it first reaches `--to-speed`;
- the ends: the largest distance between the clip's joints and the reference frame's, both taken about their own
  roots (pose), and the same for one frame's step (velocity, cm a frame) - what the engine's blend has to hide;
- settle: the first frame from which the pose stays within 3 cm of the last one;
- feet: every stretch a foot stands (low and slower than 6 cm a frame), as L or R with its frames, the largest slide
  of a standing ankle, and how far the toes dip under the floor of the reference;
- the trunk's lean from the vertical, the largest forward and back, and the head's largest travel a frame.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "scripts" / "content" / "anim"))
import gothic_anim as ga

FEET = {"L": ("BIP01 L FOOT", "BIP01 L TOE0"), "R": ("BIP01 R FOOT", "BIP01 R TOE0")}


def frame_of(spec: str | None, skel) -> np.ndarray | None:
    """Joint positions of REC[:FRAME] and of the frame after it, about the root's place on the ground."""
    if not spec:
        return None
    path, _, frame = spec.partition(":")
    anim = ga.read_animation(Path(path))
    f = int(frame or 0)
    out = []
    for k in (f, min(f + 1, anim.frame_count - 1)):
        P = ga.world_pose(skel, anim, k)[:, :3, 3].copy()
        out.append(P)
    out[1][:, [0, 2]] -= out[0][0, [0, 2]]
    out[0][:, [0, 2]] -= out[0][0, [0, 2]]
    return np.array(out), anim.fps


def stands(low: np.ndarray, speed: np.ndarray) -> list[tuple[int, int]]:
    on = low & (speed < 6.0)
    runs, start = [], None
    for f, v in enumerate(on):
        if v and start is None:
            start = f
        if not v and start is not None:
            runs.append((start, f - 1))
            start = None
    if start is not None:
        runs.append((start, len(on) - 1))
    return [(a, b) for a, b in runs if b - a >= 1]


def cues(skel, anim, a: int, b: int, ref_from, ref_to, to_speed: float) -> dict:
    idx = {n.name: i for i, n in enumerate(skel.nodes)}
    W = np.array([ga.world_pose(skel, anim, f) for f in range(a, b + 1)])
    P = W[:, :, :3, 3]
    n, fps = len(P), anim.fps
    root = P[:, 0]
    way = root[:, 2] - root[0, 2]
    v = np.diff(way) * fps
    out = {"frames": n, "s": (n - 1) / fps, "travel cm": way[-1], "side cm": root[-1, 0] - root[0, 0]}
    moving = np.nonzero(np.abs(v) >= 15.0)[0]
    rest = int(moving[-1]) + 1 if len(moving) else 0
    out["rest s"] = rest / fps
    out["way at rest cm"] = way[min(rest, n - 1)]
    if to_speed:
        fast = np.nonzero(v >= to_speed * 0.95)[0]
        out["at speed s"] = (int(fast[0]) + 1) / fps if len(fast) else float("nan")
    rel = P - np.concatenate([root[:, :1], np.zeros((n, 1)), root[:, 2:]], axis=1)[:, None]
    for name, ref, f in (("start", ref_from, 0), ("end", ref_to, n - 1)):
        if ref is None:
            continue
        (R0, R1), rfps = ref
        out[f"{name} pose cm"] = float(np.linalg.norm(rel[f] - R0, axis=1).max())
        g = f if f + 1 < n else f - 1
        step = (P[g + 1] - P[g]) * (fps / 30.0)
        out[f"{name} step cm"] = float(np.linalg.norm(step - (R1 - R0) * (rfps / 30.0), axis=1).max())
    last = rel[-1]
    off = np.linalg.norm(rel - last, axis=2).max(1)
    far = np.nonzero(off > 3.0)[0]
    out["settle s"] = (int(far[-1]) + 1) / fps if len(far) else 0.0
    floor = min(P[-1, idx[FEET["L"][1]], 1], P[-1, idx[FEET["R"][1]], 1]) if ref_to is None else \
        min(ref_to[0][0][idx[FEET["L"][1]], 1], ref_to[0][0][idx[FEET["R"][1]], 1])
    if ref_from is not None and ref_to is None:
        floor = min(ref_from[0][0][idx[FEET["L"][1]], 1], ref_from[0][0][idx[FEET["R"][1]], 1])
    steps, slide, dip = [], 0.0, 0.0
    for s, (ankle, toe) in FEET.items():
        A, T = P[:, idx[ankle]], P[:, idx[toe]]
        sp = np.concatenate([[0.0], np.linalg.norm(np.diff(A, axis=0), axis=1) * fps / 30.0])
        low = A[:, 1] < A[:, 1].min() + 6.0
        for f0, f1 in stands(low, sp):
            steps.append((f0, f1, s))
            slide = max(slide, float(np.linalg.norm(np.ptp(A[f0:f1 + 1][:, [0, 2]], axis=0))))
        dip = max(dip, float(floor - T[:, 1].min()))
    out["stands"] = " ".join(f"{s}{f0}-{f1}" for f0, f1, s in sorted(steps))
    out["slide cm"] = slide
    out["toe dip cm"] = dip
    up = P[:, idx["BIP01 NECK"]] - P[:, idx["BIP01 PELVIS"]] if "BIP01 PELVIS" in idx else P[:, idx["BIP01 NECK"]] - root
    lean = np.degrees(np.arctan2(up[:, 2], up[:, 1]))
    out["lean fwd °"], out["lean back °"] = float(lean.max()), float(lean.min())
    head = P[:, idx["BIP01 HEAD"]] - root
    out["head jerk cm"] = float(np.linalg.norm(np.diff(head, axis=0), axis=1).max()) if n > 1 else 0.0
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("models", type=Path)
    ap.add_argument("clips", nargs="+", help="LABEL=<clip.MAN.json>")
    ap.add_argument("--from", dest="ref_from", help="REC[:FRAME] the clip starts on")
    ap.add_argument("--to", dest="ref_to", help="REC[:FRAME] the clip ends on")
    ap.add_argument("--frames", help="a,b: a window of the clip")
    ap.add_argument("--to-speed", type=float, default=0.0, help="cm/s of the loop a start leads into")
    a = ap.parse_args()
    skel = ga.read_skeleton(a.models / "HUMANS.MDH.json")
    ref_from, ref_to = frame_of(a.ref_from, skel), frame_of(a.ref_to, skel)
    rows = []
    for item in a.clips:
        label, _, path = item.partition("=")
        anim = ga.read_animation(Path(path))
        f0, f1 = (int(x) for x in a.frames.split(",")) if a.frames else (0, anim.frame_count - 1)
        rows.append((label, cues(skel, anim, f0, min(f1, anim.frame_count - 1), ref_from, ref_to, a.to_speed)))
    keys = list(rows[0][1])
    width = max(len(k) for k in keys) + 2
    print(" " * width + "".join(f"{label:>14}" for label, _ in rows))
    for k in keys:
        cells = "".join(f"{r[k]:>14}" if isinstance(r[k], str) else f"{r[k]:>14.2f}" for _, r in rows)
        print(f"{k:<{width}}" + cells)


if __name__ == "__main__":
    main()
