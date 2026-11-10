"""A runner's carriage laid over a clip pose by pose, the feet staying where they are: hips lower, the trunk leaning
from the hips, elbows open. For a run loop and for the start and stops that meet it.

    python3 scripts/content/anim/run_style.py <base models> <in.MAN.json> <out.MAN.json> [--loop]
        [--drop CM] [--lean DEG] [--elbow DEG] [--arm DEG] [--w f:w,f:w,...] [--wdrop f:w,...] [--bump centre,half,deg]

- `--drop`: the hips come down by CM and the knees bend to the same feet. A runner is lower than he stands on the
  whole cycle; Kimodo's runs carry the hips at standing height.
- `--lean`: the trunk tips forward by DEG about the hips, a third of it by the pelvis; the neck gives half of it
  back and the head four fifths, so the eyes stay ahead and the back stays straight.
- `--elbow`: the elbows open by DEG (never past 20° of bend); `--arm`: the upper arms come forward by DEG. The lean
  swings a hanging arm back by as much, so `--arm` of at least the lean keeps the hands where they travelled.
- `--w`: the share of all of it by frame, linear between the points, 1 everywhere by default. A start grows into
  the carriage (0:0,5:1), a stop lets it go (0:1,10:0). `--wdrop`: the same for the drop alone: a start that
  already squats to push off takes the drop only after the push.
- `--bump`: a bell of extra lean in degrees centred on a frame, `half` frames to each side; negative leans back.
  The lean of a body follows its acceleration (about 5.8° per m/s²): forward in a start, back in a stop.
- `--loop`: a moving loop - the last frame is the first again, moved by the travel.

The same numbers give the same pose from the same pose, so a start that ends on a frame of the loop and a stop that
begins on one still meet the styled loop when all three take the same --drop, --lean, --elbow and --arm with the
share 1 at that end. The root's way and the feet are not touched, so speed, travel and foot contacts stay.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fix_layer as fl
import gothic_anim as ga

X = [1.0, 0.0, 0.0]


def points(spec: str) -> list[tuple[float, float]]:
    return [tuple(float(x) for x in p.split(":")) for p in spec.split(",")] if spec else []


def share(pts: list[tuple[float, float]], f: int) -> float:
    return float(np.interp(f, [p[0] for p in pts], [p[1] for p in pts])) if pts else 1.0


def style(skel: ga.Skeleton, anim: ga.Animation, drop: float = 0.0, lean: float = 0.0, elbow: float = 0.0,
          arm: float = 0.0, w: list | None = None, wdrop: list | None = None, bumps: list | None = None,
          loop: bool = False):
    """Lay the carriage over `anim` in place. w, wdrop: [(frame, share), ...]; bumps: [(centre, half, deg), ...]."""
    clip = fl.Clip(skel, anim)
    ix = skel.index
    n = anim.frame_count
    travel = anim.positions[-1, 0] - anim.positions[0, 0]
    w, wdrop = w or [], wdrop or w or []
    for f in range(n - 1 if loop else n):
        k = share(w, f)
        extra = sum(d * 0.5 * (1 + np.cos(np.pi * (f - c) / h)) for c, h, d in bumps or [] if abs(f - c) < h)
        tilt = np.radians(lean * k + extra)
        W = clip.world(f)
        feet = {limb: (W[ix(fl.LIMBS[limb][2])][:3, 3].copy(), W[ix(fl.LIMBS[limb][2])][:3, :3].copy())
                for limb in ("L leg", "R leg")}
        head, neck = W[ix("BIP01 HEAD")][:3, :3].copy(), W[ix("BIP01 NECK")][:3, :3].copy()
        spine = W[ix("BIP01 SPINE")][:3, :3].copy()
        anim.positions[f, clip.col[0], 1] -= drop * share(wdrop, f)
        W = clip.world(f)
        clip.set_world_rot(f, W, "BIP01 PELVIS", fl.rot(X, tilt * 0.33) @ W[ix("BIP01 PELVIS")][:3, :3])
        clip.set_world_rot(f, W, "BIP01 SPINE", fl.rot(X, tilt) @ spine)
        clip.set_world_rot(f, W, "BIP01 NECK", fl.slerp_m(W[ix("BIP01 NECK")][:3, :3], neck, 0.5))
        clip.set_world_rot(f, W, "BIP01 HEAD", fl.slerp_m(W[ix("BIP01 HEAD")][:3, :3], head, 0.8))
        for side in "LR":
            names = fl.LIMBS[f"{side} arm"]
            up, fore, hand = (ix(b) for b in names)
            if arm:
                clip.set_world_rot(f, W, names[0], fl.rot(X, -np.radians(arm * k)) @ W[up][:3, :3])
            if elbow:
                u, v = W[fore][:3, 3] - W[up][:3, 3], W[hand][:3, 3] - W[fore][:3, 3]
                hinge = np.cross(u, v)
                if np.linalg.norm(hinge) > 1e-6:
                    bend = np.arccos(np.clip(u @ v / np.linalg.norm(u) / np.linalg.norm(v), -1, 1))
                    t = min(np.radians(elbow * k), max(0.0, bend - np.radians(20)))
                    clip.set_world_rot(f, W, names[1], fl.rot(hinge / np.linalg.norm(hinge), -t) @ W[fore][:3, :3])
        for limb, (at, turn) in feet.items():
            fl.two_bone(clip, f, W, limb, at, turn)
    if loop:
        anim.positions[-1], anim.rotations[-1] = anim.positions[0], anim.rotations[0]
        anim.positions[-1, 0] = anim.positions[0, 0] + travel
    for f in range(1, n):
        flip = (anim.rotations[f] * anim.rotations[f - 1]).sum(-1) < 0
        anim.rotations[f][flip] *= -1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("models", type=Path)
    ap.add_argument("src", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--loop", action="store_true")
    ap.add_argument("--drop", type=float, default=0.0)
    ap.add_argument("--lean", type=float, default=0.0)
    ap.add_argument("--elbow", type=float, default=0.0)
    ap.add_argument("--arm", type=float, default=0.0)
    ap.add_argument("--w", default="")
    ap.add_argument("--wdrop", default="")
    ap.add_argument("--bump", action="append", default=[])
    a = ap.parse_args()
    skel = ga.read_skeleton(a.models / "HUMANS.MDH.json")
    anim = ga.read_animation(a.src)
    style(skel, anim, a.drop, a.lean, a.elbow, a.arm, points(a.w), points(a.wdrop),
          [tuple(float(x) for x in b.split(",")) for b in a.bump], a.loop)
    ga.write_animation(a.out, anim, f"{a.src.name} + run_style drop {a.drop} lean {a.lean} elbow {a.elbow} "
                                    f"arm {a.arm} w {a.w} wdrop {a.wdrop} bump {a.bump}")


if __name__ == "__main__":
    main()
