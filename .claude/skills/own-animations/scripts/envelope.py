"""Timing envelope of records: length, path, the times at 10/50/80/95 % of the path, the low phase (pelvis under 45 cm),
the inverted phase (pelvis 10 cm over the head) and the peak pelvis speed.

    python3 envelope.py <base models> <stance name, e.g. HUMANS-S_1HRUN> <record.MAN.json>...

Variants of one move keep one envelope (length, path, the engine's i-frame window); compare them here.
"""
import sys
from pathlib import Path

import numpy as np

from common import ga, positions, rs

B = Path(sys.argv[1])
st = ga.read_animation(B / f"{sys.argv[2].removesuffix('.MAN')}.MAN.json")
skel = rs.with_offsets(ga.read_skeleton(B / "HUMANS.MDH.json"), st)
gi = {n.name: i for i, n in enumerate(skel.nodes)}
w0 = ga.world_pose(skel, st, 0)
floor = min(w0[gi[f"BIP01 {s} TOE0"]][1, 3] for s in "LR")
for f in sys.argv[3:]:
    a = ga.read_animation(f)
    P = positions(skel, a)
    pel, head = P[:, gi["BIP01 PELVIS"]], P[:, gi["BIP01 HEAD"]]
    tr = ga.root_travel(a)[:, [0, 2]]
    dd = np.linalg.norm(tr - tr[0], axis=1)
    t = lambda q: next(i for i, x in enumerate(dd) if x >= q * dd[-1]) / a.fps  # noqa: E731
    span = lambda ix: f"{ix[0] / a.fps:.2f}-{ix[-1] / a.fps:.2f}" if len(ix) else "-"  # noqa: E731
    sp = np.linalg.norm(np.diff(pel[:, [0, 2]], axis=0), axis=1) * a.fps / 100
    print(f"{Path(f).name[:40]:40s} {(a.frame_count - 1) / a.fps:.2f}s path {dd[-1] / 100:.2f}m  t10 {t(.1):.2f} t50 {t(.5):.2f} "
          f"t80 {t(.8):.2f} t95 {t(.95):.2f}  low {span(np.where(pel[:, 1] - floor < 45)[0])}  "
          f"inv {span(np.where(pel[:, 1] > head[:, 1] + 10)[0])}  peak {sp.max():.1f}@{np.argmax(sp) / a.fps:.2f}")
