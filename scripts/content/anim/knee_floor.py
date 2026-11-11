"""A knee that never bends backwards: the least bend of the knees of a clip on BIP01, laid over it pose by pose.

    python3 scripts/content/anim/knee_floor.py <base models> <in.MAN.json> <out.MAN.json> [--loop]
        [--floor DEG] [--soft DEG] [--report out.json]

BIP01's hip, knee and foot nodes do not lie on a line when the body's leg is straight: the foot node sits behind
the shin, and in the bind pose - a straight leg of the mesh - the chain is bent by 13.5 degrees at the knee. A chain
drawn straighter than that is a knee bent backwards. Gothic's own clips keep to it (the stance 13.5-14, the walk
down to 7 for a frame, the run 10); a retarget that puts the foot node where the source's ankle is does not: an
ankle out of reach pulls the chain into a line, and Kimodo's walks came out with the knee 10 degrees backwards for
a quarter of the cycle, at the end of the swing and at the heel strike (reports/content/2026-11-hero-locomotion,
«Колено назад»). On the hero's own body, whose render rig bends a leg on the ground only forwards, that knee
flicked at every step.

- `--floor`: the least bend of the chain, degrees; by default the bind pose's own.
- `--soft`: the bend comes to the floor smoothly over this many degrees above it (6): a knee bent by more than
  floor + soft is not touched, one at the floor ends soft / e above it, a straight chain on the floor.
- The foot keeps its turn and moves along the line from the hip, towards it: 0.6 cm at most for a chain that was a
  line, 0.4 cm for a standing leg of Kimodo's walk. The knee goes to the thigh's front (`fix_layer.two_bone`, guard):
  the nearer the chain was to a line - or past it - the more the hinge decides and the less the knee's own offset.
- `--loop`: a moving loop - the last frame is the first again, moved by the travel.

The same pose gives the same pose, so a start or a stop that meets a loop on one of its frames still meets it after
both took the pass. Lay it before `run_style.py`: that one keeps the feet and the hips' new place.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fix_layer as fl
import gothic_anim as ga

LEGS = ("L leg", "R leg")


def bend(W: np.ndarray, skel: ga.Skeleton, limb: str) -> float:
    """The knee's bend of `limb` in the pose W, degrees: 0 - hip, knee and foot node on a line; negative - the knee
    behind that line, seen from the thigh's front."""
    iu, im, ie = (skel.index(n) for n in fl.LIMBS[limb])
    H, K, A = W[iu][:3, 3], W[im][:3, 3], W[ie][:3, 3]
    a, b = K - H, A - K
    deg = float(np.degrees(np.arccos(np.clip(a @ b / np.linalg.norm(a) / np.linalg.norm(b), -1, 1))))
    u = (A - H) / max(np.linalg.norm(A - H), 1e-9)
    front = np.cross(W[iu][:3, :3] @ fl.UP, u)
    if front @ (W[iu][:3, :3] @ np.array([0.3, 0.0, 0.95])) < 0:
        front = -front
    return -deg if (a - (a @ u) * u) @ front < 0 else deg


def bind_bend(skel: ga.Skeleton) -> float:
    W = ga.world_pose(skel, None, 0)
    return min(bend(W, skel, limb) for limb in LEGS)


def raised(deg: float, floor: float, soft: float) -> float:
    """The bend with the floor under it: itself from floor + soft up, floor + soft * e^(x) below, smooth where they meet."""
    if deg >= floor + soft or soft <= 0:
        return max(deg, floor)
    return floor + soft * float(np.exp((deg - floor - soft) / soft))


def lay(skel: ga.Skeleton, anim: ga.Animation, floor: float, soft: float = 6.0, loop: bool = False) -> dict:
    """Raise the knees of `anim` to the floor in place. Returns what changed, per leg."""
    clip = fl.Clip(skel, anim)
    n = anim.frame_count
    travel = anim.positions[-1, 0] - anim.positions[0, 0]
    rep = {limb: {"frames": 0, "least_before": 180.0, "least_after": 180.0, "most_raise": 0.0, "foot_moved_cm": 0.0}
           for limb in LEGS}
    for f in range(n - 1 if loop else n):
        W = clip.world(f)
        for limb in LEGS:
            iu, im, ie = (skel.index(b) for b in fl.LIMBS[limb])
            was = bend(W, skel, limb)
            to = raised(was, floor, soft)
            r = rep[limb]
            r["least_before"] = min(r["least_before"], was)
            if to - was > 1e-3:
                H, K, A = W[iu][:3, 3].copy(), W[im][:3, 3].copy(), W[ie][:3, 3].copy()
                l1, l2 = np.linalg.norm(K - H), np.linalg.norm(A - K)
                d = float(np.sqrt(l1 * l1 + l2 * l2 + 2 * l1 * l2 * np.cos(np.radians(to))))
                u = (A - H) / np.linalg.norm(A - H)
                # the hinge decides the more, the nearer the chain was to a line (its own offset is noise there)
                guard = 3.0 + 60.0 * float(np.clip(1.0 - was / max(floor, 1e-6), 0.0, 1.0))
                fl.two_bone(clip, f, W, limb, H + u * d, W[ie][:3, :3].copy(), guard=guard)
                r["frames"] += 1
                r["most_raise"] = max(r["most_raise"], to - was)
                r["foot_moved_cm"] = max(r["foot_moved_cm"], float(np.linalg.norm(W[ie][:3, 3] - A)))
            r["least_after"] = min(r["least_after"], bend(W, skel, limb))
    if loop:
        anim.positions[-1], anim.rotations[-1] = anim.positions[0], anim.rotations[0]
        anim.positions[-1, 0] = anim.positions[0, 0] + travel
    for f in range(1, n):
        flip = (anim.rotations[f] * anim.rotations[f - 1]).sum(-1) < 0
        anim.rotations[f][flip] *= -1
    return {limb: {k: round(v, 2) if isinstance(v, float) else v for k, v in r.items()} for limb, r in rep.items()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("models", type=Path)
    ap.add_argument("src", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--loop", action="store_true")
    ap.add_argument("--floor", type=float, help="degrees; the bind pose's knee by default")
    ap.add_argument("--soft", type=float, default=6.0)
    ap.add_argument("--report", type=Path)
    a = ap.parse_args()
    skel = ga.read_skeleton(a.models / "HUMANS.MDH.json")
    anim = ga.read_animation(a.src)
    floor = a.floor if a.floor is not None else bind_bend(skel)
    rep = {"floor_deg": round(floor, 2), "soft_deg": a.soft, "legs": lay(skel, anim, floor, a.soft, a.loop)}
    ga.write_animation(a.out, anim, f"{a.src.name} + knee_floor {floor:.2f} soft {a.soft}")
    print(json.dumps(rep, indent=1))
    if a.report:
        a.report.write_text(json.dumps(rep, indent=1))


if __name__ == "__main__":
    main()
