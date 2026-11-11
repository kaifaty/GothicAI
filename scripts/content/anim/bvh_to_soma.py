"""A mocap BVH (100STYLE's Xsens skeleton) as the SOMA motion `retarget_soma.py` reads: joints in the 30-joint
layout, global rotations away from a rest pose with world-aligned frames.

    python3 scripts/content/anim/bvh_to_soma.py <in.bvh> <out.npz> [--frames a,b] [--info]

A BVH is that already: its rest pose has no rotations, so every joint's frame is the world's there, and the global
rotation of a joint in a frame is its turn away from the rest pose. What is done here:
- the hierarchy and the channels are read, and the joints' global rotations and places found (`fk`);
- the rest pose is turned to face +Z with the left side at +X, as SOMA's does (`rest_basis`): a rotation B with
  new rest offsets B o and global rotations R B^T - the same poses, another zero;
- the joints `retarget_soma.py` maps are picked by name (`JOINTS`); SOMA joints the BVH has not (eyes, jaw, a
  second neck, the thumb) follow their parents, a hand without fingers gets its end from the forearm's line;
- `--frames a,b` keeps a stretch, and the stretch is turned about the vertical so that it walks along +Z, and
  moved to start over the origin;
- `--straighten S`: 100STYLE's actor walks round a room of 3.5 by 4.5 m and turns all the time (30 degrees a
  second at the median). Every frame is seen from its own place and heading on the root's way low-passed over
  S seconds (about a stride: the steps' sway stays, the turn of the way goes) and put on a straight line along
  +Z by the distance gone. Take the stretches that turn least: a body in a turn banks and steps unevenly.
Units stay the BVH's: the retarget scales by the legs' lengths. `--info` prints the hierarchy, the rest pose's
facing, the frame rate and the root's path, and writes nothing.

100STYLE (Ian Mason, Sebastian Starke, Taku Komura; CC BY 4.0, https://www.ianxmason.com/100style/) is 60 frames
a second: pass `--src-fps 60` to `retarget_soma.py`.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from retarget_soma import SOMA30, SOMA30_PARENT

# SOMA joint -> the BVH joint whose rotation and place it takes (100STYLE names).
JOINTS = {
    "Hips": "Hips", "Spine1": "Chest", "Spine2": "Chest3", "Chest": "Chest4", "Neck1": "Neck", "Neck2": "Neck",
    "Head": "Head", "Jaw": "Head", "LeftEye": "Head", "RightEye": "Head",
}
for _s in ("Left", "Right"):
    JOINTS.update({
        f"{_s}Shoulder": f"{_s}Collar", f"{_s}Arm": f"{_s}Shoulder", f"{_s}ForeArm": f"{_s}Elbow",
        f"{_s}Hand": f"{_s}Wrist", f"{_s}HandThumbEnd": f"{_s}Wrist", f"{_s}HandMiddleEnd": f"{_s}Wrist",
        f"{_s}Leg": f"{_s}Hip", f"{_s}Shin": f"{_s}Knee", f"{_s}Foot": f"{_s}Ankle", f"{_s}ToeBase": f"{_s}Toe",
    })


def axis_rot(axis: str, deg: np.ndarray) -> np.ndarray:
    c, s = np.cos(np.radians(deg)), np.sin(np.radians(deg))
    m = np.zeros(deg.shape + (3, 3))
    i, j, k = {"X": (0, 1, 2), "Y": (1, 2, 0), "Z": (2, 0, 1)}[axis]
    m[..., i, i] = 1.0
    m[..., j, j], m[..., j, k], m[..., k, j], m[..., k, k] = c, -s, s, c
    return m


def read_bvh(path: Path) -> dict:
    """names, parents, offsets [J, 3], channels per joint, end sites {joint: offset}, frames [T, C], frame time."""
    tok = path.read_text().split()
    names, parents, offsets, channels, ends = [], [], [], [], {}
    stack, i = [], 0
    while tok[i] != "MOTION":
        t = tok[i]
        if t in ("ROOT", "JOINT"):
            names.append(tok[i + 1]); parents.append(stack[-1] if stack else -1)
            offsets.append(None); channels.append([])
            stack.append(len(names) - 1); i += 2
        elif t == "End":
            stack.append(-1 - stack[-1]); i += 2       # an end site: remembers its joint
        elif t == "OFFSET":
            o = np.array([float(x) for x in tok[i + 1:i + 4]])
            if stack[-1] < 0:
                ends[-1 - stack[-1]] = o
            else:
                offsets[stack[-1]] = o
            i += 4
        elif t == "CHANNELS":
            n = int(tok[i + 1]); channels[stack[-1]] = tok[i + 2:i + 2 + n]; i += 2 + n
        elif t == "}":
            stack.pop(); i += 1
        else:
            i += 1
    n_frames = int(tok[tok.index("Frames:", i) + 1])
    ft = tok.index("Time:", i)
    dt = float(tok[ft + 1])
    data = np.array(tok[ft + 2:], dtype=np.float64).reshape(n_frames, -1)
    return {"names": names, "parents": parents, "offsets": np.array(offsets), "channels": channels, "ends": ends,
            "frames": data, "dt": dt}


def fk(bvh: dict, a: int = 0, b: int | None = None) -> tuple[np.ndarray, np.ndarray]:
    """Global rotations [T, J, 3, 3] and places [T, J, 3] of the frames a..b."""
    data = bvh["frames"][a:b]
    T, J = len(data), len(bvh["names"])
    R, P = np.zeros((T, J, 3, 3)), np.zeros((T, J, 3))
    col = 0
    for j in range(J):
        local, pos = np.broadcast_to(np.eye(3), (T, 3, 3)).copy(), np.broadcast_to(bvh["offsets"][j], (T, 3)).copy()
        for ch in bvh["channels"][j]:
            v = data[:, col]; col += 1
            if ch.endswith("position"):
                pos[:, "XYZ".index(ch[0])] = v
            else:
                local = local @ axis_rot(ch[0], v)
        p = bvh["parents"][j]
        if p < 0:
            R[:, j], P[:, j] = local, pos
        else:
            R[:, j] = R[:, p] @ local
            P[:, j] = P[:, p] + np.einsum("tij,tj->ti", R[:, p], pos)
    return R, P


def yaw(deg: float) -> np.ndarray:
    return axis_rot("Y", np.array(deg))


def rest_basis(bvh: dict) -> np.ndarray:
    """The turn about the vertical that makes the rest pose face +Z with its left at +X."""
    rest = np.zeros((len(bvh["names"]), 3))
    for j, p in enumerate(bvh["parents"]):
        rest[j] = bvh["offsets"][j] + (rest[p] if p >= 0 else 0.0)
    ni = {n: i for i, n in enumerate(bvh["names"])}
    left = rest[ni["LeftHip"]] - rest[ni["RightHip"]]
    return yaw(float(np.degrees(np.arctan2(left[2], left[0]))))      # brings `left` onto +X


def path_of(bvh: dict, smooth_s: float) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """The root's way on the ground without the steps' sway: its low-passed place [T, 2] (x, z), heading (radians
    from +Z towards +X) and the distance gone along it, for every frame of the record."""
    root = bvh["frames"][:, [0, 2]]
    k = max(3, int(smooth_s / bvh["dt"]) | 1)
    ker = np.hanning(k + 2)[1:-1]
    ker /= ker.sum()
    pad = np.pad(root, ((k // 2, k // 2), (0, 0)), mode="edge")
    c = np.stack([np.convolve(pad[:, i], ker, mode="valid") for i in (0, 1)], axis=1)
    v = np.gradient(c, axis=0)
    return c, np.unwrap(np.arctan2(v[:, 0], v[:, 1])), np.concatenate([[0.0], np.cumsum(np.linalg.norm(np.diff(c, axis=0), axis=1))])


def convert(bvh: dict, a: int = 0, b: int | None = None, straighten: float = 0.0) -> dict:
    """`straighten`: seconds the root's way is low-passed over (about a stride); every frame is then seen from its
    own place and heading on that way and put on a straight line along +Z by the distance gone - an actor who
    walks round a small room walks straight. 0: the stretch is turned as a whole, its ends on a line along +Z."""
    ni = {n: i for i, n in enumerate(bvh["names"])}
    missing = sorted({v for v in JOINTS.values() if v not in ni})
    if missing:
        raise SystemExit(f"the BVH has no joints {missing}; it has {bvh['names']}")
    R, P = fk(bvh, a, b)
    B = rest_basis(bvh)
    R = R @ B.T
    if straighten > 0:
        c, head, gone = path_of(bvh, straighten)
        c, head, gone = c[a:b], head[a:b], gone[a:b]
        H = axis_rot("Y", -np.degrees(head))[:, None]
        R = H @ R
        P = P.copy()
        P[:, :, 0] -= c[:, None, 0]
        P[:, :, 2] -= c[:, None, 1]
        P = np.einsum("tij,tkj->tki", H[:, 0], P)
        P[:, :, 0] -= P[:, 0, 0].mean()                # a low-passed way cuts a bend: the root is off it by a constant
        P[:, :, 2] += (gone - gone[0])[:, None]
    else:
        # walk along +Z: the heading of the stretch is the root's way from its first frame to its last
        way = P[-1, 0] - P[0, 0]
        H = yaw(float(-np.degrees(np.arctan2(way[0], way[2])))) if np.hypot(way[0], way[2]) > 1e-6 else np.eye(3)
        R = H @ R
        P = (P - P[0, 0] * np.array([1.0, 0.0, 1.0])) @ H.T
        H = np.broadcast_to(H, (len(R), 1, 3, 3))
    T = len(R)
    pos, rot = np.zeros((T, 30, 3)), np.zeros((T, 30, 3, 3))
    for k, name in enumerate(SOMA30):
        j = ni[JOINTS[name]]
        pos[:, k], rot[:, k] = P[:, j], R[:, j]
    for s in ("Left", "Right"):
        w = ni[f"{s}Wrist"]
        end = bvh["ends"].get(w)
        if end is None or np.linalg.norm(end) < 1e-6:
            end = bvh["offsets"][w] * 0.5          # no end site: half a forearm along the forearm's line
        tip = P[:, w] + np.einsum("tij,j->ti", R[:, w] @ B, end)   # R B = H R0: the BVH's own frame, turned
        pos[:, SOMA30.index(f"{s}HandMiddleEnd")] = tip
        pos[:, SOMA30.index(f"{s}HandThumbEnd")] = tip
    return {"posed_joints": pos, "global_rot_mats": rot}


def info(bvh: dict) -> None:
    print(f"{len(bvh['names'])} joints, {len(bvh['frames'])} frames, {1 / bvh['dt']:.2f} fps")
    for j, n in enumerate(bvh["names"]):
        p = bvh["parents"][j]
        end = bvh["ends"].get(j)
        print(f"  {j:2d} {n:<16} <- {bvh['names'][p] if p >= 0 else '-':<16} offset {np.round(bvh['offsets'][j], 2).tolist()}"
              f" {' '.join(c[:2] for c in bvh['channels'][j])}" + (f" end {np.round(end, 2).tolist()}" if end is not None else ""))
    B = rest_basis(bvh)
    print("rest basis yaw °", round(float(np.degrees(np.arctan2(B[0, 2], B[0, 0]))), 1))
    _, P = fk(bvh)
    root = P[:, 0]
    step = max(1, len(root) // 24)
    print("root path (x, y, z) every", step, "frames:")
    for f in range(0, len(root), step):
        print(f"  {f:6d} {np.round(root[f], 1).tolist()}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src", type=Path)
    ap.add_argument("out", type=Path, nargs="?")
    ap.add_argument("--frames", help="a,b: a stretch of the record")
    ap.add_argument("--straighten", type=float, default=0.0, metavar="S",
                    help="see every frame from its place and heading on the root's way low-passed over S seconds")
    ap.add_argument("--info", action="store_true")
    a = ap.parse_args()
    bvh = read_bvh(a.src)
    if a.info:
        info(bvh)
        return
    f0, f1 = (int(x) for x in a.frames.split(",")) if a.frames else (0, len(bvh["frames"]) - 1)
    d = convert(bvh, f0, f1 + 1, a.straighten)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    np.savez(a.out, **d)
    print(f"{a.out}: {len(d['posed_joints'])} frames of {a.src.name} [{f0}..{f1}], {1 / bvh['dt']:.0f} fps")


if __name__ == "__main__":
    main()
