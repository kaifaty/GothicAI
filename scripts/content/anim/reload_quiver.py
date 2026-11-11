"""The reload from the quiver: own T_BOWRELOADQ clips for the three bow talent steps, from key poses.

    python3 scripts/content/anim/reload_quiver.py <base>/models <reload.json> <out dir> [content dir]

Plan, keys and acceptance: reports/archery/2026-11-05-quiver-reload/README.md (the idea:
reports/archery/2026-11-04-reload-from-quiver). Each clip is the talent step's aim (`C_BOW_5` of `S_BOWAIM`, held still) with
the right arm (clavicle to finger, the arrow's slot ZS_RIGHTHAND) and the head of our own: the hand goes from the
ear after the shot to the quiver behind the right hip, takes an arrow by its fletching, draws it out straight along
the quiver's axis, lays it on the bow hand, slides back to the nock and takes the string. The engine plays it as
T_BOWRELOAD (OPENGOTHIC_QUIVERRELOAD) with the length in frames of G2's, so the tempo by dexterity stays; BowDraw
holds the upper body and the bow arm in the aim, leads the hand to the quiver as it hangs and to the string as the
aim has it.

The hand is led by keys of the arrow's grip point and direction in the chest's frame (BIP01 SPINE2: x up the spine,
y left, z forward): `aim` (the stance's arrow, moved `back` along it and `out` from the head), `shoulder` (out of the
shoulder, cm), `quiver` (`u` cm up the quiver's axis from its mouth, the arrow along the axis), `shelf` (the arrow
on the bow's line, its nock `ahead` cm in front of the string's middle), `string` (the stance's arrow with the nock
on the string). Between keys the grip point follows cubic Hermite segments (a straight one along the quiver's
axis while the arrow's tip is in it), the arrow's turn an eased slerp; the arm is solved by a two-bone IK every
frame, the elbow toward the key's `elbow` (chest frame), the wrist's twist half into the forearm. `grip` is where
the hand holds the arrow, cm from its nock: the arrow slides in the hand by the slot ZS_RIGHTHAND. The head
pitches down (> 0) by its own keys. Writes HUMANS-like records <LEVEL>-T_BOWRELOADQ.MAN.json, a full-body preview
and prints the checks; with a content dir packs them with the events of G2's entry, MUNITION_PLACE and the sound at
the grab's frame, the string's morphs.
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import gothic_anim as ga  # noqa: E402
import retarget_soma as rs  # noqa: E402

ARM = ("BIP01 R CLAVICLE", "BIP01 R UPPERARM", "BIP01 R FOREARM", "BIP01 R HAND", "BIP01 R FINGER0", "ZS_RIGHTHAND")
NODES = ARM + ("BIP01 HEAD",)
NAME = "T_BOWRELOADQ"
HEAD_R = 10.0  # cm: the head with the hair, a capsule from BIP01 HEAD to ZS_HELMET (BowDraw's HeadR)


def unit(v) -> np.ndarray:
    v = np.asarray(v, float)
    return v / np.linalg.norm(v)


def smooth(x: float) -> float:
    x = min(max(x, 0.0), 1.0)
    return x * x * (3 - 2 * x)


def rot_y(deg: float) -> np.ndarray:
    c, s = np.cos(np.radians(deg)), np.sin(np.radians(deg))
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])


def axis_rot(axis, deg: float) -> np.ndarray:
    a = unit(axis)
    k = np.array([[0, -a[2], a[1]], [a[2], 0, -a[0]], [-a[1], a[0], 0]])
    t = np.radians(deg)
    return np.eye(3) + np.sin(t) * k + (1 - np.cos(t)) * k @ k


def triad(u1, u2, w1, w2) -> np.ndarray:
    """Rotation taking local u1 exactly onto world w1 and local u2 as close as it goes onto w2."""
    def basis(a, b):
        a = unit(a)
        c = unit(np.cross(a, b))
        return np.column_stack([a, np.cross(c, a), c])
    return basis(w1, w2) @ basis(u1, u2).T


def seg_dist(p, a, b) -> float:
    ab = b - a
    return float(np.linalg.norm(p - (a + ab * np.clip(np.dot(p - a, ab) / max(np.dot(ab, ab), 1e-9), 0, 1))))


class Stance:
    """The talent step's aim (C_BOW_5): world transforms, local ones, the quiver at rest on its pelvis."""

    def __init__(self, models: Path, level: str, spec: dict):
        skel0 = ga.read_skeleton(models / "HUMANS.MDH.json")
        # the aim the game holds: S_BOWAIM straight ahead (C_BOW_5), drawn
        self.anim = ga.read_animation(models / f"{level}-C_BOW_5.MAN.json")
        self.skel = rs.with_offsets(skel0, self.anim)
        self.gi = {n.name: i for i, n in enumerate(self.skel.nodes)}
        self.world = ga.world_pose(self.skel, self.anim, 0)
        self.local = {n.name: self.skel.nodes[i].rest.copy() for i, n in enumerate(self.skel.nodes)}
        for col, node in enumerate(self.anim.node_indices):
            self.local[self.skel.nodes[node].name] = ga.sample_matrix(self.anim.positions[0, col], self.anim.rotations[0, col])
        self.chest = self.W("BIP01 SPINE2")
        self.chest_inv = np.linalg.inv(self.chest)
        # the quiver as the arrow is drawn out of it: turned round the ring onto the line (Quiver::ReloadTurnX, Z)
        q = spec["quiver"]
        turn = np.eye(4)
        turn[:3, :3] = swing_of(axis_of_angle(q.get("turn", [0.0, 0.0])))
        self.quiver = self.W("BIP01 PELVIS") @ quiver_local(skel0, models, q) @ turn
        self.span = float(spec["span"])
        arrow = spec["arrow"]
        self.nock_back, self.tip_ahead = float(arrow["nock_back"]), float(arrow["tip_ahead"])
        # the arrow of the aim: the slot ZS_RIGHTHAND, its tip along -X
        slot = self.W("ZS_RIGHTHAND")
        self.aim_dir = self.c_dir(-slot[:3, 0])
        self.aim_nock = self.c(slot[:3, 3]) - self.aim_dir * self.nock_back
        self.slot_aim = self.local["ZS_RIGHTHAND"].copy()
        self.hand_aim = self.chest_inv @ self.W("BIP01 R HAND")

    def W(self, name: str) -> np.ndarray:
        return self.world[self.gi[name]]

    def c(self, p) -> np.ndarray:
        return (self.chest_inv @ np.append(p, 1.0))[:3]

    def c_dir(self, d) -> np.ndarray:
        return unit(self.chest_inv[:3, :3] @ d)

    def mouth(self) -> np.ndarray:
        return self.c(self.quiver[:3, 3])

    def down(self) -> np.ndarray:
        return self.c_dir(-self.quiver[:3, 1])

    def head(self, pitch: float) -> np.ndarray:
        local = self.local["BIP01 HEAD"].copy()
        local[:3, :3] = local[:3, :3] @ rot_y(pitch)
        return self.W("BIP01 NECK") @ local


