#!/usr/bin/env python3
"""How far the quiver must turn round its ring for the right leg to stay out of it (<= 1 cm), per frame:
the place 50 deg / 20.5 cm / 20 / 8 rigid on the pelvis (Quiver::fit), the armour's right thigh, calf and foot
(weight >= 0.5), the walks and runs. For each frame the least turn (any direction, up to 40 deg) that leaves the
leg no deeper than 1 cm, and where along the axis the leg goes in at rest. The other obstacles are left out:
this is the leg's own need.

    uv run --no-project --with numpy --with matplotlib python reports/archery/2026-11-04-quiver-mount/research/need_angle.py
"""
import json
import math
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analysis as A  # noqa: E402

base = Path(sorted(Path("~/.cache/gothic-ai/base").expanduser().glob("*/models"))[0]).parent
body = A.Body(base)
around = body.right * math.cos(math.radians(50)) - body.fwd * math.sin(math.radians(50))
place = {"back": 0.0, "push": around, "tilt_back": 20, "tilt_out": 8, "min_push": 20.5}
clips = ["S_WALKL", "HUMANS_MILITIA-S_WALKL", "HUMANS_RELAXED-S_WALKL", "S_RUNL"]
dirs = [k * 2 * math.pi / 36 for k in range(36)]


def rot_to(deg, phi):
    a = math.radians(deg)
    u = np.array([math.sin(a) * math.cos(phi), -math.cos(a), math.sin(a) * math.sin(phi)])
    # rotation taking (0,-1,0) to u
    f = np.array([0, -1.0, 0])
    k = np.cross(f, u)
    s = np.linalg.norm(k)
    if s < 1e-9:
        return np.eye(4)
    k /= s
    c = f @ u
    K = np.array([[0, -k[2], k[1]], [k[2], 0, -k[0]], [-k[1], k[0], 0]])
    m = np.eye(4)
    m[:3, :3] = np.eye(3) + K * s + K @ K * (1 - c)
    return m


rots = {(d, p): np.linalg.inv(rot_to(d, p)) for d in range(1, 41) for p in dirs}
out = {}
for arm in ["ARMOR_LEATHER_L", "ARMOR_MIL_M", "ARMOR_BDT_H", "ARMOR_PAL_H", "ARMOR_KDF_H"]:
    pos, W, dom = body.mesh(arm)
    leg = (W[:, body.idx["BIP01 R THIGH"]] + W[:, body.idx["BIP01 R CALF"]] + W[:, body.idx["BIP01 R FOOT"]]) >= 0.5
    f, _ = A.quiver_frame(body, pos, dom, place)
    for cn in clips:
        an = A.clip(base, cn)
        worst = {"top": {"need_deg": 0, "rest_cm": 0.0, "depth_cm": 0.0}, "bottom": {"need_deg": 0, "rest_cm": 0.0, "depth_cm": 0.0}}
        if not leg.any():
            out[f"{arm} {cn}"] = worst
            continue
        for t in np.arange(0, an.duration, 1 / 30):
            pose = A.frame_pose(body, an, t)
            p = body.pose_points(arm, pose, leg)
            q = (np.linalg.inv(pose[body.pelvis] @ f) @ np.c_[p, np.ones(len(p))].T).T  # the quiver's rest frame
            c0 = A.tube_clearance(q[:, :3])[0]
            # the contacts near the ring (the top 20 cm) and down the tube apart: turning round the ring moves the
            # top little
            for part, sel in (("top", -q[:, 1] < 20), ("bottom", -q[:, 1] >= 20)):
                if not sel.any() or c0[sel].min() >= -1.0:
                    continue
                qs = q[sel]
                need = 99
                for d in range(1, 41):
                    if any(A.tube_clearance((rots[(d, ph)] @ qs.T).T[:, :3])[0].min() >= -1.0 for ph in dirs):
                        need = d
                        break
                i = int(np.argmin(c0[sel]))
                if need > worst[part]["need_deg"]:
                    worst[part] = {"need_deg": need, "rest_cm": round(float(-c0[sel].min()), 1),
                                   "depth_cm": round(float(-qs[i, 1]), 1), "t": round(float(t), 2)}
        out[f"{arm} {cn}"] = worst
        print(arm, cn, worst, flush=True)
(Path(__file__).resolve().parent / "need_angle.json").write_text(json.dumps(out, indent=1))
