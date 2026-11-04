"""Per-frame feet: height of the foot, toe and knee over the floor and the ground travel of foot and toe a frame.
Read it to set the frames of a `step` (where the foot leaves and lands) and to see a toe dragged on its tip.

    python3 feet.py <base models> <stance.MAN.json> <record.MAN.json>... [L] [R]
"""
import sys
from pathlib import Path

import numpy as np

from common import floor_of, ga, positions, skeleton

skel = skeleton(Path(sys.argv[1]))
ix = skel.index
floor = floor_of(skel, ga.read_animation(Path(sys.argv[2])))
recs = [Path(x) for x in sys.argv[3:] if x.endswith(".json")]
sides = [x for x in sys.argv[3:] if not x.endswith(".json")] or ["L", "R"]
for r in recs:
    a = ga.read_animation(r)
    P = positions(skel, a)
    print("==", r.parent.name, r.name)
    for sd in sides:
        print(f" {sd}: f  foot h/d   toe h/d   knee h")
        F, T, K = P[:, ix(f"BIP01 {sd} FOOT")], P[:, ix(f"BIP01 {sd} TOE0")], P[:, ix(f"BIP01 {sd} CALF")]
        for f in range(a.frame_count):
            d = lambda X: np.linalg.norm(X[f, [0, 2]] - X[f - 1, [0, 2]]) if f else 0  # noqa: E731
            print(f"   {f:2d} {F[f, 1] - floor:5.0f} {d(F):4.0f}  {T[f, 1] - floor:5.0f} {d(T):4.0f}  {K[f, 1] - floor:5.0f}")
