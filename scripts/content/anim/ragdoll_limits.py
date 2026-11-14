"""A pose against the joint limits of the engine's human ragdoll.

    python3 scripts/content/anim/ragdoll_limits.py <base>/models <record.MAN.json>... [--frame N] [--json]

A pose that a ragdoll motor is driven to has to lie inside the limits of its joints: a target outside
them makes the motor push against the limit for as long as it is held. This script repeats the joint
table of OpenGothic/common/physics/ragdoll.cpp (`humanJoint`, `anatomicalCone` and the knee and elbow
hinges) on the bind pose of HUMANS.MDH and measures a frame of each record in the joints' own terms:

- cone joints: `swing` is how far the bone is from the centre of its elliptic cone, as a share of the
  limit in that direction (1.0 is on the limit), `flex` and `side` are the anatomical angles the
  table is written in, `twist` is the turn about the bone with its range;
- hinges (knee, elbow): `flex` from the straight limb with the range, `off` is the turn the hinge
  cannot make (about the bone or sideways), which should stay within a few degrees.

The engine is the authority (its contract test checks the same poses on the real constraints); this
is the first look while the numbers of a pose are being chosen. Exit code 1 when a joint is outside.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gothic_anim as ga  # noqa: E402
import retarget_soma as rs  # noqa: E402

# flexMin, flexMax, sideMin, sideMax, twistMin, twistMax — degrees, as in humanJoint
RANGES = {
    "THIGH": (-15, 110, -20, 40, -40, 30),
    "FOOT": (-45, 35, -15, 15, -15, 25),
    "TOE": (-25, 45, -10, 10, -5, 5),
    "SPINE": (-8, 25, -10, 10, -12, 12),
    "NECK": (-25, 30, -20, 20, -35, 35),
    "HEAD": (-25, 30, -20, 20, -35, 35),
    "CLAVICLE": (-12, 12, -8, 15, -10, 10),
    "UPPERARM": (-40, 135, -30, 120, -70, 70),
    "HAND": (-45, 45, -45, 45, -80, 80),
    "FINGER": (-30, 30, -30, 30, -10, 10),
}
HINGE_MAX = {"CALF": 150.0, "FOREARM": 145.0}
OFF_MAX = 3.0  # degrees a hinge target may ask for outside its axis


def unit(v) -> np.ndarray:
    v = np.asarray(v, float)
    return v / np.linalg.norm(v)


def perpendicular(axis, preferred) -> np.ndarray:
    r = np.asarray(preferred, float) - axis * np.dot(preferred, axis)
    if np.linalg.norm(r) < 1e-6:
        r = np.cross(axis, [1.0, 0, 0] if abs(axis[0]) < 0.9 else [0, 1.0, 0])
    return unit(r)


def rot(axis, angle: float) -> np.ndarray:
    a = unit(axis)
    k = np.array([[0, -a[2], a[1]], [a[2], 0, -a[0]], [-a[1], a[0], 0]])
    return np.eye(3) + np.sin(angle) * k + (1 - np.cos(angle)) * k @ k


def from_to(a, b) -> np.ndarray:
    axis = np.cross(a, b)
    s = np.linalg.norm(axis)
    return np.eye(3) if s < 1e-9 else rot(axis, np.arctan2(s, np.dot(a, b)))


def kind(name: str) -> str | None:
    for k in ("CALF", "FOREARM", *RANGES):
        if k in name:
            return k
    return None


class Limits:
    def __init__(self, models: Path):
        self.skel = ga.read_skeleton(models / "HUMANS.MDH.json")
        nodes = self.skel.nodes
        self.gi = {n.name: i for i, n in enumerate(nodes)}
        bind = ga.world_pose(self.skel, None, 0)
        self.bind = np.array([m[:3, :3] for m in bind])
        self.pos = np.array([m[:3, 3] for m in bind])
        # Parts of the ragdoll: every bone but the root and the slots (HitZones); the pelvis is its root.
        self.part = [n.parent >= 0 and not n.name.startswith("ZS_") for n in nodes]
        at = lambda name: self.pos[self.gi[name]]  # noqa: E731
        left = unit(at("BIP01 L THIGH") - at("BIP01 R THIGH"))
        up = perpendicular(left, [0, 1.0, 0])
        fwd = np.cross(left, up)
        toes = at("BIP01 L TOE0") - at("BIP01 L FOOT") + at("BIP01 R TOE0") - at("BIP01 R FOOT")
        self.axes = (left, up, fwd if np.dot(fwd, toes) >= 0 else -fwd)
        self.joints = {}
        for i, n in enumerate(nodes):
            if self.part[i] and self.part[n.parent] and kind(n.name):
                self.joints[n.name] = self.joint(i)

    def chain(self, i: int) -> np.ndarray:
        """Direction of the bone: to the child that continues its chain, or on from its parent."""
        nodes = self.skel.nodes
        for j, c in enumerate(nodes):
            if c.parent == i and self.part[j] and "CLAVICLE" not in c.name and "THIGH" not in c.name:
                d = self.pos[j] - self.pos[i]
                if np.linalg.norm(d) > 1e-4:
                    return unit(d)
        return unit(self.pos[i] - self.pos[nodes[i].parent])

    def joint(self, i: int) -> dict:
        n = self.skel.nodes[i]
        k = kind(n.name)
        left, up, fwd = self.axes
        bone = self.chain(i)
        if k in HINGE_MAX:
            upper = unit(self.pos[i] - self.pos[n.parent])
            cross = np.cross(upper, bone)
            axis = unit(cross)
            return {"kind": k, "hinge": axis, "bend": float(np.arctan2(np.linalg.norm(cross), np.dot(upper, bone)))}
        lateral = left * (-1.0 if " R " in n.name else 1.0)
        r = RANGES[k]
        if k in ("THIGH", "UPPERARM"):
            neutral, front, side, medial = -up, fwd, lateral, -lateral
        elif k in ("FOOT", "TOE"):
            neutral, front, side, medial = bone, up, lateral, lateral
        elif k in ("SPINE", "NECK", "HEAD"):
            neutral, front, side, medial = bone, fwd, left, left
        else:  # CLAVICLE, HAND, FINGER
            neutral, front, side, medial = bone, fwd, up, up
        rad = np.pi / 180
        f = perpendicular(neutral, front)
        s = perpendicular(f, perpendicular(neutral, side))
        offset = f * ((r[0] + r[1]) * 0.5 * rad) + s * ((r[2] + r[3]) * 0.5 * rad)
        centre = np.eye(3)
        if np.linalg.norm(offset) > 1e-4:
            centre = rot(np.cross(neutral, offset), np.linalg.norm(offset))
        c, cf = unit(centre @ neutral), unit(centre @ f)
        plane2 = perpendicular(bone, from_to(c, bone) @ cf)
        positive = np.dot(np.cross(c, cf), medial) >= 0
        # Jolt's constraint frame: X the twist axis, Y the normal (plane x twist), Z the plane axis.
        frame = lambda t, p: np.column_stack([t, np.cross(p, t), p])  # noqa: E731
        return {"kind": k, "range": r, "f1": frame(c, cf), "f2": frame(bone, plane2), "bone": bone,
                "neutral": unit(neutral), "front": f, "side": s,
                "plane": (r[1] - r[0]) * 0.5 * rad, "normal": (r[3] - r[2]) * 0.5 * rad,
                "twist": ((r[4], r[5]) if positive else (-r[5], -r[4])), "positive": positive}

    def measure(self, world: np.ndarray, pos: np.ndarray) -> dict:
        """`world`: rotations of every node of the pose in the model frame; `pos`: their positions."""
        out = {}
        for name, j in self.joints.items():
            i = self.gi[name]
            p = self.skel.nodes[i].parent
            if "hinge" in j:
                a, b, c = pos[p], pos[i], pos[self.child(i)]
                flex = 180.0 - np.degrees(np.arccos(np.clip(unit(a - b) @ unit(c - b), -1, 1)))
                # What the pose asks of the hinge beyond a turn about its axis.
                rel = (world[p] @ self.bind[p].T).T @ (world[i] @ self.bind[i].T)
                aa = rs.axis_angle(rel)
                along = np.dot(aa, j["hinge"]) * j["hinge"]
                off = float(np.degrees(np.linalg.norm(aa - along)))
                top = HINGE_MAX[j["kind"]]
                out[name] = {"flex": round(float(flex), 1), "range": [0.0, top], "off": round(off, 1),
                             "ok": bool(-1.0 <= flex <= top + 1.0 and off <= OFF_MAX)}
                continue
            f1 = world[p] @ self.bind[p].T @ j["f1"]
            f2 = world[i] @ self.bind[i].T @ j["f2"]
            q = rs.mat_to_quat(f1.T @ f2)  # w, x, y, z
            w, x, y, z = (q if q[0] >= 0 else -q)
            tn = np.hypot(w, x)
            twist = float(np.degrees(2 * np.arctan2(x, w)))
            if tn < 1e-9:
                sy, sz = y, z
            else:  # swing = q * conj(twist)
                tw, tx = w / tn, x / tn
                sy, sz = y * tw - z * tx, z * tw + y * tx
            share = float(np.hypot(sy / np.sin(j["plane"] / 2), sz / np.sin(j["normal"] / 2)))
            # Anatomical angles: the shortest swing from the neutral direction to the bone, in the parent's frame.
            d = (world[p] @ self.bind[p].T).T @ (world[i] @ self.bind[i].T @ j["bone"])
            n = j["neutral"]
            across = d - n * np.dot(d, n)
            angle = np.degrees(np.arctan2(np.linalg.norm(across), np.dot(d, n)))
            across = across / max(np.linalg.norm(across), 1e-12)
            flex, side = angle * np.dot(across, j["front"]), angle * np.dot(across, j["side"])
            r = j["range"]
            lo, hi = j["twist"]
            out[name] = {"swing": round(share, 2), "flex": round(float(flex), 1), "side": round(float(side), 1),
                         "twist": round(twist if j["positive"] else -twist, 1), "twist_range": [r[4], r[5]],
                         "ok": bool(share <= 1.0 and lo - 0.5 <= twist <= hi + 0.5)}
        return out

    def child(self, i: int) -> int:
        for j, c in enumerate(self.skel.nodes):
            if c.parent == i and self.part[j]:
                return j
        raise ValueError(self.skel.nodes[i].name)

    def record(self, path: Path, frame: int = 0) -> dict:
        anim = ga.read_animation(path)
        skel = rs.with_offsets(self.skel, anim)
        mats = ga.world_pose(skel, anim, frame)
        return self.measure(np.array([m[:3, :3] for m in mats]), np.array([m[:3, 3] for m in mats]))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("models", type=Path)
    ap.add_argument("records", nargs="+", type=Path)
    ap.add_argument("--frame", type=int, default=0)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    limits = Limits(args.models)
    bad = 0
    report = {}
    for path in args.records:
        m = limits.record(path, args.frame)
        report[path.name] = m
        bad += sum(not v["ok"] for v in m.values())
        if args.json:
            continue
        print(path.name)
        for name, v in m.items():
            mark = "" if v["ok"] else "   <-- outside"
            if "swing" in v:
                print(f"  {name:20s} swing {v['swing']:4.2f}  flex {v['flex']:6.1f}  side {v['side']:6.1f}  "
                      f"twist {v['twist']:6.1f} of {v['twist_range']}{mark}")
            else:
                print(f"  {name:20s} flex {v['flex']:6.1f} of {v['range']}  off-axis {v['off']:4.1f}{mark}")
    if args.json:
        print(json.dumps(report, indent=1))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
