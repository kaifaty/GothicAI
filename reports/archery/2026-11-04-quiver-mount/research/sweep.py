#!/usr/bin/env python3
"""A grid of places behind the right hip (research.md): for each, the worst clearance of the right hand and
forearm, of the right leg and of the bow on the back over the walks, runs and sneaks of analysis.py, rigid.

    uv run --no-project --with numpy --with matplotlib python reports/archery/2026-11-04-quiver-mount/research/sweep.py
"""
import itertools
import json
import math
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analysis as A  # noqa: E402

base = Path(sorted(Path("~/.cache/gothic-ai/base").expanduser().glob("*/models"))[0]).parent
body = A.Body(base)
pos, _, dom = body.mesh("ARMOR_LEATHER_L")
rh = body.group("HUM_BODY_NAKED0", ("R HAND", "R FINGER", "R FOREARM"))
rl = body.group("HUM_BODY_NAKED0", ("R THIGH", "R CALF"))
xs = np.linspace(-56.7, 56.7, 40)
bow = np.c_[xs, np.zeros_like(xs), 3.7 - 31 * (xs / 56.7) ** 2]   # ITRW_BOW_L_01 by its bounds
clips = A.CLIPS + A.OVERLAY_WALKS
# every frame's points in the pelvis frame of that frame, carried into the bind pose's pelvis frame
pts = {"hand": [], "leg": [], "bow": []}
for cn in clips:
    an = A.clip(base, cn)
    for t in np.arange(0, an.duration, 1 / 30):
        pose = A.frame_pose(body, an, t)
        to = np.linalg.inv(pose[body.pelvis])
        parts = [("hand", body.pose_points("HUM_BODY_NAKED0", pose, rh)),
                 ("leg", body.pose_points("HUM_BODY_NAKED0", pose, rl))]
        if "BOW" not in cn:  # the bow hangs on the back only while put away
            parts.append(("bow", (pose[body.idx["ZS_BOW"]] @ np.c_[bow, np.ones(len(bow))].T).T[:, :3]))
        for key, p in parts:
            pts[key].append((to @ np.c_[p, np.ones(len(p))].T).T[:, :3])
pts = {k: np.concatenate(v) for k, v in pts.items()}


def around(d):
    return body.right * math.cos(math.radians(d)) - body.fwd * math.sin(math.radians(d))


rows = []
for ang, push, tb, to_ in itertools.product((45, 55, 60, 65, 75), (17, 20, 23), (5, 12, 20), (4, 10, 16)):
    f, s = A.quiver_frame(body, pos, dom, {"back": 0.0, "push": around(ang), "tilt_back": tb, "tilt_out": to_, "min_push": push})
    inv = np.linalg.inv(f)
    c = {k: float(A.tube_clearance((inv @ np.c_[v, np.ones(len(v))].T).T[:, :3])[0].min()) for k, v in pts.items()}
    rows.append({"around": ang, "push": round(float(s), 1), "tilt_back": tb, "tilt_out": to_, **{k: round(v, 1) for k, v in c.items()},
                 "worst": round(min(c.values()), 1)})
rows.sort(key=lambda r: -r["worst"])
out = Path(__file__).resolve().parent / "sweep.json"
out.write_text(json.dumps(rows, indent=0))
for r in rows[:12]:
    print(r)
