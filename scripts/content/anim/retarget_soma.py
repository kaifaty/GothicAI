"""Retarget a Kimodo SOMA motion (NPZ, somaskel30 or somaskel77) onto the Gothic human skeleton.

    python3 scripts/content/anim/retarget_soma.py motion.npz <base>/models/HUMANS.MDH.json \
        out/HUMANS-T_TEST.MAN.json --name T_TEST [--next S_RUN] [--fps 25] [--src-fps 30]

Method: global rotation transfer with a per-bone offset found in a shared pose.
- SOMA's rest (zero) pose is a T-pose with world-aligned joint frames, so its global rotations are
  the identity there. Each mapped Gothic bone is turned from its bind orientation by the shortest
  arc until its axis (Biped bones run along local X) points along the SOMA bone in that T-pose.
  This aligned orientation A_b is the bone's offset.
- Per frame: W_b(t) = F R_s(t) F A_b, where F mirrors X. SOMA is right-handed and Gothic
  left-handed, and both face +Z with Y up.
- Gothic keeps its own bone lengths (positions by FK from the bind offsets). The root follows SOMA's
  hips scaled by the leg-length ratio. Unmapped nodes keep their bind offset to the parent.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gothic_anim as ga  # noqa: E402

SOMA30 = [
    "Hips", "Spine1", "Spine2", "Chest", "Neck1", "Neck2", "Head", "Jaw", "LeftEye", "RightEye",
    "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand", "LeftHandThumbEnd", "LeftHandMiddleEnd",
    "RightShoulder", "RightArm", "RightForeArm", "RightHand", "RightHandThumbEnd", "RightHandMiddleEnd",
    "LeftLeg", "LeftShin", "LeftFoot", "LeftToeBase", "RightLeg", "RightShin", "RightFoot", "RightToeBase",
]

SOMA30_PARENT = {
    "Hips": None, "Spine1": "Hips", "Spine2": "Spine1", "Chest": "Spine2", "Neck1": "Chest",
    "Neck2": "Neck1", "Head": "Neck2", "Jaw": "Head", "LeftEye": "Head", "RightEye": "Head",
    "LeftShoulder": "Chest", "LeftArm": "LeftShoulder", "LeftForeArm": "LeftArm", "LeftHand": "LeftForeArm",
    "LeftHandThumbEnd": "LeftHand", "LeftHandMiddleEnd": "LeftHand",
    "RightShoulder": "Chest", "RightArm": "RightShoulder", "RightForeArm": "RightArm",
    "RightHand": "RightForeArm", "RightHandThumbEnd": "RightHand", "RightHandMiddleEnd": "RightHand",
    "LeftLeg": "Hips", "LeftShin": "LeftLeg", "LeftFoot": "LeftShin", "LeftToeBase": "LeftFoot",
    "RightLeg": "Hips", "RightShin": "RightLeg", "RightFoot": "RightShin", "RightToeBase": "RightFoot",
}

# Gothic node → (SOMA joint whose global rotation drives it, SOMA joint the bone points to or None)
MAP = {
    "BIP01 PELVIS": ("Hips", None),  # BIP01 itself follows the pelvis by its bind offset
    "BIP01 SPINE": ("Spine1", "Spine2"),
    "BIP01 SPINE1": ("Spine2", "Chest"),
    "BIP01 SPINE2": ("Chest", "Neck1"),
    "BIP01 NECK": ("Neck1", "Head"),
    "BIP01 HEAD": ("Head", None),
}
for side, s in (("L", "Left"), ("R", "Right")):
    MAP.update({
        f"BIP01 {side} CLAVICLE": (f"{s}Shoulder", f"{s}Arm"),
        f"BIP01 {side} UPPERARM": (f"{s}Arm", f"{s}ForeArm"),
        f"BIP01 {side} FOREARM": (f"{s}ForeArm", f"{s}Hand"),
        f"BIP01 {side} HAND": (f"{s}Hand", f"{s}HandMiddleEnd"),
        f"BIP01 {side} THIGH": (f"{s}Leg", f"{s}Shin"),
        f"BIP01 {side} CALF": (f"{s}Shin", f"{s}Foot"),
        f"BIP01 {side} FOOT": (f"{s}Foot", f"{s}ToeBase"),
    })

F = np.diag([-1.0, 1.0, 1.0])


def soma77_names() -> list[str]:
    fingers = []
    for side in ("Left", "Right"):
        hand = [f"{side}Shoulder", f"{side}Arm", f"{side}ForeArm", f"{side}Hand",
                f"{side}HandThumb1", f"{side}HandThumb2", f"{side}HandThumb3", f"{side}HandThumbEnd"]
        for f in ("Index", "Middle", "Ring", "Pinky"):
            hand += [f"{side}Hand{f}{k}" for k in ("1", "2", "3", "4", "End")]
        fingers.append(hand)
    return (["Hips", "Spine1", "Spine2", "Chest", "Neck1", "Neck2", "Head", "HeadEnd", "Jaw", "LeftEye", "RightEye"]
            + fingers[0] + fingers[1]
            + ["LeftLeg", "LeftShin", "LeftFoot", "LeftToeBase", "LeftToeEnd",
               "RightLeg", "RightShin", "RightFoot", "RightToeBase", "RightToeEnd"])


# somaskel77 joints standing in for the 30-joint layout. Its hand ends sit at the fingertips of
# relaxed (curled) fingers, so the hand points at the middle finger's knuckle instead.
SOMA77_FOR_30 = {"LeftHandMiddleEnd": "LeftHandMiddle1", "RightHandMiddleEnd": "RightHandMiddle1",
                 "LeftHandThumbEnd": "LeftHandThumb1", "RightHandThumbEnd": "RightHandThumb1"}


def load_soma(path: Path) -> tuple[np.ndarray, np.ndarray, list[str], np.ndarray | None]:
    """SOMA joints in the 30-joint layout: positions [T, 30, 3] m, global rotations [T, 30, 3, 3]."""
    d = np.load(path)
    pos, rot = d["posed_joints"], d["global_rot_mats"]
    if pos.shape[1] == 77:
        names77 = soma77_names()
        assert len(names77) == 77
        idx = [names77.index(SOMA77_FOR_30.get(n, n)) for n in SOMA30]
        pos, rot = pos[:, idx], rot[:, idx]
    elif pos.shape[1] != 30:
        raise SystemExit(f"unexpected SOMA joint count {pos.shape[1]}")
    contacts = d["foot_contacts"] if "foot_contacts" in d.files else None
    return pos.astype(np.float64), rot.astype(np.float64), SOMA30, contacts


def shortest_arc(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    a = a / np.linalg.norm(a)
    b = b / np.linalg.norm(b)
    v = np.cross(a, b)
    c = float(np.dot(a, b))
    if c < -0.999999:
        axis = np.cross(a, [1.0, 0.0, 0.0])
        if np.linalg.norm(axis) < 1e-6:
            axis = np.cross(a, [0.0, 1.0, 0.0])
        axis /= np.linalg.norm(axis)
        return 2.0 * np.outer(axis, axis) - np.eye(3)
    vx = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + vx + vx @ vx * (1.0 / (1.0 + c))


def mat_to_quat(m: np.ndarray) -> np.ndarray:
    t = np.trace(m)
    if t > 0:
        s = np.sqrt(t + 1.0) * 2
        q = [0.25 * s, (m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s]
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = np.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        q = [(m[2, 1] - m[1, 2]) / s, 0.25 * s, (m[0, 1] + m[1, 0]) / s, (m[0, 2] + m[2, 0]) / s]
    elif m[1, 1] > m[2, 2]:
        s = np.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        q = [(m[0, 2] - m[2, 0]) / s, (m[0, 1] + m[1, 0]) / s, 0.25 * s, (m[1, 2] + m[2, 1]) / s]
    else:
        s = np.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        q = [(m[1, 0] - m[0, 1]) / s, (m[0, 2] + m[2, 0]) / s, (m[1, 2] + m[2, 1]) / s, 0.25 * s]
    q = np.array(q)
    return q / np.linalg.norm(q)


def slerp(a: np.ndarray, b: np.ndarray, t: float) -> np.ndarray:
    d = float(np.dot(a, b))
    if d < 0:
        b, d = -b, -d
    if d > 0.9995:
        q = a + t * (b - a)
        return q / np.linalg.norm(q)
    th = np.arccos(d)
    return (np.sin((1 - t) * th) * a + np.sin(t * th) * b) / np.sin(th)


def leg_length(points: dict[str, np.ndarray], hip: str, knee: str, ankle: str) -> float:
    return float(np.linalg.norm(points[knee] - points[hip]) + np.linalg.norm(points[ankle] - points[knee]))


def with_offsets(skel: ga.Skeleton, ref: ga.Animation) -> ga.Skeleton:
    """The skeleton with the bone offsets of a reference clip's first frame.

    Gothic clips were animated on a Biped whose proportions differ from the mesh bind pose
    (thigh 40.9 cm instead of 39.3, shin 44.9 instead of 49.1), the same in every clip. New clips
    must use the clips' offsets, or the body jumps by centimetres at every transition.
    """
    nodes = [ga.Node(n.name, n.parent, n.rest.copy()) for n in skel.nodes]
    for col, i in enumerate(ref.node_indices):
        if i < len(nodes) and nodes[i].parent >= 0:
            nodes[i].rest[:3, 3] = ref.positions[0, col]
    return ga.Skeleton(nodes, skel.root_translation)


def soma_rig(skel: ga.Skeleton, pos0: np.ndarray, rot0: np.ndarray, names: list[str],
             ground: ga.Animation | None = None) -> dict:
    """Per-bone offsets and the SOMA → Gothic length scale, from any one SOMA frame.

    `ground`: a Gothic clip whose first frame stands on the floor (the stance); its lower ankle
    height is where retargeted feet are put. Without it, the bind pose's ankle height."""
    si = {n: i for i, n in enumerate(names)}
    bind = ga.world_pose(skel, None, 0)
    # SOMA rest offsets: each joint in its parent's frame (frames are world-aligned at rest, so a
    # rest-pose bone vector is the sum of the offsets along its chain, whatever the pose of frame 0).
    rest_off = {n: (rot0[si[p]].T @ (pos0[si[n]] - pos0[si[p]]) if p else np.zeros(3))
                for n, p in SOMA30_PARENT.items()}
    rest_dir = {}
    for g, (s, c) in MAP.items():
        if c is not None:
            v, k = np.zeros(3), c
            while k != s:
                v += rest_off[k]
                k = SOMA30_PARENT[k]
            rest_dir[g] = F @ v
    aligned = {}
    for i, n in enumerate(skel.nodes):
        r = bind[i][:3, :3]
        if n.name in rest_dir:
            r = shortest_arc(r[:, 0], rest_dir[n.name]) @ r
        aligned[n.name] = r
    soma_pts = {n: pos0[i] for n, i in si.items()}  # lengths only, any frame will do
    gothic_bind = {n.name: bind[i][:3, 3] for i, n in enumerate(skel.nodes)}
    scale = leg_length(gothic_bind, "BIP01 L THIGH", "BIP01 L CALF", "BIP01 L FOOT") / \
        leg_length(soma_pts, "LeftLeg", "LeftShin", "LeftFoot")  # SOMA metres → Gothic cm
    stand = ga.world_pose(skel, ground, 0) if ground is not None else bind
    foot_y = min(stand[skel.index("BIP01 L FOOT")][1, 3], stand[skel.index("BIP01 R FOOT")][1, 3])
    # SOMA ankle joint height over the floor at heel contact, measured in Kimodo outputs.
    return {"si": si, "aligned": aligned, "scale": scale, "bind_foot_y": float(foot_y), "soma_foot_y": 0.081}