def axis_of_angle(a) -> np.ndarray:
    """Quiver's axisOfAngle: the bottom's way in the rest frame for a turn by its angle across (x, z radians)."""
    x, z = float(a[0]), float(a[1])
    ln = np.hypot(x, z)
    if ln < 1e-9:
        return np.array([0.0, -1.0, 0.0])
    return np.array([np.sin(ln) * x / ln, -np.cos(ln), np.sin(ln) * z / ln])


def swing_of(axis) -> np.ndarray:
    """The rotation taking the rest's bottom (0, -1, 0) to the axis about their common normal (Quiver's swingOf)."""
    f, t = np.array([0.0, -1.0, 0.0]), unit(axis)
    k = np.cross(f, t)
    s = np.linalg.norm(k)
    if s < 1e-9:
        return np.eye(3)
    return axis_rot(k / s, np.degrees(np.arctan2(s, np.dot(f, t))))


def quiver_local(skel0: ga.Skeleton, models: Path, q: dict) -> np.ndarray:
    """The quiver at rest in the pelvis bone's frame as Quiver::fit puts it (graphics/mesh/quiver.cpp), pushed `out`
    cm: the mouth's centre at the origin, the bottom along -Y."""
    bind = ga.world_pose(skel0, None, 0)
    gi = {n.name: i for i, n in enumerate(skel0.nodes)}
    floor = ga.read_skin_mesh(models / q["body"], skel0).positions[:, 1].min()
    up, right, fwd = np.array([0, 1.0, 0]), np.array([1.0, 0, 0]), np.array([0, 0, 1.0])
    if bind[gi["BIP01 R THIGH"]][0, 3] < bind[gi["BIP01 L THIGH"]][0, 3]:
        right = -right
    ar = np.radians(q["around"])
    right = right * np.cos(ar) - fwd * np.sin(ar)
    pel = bind[gi["BIP01 PELVIS"]][:3, 3]
    mouth = np.array([pel[0], floor + q["height"], pel[2]]) + right * q["out"]
    down = unit(-up - fwd * np.tan(np.radians(q["tilt_back"])) + right * np.tan(np.radians(q["tilt_out"])))
    y = -down
    z = unit(fwd - y * np.dot(fwd, y))
    x = np.cross(y, z)
    m = np.eye(4)
    m[:3, 0], m[:3, 1], m[:3, 2], m[:3, 3] = x, y, z, mouth
    return np.linalg.inv(bind[gi["BIP01 PELVIS"]]) @ m


