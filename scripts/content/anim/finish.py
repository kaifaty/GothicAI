"""Finish a retargeted move: retime it, hold the sword arm, write the MAN record.

    python3 scripts/content/anim/finish.py <in.MAN.json> <out.MAN.json> --ref <stance.MAN.json> \
        --name T_1HDASHB --warp 0:0,0.24:0.06,0.88:0.40,1.16:0.52 --sword-arm 0.5,0.8,1,1

--warp maps source seconds to target seconds at a few keys (monotone, smooth between them): the
start, the active phase and the recovery are squeezed by different amounts. The clip ends at the last
key. --sword-arm blends the right clavicle, upper arm, forearm and hand towards the stance's local
rotations (0 keeps Kimodo's, 1 is the stance's); the finger always comes from the stance, since it
holds the hilt. Kimodo knows nothing of the sword, so its wrist swings the blade through the body.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gothic_anim as ga  # noqa: E402

ARM = ["BIP01 R CLAVICLE", "BIP01 R UPPERARM", "BIP01 R FOREARM", "BIP01 R HAND"]
FINGER = "BIP01 R FINGER0"


def monotone_cubic(xs: np.ndarray, ys: np.ndarray, x: np.ndarray) -> np.ndarray:
    """Fritsch–Carlson interpolation: no overshoot, so time never runs backwards."""
    h = np.diff(xs)
    d = np.diff(ys) / h
    m = np.empty_like(ys)
    m[0], m[-1] = d[0], d[-1]
    m[1:-1] = np.where(d[:-1] * d[1:] > 0, 3 * (h[:-1] + h[1:]) / ((2 * h[1:] + h[:-1]) / d[:-1] + (h[1:] + 2 * h[:-1]) / d[1:]), 0)
    i = np.clip(np.searchsorted(xs, x) - 1, 0, len(h) - 1)
    t = (x - xs[i]) / h[i]
    h00, h10, h01, h11 = 2 * t**3 - 3 * t**2 + 1, t**3 - 2 * t**2 + t, -2 * t**3 + 3 * t**2, t**3 - t**2
    return h00 * ys[i] + h10 * h[i] * m[i] + h01 * ys[i + 1] + h11 * h[i] * m[i + 1]


def slerp(a: np.ndarray, b: np.ndarray, t: np.ndarray) -> np.ndarray:
    """Per-row slerp of quaternions a, b [..., 4] by t [...]."""
    dot = (a * b).sum(-1)
    b = np.where(dot[..., None] < 0, -b, b)
    dot = np.abs(dot).clip(-1, 1)
    om = np.arccos(dot)
    so = np.sin(om)
    small = so < 1e-6
    t = np.asarray(t, dtype=np.float64)
    wa = np.where(small, 1 - t, np.sin((1 - t) * om) / np.where(small, 1, so))
    wb = np.where(small, t, np.sin(t * om) / np.where(small, 1, so))
    q = wa[..., None] * a + wb[..., None] * b
    return q / np.linalg.norm(q, axis=-1, keepdims=True)


def retime(anim: ga.Animation, keys: list[tuple[float, float]]) -> ga.Animation:
    src = np.array([k[0] for k in keys]) * anim.fps
    dst = np.array([k[1] for k in keys])
    frames = int(round(dst[-1] * anim.fps)) + 1
    dst = dst * ((frames - 1) / anim.fps / dst[-1])  # the last frame lands on the last key
    s = monotone_cubic(dst, src, np.arange(frames) / anim.fps).clip(0, anim.frame_count - 1)
    i0 = np.floor(s).astype(int).clip(0, anim.frame_count - 2)
    f = s - i0
    pos = anim.positions[i0] * (1 - f)[:, None, None] + anim.positions[i0 + 1] * f[:, None, None]
    rot = slerp(anim.rotations[i0], anim.rotations[i0 + 1], np.broadcast_to(f[:, None], (frames, len(anim.node_indices))))
    return ga.Animation(anim.name, anim.next, anim.layer, anim.fps, frames, list(anim.node_indices), pos, rot)


def hold_arm(anim: ga.Animation, skel: ga.Skeleton, ref: ga.Animation, weights: list[float]) -> None:
    for name, w in [*zip(ARM, weights), (FINGER, 1.0)]:
        node = skel.index(name)
        if node not in anim.node_indices or node not in ref.node_indices:
            continue
        j, k = anim.node_indices.index(node), ref.node_indices.index(node)
        target = np.broadcast_to(ref.rotations[0, k], anim.rotations[:, j].shape)
        anim.rotations[:, j] = slerp(anim.rotations[:, j], target, np.full(anim.frame_count, w))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("src", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--ref", type=Path, required=True, help="stance clip: the sword arm's pose")
    ap.add_argument("--skeleton", type=Path, help="HUMANS.MDH.json; default next to --ref")
    ap.add_argument("--name", required=True)
    ap.add_argument("--next", default="")
    ap.add_argument("--warp", required=True, help="src:dst seconds, comma separated, from 0:0 to the end")
    ap.add_argument("--sword-arm", default="0.5,0.8,1,1", help="clavicle, upper arm, forearm, hand weights")
    a = ap.parse_args()
    skel = ga.read_skeleton(a.skeleton or a.ref.parent / "HUMANS.MDH.json")
    ref = ga.read_animation(a.ref)
    anim = retime(ga.read_animation(a.src), [tuple(map(float, k.split(":"))) for k in a.warp.split(",")])
    hold_arm(anim, skel, ref, [float(w) for w in a.sword_arm.split(",")])
    anim.name, anim.next = a.name, a.next
    a.out.parent.mkdir(parents=True, exist_ok=True)
    ga.write_animation(a.out, anim, source=f"finish:{a.src.name} warp {a.warp} arm {a.sword_arm}")
    tr = ga.root_travel(anim)
    print(f"{anim.name}: {anim.frame_count} frames = {anim.duration * 1000:.0f} ms, root travel "
          f"{np.round(tr[-1], 1).tolist()} cm")


if __name__ == "__main__":
    main()
