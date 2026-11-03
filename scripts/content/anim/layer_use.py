"""Drinking and eating with the left hand over a fight stance: our own upper-layer clips from key targets.

    python3 scripts/content/anim/layer_use.py <base>/models <uses.json> <out dir> [content dir]

Each clip moves only the left arm (clavicle to finger) and the head, on a layer above the fight turns, so
the stance or walk below keeps the spine, the legs and the weapon hand: the sword cannot move, and the
bottle stays at the mouth whatever the spine below does, because the arm hangs on the neck and the head
on the neck as well (the neck itself stays the stance's: both clavicles hang on it).

The hand is led by keys in the chest frame (BIP01 SPINE2): `stance` (where the stance holds it), a point
of our own (`chest`: up, left, forward in cm) or `mouth` — the ZS_LEFTHAND transform in the head's frame
measured once from a frame of Gothic's own use clip (`measure`), which only fixes where the item meets the
mouth. Between keys the position follows a monotone cubic and the rotation an eased slerp; a two-bone IK
lays out the arm with the elbow towards the key's `elbow` direction. The head pitches back (> 0) by its
own keys. Writes HUMANS-like records <LEVEL>-T_<PREFIX><USE>.MAN.json with the layer of the spec, a
full-body preview (stance + clip) for blender/gothic_rig.py, and prints the checks; with a content dir it
packs the clips with their events (ITEM_INSERT into ZS_LEFTHAND, the sound, ITEM_DESTROY).
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
from finish import monotone_cubic  # noqa: E402

ARM = ("BIP01 L CLAVICLE", "BIP01 L UPPERARM", "BIP01 L FOREARM", "BIP01 L HAND", "BIP01 L FINGER0")
NODES = ARM + ("BIP01 HEAD",)
EVENT = {"frame": 0, "slot": "", "slot2": "", "item": "", "frames": [], "fight_mode": 6, "attached": False}


def unit(v) -> np.ndarray:
    v = np.asarray(v, float)
    return v / np.linalg.norm(v)


def triad(u1, u2, w1, w2) -> np.ndarray:
    """Rotation taking local u1 exactly onto world w1 and local u2 as close as it goes onto w2."""
    def basis(a, b):
        a = unit(a)
        c = unit(np.cross(a, b))
        return np.column_stack([a, np.cross(c, a), c])
    return basis(w1, w2) @ basis(u1, u2).T


def rot_y(deg: float) -> np.ndarray:
    c, s = np.cos(np.radians(deg)), np.sin(np.radians(deg))
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])


def smooth(x: np.ndarray) -> np.ndarray:
    return x * x * (3 - 2 * x)


class Stance:
    """The stance's first frame: world transforms, local offsets and the frames the clip hangs on."""

    def __init__(self, models: Path, level: str, clip: str, finger: np.ndarray):
        skel0 = ga.read_skeleton(models / "HUMANS.MDH.json")
        self.anim = ga.read_animation(models / f"{level}-{clip}.MAN.json")
        self.skel = rs.with_offsets(skel0, self.anim)
        self.gi = {n.name: i for i, n in enumerate(self.skel.nodes)}
        self.world = ga.world_pose(self.skel, self.anim, 0)
        self.local = {n.name: self.skel.nodes[i].rest.copy() for i, n in enumerate(self.skel.nodes)}
        for col, node in enumerate(self.anim.node_indices):
            self.local[self.skel.nodes[node].name] = ga.sample_matrix(self.anim.positions[0, col], self.anim.rotations[0, col])
        self.local["BIP01 L FINGER0"][:3, :3] = finger
        self.zs = self.skel.nodes[self.gi["ZS_LEFTHAND"]].rest

    def W(self, name: str) -> np.ndarray:
        return self.world[self.gi[name]]

    def head(self, pitch: float) -> np.ndarray:
        """World transform of the head pitched back by `pitch` degrees about its own left axis (local Y)."""
        local = self.local["BIP01 HEAD"].copy()
        local[:3, :3] = local[:3, :3] @ rot_y(pitch)
        return self.W("BIP01 NECK") @ local


