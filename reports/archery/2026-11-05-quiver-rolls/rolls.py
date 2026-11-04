#!/usr/bin/env python3
"""The quiver in the hero's dodges (scripts/bench/quiver-rolls.bench): the table of the report from metrics.csv and
bench.log of a run, and the clips: real time at 60 fps and 2x slower at 60 fps, from the bench's slowmo-4 videos.

    rolls.py <run dir> [--clips <dir>] [--json <file>]
"""
import argparse
import csv
import json
import math
import re
import subprocess
from pathlib import Path

SLOWMO = 4          # the bench's slowmo: a row is 1/120 s of the game
FRAME_MS = 1000 / 60
PRE_MS = 500        # the dodge starts 0.5 s into a clip


def axis(r):
    """The quiver's axis toward the bottom in its rest frame (Quiver::probe: swing, back, outward)."""
    sw = math.radians(float(r["qv_swing"]))
    y = -math.cos(sw)
    z = -math.tan(math.radians(float(r["qv_back"]))) * (-y)
    x = math.tan(math.radians(float(r["qv_outward"]))) * (-y)
    n = math.sqrt(x * x + y * y + z * z) or 1
    return x / n, y / n, z / n


def jump_per_frame(rows):
    """The largest turn of the axis in 1/60 s of the game, deg."""
    best = 0.0
    ts = [float(r["t"]) for r in rows]
    ax = [axis(r) for r in rows]
    j = 0
    for i in range(len(rows)):
        while j < len(rows) and ts[j] - ts[i] < FRAME_MS - 0.5:
            j += 1
        if j >= len(rows):
            break
        dot = max(-1.0, min(1.0, sum(a * b for a, b in zip(ax[i], ax[j]))))
        best = max(best, math.degrees(math.acos(dot)) * FRAME_MS / (ts[j] - ts[i]))
    return best


def dodges(log):
    out = []
    for line in log.read_text(errors="replace").splitlines():
        m = re.search(r"melee dodge PC_HERO: (\w+), armour (\w+), (\w+) (T_\w+)", line)
        if m:
            out.append(dict(way=m[1], armour=m[2], move=m[3], anim=m[4]))
    return out


def anim_ms(log):
    ret = {}
    for line in log.read_text(errors="replace").splitlines():
        m = re.match(r"animinfo (T_\w+): .* (\d+) ms ", line)
        if m:
            ret[m[1]] = int(m[2])
    return ret


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run", type=Path)
    ap.add_argument("--clips", type=Path)
    ap.add_argument("--json", type=Path)
    a = ap.parse_args()
    rows = list(csv.DictReader(open(a.run / "metrics.csv")))
    segs = []
    for r in rows:
        if r["seg"] and r["seg"] != "probe_frame" and (not segs or segs[-1] != r["seg"]):
            segs.append(r["seg"])
    log = a.run / "bench.log"
    dd = dodges(log)
    lengths = anim_ms(log)
    table = []
    for seg, d in zip(segs, dd):
        s = [r for r in rows if r["seg"] == seg]
        f = lambda k: max(float(r[k]) for r in s)
        worst_leg = max(s, key=lambda r: float(r["qv_leg"]))
        under = [float(r["t"]) for r in s if float(r["qv_gnd"]) > 0.5]
        row = dict(
            seg=seg, **d,
            anim_ms=lengths.get(d["anim"]),
            clip_ms=round(float(s[-1]["t"])),
            on=min(int(r["qv_on"]) for r in s),
            gnd=f("qv_gnd"), gnd_mouth=f("qv_gndm"),
            gnd_ms=round(len(under) * 1000 / 120),
            gnd_from=round(min(under) - PRE_MS) if under else None,
            leg=f("qv_leg"), leg_at=float(worst_leg["qv_legt"]), leg_left=f("qv_legl"),
            arm=f("qv_arm"), armor=f("qv_armor"),
            swing=f("qv_swing"), yield_=f("qv_yield"),
            jump=jump_per_frame(s),
        )
        table.append(row)
        if a.clips:
            src = a.run / f"{seg}.mp4"
            for name, pts in ((f"{seg}.mp4", 1 / SLOWMO), (f"{seg}_slow2x.mp4", 2 / SLOWMO)):
                subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", str(src), "-vf", f"setpts={pts}*PTS",
                                "-r", "60", "-c:v", "libx264", "-crf", "20", "-pix_fmt", "yuv420p", "-an",
                                str(a.clips / name)], check=True)
    hdr = ("| клип | броня | уклонение | длительность, мс | земля: труба / устье, см | в земле, мс (с какой мс) "
           "| нога, см (глубина по оси) | из неё левая | кисть и предплечье | полы | отклон, ° | цель уступки, ° "
           "| скачок за 1/60 с, ° |")
    print(hdr)
    print("|" + "---|" * 13)
    for r in table:
        gfrom = f" (с {r['gnd_from']})" if r["gnd_from"] is not None else ""
        print(f"| `{r['seg']}` | {r['armour']} | {r['move']} {r['way']} `{r['anim']}` | {r['anim_ms']} "
              f"| {r['gnd']:.2f} / {r['gnd_mouth']:.2f} | {r['gnd_ms']}{gfrom} | {r['leg']:.2f} ({r['leg_at']:.1f}) "
              f"| {r['leg_left']:.2f} | {r['arm']:.2f} | {r['armor']:.2f} | {r['swing']:.2f} | {r['yield_']:.2f} "
              f"| {r['jump']:.2f} |")
    if a.json:
        a.json.write_text(json.dumps(table, ensure_ascii=False, indent=1) + "\n")


if __name__ == "__main__":
    main()
