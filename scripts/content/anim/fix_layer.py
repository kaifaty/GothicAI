"""A layer of small fixes over a finished move: each fix names a limb or a bone and a window of frames, eases in
and out there, and leaves every other frame of the record exactly as it was.

    python3 scripts/content/anim/fix_layer.py <base models> <in.MAN.json> <fixes.json> <out.MAN.json>

fixes.json: {"stance": "HUMANS-S_1HRUN", "fixes": [fix, ...]}, applied in order; the stance's toes give the floor.
A window is [a, b, c, d] in frames: nothing up to a, easing in to full at b, full to c, easing out to nothing at d.
Kinds:

- step  {"limb": "L leg", "from": 6, "to": 10, "lift": 10}: the foot leaves its place at `from` and lands where it
        is at `to` in an arc `lift` cm high, instead of sliding along the ground; its turn goes over smoothly;
- lift  {"limb": "R leg", "window": [...], "cm": 10}: the foot or hand rises, keeping its turn (a foot turning on
        the spot clears the ground);
- tuck  {"limb": "L leg", "window": [...], "k": 0.7}: the knee or elbow bends more, the end comes towards the hip or
        shoulder to the share k;
- soles {"window": [...], "clear": 3, "step": 12}: wherever a toe node is under `clear` cm over the floor, the foot
        pitches up just enough; the angle eases in and out by at most `step` degrees a frame, and the first and
        last frames keep the stance they were snapped to;
- rot   {"bone": "BIP01 L THIGH", "window": [...], "axis": [x, y, z], "deg": 20, "space": "local" | "model"};
- calm  {"bone": "BIP01 L THIGH", "window": [...], "sigma": 1.5}: the bone's own turn smoothed over frames, which
        spreads a whip over its neighbours;
- hold  {"bones": ["BIP01 R UPPERARM", ...], "frame": 12, "window": [...]}: the bones keep their own turn of `frame`,
        so a limb stays still against its parent while the body turns: a sword arm rides a roll over the shoulder
        instead of flipping the blade from one side of the body to the other;
- keep  {"bones": ["BIP01 R CLAVICLE"], "window": [...]}: the bones keep the model-space turn they had before this
        layer, so a fix of a parent leaves the limb below it where it was: both clavicles hang on the neck, and a calm
        of the neck would swing the sword arm;
- raise {"window": [...], "cm": 12}: the whole body rises, by the root's height only: the engine moves the NPC by
        the root's XZ, so the path stays. For a trunk that rolls over a shoulder sunk into the floor; a hand or foot
        on the ground rises with it, and a later `lift` with negative cm puts it back;
- wrist {"limbs": ["L arm", "R arm"], "window": [...], "max": 10}: a hand bent off its forearm's line by more than
        `max` degrees comes back to it, keeping its twist: Kimodo's runners carry a limp, bent wrist;
- pelvis {"window": [...], "roll": 0.5}: the hips' roll seen from the front (one hip up, the other down) keeps the
        share `roll` of its swing about the cycle's mean; the trunk keeps its turn and the legs reach the same feet,
        so only the hips move. A strong hip drop reads as a woman's gait (Gothic's own female run: 11°, male: 5°).
        The roll is seen along the record's travel; a record that stays in place names it: "axis": [x, 0, z].
- steady {"bones": ["BIP01 NECK", "BIP01 HEAD"], "window": [...], "share": 0.4}: the last bone keeps the share of its
        swing in the world about its mean over the record, and the turn that takes is spread evenly over the listed
        bones: a runner holds the head still while the chest swings. Both clavicles hang on the neck: `keep` them.

{"loop": true} in fixes.json: the record is a moving loop - its last frame is the first again, moved by the
travel (loop_cycle.py). Windows may then run past the last cycle frame and go on from frame 0 (a window [18, 23,
25, 29] in a 21-frame cycle covers 18..20 and 0..8), and the last frame is rebuilt from the first.

The legs and arms are solved by two-bone IK: the knee or elbow stays in its own plane. With `"joint_clear": cm` on
a step, lift or tuck the plane turns only as far as it takes to keep the knee or elbow that high over the floor.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gothic_anim as ga
import retarget_soma as rs

LIMBS = {"L leg": ("BIP01 L THIGH", "BIP01 L CALF", "BIP01 L FOOT"), "R leg": ("BIP01 R THIGH", "BIP01 R CALF", "BIP01 R FOOT"),
         "L arm": ("BIP01 L UPPERARM", "BIP01 L FOREARM", "BIP01 L HAND"),
         "R arm": ("BIP01 R UPPERARM", "BIP01 R FOREARM", "BIP01 R HAND")}
UP = np.array([0.0, 1.0, 0.0])


def window_of(fx: dict) -> list[int]:
    return fx.get("window") or [fx["from"], fx["from"], fx["to"], fx["to"]]


def weight(window, f: int) -> float:
    a, b, c, d = window
    if f < a or f > d:
        return 0.0
    if b <= f <= c:
        return 1.0
    x = (f - a) / (b - a) if f < b else (d - f) / (d - c)
    return x * x * (3 - 2 * x)


def weight_at(window, f: int, cycle: int = 0) -> float:
    """weight() of a loop: a window may run past the cycle's last frame and go on from its first."""
    return max(weight(window, f), weight(window, f + cycle)) if cycle else weight(window, f)