def measure(models: Path, clip: str, frame: int) -> np.ndarray:
    """ZS_LEFTHAND in the head's frame at a frame of a Gothic clip."""
    skel0 = ga.read_skeleton(models / "HUMANS.MDH.json")
    anim = ga.read_animation(models / f"HUMANS-{clip}.MAN.json")
    skel = rs.with_offsets(skel0, anim)
    gi = {n.name: i for i, n in enumerate(skel.nodes)}
    w = ga.world_pose(skel, anim, frame)
    return np.linalg.inv(w[gi["BIP01 HEAD"]]) @ w[gi["ZS_LEFTHAND"]]


def key_target(st: Stance, key: dict, pitch: float, mouth: np.ndarray) -> np.ndarray:
    """The ZS_LEFTHAND transform a key asks for, in the chest frame."""
    chest_inv = np.linalg.inv(st.W("BIP01 SPINE2"))
    stance = chest_inv @ st.W("ZS_LEFTHAND")
    at_mouth = chest_inv @ st.head(pitch) @ mouth
    hand = key["hand"]
    if hand == "stance":
        return stance
    if hand == "mouth":
        return at_mouth
    m = np.eye(4)
    w = key.get("turn", 0.5)  # the item's turn from the stance's (0) to the mouth's (1)
    m[:3, :3] = ga.quat_matrix(rs.slerp(rs.mat_to_quat(stance[:3, :3]), rs.mat_to_quat(at_mouth[:3, :3]), w))
    # SPINE2's local axes: X up the spine, Y to the left, Z forward (as the head's)
    m[:3, 3] = np.asarray(hand["chest"], float)
    return m


def elbow_dir(st: Stance, key: dict) -> np.ndarray:
    if "elbow" in key:
        return np.asarray(key["elbow"], float)
    chest_inv = np.linalg.inv(st.W("BIP01 SPINE2"))
    e = st.W("BIP01 L FOREARM")[:3, 3] - (st.W("BIP01 L UPPERARM")[:3, 3] + st.W("BIP01 L HAND")[:3, 3]) / 2
    return chest_inv[:3, :3] @ e


def solve(st: Stance, zs_world: np.ndarray, pole: np.ndarray, wrist: float) -> tuple[dict, dict, np.ndarray]:
    """The left arm reaching a ZS_LEFTHAND transform: local rotations of the upper arm, forearm and hand,
    the misses, and the elbow. The elbow goes where the wrist needs the least twist (`wrist` 1) or towards
    the key's direction (0); half the wrist's twist from its bind pose goes into the forearm."""
    neck = st.W("BIP01 NECK")
    clav = neck @ st.local["BIP01 L CLAVICLE"]
    up = clav @ st.local["BIP01 L UPPERARM"]
    a = np.linalg.norm(st.local["BIP01 L FOREARM"][:3, 3])
    b = np.linalg.norm(st.local["BIP01 L HAND"][:3, 3])
    bind_hand = st.skel.nodes[st.gi["BIP01 L HAND"]].rest[:3, :3]
    hand_world = zs_world @ np.linalg.inv(st.zs)
    sh, end = up[:3, 3], hand_world[:3, 3]
    to = end - sh
    dist = float(np.linalg.norm(to))
    d = float(np.clip(dist, abs(a - b) + 1e-3, a + b - 1e-3))
    axis = to / dist
    along = (a * a - b * b + d * d) / (2 * d)
    height = np.sqrt(max(a * a - along * along, 0.0))
    end = sh + axis * d
    w_hand = hand_world[:3, :3]
    key_p = unit(pole - axis * np.dot(pole, axis))
    p = key_p
    for _ in range(3):
        e = sh + axis * along + p * height
        fx = unit(end - e)
        hb = (w_hand @ bind_hand.T)[:, 1]  # the hand's Y as it lies on the forearm in the bind pose
        p_wrist = unit(np.cross(axis, unit(hb - fx * np.dot(hb, fx))))
        p = unit(wrist * p_wrist + (1 - wrist) * key_p)
    e = sh + axis * along + p * height
    hinge = unit(np.cross(p, axis))  # the bend plane's normal, also when the arm is straight
    y = np.array([0.0, 1.0, 0.0])
    w_up = triad(unit(st.local["BIP01 L FOREARM"][:3, 3]), y, e - sh, hinge)
    w_fore = triad(unit(st.local["BIP01 L HAND"][:3, 3]), y, end - e, hinge)
    rel = bind_hand.T @ (w_fore.T @ w_hand)  # the wrist's turn from its bind pose (90° about X)
    twist = float(np.degrees(np.arctan2(rel[2, 1] - rel[1, 2], rel[1, 1] + rel[2, 2])))
    c, sn = np.cos(np.radians(0.5 * twist)), np.sin(np.radians(0.5 * twist))
    w_fore = w_fore @ np.array([[1, 0, 0], [0, c, -sn], [0, sn, c]])
    locals_ = {"BIP01 L UPPERARM": clav[:3, :3].T @ w_up, "BIP01 L FOREARM": w_up.T @ w_fore,
               "BIP01 L HAND": w_fore.T @ w_hand}
    return locals_, {"miss": dist - d, "twist": abs(twist)}, e