def key_grip(st: Stance, key: dict) -> tuple[np.ndarray, np.ndarray]:
    """The arrow's grip point and its direction (to the tip) a key asks for, in the chest's frame."""
    h = key["hand"]
    kind = h["at"]
    if kind == "aim":
        # the arrow of the aim, the grip at the nock: back along it, out from the head
        d = st.aim_dir
        out = unit(np.cross(d, [1.0, 0, 0]))
        if np.dot(out, st.aim_nock - st.c(st.W("BIP01 HEAD")[:3, 3])) < 0:
            out = -out
        p = st.aim_nock + d * key.get("grip", 3.0) - d * h.get("back", 0.0) + out * h.get("out", 0.0)
        return p, d
    if kind == "quiver":
        return st.mouth() - st.down() * h["u"], st.down()
    if kind == "shelf" or kind == "string":
        # the string's middle at rest: the aim's nock `span` forward along the arrow (as BowDraw holds the bow)
        d = st.aim_dir
        nock = st.aim_nock + d * (st.span + h.get("ahead", 0.0)) + np.asarray(h.get("offset", [0, 0, 0]), float)
        return nock + d * key["grip"], unit(d + np.asarray(h.get("tilt", [0, 0, 0]), float))
    if kind == "arrow":
        # the arrow measured in the game (chest frame): its nock and way; the nock `ahead` cm along it
        d = unit(h["dir"])
        return np.asarray(h["nock"], float) + d * (h.get("ahead", 0.0) + key.get("grip", 3.0)), d
    if kind == "point":
        return np.asarray(h["chest"], float), unit(h["dir"])
    raise ValueError(kind)


class Arm:
    """The right arm hung on the stance's neck: two-bone IK for the hand bone, the wrist's twist half into the forearm."""

    def __init__(self, st: Stance):
        self.st = st
        self.neck = st.W("BIP01 NECK")
        self.clav = self.neck @ st.local["BIP01 R CLAVICLE"]
        self.up = self.clav @ st.local["BIP01 R UPPERARM"]
        self.a = float(np.linalg.norm(st.local["BIP01 R FOREARM"][:3, 3]))
        self.b = float(np.linalg.norm(st.local["BIP01 R HAND"][:3, 3]))
        self.bind_hand = st.skel.nodes[st.gi["BIP01 R HAND"]].rest[:3, :3]

    def solve(self, hand_world: np.ndarray, pole: np.ndarray) -> tuple[dict, dict, np.ndarray]:
        st = self.st
        a, b = self.a, self.b
        sh, end = self.up[:3, 3], hand_world[:3, 3]
        to = end - sh
        dist = float(np.linalg.norm(to))
        d = float(np.clip(dist, abs(a - b) + 1e-3, a + b - 1e-3))
        axis = to / dist
        along = (a * a - b * b + d * d) / (2 * d)
        height = np.sqrt(max(a * a - along * along, 0.0))
        end = sh + axis * d
        p = unit(pole - axis * np.dot(pole, axis))
        e = sh + axis * along + p * height
        hinge = unit(np.cross(p, axis))
        y = np.array([0.0, 1.0, 0.0])
        w_up = triad(unit(st.local["BIP01 R FOREARM"][:3, 3]), y, e - sh, hinge)
        w_fore = triad(unit(st.local["BIP01 R HAND"][:3, 3]), y, end - e, hinge)
        w_hand = hand_world[:3, :3]
        rel = self.bind_hand.T @ (w_fore.T @ w_hand)
        twist = float(np.degrees(np.arctan2(rel[2, 1] - rel[1, 2], rel[1, 1] + rel[2, 2])))
        c, sn = np.cos(np.radians(0.5 * twist)), np.sin(np.radians(0.5 * twist))
        w_fore = w_fore @ np.array([[1, 0, 0], [0, c, -sn], [0, sn, c]])
        locals_ = {"BIP01 R UPPERARM": self.clav[:3, :3].T @ w_up, "BIP01 R FOREARM": w_up.T @ w_fore,
                   "BIP01 R HAND": w_fore.T @ w_hand}
        return locals_, {"miss": dist - d, "twist": abs(twist)}, e