def gothic_to_soma(skel: ga.Skeleton, anim: ga.Animation, frame: int, rig: dict, names: list[str],
                   origin_xz: np.ndarray | None = None) -> tuple[np.ndarray, np.ndarray]:
    """A Gothic pose as SOMA local axis-angle rotations [J, 3] and hips position [3] in metres."""
    world = ga.world_pose(skel, anim, frame)
    gi = {n.name: i for i, n in enumerate(skel.nodes)}
    soma_global = {}
    for g, (s, _) in MAP.items():
        soma_global[s] = F @ world[gi[g]][:3, :3] @ rig["aligned"][g].T @ F
    out = np.zeros((len(names), 3))
    for j, n in enumerate(names):
        parent = SOMA30_PARENT[n]
        if n not in soma_global:
            soma_global[n] = soma_global[parent]
        local = soma_global[n] if parent is None else soma_global[parent].T @ soma_global[n]
        out[j] = axis_angle(local)
    hips = F @ world[gi["BIP01 PELVIS"]][:3, 3]
    hips[1] -= rig["bind_foot_y"] - rig.get("soma_foot_y", 0.0) * rig["scale"]
    hips = hips / rig["scale"]
    if origin_xz is not None:
        hips[[0, 2]] -= origin_xz
    return out, hips


