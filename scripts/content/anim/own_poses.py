"""Our own key poses, built on the BIP01 rig from numbers instead of frames of Gothic clips.

    python3 scripts/content/anim/own_poses.py <base>/models <poses.json> <pose dir> [name ...]

Every bone starts from the skeleton's bind pose (standing, arms down, facing +Z) and is set by the
spec: the pelvis, chest and head by angles, legs and arms by two-bone IK to an ankle or wrist target
with a knee or elbow direction, the feet by toe direction and heel lift, the sword hand by the blade
and true-edge directions, the free hand by palm and finger directions. Taken from the base are only
the skeleton, the bone lengths of the stance clip (every clip of ours shares them), its ankle height
over the floor and the finger's grip on the hilt. Writes one-frame records HUMANS-<name>.MAN.json
(the pose dir is the fifth argument of kimodo_moves.py) and prints the checks of each pose.

Frame: Gothic world, cm, Y up, the floor at 0, the character faces +Z, its right hand is at +X.
Angles in degrees: yaw > 0 turns to the character's right, pitch > 0 leans forward (towards +Z,
whatever the turn), roll > 0 bends to its left. Pelvis, chest and head angles are all in the world:
the three spine links share the turn from the pelvis to the chest, the neck a part of the turn
from the chest to the head; head yaw 0 looks at the target ahead.

The sword blade lies along ZS_RIGHTHAND's -X, as the engine binds the weapon mesh to the slot
(MdlVisual::bladePath; the converted .glb meshes are mirrored in X and show it along +X): a test clip
holding the slot's +X forward shows the blade backwards in the game. Its edges lie along the slot's ±Z
(the weapon meshes are 1.5 cm thick in Y and 6-10 cm wide in Z).
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gothic_anim as ga  # noqa: E402
import retarget_soma as rs  # noqa: E402

SPINE = (("BIP01 SPINE", 0.3), ("BIP01 SPINE1", 0.65), ("BIP01 SPINE2", 1.0))  # share of the chest turn
NECK_SHARE = 0.4  # of the head turn; the clavicles hang on the neck, so it stays small
BLADE_CM = 90.0
TORSO_R, HEAD_R = 14.0, 12.0  # capsule radii for the blade-through-body check


def unit(v) -> np.ndarray:
    v = np.asarray(v, float)
    return v / np.linalg.norm(v)


def rot(axis, deg: float) -> np.ndarray:
    a, t = unit(axis), np.radians(deg)
    k = np.array([[0, -a[2], a[1]], [a[2], 0, -a[0]], [-a[1], a[0], 0]])
    return np.eye(3) + np.sin(t) * k + (1 - np.cos(t)) * k @ k


def ypr(d: dict) -> np.ndarray:
    """Turn, then lean towards +Z: a body turned sideways still leans at its target."""
    return rot([1, 0, 0], d.get("pitch", 0)) @ rot([0, 1, 0], d.get("yaw", 0)) @ rot([0, 0, 1], d.get("roll", 0))


def share(r: np.ndarray, t: float) -> np.ndarray:
    """The rotation `r` scaled to the fraction `t` of its angle."""
    aa = rs.axis_angle(r)
    angle = np.linalg.norm(aa)
    return np.eye(3) if angle < 1e-9 else rot(aa, np.degrees(angle) * t)


def triad(u1, u2, w1, w2) -> np.ndarray:
    """Rotation taking local u1 exactly onto world w1 and local u2 as close as it goes onto w2."""
    def basis(a, b):
        a = unit(a)
        c = unit(np.cross(a, b))
        return np.column_stack([a, np.cross(c, a), c])
    return basis(w1, w2) @ basis(u1, u2).T


def limb(start, a: float, b: float, target, pole) -> tuple[np.ndarray, np.ndarray, float]:
    """Two-bone IK: the middle joint bends towards `pole`. Returns the middle joint, the end and how
    far the end falls short of the target (cm; 0 when it reaches)."""
    to = np.asarray(target, float) - start
    dist = float(np.linalg.norm(to))
    d = float(np.clip(dist, abs(a - b) + 1e-3, a + b - 1e-3))
    axis = to / dist
    p = np.asarray(pole, float) - axis * np.dot(pole, axis)
    p = unit(p)
    along = (a * a - b * b + d * d) / (2 * d)
    mid = start + axis * along + p * np.sqrt(max(a * a - along * along, 0.0))
    return mid, start + axis * d, max(dist - d, 0.0)


class Builder:
    def __init__(self, models: Path, stance: str):
        skel0 = ga.read_skeleton(models / "HUMANS.MDH.json")
        self.stance = ga.read_animation(models / f"{stance}.json")
        self.skel = rs.with_offsets(skel0, self.stance)
        self.gi = {n.name: i for i, n in enumerate(self.skel.nodes)}
        self.bind = np.array([m[:3, :3] for m in ga.world_pose(self.skel, None, 0)])
        self.bind_pos = np.array([m[:3, 3] for m in ga.world_pose(self.skel, None, 0)])
        st = ga.world_pose(self.skel, self.stance, 0)
        self.ankle_y = float(min(st[self.gi[f"BIP01 {s} FOOT"]][1, 3] for s in "LR"))
        # The grip of the fingers is the stance's: Kimodo has no fingers and finishing takes them from there.
        self.finger_local = {s: st[self.gi[f"BIP01 {s} HAND"]][:3, :3].T @ st[self.gi[f"BIP01 {s} FINGER0"]][:3, :3]
                             for s in "LR"}

    def off(self, name: str) -> np.ndarray:
        return self.skel.nodes[self.gi[name]].rest[:3, 3]

    def children(self, i: int) -> list[int]:
        return [j for j, n in enumerate(self.skel.nodes) if n.parent == i]

    def build(self, spec: dict) -> dict:
        gi, B, W = self.gi, self.bind, {}
        pel = spec["pelvis"]
        q_pel = ypr(pel)
        W["BIP01"] = rot([0, 1, 0], pel.get("yaw", 0)) @ B[gi["BIP01"]]
        W["BIP01 PELVIS"] = q_pel @ B[gi["BIP01 PELVIS"]]
        pelvis_pos = np.asarray(pel["pos"], float)
        root = pelvis_pos - W["BIP01"] @ self.off("BIP01 PELVIS")
        q_chest = ypr(spec.get("chest", {}))
        chest = q_pel.T @ q_chest
        for name, t in SPINE:
            W[name] = q_pel @ share(chest, t) @ B[gi[name]]
        q_head = ypr(spec.get("head", {}))
        W["BIP01 NECK"] = q_chest @ share(q_chest.T @ q_head, NECK_SHARE) @ B[gi["BIP01 NECK"]]
        W["BIP01 HEAD"] = q_head @ B[gi["BIP01 HEAD"]]
        for s, sign in (("L", -1), ("R", 1)):
            c = spec.get("clavicle", {}).get(s, {})
            # up: about the chest's forward axis; fwd: the shoulder forward, about the vertical
            q = rot([0, 0, 1], sign * c.get("up", 0)) @ rot([0, 1, 0], -sign * c.get("fwd", 0))
            W[f"BIP01 {s} CLAVICLE"] = q_chest @ q @ B[gi[f"BIP01 {s} CLAVICLE"]]
        report = {"miss": {}}
        pos = self.positions(W, root)
        for s in "LR":
            self.leg(spec["feet"][s], spec.get("knees", {}).get(s), s, W, pos, report)
            self.arm(spec["arms"][s], s, W, pos, report)
        return self.finish(W, root, report)

    def positions(self, W: dict, root: np.ndarray) -> dict:
        """World positions of the nodes whose parents are already set."""
        out = {}
        for i, n in enumerate(self.skel.nodes):
            if n.parent < 0:
                out[n.name] = root
            else:
                p = self.skel.nodes[n.parent].name
                if p in W and p in out:
                    out[n.name] = out[p] + W[p] @ n.rest[:3, 3]
        return out

    def leg(self, foot: dict, knee, s: str, W: dict, pos: dict, report: dict) -> None:
        gi, B = self.gi, self.bind
        names = [f"BIP01 {s} {x}" for x in ("THIGH", "CALF", "FOOT", "TOE0")]
        bf = B[gi[names[2]]]
        toe_w = bf @ self.off(names[3])
        fwd_l = bf.T @ unit([toe_w[0], 0, toe_w[2]])
        up_l = bf.T @ np.array([0.0, 1.0, 0.0])
        fwd = rot([0, 1, 0], foot.get("yaw", 0)) @ np.array([0.0, 0.0, 1.0])
        flat = triad(fwd_l, up_l, fwd, [0, 1, 0])
        ankle = np.array([foot["pos"][0], self.ankle_y, foot["pos"][1]])
        toe = ankle + flat @ self.off(names[3])
        lift = rot(np.cross([0, 1, 0], fwd), foot.get("heel", 0))  # heel up, pivoting on the toe
        r_foot = lift @ flat
        ankle = toe + lift @ (ankle - toe)
        pole = np.asarray(knee if knee is not None else fwd, float)
        hip = pos[names[0]]
        a, b = np.linalg.norm(self.off(names[1])), np.linalg.norm(self.off(names[2]))
        k, end, miss = limb(hip, a, b, ankle, pole)
        hinge = np.cross(unit(k - hip), unit(end - k))
        hinge = hinge if np.linalg.norm(hinge) > 1e-6 else np.cross(pole, end - hip)
        y = np.array([0.0, 1.0, 0.0])
        W[names[0]] = triad(unit(self.off(names[1])), y, k - hip, hinge)
        W[names[1]] = triad(unit(self.off(names[2])), y, end - k, hinge)
        W[names[2]] = r_foot
        W[names[3]] = flat @ bf.T @ B[gi[names[3]]]  # toes stay flat on the floor
        report["miss"][f"foot {s}"] = round(miss, 1)
        pos.update(self.positions(W, pos["BIP01"]))

    def arm(self, arm: dict, s: str, W: dict, pos: dict, report: dict) -> None:
        gi, B = self.gi, self.bind
        names = [f"BIP01 {s} {x}" for x in ("UPPERARM", "FOREARM", "HAND", "FINGER0")]
        sh = pos[names[0]]
        a, b = np.linalg.norm(self.off(names[1])), np.linalg.norm(self.off(names[2]))
        target = np.asarray(arm["hand"], float)
        if "blade" in arm:
            # The hilt sits across the fist, 90° to the hand's axis, so the blade can never run along
            # the forearm: the wrist bends towards the little finger by `wrist` degrees and the forearm
            # lies between the true edge and the blade. The arm is laid out from that forearm, the hand
            # coming as close to the target as the upper arm lets it.
            blade = unit(arm["blade"])
            if "forearm" in arm:
                fx = unit(arm["forearm"])
                edge = unit(fx - np.dot(fx, blade) * blade)
            else:
                edge = unit(np.asarray(arm["edge"], float) - np.dot(arm["edge"], blade) * blade)
                d = np.radians(arm.get("wrist", 30))
                fx = np.cos(d) * edge + np.sin(d) * blade
            u = unit(target - b * fx - sh)
            e = sh + a * u
            end = e + b * fx
            miss = float(np.linalg.norm(end - target))
        else:
            e, end, miss = limb(sh, a, b, target, arm["elbow"])
        hinge = np.cross(unit(e - sh), unit(end - e))
        hinge = hinge if np.linalg.norm(hinge) > 1e-3 else np.cross(arm.get("elbow", [0, -1, 0]), end - sh)
        y = np.array([0.0, 1.0, 0.0])
        W[names[0]] = triad(unit(self.off(names[1])), y, e - sh, hinge)
        fore = triad(unit(self.off(names[2])), y, end - e, hinge)
        bh = B[gi[names[2]]]
        if "blade" in arm:
            slot = self.skel.nodes[gi["ZS_RIGHTHAND"]].rest[:3, :3]
            blade_l, z_l = slot @ [-1.0, 0, 0], slot @ [0, 0, 1.0]
            # the true edge is the one in line with the knuckles, where the hand's X points
            edge_l = z_l if np.dot(z_l, [1.0, 0, 0]) > 0 else -z_l
            hand = triad(blade_l, edge_l, blade, edge)
        else:
            # bind: arms hang with the palms to the thighs; the wrist stays straight unless "fingers" bends it
            palm_l = bh.T @ np.array([-1.0 if s == "R" else 1.0, 0, 0])
            hand = triad([1.0, 0, 0], palm_l, arm.get("fingers", end - e), arm["palm"])
        # Half of the forearm-hand twist goes into the forearm, as the radius turns: skinning only,
        # the wrist stays on the forearm's axis.
        rel = fore.T @ hand
        twist = np.degrees(np.arctan2(rel[2, 1] - rel[1, 2], rel[1, 1] + rel[2, 2]))
        W[names[1]] = fore @ rot([1, 0, 0], 0.5 * twist)
        W[names[2]] = hand
        W[names[3]] = hand @ self.finger_local[s]
        report["miss"][f"hand {s}"] = round(miss, 1)
        pos.update(self.positions(W, pos["BIP01"]))

    def finish(self, W: dict, root: np.ndarray, report: dict) -> dict:
        gi = self.gi
        world = np.zeros((len(self.skel.nodes), 3, 3))
        for i, n in enumerate(self.skel.nodes):
            if n.name in W:
                world[i] = W[n.name]
            else:  # slots and helpers keep their bind offset to the parent
                world[i] = world[n.parent] @ n.rest[:3, :3]
        p = rs.world_positions(self.skel, world, root)
        report.update(self.checks(world, p))
        return {"world": world, "root": root, "report": report}

    def checks(self, world: np.ndarray, p: np.ndarray) -> dict:
        gi = self.gi
        out = {}
        for s in "LR":
            for top, mid, end, key in (("THIGH", "CALF", "FOOT", "knee"), ("UPPERARM", "FOREARM", "HAND", "elbow")):
                a, b, c = (p[gi[f"BIP01 {s} {x}"]] for x in (top, mid, end))
                out[f"{key} {s}"] = round(float(np.degrees(np.arccos(np.clip(unit(a - b) @ unit(c - b), -1, 1)))), 1)
            # A broken wrist: the hand's axis off the forearm's (both point along local X)
            fx, hx = world[gi[f"BIP01 {s} FOREARM"]][:, 0], world[gi[f"BIP01 {s} HAND"]][:, 0]
            out[f"wrist {s}"] = round(float(np.degrees(np.arccos(np.clip(fx @ hx, -1, 1)))), 1)
        slot = world[gi["ZS_RIGHTHAND"]]
        base = p[gi["ZS_RIGHTHAND"]]
        tip = base + slot @ np.array([-BLADE_CM, 0, 0])
        out["tip"] = tip.round(1).tolist()
        pel, neck, head = p[gi["BIP01 PELVIS"]], p[gi["BIP01 NECK"]], p[gi["BIP01 HEAD"]]
        blade = [base + (tip - base) * t for t in np.linspace(0.15, 1, 18)]

        def seg(x, a, b):
            t = np.clip(np.dot(x - a, b - a) / np.dot(b - a, b - a), 0, 1)
            return float(np.linalg.norm(x - (a + t * (b - a))))
        out["blade_body_cm"] = round(min(min(seg(x, pel, neck) - TORSO_R, np.linalg.norm(x - head - [0, 8, 0]) - HEAD_R)
                                         for x in blade), 1)
        lean = neck - pel
        out["lean"] = round(float(np.degrees(np.arctan2(lean[2], lean[1]))), 1)
        across = p[gi["BIP01 R UPPERARM"]] - p[gi["BIP01 L UPPERARM"]]
        out["shoulder_yaw"] = round(float(np.degrees(np.arctan2(-across[2], across[0]))), 1)
        feet = np.array([p[gi[f"BIP01 {s} FOOT"]][[0, 2]] for s in "LR"])
        out["stance_cm"] = (feet[0] - feet[1]).round(1).tolist()
        # Centre of mass, roughly: pelvis 0.45, chest 0.35, head 0.1, arms 0.1 — over the line between the feet?
        com = (0.45 * pel + 0.35 * p[gi["BIP01 SPINE2"]] + 0.1 * head
               + 0.05 * p[gi["BIP01 L FOREARM"]] + 0.05 * p[gi["BIP01 R FOREARM"]])[[0, 2]]
        a, b = feet
        t = float(np.clip(np.dot(com - a, b - a) / np.dot(b - a, b - a), 0, 1))
        out["com_off_cm"] = round(float(np.linalg.norm(com - (a + t * (b - a)))), 1)
        out["com_share_R"] = round(t, 2)  # 0 over the left foot, 1 over the right
        out["hand R"] = p[gi["BIP01 R HAND"]].round(1).tolist()
        return out

    def save(self, path: Path, name: str, pose: dict, source: str) -> None:
        world, root = pose["world"], pose["root"]
        q, ps = [], []
        for i, n in enumerate(self.skel.nodes):
            if n.parent < 0:
                q.append(rs.mat_to_quat(world[i])); ps.append(root)
            else:
                q.append(rs.mat_to_quat(world[n.parent].T @ world[i])); ps.append(n.rest[:3, 3])
        anim = ga.Animation(name=name, next="", layer=1, fps=25.0, frame_count=1,
                            node_indices=list(range(len(self.skel.nodes))),
                            positions=np.array([ps]), rotations=np.array([q]))
        ga.write_animation(path, anim, source=source)


def main():
    models, spec_path, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    only = set(sys.argv[4:])
    spec = json.loads(spec_path.read_text())
    b = Builder(models, spec["stance"])
    out.mkdir(parents=True, exist_ok=True)
    for name, ps in spec["poses"].items():
        if only and name not in only:
            continue
        pose = b.build(ps)
        b.save(out / f"HUMANS-{name}.MAN.json", name, pose, source=f"own pose {spec_path.name}:{name}")
        print(name, json.dumps(pose["report"]))


if __name__ == "__main__":
    main()
