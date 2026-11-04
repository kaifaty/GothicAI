"""Phase timeline of a source move, every 2 frames: time, share of the path, speed, pelvis and head height, inverted.
Pick the --warp knots of finish_levels.py from it: the end of the squat, the flip, the landing, the rise.

    python3 timeline.py <base models> <record.MAN.json>
"""
import sys
from pathlib import Path

import numpy as np

from common import ga, positions, skeleton

skel = skeleton(Path(sys.argv[1]))
ix = skel.index
a = ga.read_animation(Path(sys.argv[2]))
n = a.frame_count
P = positions(skel, a)
tr = ga.root_travel(a)[:, [0, 2]]
dd = np.linalg.norm(tr - tr[0], axis=1)
pel, head = P[:, ix("BIP01 PELVIS")], P[:, ix("BIP01 HEAD")]
g = P[:, :, 1].min()
for f in range(0, n, 2):
    v = np.linalg.norm(pel[min(f + 1, n - 1), [0, 2]] - pel[max(f - 1, 0), [0, 2]]) / (2 / a.fps) / 100
    print(f"{f / a.fps:5.2f}s share {dd[f] / dd[-1]:4.2f} speed {v:4.1f} pel {pel[f, 1] - g:4.0f} head {head[f, 1] - g:4.0f} "
          f"{'INV' if pel[f, 1] > head[f, 1] + 10 else ''}")
