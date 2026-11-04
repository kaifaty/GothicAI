#!/usr/bin/env python3
"""The place behind the right hip again, by the armours' meshes (not the naked body): Leather_L, MIL_M, BDT_H, PAL_H,
KDF_H in the walks, runs and sneaks, rigid on the pelvis as Quiver::fit places it (each armour pushes the ring out
by its own mesh standing). For each place, the worst clearance (cm, minus: inside the tube):
- leg: the right thigh and calf of the armour mesh (the quiver gives way to them by up to 5 cm in the engine);
- arm: the right hand and forearm (KDF_H: its wide sleeves, cloth, allowed 5 cm);
- bow, longsword: the bow (ITRW_BOW_L_01 by its bounds) and a two-hander on the back, only while put away;
- reach: the right shoulder to the mouth in the shooting pose against the arm's length.
Writes sweep_armour.json: the places that keep the arm, the bow and the two-hander, the least leg contact first.

    uv run --no-project --with numpy --with matplotlib python reports/archery/2026-11-04-quiver-mount/research/sweep_armour.py
"""
import itertools
import json
import math
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analysis as A  # noqa: E402

ARMOURS = ["ARMOR_LEATHER_L", "ARMOR_MIL_M", "ARMOR_BDT_H", "ARMOR_PAL_H", "ARMOR_KDF_H"]
CLIPS = ["S_WALKL", "HUMANS_MILITIA-S_WALKL", "HUMANS_RELAXED-S_WALKL", "HUMANS_TIRED-S_WALKL", "HUMANS_ARROGANCE-S_WALKL",
         "S_RUNL", "S_SNEAKL", "S_BOWWALKL", "S_BOWRUNL", "S_BOWSNEAKL"]

base = Path(sorted(Path("~/.cache/gothic-ai/base").expanduser().glob("*/models"))[0]).parent
body = A.Body(base)
xs = np.linspace(-56.7, 56.7, 40)
bow = np.c_[xs, np.zeros_like(xs), 3.7 - 31 * (xs / 56.7) ** 2]
longsword = A.glb_points(base / "meshes/ITMW_025_2H_SWORD_OLD_01.glb")[::3]

# every frame's points in that frame's pelvis frame
pts = {}
for arm in ARMOURS:
    _, W, _ = body.mesh(arm)
    leg = (W[:, body.idx["BIP01 R THIGH"]] + W[:, body.idx["BIP01 R CALF"]]) >= 0.5  # BDT_H: the pelvis leads
    hand = body.group(arm, ("R HAND", "R FINGER", "R FOREARM"))
    acc = {"leg": [], "arm": []}
    for cn in CLIPS:
        an = A.clip(base, cn)
        for t in np.arange(0, an.duration, 1 / 30):
            pose = A.frame_pose(body, an, t)
            to = np.linalg.inv(pose[body.pelvis])
            for key, sel in (("leg", leg), ("arm", hand)):
                p = body.pose_points(arm, pose, sel)
                acc[key].append((to @ np.c_[p, np.ones(len(p))].T).T[:, :3])
    pts[arm] = {k: np.concatenate(v) for k, v in acc.items()}
carried = {"bow": [], "longsword": []}
for cn in CLIPS:
    if "BOW" in cn:
        continue  # the bow is in the hand
    an = A.clip(base, cn)
    for t in np.arange(0, an.duration, 1 / 30):
        pose = A.frame_pose(body, an, t)
        to = np.linalg.inv(pose[body.pelvis])
        for key, slot, p in (("bow", "ZS_BOW", bow), ("longsword", "ZS_LONGSWORD", longsword)):
            w = (pose[body.idx[slot]] @ np.c_[p, np.ones(len(p))].T).T[:, :3]
            carried[key].append((to @ np.c_[w, np.ones(len(w))].T).T[:, :3])
carried = {k: np.concatenate(v) for k, v in carried.items()}
aim = A.clip(base, "S_BOWSHOOT")
apose = A.frame_pose(body, aim, 0.0)
shoulder = A.tr(apose[body.idx["BIP01 R UPPERARM"]])
arm_len = 63.7


def around(d):
    return body.right * math.cos(math.radians(d)) - body.fwd * math.sin(math.radians(d))


def clearance(f, p):
    if len(p) == 0:
        return 9e9
    inv = np.linalg.inv(f)
    return float(A.tube_clearance((inv @ np.c_[p, np.ones(len(p))].T).T[:, :3])[0].min())


bind = {arm: body.mesh(arm) for arm in ARMOURS}
rows = []
for ang, push, tb, to_ in itertools.product((50, 55, 60, 65, 70), (18, 20.5, 23, 25.5, 28), (15, 20, 25), (8, 13, 18)):
    place = {"back": 0.0, "push": around(ang), "tilt_back": tb, "tilt_out": to_, "min_push": push}
    r = {"around": ang, "push": push, "tilt_back": tb, "tilt_out": to_, "leg": {}, "arm": {}, "bow": 9e9, "longsword": 9e9,
         "out": {}, "reach": 0.0}
    for arm in ARMOURS:
        pos, _, dom = bind[arm]
        f, s = A.quiver_frame(body, pos, dom, place)
        r["out"][arm] = round(float(s), 1)
        r["leg"][arm] = round(clearance(f, pts[arm]["leg"]), 1)
        r["arm"][arm] = round(clearance(f, pts[arm]["arm"]), 1)
        r["bow"] = min(r["bow"], clearance(f, carried["bow"]))
        r["longsword"] = min(r["longsword"], clearance(f, carried["longsword"]))
        r["reach"] = max(r["reach"], float(np.linalg.norm(A.tr(apose[body.pelvis] @ f) - shoulder)))
    r["bow"], r["longsword"], r["reach"] = round(r["bow"], 1), round(r["longsword"], 1), round(r["reach"], 1)
    arm_ok = all(v >= (-5.0 if a == "ARMOR_KDF_H" else -1.0) for a, v in r["arm"].items())
    r["ok"] = arm_ok and r["bow"] >= 0 and r["longsword"] >= 0 and r["reach"] <= arm_len
    r["leg_worst"] = min(r["leg"].values())
    rows.append(r)
rows.sort(key=lambda r: (not r["ok"], -r["leg_worst"]))
out = Path(__file__).resolve().parent / "sweep_armour.json"
out.write_text(json.dumps(rows, indent=0))
print("places", len(rows), "ok", sum(r["ok"] for r in rows))
for r in rows[:8]:
    print(json.dumps(r))
