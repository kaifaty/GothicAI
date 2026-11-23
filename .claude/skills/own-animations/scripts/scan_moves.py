"""Retarget every Kimodo sample of one move onto BIP01 and measure it raw: path, roll over, flight, turn, speed, style.

    python3 scan_moves.py <base models> <out dir> <label> <move regex> <set dir>...

A set dir holds stance.json ({"stance": "HUMANS-S_1HRUN.MAN"}) and <move>/out/*.npz; <move regex> picks the move
folders, e.g. '^(roll_forward|forward_\\d+m_\\d+)$' or '^(heavy_roll_left|heavy_left_\\d+)$'.
Writes <out dir>/src/HUMANS-T_<SET>-<MOVE>-<NN>.MAN.json per sample and <out dir>/scan-<label>.json:
  s     seconds;            fwd/right  pelvis travel in m (constrained path, same across one set)
  inv   frames with the pelvis 10 cm over the head (rolls over);  air  frames with every part 22 cm off the floor
  turn  largest pelvis turn a frame, deg;  peak  pelvis ground speed m/s;  pel  lowest pelvis cm
  kneel frames of the last 45 % with a knee under 12 cm;  hands  frames with a hand under 10 cm
  tuck  calf to chest at the most inverted frame, cm;  low  frames with the pelvis under 45 cm
"""
import json
import re
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np

from common import ga, positions, rs, turn

B, OUT, LABEL, PAT = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3], sys.argv[4]
N = ("BIP01 L TOE0", "BIP01 R TOE0", "BIP01 L FOOT", "BIP01 R FOOT", "BIP01 L HAND", "BIP01 R HAND", "BIP01 HEAD", "BIP01 PELVIS",
     "BIP01 SPINE2", "BIP01 NECK", "BIP01 L UPPERARM", "BIP01 R UPPERARM", "BIP01 L CALF", "BIP01 R CALF")


def one(job):
    npz, stance, tag = job
    skel0 = ga.read_skeleton(B / "HUMANS.MDH.json")
    st = ga.read_animation(B / f"{stance}.json")
    a = rs.retarget(Path(npz), skel0, "T_" + tag.upper(), "", 25.0, 30.0, None, st)
    dst = OUT / "src" / f"HUMANS-T_{tag.upper()}.MAN.json"
    dst.parent.mkdir(parents=True, exist_ok=True)
    ga.write_animation(dst, a, source=f"kimodo:{npz}")
    skel = rs.with_offsets(skel0, st)
    gi = {n.name: i for i, n in enumerate(skel.nodes)}
    w0 = ga.world_pose(skel, st, 0)
    floor = min(w0[gi[f"BIP01 {s} TOE0"]][1, 3] for s in "LR")
    P = positions(skel, a)
    W = [ga.world_pose(skel, a, k) for k in range(a.frame_count)]
    g = lambda n: P[:, gi[n]]  # noqa: E731
    low = np.min(np.stack([g(n)[:, 1] for n in N]), axis=0) - floor
    pel, head = g("BIP01 PELVIS"), g("BIP01 HEAD")
    invm = pel[:, 1] - head[:, 1]
    unit = lambda v: v / (np.linalg.norm(v) + 1e-9)  # noqa: E731
    fwd = unit(sum(g(f"BIP01 {s} TOE0")[0, [0, 2]] - g(f"BIP01 {s} FOOT")[0, [0, 2]] for s in "LR"))
    right = unit(g("BIP01 R THIGH")[0, [0, 2]] - g("BIP01 L THIGH")[0, [0, 2]])
    d = pel[-1, [0, 2]] - pel[0, [0, 2]]
    pt = max(turn(W[i][gi["BIP01 PELVIS"]][:3, :3], W[i + 1][gi["BIP01 PELVIS"]][:3, :3]) for i in range(len(W) - 1))
    sp = np.linalg.norm(np.diff(pel[:, [0, 2]], axis=0), axis=1) * 25 / 100
    knee = np.minimum(g("BIP01 L CALF")[:, 1], g("BIP01 R CALF")[:, 1]) - floor
    hand = np.minimum(g("BIP01 L HAND")[:, 1], g("BIP01 R HAND")[:, 1]) - floor
    n = a.frame_count
    k = int(np.argmax(invm))
    return {"tag": tag, "file": str(dst), "s": round((n - 1) / 25, 2), "fwd": round(float(d @ fwd) / 100, 2),
            "right": round(float(d @ right) / 100, 2), "inv": int((invm > 10).sum()), "air": int((low > 22).sum()), "turn": round(pt),
            "peak": round(float(sp.max()), 1), "pel": round(float(pel[:, 1].min() - floor)),
            "kneel": int((knee[int(n * 0.55):] < 12).sum()), "hands": int((hand < 10).sum()),
            "tuck": round(float(min(np.linalg.norm(g(f"BIP01 {s} CALF")[k] - g("BIP01 SPINE2")[k]) for s in "LR"))),
            "low": int((pel[:, 1] - floor < 45).sum())}


if __name__ == "__main__":
    jobs = []
    for sd in map(Path, sys.argv[5:]):
        stance = json.loads((sd / "stance.json").read_text())["stance"].removesuffix(".json")
        for mv in sorted(x for x in sd.iterdir() if (x / "out").is_dir() and re.match(PAT, x.name)):
            for npz in sorted((mv / "out").glob("*.npz")):
                jobs.append((str(npz), stance, f"{sd.name}-{mv.name}-{npz.stem.split('_')[-1]}"))
    with ProcessPoolExecutor(8) as ex:
        rows = list(ex.map(one, jobs))
    (OUT / f"scan-{LABEL}.json").write_text(json.dumps(rows, indent=1))
    print(LABEL, len(rows), "samples")
