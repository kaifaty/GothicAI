"""Pick the most different plausible samples of one move: farthest-point order from the sample in the game.

    python3 diverse.py <base models> <scan.json> <in-game tag> [k] [--path MIN,MAX] [--turn DEG]

Plausible: not in the air (air ≤ 4 frames), the pelvis goes low (< 45 cm), the pelvis turns at most --turn deg a frame
(45 by default), and with --path the source path in m is inside MIN..MAX (finish.py brings every path to the game's
anyway, so a path filter only keeps samples whose pace would change least). Distance: joints of head, hands, feet,
knees and chest against the pelvis, in the travel frame, over 24 points of normalised time, RMS in metres.
The picks are only different by numbers: look at each in the game before naming it (a "low roll" can be a slide).
"""
import argparse
import json
from pathlib import Path

import numpy as np

from common import ga, positions, rs

ap = argparse.ArgumentParser()
ap.add_argument("base", type=Path)
ap.add_argument("scan", type=Path)
ap.add_argument("current")
ap.add_argument("k", type=int, nargs="?", default=8)
ap.add_argument("--path", help="MIN,MAX source path in m")
ap.add_argument("--turn", type=float, default=45.0)
args = ap.parse_args()
skel0 = ga.read_skeleton(args.base / "HUMANS.MDH.json")
skel = rs.with_offsets(skel0, ga.read_animation(args.base / "HUMANS-S_1HRUN.MAN.json"))
gi = {n.name: i for i, n in enumerate(skel.nodes)}
J = ["BIP01 HEAD", "BIP01 L HAND", "BIP01 R HAND", "BIP01 L FOOT", "BIP01 R FOOT", "BIP01 L CALF", "BIP01 R CALF", "BIP01 SPINE2"]
lo, hi = map(float, args.path.split(",")) if args.path else (0.0, 1e9)
rows = [r for r in json.loads(args.scan.read_text())
        if r["tag"] == args.current or (r["air"] <= 4 and r["pel"] < 45 and r["turn"] <= args.turn and lo <= np.hypot(r["fwd"], r["right"]) <= hi)]


def feat(f):
    a = ga.read_animation(f)
    P = positions(skel, a)
    pel = P[:, gi["BIP01 PELVIS"]]
    d = pel[-1, [0, 2]] - pel[0, [0, 2]]
    d /= np.linalg.norm(d) + 1e-9
    side = np.array([-d[1], d[0]])
    out = []
    for x in np.linspace(0, a.frame_count - 1, 24):
        i = int(x)
        w = x - i
        Q = P[i] * (1 - w) + P[min(i + 1, a.frame_count - 1)] * w
        p = Q[gi["BIP01 PELVIS"]]
        out.append([p[1] / 100])
        for n in J:
            r = Q[gi[n]] - p
            out.append([(r[[0, 2]] @ d) / 100, r[1] / 100, (r[[0, 2]] @ side) / 100])
    return np.concatenate([np.ravel(o) for o in out])


F = {r["tag"]: feat(r["file"]) for r in rows}
D = lambda a, b: float(np.sqrt(np.mean((F[a] - F[b]) ** 2)))  # noqa: E731
pick = [args.current]
while len(pick) < min(args.k, len(F)):
    pick.append(max((t for t in F if t not in pick), key=lambda t: min(D(t, p) for p in pick)))
by = {r["tag"]: r for r in rows}
print(f"{len(rows)} plausible")
for t in pick:
    r = by[t]
    print(f"{t:36s} min dist {min([D(t, p) for p in pick if p != t] or [0]):.3f}  s {r['s']} path {np.hypot(r['fwd'], r['right']):.1f} "
          f"inv {r['inv']} turn {r['turn']} peak {r['peak']} pel {r['pel']} kneel {r['kneel']} hands {r['hands']} tuck {r['tuck']} low {r['low']}")