def spin(st: Stance, use: dict, keys: list, hk: np.ndarray, chest: np.ndarray, mouth: np.ndarray, wrist: float) -> np.ndarray:
    """The item may turn about its own long axis (the slot's Y: a bottle from its foot to its neck)
    without changing the look; the turn that leaves the wrist least twisted at the mouth keys, with the
    elbow out of the torso, is the one the clip holds."""
    torso = [st.W(n)[:3, 3] for n in ("BIP01 SPINE", "BIP01 SPINE2", "BIP01 NECK")]

    def seg_dist(p, a, b):
        ab = b - a
        return float(np.linalg.norm(p - (a + ab * np.clip(np.dot(p - a, ab) / np.dot(ab, ab), 0, 1))))
    best, best_cost = mouth, None
    for deg in range(-180, 180, 5):
        c, sn = np.cos(np.radians(deg)), np.sin(np.radians(deg))
        m = mouth.copy()
        m[:3, :3] = mouth[:3, :3] @ np.array([[c, 0, sn], [0, 1, 0], [-sn, 0, c]])
        cost = 0.0
        for k in keys:
            if k["hand"] != "mouth":
                continue
            zs = chest @ key_target(st, k, float(np.interp(k["t"], hk[:, 0], hk[:, 1])), m)
            _, r, e = solve(st, zs, chest[:3, :3] @ unit(elbow_dir(st, k)), float(k.get("wrist_pole", wrist)))
            inside = max(0.0, 16.0 - min(seg_dist(e, torso[i], torso[i + 1]) for i in range(2)))
            cost = max(cost, r["twist"] + 10.0 * inside)
        if best_cost is None or cost < best_cost:
            best, best_cost = m, cost
    return best