def axis_angle(m: np.ndarray) -> np.ndarray:
    q = mat_to_quat(m)
    if q[0] < 0:
        q = -q
    s = np.linalg.norm(q[1:])
    if s < 1e-9:
        return np.zeros(3)
    return q[1:] / s * 2.0 * np.arctan2(s, q[0])


def retarget(npz: Path, skel: ga.Skeleton, name: str, next_ani: str, fps: float, src_fps: float,
             root_offset_y: float | None = None, ref: ga.Animation | None = None) -> ga.Animation:
    """`ref`: the stance clip the move starts from; gives bone offsets and the floor height."""
    pos, rot, names, _ = load_soma(npz)
    if ref is not None:
        skel = with_offsets(skel, ref)
    rig = soma_rig(skel, pos[0], rot[0], names, ref)
    si, aligned, scale = rig["si"], rig["aligned"], rig["scale"]
    gi = {n.name: i for i, n in enumerate(skel.nodes)}

    frames_src = pos.shape[0]
    world_rot = np.zeros((frames_src, len(skel.nodes), 3, 3))
    root_pos = np.zeros((frames_src, 3))
    for t in range(frames_src):
        pelvis = gi["BIP01 PELVIS"]
        world_rot[t, pelvis] = F @ rot[t, si["Hips"]] @ F @ aligned["BIP01 PELVIS"]
        for g, i in gi.items():
            if skel.nodes[i].parent < 0:
                world_rot[t, i] = world_rot[t, pelvis] @ skel.nodes[pelvis].rest[:3, :3].T
            elif g in MAP:
                world_rot[t, i] = F @ rot[t, si[MAP[g][0]]] @ F @ aligned[g]
            else:
                parent = skel.nodes[i].parent
                local = skel.nodes[i].rest[:3, :3]
                world_rot[t, i] = (world_rot[t, parent] @ local) if parent >= 0 else aligned[g]
        # SOMA hips sit at the Gothic pelvis; BIP01 is its parent by the bind offset.
        root_pos[t] = F @ pos[t, si["Hips"]] * scale - world_rot[t, 0] @ skel.nodes[pelvis].rest[:3, 3]

    if root_offset_y is None:
        # Ground the clip: the lower foot's 10th percentile height equals the stance's ankle height.
        feet = np.array([[world_feet(skel, world_rot[t], root_pos[t], k) for k in ("L", "R")]
                         for t in range(frames_src)])
        root_offset_y = rig["bind_foot_y"] - float(np.percentile(feet.min(axis=1), 10))
    root_pos[:, 1] += root_offset_y

    # Local samples at the source rate.
    rest_t = [n.rest[:3, 3] for n in skel.nodes]
    local_q = np.zeros((frames_src, len(skel.nodes), 4))
    local_p = np.zeros((frames_src, len(skel.nodes), 3))
    for t in range(frames_src):
        for i, n in enumerate(skel.nodes):
            if n.parent < 0:
                local_q[t, i] = mat_to_quat(world_rot[t, i])
                local_p[t, i] = root_pos[t]
            else:
                local_q[t, i] = mat_to_quat(world_rot[t, n.parent].T @ world_rot[t, i])
                local_p[t, i] = rest_t[i]
            if t and np.dot(local_q[t, i], local_q[t - 1, i]) < 0:
                local_q[t, i] = -local_q[t, i]

    # Resample to the engine's rate.
    duration = (frames_src - 1) / src_fps
    count = int(round(duration * fps)) + 1
    out_q = np.zeros((count, len(skel.nodes), 4))
    out_p = np.zeros((count, len(skel.nodes), 3))
    for k in range(count):
        x = min(k / fps * src_fps, frames_src - 1)
        a = int(np.floor(x))
        b = min(a + 1, frames_src - 1)
        u = x - a
        for i in range(len(skel.nodes)):
            out_q[k, i] = slerp(local_q[a, i], local_q[b, i], u)
            out_p[k, i] = local_p[a, i] * (1 - u) + local_p[b, i] * u
    return ga.Animation(name=name, next=next_ani, layer=1, fps=fps, frame_count=count,
                        node_indices=list(range(len(skel.nodes))), positions=out_p, rotations=out_q)


