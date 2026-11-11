"""A Kimodo spec that keys a new walk or run to the poses of a loop we already have (Gothic's own S_WALKL, S_RUNL).

    python3 scripts/content/anim/loop_keys.py <base models> <soma-ref.npz> <out dir> --clip HUMANS-S_WALKL.MAN \
        [--id walk] [--keys 2,4,8] [--text "A man walks forward."] [--stance HUMANS-S_RUN.MAN] [--samples 8] [--seed 9401]
    python3 scripts/content/anim/kimodo_moves.py <out dir>/moves-<id>.json <base models> <soma-ref.npz> <out dir>/gen \
        <out dir>/poses

Why: a loop made from a text prompt alone is the generator's average passer-by, and a live actor's walk is that
actor's (reports/content/2026-11-hero-locomotion, «Ходьба по ключевым позам»). What reads right in Gothic's walk is
its authored poses - the elbow always bent, the arm swinging back, the knee that gives - so the poses are kept and
the generator draws the motion between them.

- The loop (its last frame is the first again, moved by the travel) is tiled over time and sampled at Kimodo's
  30 frames a second into a pose record `<out dir>/poses/HUMANS-P_<ID>30.MAN.json`: frame t of the record is the
  pose at t / 30 s, so a key of Kimodo's frame t takes the record's frame t and no timing is rounded twice.
- `--keys`: how many poses of a cycle are keyed, evenly over it from the cycle's first frame: 2 - the two contacts
  of a loop that starts on a strike, 4 - with both passing poses, 8 - every eighth of the cycle. One move per
  number, `<id>_k<N>`, as many cycles long as stay under Kimodo's 20 full-body keys (6 at most). Each key is a
  full-body one with the root at its own place on the loop's way.
- Fewer keys leave more to the generator. Measured on S_WALKL (8 samples each): at 2 keys a cycle the pelvis
  sways 56-75 mm instead of Gothic's 15 and the mean joint is 5-6 cm off Gothic's pose; at 4 keys 16-23 mm and
  5 cm; at 8 keys the walk is Gothic's within 3.7 cm.
  A run needs more: on S_RUNL (cycle 0.76 s) 4 keys leave the pelvis' low and high to the generator and it bounces
  15-21 cm against Gothic's 9, wherever in the cycle the keys stand; 6 keys (`--keys 6`) hold 8-9 cm.
- `--symmetric`: the loop averaged with its own mirror image half a cycle on, so the right step is the left one
  mirrored. Gothic's run needs it: its trunk leans back and forward once a cycle (10.3°, so once a step each way),
  its chest is turned (the left shoulder 20.6° ahead, the right 4.6° behind) and its hips sink deeper on the left
  step; keyed as they are, 6 keys copy all of it. Averaged: 0.9°, both shoulders 8°, the hips' lows 0.6 cm apart.
- The shoulders reach Kimodo through `retarget_soma.gothic_to_soma`, from Gothic's at ease to SOMA's: copied as
  they are, Gothic's poses put them 24° ahead of where Kimodo keeps them, and it drew a hunched man.

The generated samples go the usual way: `retarget_soma.py`, `loop_cycle.py --period <the loop's seconds>` (the
period is known, and a walk that hardly lifts its feet gives the autocorrelation nothing to hold on to),
`knee_floor.py --loop`, `fix_layer.py`.
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gothic_anim as ga
import kimodo_moves as km
import retarget_soma as rs

MAX_KEYS, MAX_CYCLES = 19, 6


def tile(loop: ga.Animation, fps: float, count: int, name: str, start: float = 0.0) -> ga.Animation:
    """`count` frames of the loop played over and over, sampled at `fps` from `start` cycles in; the root goes on by
    the travel."""
    n = loop.frame_count - 1
    travel = loop.positions[n, 0] - loop.positions[0, 0]
    pos = np.zeros((count,) + loop.positions.shape[1:])
    rot = np.zeros((count,) + loop.rotations.shape[1:])
    for t in range(count):
        cycle, g = divmod(t / fps * loop.fps + start * n, n)
        a = min(int(np.floor(g)), n - 1)
        u = g - a
        for k in range(loop.rotations.shape[1]):
            rot[t, k] = rs.slerp(loop.rotations[a, k], loop.rotations[a + 1, k], u)
        pos[t] = loop.positions[a] * (1 - u) + loop.positions[a + 1] * u
        pos[t, 0] += cycle * travel
        if t:
            flip = (rot[t] * rot[t - 1]).sum(-1) < 0
            rot[t][flip] *= -1
    if start:
        pos[:, 0] -= start * travel                    # the way starts where an unshifted tiling starts
    return dataclasses.replace(loop, name=name, next="", fps=float(fps), frame_count=count, positions=pos, rotations=rot)


def blend(a: np.ndarray, b: np.ndarray, u: float) -> np.ndarray:
    """Quaternions [..., 4] from a towards b by u (normalised lerp on the near side; poses here differ by a few
    tens of degrees at most)."""
    b = np.where(((a * b).sum(-1) < 0)[..., None], -b, b)
    q = a * (1 - u) + b * u
    return q / np.linalg.norm(q, axis=-1, keepdims=True)


def symmetric(skel: ga.Skeleton, loop: ga.Animation, tiled: ga.Animation) -> ga.Animation:
    """The tiled loop averaged with its mirror image half a cycle on: the left step and the right one become one
    another's mirror. Gothic's run is not: its trunk leans back to -8° and forward to +2° once a cycle, so once a
    step it is thrown back and once forward, and the hips sink 2.3 cm deeper on the left step (2026-11-11, «на шаг
    левой ноги он сильно заваливается»). The mean of the two halves keeps everything that comes once a step."""
    from trans_clip import mirror
    other = tile(mirror(skel, loop), tiled.fps, tiled.frame_count, tiled.name, start=0.5)
    pos = 0.5 * (tiled.positions + other.positions)
    return dataclasses.replace(tiled, positions=pos, rotations=blend(tiled.rotations, other.rotations, 0.5))


def key_frames(cycle: float, per: int) -> list[int]:
    """Kimodo frames of `per` keys a cycle of `cycle` frames, over as many cycles as the key limit lets."""
    cycles = max(1, min(MAX_CYCLES, (MAX_KEYS - 1) // per))
    return sorted({int(round(i * cycle / per)) for i in range(per * cycles + 1)})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("models", type=Path)
    ap.add_argument("soma_ref", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--clip", required=True, help="a loop of the base, HUMANS-S_WALKL.MAN, or a .MAN.json record")
    ap.add_argument("--id", default="walk")
    ap.add_argument("--keys", default="2,4,8")
    ap.add_argument("--text", default="A man walks forward.")
    ap.add_argument("--stance", default="HUMANS-S_RUN.MAN")
    ap.add_argument("--samples", type=int, default=8)
    ap.add_argument("--seed", type=int, default=9401)
    ap.add_argument("--symmetric", action="store_true", help="average the loop with its mirror half a cycle on")
    a = ap.parse_args()

    path = Path(a.clip) if a.clip.endswith(".json") else a.models / (a.clip + ".json")
    loop = ga.read_animation(path)
    if loop.node_indices[0] != 0:
        raise SystemExit("the loop does not animate the root: no travel to key")
    cycle = (loop.frame_count - 1) / loop.fps * km.KIMODO_FPS
    record = f"HUMANS-P_{a.id.upper()}30.MAN.json"
    tiled = tile(loop, km.KIMODO_FPS, int(np.ceil(cycle * MAX_CYCLES)) + 2, f"P_{a.id.upper()}30")
    skel = ga.read_skeleton(a.models / "HUMANS.MDH.json")
    if a.symmetric:
        tiled = symmetric(skel, loop, tiled)
    (a.out / "poses").mkdir(parents=True, exist_ok=True)
    ga.write_animation(a.out / "poses" / record, tiled, f"{path.name} tiled at {km.KIMODO_FPS} fps")

    spec = {"note": f"A loop keyed to the poses of {path.name} (tiled at {km.KIMODO_FPS} fps"
                    f"{', averaged with its mirror' if a.symmetric else ''}), {a.keys} keys a cycle.",
            "stance": a.stance, "samples": a.samples, "seed": a.seed, "steps": 100, "poses": {}, "moves": []}
    poses = km.Poses(a.models, {"stance": a.stance}, a.soma_ref, a.out / "poses")
    hips = lambda t: rs.gothic_to_soma(poses.skel, tiled, t, poses.rig, poses.names)[1]
    origin = hips(0)
    for per in (int(x) for x in a.keys.split(",")):
        frames = key_frames(cycle, per)
        count = frames[-1] + 1
        if count in km.BAD_FRAME_COUNTS:
            raise SystemExit(f"{per} keys a cycle make {count} frames, which Kimodo cuts by one: change --keys")
        keys = []
        for t in frames:
            spec["poses"][f"p{t}"] = {"clip": record, "frame": t}
            h = hips(t) - origin
            keys.append({"type": "fullbody", "frame": t, "pose": f"p{t}", "pos": [round(float(h[0]), 5), round(float(h[2]), 5)]})
        end = hips(frames[-1]) - origin
        spec["moves"].append({"id": f"{a.id}_k{per}", "frames": count, "travel": [0.0, round(float(end[2]), 4)],
                              "text": a.text, "keys": keys})
        print(f"{a.id}_k{per}: {count} frames, {len(frames)} keys at {frames}")
    out = a.out / f"moves-{a.id}.json"
    out.write_text(json.dumps(spec, indent=1))
    print(f"{out}; the cycle is {cycle:.2f} Kimodo frames, {(loop.frame_count - 1) / loop.fps:.3f} s")


if __name__ == "__main__":
    main()
