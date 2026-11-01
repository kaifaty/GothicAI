"""Prepare Kimodo generation folders (meta.json + constraints.json) for a list of moves.

    python3 scripts/content/anim/kimodo_moves.py scripts/content/anim/moves-probe.json \
        <base>/models <soma-ref.npz> <out-dir> [--stance HUMANS_1HST2-S_1HRUN.MAN] [--only dash_back,...]

Each move starts in the Gothic stance (a full-body keyframe at frame 0, translated to SOMA) and,
unless `end_free`, ends in the same stance displaced by `travel`. --stance overrides the spec's stance (each
talent overlay has its own combat stance, and Gothic its own jump-back per overlay); --only picks moves. The folders are what
`kimodo_gen --input_folder` reads; `run_kimodo.sh` runs them all.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gothic_anim as ga  # noqa: E402
import retarget_soma as rs  # noqa: E402

KIMODO_FPS = 30


def keyframe(kind: str, frames: list[int], rots: list[np.ndarray], hips: list[np.ndarray]) -> dict:
    return {
        "type": kind,
        "frame_indices": frames,
        "local_joints_rot": [r.round(6).tolist() for r in rots],
        "root_positions": [h.round(5).tolist() for h in hips],
        "smooth_root_2d": [[float(h[0]), float(h[2])] for h in hips],
    }


def main():
    ap = argparse.ArgumentParser()
    for name in ("spec", "models", "soma_ref", "out"):
        ap.add_argument(name, type=Path)
    ap.add_argument("--stance")
    ap.add_argument("--only")
    a = ap.parse_args()
    spec_path, models, soma_ref, out = a.spec, a.models, a.soma_ref, a.out
    spec = json.loads(spec_path.read_text())
    if a.stance:
        spec["stance"] = a.stance
    if a.only:
        spec["moves"] = [m for m in spec["moves"] if m["id"] in a.only.split(",")]
    skel0 = ga.read_skeleton(models / "HUMANS.MDH.json")
    stance = ga.read_animation(models / (spec["stance"] + ".json"))
    skel = rs.with_offsets(skel0, stance)
    pos, rot, names, _ = rs.load_soma(soma_ref)
    rig = rs.soma_rig(skel, pos[0], rot[0], names, stance)
    rots, hips = rs.gothic_to_soma(skel, stance, 0, rig, names)
    origin = hips[[0, 2]].copy()
    hips[[0, 2]] = 0.0  # Kimodo canonical start: the root at the XZ origin, facing +Z
    for i, move in enumerate(spec["moves"]):
        frames = int(round(move["duration"] * KIMODO_FPS))
        end = hips.copy()
        end[0] += move["travel"][0]
        end[2] += move["travel"][1]
        cons = [keyframe("fullbody", [0], [rots], [hips])]
        if not move.get("end_free"):
            cons.append(keyframe("fullbody", [frames - 1], [rots], [end]))
        else:
            cons.append({"type": "root2d", "frame_indices": [frames - 1],
                         "smooth_root_2d": [[float(end[0]), float(end[2])]]})
        d = out / move["id"]
        d.mkdir(parents=True, exist_ok=True)
        (d / "constraints.json").write_text(json.dumps(cons))
        (d / "meta.json").write_text(json.dumps({
            "text": move["text"], "duration": frames / KIMODO_FPS, "num_samples": spec["samples"],
            "seed": spec["seed"] + i, "diffusion_steps": 100,
            "cfg": {"enabled": True, "text_weight": 2.0, "constraint_weight": 2.0},
        }, indent=1))
    (out / "stance.json").write_text(json.dumps({"stance": spec["stance"], "origin_xz_m": origin.tolist(),
                                                 "hips_m": hips.tolist()}, indent=1))
    print("prepared", len(spec["moves"]), "moves in", out)


if __name__ == "__main__":
    main()
