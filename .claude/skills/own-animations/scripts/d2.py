"""The largest second differences of joints (a jerk in cm a frame², ground travel of the root taken out), four per record.

    python3 d2.py <base models> <record.MAN.json>...
"""
import sys
from pathlib import Path

import numpy as np

from common import ga, positions, skeleton

skel = skeleton(Path(sys.argv[1]))
for r in sys.argv[2:]:
    a = ga.read_animation(Path(r))
    P = positions(skel, a)
    P = P - P[:, [skel.index("BIP01")]] * np.array([1, 0, 1])
    d2 = np.linalg.norm(P[2:] - 2 * P[1:-1] + P[:-2], axis=2)
    top = sorted(((d2[:, k].max(), skel.nodes[k].name, int(np.argmax(d2[:, k])) + 1) for k in range(d2.shape[1])), reverse=True)[:4]
    print(Path(r).parent.name, Path(r).name, " ".join(f"{n.removeprefix('BIP01 ')} {v:.0f}@{f}" for v, n, f in top))