def rot(axis, rad: float) -> np.ndarray:
    k = np.asarray(axis, float) / np.linalg.norm(axis)
    K = np.array([[0, -k[2], k[1]], [k[2], 0, -k[0]], [-k[1], k[0], 0]])
    return np.eye(3) + np.sin(rad) * K + (1 - np.cos(rad)) * K @ K


def between(u: np.ndarray, v: np.ndarray) -> np.ndarray:
    """The smallest turn taking direction u onto direction v."""
    u, v = u / np.linalg.norm(u), v / np.linalg.norm(v)
    k = np.cross(u, v)
    s = np.linalg.norm(k)
    return np.eye(3) if s < 1e-9 else rot(k / s, np.arctan2(s, float(u @ v)))


def quat(m: np.ndarray) -> np.ndarray:
    q = rs.mat_to_quat(m)
    return q / np.linalg.norm(q)


def slerp_m(A: np.ndarray, B: np.ndarray, t: float) -> np.ndarray:
    return ga.quat_matrix(rs.slerp(quat(A), quat(B), t))


class Clip:
    def __init__(self, skel: ga.Skeleton, anim: ga.Animation):
        self.skel, self.a = skel, anim
        self.col = {node: c for c, node in enumerate(anim.node_indices)}
        self.below = [[j for j in range(len(skel.nodes)) if self.descends(j, i)] for i in range(len(skel.nodes))]

    def descends(self, j: int, i: int) -> bool:
        p = self.skel.nodes[j].parent
        while p >= 0:
            if p == i:
                return True
            p = self.skel.nodes[p].parent
        return False

    def world(self, f: int) -> np.ndarray:
        return ga.world_pose(self.skel, self.a, f)

    def set_world_rot(self, f: int, W: np.ndarray, name: str, R: np.ndarray):
        """Give node `name` the model-space turn R at frame f; W (that frame's pose) follows down the chain."""
        i = self.skel.index(name)
        self.a.rotations[f, self.col[i]] = quat(W[self.skel.nodes[i].parent][:3, :3].T @ R)
        for j in [i] + self.below[i]:
            c = self.col.get(j)
            L = ga.sample_matrix(self.a.positions[f, c], self.a.rotations[f, c]) if c is not None else self.skel.nodes[j].rest
            W[j] = W[self.skel.nodes[j].parent] @ L


