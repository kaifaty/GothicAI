"""Finish a retargeted move: retime it, hold the sword arm, write the MAN record.

    python3 scripts/content/anim/finish.py <in.MAN.json> <out.MAN.json> --ref <stance.MAN.json> \
        --name T_1HDASHB --warp 0:0,0.24:0.06,0.88:0.40,1.16:0.52 --sword-arm 0.5,0.8,1,1

Both ends are eased onto the stance --ref exactly (--fade seconds at the start and the end). --warp maps
source seconds to target seconds at a few keys (monotone, smooth between them): the
start, the active phase and the recovery are squeezed by different amounts. The clip ends at the last
key. --sword-arm blends the right clavicle, upper arm, forearm and hand towards the stance's local
rotations (0 keeps Kimodo's, 1 is the stance's); the finger always comes from the stance, since it
holds the hilt. Kimodo knows nothing of the sword, so its wrist swings the blade through the body.

Strikes: --arm-window keeps the swing and blends the arms to the stance only on the way back;
--start-ref / --end-ref land an end on another record (a charge windup ends in the pose its hold
loop starts from, the loop ends where it starts, the release starts there); a fade of 0 leaves an
end as it is.
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


def snap_ends(anim: ga.Animation, target: ga.Animation, fade_in: float, fade_out: float,
              end: ga.Animation | None = None) -> None:
    """Land both ends of the clip exactly on the stance `target` (frame 0 of its S_1HRUN), the end on `end`'s
    frame 0 when given (a windup ends in the pose its hold loop starts from). A fade of 0 leaves that end alone.

    Each node gets the local difference between the stance and the clip's first frame, fading out over
    `fade_in` seconds, and the difference to its last frame, fading in over the last `fade_out` seconds; the
    middle of the move stays as generated. This takes away what foot IK and Kimodo's constraint tolerance
    leave (2-5 cm). The root is snapped in height only: the engine moves the body by the root's way from the
    clip's own first frame, and Gothic's stances carry an arbitrary horizontal offset (S_1HRUN: -6, -33 cm);
    snapping it dragged the whole body and shortened the way."""
    t = np.arange(anim.frame_count) / anim.fps
    s = lambda x: x * x * (3 - 2 * x)  # noqa: E731
    last = (anim.frame_count - 1) / anim.fps
    w_in = s(np.clip(1 - t / fade_in, 0, 1)) if fade_in > 0 else np.zeros_like(t)
    w_out = s(np.clip(1 - (last - t) / fade_out, 0, 1)) if fade_out > 0 else np.zeros_like(t)
    rot0, pos0 = anim.rotations.copy(), anim.positions.copy()
    ident = np.broadcast_to(np.array([1.0, 0, 0, 0]), (anim.frame_count, 4))
    conj = np.array([1.0, -1.0, -1.0, -1.0])
    for j, node in enumerate(anim.node_indices):
        for w, f, tgt in ((w_in, 0, target), (w_out, -1, end or target)):
            if node not in tgt.node_indices:
                continue
            k = tgt.node_indices.index(node)
            delta = qmul(tgt.rotations[0, k], rot0[f, j] * conj)
            anim.rotations[:, j] = qmul(slerp(ident, np.broadcast_to(delta, (anim.frame_count, 4)), w), anim.rotations[:, j])
            dp = tgt.positions[0, k] - pos0[f, j]
            if node == 0:
                dp = dp * np.array([0.0, 1.0, 0.0])  # the engine takes the root's way from the clip's first frame
            anim.positions[:, j] += w[:, None] * dp


LEGS = {"L": ("BIP01 L THIGH", "BIP01 L CALF", "BIP01 L FOOT"), "R": ("BIP01 R THIGH", "BIP01 R CALF", "BIP01 R FOOT")}


def frame_skeleton(skel: ga.Skeleton, anim: ga.Animation, t: int) -> ga.Skeleton:
    """The skeleton with the clip's own bone offsets at frame t: Gothic's clips carry their own (Biped) offsets,
    and IK on another stance's lengths misses the target and drops the foot."""
    nodes = [ga.Node(nd.name, nd.parent, nd.rest.copy()) for nd in skel.nodes]
    for j, node in enumerate(anim.node_indices):
        nodes[node].rest[:3, 3] = anim.positions[t, j]
    return ga.Skeleton(nodes, skel.root_translation)


