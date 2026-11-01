"""Finish a retargeted move: retime it, hold the sword arm, write the MAN record.

    python3 scripts/content/anim/finish.py <in.MAN.json> <out.MAN.json> --ref <stance.MAN.json> \
        --name T_1HDASHB --warp 0:0,0.24:0.06,0.88:0.40,1.16:0.52 --sword-arm 0.5,0.8,1,1

Both ends are eased onto the stance --ref exactly (--fade seconds at the start and the end). --warp maps
source seconds to target seconds at a few keys (monotone, smooth between them): the
start, the active phase and the recovery are squeezed by different amounts. The clip ends at the last
key. --sword-arm blends the right clavicle, upper arm, forearm and hand towards the stance's local
rotations (0 keeps Kimodo's, 1 is the stance's); the finger always comes from the stance, since it
holds the hilt. Kimodo knows nothing of the sword, so its wrist swings the blade through the body.
--clear-sword (rolls) then moves that held arm aside where it would put the hand or the blade into the floor or
the body: the stance arm turns with the chest and in a tumble went up to 70 cm under the floor.
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


def snap_ends(anim: ga.Animation, target: ga.Animation, fade_in: float, fade_out: float) -> None:
    """Land both ends of the clip exactly on the stance `target` (frame 0 of its S_1HRUN).

    Each node gets the local difference between the stance and the clip's first frame, fading out over
    `fade_in` seconds, and the difference to its last frame, fading in over the last `fade_out` seconds; the
    middle of the move stays as generated. This takes away what foot IK and Kimodo's constraint tolerance
    leave (2-5 cm). The root is snapped in height only: the engine moves the body by the root's way from the
    clip's own first frame, and Gothic's stances carry an arbitrary horizontal offset (S_1HRUN: -6, -33 cm);
    snapping it dragged the whole body and shortened the way."""
    t = np.arange(anim.frame_count) / anim.fps
    end = (anim.frame_count - 1) / anim.fps
    s = lambda x: x * x * (3 - 2 * x)  # noqa: E731
    w_in = s(np.clip(1 - t / fade_in, 0, 1))
    w_out = s(np.clip(1 - (end - t) / fade_out, 0, 1))
    rot0, pos0 = anim.rotations.copy(), anim.positions.copy()
    ident = np.broadcast_to(np.array([1.0, 0, 0, 0]), (anim.frame_count, 4))
    conj = np.array([1.0, -1.0, -1.0, -1.0])
    for j, node in enumerate(anim.node_indices):
        if node not in target.node_indices:
            continue
        k = target.node_indices.index(node)
        for w, f in ((w_in, 0), (w_out, -1)):
            delta = qmul(target.rotations[0, k], rot0[f, j] * conj)
            anim.rotations[:, j] = qmul(slerp(ident, np.broadcast_to(delta, (anim.frame_count, 4)), w), anim.rotations[:, j])
            dp = target.positions[0, k] - pos0[f, j]
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
              ends: float | None = None) -> None:
    """Pin each foot while it stands and bend the leg to it (two-bone IK on thigh and calf).

    A foot stands where its ankle is within `height` cm of its standing height (the stance at the clip's ends)
    and moves less than `speed` cm a frame (a swinging foot moves 20+). It is pinned where it touched down; a
    stand at the end of the clip is pinned where the stance puts it, so the snapped end stays. The offset
    between one pin and the next is spread over the swing between them, where it shows as no slide; before the
    first stand and after the last it fades to nothing. Squeezing time doubles a slide per frame, and easing
    the ends onto another stance moves the feet. `ends` (seconds) limits the stands to the start and the end of
    the clip: in a roll the feet also lie low and still on the ground, and pinning them there would bend the legs."""
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
        corr = np.zeros((n, 3))
        for f, g in spans:
            pin = p[n - 1] if g == n - 1 and f > 0 else p[f]
            corr[f:g + 1, [0, 2]] = pin[[0, 2]] - p[f:g + 1][:, [0, 2]]
        for (_, g), (f, _) in zip(spans, spans[1:]):  # the swing between two stands carries the offset over
            for t in range(g + 1, f):
                u = (t - g) / (f - g)
                corr[t] = (1 - u) * corr[g] + u * corr[f]
        first, last = spans[0][0], spans[-1][1]
        for t in range(0, first):
            corr[t] = corr[first] * (t / first)
        for t in range(last + 1, n):
            corr[t] = corr[last] * ((n - 1 - t) / (n - 1 - last))
        for t in range(n):
            if np.linalg.norm(corr[t]) < 0.05:
                continue
            world_rot = np.array([m[:3, :3] for m in worlds[t]])
            rs.two_bone_ik(world_rot, worlds[t][0][:3, 3], frame_skeleton(skel, anim, t), chain, p[t] + corr[t])
            for node in chain[:2]:
                local = world_rot[skel.nodes[node].parent].T @ world_rot[node]
                j = anim.node_indices.index(node)
                q = rs.mat_to_quat(local)
                anim.rotations[t, j] = -q if np.dot(q, anim.rotations[t, j]) < 0 else q


def hold_arm(anim: ga.Animation, skel: ga.Skeleton, ref: ga.Animation, weights: list[float], side: str = "R") -> None:
    """Blend clavicle, upper arm, forearm and hand of one side towards the stance's local rotations. The sword
    hand's finger always comes from the stance; the free (left) arm is only damped, to calm Kimodo's swing."""
    chain = [n.replace(" R ", f" {side} ") for n in ARM]
    extra = [(FINGER, 1.0)] if side == "R" else []
    for name, w in [*zip(chain, weights), *extra]:
        node = skel.index(name)
        if node not in anim.node_indices or node not in ref.node_indices:
            continue
        j, k = anim.node_indices.index(node), ref.node_indices.index(node)
        target = np.broadcast_to(ref.rotations[0, k], anim.rotations[:, j].shape)
        anim.rotations[:, j] = slerp(anim.rotations[:, j], target, np.full(anim.frame_count, w))


DETOUR = 4  # steps of 5° past a forearm's smallest clearing shoulder turn that the path may take
STEP = 6.0  # a step of x° between two frames costs x²/STEP: 6° as much as 6° off the held arm, 60° ten times more
BLADE_CM = 90.0  # a long 1H sword lies along ZS_RIGHTHAND's +X: the base's sword meshes run 1-89 cm along it
# What the sword arm keeps out of: capsules (from node, to node, radius cm); the head's runs 14 cm past its node.
# The last is the torso's shadow on the floor: a blade kept just off the floor slid under the rolling body.
BODY = (("BIP01 PELVIS", "BIP01 NECK", 14.0), ("BIP01 NECK", "BIP01 HEAD", 11.0), ("BIP01 HEAD", None, 11.0),
        ("BIP01 L THIGH", "BIP01 L CALF", 8.0), ("BIP01 L CALF", "BIP01 L FOOT", 6.0),
        ("BIP01 R THIGH", "BIP01 R CALF", 8.0), ("BIP01 R CALF", "BIP01 R FOOT", 6.0), ("BIP01 PELVIS", "BIP01 NECK", 18.0))


def fibonacci_sphere(n: int) -> np.ndarray:
    i = np.arange(n) + 0.5
    y = 1 - 2 * i / n
    r, a = np.sqrt(1 - y * y), np.pi * (1 + 5 ** 0.5) * i
    return np.stack([r * np.cos(a), y, r * np.sin(a)], axis=1)


def rotvec_matrix(v: np.ndarray) -> np.ndarray:
    """Rotation matrices from rotation vectors (axis times angle in radians), any leading shape."""
    angle = np.linalg.norm(v, axis=-1)[..., None, None]
    k = v / np.maximum(angle[..., 0], 1e-12)
    kx = np.zeros(v.shape[:-1] + (3, 3))
    kx[..., 0, 1], kx[..., 0, 2], kx[..., 1, 2] = -k[..., 2], k[..., 1], -k[..., 0]
    kx = kx - np.swapaxes(kx, -1, -2)
    return np.eye(3) + np.sin(angle) * kx + (1 - np.cos(angle)) * kx @ kx


def axis_matrix(axis: np.ndarray, angle: float) -> np.ndarray:
    return rotvec_matrix(axis / max(np.linalg.norm(axis), 1e-12) * angle)


def clear_sword(anim: ga.Animation, skel: ga.Skeleton, floor: float, hand: float = 5.0, blade: float = 5.0,
                spare: float = 4.0) -> dict:
    """Keep the sword hand and the blade off the floor and out of the body: in a roll the held stance arm turns
    with the chest and drove the hand 4-38 cm and the blade 35-52 cm under the floor.

    A clear arm puts the elbow and the fist `hand` cm and every point of the blade `blade` cm over `floor`, the
    blade clear of the torso, head and legs and of the floor under the torso, the fist clear of the torso and head. To get there the arm may
    twist the forearm about its own axis (up to 90°, which turns the blade about the arm), bend the elbow more
    (up to 50°, never past 150°) and turn as a whole at the shoulder; the wrist keeps its bend. Each frame offers
    its smallest clear changes, and the path through the frames pays for every change (a degree at the shoulder
    twice a degree of twist or bend) and for the square of every step between frames, so the arm moves aside
    early and smoothly instead of flipping. A changed arm is asked `spare` cm more, which a light smoothing then
    spends. The first and the last frame are the stance and stay. Returns the worst clearance (cm) before and
    after, the stance's own (0 unless it already holds the blade within the margins) and the biggest changes."""
    names = [nd.name for nd in skel.nodes]
    need = ("BIP01 R UPPERARM", "BIP01 R FOREARM", "BIP01 R HAND", "ZS_RIGHTHAND", "BIP01 NECK")
    if any(x not in names for x in need):
        return {}
    ua, fa, hd, slot, neck = (names.index(x) for x in need)
    fist = [hd, slot] + ([names.index(FINGER)] if FINGER in names else [])
    caps = [(names.index(a), names.index(b) if b else None, r)
            for a, b, r in BODY if a in names and (b is None or b in names)]
    n = anim.frame_count
    worlds = [ga.world_pose(skel, anim, f) for f in range(n)]
    twists = np.radians(np.arange(-90, 91, 15))
    bends = np.radians([0.0, 25.0, 50.0])
    axes = fibonacci_sphere(200)
    angles = np.radians(np.arange(5, 181, 5))
    turns = rotvec_matrix(axes[None] * angles[:, None, None])  # (angles, axes, 3, 3)

    def gap(x, cap):  # distance of points x (..., 3) to a capsule's surface
        a, b, r = cap
        ab = b - a
        t = np.clip(((x - a) @ ab) / max(ab @ ab, 1e-9), 0, 1)
        return np.linalg.norm(x - (a + t[..., None] * ab), axis=-1) - r

    def clearance(q, body, extra):
        """Worst margin of arm points q (..., 14, 3): the elbow, 3 fist points, 10 blade points; >= 0 is clear."""
        worst = np.minimum(q[..., :4, 1].min(-1) - floor - hand, q[..., 4:, 1].min(-1) - floor - blade) - extra
        for k, cap in enumerate(body):
            worst = np.minimum(worst, gap(q[..., 4:, :], cap).min(-1) - 1)
            if k < 3:  # the fist may touch the chest and the head, not sink into them
                worst = np.minimum(worst, gap(q[..., 1:4, :], cap).min(-1) + 3)
        return worst - allow

    frames = []  # per frame: the shoulder, the arm's points from it, the elbow's hinge, the forearm, its bend, the body
    for w in worlds:
        s, e, h = (w[i][:3, 3] for i in (ua, fa, hd))
        tip = w[slot][:3, :3] @ np.array([BLADE_CM, 0, 0])
        arm = np.vstack([[e], [w[i][:3, 3] for i in fist], w[slot][:3, 3] + np.linspace(0.1, 1, 10)[:, None] * tip]) - s
        hinge = np.cross(e - s, h - e)
        if np.linalg.norm(hinge) < 1e-3 * np.linalg.norm(e - s) * np.linalg.norm(h - e):
            hinge = w[ua][:3, :3] @ np.array([0.0, 0.0, 1.0])  # a straight arm bends about the upper arm's Z
        bend = np.arccos(np.clip(np.dot(e - s, h - e) / (np.linalg.norm(e - s) * np.linalg.norm(h - e)), -1, 1))
        body = []
        for a, b, r in caps:
            pa = w[a][:3, 3]
            up = pa - w[neck][:3, 3]
            body.append((pa, w[b][:3, 3] if b is not None else pa + 14 * up / max(np.linalg.norm(up), 1e-6), r))
        if len(body) == len(BODY):
            a, b, r = body[-1]
            body[-1] = (a * [1, 0, 1] + [0, floor, 0], b * [1, 0, 1] + [0, floor, 0], r)
        frames.append((s, arm, hinge, h - e, bend, body))
    # The ends are the stance and stay as they are; where the stance itself holds the blade nearer a thigh than
    # the margins, no frame is asked for more than it gives
    allow = 0.0  # clearance() reads it
    allow = min(0.0, *(float(clearance(frames[f][1] + frames[f][0], frames[f][5], spare)) for f in (0, n - 1)))

    def forearm(f, tw, bd):
        _, _, hinge, along, _, _ = frames[f]
        return axis_matrix(hinge, bd) @ axis_matrix(along, tw)

    def posed(f, p):
        """The arm's points after the forearm's twist and bend at the elbow and the shoulder's turn, in the world."""
        s, arm = frames[f][:2]
        q = np.vstack([arm[:1], (arm[1:] - arm[0]) @ forearm(f, p[3], p[4]).T + arm[0]])
        return q @ rotvec_matrix(p[:3]).T + s

    def size(p):  # degrees off the held arm: the shoulder counts twice the forearm
        return np.degrees(np.linalg.norm(p[..., :3], axis=-1)) + 0.5 * np.degrees(np.abs(p[..., 3]) + np.abs(p[..., 4]))

    def step(d):  # degrees of a step between frames, by how far it swings the blade: a twist almost as far as a turn
        return np.degrees(np.linalg.norm(d[..., :3], axis=-1)) + 0.7 * np.degrees(np.abs(d[..., 3]) + np.abs(d[..., 4]))

    def states(f):
        """Clear arms of a frame: for each forearm twist and bend, the smallest shoulder turns that clear and those
        up to DETOUR steps (of 5°) bigger, up to 8 axes a step, or no turn when none is needed; the clearest arm if
        nothing clears. The detours let the path keep the forearm's twist instead of flipping it. An arm that
        clears without the spare is one more state: asking the spare of it bent the arm right after the start, and
        making it the only state snapped a twisted arm back in one frame."""
        s, arm, _, _, bend, body = frames[f]
        if f in (0, n - 1):
            return np.zeros((1, 5))  # the ends are the stance
        keys = [(tw, bd) for tw in twists for bd in bends if bend + bd <= np.radians(150)]
        local = np.array([posed(f, np.r_[0, 0, 0, k]) - s for k in keys])  # (keys, 14, 3)
        out, found, best = [np.zeros(5)] if clearance(arm + s, body, 0) >= 0 else [], {}, (-np.inf, np.zeros(5))
        for i in range(len(angles) + 1):
            rots, vecs = (np.eye(3)[None], np.zeros((1, 3))) if i == 0 else (turns[i - 1], axes * angles[i - 1])
            c = clearance(np.einsum("aij,cpj->acpi", rots, local) + s, body, spare)  # (turns, keys)
            if c.max() > best[0]:
                ai, ci = np.unravel_index(np.argmax(c), c.shape)
                best = (c.max(), np.r_[vecs[ai], keys[ci]])
            for ci in range(len(keys)):
                if found.get(ci, i) < i - DETOUR or (found.get(ci) == 0 and i > 0):
                    continue  # this forearm already has its smallest turns and the detours past them
                ok = np.flatnonzero(c[:, ci] >= 0)
                if len(ok):
                    out += [np.r_[vecs[ai], keys[ci]] for ai in ok[::max(1, len(ok) // 8)][:8]]
                    found.setdefault(ci, i)
            if len(found) == len(keys) and all(i - v >= DETOUR or v == 0 for v in found.values()):
                break
        return np.array(out) if out else best[1][None]

    def worst(params):
        return min(float(clearance(posed(f, params[f]), frames[f][5], 0)) for f in range(n))

    before = worst(np.zeros((n, 5)))
    # The cheapest path through every frame's clear arms. A step between frames costs its square: a greedy pick
    # flipped the forearm's twist from +90° to -60° between two frames, and so did a path paying the step linearly,
    # which costs a flip no more than the same turn spread over five frames; a twist priced at half a turn still
    # snapped back 60° in one frame, a swing of 80 cm at the tip
    cand = [states(f) for f in range(n)]
    # A neighbour's arm that clears this frame too lets the path hold a pose: the smallest turns of a frame that
    # clears with less left no state near the last frame's, and the arm sprang 15° and back right after the start
    cand = [c if f in (0, n - 1) else np.vstack([c] + [p[None] for p in np.vstack([cand[f - 1], cand[f + 1]])
                                                       if clearance(posed(f, p), frames[f][5], 0) >= 0])
            for f, c in enumerate(cand)]
    cost, back = size(cand[0]), []
    for f in range(1, n):
        d = cand[f - 1][:, None, :] - cand[f][None, :, :]
        total = cost[:, None] + step(d) ** 2 / STEP
        back.append(np.argmin(total, axis=0))
        cost = total[back[-1], np.arange(len(cand[f]))] + size(cand[f])
    pick = [int(np.argmin(cost))]
    for f in range(n - 1, 0, -1):
        pick.append(int(back[f - 1][pick[-1]]))
    raw = np.array([cand[f][k] for f, k in enumerate(reversed(pick))])  # shoulder turn (rotation vector), twist, bend
    kern = np.exp(-0.5 * (np.arange(-2, 3) / 0.8) ** 2)
    pad = np.pad(raw, ((2, 2), (0, 0)), mode="edge")
    params = np.array([(kern[:, None] * pad[f:f + 5]).sum(0) / kern.sum() for f in range(n)])
    for f in range(n):  # where the smoothing spent more than the spare, the frame keeps its own clear arm
        if clearance(posed(f, params[f]), frames[f][5], 0) < 0 <= clearance(posed(f, raw[f]), frames[f][5], 0):
            params[f] = raw[f]
    params[[0, n - 1]] = 0.0
    sizes = size(params)
    after = worst(params)
    for f in range(n):
        if size(params[f]) < 0.05:
            continue
        w = worlds[f]
        turn = rotvec_matrix(params[f, :3])
        for node, local in ((ua, w[skel.nodes[ua].parent][:3, :3].T @ turn @ w[ua][:3, :3]),
                            (fa, w[ua][:3, :3].T @ forearm(f, params[f, 3], params[f, 4]) @ w[fa][:3, :3])):
            j = anim.node_indices.index(node)
            q = rs.mat_to_quat(local)
            anim.rotations[f, j] = -q if np.dot(q, anim.rotations[f, j]) < 0 else q
    return {"before_cm": round(before + allow, 1), "after_cm": round(after + allow, 1), "stance_cm": round(allow, 1),
            "frames_changed": int((sizes > 0.5).sum()),
            "max_shoulder_deg": round(float(np.degrees(np.linalg.norm(params[:, :3], axis=1)).max()), 1),
            "max_twist_deg": round(float(np.degrees(np.abs(params[:, 3])).max()), 1),
            "max_bend_deg": round(float(np.degrees(params[:, 4]).max()), 1)}


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
    ap.add_argument("--fade", default="0.12,0.2", help="seconds of stance matching at the start and at the end")
    ap.add_argument("--clear-sword", action="store_true",
                    help="turn the sword arm at the shoulder so hand and blade stay off the floor and out of the body (rolls)")
    a = ap.parse_args()
    skel = ga.read_skeleton(a.skeleton or a.ref.parent / "HUMANS.MDH.json")
    ref = ga.read_animation(a.ref)
    anim = retime(ga.read_animation(a.src), [tuple(map(float, k.split(":"))) for k in a.warp.split(",")])
    snap_ends(anim, ref, *map(float, a.fade.split(",")))
    hold_arm(anim, skel, ref, [float(w) for w in a.sword_arm.split(",")])
    hold_arm(anim, skel, ref, [float(w) for w in a.free_arm.split(",")], side="L")
    if not a.no_foot_lock:
        lock_feet(anim, rs.with_offsets(skel, ref), ends=a.lock_ends)
    fills = [ga.read_animation(f) for f in a.fill]
    body_nodes(anim, skel, [ref] + fills, fills[0] if fills else ref)
    if a.clear_sword:
        w0 = ga.world_pose(skel, ref, 0)
        # the toe nodes of the stance sit about 3 cm over the sole
        floor = min(w0[skel.index(f"BIP01 {s} TOE0")][1, 3] for s in "LR") - 3
        print("sword clearance", clear_sword(anim, skel, floor))
    anim.name, anim.next = a.name, a.next
    a.out.parent.mkdir(parents=True, exist_ok=True)
    ga.write_animation(a.out, anim, source=f"finish:{a.src.name} warp {a.warp} arm {a.sword_arm}")
    tr = ga.root_travel(anim)
    print(f"{anim.name}: {anim.frame_count} frames = {anim.duration * 1000:.0f} ms, root travel "
          f"{np.round(tr[-1], 1).tolist()} cm")


if __name__ == "__main__":
    main()
