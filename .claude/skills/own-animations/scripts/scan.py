"""Per-frame numbers of one record: way, speed, hip turn, head over pelvis, the lowest parts and the blade tip; then the
largest local bone turns, the fastest limb ends and the feet near the floor with their slide.

    python3 scan.py <base models> <record.MAN.json> <stance.MAN.json>

The floor is the stance's own lowest toe minus 3 cm; "tip" is the blade tip (−X of ZS_RIGHTHAND, 90 cm). Local turns
overstate a whip a child undoes for its parent: judge a snap by worldturn.py.
"""
import sys
from pathlib import Path

import numpy as np

from common import ga  # noqa: E402

B = Path(sys.argv[1])
skel = ga.read_skeleton(B / "HUMANS.MDH.json")
a, st = ga.read_animation(Path(sys.argv[2])), ga.read_animation(Path(sys.argv[3]))
ix = skel.index
fps, n = a.fps, a.frame_count
W = np.array([ga.world_pose(skel, a, f) for f in range(n)])
P = W[:, :, :3, 3]
w0 = ga.world_pose(skel, st, 0)
toes = [ix(f"BIP01 {s} TOE0") for s in "LR"]
floor = min(w0[i][1, 3] for i in [i for i in toes if i in st.node_indices] or toes) - 3
tip = np.array([(W[f, ix("ZS_RIGHTHAND")] @ np.array([-90.0, 0, 0, 1]))[:3] for f in range(n)])
PARTS = {"head": "BIP01 HEAD", "pelvis": "BIP01 PELVIS", "chest": "BIP01 SPINE2", "L hand": "BIP01 L HAND",
         "R hand": "BIP01 R HAND", "L elbow": "BIP01 L FOREARM", "R elbow": "BIP01 R FOREARM", "L knee": "BIP01 L CALF",
         "R knee": "BIP01 R CALF", "L foot": "BIP01 L FOOT", "R foot": "BIP01 R FOOT", "L toe": "BIP01 L TOE0",
         "R toe": "BIP01 R TOE0"}
pel = P[:, ix("BIP01 PELVIS")]
step = np.linalg.norm(np.diff(pel[:, [0, 2]], axis=0), axis=1)
hip = P[:, ix("BIP01 R THIGH")] - P[:, ix("BIP01 L THIGH")]
yaw = np.degrees(np.arctan2(hip[:, 0], hip[:, 2]))
yaw = np.degrees(np.unwrap(np.radians(yaw)))
yaw -= yaw[0]
headup = P[:, ix("BIP01 HEAD"), 1] - pel[:, 1]
print(f"floor {floor:.1f}; frames {n} at {fps} fps; way {np.linalg.norm(pel[-1, [0, 2]] - pel[0, [0, 2]]):.0f} cm")
print(" f    t | pel h | step cm/f  m/s | hips turn | head-pelvis | lowest parts (cm over floor)")
for f in range(n):
    low = sorted(((P[f, ix(j)][1] - floor, k) for k, j in PARTS.items()))[:3]
    s = step[f - 1] if f else 0.0
    print(f"{f:2d} {f / fps:4.2f} | {pel[f, 1] - floor:5.0f} | {s:5.1f} {s * fps / 100:5.2f} | {yaw[f]:7.0f}° | {headup[f]:6.0f} | "
          + ", ".join(f"{k} {h:.0f}" for h, k in low) + f" | tip {tip[f, 1] - floor:.0f}")
# pops: local rotation change per frame
q = a.rotations
print("\nlargest local turns per frame (deg), > 25:")
rows = []
for k, node in enumerate(a.node_indices):
    d = np.abs((q[1:, k] * q[:-1, k]).sum(-1)).clip(0, 1)
    ang = np.degrees(2 * np.arccos(d))
    for f in np.where(ang > 25)[0]:
        rows.append((ang[f], skel.nodes[node].name, f, f + 1))
for ang, name, f0, f1 in sorted(rows, reverse=True)[:25]:
    print(f"  {name:20s} {f0:2d}->{f1:2d} {ang:5.0f}")
# limb end speeds
print("\nfastest ends cm/frame:")
for k in ("head", "L hand", "R hand", "L foot", "R foot"):
    v = np.linalg.norm(np.diff(P[:, ix(PARTS[k])], axis=0), axis=1)
    f = int(np.argmax(v))
    acc = np.linalg.norm(np.diff(P[:, ix(PARTS[k])], 2, axis=0), axis=1)
    g = int(np.argmax(acc))
    print(f"  {k:7s} max {v[f]:5.1f} at {f}->{f + 1};  max change of speed {acc[g]:5.1f} cm/f² at {g + 1}")
v = np.linalg.norm(np.diff(tip, axis=0), axis=1)
print(f"  tip     max {v.max():5.1f} at {int(np.argmax(v))}->{int(np.argmax(v)) + 1}")
# planted feet: low and their slide
print("\nfeet near the floor (<8 cm) and their slide cm/frame:")
for s in "LR":
    for part in ("FOOT", "TOE0"):
        p = P[:, ix(f"BIP01 {s} {part}")]
        h = p[:, 1] - floor - (w0[ix(f"BIP01 {s} {part}")][1, 3] - floor if part == "FOOT" else 0)
        sl = np.r_[0, np.linalg.norm(np.diff(p[:, [0, 2]], axis=0), axis=1)]
        frames = [f"{f}:{sl[f]:.0f}" for f in range(n) if h[f] < 8]
        print(f"  {s} {part}: " + " ".join(frames))
