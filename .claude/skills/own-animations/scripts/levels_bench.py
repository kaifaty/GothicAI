"""Bench scenario that films one 1H move at the three talent levels: novice, fighter, master; four takes each.

    python3 levels_bench.py <animation, e.g. T_1HROLLF> <out.bench> [--side] [--wait 1700]

Takes nov, fig, mas with suffixes "", -q, -front, -slow (slowmo 3), as compose_ba.py expects. Forward and back moves
are filmed from the side first; --side (left and right moves) puts the front view first and turns the three-quarter
view to the side the hero goes. --wait covers the move and the settle (1700 ms for a 1.12 s roll, longer for a heavy
one). Run it on the Mac twice, with the game's content and with the new one:

    TIMEOUT=900 WORLD=NEWWORLD.ZEN BENCH_HOST=local OPENGOTHIC_CONTENT=<content dir> scripts/bench.sh <out.bench> <bench dir>
"""
import argparse

ap = argparse.ArgumentParser()
ap.add_argument("ani")
ap.add_argument("out")
ap.add_argument("--side", action="store_true")
ap.add_argument("--wait", type=int, default=1700)
args = ap.parse_args()
SPOT = "NW_BIGFARM_VORPOSTEN1_01_RIGHTFIELD_02"
if args.side:
    q = 35 if args.ani.endswith("L") else 325
    views = [("", (420, 120, 0)), ("-q", (420, 140, q)), ("-front", (400, 110, 270)), ("-slow", (420, 120, 0))]
else:
    views = [("", (400, 110, 270)), ("-q", (420, 140, 315)), ("-front", (420, 120, 0)), ("-slow", (400, 110, 270))]
out = [f"# {args.ani} by 1H talent: novice (HUMANS), fighter (HUMANS_1HST1), master (HUMANS_1HST2); four views, the last slowed 3x.",
       "# WORLD=NEWWORLD.ZEN OPENGOTHIC_CONTENT=/abs/content",
       "seed 73013", "cheat god", "set time 12 0", "release all", "overlay off", f"goto {SPOT}", "wait 500", "watch player", "clear 3000",
       "melee-shadow weapon 1h", "wait 1500", ""]
for key, (skill, talent) in (("nov", (10, 0)), ("fig", (40, 1)), ("mas", (80, 2))):
    for suf, (dist, height, angle) in views:
        if suf == "-slow":
            out.append("slowmo 3")
        out += [f"cam follow {dist} {height} {angle}", f"goto {SPOT}", "melee-shadow weapon 1h", f"stats 1h {skill} talent {talent}", "wait 2000",
                f"animinfo S_1HRUN {args.ani}", f"rec {key}{suf}", f"ani player {args.ani}", f"wait {args.wait}", "stop"]
        if suf == "-slow":
            out.append("slowmo 1")
        out.append("")
open(args.out, "w").write("\n".join(out))
print(args.out, "12 takes")
