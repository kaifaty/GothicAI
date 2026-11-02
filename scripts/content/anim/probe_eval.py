"""Measure Kimodo probe samples on BIP01 and reject them by hard thresholds per move.

    python3 scripts/content/anim/probe_eval.py <moves.json> <base>/models <probe dir> <pose dir> <out dir>

Retargets every sample (foot IK), writes <out>/<folder>/HUMANS-T_<FOLDER>_<k>.MAN.json at 25 fps and
<out>/report.json, and prints each folder's pass count. Thresholds follow the research note
(reports/Kimodo связки моделей и приёмы.md): numbers only fence off broken takes, the eye on a clip
accepts a move. Times are before time compression. Move kinds are read from the move id prefix.
"""
import json
import re
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gothic_anim as ga  # noqa: E402
import retarget_soma as rs  # noqa: E402

FPS = 30.0  # Kimodo's rate; metrics are taken before resampling


def fails(mid: str, m: dict) -> list[str]:
    f = []
    if m["slide_max"] > 2:
        f.append("slide")
    upright = mid.startswith(("dash", "barge", "charge", "riposte_hand", "riposte_body"))
    if upright and m["tip_below_floor"]:  # in rolls and falls the hand is on the ground: finishing
        f.append("blade_floor")
    if mid.startswith("dash"):
        if m["t80"] > 0.55:
            f.append("slow")
        if m["yaw_max"] > 15:
            f.append("turns")
    if mid.startswith("roll"):
        if m["low_frames"] < 5 or m["flat_frames"] < 5:
            f.append("no_roll")
        if m["t80"] > 1.0:
            f.append("slow")
    if mid.startswith("heavy") and m["low_frames"] < 5:
        f.append("no_roll")
    if mid.startswith("barge") and (m["lead_cm"] < 15 or m["lean_contact"] < 20 or m["v_contact"] < 2.5):
        f.append("no_contact")
    if mid in ("charge_windup", "charge_hold") and m["hand_over_head_end"] < 0:
        f.append("low_apex")
    if mid == "charge_release" and (m["hand_drop_cm"] < 100 or m["hand_vmax"] < 4):
        f.append("weak_swing")
    if m.get("rearm_cm", -99) > -5:  # the sword climbs back to the head on the way to the stance
        f.append("rearm")
    if mid == "charge_hold" and m["pivot_deg"] > 10:  # a hold stands still; a strike's back foot may turn on its ball
        f.append("pivot")
    if mid in ("riposte_hand", "riposte_body") and (m["lean_15"] > 35 or m["hand_ahead_cm"] < 45):
        f.append("no_thrust")  # the attacker doubling over, as in the first probe, leans ~60°
    if mid == "riposte_victim" and m["pelvis_min"] > 40:
        f.append("no_fall")
    return f


