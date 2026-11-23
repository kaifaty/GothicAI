"""A standing loop on BIP01: the pose it holds and how much life it has.

    python3 .claude/skills/own-animations/scripts/stance_cues.py <base models> LABEL=<clip.MAN.json> ...

The model looks along +Z, its right hand is at +X, Y is up, centimetres. A stance is a loop in place: the last frame
repeats the first, so the cycle is frames 0..N-2. Printed per clip:
- pose (the mean over the cycle): the feet's base and stagger, toe-out, knee bend, where the weight is between the
  feet, the trunk's lean and the chest / neck / head tilts (as in gait_cues.py), the shoulders' turn against the hips,
  the hands' place against the pelvis, elbow and wrist;
- life (the range over the cycle): the pelvis' sway, the chest's rise and pitch with its period (breathing), the
  shoulders' rise, the head's travel and turn, the hands' travel;
- faults: the largest travel of a planted ankle and toe, the seam between the last frame and the first.
Gothic's own S_RUN (2 s, 50 frames) is the reference for the pose: every T_RUN_2_* and T_STAND_2_* clip of the base
starts from it.
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "scripts" / "content" / "anim"))
import gothic_anim as ga

AHEAD = np.array([0.0, 0.0, 1.0])
RIGHT = np.array([1.0, 0.0, 0.0])
UP = np.array([0.0, 1.0, 0.0])


def angle(u: np.ndarray, v: np.ndarray) -> np.ndarray:
    u = u / np.linalg.norm(u, axis=-1, keepdims=True)
    v = v / np.linalg.norm(v, axis=-1, keepdims=True)
    return np.degrees(np.arccos(np.clip((u * v).sum(-1), -1, 1)))


def period(x: np.ndarray, fps: float) -> float:
    """The dominant period of a signal over the cycle, seconds; 0 when it barely moves."""
    x = x - x.mean()
    if np.ptp(x) < 1e-3 or len(x) < 4:
        return 0.0
    spec = np.abs(np.fft.rfft(x))
    spec[0] = 0
    k = int(spec.argmax())
    return len(x) / fps / k if k else 0.0


def cues(skel: ga.Skeleton, path: str) -> dict[str, float]:
    idx = {n.name: i for i, n in enumerate(skel.nodes)}
    a = ga.read_animation(path)
    n = a.frame_count - 1 if a.frame_count > 2 else a.frame_count
    W = [ga.world_pose(skel, a, f) for f in range(a.frame_count)]

    def P(name: str, frames: int = n) -> np.ndarray:
        return np.array([w[idx[name]][:3, 3] for w in W[:frames]])

    def R(name: str) -> np.ndarray:
        return np.array([w[idx[name]][:3, :3] for w in W[:n]])

    def tilt(v: np.ndarray) -> np.ndarray:  # forward tilt, + = tipped forward
        return np.degrees(np.arctan2(v @ AHEAD, v[:, 1]))

    def yaw(v: np.ndarray) -> np.ndarray:  # turn of a left-to-right line seen from above, + = right side forward
        return np.degrees(np.arctan2(v @ AHEAD, v @ RIGHT))

    lf, rf = P("BIP01 L FOOT"), P("BIP01 R FOOT")
    pel = P("BIP01 PELVIS")
    base = (rf - lf) @ RIGHT
    mid = (lf + rf) / 2
    out: dict[str, float] = {
        "frames": a.frame_count, "seconds": n / a.fps,
        "base cm": base.mean(),
        "stagger cm": ((lf - rf) @ AHEAD).mean(),  # + = the left foot is ahead
        "weight %": (((pel - mid) @ RIGHT) / (base / 2)).mean() * 100,  # + = over the right foot
        "pelvis h cm": (pel[:, 1] - np.minimum(lf[:, 1], rf[:, 1])).mean(),
    }
    for s in "LR":
        toe = P(f"BIP01 {s} TOE0") - P(f"BIP01 {s} FOOT")
        out[f"toe-out {s} °"] = (np.degrees(np.arctan2(toe @ RIGHT, toe @ AHEAD)) * (1 if s == "R" else -1)).mean()
        out[f"knee {s} °"] = (180 - angle(P(f"BIP01 {s} THIGH") - P(f"BIP01 {s} CALF"),
                                          P(f"BIP01 {s} FOOT") - P(f"BIP01 {s} CALF"))).mean()
        hand = P(f"BIP01 {s} HAND")
        out[f"hand {s} out cm"] = ((hand - pel) @ RIGHT * (1 if s == "R" else -1)).mean()
        out[f"hand {s} ahead cm"] = ((hand - pel) @ AHEAD).mean()
        out[f"hand {s} up cm"] = (hand[:, 1] - pel[:, 1]).mean()
        out[f"elbow {s} °"] = (180 - angle(P(f"BIP01 {s} UPPERARM") - P(f"BIP01 {s} FOREARM"),
                                           hand - P(f"BIP01 {s} FOREARM"))).mean()
        out[f"wrist {s} °"] = angle(hand - P(f"BIP01 {s} FOREARM"), P(f"BIP01 {s} FINGER0") - hand).mean()
    out["lean °"] = tilt(P("BIP01 NECK") - pel).mean()
    out["chest °"] = tilt(P("BIP01 NECK") - P("BIP01 SPINE2")).mean()
    out["neck °"] = tilt(P("BIP01 HEAD") - P("BIP01 NECK")).mean()
    head_up = R("BIP01 HEAD")[:, :, 0]  # the head bone's X axis runs up the head on BIP01
    out["head °"] = tilt(head_up).mean()
    hips = P("BIP01 R THIGH") - P("BIP01 L THIGH")
    shoulders = P("BIP01 R UPPERARM") - P("BIP01 L UPPERARM")
    out["hips turn °"] = yaw(hips).mean()
    out["shoulders vs hips °"] = (yaw(shoulders) - yaw(hips)).mean()
    out["hip drop °"] = np.degrees(np.arctan2(hips[:, 1], hips @ RIGHT)).mean()  # + = the right hip is higher

    # life
    out["pelvis side cm"] = np.ptp(pel @ RIGHT)
    out["pelvis ahead cm"] = np.ptp(pel @ AHEAD)
    out["pelvis up cm"] = np.ptp(pel[:, 1])
    chest = P("BIP01 SPINE2")
    neck = P("BIP01 NECK")
    out["chest rise cm"] = np.ptp(neck[:, 1] - pel[:, 1])
    pitch = tilt(neck - chest)
    out["chest pitch °"] = np.ptp(pitch)
    out["breath s"] = period(pitch, a.fps)
    out["shoulder rise cm"] = max(np.ptp(P(f"BIP01 {s} UPPERARM")[:, 1] - neck[:, 1]) for s in "LR")
    head = P("BIP01 HEAD")
    out["head travel cm"] = float(np.linalg.norm(np.ptp(head, axis=0)))
    out["head nod °"] = np.ptp(tilt(head_up))
    face = R("BIP01 HEAD")[:, :, 2]  # ...and its Z axis looks out of the face
    out["head turn °"] = np.ptp(np.degrees(np.arctan2(face @ RIGHT, face @ AHEAD)))
    out["hand travel cm"] = max(float(np.linalg.norm(np.ptp(P(f"BIP01 {s} HAND"), axis=0))) for s in "LR")

    # faults
    out["ankle travel cm"] = max(float(np.linalg.norm(np.ptp(p, axis=0))) for p in (lf, rf))
    out["toe travel cm"] = max(float(np.linalg.norm(np.ptp(P(f"BIP01 {s} TOE0"), axis=0))) for s in "LR")
    first, last = W[0], W[a.frame_count - 1]
    out["seam cm"] = max(float(np.linalg.norm(first[i][:3, 3] - last[i][:3, 3])) for i in a.node_indices
                         if i < len(skel.nodes))
    return out


def main() -> None:
    base = Path(sys.argv[1])
    skel = ga.read_skeleton(base / "HUMANS.MDH.json")
    rows = []
    for arg in sys.argv[2:]:
        label, path = arg.split("=", 1)
        rows.append((label, cues(skel, path)))
    keys = list(rows[0][1])
    width = max(len(k) for k in keys)
    print(" " * width + "".join(f"{label:>12}" for label, _ in rows))
    for k in keys:
        print(f"{k:<{width}}" + "".join(f"{r[k]:>12.1f}" for _, r in rows))


if __name__ == "__main__":
    main()
