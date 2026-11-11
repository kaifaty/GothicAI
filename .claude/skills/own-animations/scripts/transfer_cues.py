"""What the transfer to Gothic's skeleton changes: a Kimodo sample beside the same frames on BIP01.

    python3 .claude/skills/own-animations/scripts/transfer_cues.py <base models> <sample.npz> <retargeted.MAN.json> \
        [<later.MAN.json>]

A flaw seen in the game is the generator's or ours. This table is the first split: the sample's own joints (SOMA)
against the record `retarget_soma.py` made of it, frame by frame - knee and elbow flex, where the feet and hands
are about the hips, the trunk's lean. A third record of the same frames (after a fix layer, or a loop tiled in step
with the sample) shows what the later steps changed. The eye's half of the check is `blender/soma_view.py` beside
`blender/gothic_rig.py`.

Columns: each signal's range in the sample and on BIP01, the mean and largest difference between them with the
constant offset of the two skeletons taken out (their hips, neck and feet nodes sit at different places of the body;
knee and elbow angles are compared as they are), and
how the two follow each other (1.000 - the same motion). Angles are 0 for a straight limb; heights and places in cm.

What it has shown (run keyed to Gothic's poses, 2026-11-11): angles come over within 0.6° on average, places within 1.3 cm; the
toe does not - BIP01's toe node goes 2-4 cm lower than SOMA's at the push-off (under the floor), and a leg SOMA
holds straight reads 0° on BIP01's chain, which is bent 13.5° when the body's leg is straight (`knee_floor.py`).
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "scripts" / "content" / "anim"))
import gothic_anim as ga  # noqa: E402
import retarget_soma as rs  # noqa: E402


def ang(u, v):
    return np.degrees(np.arccos(np.clip((u * v).sum(-1) / np.linalg.norm(u, axis=-1) / np.linalg.norm(v, axis=-1), -1, 1)))


def signals(j, hips, neck, limb) -> dict[str, np.ndarray]:
    """`j(name)` gives a joint's places [T, 3] in Gothic's frame; `limb(side)` its hip, knee, ankle, toe, shoulder,
    elbow and wrist names."""
    out = {}
    for side in "LR":
        hip, knee, ankle, toe, shoulder, elbow, wrist = (j(n) for n in limb(side))
        out[f"knee {side} °"] = 180 - ang(hip - knee, ankle - knee)
        out[f"elbow {side} °"] = 180 - ang(shoulder - elbow, wrist - elbow)
        out[f"ankle height {side}"] = ankle[:, 1]
        out[f"toe height {side}"] = toe[:, 1]
        out[f"foot ahead of hips {side}"] = ankle[:, 2] - j(hips)[:, 2]
        out[f"hand over hips {side}"] = wrist[:, 1] - j(hips)[:, 1]
        out[f"hand ahead of hips {side}"] = wrist[:, 2] - j(hips)[:, 2]
    up = j(neck) - j(hips)
    out["lean °"] = np.degrees(np.arctan2(up[:, 2], up[:, 1]))
    out["hips height"] = j(hips)[:, 1]
    return out


def soma_signals(npz: Path) -> dict[str, np.ndarray]:
    pos, _, names, _ = rs.load_soma(npz)
    pts = pos * 100.0
    pts[..., 0] *= -1                                    # SOMA's x is the character's left, Gothic's its right
    full = {"L": "Left", "R": "Right"}
    return signals(lambda n: pts[:, names.index(n)], "Hips", "Neck1",
                   lambda s: [full[s] + n for n in ("Leg", "Shin", "Foot", "ToeBase", "Arm", "ForeArm", "Hand")])


def bip_signals(skel: ga.Skeleton, path: Path) -> dict[str, np.ndarray]:
    a = ga.read_animation(path)
    idx = {n.name: i for i, n in enumerate(skel.nodes)}
    pts = np.array([ga.world_pose(skel, a, f)[:, :3, 3] for f in range(a.frame_count)])
    return signals(lambda n: pts[:, idx["BIP01 " + n]], "PELVIS", "NECK",
                   lambda s: [f"{s} {n}" for n in ("THIGH", "CALF", "FOOT", "TOE0", "UPPERARM", "FOREARM", "HAND")])


def differ(a: np.ndarray, b: np.ndarray, offset: bool) -> tuple[float, float]:
    d = np.abs((a - a.mean()) - (b - b.mean())) if offset else np.abs(a - b)
    return float(d.mean()), float(d.max())


def main():
    models, npz, rec = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    later = Path(sys.argv[4]) if len(sys.argv) > 4 else None
    skel = ga.read_skeleton(models / "HUMANS.MDH.json")
    S, B = soma_signals(npz), bip_signals(skel, rec)
    n = min(len(S["lean °"]), len(B["lean °"]))
    L = bip_signals(skel, later) if later else None
    rng = lambda v: f"{v.min():7.1f}..{v.max():<7.1f}"
    print(f"{'':24}{'sample':>16}{'on BIP01':>16}  transfer: mean  max  follows" + ("          later   mean  max" if L else ""))
    for k in S:
        a, b = S[k][:n], B[k][:n]
        m, x = differ(a, b, not k.endswith("°") or k.startswith("lean"))
        row = f"{k:24}{rng(a)}{rng(b)}{m:14.1f}{x:6.1f}{np.corrcoef(a, b)[0, 1]:8.3f}"
        if L:
            m2, x2 = differ(b, L[k][:n], False)
            row += f"   {rng(L[k][:n])}{m2:6.1f}{x2:6.1f}"
        print(row)


if __name__ == "__main__":
    main()