def lock_feet(anim: ga.Animation, skel: ga.Skeleton, height: float = 4.0, speed: float = 8.0,
              ends: float | None = None, keep_end: bool = False, turn: bool = False) -> None:
    """Pin each foot while it stands and bend the leg to it (two-bone IK on thigh and calf).

    A foot stands where its ankle is within `height` cm of its standing height (the stance at the clip's ends)
    and moves less than `speed` cm a frame (a swinging foot moves 20+). It is pinned where it touched down; a
    stand at the end of the clip is pinned where the stance puts it, so the snapped end stays. The offset
    between one pin and the next is spread over the swing between them, where it shows as no slide; before the
    first stand and after the last it fades to nothing. Squeezing time doubles a slide per frame, and easing
    the ends onto another stance moves the feet. `ends` (seconds) limits the stands to the start and the end of
    the clip: in a roll the feet also lie low and still on the ground, and pinning them there would bend the legs.
    `keep_end`: the end was landed on another record (a windup's on its hold): a foot that stands the whole clip
    goes over from where it starts to where that record puts it, instead of staying where it touched down.
    `turn`: a standing foot also keeps its turn about the vertical (Kimodo turns the feet of a still pose between
    its keys by 12-25°: the heel stays, the toe sweeps the ground)."""
    n = anim.frame_count
    worlds = [ga.world_pose(skel, anim, f) for f in range(n)]
    names = [nd.name for nd in skel.nodes]
    for chain_names in LEGS.values():
        if any(x not in names for x in chain_names):
            continue
        chain = tuple(names.index(x) for x in chain_names)
        if any(c not in anim.node_indices for c in chain[:2]):
            continue
        p = np.array([w[chain[2]][:3, 3] for w in worlds])
        toe = chain_names[2].replace("FOOT", "TOE0")
        yaw = np.zeros(n)
        if turn and toe in names:
            d = np.array([w[names.index(toe)][:3, 3] for w in worlds]) - p
            yaw = np.unwrap(np.arctan2(d[:, 0], d[:, 2]))
        v = np.r_[0.0, np.linalg.norm(np.diff(p[:, [0, 2]], axis=0), axis=1)]
        planted = (p[:, 1] < min(p[0, 1], p[-1, 1]) + height) & (v < speed)
        if ends is not None:
            t = np.arange(n) / anim.fps
            planted &= (t <= ends) | (t >= t[-1] - ends)
        spans, f = [], 0
        while f < n:
            if planted[f]:
                g = f
                while g + 1 < n and planted[g + 1]:
                    g += 1
                spans.append((f, g))
                f = g + 1
            else:
                f += 1
        if not spans:
            continue
        whole = keep_end and spans == [(0, n - 1)]
        corr = np.zeros((n, 4))  # x, y, z of the ankle and the foot's turn about the vertical (radians)
        if whole:
            # A foot that stands the whole clip between two snapped ends (a windup ending in its hold's pose)
            # goes from the one to the other: pinning it at the start would tear it off the end pose.
            u = np.linspace(0, 1, n)[:, None]
            corr[:, [0, 2]] = ((1 - u) * p[0] + u * p[n - 1])[:, [0, 2]] - p[:, [0, 2]]
            corr[:, 3] = (1 - u[:, 0]) * yaw[0] + u[:, 0] * yaw[n - 1] - yaw
            spans = []
        for f, g in spans:
            k = n - 1 if g == n - 1 and f > 0 else f
            corr[f:g + 1, [0, 2]] = p[k][[0, 2]] - p[f:g + 1][:, [0, 2]]
            corr[f:g + 1, 3] = yaw[k] - yaw[f:g + 1]
        for (_, g), (f, _) in zip(spans, spans[1:]):  # the swing between two stands carries the offset over
            for t in range(g + 1, f):
                u = (t - g) / (f - g)
                corr[t] = (1 - u) * corr[g] + u * corr[f]
        if spans:
            first, last = spans[0][0], spans[-1][1]
            for t in range(0, first):
                corr[t] = corr[first] * (t / first)
            for t in range(last + 1, n):
                corr[t] = corr[last] * ((n - 1 - t) / (n - 1 - last))
        for t in range(n):
            if np.linalg.norm(corr[t, :3]) < 0.05 and abs(corr[t, 3]) < 0.002:
                continue
            world_rot = np.array([m[:3, :3] for m in worlds[t]])
            foot = world_rot[chain[2]].copy()
            rs.two_bone_ik(world_rot, worlds[t][0][:3, 3], frame_skeleton(skel, anim, t), chain, p[t] + corr[t, :3])
            if turn and chain[2] in anim.node_indices:
                c, s_ = np.cos(corr[t, 3]), np.sin(corr[t, 3])
                local = world_rot[chain[1]].T @ np.array([[c, 0, s_], [0, 1, 0], [-s_, 0, c]]) @ foot
                j = anim.node_indices.index(chain[2])
                q = rs.mat_to_quat(local)
                anim.rotations[t, j] = -q if np.dot(q, anim.rotations[t, j]) < 0 else q
            for node in chain[:2]:
                local = world_rot[skel.nodes[node].parent].T @ world_rot[node]
                j = anim.node_indices.index(node)
                q = rs.mat_to_quat(local)
                anim.rotations[t, j] = -q if np.dot(q, anim.rotations[t, j]) < 0 else q