def build(st: Stance, use: dict, fps: float, layer: int, mouth: np.ndarray, name: str) -> tuple[ga.Animation, dict]:
    """Each key's arm by IK once; between keys the joints turn by an eased slerp, so every key is met
    exactly and no frame can flip the arm."""
    frames = int(round(use["seconds"] * fps))
    t = np.arange(frames) / fps
    hk = np.array(use["head"], float)
    pitch = monotone_cubic(hk[:, 0], hk[:, 1], t)
    keys = use["keys"]
    kt = np.array([k["t"] for k in keys])
    chest = st.W("BIP01 SPINE2")
    wrist = float(use.get("wrist_pole", 0.5))
    arm = ("BIP01 L UPPERARM", "BIP01 L FOREARM", "BIP01 L HAND")
    report = {"miss": 0.0, "twist": 0.0}
    mouth = spin(st, use, keys, hk, chest, mouth, wrist)
    solved = []
    for k in keys:
        if k["hand"] == "stance":
            solved.append({n: st.local[n][:3, :3] for n in arm})
            continue
        zs = chest @ key_target(st, k, float(np.interp(k["t"], hk[:, 0], hk[:, 1])), mouth)
        locals_, r, _ = solve(st, zs, chest[:3, :3] @ unit(elbow_dir(st, k)), float(k.get("wrist_pole", wrist)))
        solved.append(locals_)
        if r["miss"] > report["miss"]:
            report["miss"], report["miss_at"] = round(r["miss"], 1), k["t"]
        if r["twist"] > report["twist"]:
            report["twist"], report["twist_at"] = round(r["twist"]), k["t"]
    quats = [{n: rs.mat_to_quat(m[n]) for n in arm} for m in solved]
    rot = np.zeros((frames, len(NODES), 4))
    loc = np.zeros((frames, len(NODES), 3))
    for f in range(frames):
        j = int(np.clip(np.searchsorted(kt, t[f], side="right") - 1, 0, len(keys) - 2))
        s = float(smooth(np.clip((t[f] - kt[j]) / (kt[j + 1] - kt[j]), 0, 1)))
        locals_ = {n: ga.quat_matrix(rs.slerp(quats[j][n], quats[j + 1][n], s)) for n in arm}
        locals_["BIP01 L CLAVICLE"] = st.local["BIP01 L CLAVICLE"][:3, :3]
        locals_["BIP01 L FINGER0"] = st.local["BIP01 L FINGER0"][:3, :3]
        locals_["BIP01 HEAD"] = st.local["BIP01 HEAD"][:3, :3] @ rot_y(float(pitch[f]))
        for col, n in enumerate(NODES):
            rot[f, col] = rs.mat_to_quat(locals_[n])
            loc[f, col] = st.local[n][:3, 3]
    report["speed"] = 0.0
    for col, n in enumerate(NODES):
        q = rot[:, col]
        dots = np.abs(np.sum(q[1:] * q[:-1], axis=1)).clip(0, 1)
        sp = np.degrees(2 * np.arccos(dots)) * fps
        if sp.max() > report["speed"]:
            report["speed"], report["speed_at"] = round(float(sp.max())), f"{n.removeprefix('BIP01 ')} {t[int(sp.argmax()) + 1]:.2f}"
    anim = ga.Animation(name, "", layer, fps, frames, [st.gi[n] for n in NODES], loc, rot)
    report.update(checks(st, anim, use, mouth, fps))
    return anim, report