class Probe:
    def __init__(self, spec: dict, models: Path, pose_dir: Path):
        self.spec, self.pose_dir = spec, pose_dir
        self.moves = {m["id"]: m for m in spec["moves"]}
        self.skel0 = ga.read_skeleton(models / "HUMANS.MDH.json")
        self.stance = ga.read_animation(models / (spec["stance"] + ".json"))
        self.skel = rs.with_offsets(self.skel0, self.stance)
        self.gi = {n.name: i for i, n in enumerate(self.skel.nodes)}

    def blade(self, w):
        m = w[self.gi["ZS_RIGHTHAND"]]
        return m[:3, 3], m[:3, :3] @ np.array([-1.0, 0, 0])  # the blade lies along the slot's -X

    def pose_world(self, name):
        return ga.world_pose(self.skel, ga.read_animation(self.pose_dir / self.spec["poses"][name]["clip"]), 0)

    def hand_key(self, w, pw):
        """Right hand relative to the pelvis and blade direction against a pose: (cm, degrees).
        The hand is relative to the pelvis, so a body leaning otherwise than the pose counts too."""
        gi = self.gi
        rel = w[gi["BIP01 R HAND"]][:3, 3] - w[gi["BIP01 PELVIS"]][:3, 3]
        prel = pw[gi["BIP01 R HAND"]][:3, 3] - pw[gi["BIP01 PELVIS"]][:3, 3]
        a, b = self.blade(w)[1], self.blade(pw)[1]
        return float(np.linalg.norm(rel - prel)), float(np.degrees(np.arccos(np.clip(a @ b, -1, 1))))

    def measure(self, mid: str, npz: Path) -> dict:
        gi, move = self.gi, self.moves[mid]
        anim = rs.retarget(npz, self.skel0, "T", "", FPS, FPS, None, self.stance)
        W = [ga.world_pose(self.skel, anim, f) for f in range(anim.frame_count)]
        P = np.array([[m[:3, 3] for m in w] for w in W])  # [T, N, 3] cm
        T = len(P)
        root = P[:, gi["BIP01 PELVIS"], [0, 2]]
        dist = np.linalg.norm(root - root[0], axis=1)
        total = max(dist.max() if mid.startswith(("roll", "heavy")) else dist[-1], 1e-6)
        t80 = next((i for i, d in enumerate(dist) if d >= 0.8 * total), T - 1) / FPS
        speed = np.linalg.norm(np.diff(root, axis=0), axis=1) * FPS / 100
        # Planted-foot slide on Kimodo's own contact flags.
        fc = np.load(npz)["foot_contacts"].astype(bool)
        sl = [np.zeros(1)]
        for side, ch in (("L", 0), ("R", 3)):
            for k, node in enumerate((f"BIP01 {side} FOOT", f"BIP01 {side} TOE0")):
                c = fc[:T, ch + k]
                sl.append(np.linalg.norm(np.diff(P[:, gi[node], [0, 2]], axis=0), axis=1)[c[1:] & c[:-1]])
        sl = np.concatenate(sl)
        # How far a planted foot turns about the vertical while it stands (Kimodo turns the feet of a pose it
        # does not like between its keys; the ankle stays, the toe sweeps the ground).
        pivot = [0.0]
        for side, ch in (("L", 0), ("R", 3)):
            v = P[:, gi[f"BIP01 {side} TOE0"]] - P[:, gi[f"BIP01 {side} FOOT"]]
            yaw = np.degrees(np.unwrap(np.arctan2(v[:, 0], v[:, 2])))
            c = fc[:T, ch]
            f = 0
            while f < T:
                g = f
                while g + 1 < T and c[g + 1] == c[f]:
                    g += 1
                if c[f] and g - f >= 2:
                    pivot.append(float(np.abs(yaw[f:g + 1] - yaw[f]).max()))
                f = g + 1
        tips = np.array([b[0] + 90 * b[1] for b in map(self.blade, W)])
        neck = P[:, gi["BIP01 NECK"]] - P[:, gi["BIP01 PELVIS"]]
        lean = np.degrees(np.arccos(np.clip(neck[:, 1] / np.linalg.norm(neck, axis=1), -1, 1)))
        across = P[:, gi["BIP01 R THIGH"]] - P[:, gi["BIP01 L THIGH"]]
        yaw = np.degrees(np.abs(np.arctan2(across[:, 2], across[:, 0])))
        pelvis_y = P[:, gi["BIP01 PELVIS"], 1]
        hand = P[:, gi["BIP01 R HAND"]]
        m = {"frames": T, "t80": round(t80, 2), "vmax": round(float(speed.max()), 1),
             "slide_med": round(float(np.median(sl)), 2), "slide_max": round(float(sl.max()), 2),
             "jerk": round(float(np.linalg.norm(np.diff(P, n=2, axis=0), axis=2).max()), 1),
             "tip_below_floor": int((tips[:, 1] < -2).sum()), "pelvis_min": round(float(pelvis_y.min()), 1),
             "yaw_max": round(float(yaw.max()), 1), "pivot_deg": round(max(pivot), 1)}
        misses = [self.hand_key(W[T - 1 if k["frame"] == "end" else int(k["frame"])], self.pose_world(k["pose"]))
                  for k in move["keys"] if k["type"] == "right-hand"]
        if misses:
            m["hand_miss_cm"] = round(max(x for x, _ in misses), 1)
            m["blade_miss_deg"] = round(max(y for _, y in misses), 1)
        if mid.startswith(("roll", "heavy")):
            low = np.where(pelvis_y < 40)[0]
            m["low_frames"], m["flat_frames"] = len(low), int((lean > 60).sum())
            if len(low):
                m["rise_s"] = round((T - 1 - int(low[-1])) / FPS, 2)
        if mid.startswith("barge"):
            travel = np.array([-move["travel"][0], move["travel"][1]])
            travel /= np.linalg.norm(travel)
            lead = [(P[f, gi["BIP01 L UPPERARM"], [0, 2]] - P[f, gi["BIP01 R UPPERARM"], [0, 2]]) @ travel
                    for f in range(25, min(30, T))]
            m["lead_cm"] = round(float(max(lead)), 1)
            m["lean_contact"] = round(float(lean[25:30].max()), 1)
            m["v_contact"] = round(float(speed[23:28].max()), 1)
        if mid in ("charge_windup", "charge_hold"):
            m["hand_over_head_end"] = round(float(hand[-1, 1] - P[-1, gi["BIP01 HEAD"], 1] - 15), 1)
        if mid == "charge_release":
            # A heavy overhead swing: the hand falls >= 1 m by the end of the follow-through and moves
            # >= 4 m/s at its fastest (real overhead sword swings: about 5-8 m/s).
            hs = np.linalg.norm(np.diff(hand[:19], axis=0), axis=1) * FPS / 100
            m["hand_drop_cm"] = round(float(hand[:3, 1].max() - hand[:19, 1].min()), 1)
            m["hand_vmax"] = round(float(hs.max()), 1)
            # After the last hand key (the follow-through) the hand only goes down and back to the stance.
            follow = max(int(k["frame"]) for k in move["keys"] if k["type"] == "right-hand" and k["frame"] != "end")
            head = P[:, gi["BIP01 HEAD"], 1]
            m["rearm_cm"] = round(float((hand[:, 1] - head)[follow + 3:T - 4].max()), 1)
        if mid in ("riposte_hand", "riposte_body"):
            m["lean_15"] = round(float(lean[15]), 1)
            m["hand_ahead_cm"] = round(float(hand[15, 2] - P[15, gi["BIP01 SPINE2"], 2]), 1)
            m["hand_hold_cm"] = round(float(np.linalg.norm(hand[24] - hand[15])), 1)
        m["fails"] = fails(mid, m)
        return m


