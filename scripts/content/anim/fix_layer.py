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
        spreads a whip over its neighbours.

The legs and arms are solved by two-bone IK: the knee or elbow stays in its own plane.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gothic_anim as ga  # noqa: E402
import retarget_soma as rs  # noqa: E402

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


def two_bone(clip: Clip, f: int, W: np.ndarray, limb: str, target: np.ndarray, end_rot: np.ndarray):
    """The limb's end to `target` with the model-space turn end_rot; the knee or elbow stays in its plane."""
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
    K2 = H + l1 * (ca * u + np.sqrt(max(0.0, 1 - ca * ca)) * v)
    clip.set_world_rot(f, W, up, between(K - H, K2 - H) @ W[iu][:3, :3])
    clip.set_world_rot(f, W, mid, between(W[ie][:3, 3] - W[im][:3, 3], H + dist * u - W[im][:3, 3]) @ W[im][:3, :3])
    clip.set_world_rot(f, W, end, end_rot)


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


def apply(skel: ga.Skeleton, anim: ga.Animation, fixes: list, floor: float) -> list[str]:
    clip = Clip(skel, anim)
    log = []
    for fx in fixes:
        win = window_of(fx)
        frames = [f for f in range(anim.frame_count) if weight(win, f) > 0]
        kind = fx["kind"]
        if kind == "step":
            ie = skel.index(LIMBS[fx["limb"]][2])
            f0, f1 = fx["from"], fx["to"]
            W0, W1 = clip.world(f0), clip.world(f1)
            for f in range(f0 + 1, f1):
                u = (f - f0) / (f1 - f0)
                e = u * u * (3 - 2 * u)
                target = (1 - e) * W0[ie][:3, 3] + e * W1[ie][:3, 3] + fx.get("lift", 8.0) * np.sin(np.pi * u) * UP
                two_bone(clip, f, clip.world(f), fx["limb"], target, slerp_m(W0[ie][:3, :3], W1[ie][:3, :3], e))
        elif kind in ("lift", "tuck"):
            iu, ie = (skel.index(LIMBS[fx["limb"]][j]) for j in (0, 2))
            for f in frames:
                W, w = clip.world(f), weight(win, f)
                H, A = W[iu][:3, 3], W[ie][:3, 3]
                target = A + fx["cm"] * w * UP if kind == "lift" else H + (1 - (1 - fx["k"]) * w) * (A - H)
                two_bone(clip, f, W, fx["limb"], target, W[ie][:3, :3].copy())
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
                    t = sign * ease[f] * weight(win, f)
                    clip.set_world_rot(f, W, foot, rot(k, t) @ W[ie][:3, :3])
                    log.append(f"soles {foot} f{f}: {np.degrees(t):+.0f}°")
        elif kind == "rot":
            i = skel.index(fx["bone"])
            for f in frames:
                W = clip.world(f)
                R = rot(fx["axis"], np.radians(fx["deg"]) * weight(win, f))
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
                anim.rotations[f, c] = rs.slerp(q[f], m / np.linalg.norm(m), weight(win, f))
        else:
            raise SystemExit(f"unknown fix kind {kind}")
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
    log = apply(skel, anim, spec["fixes"], floor_of(skel, ga.read_animation(B / f"{spec['stance']}.MAN.json")))
    outside = [f for f in range(anim.frame_count) if all(weight(window_of(fx), f) == 0 for fx in spec["fixes"])]
    assert np.array_equal(orig.rotations[outside], anim.rotations[outside]) and np.array_equal(orig.positions, anim.positions)
    ga.write_animation(out, anim, f"fix_layer:{src.name} {fixes_path.name}")
    diff = ~np.all(orig.rotations == anim.rotations, axis=2)
    bones = sorted({skel.nodes[anim.node_indices[c]].name for c in np.where(diff.any(0))[0]})
    print(f"{out.name}: {int(diff.any(1).sum())}/{anim.frame_count} frames changed "
          f"{[int(f) for f in np.where(diff.any(1))[0]]}, bones {bones}; the frames outside the windows are the same")
    for line in log:
        print(" ", line)


if __name__ == "__main__":
    main()