def hermite(p0, p1, m0, m1, s):
    h00, h10, h01, h11 = 2 * s**3 - 3 * s**2 + 1, s**3 - 2 * s**2 + s, -2 * s**3 + 3 * s**2, s**3 - s**2
    return h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1


def slerp_m(a: np.ndarray, b: np.ndarray, t: float) -> np.ndarray:
    return ga.quat_matrix(rs.slerp(rs.mat_to_quat(a), rs.mat_to_quat(b), t))


def grip_frame(st: Stance, grip: np.ndarray, d: np.ndarray, roll: float, ref: np.ndarray) -> np.ndarray:
    """The slot's frame at the grip (chest): -X along the arrow (to its tip), Y rolled about it from `ref` by `roll`."""
    x = -unit(d)
    yv = unit(ref - x * np.dot(ref, x))
    yv = axis_rot(x, roll) @ yv
    z = np.cross(x, yv)
    m = np.eye(4)
    m[:3, 0], m[:3, 1], m[:3, 2], m[:3, 3] = x, yv, z, grip
    return m


def build(st: Stance, lv: dict, spec: dict, name: str) -> tuple[ga.Animation, dict, list]:
    fps = float(spec["fps"])
    frames = int(lv["frames"])
    total = frames / fps
    keys = lv["keys"]
    kp = np.array([k["p"] for k in keys])
    grips = [key_grip(st, k) if k["hand"]["at"] != "between" else (np.asarray(k["hand"]["chest"], float), None) for k in keys]
    pts = np.array([g[0] for g in grips])
    dirs = [g[1] for g in grips]
    for i, d in enumerate(dirs):
        if d is None:  # the arrow's (the hand's) way halfway from the key before to the one after
            w = keys[i]["hand"].get("w", 0.5)
            dirs[i] = unit(dirs[i - 1] * (1 - w) + dirs[i + 1] * w)
    # the elbow of the stance (where the aim has it), for keys that ask for it
    sh, el, hd = (st.c(st.W(n)[:3, 3]) for n in ("BIP01 R UPPERARM", "BIP01 R FOREARM", "BIP01 R HAND"))
    stance_elbow = unit(el - (sh + hd) / 2)
    for k in keys:
        if k["elbow"] == "stance":
            k["elbow"] = list(stance_elbow)
    arm = Arm(st)
    # the slot's Y of the aim (the hand's turn round the arrow), the reference every roll is from
    aim_slot = st.chest_inv @ st.W("ZS_RIGHTHAND")
    ref_y = aim_slot[:3, 1]
    # tangents (cm per unit p): 0 where the hand stops (`stop`), along the quiver's axis on the way out
    # (`along`), else Catmull-Rom
    tang = np.zeros_like(pts)
    for i, k in enumerate(keys):
        if k.get("stop", False) or i == 0 or i == len(keys) - 1:
            continue
        cr = (pts[i + 1] - pts[i - 1]) / (kp[i + 1] - kp[i - 1])
        if "along" in k:
            ax = -st.down()
            tang[i] = ax * np.dot(cr, ax) * k["along"]
        else:
            tang[i] = cr * k.get("tension", 1.0)
    # the hand's roll round the arrow at each key: the least twist of the wrist, near the key before's turn
    hold0 = st.slot_aim.copy()
    rolls = []
    prev = None
    for i, k in enumerate(keys):
        if "roll" in k:
            rolls.append(float(k["roll"]))
            prev = grip_frame(st, pts[i], dirs[i], rolls[-1], ref_y)[:3, :3]
            continue
        best = None
        for roll in range(-180, 180, 10):
            fr = grip_frame(st, pts[i], dirs[i], float(roll), ref_y)
            hold = hold0.copy()
            g = k.get("grip", 3.0)
            hold[:3, 3] = hold[:3, 3] + (hold[:3, :3] @ np.array([-1.0, 0, 0])) * (3.0 - g)
            slot = fr.copy()
            slot[:3, 3] = pts[i] + dirs[i] * (st.nock_back - g)
            _, r, _ = arm.solve(st.chest @ (slot @ np.linalg.inv(hold)), st.chest[:3, :3] @ unit(k["elbow"]))
            turn = 0.0 if prev is None else float(np.degrees(np.linalg.norm(rs.axis_angle(fr[:3, :3] @ prev.T))))
            cost = r["twist"] + 0.3 * turn + 20.0 * max(r["miss"], 0.0)
            if best is None or cost < best[0]:
                best = (cost, float(roll), fr[:3, :3])
        rolls.append(best[1])
        prev = best[2]
        k["roll"] = best[1]
    t = np.arange(frames) / fps
    p_of = t / total
    hk = np.array(lv["head"], float)
    from finish import monotone_cubic
    pitch = monotone_cubic(hk[:, 0], hk[:, 1], p_of)
    rot = np.zeros((frames, len(NODES), 4))
    loc = np.zeros((frames, len(NODES), 3))
    report = {"miss": 0.0, "twist": 0.0}
    track = []
    for f in range(frames):
        p = p_of[f]
        j = int(np.clip(np.searchsorted(kp, p, side="right") - 1, 0, len(keys) - 2))
        h = kp[j + 1] - kp[j]
        s = float(np.clip((p - kp[j]) / h, 0, 1))
        if keys[j].get("straight", False):
            # along the quiver's axis: one dimension, eased in at the grab
            a0, a1 = pts[j], pts[j + 1]
            m1 = float(np.linalg.norm(tang[j + 1])) * h / max(float(np.linalg.norm(a1 - a0)), 1e-6)
            u = hermite(0.0, 1.0, 0.0, m1, s)
            grip = a0 + (a1 - a0) * u
        else:
            grip = hermite(pts[j], pts[j + 1], tang[j] * h, tang[j + 1] * h, s)
        se = smooth(s)
        # the arrow's direction and the hand's roll round it
        d0, d1 = dirs[j], dirs[j + 1]
        r0 = grip_frame(st, pts[j], d0, keys[j].get("roll", 0.0), ref_y)[:3, :3]
        r1 = grip_frame(st, pts[j + 1], d1, keys[j + 1].get("roll", 0.0), ref_y)[:3, :3]
        rs_ = slerp_m(r0, r1, se)
        g = keys[j].get("grip", 3.0) + (keys[j + 1].get("grip", 3.0) - keys[j].get("grip", 3.0)) * se
        pole = unit(unit(keys[j]["elbow"]) * (1 - se) + unit(keys[j + 1]["elbow"]) * se)
        # the slot in the chest's frame: the arrow's origin (nock_back from the nock) is at the grip moved along
        slot = np.eye(4)
        slot[:3, :3] = rs_
        dir_now = -rs_[:, 0]
        slot[:3, 3] = grip + dir_now * (st.nock_back - g)
        # the hand bone: the grip point and the arrow in it as the hand holds it (the aim's slot, slid by g)
        hold = st.slot_aim.copy()
        hold[:3, 3] = hold[:3, 3] + (hold[:3, :3] @ np.array([-1.0, 0, 0])) * (3.0 - g)
        hand_c = slot @ np.linalg.inv(hold)
        hand_w = st.chest @ hand_c
        locals_, r, elbow = arm.solve(hand_w, st.chest[:3, :3] @ pole)
        if r["miss"] > report["miss"]:
            report["miss"], report["miss_at"] = round(r["miss"], 1), round(float(p), 2)
        if r["twist"] > report["twist"]:
            report["twist"], report["twist_at"] = round(r["twist"]), round(float(p), 2)
        locals_["BIP01 R CLAVICLE"] = st.local["BIP01 R CLAVICLE"][:3, :3]
        locals_["BIP01 R FINGER0"] = st.local["BIP01 R FINGER0"][:3, :3]
        locals_["BIP01 HEAD"] = st.local["BIP01 HEAD"][:3, :3] @ rot_y(float(pitch[f]))
        zs = np.linalg.inv(hand_c) @ slot
        for col, n in enumerate(NODES):
            if n == "ZS_RIGHTHAND":
                rot[f, col] = rs.mat_to_quat(zs[:3, :3])
                loc[f, col] = zs[:3, 3]
                continue
            rot[f, col] = rs.mat_to_quat(locals_[n])
            loc[f, col] = st.local[n][:3, 3]
        track.append({"p": round(float(p), 3), "grip": grip, "dir": dir_now, "g": g, "elbow": st.c(elbow)})
    anim = ga.Animation(name, "S_BOWAIM", 1, fps, frames, [st.gi[n] for n in NODES], loc, rot)
    return anim, report, track


