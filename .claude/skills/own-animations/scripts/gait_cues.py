"""What makes a run or walk loop read as a man's or a woman's gait, and how much power it shows, on BIP01.

    python3 .claude/skills/own-animations/scripts/gait_cues.py <base models> LABEL=<loop.MAN.json> ...

One cycle of each loop (the last frame is the first again, moved by the travel). Compare with Gothic's own male and
female runs: G=$B/HUMANS-S_RUNL.MAN.json BABE=$B/HUMANS_BABE-S_RUNL.MAN.json. On 2026-11-09 loop A was rejected as
«бегает как девочка»; the cues that set it apart (Gothic male / female / A):
- hip drop: the range of the hips' roll seen from the front, 4.8 / 11.3 / 17.9°;
- wrist: the hand's bend off its forearm's line, 3 / 8 / 70°;
- hand vs elbow: how far the hand is outside its elbow (+) or inside (-), -10 / -8 / 0 cm;
- hand height over the pelvis, 25 / 23 / 15 cm;
- knee in the swing and the ankle's rise: 129° and 62 cm for Gothic, 81-104° and 22-35 cm for Kimodo's runs.
The base (feet apart at stance) is printed too, but runners do run on a narrow line: Gothic's 16 cm is a style.
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "scripts" / "content" / "anim"))
import gothic_anim as ga


def angle(u: np.ndarray, v: np.ndarray) -> np.ndarray:
    u = u / np.linalg.norm(u, axis=-1, keepdims=True)
    v = v / np.linalg.norm(v, axis=-1, keepdims=True)
    return np.degrees(np.arccos(np.clip((u * v).sum(-1), -1, 1)))


def cues(skel: ga.Skeleton, path: str) -> dict[str, float]:
    idx = {n.name: i for i, n in enumerate(skel.nodes)}
    a = ga.read_animation(path)
    n = a.frame_count - 1 if a.frame_count > 2 else a.frame_count
    W = [ga.world_pose(skel, a, f) for f in range(n)]

    def P(name: str) -> np.ndarray:
        return np.array([w[idx[name]][:3, 3] for w in W])

    ends = [ga.world_pose(skel, a, f)[idx["BIP01"]][:3, 3] for f in (0, a.frame_count - 1)]
    ahead = ends[1] - ends[0]
    ahead[1] = 0
    ahead /= np.linalg.norm(ahead)
    side = np.cross([0, 1.0, 0], ahead)
    if ((P("BIP01 L THIGH") - P("BIP01 R THIGH")) @ side).mean() < 0:
        side = -side  # +side is the left
    mid = (P("BIP01 PELVIS") @ side).mean()

    def lat(name: str) -> np.ndarray:
        return P(name) @ side - mid

    def stance(foot: str) -> np.ndarray:
        h = P(foot)[:, 1]
        return h < h.min() + 0.2 * np.ptp(h)

    hips = P("BIP01 L THIGH") - P("BIP01 R THIGH")
    out = {
        "hip drop °": np.ptp(np.degrees(np.arctan2(hips[:, 1], hips @ side))),
        "base cm": lat("BIP01 L FOOT")[stance("BIP01 L FOOT")].mean() - lat("BIP01 R FOOT")[stance("BIP01 R FOOT")].mean(),
        "wrist °": max(angle(P(f"BIP01 {s} HAND") - P(f"BIP01 {s} FOREARM"),
                             P(f"BIP01 {s} FINGER0") - P(f"BIP01 {s} HAND")).max() for s in "LR"),
        "hand-elbow cm": np.concatenate([lat("BIP01 L HAND") - lat("BIP01 L FOREARM"),
                                         -(lat("BIP01 R HAND") - lat("BIP01 R FOREARM"))]).mean(),
        "hand h cm": np.concatenate([P(f"BIP01 {s} HAND")[:, 1] - P("BIP01 PELVIS")[:, 1] for s in "LR"]).mean(),
        "knee °": max((180 - angle(P(f"BIP01 {s} THIGH") - P(f"BIP01 {s} CALF"),
                                   P(f"BIP01 {s} FOOT") - P(f"BIP01 {s} CALF"))).max() for s in "LR"),
        "ankle rise cm": max(np.ptp(P(f"BIP01 {s} FOOT")[:, 1]) for s in "LR"),
    }
    trunk = P("BIP01 NECK") - P("BIP01 PELVIS")
    out["lean °"] = np.degrees(np.arctan2(trunk @ ahead, trunk[:, 1])).mean()
    return {k: float(v) for k, v in out.items()}


def main():
    skel = ga.read_skeleton(Path(sys.argv[1]) / "HUMANS.MDH.json")
    rows = [(spec.split("=", 1)[0], cues(skel, spec.split("=", 1)[1])) for spec in sys.argv[2:]]
    keys = list(rows[0][1])
    print("clip | " + " | ".join(keys))
    for label, c in rows:
        print(f"{label} | " + " | ".join(f"{c[k]:.1f}" for k in keys))


if __name__ == "__main__":
    main()