def two_bone(clip: Clip, f: int, W: np.ndarray, limb: str, target: np.ndarray, end_rot: np.ndarray,
             above: float | None = None):
    """The limb's end to `target` with the model-space turn end_rot; the knee or elbow stays in its plane, unless
    that plane puts it under `above` (the floor): then the plane turns about the hip-to-end line just enough."""
    up, mid, end = LIMBS[limb]
    iu, im, ie = (clip.skel.index(n) for n in (up, mid, end))
    H, K, A = W[iu][:3, 3], W[im][:3, 3], W[ie][:3, 3]
    l1, l2 = np.linalg.norm(K - H), np.linalg.norm(A - K)
    d = target - H
    dist = float(np.clip(np.linalg.norm(d), abs(l1 - l2) + 1e-3, l1 + l2 - 1e-3))
    u = d / np.linalg.norm(d)
    pole = (K - H) - ((K - H) @ u) * u
    v = pole / np.linalg.norm(pole) if np.linalg.norm(pole) > 1e-6 else W[im][:3, 1]
    ca = (l1 * l1 + dist * dist - l2 * l2) / (2 * l1 * dist)
    sa = np.sqrt(max(0.0, 1 - ca * ca))
    K2 = H + l1 * (ca * u + sa * v)
    if above is not None and K2[1] < above:
        w = np.cross(u, v)
        for deg in sorted(range(-180, 181, 3), key=abs):
            ph = np.radians(deg)
            k = H + l1 * (ca * u + sa * (np.cos(ph) * v + np.sin(ph) * w))
            if k[1] >= above:
                K2 = k
                break
    clip.set_world_rot(f, W, up, between(K - H, K2 - H) @ W[iu][:3, :3])
    clip.set_world_rot(f, W, mid, between(W[ie][:3, 3] - W[im][:3, 3], H + dist * u - W[im][:3, 3]) @ W[im][:3, :3])
    clip.set_world_rot(f, W, end, end_rot)


def hip_roll(W: np.ndarray, i_left: int, i_right: int, side: np.ndarray) -> float:
    """How far the left hip joint is over the right one, as an angle seen along the travel (radians)."""
    v = W[i_left][:3, 3] - W[i_right][:3, 3]
    return float(np.arctan2(v[1], abs(v @ side)))


def toe_lift(W: np.ndarray, ie: int, it: int, height: float) -> tuple[float, np.ndarray]:
    """The smallest pitch of the foot about its ankle (and the axis) that puts the toe node at `height`."""
    A, v = W[ie][:3, 3], W[it][:3, 3] - W[ie][:3, 3]
    k = np.cross(v, UP)
    if np.linalg.norm(k) < 0.2 * np.linalg.norm(v):  # the foot stands nearly upright: take its own side axis
        k = W[ie][:3, :3] @ np.array([1.0, 0, 0])
        k -= (k @ UP) * UP
    k /= np.linalg.norm(k)
    # the toe's height after a turn t about k: a cos t + b sin t + c
    a, b, c = v[1] - k[1] * (k @ v), np.cross(k, v)[1], k[1] * (k @ v)
    x = np.clip((height - A[1] - c) / np.hypot(a, b), -1, 1)
    ph = np.arctan2(b, a)
    ts = [(s + np.pi) % (2 * np.pi) - np.pi for s in (ph + np.arccos(x), ph - np.arccos(x))]
    return min(ts, key=abs), k


