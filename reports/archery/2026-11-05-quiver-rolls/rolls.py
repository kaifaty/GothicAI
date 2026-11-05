#!/usr/bin/env python3
"""The quiver in the hero's dodges and on the ground (scripts/bench/quiver-rolls.bench): the tables of the report
from metrics.csv and bench.log of a run, and the clips (real time and 2x slower, 60 fps) from its slowmo-4 videos.

    rolls.py <run dir> [--clips <dir>] [--json <file>]

A dodge belongs to the recording whose `rec <name>` line precedes its `melee dodge PC_HERO: ...` line in bench.log.
The jump per 1/60 s is split by the quiver's mode: `ground` where qv_gmode is not 0 in any of the rows it spans
(the ground pushes it or it comes back), `give` elsewhere (the ordinary give, 1 deg in 1/60 s at most).
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

NAMES = {
    "lea_sneak": "подкрадывание, лёгкая броня",
    "kdf_sneak": "подкрадывание, мантия мага огня",
    "npc_sit_ground": "NPC садится на землю у костра",
    "hero_knockdown": "нокдаун героя: удар, лёжа, подъём",
}


def dodges(log):
    """{recording: dodge} by the `rec <name>` line before each dodge line."""
    out, seg = {}, None
    for line in log.read_text(errors="replace").splitlines():
        m = re.match(r"rec (\S+)$", line)
        if m:
            seg = m[1]
            continue
        m = re.search(r"melee dodge PC_HERO: (\w+), armour (\w+), (\w+) (T_\w+)", line)
        if m and seg:
            out[seg] = dict(way=m[1], armour=m[2], move=m[3], anim=m[4])
    return out


def anim_ms(log):
    ret = {}
    for line in log.read_text(errors="replace").splitlines():
        m = re.match(r"animinfo (T_\w+): .* (\d+) ms ", line)
        if m:
            ret[m[1]] = int(m[2])
    return ret


def axis(r):
    return float(r["qv_ax"]), float(r["qv_ay"]), float(r["qv_az"])


def jumps(rows):
    """The largest turn of the axis in 1/60 s of the game, deg: (ground, give)."""
    ground = give = 0.0
    ts = [float(r["t"]) for r in rows]
    j = 0
    for i in range(len(rows)):
        while j < len(rows) and ts[j] - ts[i] < FRAME_MS - 0.5:
            j += 1
        if j >= len(rows):
            break
        a, b = axis(rows[i]), axis(rows[j])
        dot = max(-1.0, min(1.0, sum(p * q for p, q in zip(a, b))))
        d = math.degrees(math.acos(dot)) * FRAME_MS / (ts[j] - ts[i])
        if any(rows[k]["qv_gmode"] != "0" for k in range(i, j + 1)):
            ground = max(ground, d)
        else:
            give = max(give, d)
    return ground, give


def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * q))] if v else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run", type=Path)
    ap.add_argument("--clips", type=Path)
    ap.add_argument("--json", type=Path)
    a = ap.parse_args()
    rows = list(csv.DictReader(open(a.run / "metrics.csv")))
    segs = []
    for r in rows:
        if r["seg"] and r["seg"] != "probe_frame" and r["seg"] not in segs:
            segs.append(r["seg"])
    log = a.run / "bench.log"
    dd = dodges(log)
    lengths = anim_ms(log)
    table = []
    for seg in segs:
        s = [r for r in rows if r["seg"] == seg]
        f = lambda k: max(float(r[k]) for r in s)
        d = dd.get(seg)
        under = [float(r["t"]) for r in s if float(r["qv_gndt"]) > 2.0]
        ground, give = jumps(s)
        bot = [float(r["qv_botup"]) for r in s if r["qv_botup"] not in ("", "nan") and r["qv_gmode"] != "0"]
        row = dict(
            seg=seg,
            what=(f"{d['armour']}: {d['move']} {d['way']} `{d['anim']}`" if d else NAMES.get(seg, seg)),
            anim_ms=lengths.get(d["anim"]) if d else None,
            clip_ms=round(float(s[-1]["t"])),
            on=min(int(r["qv_on"]) for r in s),
            tube=f("qv_gndt"), tube_all=f("qv_gnd"), mouth=f("qv_gndm"),
            tube_ms=round(len(under) * 1000 / 120),
            trunk=f("qv_trunk"), leg=f("qv_leg"), leg_left=f("qv_legl"), arm=f("qv_arm"), armor=f("qv_armor"),
            swing=f("qv_swing"), ground_jump=ground, give_jump=give,
            bottom_max=max(bot) if bot else None, bottom_min=min(bot) if bot else None,
            ground_ms=round(sum(r["qv_gmode"] != "0" for r in s) * 1000 / 120),
            rays=sum(int(r["qv_rays"]) for r in s),
            us_p50=pct([float(r["qv_us"]) for r in s], .5), us_p95=pct([float(r["qv_us"]) for r in s], .95),
            us_max=f("qv_us"), cpu_max=f("qv_cpu"),
        )
        table.append(row)
        if a.clips:
            src = a.run / f"{seg}.mp4"
            for name, pts in ((f"{seg}.mp4", 1 / SLOWMO), (f"{seg}_slow2x.mp4", 2 / SLOWMO)):
                subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", str(src), "-vf", f"setpts={pts}*PTS",
                                "-r", "60", "-c:v", "libx264", "-crf", "22", "-pix_fmt", "yuv420p", "-an",
                                str(a.clips / name)], check=True)
    print("| клип | что | длит., мс | труба в земле, см (мс > 2 см) | устье, см | туловище | нога (из неё левая) "
          "| кисть и рука | броня | отклон, ° | скачок от земли / прочего, ° |")
    print("|" + "---|" * 11)
    for r in table:
        print(f"| `{r['seg']}` | {r['what']} | {r['anim_ms'] or r['clip_ms']} | {r['tube']:.2f} ({r['tube_ms']}) "
              f"| {r['mouth']:.2f} | {r['trunk']:.2f} | {r['leg']:.2f} ({r['leg_left']:.2f}) | {r['arm']:.2f} "
              f"| {r['armor']:.2f} | {r['swing']:.1f} | {r['ground_jump']:.2f} / {r['give_jump']:.2f} |")
    print()
    print("| клип | земля толкает, мс | низ трубы над землёй в режиме земли, см | лучей | цена p50 / p95 / наиб., мкс "
          "| время потока наиб., мкс |")
    print("|" + "---|" * 6)
    for r in table:
        b = f"{r['bottom_min']:.1f}–{r['bottom_max']:.1f}" if r["bottom_max"] is not None else "—"
        print(f"| `{r['seg']}` | {r['ground_ms']} | {b} | {r['rays']} | {r['us_p50']:.1f} / {r['us_p95']:.1f} / "
              f"{r['us_max']:.0f} | {r['cpu_max']:.0f} |")
    if a.json:
        a.json.write_text(json.dumps(table, ensure_ascii=False, indent=1) + "\n")


if __name__ == "__main__":
    main()
