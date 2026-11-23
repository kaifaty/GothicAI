"""Shared bits of the review scripts: the repo's animation tools, the skeleton, world poses, the floor under a stance."""
import sys
from pathlib import Path

import numpy as np

ANIM = Path(__file__).resolve().parents[4] / "scripts" / "content" / "anim"
sys.path.insert(0, str(ANIM))
import gothic_anim as ga  # noqa: E402,F401
import retarget_soma as rs  # noqa: E402,F401

LEVELS = {"novice": "HUMANS", "fighter": "HUMANS_1HST1", "master": "HUMANS_1HST2"}


def skeleton(base: Path):
    return ga.read_skeleton(Path(base) / "HUMANS.MDH.json")


def positions(skel, anim) -> np.ndarray:
    """Model-space joint positions, frames × nodes × 3, in cm."""
    return np.array([ga.world_pose(skel, anim, f) for f in range(anim.frame_count)])[:, :, :3, 3]


def floor_of(skel, stance) -> float:
    """As fix_layer.py: the stance's own toe nodes sit about 3 cm over the sole (the master stance writes no right foot)."""
    w0 = ga.world_pose(skel, stance, 0)
    toes = [skel.index(f"BIP01 {s} TOE0") for s in "LR"]
    return min(w0[i][1, 3] for i in [i for i in toes if i in stance.node_indices] or toes) - 3


def turn(R0: np.ndarray, R1: np.ndarray) -> float:
    """Degrees between two rotation matrices."""
    return float(np.degrees(np.arccos(np.clip((np.trace(R0.T @ R1) - 1) / 2, -1, 1))))


def blade(W: np.ndarray, skel, length: float = 90.0, samples: int = 18) -> np.ndarray:
    """Points along the blade at one frame: −X of ZS_RIGHTHAND, as the engine hangs the weapon mesh (not +X of the .glb)."""
    s = W[skel.index("ZS_RIGHTHAND")]
    return np.array([s[:3, 3] - s[:3, 0] * length * t for t in np.linspace(0.15, 1, samples)])


def runs(frames) -> str:
    """1,2,3,7 → "1–3,7"."""
    out = []
    for f in sorted(frames):
        if out and f == out[-1][1] + 1:
            out[-1][1] = f
        else:
            out.append([f, f])
    return ",".join(f"{a}" if a == b else f"{a}–{b}" for a, b in out) or "-"
