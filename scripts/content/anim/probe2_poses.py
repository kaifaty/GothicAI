"""Key poses of the second Kimodo probe as one-frame MAN records, made from Gothic clips.

    python3 scripts/content/anim/probe2_poses.py <base>/models <pose dir>

Reference poses for the probe only: they are derived from the original animations, so they stay
local and out of git, and shipped clips need poses of our own (docs/content-delivery.md). The pose
dir is the fifth argument of kimodo_moves.py for moves-probe2.json.
"""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gothic_anim as ga  # noqa: E402
import retarget_soma as rs  # noqa: E402

models, out = Path(sys.argv[1]), Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
skel0 = ga.read_skeleton(models / "HUMANS.MDH.json")
stance = ga.read_animation(models / "HUMANS_1HST1-S_1H.MAN.json")
skel = rs.with_offsets(skel0, stance)
gi = {n.name: i for i, n in enumerate(skel.nodes)}
clips = {}


def clip(name):
    if name not in clips:
        clips[name] = ga.read_animation(models / f"{name}.MAN.json")
    return clips[name]


def world(name, frame):
    """World rotations [N,3,3] and root position of a clip frame."""
    w = ga.world_pose(skel, clip(name), frame)
    return np.array([m[:3, :3] for m in w]), w[0][:3, 3].copy()


def subtree(root):
    ids, stack = [], [gi[root]]
    while stack:
        i = stack.pop()
        ids.append(i)
        stack += [j for j, n in enumerate(skel.nodes) if n.parent == i]
    return ids


def compose(base, parts):
    """Locals of `base` (clip, frame), with the local rotations of each subtree in `parts` taken
    from other frames: [(subtree root, clip, frame)]."""
    rot, root = world(*base)
    local = np.array([rot[i] if n.parent < 0 else rot[n.parent].T @ rot[i] for i, n in enumerate(skel.nodes)])
    for top, c, f in parts:
        r2, _ = world(c, f)
        for i in subtree(top):
            p = skel.nodes[i].parent
            local[i] = r2[i] if p < 0 else r2[p].T @ r2[i]
    rot = np.zeros_like(local)
    for i, n in enumerate(skel.nodes):
        rot[i] = local[i] if n.parent < 0 else rot[n.parent] @ local[i]
    return rot, root


def turn(rot, top, axis, deg):
    """Rotate the subtree under `top` by `deg` about a world axis through its joint."""
    a = np.asarray(axis, float) / np.linalg.norm(axis)
    t = np.radians(deg)
    k = np.array([[0, -a[2], a[1]], [a[2], 0, -a[0]], [-a[1], a[0], 0]])
    r = np.eye(3) + np.sin(t) * k + (1 - np.cos(t)) * k @ k
    for i in subtree(top):
        rot[i] = r @ rot[i]


def face_forward(rot, root):
    """Turn the whole pose about Y so the thighs' line is across +Z (facing +Z like the stance)."""
    p = rs.world_positions(skel, rot, root)
    across = p[gi["BIP01 R THIGH"]] - p[gi["BIP01 L THIGH"]]
    ang = np.degrees(np.arctan2(across[2], across[0]))
    turn(rot, "BIP01", [0, 1, 0], ang)  # positions are recomputed from rotations, the root stays


def arm_to(rot, root, side, hand_at, blade_dir=None):
    """Two-bone IK of the arm to a world point (cm), then the blade along `blade_dir`."""
    chain = (gi[f"BIP01 {side} UPPERARM"], gi[f"BIP01 {side} FOREARM"], gi[f"BIP01 {side} HAND"])
    rs.two_bone_ik(rot, root, skel, chain, np.asarray(hand_at, float))
    if blade_dir is not None:
        slot = gi["ZS_RIGHTHAND"]
        blade = rot[chain[2]] @ skel.nodes[slot].rest[:3, :3] @ np.array([1.0, 0, 0])
        r = rs.shortest_arc(blade, np.asarray(blade_dir, float) / np.linalg.norm(blade_dir))
        for i in subtree(f"BIP01 {side} HAND"):
            rot[i] = r @ rot[i]


def save(name, rot, root):
    local_q, local_p = [], []
    for i, n in enumerate(skel.nodes):
        if n.parent < 0:
            local_q.append(rs.mat_to_quat(rot[i])); local_p.append(root)
        else:
            local_q.append(rs.mat_to_quat(rot[n.parent].T @ rot[i])); local_p.append(n.rest[:3, 3])
    anim = ga.Animation(name=name, next="", layer=1, fps=25.0, frame_count=1, node_indices=list(range(len(skel.nodes))),
                        positions=np.array([local_p]), rotations=np.array([local_q]))
    ga.write_animation(out / f"HUMANS-{name}.MAN.json", anim, source="probe2 pose")
    p = rs.world_positions(skel, rot, root)
    print(name, "hand R", p[gi["BIP01 R HAND"]].round(1), "pelvis", p[gi["BIP01 PELVIS"]].round(1))


ATT = "HUMANS_1HST1-S_1HATTACK"
for name, f in (("P_APEX", 54), ("P_IMPACT", 66), ("P_FOLLOW", 74)):
    rot, root = compose((ATT, f), [])
    face_forward(rot, root)
    save(name, rot, root)

rot, root = compose(("HUMANS-T_1HSFINISH", 29), [])
face_forward(rot, root)
save("P_KNEEL", rot, root)

# Thrust: lunge legs, upright torso leaning 10°, sword arm straight ahead at waist height.
rot, root = compose((ATT, 110), [("BIP01 SPINE", "HUMANS_1HST1-S_1H", 0)])
face_forward(rot, root)
turn(rot, "BIP01 SPINE", [1, 0, 0], 10)
p = rs.world_positions(skel, rot, root)
chest = p[gi["BIP01 SPINE2"]]
arm_to(rot, root, "R", chest + np.array([4.0, -18.0, 52.0]), [0.0, -0.1, 1.0])
save("P_THRUST", rot, root)

# Shoulder barge contact: lunge legs, torso leaning 28° forward and turned so the left shoulder leads.
rot, root = compose((ATT, 110), [("BIP01 SPINE", "HUMANS_1HST1-S_1H", 0)])
face_forward(rot, root)
turn(rot, "BIP01 SPINE", [0, 1, 0], 35)
turn(rot, "BIP01 SPINE", [1, 0, 0], 28)
save("P_BARGE", rot, root)

# Charged apex: Gothic's raised arm, the blade pulled back over the shoulder, torso leaning back 8°.
rot, root = compose((ATT, 54), [])
face_forward(rot, root)
turn(rot, "BIP01 SPINE", [1, 0, 0], -8)
p = rs.world_positions(skel, rot, root)
arm_to(rot, root, "R", p[gi["BIP01 R HAND"]] + np.array([0.0, 2.0, -8.0]), [0.15, 0.5, -1.0])
save("P_APEX_BACK", rot, root)
