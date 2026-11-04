"""Finish one retargeted dodge (roll, dash, heavy roll) for the chosen 1H levels, as the in-game set is finished.

    python3 finish_levels.py <src.MAN.json> <base models> <out dir> --name T_1HROLLF --warp "0:0,0.40:0.20,...,1.68:1.12"
                             [--levels novice,fighter,master] [any finish.py flag, e.g. --fade 0.24,0.2]

Writes <out dir>/<MODEL>-<name>.MAN.json (+ .bin) per level: novice HUMANS, fighter HUMANS_1HST1, master HUMANS_1HST2.
Each level ends on its own stance <MODEL>-S_1HRUN; the master's stance writes no right foot, so it comes from the
novice stance (--ref-below). Feet are pinned only near the ends (--lock-ends 0.3: in a roll a foot lies on the ground
mid-move too), the sword arm is cleared of the floor and the body (--clear-sword), parade-jump nodes fill the rest.
--warp is src:dst seconds from 0:0 to the end; put the knots on the phases (envelope.py, timeline.py), not one
uniform stretch. Strikes and other sets: scripts/content/anim/finish_set.py.
"""
import argparse
import subprocess
import sys
from pathlib import Path

from common import ANIM, LEVELS

ap = argparse.ArgumentParser()
ap.add_argument("src", type=Path)
ap.add_argument("base", type=Path)
ap.add_argument("out", type=Path)
ap.add_argument("--name", required=True)
ap.add_argument("--warp", required=True)
ap.add_argument("--levels", default="novice,fighter,master")
args, extra = ap.parse_known_args()
B = args.base
args.out.mkdir(parents=True, exist_ok=True)
for level in args.levels.split(","):
    m = LEVELS[level]
    dst = args.out / f"{m}-{args.name}.MAN.json"
    cmd = [sys.executable, str(ANIM / "finish.py"), str(args.src), str(dst), "--ref", str(B / f"{m}-S_1HRUN.MAN.json"),
           "--skeleton", str(B / "HUMANS.MDH.json"), "--name", args.name, "--warp", args.warp, "--lock-ends", "0.3", "--clear-sword",
           *(["--ref-below", str(B / "HUMANS-S_1HRUN.MAN.json")] if m == "HUMANS_1HST2" else []),
           "--fill", str(B / f"{m}-T_1HPARADEJUMPB.MAN.json"), "--fill", str(B / "HUMANS-T_1HPARADEJUMPB.MAN.json"), *extra]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stderr[-1500:])
    print(dst)
