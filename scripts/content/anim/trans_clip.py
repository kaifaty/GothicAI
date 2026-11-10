"""Cut a transition between a stance and a moving loop out of a retargeted Kimodo sample: a start of a run
(T_RUN_2_RUNL) or a stop on one foot (T_RUNL_2_RUN, T_RUNR_2_RUN).

    python3 scripts/content/anim/trans_clip.py <in.MAN.json> <models dir> <out.MAN.json> --name T_RUNL_2_RUN \
        --next S_RUN --frames a,b --start REC[:FRAME] --end REC[:FRAME] [--fade 0.15,0.2] [--mirror] [--report out.json]

The sample is generated with Kimodo keyed to the poses it has to meet (`kimodo_moves.py`: the stance at one end,
frames of the loop at the other, a dense root path that speeds up or brakes), then retargeted at 30 fps.

1. `--mirror` swaps left and right first (`mirror`): one sample gives the stop on either foot, so both stops look
   alike. Every node takes its partner's turn away from the bind pose, seen in a mirror, and the partner's place.
2. Frames a..b of the sample are the clip.
3. Both ends land on the poses the game plays around the clip (`finish.snap_ends`): the first frame on `--start`,
   the last on `--end`, each a record and a frame of it (the loop's frame for the foot the stop begins on). The
   difference fades over `--fade` seconds; the root is snapped in height only, the engine moves the body by the
   root's own way.
4. The root ends straight ahead of where it began: its drift aside is taken out evenly, the body goes with it.
5. A foot that stands is pinned where it touched down and the leg bent to it (`finish.lock_feet`), with its turn.

The engine plays a transition to its end before anything else may start. A stop packed with the flag SETTLE (64)
gives way once its root has come to rest (OpenGothic/common/graphics/mesh/stopfoot.h), so the clip may take its
time to straighten up. Toes under the floor are a job for `fix_layer.py` (`soles`) afterwards.
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import finish
import fix_layer as fl
import gothic_anim as ga

FLIP = np.diag([-1.0, 1.0, 1.0])


def partner(name: str) -> str:
    if " L " in name:
        return name.replace(" L ", " R ")
    if " R " in name:
        return name.replace(" R ", " L ")
    return name


def mirror(skel: ga.Skeleton, anim: ga.Animation) -> ga.Animation:
    """The record seen in a mirror across the plane the model faces along (x = 0), left and right swapped. Every
    bone takes its partner's turn away from the bind pose, mirrored, and the partner's mirrored place; the slots
    for weapons (ZS_*) stay where they are on their bones."""
    names = [n.name for n in skel.nodes]
    pair = [names.index(partner(n)) if partner(n) in names else i for i, n in enumerate(names)]
    bind = ga.world_pose(skel, None, 0)
    out = dataclasses.replace(anim, positions=anim.positions.copy(), rotations=anim.rotations.copy())
    cols = {node: c for c, node in enumerate(anim.node_indices)}
    if any(pair[n] not in cols for n in anim.node_indices):
        raise SystemExit("mirror: the record animates a node without its partner")
    for f in range(anim.frame_count):
        W = ga.world_pose(skel, anim, f)
        M = np.empty_like(W)
        for i in range(len(names)):
            m = pair[i]
            M[i] = np.eye(4)
            M[i][:3, :3] = FLIP @ W[m][:3, :3] @ bind[m][:3, :3].T @ FLIP @ bind[i][:3, :3]
            M[i][:3, 3] = FLIP @ W[m][:3, 3]
        for node, c in cols.items():
            if names[node].startswith("ZS_"):
                continue                      # a slot keeps its place on its own bone: the sword stays on the left hip
            parent = skel.nodes[node].parent
            L = M[node] if parent < 0 else np.linalg.inv(M[parent]) @ M[node]
            out.positions[f, c] = L[:3, 3]
            out.rotations[f, c] = fl.quat(L[:3, :3])
    for f in range(1, out.frame_count):
        flip = (out.rotations[f] * out.rotations[f - 1]).sum(-1) < 0
        out.rotations[f][flip] *= -1
    return out


def one_frame(spec: str) -> ga.Animation:
    path, _, frame = spec.partition(":")
    rec = ga.read_animation(Path(path))
    f = int(frame or 0)
    return dataclasses.replace(rec, frame_count=1, positions=rec.positions[f:f + 1].copy(),
                               rotations=rec.rotations[f:f + 1].copy())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src", type=Path)
    ap.add_argument("models", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--name", required=True)
    ap.add_argument("--next", default="")
    ap.add_argument("--frames", required=True, help="a,b: the first and the last frame of the sample")
    ap.add_argument("--start", required=True, help="REC[:FRAME] the clip's first frame lands on")
    ap.add_argument("--end", required=True, help="REC[:FRAME] the clip's last frame lands on")
    ap.add_argument("--fade", default="0.15,0.2", help="seconds the difference to the start and to the end fades over")
    ap.add_argument("--mirror", action="store_true", help="swap left and right first")
    ap.add_argument("--report", type=Path)
    a = ap.parse_args()

    skel = ga.read_skeleton(a.models / "HUMANS.MDH.json")
    idx = {n.name: i for i, n in enumerate(skel.nodes)}
    src = ga.read_animation(a.src)
    if a.mirror:
        src = mirror(skel, src)
    f0, f1 = (int(x) for x in a.frames.split(","))
    clip = dataclasses.replace(src, name=a.name.upper(), next=a.next.upper(), frame_count=f1 - f0 + 1,
                               positions=src.positions[f0:f1 + 1].copy(), rotations=src.rotations[f0:f1 + 1].copy(),
                               events=[])
    start, end = one_frame(a.start), one_frame(a.end)
    finish.snap_ends(clip, start, *(float(x) for x in a.fade.split(",")), end=end)
    root = clip.positions[:, 0]
    root[:, 0] -= np.linspace(0.0, root[-1, 0] - root[0, 0], clip.frame_count)   # straight ahead: no drift aside
    root[:, [0, 2]] += start.positions[0, 0, [0, 2]] - root[0, [0, 2]]
    before = [ga.world_pose(skel, clip, f) for f in range(clip.frame_count)]
    finish.lock_feet(clip, skel, turn=True)
    ga.write_animation(a.out, clip, f"{a.src.name} frames {f0}..{f1}{', mirrored' if a.mirror else ''}")

    after = [ga.world_pose(skel, clip, f) for f in range(clip.frame_count)]
    way = np.array([w[0][:3, 3] for w in after])
    rep = {"source": str(a.src), "frames": [f0, f1], "mirrored": a.mirror, "fps": clip.fps,
           "duration_s": round(clip.frame_count / clip.fps, 3),
           "travel_cm": round(float(way[-1, 2] - way[0, 2]), 2),
           "feet_moved_by_pin_cm": {s: round(float(max(np.linalg.norm(x[idx[f"BIP01 {s} FOOT"]][:3, 3] -
                                                                         y[idx[f"BIP01 {s} FOOT"]][:3, 3])
                                                          for x, y in zip(before, after))), 2) for s in "LR"}}
    print(json.dumps(rep, indent=1))
    if a.report:
        a.report.write_text(json.dumps(rep, indent=1))


if __name__ == "__main__":
    main()