def lift_skims(anim: ga.Animation, skel: ga.Skeleton, low: float = 3.0, fast: float = 6.0) -> list[tuple]:
    """Lift a foot that skims the ground fast into a step.

    Kimodo now and then drags a foot along the floor 1-4 cm up, outside its own contact flags: a lunge's front
    foot pulled back, a landing that skids on. Squeezing time turns that into a skate of 20-30 cm a frame. Each
    run of frames where an ankle is within `low` cm of its standing height and moves more than `fast` cm a frame
    gets an arc from the last slow frame before it to the first after: up by a quarter of its way (3-8 cm) and
    down again, by leg IK. Returns the arcs as (side, first frame, last frame, height)."""
    n = anim.frame_count
    worlds = [ga.world_pose(skel, anim, f) for f in range(n)]
    names = [nd.name for nd in skel.nodes]
    arcs = []
    for side, chain_names in LEGS.items():
        if any(x not in names for x in chain_names):
            continue
        chain = tuple(names.index(x) for x in chain_names)
        if any(c not in anim.node_indices for c in chain[:2]):
            continue
        p = np.array([w[chain[2]][:3, 3] for w in worlds])
        v = np.r_[0.0, np.linalg.norm(np.diff(p[:, [0, 2]], axis=0), axis=1)]
        skim = (p[:, 1] < min(p[0, 1], p[-1, 1]) + low) & (v > fast)
        f = 0
        while f < n:
            if not skim[f]:
                f += 1
                continue
            g = f
            while g + 1 < n and skim[g + 1]:
                g += 1
            a, b = max(f - 1, 0), min(g + 1, n - 1)
            height = float(np.clip(np.linalg.norm(p[b, [0, 2]] - p[a, [0, 2]]) / 4, 3.0, 8.0))
            for t in range(a + 1, b):
                world_rot = np.array([m[:3, :3] for m in worlds[t]])
                lift = np.array([0.0, height * np.sin(np.pi * (t - a) / (b - a)), 0.0])
                rs.two_bone_ik(world_rot, worlds[t][0][:3, 3], frame_skeleton(skel, anim, t), chain, p[t] + lift)
                for node in chain[:2]:
                    local = world_rot[skel.nodes[node].parent].T @ world_rot[node]
                    j = anim.node_indices.index(node)
                    q = rs.mat_to_quat(local)
                    anim.rotations[t, j] = -q if np.dot(q, anim.rotations[t, j]) < 0 else q
            arcs.append((side, a, b, round(height, 1)))
            f = g + 1
    return arcs


def hold_arm(anim: ga.Animation, skel: ga.Skeleton, ref: ga.Animation, weights: list[float], side: str = "R",
             window: tuple[float, ...] | None = None) -> None:
    """Blend clavicle, upper arm, forearm and hand of one side towards the stance's local rotations. The sword
    hand's finger always comes from the stance; the free (left) arm is only damped, to calm Kimodo's swing.
    `window` (seconds): (t0, t1) — the arm is the clip's own before t0 and blends in to the full weights by t1: a
    strike keeps its swing and only the way back to the stance takes the stance's arm. (a, b, c, d) — the stance's
    arm at the start hands over to the clip's own from a to b and comes back from c to d: Kimodo turns a free wrist
    at once (a full-body key pins joints, not the wrist's twist), and the end snap alone flicked the blade 40-100° a
    frame."""
    chain = [n.replace(" R ", f" {side} ") for n in ARM]
    extra = [(FINGER, 1.0)] if side == "R" else []
    ramp = np.ones(anim.frame_count)
    if window is not None:
        t = np.arange(anim.frame_count) / anim.fps
        def up(t0, t1):
            x = np.clip((t - t0) / max(t1 - t0, 1e-6), 0, 1)
            return x * x * (3 - 2 * x)
        ramp = up(*window[-2:])
        if len(window) == 4:
            ramp = np.maximum(1 - up(*window[:2]), ramp)
    for (name, w), r in [*((c, ramp) for c in zip(chain, weights)), *((e, np.ones(anim.frame_count)) for e in extra)]:
        node = skel.index(name)
        if node not in anim.node_indices or node not in ref.node_indices:
            continue
        j, k = anim.node_indices.index(node), ref.node_indices.index(node)
        target = np.broadcast_to(ref.rotations[0, k], anim.rotations[:, j].shape)
        anim.rotations[:, j] = slerp(anim.rotations[:, j], target, w * r)