def apply(skel: ga.Skeleton, anim: ga.Animation, fixes: list, floor: float, cycle: int = 0) -> list[str]:
    """cycle > 0: the record is a moving loop of `cycle` frames plus its seam frame (the first again, moved by the
    travel); windows wrap over the seam and the seam frame is rebuilt from the first at the end."""
    clip = Clip(skel, anim)
    log = []
    count = cycle or anim.frame_count
    travel = anim.positions[-1, 0] - anim.positions[0, 0]

    def wt(window, f):
        return weight_at(window, f, cycle)

    first = [clip.world(f) for f in range(anim.frame_count)] if any(fx["kind"] == "keep" for fx in fixes) else None
    for fx in fixes:
        win = window_of(fx)
        frames = [f for f in range(count) if wt(win, f) > 0]
        kind = fx["kind"]
        above = floor + fx["joint_clear"] if "joint_clear" in fx else None  # the knee or elbow stays over the floor
        if kind == "step":
            ie = skel.index(LIMBS[fx["limb"]][2])
            f0, f1 = fx["from"], fx["to"]
            W0, W1 = clip.world(f0), clip.world(f1)
            for f in range(f0 + 1, f1):
                u = (f - f0) / (f1 - f0)
                e = u * u * (3 - 2 * u)
                target = (1 - e) * W0[ie][:3, 3] + e * W1[ie][:3, 3] + fx.get("lift", 8.0) * np.sin(np.pi * u) * UP
                two_bone(clip, f, clip.world(f), fx["limb"], target, slerp_m(W0[ie][:3, :3], W1[ie][:3, :3], e), above)
        elif kind in ("lift", "tuck"):
            iu, ie = (skel.index(LIMBS[fx["limb"]][j]) for j in (0, 2))
            for f in frames:
                W, w = clip.world(f), wt(win, f)
                H, A = W[iu][:3, 3], W[ie][:3, 3]
                target = A + fx["cm"] * w * UP if kind == "lift" else H + (1 - (1 - fx["k"]) * w) * (A - H)
                two_bone(clip, f, W, fx["limb"], target, W[ie][:3, :3].copy(), above)
        elif kind == "soles":
            height = floor + fx.get("clear", 3.0)
            step = np.radians(fx.get("step", 12.0))
            n = anim.frame_count
            for limb in fx.get("limbs", ["L leg", "R leg"]):
                foot = LIMBS[limb][2]
                ie, it = skel.index(foot), skel.index(foot.replace("FOOT", "TOE0"))
                need = np.zeros(n)
                for f in frames:
                    W = clip.world(f)
                    if W[it][1, 3] < height:
                        need[f] = abs(toe_lift(W, ie, it, height)[0])
                # the smallest curve over the need that changes by at most `step` a frame
                ease = np.array([max(need[g] - step * abs(f - g) for g in range(n)) for f in range(n)]).clip(0)
                ease[[0, -1]] = need[[0, -1]]
                for f in frames:
                    if ease[f] < np.radians(0.5):
                        continue
                    W = clip.world(f)
                    t, k = toe_lift(W, ie, it, height)
                    v = W[it][:3, 3] - W[ie][:3, 3]
                    sign = 1.0 if (rot(k, 0.01) @ v)[1] > v[1] else -1.0  # the way that lifts the toe
                    t = sign * ease[f] * wt(win, f)
                    clip.set_world_rot(f, W, foot, rot(k, t) @ W[ie][:3, :3])
                    log.append(f"soles {foot} f{f}: {np.degrees(t):+.0f}°")
        elif kind == "rot":
            i = skel.index(fx["bone"])
            for f in frames:
                W = clip.world(f)
                R = rot(fx["axis"], np.radians(fx["deg"]) * wt(win, f))
                clip.set_world_rot(f, W, fx["bone"], W[i][:3, :3] @ R if fx.get("space", "local") == "local" else R @ W[i][:3, :3])
        elif kind == "calm":
            c = clip.col[skel.index(fx["bone"])]
            q = anim.rotations[:, c].copy()
            for f in range(1, len(q)):
                if q[f] @ q[f - 1] < 0:
                    q[f] = -q[f]
            for f in frames:
                g = np.exp(-0.5 * ((np.arange(len(q)) - f) / fx.get("sigma", 1.5)) ** 2)
                m = (g[:, None] * q).sum(0)
                anim.rotations[f, c] = rs.slerp(q[f], m / np.linalg.norm(m), wt(win, f))
        elif kind == "hold":
            for bone in fx["bones"]:
                c = clip.col[skel.index(bone)]
                q0 = anim.rotations[fx["frame"], c].copy()
                for f in frames:
                    q = q0 if q0 @ anim.rotations[f, c] >= 0 else -q0
                    anim.rotations[f, c] = rs.slerp(anim.rotations[f, c], q, wt(win, f))
        elif kind == "keep":
            for f in frames:
                W = clip.world(f)
                for bone in fx["bones"]:
                    i = skel.index(bone)
                    clip.set_world_rot(f, W, bone, slerp_m(W[i][:3, :3], first[f][i][:3, :3], wt(win, f)))
        elif kind == "raise":
            c = clip.col[0]
            for f in frames:
                anim.positions[f, c, 1] += fx["cm"] * wt(win, f)
        elif kind == "wrist":
            top = np.radians(fx.get("max", 10.0))
            for limb in fx.get("limbs", ["L arm", "R arm"]):
                fore, hand = LIMBS[limb][1], LIMBS[limb][2]
                i_fore, i_hand, i_tip = (skel.index(b) for b in (fore, hand, hand.replace("HAND", "FINGER0")))
                for f in frames:
                    W = clip.world(f)
                    d_fore = W[i_hand][:3, 3] - W[i_fore][:3, 3]
                    d_hand = W[i_tip][:3, 3] - W[i_hand][:3, 3]
                    bend = np.arccos(np.clip(d_fore @ d_hand / np.linalg.norm(d_fore) / np.linalg.norm(d_hand), -1, 1))
                    if bend <= top:
                        continue
                    k = np.cross(d_hand, d_fore)  # a turn about it brings the hand towards the forearm's line
                    t = (bend - top) * wt(win, f)
                    clip.set_world_rot(f, W, hand, rot(k / np.linalg.norm(k), t) @ W[i_hand][:3, :3])
                    log.append(f"wrist {hand} f{f}: {np.degrees(bend):.0f}° -> {np.degrees(bend - t):.0f}°")
        elif kind == "steady":
            # the last bone keeps the share `share` of its swing in the world about its cycle mean (a runner holds
            # the head still), and the turn that takes is spread evenly over the bones of the list, top down
            bones = fx["bones"]
            i_end = skel.index(bones[-1])
            qs = np.array([quat(clip.world(f)[i_end][:3, :3]) for f in range(count)])
            qs[(qs @ qs[0]) < 0] *= -1
            mean = ga.quat_matrix(qs.mean(0) / np.linalg.norm(qs.mean(0)))
            for f in frames:
                W = clip.world(f)
                now = W[i_end][:3, :3].copy()
                goal = slerp_m(now, mean, (1 - fx["share"]) * wt(win, f))
                part = slerp_m(np.eye(3), goal @ now.T, 1 / len(bones))
                for bone in bones[:-1]:
                    i = skel.index(bone)
                    clip.set_world_rot(f, W, bone, part @ W[i][:3, :3])
                clip.set_world_rot(f, W, bones[-1], goal)
        elif kind == "pelvis":
            # the hips' roll (one hip up, the other down, seen from the front) kept to the share `roll` of its swing
            # about the mean; the trunk keeps its turn and the legs reach the same feet, so only the hips move
            i_root, i_pel = skel.index("BIP01"), skel.index("BIP01 PELVIS")
            i_lt, i_rt = skel.index("BIP01 L THIGH"), skel.index("BIP01 R THIGH")
            ahead = clip.world(anim.frame_count - 1)[i_root][:3, 3] - clip.world(0)[i_root][:3, 3]
            ahead[1] = 0.0
            ahead = np.asarray(fx["axis"], float) if "axis" in fx else ahead / np.linalg.norm(ahead)
            side = np.cross(UP, ahead)
            rolls = np.array([hip_roll(clip.world(f), i_lt, i_rt, side) for f in range(count)])
            mean = rolls.mean()
            if ((clip.world(0)[i_lt][:3, 3] - clip.world(0)[i_rt][:3, 3]) @ side) < 0:
                ahead = -ahead  # the turn below must lower the left hip when the roll is positive
            for f in frames:
                W = clip.world(f)
                feet = {limb: (W[skel.index(LIMBS[limb][2])][:3, 3].copy(), W[skel.index(LIMBS[limb][2])][:3, :3].copy())
                        for limb in ("L leg", "R leg")}
                trunk = W[skel.index("BIP01 SPINE")][:3, :3].copy()
                t = (1 - fx["roll"]) * (rolls[f] - mean) * wt(win, f)
                clip.set_world_rot(f, W, "BIP01 PELVIS", rot(ahead, -t) @ W[i_pel][:3, :3])
                clip.set_world_rot(f, W, "BIP01 SPINE", trunk)
                for limb, (at, turn) in feet.items():
                    two_bone(clip, f, W, limb, at, turn)
                log.append(f"pelvis f{f}: roll {np.degrees(rolls[f] - mean):+.1f}° -> "
                           f"{np.degrees(hip_roll(W, i_lt, i_rt, side) - mean):+.1f}°")
        else:
            raise SystemExit(f"unknown fix kind {kind}")
    if cycle:  # the seam frame is the first frame again, moved by the travel
        anim.rotations[cycle] = anim.rotations[0]
        anim.positions[cycle] = anim.positions[0]
        anim.positions[cycle, 0] = anim.positions[0, 0] + travel
    return log