def composite(st: Stance, anim: ga.Animation) -> ga.Animation:
    """The stance held under the clip: the full-body record the game plays as T_BOWRELOAD."""
    nodes = list(st.anim.node_indices)
    cols = {n: c for c, n in enumerate(nodes)}
    for n in anim.node_indices:
        if n not in cols:
            cols[n] = len(nodes)
            nodes.append(n)
    f = anim.frame_count
    pos = np.zeros((f, len(nodes), 3))
    rot = np.zeros((f, len(nodes), 4))
    for n, c in cols.items():
        m = st.local[st.skel.nodes[n].name]
        pos[:, c] = m[:3, 3]
        rot[:, c] = rs.mat_to_quat(m[:3, :3])
    for c, n in enumerate(anim.node_indices):
        pos[:, cols[n]] = anim.positions[:, c]
        rot[:, cols[n]] = anim.rotations[:, c]
    return ga.Animation(anim.name, anim.next, 1, anim.fps, f, nodes, pos, rot)


def checks(st: Stance, full: ga.Animation, lv: dict, spec: dict) -> dict:
    """Per frame: the hand, the forearm and the arrow out of the head (capsule) and the trunk; the arrow on the
    quiver's axis while its tip is in it; the hand's jump at the joints (|Δx(i) − Δx(i−1)|, cm per frame at the
    clip's fps)."""
    gi = st.gi
    mouth, down = st.mouth(), st.down()
    out = {"head_hand": 99.0, "head_fore": 99.0, "head_arrow": 99.0, "trunk_hand": 99.0, "trunk_fore": 99.0,
           "trunk_arrow": 99.0, "tube_hand": 99.0, "tube_fore": 99.0, "off_axis": 0.0, "jerk": 0.0}
    hands = []
    for f in range(full.frame_count):
        w = ga.world_pose(st.skel, full, f)
        ci = np.linalg.inv(w[gi["BIP01 SPINE2"]])
        c = lambda n: (ci @ w[gi[n]][:, 3])[:3]  # noqa: E731
        h0, h1 = c("BIP01 HEAD"), c("ZS_HELMET")
        hand, fore, slot = c("BIP01 R HAND"), c("BIP01 R FOREARM"), w[gi["ZS_RIGHTHAND"]]
        sl = (ci @ slot[:, 3])[:3]
        ax = unit(ci[:3, :3] @ -slot[:3, 0])
        nock, tip = sl - ax * st.nock_back, sl + ax * st.tip_ahead
        hands.append(hand)
        fm = (fore + hand) / 2
        seg = lambda a, b: [a + (b - a) * i / 8 for i in range(9)]  # noqa: E731
        out["head_hand"] = min(out["head_hand"], seg_dist(hand, h0, h1) - HEAD_R)
        out["head_fore"] = min(out["head_fore"], min(seg_dist(q, h0, h1) for q in seg(fm, hand)) - HEAD_R)
        arrow_in = f >= round(lv["take"] * full.frame_count)
        if arrow_in:
            out["head_arrow"] = min(out["head_arrow"], min(seg_dist(q, h0, h1) for q in seg(nock, tip)) - HEAD_R)
        pv = c("BIP01 PELVIS")

        def trunk(q):
            if q[0] > 15 or q[0] < pv[0]:
                return 99.0
            tt = np.clip((q[0] - pv[0]) / -pv[0], 0, 1)
            y, z = q[1] - pv[1] * (1 - tt), q[2] - pv[2] * (1 - tt)
            r = np.hypot(y, z)
            k = np.hypot(y / 15, z / 11)
            return r * (1 - 1 / k) if k > 1e-6 else -11.0

        def tube(q):
            tq = float(np.dot(q - mouth, down))
            if tq < 0 or tq > 67:
                return 99.0
            return float(np.linalg.norm(q - mouth - down * tq)) - (5.75 - tq / 67)
        out["trunk_hand"] = min(out["trunk_hand"], trunk(hand))
        out["trunk_fore"] = min(out["trunk_fore"], min(trunk(q) for q in seg(fm, hand)))
        out["tube_hand"] = min(out["tube_hand"], tube(hand))
        out["tube_fore"] = min(out["tube_fore"], min(tube(q) for q in seg(fm, hand)))
        if arrow_in:
            out["trunk_arrow"] = min(out["trunk_arrow"], min(trunk(q) for q in seg(nock, tip)))
            if float(np.dot(tip - mouth, down)) > 0:
                for q in seg(nock, tip):
                    tq = float(np.dot(q - mouth, down))
                    if tq >= 0:
                        out["off_axis"] = max(out["off_axis"], float(np.linalg.norm(q - mouth - down * tq)))
    hs = np.array(hands)
    dv = np.diff(hs, axis=0)
    jerk = np.linalg.norm(np.diff(dv, axis=0), axis=1)
    out["jerk"] = float(jerk.max())
    out["jerk_at"] = int(jerk.argmax()) + 1
    out["speed_max"] = float(np.linalg.norm(dv, axis=1).max())
    return {k: round(v, 2) if isinstance(v, float) else v for k, v in out.items()}


