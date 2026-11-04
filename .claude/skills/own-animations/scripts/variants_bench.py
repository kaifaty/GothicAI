"""Bench scenario that films picked variants next to the move in the game: per move code the game's animation, then each
variant T_VAR<code><letter>, from two views (side or front, and three-quarter), at talent 2 (melee-shadow arms the master).

    python3 variants_bench.py <picks.txt> <out.bench> [--wait 1900]

picks.txt, one variant a line: <code> <game animation> <direction F|B|L|R> <letter> <source tag> [wait ms], e.g.
    F  T_1HROLLF       F A V2-ROLL_FORWARD-00
    HB T_1HROLLHEAVYB  B A DODGE-ST0-HEAVY_ROLL_BACK-01 2300
Takes: <code>0 for the game's move, <code><letter> for a variant, lower case, plus -q. A bench over ~40 takes needs
TIMEOUT=900 (the default 300 s kills it midway). Grid video: compose_variants.py.
"""
import argparse
from collections import defaultdict

ap = argparse.ArgumentParser()
ap.add_argument("picks")
ap.add_argument("out")
ap.add_argument("--wait", type=int, default=1900)
args = ap.parse_args()
moves = defaultdict(list)
game, way, wait = {}, {}, {}
for line in open(args.picks):
    if not line.strip() or line.startswith("#"):
        continue
    code, ani, d, letter, _tag, *ms = line.split()
    game[code], way[code] = ani, d
    wait[code] = int(ms[0]) if ms else wait.get(code, args.wait)
    moves[code].append(letter)
VIEWS = {"F": (270, 315), "B": (270, 315), "L": (0, 35), "R": (0, 325)}
SPOT = "NW_BIGFARM_VORPOSTEN1_01_RIGHTFIELD_02"
out = ["# Variants of a move side by side: the move in the game, then Kimodo samples (finished only), two views each.",
       "# WORLD=NEWWORLD.ZEN OPENGOTHIC_CONTENT=/abs/content",
       "seed 73013", "cheat god", "set time 12 0", "release all", "overlay off", f"goto {SPOT}", "wait 500", "watch player", "clear 3000",
       "melee-shadow weapon 1h", "wait 1500", ""]
for code, letters in moves.items():
    side, q = VIEWS[way[code]]
    for key, ani in [(f"{code}0", game[code])] + [(f"{code}{x}", f"T_VAR{code}{x}") for x in letters]:
        for suf, ang, dist, h in (("", side, 400, 110), ("-q", q, 420, 140)):
            out += [f"cam follow {dist} {h} {ang}", f"goto {SPOT}", "melee-shadow weapon 1h", "wait 2000",
                    f"rec {key.lower()}{suf}", f"ani player {ani}", f"wait {wait[code]}", "stop", ""]
open(args.out, "w").write("\n".join(out))
print(args.out, sum(len(v) + 1 for v in moves.values()) * 2, "takes")