def checks(st: Stance, anim: ga.Animation, use: dict, mouth: np.ndarray, fps: float) -> dict:
    """At the gulp or bite the item against the measured mouth transform; the elbow and hand out of the
    torso; the hand out of the head except at the mouth."""
    bite = int(round(use["sfx"][0] * fps))
    full = composite(st, anim)
    worlds = [ga.world_pose(st.skel, full, f) for f in range(anim.frame_count)]
    w = ga.world_pose(st.skel, full, bite)
    rel = np.linalg.inv(w[st.gi["BIP01 HEAD"]]) @ w[st.gi["ZS_LEFTHAND"]]
    dang = np.degrees(np.linalg.norm(rs.axis_angle(rel[:3, :3] @ mouth[:3, :3].T)))
    torso = [st.W(n)[:3, 3] for n in ("BIP01 SPINE", "BIP01 SPINE2", "BIP01 NECK")]

    def seg_dist(p, a, b):
        ab = b - a
        return float(np.linalg.norm(p - (a + ab * np.clip(np.dot(p - a, ab) / np.dot(ab, ab), 0, 1))))
    elbow = min(min(seg_dist(w[st.gi["BIP01 L FOREARM"]][:3, 3], torso[i], torso[i + 1]) for i in range(2)) for w in worlds)
    sword = max(float(np.linalg.norm(w[st.gi["ZS_RIGHTHAND"]][:3, 3] - st.W("ZS_RIGHTHAND")[:3, 3])) for w in worlds)
    blade = 90.0  # the blade along ZS_RIGHTHAND's -X, as the engine binds weapon meshes

    def seg_seg(p1, q1, p2, q2):
        ts = np.linspace(0, 1, 21)
        a = p1 + np.outer(ts, q1 - p1)
        b = p2 + np.outer(ts, q2 - p2)
        return float(np.min(np.linalg.norm(a[:, None] - b[None], axis=2)))
    clear = min(
        seg_seg(w[st.gi["BIP01 L FOREARM"]][:3, 3], w[st.gi["ZS_LEFTHAND"]][:3, 3] + 20 * w[st.gi["ZS_LEFTHAND"]][:3, 1],
                w[st.gi["ZS_RIGHTHAND"]][:3, 3], w[st.gi["ZS_RIGHTHAND"]][:3, 3] - blade * w[st.gi["ZS_RIGHTHAND"]][:3, 0])
        for w in worlds[2:-2])  # the first and last frames are the stance's own (2H holds the grip)
    return {"blade_clear_cm": round(clear, 1),
            "at_mouth_cm": round(float(np.linalg.norm(rel[:3, 3] - mouth[:3, 3])), 1), "at_mouth_deg": round(float(dang), 1),
            "elbow_from_spine_cm": round(elbow, 1), "sword_moves_cm": round(sword, 2)}


def composite(st: Stance, anim: ga.Animation) -> ga.Animation:
    """The stance's first frame held under the clip: the full-body record a preview or a check needs."""
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
    return ga.Animation(anim.name + "_PREVIEW", "", 1, anim.fps, f, nodes, pos, rot)


def main():
    models, spec_path, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    content = Path(sys.argv[4]) if len(sys.argv) > 4 else None
    spec = json.loads(spec_path.read_text())
    fps, layer = float(spec["fps"]), int(spec["layer"])
    fist = Stance(models, "HUMANS", "S_FISTRUN", np.eye(3))
    finger = fist.local["BIP01 L FINGER0"][:3, :3]  # the fist: the fingers close round the bottle or the bread
    packs: dict[str, list] = {}
    for s in spec["stances"]:
        st = Stance(models, s["level"], s["stance"], finger)
        for use in spec["uses"]:
            mouth = measure(models, use["measure"]["clip"], use["measure"]["frame"])
            name = f"T_{s['prefix']}{use['name']}"
            anim, report = build(st, use, fps, layer, mouth, name)
            dst = out / s["level"] / f"{s['level']}-{name}.MAN.json"
            dst.parent.mkdir(parents=True, exist_ok=True)
            ga.write_animation(dst, anim, source=f"layer_use {use['name']} over {s['level']}-{s['stance']}")
            ga.write_animation(dst.with_name(dst.name.replace(".MAN", "_PREVIEW.MAN")), composite(st, anim), source="preview")
            print(s["level"], name, json.dumps(report))
            ev = [{**EVENT, "type": 2, "slot": "ZS_LEFTHAND", "frame": int(round(use["insert"] * fps))},
                  {**EVENT, "type": 4, "frame": int(round(use["destroy"] * fps))}]
            sfx = [{"frame": int(round(use["sfx"][0] * fps)), "name": use["sfx"][1], "range": 1000.0, "empty_slot": False}]
            packs.setdefault(s["level"], []).append({"file": str(dst), "layer": layer, "blend_in": 0.1, "blend_out": 0.1,
                                                     "events": ev, "sfx": sfx})
    if content:
        for level, anims in packs.items():
            pack_path = out / level / "pack.json"
            pack_path.write_text(json.dumps({"model": level, "script": f"{level}.MSB", "animations": anims}, indent=1))
            subprocess.run([sys.executable, str(HERE / "content_pack.py"), str(content), str(pack_path)], check=True)


if __name__ == "__main__":
    main()