def main():
    models, spec_path, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    content = Path(sys.argv[4]) if len(sys.argv) > 4 else None
    spec = json.loads(spec_path.read_text())
    for lv in spec["levels"]:
        level = lv["level"]
        st = Stance(models, level, spec)
        anim, report, _ = build(st, lv, spec, NAME)
        full = composite(st, anim)
        report.update(checks(st, full, lv, spec))
        dst = out / level / f"{level}-{NAME}.MAN.json"
        dst.parent.mkdir(parents=True, exist_ok=True)
        ga.write_animation(dst, full, source=f"reload_quiver over {level}-C_BOW_5")
        print(level, NAME, json.dumps(report))
        if content:
            g2 = next(a for a in json.loads((models / f"{level}.MSB.json").read_text())["data"]["animations"]
                      if a["name"] == "T_BOWRELOAD")
            n = int(lv["frames"])
            # the engine places the arrow a frame after the event's: the event a frame before the grab (K3)
            take = max(0, round(lv["take"] * n) - 1)
            ev = [{**e, "frame": take} for e in g2["events"]]
            sfx = [{**x, "frame": take} for x in g2["sfx"]]
            morph = [{**m, "frame": 0 if i == 0 else min(n - 1, round(lv["ready"] * n))} for i, m in enumerate(g2["morph"])]
            pack = {"model": level, "script": f"{level}.MSB",
                    "animations": [{"file": str(dst), "layer": 1, "next": "S_BOWAIM", "blend_in": 0.0, "blend_out": 0.0,
                                    "events": ev, "sfx": sfx, "morph": morph}]}
            pack_path = out / level / "pack.json"
            pack_path.write_text(json.dumps(pack, indent=1))
            subprocess.run([sys.executable, str(HERE / "content_pack.py"), str(content), str(pack_path)], check=True)


if __name__ == "__main__":
    main()