def floor_of(skel: ga.Skeleton, stance: ga.Animation) -> float:
    """The floor under a stance: its written toe nodes sit about 3 cm over the sole."""
    w0 = ga.world_pose(skel, stance, 0)
    toes = [skel.index(f"BIP01 {s} TOE0") for s in "LR"]
    return min(w0[i][1, 3] for i in [i for i in toes if i in stance.node_indices] or toes) - 3


def main():
    B, src, fixes_path, out = (Path(x) for x in sys.argv[1:5])
    skel = ga.read_skeleton(B / "HUMANS.MDH.json")
    spec = json.loads(fixes_path.read_text())
    orig, anim = ga.read_animation(src), ga.read_animation(src)
    cycle = anim.frame_count - 1 if spec.get("loop") else 0
    log = apply(skel, anim, spec["fixes"], floor_of(skel, ga.read_animation(B / f"{spec['stance']}.MAN.json")), cycle)
    count = cycle or anim.frame_count
    outside = [f for f in range(count) if all(weight_at(window_of(fx), f, cycle) == 0 for fx in spec["fixes"])]
    still = np.ones(anim.positions.shape[:2], bool)  # only `raise` moves a position: the root's, in its window
    still[[f for fx in spec["fixes"] if fx["kind"] == "raise" for f in range(count) if weight_at(window_of(fx), f, cycle) > 0], 0] = False
    if cycle:  # the seam frame follows the first
        still[cycle] = still[0]
        outside += [cycle] if 0 in outside else []
    assert np.array_equal(orig.rotations[outside], anim.rotations[outside]) and np.array_equal(orig.positions[still], anim.positions[still])
    assert np.array_equal(orig.positions[:, 0, [0, 2]], anim.positions[:, 0, [0, 2]])  # the path is the same
    ga.write_animation(out, anim, f"fix_layer:{src.name} {fixes_path.name}")
    diff = ~np.all(orig.rotations == anim.rotations, axis=2)
    bones = sorted({skel.nodes[anim.node_indices[c]].name for c in np.where(diff.any(0))[0]})
    print(f"{out.name}: {int(diff.any(1).sum())}/{anim.frame_count} frames changed "
          f"{[int(f) for f in np.where(diff.any(1))[0]]}, bones {bones}; the frames outside the windows are the same")
    for line in log:
        print(" ", line)


if __name__ == "__main__":
    main()