def world_feet(skel: ga.Skeleton, world_rot: np.ndarray, root: np.ndarray, side: str) -> float:
    """Height of a foot node from world rotations and the root position (FK on the rest offsets)."""
    chain = []
    i = skel.index(f"BIP01 {side} FOOT")
    while i >= 0:
        chain.append(i)
        i = skel.nodes[i].parent
    p = root.copy()
    for i in reversed(chain[:-1]):
        parent = skel.nodes[i].parent
        p = p + world_rot[parent] @ skel.nodes[i].rest[:3, 3]
    return float(p[1])


def foot_report(skel: ga.Skeleton, anim: ga.Animation) -> dict:
    """Lowest foot height and the largest per-frame horizontal slide of the lower foot (cm)."""
    li, ri = skel.index("BIP01 L FOOT"), skel.index("BIP01 R FOOT")
    feet = np.array([[ga.world_pose(skel, anim, f)[j][:3, 3] for j in (li, ri)] for f in range(anim.frame_count)])
    low = feet[:, :, 1].min()
    slide = []
    for f in range(1, anim.frame_count):
        j = int(np.argmin(feet[f, :, 1]))
        if feet[f, j, 1] < low + 3.0 and feet[f - 1, j, 1] < low + 3.0:
            slide.append(float(np.linalg.norm(feet[f, j, [0, 2]] - feet[f - 1, j, [0, 2]])))
    return {"foot_min_y": round(float(low), 1), "planted_slide_max": round(max(slide, default=0.0), 1),
            "planted_slide_median": round(float(np.median(slide)) if slide else 0.0, 1)}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("npz", type=Path)
    ap.add_argument("skeleton", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--name", required=True)
    ap.add_argument("--next", default="")
    ap.add_argument("--fps", type=float, default=25.0)
    ap.add_argument("--src-fps", type=float, default=30.0)
    ap.add_argument("--ref", type=Path, help="stance clip (MAN record) for bone offsets and floor height")
    ap.add_argument("--root-offset-y", type=float, default=None,
                    help="cm added to the root height; default grounds the feet at the stance's ankle height")
    a = ap.parse_args()
    skel = ga.read_skeleton(a.skeleton)
    ref = ga.read_animation(a.ref) if a.ref else None
    anim = retarget(a.npz, skel, a.name, a.next, a.fps, a.src_fps, a.root_offset_y, ref)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    ga.write_animation(a.out, anim, source=f"kimodo:{a.npz.name}")
    tr = ga.root_travel(anim)
    print(f"{anim.name}: {anim.frame_count} frames @ {anim.fps:g} = {anim.duration*1000:.0f} ms, "
          f"root travel {np.round(tr[-1], 1).tolist()} cm", foot_report(skel, anim))


if __name__ == "__main__":
    main()