def main():
    spec_path, models, probe, pose_dir, out = (Path(a) for a in sys.argv[1:6])
    p = Probe(json.loads(spec_path.read_text()), models, pose_dir)
    report = {}
    for d in sorted(x for x in probe.iterdir() if (x / "out").is_dir()):
        mid = re.sub(r"_c[\d.]+-[\d.]+$", "", d.name)
        rows = []
        for npz in sorted((d / "out").glob("*.npz")):
            k = npz.stem.split("_")[-1]
            m = p.measure(mid, npz)
            a25 = rs.retarget(npz, p.skel0, f"T_{d.name.upper()}_{k}", "", 25.0, FPS, None, p.stance)
            path = out / d.name / f"HUMANS-T_{d.name.upper()}_{k}.MAN.json"
            path.parent.mkdir(parents=True, exist_ok=True)
            ga.write_animation(path, a25, source=f"kimodo:{d.name}/{npz.name}")
            m.update(sample=k, file=str(path))
            rows.append(m)
        report[d.name] = rows
        print(f"== {d.name}: {sum(not r['fails'] for r in rows)}/{len(rows)} pass", flush=True)
        for r in rows:
            extra = {k: v for k, v in r.items() if k not in ("frames", "file", "sample", "fails")}
            print(f"   {r['sample']} {'OK' if not r['fails'] else 'x '} {','.join(r['fails']):20s} {extra}")
    (out / "report.json").write_text(json.dumps(report, indent=1))


if __name__ == "__main__":
    main()