def body_nodes(anim: ga.Animation, skel: ga.Skeleton, sources: list[ga.Animation], like: ga.Animation) -> None:
    """Write the nodes Gothic's own move writes (`like`: the overlay's jump-back, 27 nodes with the weapon slots
    ZS_RIGHTHAND and ZS_SWORD), the rest keep what was set before.

    Nodes Kimodo does not drive (toes, the weapon slots) hold the pose of the first of `sources` that has them
    (the stance, then the jump-back). A slot left to the bind pose points the sword at the ground; a toe left to
    it jumped tens of centimetres at the switch."""
    names = [n.name for n in skel.nodes]
    # BIP01 is not in the map: the retarget derives it from the pelvis, and it carries the root motion
    driven = {names.index(n) for n in [*rs.MAP, "BIP01"] if n in names}
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
    ap.add_argument("--free-arm", default="0,0,0,0", help="the left arm's weights towards the stance (damps its swing)")
    ap.add_argument("--fill", type=Path, action="append", default=[],
                    help="the overlay's jump-back: its node set is written, and it poses the toes and weapon slots the stance lacks")
    ap.add_argument("--no-foot-lock", action="store_true", help="leave the feet as retargeted")
    ap.add_argument("--lock-ends", type=float, help="pin feet only this many seconds from the start and the end (rolls)")
    ap.add_argument("--lock-turn", action="store_true", help="a standing foot also keeps its turn about the vertical")
    ap.add_argument("--lift-skims", type=float, nargs="?", const=6.0, metavar="CM_A_FRAME",
                    help="lift a foot that skims the floor faster than this (default 6) into a step")
    ap.add_argument("--lock-speed", type=float, default=8.0, help="cm a frame under which a low foot stands (a "
                    "shuffle of a few cm a frame in a slow move is a step, not a stand)")
    ap.add_argument("--fade", default="0.12,0.2", help="seconds of stance matching at the start and at the end")
    ap.add_argument("--start-ref", type=Path, help="land the start on this record's frame 0 instead of --ref")
    ap.add_argument("--end-ref", type=Path, help="land the end on this record's frame 0 instead of --ref (a windup "
                    "ends where its hold loop starts; a loop ends where it starts)")
    ap.add_argument("--arm-window", help="t0,t1 seconds: the sword arm is the clip's own before t0 and blends to "
                    "--sword-arm by t1 (strikes keep their swing); a,b,c,d: the stance's at the start, own from a to b, "
                    "the stance's again from c to d")
    ap.add_argument("--free-window", help="the same for the free arm and --free-arm; default --arm-window")
    a = ap.parse_args()
    skel = ga.read_skeleton(a.skeleton or a.ref.parent / "HUMANS.MDH.json")
    ref = ga.read_animation(a.ref)
    anim = retime(ga.read_animation(a.src), [tuple(map(float, k.split(":"))) for k in a.warp.split(",")])
    start = ga.read_animation(a.start_ref) if a.start_ref else ref
    snap_ends(anim, start, *map(float, a.fade.split(",")), end=ga.read_animation(a.end_ref) if a.end_ref else ref)
    window = tuple(map(float, a.arm_window.split(","))) if a.arm_window else None
    free = tuple(map(float, a.free_window.split(","))) if a.free_window else window
    hold_arm(anim, skel, ref, [float(w) for w in a.sword_arm.split(",")], window=window)
    hold_arm(anim, skel, ref, [float(w) for w in a.free_arm.split(",")], side="L", window=free)
    if not a.no_foot_lock:
        lock_feet(anim, rs.with_offsets(skel, ref), speed=a.lock_speed, ends=a.lock_ends, keep_end=a.end_ref is not None,
                  turn=a.lock_turn)
    if a.lift_skims is not None:
        for arc in lift_skims(anim, rs.with_offsets(skel, ref), fast=a.lift_skims):
            print("lifted a skimming foot:", arc)
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
