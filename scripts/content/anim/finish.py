"""Finish a retargeted move: retime it, hold the sword arm, write the MAN record.

    python3 scripts/content/anim/finish.py <in.MAN.json> <out.MAN.json> --ref <stance.MAN.json> \
        --name T_1HDASHB --warp 0:0,0.24:0.06,0.88:0.40,1.16:0.52 --sword-arm 0.5,0.8,1,1

--gen-stance with another --ref moves the ends onto that stance (one body motion for every talent
overlay). --warp maps source seconds to target seconds at a few keys (monotone, smooth between them): the
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
import retarget_soma as rs  # noqa: E402

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


def qmul(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    aw, ax, ay, az = np.moveaxis(a, -1, 0)
    bw, bx, by, bz = np.moveaxis(b, -1, 0)
    return np.stack([aw * bw - ax * bx - ay * by - az * bz, aw * bx + ax * bw + ay * bz - az * by,
                     aw * by - ax * bz + ay * bw + az * bx, aw * bz + ax * by - ay * bx + az * bw], -1)


def match_stance(anim: ga.Animation, gen: ga.Animation, target: ga.Animation, fade_in: float, fade_out: float) -> None:
    """Move a clip made from the stance `gen` onto the stance `target` at its ends.

    Each node gets the local difference target · gen⁻¹ at frame 0, fading out over `fade_in` seconds,
    and again fading in over the last `fade_out` seconds; the middle of the move stays as generated.
    One body motion then serves every talent overlay, each with its own S_1HRUN at both ends."""
    t = np.arange(anim.frame_count) / anim.fps
    end = (anim.frame_count - 1) / anim.fps
    s = lambda x: x * x * (3 - 2 * x)  # noqa: E731
    w = np.maximum(s(np.clip(1 - t / fade_in, 0, 1)), s(np.clip(1 - (end - t) / fade_out, 0, 1)))
    for j, node in enumerate(anim.node_indices):
        if node not in gen.node_indices or node not in target.node_indices:
            continue
        g, k = gen.node_indices.index(node), target.node_indices.index(node)
        conj = gen.rotations[0, g] * np.array([1.0, -1.0, -1.0, -1.0])
        delta = qmul(target.rotations[0, k], conj)
        ident = np.broadcast_to(np.array([1.0, 0, 0, 0]), (anim.frame_count, 4))
        step = slerp(ident, np.broadcast_to(delta, (anim.frame_count, 4)), w)
        anim.rotations[:, j] = qmul(step, anim.rotations[:, j])
        anim.positions[:, j] += w[:, None] * (target.positions[0, k] - gen.positions[0, g])


def hold_arm(anim: ga.Animation, skel: ga.Skeleton, ref: ga.Animation, weights: list[float]) -> None:
    for name, w in [*zip(ARM, weights), (FINGER, 1.0)]:
        node = skel.index(name)
        if node not in anim.node_indices or node not in ref.node_indices:
            continue
        j, k = anim.node_indices.index(node), ref.node_indices.index(node)
        target = np.broadcast_to(ref.rotations[0, k], anim.rotations[:, j].shape)
        anim.rotations[:, j] = slerp(anim.rotations[:, j], target, np.full(anim.frame_count, w))


def body_nodes(anim: ga.Animation, skel: ga.Skeleton, sources: list[ga.Animation], like: ga.Animation) -> None:
    """Write the nodes Gothic's own move writes (`like`: the overlay's jump-back, 27 nodes with the weapon slots
    ZS_RIGHTHAND and ZS_SWORD), the rest keep what was set before.

    Nodes Kimodo does not drive (toes, the weapon slots) hold the pose of the first of `sources` that has them
    (the stance, then the jump-back). A slot left to the bind pose points the sword at the ground; a toe left to
    it jumped tens of centimetres at the switch."""
    names = [n.name for n in skel.nodes]
    driven = {names.index(n) for n in rs.MAP if n in names}
    for j, node in enumerate(anim.node_indices):
        if node in driven:
            continue
        src = next((f for f in sources if node in f.node_indices), None)
        if src is not None:
            k = src.node_indices.index(node)
            anim.rotations[:, j] = src.rotations[0, k]
            anim.positions[:, j] = src.positions[0, k]
    cols = [anim.node_indices.index(n) for n in like.node_indices if n in anim.node_indices]
    anim.node_indices = [anim.node_indices[c] for c in cols]
    anim.positions = anim.positions[:, cols]
    anim.rotations = anim.rotations[:, cols]


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
    ap.add_argument("--gen-stance", type=Path, help="the stance the move was generated from, when --ref is another one "
                    "(a talent overlay's S_1HRUN): the ends are moved onto --ref")
    ap.add_argument("--fill", type=Path, action="append", default=[],
                    help="the overlay's jump-back: its node set is written, and it poses the toes and weapon slots the stance lacks")
    ap.add_argument("--fade", default="0.12,0.2", help="seconds of stance matching at the start and at the end")
    a = ap.parse_args()
    skel = ga.read_skeleton(a.skeleton or a.ref.parent / "HUMANS.MDH.json")
    ref = ga.read_animation(a.ref)
    anim = retime(ga.read_animation(a.src), [tuple(map(float, k.split(":"))) for k in a.warp.split(",")])
    if a.gen_stance:
        match_stance(anim, ga.read_animation(a.gen_stance), ref, *map(float, a.fade.split(",")))
    hold_arm(anim, skel, ref, [float(w) for w in a.sword_arm.split(",")])
    fills = [ga.read_animation(f) for f in a.fill]
    body_nodes(anim, skel, [ref] + fills, fills[0] if fills else ref)
    anim.name, anim.next = a.name, a.next
    a.out.parent.mkdir(parents=True, exist_ok=True)
    ga.write_animation(a.out, anim, source=f"finish:{a.src.name} warp {a.warp} arm {a.sword_arm}")
    tr = ga.root_travel(anim)
    print(f"{anim.name}: {anim.frame_count} frames = {anim.duration * 1000:.0f} ms, root travel "
          f"{np.round(tr[-1], 1).tolist()} cm")


if __name__ == "__main__":
    main()
