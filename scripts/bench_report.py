#!/usr/bin/env python3
"""Report for a test-bench run (scripts/bench.sh): finds suspicious frames in metrics.csv and makes
contact sheets (frame grids) from the recorded videos, so the frames can be looked at as images.

  bench_report.py <out dir>                          summary, report.md, sheets/
  bench_report.py frames <out dir> <seg> <from> <to> [step]   one sheet of the given frames
  bench_report.py compare <video a> <video b> [from] [to]      side by side video and sheet (a left, b right)
  bench_report.py diff <out dir a> <out dir b>                 event counts per segment, before -> after
  bench_report.py aggregate <out dir>...                       the arrows, dice and damage of several runs, by segment
"""
import csv
import json
import math
import os
import re
import subprocess
import sys
from collections import Counter, defaultdict, deque

FPS = 30
TOP_EVENTS = 12
LEGS = ("l", "r", "fl", "fr")  # hind (or the only) pair, front pair of a four-legged creature
OTHER = {"l": "r", "r": "l", "fl": "fr", "fr": "fl"}
CONTACT = ("float", "sink", "toe_sink")

# flag: (threshold, unit, what it means)
# Gaze::Source
GAZE_SRC = {0: "none", 1: "talk", 2: "script", 3: "focus", 4: "attention"}

FLAGS = {
    "float":     (3.0, "cm",  "planted foot above the ground: ankle and toes (contact error > 0)"),
    "sink":      (3.0, "cm",  "planted ankle below the ground (contact error < 0)"),
    "toe_sink":  (3.0, "cm",  "toe joint lower under the ground than in the animation"),
    "pop":       (12.0, "cm", "IK correction of a joint changed abruptly (second difference over 3 frames)"),
    "pelvis":    (2.0, "cm",  "body height jumped in one frame"),
    "knee":      (25.0, "deg", "knee bends away from the animated direction"),
    "toe_twist": (12.0, "deg", "planted toes turned sideways beyond the intended outward turn"),
    "stretch":   (2.0, "cm",  "planted leg straight and still can not reach the ground"),
    "gaze_off":  (15.0, "deg", "looking past the partner although it is within reach of the turn"),
    "aim_off":   (3.0, "deg", "the bow (the spell) aims past where the shot leaves, although the turn could reach it"),
    "gaze_jerk": (10.0, "deg", "the face turned this much in one frame (a turn to the partner goes up to 9)"),
    "anim_pop":  (20.0, "cm", "a joint of the animated pose (before lean, IK, gaze) jumped at a switch of animations: second difference over 3 frames"),
    "lip_open":  (0.3, "",    "the mouth stays open although the voice is silent"),
    "lip_shut":  (0.7, "",    "the voice is loud but the mouth stays shut"),
    "face_jump": (0.5, "",    "a face came or went at once (it fades in about a second; T_HURT in 0.1 s)"),
    "flinch_none": (1.0, "x", "a blow and the upper body hardly gave way: 1.5 degrees over the peak"),
    "flinch_slow": (1.5, "s", "the upper body is not back this long after the last blow"),
    "blink_shut": (0.5, "",   "a blink keeps the eyes shut too long (a blink is closed for 2-4 frames)"),
}
TEXT = ("an_bone", "an_layers")  # columns that are names, not numbers
FACES = ("face_neutral", "face_friendly", "face_angry", "face_hostile", "face_afraid", "face_pain")
LIPS = ("lip_open", "lip_shut")
BLINK_FRAMES = 7  # the eyes half shut by a blink longer than this many frames: stuck
LIP_FRAMES = 4  # the mouth closes and opens over a frame or two: shorter runs are not flagged


def counted(rows, col):
    """what a counter of the watched one added over the rows: a switch of the watched one (a new counter,
    first read as 0) is not counted"""
    return int(sum(d for d in (b[col] - a[col] for a, b in zip(rows, rows[1:])) if 0 < d <= 3))


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return math.nan


def load(out):
    path = os.path.join(out, "metrics.csv")
    segs = defaultdict(list)
    if not os.path.exists(path):
        return segs
    with open(path) as f:
        for row in csv.DictReader(f):
            seg = row.pop("seg")
            segs[seg].append({k: (v if k in TEXT else num(v)) for k, v in row.items()})
    return segs


def legs_of(r):
    n = r.get("nlegs")
    return LEGS[:4] if n is not None and n >= 4 else LEGS[:2]


def vec(r, leg, joint):
    return (r[f"{leg}_{joint}_x"], r[f"{leg}_{joint}_y"], r[f"{leg}_{joint}_z"])


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def length(a):
    return math.sqrt(a[0] ** 2 + a[1] ** 2 + a[2] ** 2)


def contact_err(r, leg):
    """how far a planted foot is above (+) or below (-) the ground; standing on the toes counts"""
    err = r[f"{leg}_err"]
    if r[f"{leg}_toehas"] > 0 and r[f"{leg}_toey"] - r[f"{leg}_gy"] < 25:  # higher: a wall, not a step
        toe_err = (r[f"{leg}_toef_y"] - r[f"{leg}_toey"]) - (r[f"{leg}_toea_y"] - r["floor"])
        if err > 0:
            return min(err, max(toe_err, 0.0))
    return err


def toe_yaw(r, leg):
    """horizontal turn of the toes from the animated direction, degrees, + is outward"""
    a = sub(vec(r, leg, "toea"), vec(r, leg, "anka"))
    f = sub(vec(r, leg, "toef"), vec(r, leg, "ankf"))
    la, lf = math.hypot(a[0], a[2]), math.hypot(f[0], f[2])
    if la < 8 or lf < 8:
        return math.nan  # the foot points almost straight down: its horizontal direction is noise
    cos = (a[0] * f[0] + a[2] * f[2]) / (la * lf)
    ang = math.degrees(math.acos(max(-1.0, min(1.0, cos))))
    side = sub(vec(r, leg, "hip"), vec(r, OTHER[leg], "hip"))
    grow = (f[0] / lf - a[0] / la) * side[0] + (f[2] / lf - a[2] / la) * side[2]
    return ang if grow >= 0 else -ang


def frame_flags(prev2, prev, r, switch=False):
    """flags of one frame: list of (flag, leg, value); switch: next to a switch of animations"""
    out = []
    on = r["ik_valid"] > 0 and r["ik_w"] > 0.99
    if prev is not None and on and prev["ik_w"] > 0.99:
        jump = abs(r["pelvis"] - prev["pelvis"])
        if "chest" in r and legs_of(r) == LEGS:
            jump = max(jump, abs(r["chest"] - prev["chest"]))
        out.append(("pelvis", "-", jump))
    if "g_w" in r:
        # within reach: the head can turn 65 degrees, with the body 110 (the goal is clamped to that)
        if r["g_w"] > 0.99 and r["g_err"] >= 0 and abs(r["g_yawgoal"]) < 60:
            out.append(("gaze_off", "-", r["g_err"]))
        if prev is not None and "g_w" in prev and "g_fyaw" in r:
            # the face in the world, while the gaze holds it (the body may turn under it); not right
            # after a segment starts: the bench teleports the hero there
            if r["g_w"] > 0 and prev["g_w"] > 0 and r["frame"] >= 6:
                dy = (r["g_fyaw"] - prev["g_fyaw"] + 180) % 360 - 180
                out.append(("gaze_jerk", "-", math.hypot(dy, r["g_fpitch"] - prev["g_fpitch"])))
        elif prev is not None and "g_w" in prev:
            out.append(("gaze_jerk", "-", abs(r["g_yaw"] * r["g_w"] - prev["g_yaw"] * prev["g_w"])))
    # the bench starts the history of the pose with the segment: two frames before the first difference.
    # Only at a switch: within an animation it is how the animation moves (a running waran: 80 cm)
    if "an_pop" in r and r["frame"] >= 2 and switch:
        out.append(("anim_pop", r.get("an_bone", "-"), r["an_pop"]))
    if "a_w" in r and r["a_w"] > 0.99 and r.get("a_steady", 1) > 0 and r["a_err"] >= 0 and abs(r["a_yaw"]) < 40 \
            and -45 < r["a_pitch"] < 55:
        out.append(("aim_off", "-", r["a_err"]))
    if prev is not None and FACES[0] in r and FACES[0] in prev:
        out.append(("face_jump", "-", max(abs(r[c] - prev[c]) for c in FACES)))
    if r.get("bk_w", 0) > 0.5:
        out.append(("blink_shut", "-", r["bk_w"]))
    if r.get("lip_on", 0) > 0:
        # the level the mouth aims at (the voice a moment ahead) against how wide it is shown
        if r["lip_level"] < 0.05:
            out.append(("lip_open", "-", r["lip_open"]))
        if r["lip_open"] < 0.1:
            out.append(("lip_shut", "-", r["lip_level"]))
    for leg in legs_of(r):
        planted = r[f"{leg}_plant"] > 0.9 and r[f"{leg}_has"] > 0
        if on and planted:
            err = r[f"{leg}_err"]
            toe_err = math.nan
            if r[f"{leg}_toehas"] > 0 and r[f"{leg}_toey"] - r[f"{leg}_gy"] < 25:  # higher: a wall, not a step
                anim_clear = r[f"{leg}_toea_y"] - r["floor"]
                clear = r[f"{leg}_toef_y"] - r[f"{leg}_toey"]
                toe_err = clear - anim_clear
                out.append(("toe_sink", leg, -toe_err))
            # standing on the toes (end of a step) is standing too
            out.append(("float", leg, err if math.isnan(toe_err) else min(err, toe_err)))
            out.append(("sink", leg, -err))
            hip, knee, ank = vec(r, leg, "hip"), vec(r, leg, "kneef"), vec(r, leg, "ankf")
            full = length(sub(knee, hip)) + length(sub(ank, knee))
            if full > 0 and length(sub(ank, hip)) > 0.97 * full:
                out.append(("stretch", leg, -err))
            out.append(("toe_twist", leg, abs(toe_yaw(r, leg) - r[f"{leg}_out"])))
        if on:
            out.append(("knee", leg, r[f"{leg}_knee"]))
        if prev2 is not None and on and prev["ik_w"] > 0.99 and prev2["ik_w"] > 0.99:
            jump = 0.0
            for j in ("knee", "ank", "toe"):
                c2 = sub(vec(r, leg, j + "f"), vec(r, leg, j + "a"))
                c1 = sub(vec(prev, leg, j + "f"), vec(prev, leg, j + "a"))
                c0 = sub(vec(prev2, leg, j + "f"), vec(prev2, leg, j + "a"))
                jump = max(jump, length((c2[0] - 2 * c1[0] + c0[0], c2[1] - 2 * c1[1] + c0[1], c2[2] - 2 * c1[2] + c0[2])))
            out.append(("pop", leg, jump))
    return out


def switches(rows):
    """frames where the animations switch: another one plays, or one restarts (AnimBlend saw it)"""
    return [i for i in range(1, len(rows)) if rows[i].get("an_layers") != rows[i - 1].get("an_layers")
            or rows[i].get("bl_started", 0) > 0]


def switch_frames(rows):
    """frames a switch can show in: the one before it (it is seen a frame late) to 6 after"""
    return {j for i in switches(rows) for j in range(max(0, i - 1), min(len(rows), i + 7))}


def analyze(rows):
    """per-segment stats and events (runs of flagged frames)"""
    stats = {"frames": len(rows)}
    errs = []
    occl = 0
    on = 0
    current = {}   # (flag, leg) -> event being built
    events = []
    prev2 = prev = None
    near = switch_frames(rows)
    for idx, r in enumerate(rows):
        if r["ik_w"] > 0.99:
            on += 1
        if r["occl"] > 0:
            occl += 1
        for leg in legs_of(r):
            # with IK off too: that is how far the animation misses the ground
            if (r["ik_w"] > 0.99 or r["ik_w"] <= 0) and r[f"{leg}_plant"] > 0.9 and r[f"{leg}_has"] > 0:
                errs.append(abs(contact_err(r, leg)))
        hit = set()
        for flag, leg, v in frame_flags(prev2, prev, r, idx in near):
            if not (v > FLAGS[flag][0]):
                continue
            key = (flag, leg)
            hit.add(key)
            ev = current.get(key)
            if ev is not None and r["frame"] - ev["end"] <= 3:
                ev["end"] = int(r["frame"])
                if v > ev["worst"]:
                    ev["worst"], ev["at"] = v, int(r["frame"])
            else:
                ev = {"flag": flag, "leg": leg, "start": int(r["frame"]), "end": int(r["frame"]),
                      "worst": v, "at": int(r["frame"])}
                if flag == "anim_pop":
                    # what played a few frames before and what plays now: a pop at a switch is a transition
                    before = rows[max(0, idx - 3)].get("an_layers", "-")
                    ev["layers"] = r.get("an_layers", "-") if before == r.get("an_layers") else \
                        f"{before} -> {r.get('an_layers', '-')}"
                current[key] = ev
                events.append(ev)
        prev2, prev = prev, r
    # how much the height of a planted foot above the ground varies: ~0 when it stands on the ground,
    # whatever the creature; works for a body tilted to the ground as a whole too (no floor plane needed)
    gaps = defaultdict(list)
    for r in rows:
        for leg in legs_of(r):
            if r[f"{leg}_plant"] > 0.9 and r[f"{leg}_has"] > 0:
                gaps[leg].append(r[f"{leg}_ankf_y"] - r[f"{leg}_gy"])
    dev = []
    for v in gaps.values():
        if len(v) > 5:
            med = sorted(v)[len(v) // 2]
            dev += [abs(x - med) for x in v]
    if dev:
        dev.sort()
        stats["spread"] = dev[int(len(dev) * 0.9)]
    errs.sort()
    if errs:
        stats["contact_p50"] = errs[len(errs) // 2]
        stats["contact_p90"] = errs[int(len(errs) * 0.9)]
        stats["contact_max"] = errs[-1]
    # limp: the body is lower on the steps of one leg than on the other's (each pair of legs)
    stance = {leg: [] for leg in LEGS}
    for r in rows:
        if r["ik_w"] > 0.99:
            for leg in legs_of(r):
                if r[f"{leg}_plant"] > 0.99 and r[f"{OTHER[leg]}_plant"] < 0.5:
                    stance[leg].append(r["chest"] if leg.startswith("f") else r["pelvis"])
    for a, b in (("l", "r"), ("fl", "fr")):
        if len(stance[a]) > 5 and len(stance[b]) > 5:
            limp = abs(sum(stance[a]) / len(stance[a]) - sum(stance[b]) / len(stance[b]))
            stats["limp"] = max(stats.get("limp", 0.0), limp)
    gaze = sorted(r["g_err"] for r in rows if r.get("g_w", 0) > 0.99 and r.get("g_err", -1) >= 0
                  and abs(r.get("g_yawgoal", 0)) < 60)
    if gaze:
        stats["gaze_p90"] = gaze[int(len(gaze) * 0.9)]
    # how often it looks: share of frames, times it turned to someone, why (talk, script, ...)
    if rows and "g_on" in rows[0]:
        stats["gaze_on"] = sum(1 for r in rows if r["g_on"] > 0) / len(rows)
        stats["gaze_looks"] = sum(1 for a, b in zip(rows, rows[1:]) if a["g_on"] == 0 and b["g_on"] > 0) + int(rows[0]["g_on"] > 0)
        stats["gaze_src"] = sorted({GAZE_SRC.get(int(r.get("g_src", 0)), "?") for r in rows if r["g_on"] > 0})
    # aim: how far off the shot direction while aiming, how far the upper body bends
    # (only while the bow holds still: reloading swings the arrow around on purpose)
    aims = sorted(r["a_err"] for r in rows if r.get("a_w", 0) > 0.99 and r.get("a_err", -1) >= 0
                  and r.get("a_steady", 1) > 0)
    if aims:
        stats["aim_p90"] = aims[int(len(aims) * 0.9)]
        stats["aim_bend"] = max((r["a_bend"] for r in rows if r.get("a_w", 0) > 0.99), key=abs)
    # lean: the largest roll into a turn, the trunk pitch range (degrees)
    if rows and "ln_roll" in rows[0]:
        stats["lean_roll"] = max(abs(r["ln_roll"]) for r in rows)
        stats["lean_pitch"] = (min(r["ln_pitch"] for r in rows), max(r["ln_pitch"] for r in rows))
    # the animated pose: pops (second difference of a joint) overall and next to a switch of animations
    if rows and "an_pop" in rows[0]:
        pops = sorted(r["an_pop"] for r in rows[2:] if not math.isnan(r["an_pop"]))
        if pops:
            stats["anim_p99"] = pops[min(len(pops) - 1, int(len(pops) * 0.99))]
            stats["anim_max"] = pops[-1]
        stats["anim_switches"] = len(switches(rows))
        stats["anim_switch_max"] = max([rows[j]["an_pop"] for j in near if j >= 2] or [0.0])
    # lips: how well the mouth follows the voice (correlation of the voice level and the shown opening)
    lips = [(r["lip_level"], r["lip_open"]) for r in rows if r.get("lip_on", 0) > 0]
    if len(lips) > 10:
        n = len(lips)
        ma = sum(a for a, _ in lips) / n
        mb = sum(b for _, b in lips) / n
        va = sum((a - ma) ** 2 for a, _ in lips)
        vb = sum((b - mb) ** 2 for _, b in lips)
        cov = sum((a - ma) * (b - mb) for a, b in lips)
        if va > 0 and vb > 0:
            stats["lip_corr"] = cov / math.sqrt(va * vb)
    # hands while it speaks: how much of the talk has a gesture, how much of the listening after it
    # (a second after the voice), gestures started, started over a playing one, stopped early
    if rows and "ge_talk" in rows[0]:
        talk = [r for r in rows if r["ge_talk"] > 0]
        quiet, since = [], None
        for i, r in enumerate(rows):
            since = i if r["ge_talk"] > 0 else since
            if r["ge_talk"] == 0 and since is not None and i - since > FPS:
                quiet.append(r)
        if talk:
            stats["ges_talk"] = sum(1 for r in talk if r["ge_id"] > 0) / len(talk)
            stats["ges_listen"] = None if not quiet else sum(1 for r in quiet if r["ge_id"] > 0) / len(quiet)
            for k in ("starts", "cuts", "stops") + (("nods",) if "ge_nods" in rows[0] else ()):
                stats["ges_" + k] = counted(rows, "ge_" + k)
            stats["ges_kinds"] = len({int(r["ge_id"]) for r in rows if r["ge_id"] > 0})
    # listening to the hero (nods as his words come) and thinking of an answer (the eyes aside)
    if rows and "ge_listen" in rows[0] and any(r["ge_listen"] > 0 or r["ge_think"] > 0 for r in rows):
        stats["ges_lnods"] = counted(rows, "ge_lnods")
        stats["ges_aside"] = max(r["ge_aside"] for r in rows)
    # flinch: blows, how far the upper body gives way at most (trunk bend, twist, head), how long it takes
    # to come back after a blow; a blow without a flinch, a flinch that does not come back
    if rows and "f_hits" in rows[0]:
        hits = [i for i in range(1, len(rows)) if rows[i]["f_hits"] > rows[i - 1]["f_hits"]]
        if hits:
            stats["flinch_hits"] = len(hits)
            stats["flinch_peak"] = max(max(r["f_bend"], abs(r["f_twist"]), r["f_head"]) for r in rows)
            settle = 0.0
            for k, i in enumerate(hits):
                end = hits[k + 1] if k + 1 < len(hits) else len(rows)
                peak = max(max(r["f_bend"], abs(r["f_twist"]), r["f_head"]) for r in rows[i:min(end, i + 8)])
                if peak < 1.5:
                    events.append({"flag": "flinch_none", "leg": "-", "start": int(rows[i]["frame"]),
                                   "end": int(rows[i]["frame"]), "worst": 1.5 / max(peak, 0.01), "at": int(rows[i]["frame"])})
                if k + 1 == len(hits):
                    rest = next((j for j in range(i, len(rows)) if rows[j]["f_on"] == 0), None)
                    if rest is not None:
                        settle = max(settle, (rows[rest]["t"] - rows[i]["t"]) / 1000.0)
                    elif len(rows) - i > 45:
                        t = (rows[-1]["t"] - rows[i]["t"]) / 1000.0
                        events.append({"flag": "flinch_slow", "leg": "-", "start": int(rows[i]["frame"]),
                                       "end": int(rows[-1]["frame"]), "worst": t, "at": int(rows[-1]["frame"])})
            stats["flinch_settle"] = settle
    # blinks of the watched one: how many a minute, of them in a pause of the voice, at a shift of the eyes
    if rows and "bk_n" in rows[0] and rows[-1]["bk_on"] > 0:
        secs = (rows[-1]["t"] - rows[0]["t"]) / 1000.0
        if secs > 1:
            for k in ("pause", "shift", "double"):
                stats["blink_" + k] = counted(rows, "bk_" + k)
            # the blinks seen: the eyes going half shut
            stats["blink_n"] = sum(1 for a, b in zip(rows, rows[1:]) if a["bk_w"] <= 0.3 < b["bk_w"])
            stats["blink_rate"] = stats["blink_n"] * 60.0 / secs
    stats["ik_on"] = on / max(1, len(rows))
    stats["hidden"] = occl / max(1, len(rows))
    stats["speed"] = sum(r["speed"] for r in rows) / max(1, len(rows))
    # a contact error for a single frame at the start or the end of a step is not visible
    events = [ev for ev in events if ev["flag"] not in CONTACT or ev["end"] > ev["start"]
              or ev["worst"] >= 2 * FLAGS[ev["flag"]][0]]
    events = [ev for ev in events if ev["flag"] not in LIPS or ev["end"] - ev["start"] + 1 >= LIP_FRAMES]
    events = [ev for ev in events if ev["flag"] != "blink_shut" or ev["end"] - ev["start"] + 1 > BLINK_FRAMES]
    # a blow throws the head around on purpose (the flinch, the stumble): no gaze jerk for a second after it
    if rows and "f_hits" in rows[0]:
        struck = [int(rows[i]["frame"]) for i in range(1, len(rows)) if rows[i]["f_hits"] > rows[i - 1]["f_hits"]]
        events = [ev for ev in events if ev["flag"] != "gaze_jerk" or
                  not any(0 <= ev["start"] - f <= 30 for f in struck)]
    for ev in events:
        ev["severity"] = ev["worst"] / FLAGS[ev["flag"]][0]
    return stats, events


ARCHER_STAGE = {0: "lowered", 1: "raising", 2: "nocked", 3: "drawing", 4: "held", 5: "reloading"}


def faces(segs):
    """the watched one's face per segment: the face anims as shown (face_*, the largest in the segment) and, for an
    own head with a face profile, its FaceRig: anims and targets at most, the blend and the upload of the deformation"""
    rows_all = [r for rows in segs.values() for r in rows]
    if not rows_all or FACES[0] not in rows_all[0]:
        return [], {}
    rig = "rig_on" in rows_all[0] and any(r["rig_on"] > 0 for r in rows_all)
    # the facial gestures of the own head (cue_<name>), those that showed at all
    cues = [c for c in rows_all[0] if c.startswith("cue_") and any(r[c] > 0 for r in rows_all)]
    if not rig and not any(r[c] > 0 for r in rows_all for c in FACES):
        return [], {}
    out = ["## Faces", "",
           "Face anims of the watched one as shown, the largest in a segment (0..1)." +
           (" FaceRig of its own head: active anims and targets with a weight at most, the blend of the deformation "
            "(median / max µs, blends) and its upload to the head (median µs)." if rig else ""), ""]
    cols = list(FACES) + cues + (["rig anims", "rig targets", "blend µs p50 / max (n)", "upload µs p50"] if rig else [])
    out.append("| segment | " + " | ".join(cols) + " |")
    out.append("|" + "---|" * (len(cols) + 1))
    blend_all = []
    for seg, rows in segs.items():
        if not rows:
            continue
        cells = [f"{max(r[c] for r in rows):.2f}" for c in list(FACES) + cues]
        if rig:
            blended = [(b["rig_us"], b["rig_up_us"]) for a, b in zip(rows, rows[1:]) if b["rig_n"] > a["rig_n"]]
            blend_all += [u for u, _ in blended]
            us = sorted(u for u, _ in blended)
            up = sorted(u for _, u in blended)
            cells += [f"{max(r['rig_anims'] for r in rows):.0f}", f"{max(r['rig_targets'] for r in rows):.0f}",
                      "-" if not us else f"{us[len(us) // 2]:.0f} / {us[-1]:.0f} ({len(us)})",
                      "-" if not up else f"{up[len(up) // 2]:.0f}"]
        out.append(f"| {seg} | " + " | ".join(cells) + " |")
    out.append("")
    numbers = {}
    if blend_all:
        blend_all.sort()
        numbers = {"face_blend_us_p50": blend_all[len(blend_all) // 2], "face_blend_us_max": blend_all[-1],
                   "face_blends": len(blend_all)}
        out += [f"FaceRig blend: median {numbers['face_blend_us_p50']:.0f} µs, max {numbers['face_blend_us_max']:.0f} µs "
                f"over {len(blend_all)} blends.", ""]
    return out, numbers


# Eyes::Kind: what the eyes of an own head look at
EYE_KIND = {0: "none", 1: "forward", 2: "eyes", 3: "mouth", 4: "away", 5: "aside", 6: "target"}


def eyes(segs):
    """the watched one's eyes per segment (an own head with "eyes" in its face profile, graphics/mesh/eyes):
    saccades a second, the largest turn in the socket (the left eye's + yaw and the right one's - yaw are out),
    beyond the limits, the error from the fixation point after landing (frames not in a saccade), where it
    looked (share of the frames by kind), the point looked at from the middle of its eyes, without the eyes' own
    jumps around it (median below their level, degrees, and above them, cm, the head's forward above the level,
    degrees, and the eyes' pitch in the socket, degrees, the mean of both; over the frames at the other one's eyes,
    else all) and the blend of the face (median µs of the blends in the segment)"""
    rows_all = [r for rows in segs.values() for r in rows]
    if not rows_all or "ey_on" not in rows_all[0] or not any(r["ey_on"] > 0 for r in rows_all):
        return [], {}
    out = ["## Eyes", "",
           "The eyes of the watched one's own head: saccades a second (all, of 20° and more), the largest turn "
           "in the socket out / in / up / down and beyond the profile's limits (degrees), the error from the "
           "fixation point after landing (median / 90th percentile, degrees), where they looked (share of "
           "the frames), the point looked at from the middle of the eyes in the world, without the eyes' own "
           "jumps around it (median degrees below their level, cm above them, the head's forward above the level "
           "in degrees, the eyes' pitch in the socket in degrees; at the other one's eyes, else all frames) and "
           "the FaceRig blend (median µs, blends).", "",
           "| segment | saccades/s (n, big) | out / in / up / down ° | beyond ° | error p50 / p90 ° | looked at | "
           "point down ° / rise cm / head ° / socket ° (of) | blend µs p50 (n) |",
           "|---|---|---|---|---|---|---|---|"]
    landed_all, rates, blend_all, beyond_all, down_eye, head_point, socket = [], {}, [], 0.0, {}, {}, {}
    for seg, rows in segs.items():
        rows = [r for r in rows if r["ey_on"] > 0]
        if len(rows) < 2:
            continue
        secs = max((rows[-1]["t"] - rows[0]["t"]) / 1000.0, 1e-3)
        n = rows[-1]["ey_sac"] - rows[0]["ey_sac"]
        big = rows[-1]["ey_big"] - rows[0]["ey_big"]
        rates[seg] = n / secs
        turn = (max(max(r["ey_ly"], -r["ey_ry"]) for r in rows), max(max(-r["ey_ly"], r["ey_ry"]) for r in rows),
                max(max(r["ey_lp"], r["ey_rp"]) for r in rows), max(max(-r["ey_lp"], -r["ey_rp"]) for r in rows))
        beyond = max(r["ey_over"] for r in rows)
        beyond_all = max(beyond_all, beyond)
        landed = sorted(r["ey_err"] for r in rows if r["ey_move"] == 0 and r["ey_err"] >= 0)
        landed_all += landed
        kinds = Counter(EYE_KIND.get(int(r["ey_kind"]), "?") for r in rows)
        looked = ", ".join(f"{k} {v * 100 / len(rows):.0f}%" for k, v in kinds.most_common())
        blended = sorted(b["rig_us"] for a, b in zip(rows, rows[1:]) if b["rig_n"] > a["rig_n"])
        blend_all += blended
        err = "-" if not landed else f"{landed[len(landed) // 2]:.2f} / {landed[int(len(landed) * 0.9)]:.2f}"
        blend = "-" if not blended else f"{blended[len(blended) // 2]:.0f} ({len(blended)})"
        point = "-"
        if "ey_down" in rows[0]:
            at_eye = [r for r in rows if int(r["ey_kind"]) == 2]
            of = at_eye or rows
            med = [sorted(r[k] for r in of)[len(of) // 2] for k in ("ey_down", "ey_dy", "ey_hp")]
            pitch = sorted((r["ey_lp"] + r["ey_rp"]) / 2 for r in of)[len(of) // 2]
            point = f"{med[0]:.1f} / {med[1]:.1f} / {med[2]:.1f} / {pitch:.1f} ({'eyes' if at_eye else 'all'})"
            if at_eye:
                down_eye[seg] = med[0]
            # the head's pitch less the point's angle above the eyes: 0 when the face points at it from the eyes
            head_point[seg] = med[2] + med[0]
            socket[seg] = pitch
        out.append(f"| {seg} | {n / secs:.2f} ({n:.0f}, {big:.0f}) | " + " / ".join(f"{v:.1f}" for v in turn) +
                   f" | {beyond:.2f} | {err} | {looked} | {point} | {blend} |")
    out.append("")
    numbers = {"eyes_beyond_max": beyond_all, "eyes_saccades_per_s": rates, "eyes_down_at_eyes_p50": down_eye,
               "eyes_head_minus_point_p50": head_point, "eyes_socket_pitch_p50": socket}
    if landed_all:
        landed_all.sort()
        numbers["eyes_error_p50"] = landed_all[len(landed_all) // 2]
    if blend_all:
        blend_all.sort()
        numbers["eyes_blend_us_p50"] = blend_all[len(blend_all) // 2]
    out += [f"Eyes: beyond the limits at most {beyond_all:.2f}°" +
            ("" if not landed_all else f", error after landing median {numbers['eyes_error_p50']:.2f}°") +
            ("" if not blend_all else f", blend median {numbers['eyes_blend_us_p50']:.0f} µs") + ".", ""]
    return out, numbers


def arrows(out):
    """the arrows of the manual aim per segment, from bench.log: [(zone or 'miss', draw, flew cm)]. An arrow
    counts for the segment it was shot in: a far miss may come down when the next one runs (they come
    down in the order they were shot)"""
    path = os.path.join(out, "bench.log")
    per = defaultdict(list)
    if not os.path.exists(path):
        return per
    seg, shot = None, deque()
    with open(path, errors="replace") as f:  # names of the game's npcs come in its codepage
        for line in f:
            line = line.strip()
            if line.startswith("rec stop"):
                seg = None
            elif line.startswith("rec "):
                seg = line.split()[1]
            elif line.startswith("arrow shot"):
                shot.append(seg)
            elif line.startswith("arrow hit") or line.startswith("arrow missed"):
                at = shot.popleft() if shot else seg
                if not at:
                    continue
                if line.startswith("arrow hit"):
                    body = line.split(": ", 1)[1]
                    zone = body.split(",")[0].strip()
                    draw = float(body.split("draw ")[1].split(",")[0])
                    flew = float(body.split("flew ")[1].split(" ")[0])
                    per[at].append((zone, draw, flew))
                else:
                    # "came down at <xyz>, flew <cm> cm" or "flew out at <xyz>, flew <cm> cm"
                    m = re.search(r"flew (-?[0-9.]+) cm", line)
                    per[at].append(("miss", math.nan, float(m.group(1)) if m else math.nan))
    return per


def manual_aim(out, segs):
    """the manual aim: hits by zone, the sway at full draw, how long the draw takes"""
    shots = arrows(out)
    lines = []
    for seg, rows in segs.items():
        if not rows or "m_stage" not in rows[0] or not any(r["m_stage"] >= 1 for r in rows):
            continue
        held = [r for r in rows if r["m_stage"] == 4]
        sway = math.sqrt(sum(r["m_sway_yaw"] ** 2 + r["m_sway_pitch"] ** 2 for r in held) / len(held)) if held else math.nan
        offs = sorted(r["m_off"] for r in held if r["m_off"] >= 0)
        off = f"{offs[len(offs) // 2]:.2f} / {offs[int(len(offs) * 0.9)]:.2f}" if offs else "-"
        draws, start = [], None
        for r in rows:
            if r["m_stage"] == 3 and start is None:
                start = r["t"]
            elif r["m_stage"] != 3 and start is not None:
                if r["m_stage"] == 4:
                    draws.append(r["t"] - start)
                start = None
        draw = f"{sum(draws) / len(draws) / 1000:.2f}" if draws else "-"
        a = shots.get(seg, [])
        count = defaultdict(int)
        for z, _, _ in a:
            count[z] += 1
        hits = " ".join(f"{z} {count[z]}" for z in ("head", "body", "limb", "miss") if count[z])
        flew = f"{sum(x[2] for x in a) / len(a) / 100:.1f}" if a else "-"
        # the crosshair: how open at full draw, how far it jumps in a frame (it stays where it aims)
        spreads = sorted(r["m_spread"] for r in held if r.get("m_spread", -1) >= 0)
        spread = f"{spreads[len(spreads) // 2]:.2f}" if spreads else "-"
        jumps = [r["m_cross"] for r in rows if r.get("m_cross", -1) >= 0]
        jump = f"{max(jumps):.2f}" if jumps else "-"
        # how smooth it opens and closes: the fastest, deg/s; the largest change of that speed in a frame
        # (a jerk: it starts moving at once)
        rates = []
        for p, q in zip(rows, rows[1:]):
            if p.get("m_spread", -1) >= 0 and q.get("m_spread", -1) >= 0 and q["t"] > p["t"]:
                rates.append((q["m_spread"] - p["m_spread"]) / (q["t"] - p["t"]) * 1000)
        rate = f"{max(abs(v) for v in rates):.0f} / {max(abs(v - u) for u, v in zip(rates, rates[1:])):.0f}" \
            if len(rates) > 1 else "-"
        lines.append(f"| {seg} | {len(a)} | {hits or '-'} | {flew} | {sway:.2f} | {off} | {spread} | {jump} | {rate} | {draw} | "
                     f"{max(r['m_fatigue'] for r in rows):.2f} | {'yes' if any(r['m_holding'] > 0 for r in rows) else '-'} |")
    if not lines:
        return []
    return ["## Manual aim", "",
            "| segment | shots | hits | flew m | sway RMS deg (held) | off p50 / p90 deg (held) | crosshair open deg (held) | crosshair jump max % screen | crosshair speed max / its step deg/s | draw s | fatigue max | breath held |",
            "|---|---|---|---|---|---|---|---|---|---|---|---|"] + lines + [""]


def hit_zones(out):
    """the hit zones fitted to the bodies (`zones` with `zones fit on|off`): of each zone, the points of the
    body in it, in another zone only; by the body's mesh, with the guess and fitted"""
    path = os.path.join(out, "game.log")
    if not os.path.exists(path):
        return []
    fit, table = "on", {}
    for l in open(path, encoding="utf-8", errors="replace"):
        m = re.search(r"\[bench\] zones fit (on|off)", l)
        if m:
            fit = m.group(1)
            continue
        if "coverage (points" not in l:
            continue
        body = re.search(r" body (\S+)\s*$", l)
        name = body.group(1) if body else "?"
        zones = {z: (int(n), int(o), int(w)) for z, n, o, w in
                 re.findall(r"(head|body|limb) (\d+): (\d+)% / (\d+)% / \d+%", l)}
        fitted = re.search(r"(\d+ of \d+) capsules fitted", l)
        table.setdefault(name, {})[fit] = (zones, fitted.group(1) if fitted else "-")
    if not table:
        return []
    lines = ["## Hit zones", "",
             "Of each zone: the points of the body in it, guessed -> fitted (in another zone only, fitted).", "",
             "| body | head | trunk | limbs | fitted capsules |", "|---|---|---|---|---|"]
    for name, v in table.items():
        def cell(z):
            a = v.get("off", ({}, ""))[0].get(z)
            b = v.get("on", ({}, ""))[0].get(z)
            if a is None and b is None:
                return "-"
            s = (f"{a[1]}%" if a else "?") + " -> " + (f"{b[1]}%" if b else "?")
            return s + (f" ({b[2]}%)" if b and b[2] >= 5 else "")
        lines.append(f"| {name} | {cell('head')} | {cell('body')} | {cell('limb')} | {v.get('on', ({}, '-'))[1]} |")
    return lines + [""]


# The arrows, the dice and the damage: the game's notes in bench.log (`arrow shot`, `arrow hit`, `damage`...), their
# times from events.jsonl (since 2026-10-13 each note is a `note` row there too). Older runs lack the newer notes and
# the `by` of the older ones: all of it is optional
GRAVITY = 9.8e-4  # cm/ms^2: the arrows fall by it (DynamicWorld::gravity)
ZONE_DAMAGE = {"head": 2.0, "body": 1.0, "limb": 0.6}  # Archery::zoneDamage
MIN_DAMAGE = 5  # G2: a hit does at least that much
HERO = "PC_HERO"  # who shot when a line does not say: before the notes had `by`, only the hero aimed by hand
# whom the shooters would fight: hit points, protection against points
VICTIMS = (("hero ch1", 160, 20), ("hero ch3", 400, 50), ("hero ch4", 520, 100), ("Bandit_M", 160, 35),
           ("paladin", 520, 100))
# the zone scales the damage after the armour since 2026-10-13 (the game); before it, the weapon
RULES = (("zone after armour", "after"), ("zone before armour", "before"))
Z95 = 1.959964
NUM = r"(-?\d+(?:\.\d+)?)"
RE_SHOT = re.compile(rf"arrow shot: from {NUM} {NUM} {NUM} dir {NUM} {NUM} {NUM} speed {NUM} draw {NUM}(?:, by (\S+))?")
RE_HIT = re.compile(rf"arrow hit (\S+): (\w+), hp {NUM} -> {NUM}, draw {NUM}, flew {NUM} cm(?:, at {NUM} {NUM} {NUM})?"
                    r"(?:, by (\S+))?")
RE_MISSED = re.compile(rf"arrow missed: .*?flew {NUM} cm(?:, by (\S+))?$")
RE_DICE_SHOT = re.compile(r"dice shot: by (\S+) at (\S+)")
RE_DICE = re.compile(rf"arrow dice (\S+): dist {NUM} path {NUM} talent {NUM} chance {NUM} roll {NUM}, (hit|miss)"
                     r"(?:, by (\S+))?")
RE_DAMAGE = re.compile(rf"damage (\S+): raw {NUM} zone (\S+) k {NUM} prot {NUM} net {NUM} hit (\d)(?:, hp {NUM})?"
                       r"(?:, (\w+))?(?:, by (\S+))?")
RE_FIGHT = re.compile(rf"fight over: (\S+) down after {NUM}")
RE_AIM_ZONE = re.compile(rf"aim zone {NUM} {NUM} {NUM}")
RE_AIMAT = re.compile(r"aimat (\S+)(?: (head|body|limb))?")
RE_STATS = re.compile(rf"stats str {NUM} dex {NUM} bow {NUM} cbow {NUM}.*? sway {NUM} deg.*? cone {NUM} deg")
RE_ZONES = re.compile(rf"zones (\S+) at {NUM} {NUM} {NUM}:")
RE_CAPSULE = re.compile(rf"\s+(head|body|limb) \d+-\d+: {NUM} {NUM} {NUM} - {NUM} {NUM} {NUM} r {NUM}")


def add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def mul(a, k):
    return (a[0] * k, a[1] * k, a[2] * k)


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def mean3(ps):
    return tuple(sum(p[c] for p in ps) / len(ps) for c in range(3))


def mean(v):
    return sum(v) / len(v) if v else math.nan


def lround(v):
    """as std::lround: halves away from zero"""
    return int(math.floor(v + 0.5)) if v >= 0 else -int(math.floor(-v + 0.5))


def f2(v, nd=2):
    return "-" if v is None or (isinstance(v, float) and math.isnan(v)) else f"{v:.{nd}f}"


def clean(v):
    """for report.json: NaN is not JSON"""
    if isinstance(v, float) and math.isnan(v):
        return None
    if isinstance(v, dict):
        return {k: clean(x) for k, x in v.items()}
    if isinstance(v, (list, tuple)):
        return [clean(x) for x in v]
    return v


def fields(body):
    """`at 1 2 3, err 0.5 -0.2 deg, settled, by SYM` -> {"at": [1, 2, 3], "err": [0.5, -0.2], "deg": [],
    "settled": [], "by": "SYM"}: a word and the numbers after it"""
    out, key, toks = {}, None, body.replace(",", " ").split()
    i = 0
    while i < len(toks):
        if toks[i] == "by" and i + 1 < len(toks):
            out["by"] = toks[i + 1]
            i += 2
            continue
        v = num(toks[i])
        if math.isnan(v):
            key = toks[i]
            out.setdefault(key, [])
        elif key is not None:
            out[key].append(v)
        i += 1
    return out


def note_times(out):
    """the game's notes in events.jsonl, in order: [(text, game ms)]; none in the runs before them"""
    path = os.path.join(out, "events.jsonl")
    notes = []
    if not os.path.exists(path):
        return notes
    with open(path, errors="replace") as f:
        for line in f:
            try:
                ev = json.loads(line)
            except ValueError:
                continue
            if ev.get("kind") == "note" and "text" in ev:
                notes.append((ev["text"], ev.get("t")))
    return notes


def combat(out, run=0):
    """the shots of a run from bench.log: the hand's arrows (where from, which way, the point meant, where they went),
    the dice's arrows, the dice rolls, the damage, the fights over, the hit zones `zones` dumped. A note gets its time
    from events.jsonl (the same text in the same order). A shot belongs to the segment it was shot in ("-": none)"""
    data = {"shots": [], "rolls": [], "damage": [], "fights": [], "dumps": []}
    path = os.path.join(out, "bench.log")
    if not os.path.exists(path):
        return data
    with open(path, errors="replace") as f:  # names of the game's npcs come in its codepage
        lines = [line.rstrip("\n") for line in f]
    notes, k = note_times(out), 0
    shots, rolls = data["shots"], data["rolls"]
    seg, dump, stats = "-", None, None
    target, zone, aimed = None, None, None  # the hero's `aimat` npc and zone, the last `aim zone`
    aims = {}  # an npc's last `arrow aim`, until its arrow leaves
    flying = defaultdict(deque)  # the arrows of each shooter in the air: they come down in the order shot
    for i, line in enumerate(lines):
        t = None
        for j in range(k, min(k + 64, len(notes))):
            if notes[j][0] == line:
                t, k = notes[j][1], j + 1
                break
        m = RE_CAPSULE.match(line)
        if dump is not None and m:
            g = [float(v) for v in m.groups()[1:]]
            dump["caps"].append((m.group(1), tuple(g[0:3]), tuple(g[3:6]), g[6]))
            continue
        dump = None
        s = line.strip()
        base = {"run": run, "seg": seg, "t": t, "line": i}
        if s.startswith("rec stop"):
            seg = "-"
        elif s.startswith("rec "):
            seg = s.split()[1]
        elif s.startswith("arrow shot"):
            m = RE_SHOT.match(s)
            if not m:
                continue
            g = [float(v) for v in m.groups()[:8]]
            by = m.group(9) or HERO
            x = dict(base, kind="precise", by=by, origin=tuple(g[0:3]), dir=tuple(g[3:6]), speed=g[6], draw=g[7],
                     aim=None, err=None, info=aims.pop(by, None), target=None, zone=None, stats=None, result=None,
                     victim=None, hit_line=None, damage=None)
            if x["info"] is not None and len(x["info"].get("true", [])) == 3:
                x["aim"] = tuple(x["info"]["true"])
                if len(x["info"].get("err", [])) == 2:
                    x["err"] = tuple(x["info"]["err"])
            elif by == HERO:
                x["aim"], aimed = aimed, None
            if by == HERO:
                x["target"], x["zone"], x["stats"] = target, zone, stats
            flying[by].append(len(shots))
            shots.append(x)
        elif s.startswith("arrow aim"):
            f = fields(s.split(":", 1)[-1])
            aims[f.get("by", HERO)] = f
        elif s.startswith("arrow hit"):
            m = RE_HIT.match(s)
            by = (m.group(10) or HERO) if m else HERO
            if m and flying[by]:
                shots[flying[by].popleft()].update(result=m.group(2), victim=m.group(1), hit_line=i)
        elif s.startswith("arrow missed"):
            m = RE_MISSED.match(s)
            by = (m.group(2) or HERO) if m else HERO
            if flying[by]:
                shots[flying[by].popleft()].update(result="miss", hit_line=i)
        elif s.startswith("dice shot"):
            m = RE_DICE_SHOT.match(s)
            if m:
                shots.append(dict(base, kind="dice", by=m.group(1), target=m.group(2), victim=None, draw=None, aim=None,
                                  result=None, damage=None))
        elif s.startswith("arrow dice"):
            m = RE_DICE.match(s)
            if m:
                g = [float(v) for v in m.groups()[1:6]]
                rolls.append(dict(base, victim=m.group(1), dist=g[0], path=g[1], talent=g[2], chance=g[3], roll=g[4],
                                  hit=m.group(7) == "hit", by=m.group(8) or "?", damage=None))
        elif s.startswith("damage "):
            m = RE_DAMAGE.match(s)
            if m:
                d = dict(base, victim=m.group(1), raw=int(m.group(2)), zone=m.group(3), k=float(m.group(4)),
                         prot=int(m.group(5)), net=int(m.group(6)), hit=m.group(7) == "1", kind=m.group(9) or "?",
                         by=m.group(10) or "?", roll=None)
                if d["kind"] == "dice":
                    # its dice: the roll just before, of the same shooter at the same victim
                    for r in reversed(rolls):
                        if r["by"] == d["by"] and r["victim"] == d["victim"]:
                            if r["damage"] is None:
                                r["damage"], d["roll"] = d, r
                            break
                data["damage"].append(d)
        elif s.startswith("fight over"):
            m = RE_FIGHT.match(s)
            if m:
                data["fights"].append(dict(base, sym=m.group(1), ms=float(m.group(2))))
        elif s.startswith("aim zone"):
            m = RE_AIM_ZONE.match(s)
            if m:
                aimed = tuple(float(v) for v in m.groups())
        elif s.startswith("aimat"):
            m = RE_AIMAT.match(s)
            if m:
                npc = m.group(1) not in ("off", "ahead") and not re.match(r"-?\d", m.group(1))  # not at a point
                target, zone = (m.group(1), m.group(2) or "body") if npc else (None, None)
        elif s.startswith("stats "):
            m = RE_STATS.match(s)
            if m:
                g = [float(v) for v in m.groups()]
                stats = (g[2], g[4], g[5])  # bow skill, sway, cone
        elif s.startswith("zones "):
            m = RE_ZONES.match(s)
            if m:
                dump = dict(run=run, line=i, sym=m.group(1), caps=[])
                data["dumps"].append(dump)
    # the damage to the arrow it came from: a hand's arrow by its hit next to it, a dice's arrow the last one of that
    # shooter before; a blow (melee, a spell) is a shot of its own
    mine = defaultdict(list)
    for x in shots:
        mine[x["by"]].append(x)
    for d in data["damage"]:
        if d["kind"] == "precise":
            cand = [x for x in mine[d["by"]] if x["kind"] == "precise" and x["damage"] is None
                    and x["victim"] == d["victim"] and x["result"] in ZONE_DAMAGE]
            if cand:
                min(cand, key=lambda x: abs(x["hit_line"] - d["line"]))["damage"] = d
        elif d["kind"] == "dice":
            cand = [x for x in mine[d["by"]] if x["kind"] == "dice" and x["damage"] is None and x["line"] < d["line"]]
            cand = [x for x in cand if x["target"] == d["victim"]] or cand
            if cand:
                cand[-1]["damage"] = d
        else:
            shots.append(dict(run=run, seg=d["seg"], t=d["t"], line=d["line"], kind=d["kind"], by=d["by"],
                              target=d["victim"], victim=d["victim"], draw=None, aim=None, result=None, damage=d))
    shots.sort(key=lambda x: x["line"])
    # whom a shot was at: the one it hit, the dice's target, the hero's `aimat`; else whom that shooter hit most
    most = defaultdict(Counter)
    for x in shots:
        if x["victim"] is None and x["damage"] is not None:
            x["victim"] = x["damage"]["victim"]
        if x["victim"] is None:
            x["victim"] = x["target"]
        if x["victim"] is not None:
            most[(x["seg"], x["by"])][x["victim"]] += 1
    for x in shots:
        if x["victim"] is None:
            c = most[(x["seg"], x["by"])]
            x["victim"] = c.most_common(1)[0][0] if c else "?"
    # the gap since that shooter's shot before, in the same segment
    last = {}
    for x in shots:
        key = (x["seg"], x["by"])
        x["gap"] = x["t"] - last[key] if x["t"] is not None and key in last else None
        if x["t"] is not None:
            last[key] = x["t"]
    return data


def axes(f):
    """right and up square to the line of sight f (unit) as the shooter sees them: 90 degrees to the left of a
    facing (x, z) is (-z, x), the world is left-handed"""
    l = math.hypot(f[0], f[2])
    right = (f[2] / l, 0.0, -f[0] / l) if l > 1e-6 else (1.0, 0.0, 0.0)
    return right, cross(f, right)


def closest(x, q):
    """the arrow flown as the game flies it (from where it left, straight at its speed, falling by GRAVITY) to where
    it comes closest to q: that point minus q"""
    o, v = x["origin"], mul(x["dir"], x["speed"])

    def miss(t):
        return (o[0] + v[0] * t - q[0], o[1] + v[1] * t - 0.5 * GRAVITY * t * t - q[1], o[2] + v[2] * t - q[2])

    far = 2.0 * length(sub(q, o)) / max(x["speed"], 1e-3) + 1.0
    ts = [far * i / 64 for i in range(65)]
    i = min(range(65), key=lambda j: dot(miss(ts[j]), miss(ts[j])))
    lo, hi = ts[max(0, i - 1)], ts[min(64, i + 1)]
    for _ in range(60):  # golden section
        a, b = hi - 0.618034 * (hi - lo), lo + 0.618034 * (hi - lo)
        if dot(miss(a), miss(a)) < dot(miss(b), miss(b)):
            hi = b
        else:
            lo = a
    return miss((lo + hi) / 2)


def miss_angles(x):
    """how far an arrow passed the point meant, degrees as the shooter sees it (+ right, + up), in the plane square to
    the line from where it left to that point; that line's length, cm"""
    f = sub(x["aim"], x["origin"])
    dist = length(f)
    right, up = axes(mul(f, 1.0 / dist))
    m = closest(x, x["aim"])
    return math.degrees(math.atan2(dot(m, right), dist)), math.degrees(math.atan2(dot(m, up), dist)), dist


def gamma_p(a, x):
    """the regularized lower incomplete gamma function P(a, x) (Numerical Recipes: a series below a + 1, a continued
    fraction above)"""
    if x <= 0:
        return 0.0
    front = math.exp(-x + a * math.log(x) - math.lgamma(a))
    if x < a + 1:
        term = total = 1.0 / a
        ap = a
        for _ in range(1000):
            ap += 1
            term *= x / ap
            total += term
            if abs(term) < abs(total) * 1e-14:
                break
        return total * front
    b, c = x + 1 - a, 1e300
    d = 1.0 / b
    h = d
    for i in range(1, 1000):
        an = -i * (i - a)
        b += 2
        d = an * d + b
        d = 1e-300 if abs(d) < 1e-300 else d
        c = b + an / c
        c = 1e-300 if abs(c) < 1e-300 else c
        d = 1.0 / d
        h *= d * c
        if abs(d * c - 1) < 1e-14:
            break
    return 1.0 - front * h


def chi2_quantile(p, k):
    """x where the chi-square of k degrees of freedom reaches p, by bisection"""
    lo, hi = 0.0, k + 10.0
    while gamma_p(k / 2.0, hi / 2.0) < p:
        hi *= 2
    for _ in range(100):
        mid = (lo + hi) / 2
        if gamma_p(k / 2.0, mid / 2.0) < p:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2


def quantile(v, q):
    """of sorted values, between the two nearest"""
    if not v:
        return math.nan
    p = q * (len(v) - 1)
    i = int(p)
    return v[i] if i + 1 >= len(v) else v[i] + (v[i + 1] - v[i]) * (p - i)


def spread_of(xs):
    """one axis of the misses: the bias (the mean), sigma as the RMS about it with its 95% CI (chi-square), sigma as
    MAD / 0.6745 (a few wild ones do not move it)"""
    n = len(xs)
    out = {"n": n, "bias": mean(xs), "sigma": math.nan, "ci": (math.nan, math.nan), "mad": math.nan}
    if n > 1:
        sd = math.sqrt(sum((x - out["bias"]) ** 2 for x in xs) / (n - 1))
        out["sigma"] = sd
        out["ci"] = (sd * math.sqrt((n - 1) / chi2_quantile(0.975, n - 1)),
                     sd * math.sqrt((n - 1) / chi2_quantile(0.025, n - 1)))
        med = quantile(sorted(xs), 0.5)
        out["mad"] = quantile(sorted(abs(x - med) for x in xs), 0.5) / 0.6745
    return out


def wilson(k, n):
    """the 95% interval of a share k of n (Wilson)"""
    if n == 0:
        return (math.nan, math.nan)
    p, z2 = k / n, Z95 * Z95
    c = (p + z2 / (2 * n)) / (1 + z2 / n)
    h = Z95 * math.sqrt(p * (1 - p) / n + z2 / (4 * n * n)) / (1 + z2 / n)
    return (max(0.0, c - h), min(1.0, c + h))


def per_minute(xs):
    """how many a minute: by the gaps between them (note times), in each run"""
    gaps, span = 0, 0.0
    times = defaultdict(list)
    for x in xs:
        if x["t"] is not None:
            times[x["run"]].append(x["t"])
    for ts in times.values():
        if len(ts) > 1:
            gaps += len(ts) - 1
            span += max(ts) - min(ts)
    return gaps * 60000.0 / span if span > 0 else None


def sphere_hit(ro, rd, c, r):
    oc = sub(ro, c)
    b = dot(oc, rd)
    cc = dot(oc, oc) - r * r
    if cc <= 0:
        return 0.0
    h = b * b - cc
    if h < 0:
        return -1.0
    t = -b - math.sqrt(h)
    return t if t >= 0 else -1.0


def capsule_hit(ro, rd, pa, pb, r):
    """where a ray (rd: unit) enters a capsule, 0 from inside, -1: past it (capsuleHit of HitZones)"""
    ba = sub(pb, pa)
    baba = dot(ba, ba)
    best = -1.0
    if baba > 1e-4:
        oa = sub(ro, pa)
        k = min(max(dot(oa, ba) / baba, 0.0), 1.0)
        if length(sub(oa, mul(ba, k))) <= r:
            return 0.0
        bard, baoa = dot(ba, rd), dot(ba, oa)
        a = baba - bard * bard
        if a > 1e-6:
            b = baba * dot(rd, oa) - baoa * bard
            c = baba * dot(oa, oa) - baoa * baoa - r * r * baba
            h = b * b - a * c
            if h < 0:
                return -1.0  # misses the endless cylinder: the caps lie inside it
            t = (-b - math.sqrt(h)) / a
            if 0 < baoa + t * bard < baba and t >= 0:
                best = t
        t = sphere_hit(ro, rd, pb, r)
        if t >= 0 and (best < 0 or t < best):
            best = t
    t = sphere_hit(ro, rd, pa, r)
    if t >= 0 and (best < 0 or t < best):
        best = t
    return best


def zone_centres(caps):
    """the middle of each zone as the game takes it (HitZones::center): the head and the trunk the capsules' middles
    by their weight, a limb the biggest one"""
    out = {}
    for z in ZONE_DAMAGE:
        acc, wsum, best = (0.0, 0.0, 0.0), 0.0, None
        for zone, a, b, r in caps:
            if zone != z:
                continue
            w = (length(sub(b, a)) + 2 * r) * r * r
            mid = mul(add(a, b), 0.5)
            if z == "limb":
                best = (w, mid) if best is None or w > best[0] else best
            else:
                acc, wsum = add(acc, mul(mid, w)), wsum + w
        if best is not None:
            out[z] = best[1]
        elif wsum > 0:
            out[z] = mul(acc, 1.0 / wsum)
    return out


def raster(caps, s, q):
    """the target as the shooter at s sees it, on the plane through q square to the line of sight: 1 cm cells (u right,
    v up from q), each the zone of the capsule a ray from s through it enters first (HitZones::rayTest)"""
    f = sub(q, s)
    dist = length(f)
    f = mul(f, 1.0 / dist)
    right, up = axes(f)

    def seen(p):  # where p is seen on the plane, how far it is along the line of sight
        w = sub(p, s)
        depth = dot(w, f)
        return dot(w, right) * dist / depth, dot(w, up) * dist / depth, depth

    cells = defaultdict(list)
    for idx, (zone, a, b, r) in enumerate(caps):
        (ua, va, da), (ub, vb, db) = seen(a), seen(b)
        if min(da, db) <= r + 1:
            continue  # at the shooter or behind it
        rr = r * dist / (min(da, db) - r) + 1  # it looks at most this big
        for iu in range(math.floor(min(ua, ub) - rr), math.ceil(max(ua, ub) + rr) + 1):
            for iv in range(math.floor(min(va, vb) - rr), math.ceil(max(va, vb) + rr) + 1):
                cells[(iu, iv)].append(idx)
    out = {}
    for (iu, iv), idxs in cells.items():
        p = add(q, add(mul(right, iu), mul(up, iv)))
        rd = sub(p, s)
        rd = mul(rd, 1.0 / length(rd))
        best, zone = -1.0, None
        for idx in idxs:
            z, a, b, r = caps[idx]
            h = capsule_hit(s, rd, a, b, r)
            if h >= 0 and (best < 0 or h < best):
                best, zone = h, z
        if zone is not None:
            out[(iu, iv)] = zone
    return out


def gauss_cells(cells, mu, sigma):
    """P of each zone: a 2D Gaussian (independent axes, mean and sigma in cm on the plane) over the cells"""
    def part(c, m, sd):
        if sd <= 0:
            return 1.0 if abs(c - m) <= 0.5 else 0.0
        k = 1.0 / (sd * math.sqrt(2))
        return 0.5 * (math.erf((c + 0.5 - m) * k) - math.erf((c - 0.5 - m) * k))
    pu, pv, out = {}, {}, defaultdict(float)
    for (iu, iv), z in cells.items():
        if iu not in pu:
            pu[iu] = part(iu, mu[0], sigma[0])
        if iv not in pv:
            pv[iv] = part(iv, mu[1], sigma[1])
        out[z] += pu[iu] * pv[iv]
    return out


def predict(aimed, yaw, pitch, dumps):
    """P(head), P(body), P(limb) of a shot: the target's capsules of its last `zones` dump, seen from where the shots
    left, under a 2D Gaussian of the misses (each axis its bias and sigma, as angles times the distance). The dump is
    moved to where the shots aimed when the target stood elsewhere (the hero aims at the middle of its `aimat` zone;
    an npc's point meant is taken for the trunk's). None without a dump"""
    if len(aimed) < 2 or math.isnan(yaw["sigma"]) or math.isnan(pitch["sigma"]):
        return None
    who = Counter(x["target"] for x in aimed if x["target"]) or \
        Counter(x["victim"] for x in aimed if x["result"] in ZONE_DAMAGE)
    if not who:
        return None
    sym = who.most_common(1)[0][0]
    runs = {x["run"] for x in aimed}
    mine = [d for d in dumps if d["sym"] == sym and d["run"] in runs and d["caps"]]
    if not mine:
        return None
    run = aimed[0]["run"] if any(d["run"] == aimed[0]["run"] for d in mine) else mine[0]["run"]
    mine = [d for d in mine if d["run"] == run]
    end = max(x["line"] for x in aimed if x["run"] == run)
    before = [d for d in mine if d["line"] < end]
    dump = before[-1] if before else mine[0]
    s, q = mean3([x["origin"] for x in aimed]), mean3([x["aim"] for x in aimed])
    dist = length(sub(q, s))
    centres = zone_centres(dump["caps"])
    zone = Counter(x["zone"] for x in aimed if x.get("zone")).most_common(1)
    if zone and zone[0][0] in centres:
        shift = sub(q, centres[zone[0][0]])
    else:
        near = min(centres.values(), key=lambda c: length(sub(c, q)))
        shift = (0.0, 0.0, 0.0) if length(sub(near, q)) < 40 else sub(q, centres.get("body", near))
    caps = [(z, add(a, shift), add(b, shift), r) for z, a, b, r in dump["caps"]]
    mu = (dist * math.tan(math.radians(yaw["bias"])), dist * math.tan(math.radians(pitch["bias"])))
    sd = (dist * math.radians(yaw["sigma"]), dist * math.radians(pitch["sigma"]))
    p = gauss_cells(raster(caps, s, q), mu, sd)
    return {"target": sym, "dist_m": dist / 100, "moved_cm": length(shift), "p": {z: p.get(z, 0.0) for z in ZONE_DAMAGE}}


def archery(data):
    """the hand's arrows per segment and shooter: the miss at the point meant, hits by zone; the hits the misses'
    spread predicts on the target's hit zones (when `zones` dumped them)"""
    groups = defaultdict(list)
    for x in data["shots"]:
        if x["kind"] == "precise":
            groups[(x["seg"], x["by"])].append(x)
    rows, js, pred, pjs = [], [], [], []

    def axis_cells(st):
        if st["n"] == 0:
            return ["-"] * 3
        ci = "-" if math.isnan(st["sigma"]) else f"{st['sigma']:.2f} ({st['ci'][0]:.2f}-{st['ci'][1]:.2f})"
        return [f2(st["bias"]), ci, f2(st["mad"])]

    for (seg, by), xs in groups.items():
        aimed = [x for x in xs if x["aim"] is not None and length(sub(x["aim"], x["origin"])) > 1]
        miss = [miss_angles(x) for x in aimed]
        yaw, pitch = spread_of([m[0] for m in miss]), spread_of([m[1] for m in miss])
        done = [x["result"] for x in xs if x["result"] is not None]
        hits = {z: done.count(z) for z in ("head", "body", "limb", "miss")}
        rate = per_minute(xs)
        dist = mean([m[2] for m in miss]) / 100
        infos = [x["info"] for x in xs if x["info"]]
        if infos:
            # an npc: what its aim says of itself
            sig = [i["sigma"][0] for i in infos if i.get("sigma")]
            states = Counter(w for i in infos for w in ("settled", "waited", "flinch") if w in i)
            skill = "; ".join(([f"sigma {mean(sig):.2f}"] if sig else []) + [f"{w} {n}" for w, n in states.items()])
        else:
            st = Counter(x["stats"] for x in xs if x["stats"]).most_common(1)
            skill = f"bow {st[0][0][0]:.0f}: sway {st[0][0][1]:.2f}, cone {st[0][0][2]:.2f}" if st else "-"

        def zc(z):
            lo, hi = wilson(hits[z], len(done))
            return f"{hits[z]} ({lo * 100:.0f}-{hi * 100:.0f}%)" if done else "-"
        rows.append(f"| {seg} | {by} | {len(xs)} | {f2(rate, 1)} | {len(miss)} | {f2(dist, 1)} | "
                    + " | ".join(axis_cells(yaw) + axis_cells(pitch)) + " | "
                    + " | ".join(zc(z) for z in ("head", "body", "limb", "miss")) + f" | {skill} |")
        row = {"segment": seg, "shooter": by, "shots": len(xs), "per_min": rate, "n": len(miss), "dist_m": dist,
               "yaw": yaw, "pitch": pitch, "skill": skill,
               "hits": {z: [hits[z], *wilson(hits[z], len(done))] for z in hits}, "of": len(done)}
        # an npc's own deliberate offset taken off: what its body and the bow threw the arrow off by
        phys = [(m[0] - x["err"][0], m[1] - x["err"][1]) for m, x in zip(miss, aimed) if x["err"] is not None]
        if phys:
            py, pp = spread_of([p[0] for p in phys]), spread_of([p[1] for p in phys])
            rows.append(f"| {seg} | {by} - err | | | {len(phys)} | | " + " | ".join(axis_cells(py) + axis_cells(pp))
                        + " | | | | | |")
            row["physical"] = {"n": len(phys), "yaw": py, "pitch": pp}
        js.append(row)
        p = predict(aimed, yaw, pitch, data["dumps"])
        if p is not None:
            n = len(done)
            hit = sum(hits[z] for z in ZONE_DAMAGE)
            cells = [f"{p['p'][z] * 100:.0f}% / {hits[z] / n * 100:.0f}% ({hits[z]})" if n else f"{p['p'][z] * 100:.0f}% / -"
                     for z in ZONE_DAMAGE]
            anyp = sum(p["p"].values())
            cells.append(f"{anyp * 100:.0f}% / {hit / n * 100:.0f}% ({hit})" if n else f"{anyp * 100:.0f}% / -")
            pred.append(f"| {seg} | {by} | {p['target']} | {p['dist_m']:.1f} | {p['moved_cm']:.0f} | " + " | ".join(cells) + " |")
            pjs.append({"segment": seg, "shooter": by, **p, "any": anyp,
                        "counted": {z: hits[z] for z in ZONE_DAMAGE}, "of": n})
    if not rows:
        return [], {}
    lines = ["## Archery accuracy", "",
             "Each arrow flown from `arrow shot` (with the fall) to where it passes closest to the point meant (the "
             "hero's: the last `aim zone`; an npc's: `true` of its `arrow aim`); the miss in degrees as the shooter "
             "sees it, + right / + up. σ: RMS about the bias (95% CI), and MAD / 0.6745. `- err`: an npc's own "
             "offset taken off. Hits: count (Wilson 95%).", "",
             "| segment | shooter | shots | /min | n | dist m | yaw bias deg | yaw σ deg (95% CI) | yaw σ MAD | "
             "pitch bias deg | pitch σ deg (95% CI) | pitch σ MAD | head | body | limb | miss | skill |",
             "|" + "---|" * 17] + rows + [""]
    if pred:
        lines += ["### Predicted hits from σ", "",
                  "The target's capsules (`zones` dump; moved to the point meant if it stood elsewhere) seen from the "
                  "shooter in 1 cm cells, under a Gaussian of the bias and σ above: predicted / counted.", "",
                  "| segment | shooter | target | dist m | dump moved cm | head | body | limb | any |",
                  "|" + "---|" * 9] + pred + [""]
    return lines, {"archery": clean(js), **({"archery_predicted": clean(pjs)} if pjs else {})}


def g2_chance(h, d):
    """vanilla G2's chance (%) by the straight distance"""
    if d <= 1500:
        return math.trunc(100 + (h - 100) * d / 1500)
    return max(0, math.trunc(h - h * d / 4500))


def og_chance(h, path):
    """OpenGothic's (DamageCalculator::rangeDamage), by the arrow's path"""
    if path < 1500:
        return 100 + (h - 100) * path / 1500
    if path < 4500:
        return h * (1 - (path - 1500) / 3000)
    return 0.0


def dice(data):
    """the dice's arrows per segment and shooter: the hits the logged chances expect against those rolled; the same
    rolls under the chance recomputed as OpenGothic and as vanilla G2 take it"""
    groups = defaultdict(lambda: [0, []])
    for x in data["shots"]:
        if x["kind"] == "dice":
            groups[(x["seg"], x["by"])][0] += 1
    for r in data["rolls"]:
        groups[(r["seg"], r["by"])][1].append(r)
    rows, js = [], []
    for (seg, by), (shots, rs) in groups.items():
        ps = [min(max(r["chance"] / 100, 0.0), 1.0) for r in rs]
        exp, sd = sum(ps), math.sqrt(sum(p * (1 - p) for p in ps))
        hits = sum(r["hit"] for r in rs)
        z = (hits - exp) / sd if sd > 0 else math.nan
        og = [og_chance(r["talent"], r["path"]) for r in rs]
        og_hit = [c > r["roll"] and r["path"] <= 4500 for c, r in zip(og, rs)]
        g2 = [g2_chance(r["talent"], r["dist"]) for r in rs]
        g2_hit = [r["roll"] < c for c, r in zip(g2, rs)]
        flips = (sum(a != r["hit"] for a, r in zip(og_hit, rs)), sum(a != r["hit"] for a, r in zip(g2_hit, rs)))
        dist = mean([r["dist"] for r in rs]) / 100
        detour = mean([r["path"] - r["dist"] for r in rs])
        rows.append(f"| {seg} | {by} | {shots} | {len(rs)} | {f2(dist, 1)} | {f2(mean([r['chance'] for r in rs]), 1)} | "
                    f"{exp:.1f} ± {sd:.1f} | {hits} | {f2(z)} | "
                    f"{f2(mean(og), 1)}, {sum(og_hit)}, {flips[0]} | {f2(mean(g2), 1)}, {sum(g2_hit)}, {flips[1]} | "
                    f"{f2(detour, 0)} |")
        js.append({"segment": seg, "shooter": by, "shots": shots, "rolls": len(rs), "dist_m": dist,
                   "chance": mean([r["chance"] for r in rs]), "expected": exp, "sd": sd, "hits": hits, "z": z,
                   "opengothic": {"chance": mean(og), "hits": sum(og_hit), "flips": flips[0]},
                   "g2": {"chance": mean(g2), "hits": sum(g2_hit), "flips": flips[1]}, "path_minus_dist": detour})
    if not rows:
        return [], {}
    return ["## Dice", "",
            "`arrow dice` rolls: expected hits Σp ± sqrt(Σp(1-p)) by the logged chance, the hits, z. Recomputed from "
            "each line, with its roll: OpenGothic (by the path: 100+(H-100)·p/1500, then H·(1-(p-1500)/3000), 0 past "
            "45 m) and vanilla G2 (by the straight distance, truncated: 100+(H-100)·d/1500, then H-H·d/4500): mean "
            "chance %, hits, outcomes that flip against the log.", "",
            "| segment | shooter | dice shots | rolls | dist m | chance % | expected hits | hits | z | "
            "OpenGothic again: chance, hits, flips | vanilla G2: chance, hits, flips | path - dist cm |",
            "|" + "---|" * 12] + rows + [""], {"dice": clean(js)}


def hit_damage(v, prot, rule):
    """what a shot (t, raw, k, hit, draw, zone) does to a victim of that protection: as the game takes it (the draw
    scales the weapon, the armour takes its part, the zone scales the rest) or as it did until 2026-10-13 (the draw
    and the zone scaled the weapon, the armour after); G2's floor"""
    _, raw, k, hit, draw, zone = v
    if not hit:
        return 0
    if rule == "before":
        net = max(lround(raw * k) - prot, 0)
    else:
        draw = min(max(draw, 0.0), 1.0) if draw is not None else None
        zf = k / draw if draw else ZONE_DAMAGE.get(zone, 1.0)
        dr = draw if draw else (k / zf if zf else 1.0)
        net = lround(max(lround(raw * dr) - prot, 0) * zf)
    return max(net, MIN_DAMAGE)


def to_kill(seq, hp, prot, rule):
    """from each shot of the sequence as the first one, on and round it (after the last one the mean gap): the ms and
    the shots until hp is gone. Every start point once: the limit of a bootstrap over them. [] when it does no harm"""
    nets = [hit_damage(v, prot, rule) for v in seq]
    n = len(seq)
    if not any(nets):
        return []
    times = [v[0] for v in seq]
    timed = n > 1 and all(t is not None for t in times)
    period = (times[-1] - times[0]) * n / (n - 1) if timed else 0
    out = []
    for j in range(n):
        left, m = hp, j
        while True:
            left -= nets[m % n]
            if left <= 0:
                break
            m += 1
        out.append((times[m % n] + (m // n) * period - times[j] if timed else None, m - j + 1))
    return out


def ttk_cell(samples):
    if not samples:
        return "-", None
    shots = sorted(s for _, s in samples)
    ms = sorted(m for m, _ in samples if m is not None)
    out = {"shots": mean(shots), "shots_5": quantile(shots, 0.05), "shots_95": quantile(shots, 0.95)}
    if len(ms) == len(samples):
        out.update(s=mean(ms) / 1000, s_5=quantile(ms, 0.05) / 1000, s_95=quantile(ms, 0.95) / 1000)
        return f"{out['s']:.1f} s ({out['s_5']:.1f}-{out['s_95']:.1f}), {out['shots']:.1f} shots", out
    return f"{out['shots']:.1f} shots ({out['shots_5']:.0f}-{out['shots_95']:.0f})", out


def damage(data):
    """per segment, shooter and victim: the shots, the hits by zone, the damage, the cadence, the fights over; the
    time to kill the shooter's sequence of shots gives against the victims of VICTIMS"""
    if not data["damage"]:
        return [], {}
    groups = defaultdict(list)
    for x in data["shots"]:
        groups[(x["seg"], x["by"], x["victim"])].append(x)
    rows, js, ttk, tjs, seen = [], [], [], [], set()
    for (seg, by, victim), xs in groups.items():
        cnt = Counter()
        for x in xs:
            d = x["damage"]
            if d is not None and d["hit"]:
                cnt[d["zone"] if d["zone"] in ZONE_DAMAGE else "hit"] += 1
            elif d is None and x["result"] in ZONE_DAMAGE:
                cnt[x["result"]] += 1
        hits = " ".join(f"{z} {cnt[z]}" for z in ("head", "body", "limb", "hit") if cnt[z]) or "-"
        nets = [x["damage"]["net"] for x in xs if x["damage"] is not None and x["damage"]["hit"]]
        gaps = [x["gap"] for x in xs if x["gap"] is not None]
        cad = mean(gaps)
        cad_sd = math.sqrt(sum((g - cad) ** 2 for g in gaps) / (len(gaps) - 1)) if len(gaps) > 1 else math.nan
        total = sum(x["damage"]["net"] for x in xs if x["damage"] is not None)
        dpm = total / len(xs) * 60000 / cad if gaps and cad > 0 else None
        downs = [f for f in data["fights"] if f["seg"] == seg and f["sym"] == victim]
        seen.update(id(f) for f in downs)
        kinds = "/".join(sorted({x["kind"] for x in xs}))
        down = ", ".join(f"{f['ms'] / 1000:.1f}" for f in downs) or "-"
        if len(downs) > 4:
            secs = sorted(f["ms"] / 1000 for f in downs)
            down = f"{len(secs)} fights: {mean(secs):.1f} ({secs[0]:.1f}-{secs[-1]:.1f})"
        rows.append(f"| {seg} | {by} | {victim} | {kinds} | {len(xs)} | {hits} | {f2(mean(nets), 1)} | {f2(dpm, 0)} | "
                    f"{f2(cad / 1000 if gaps else None)} ± {f2(cad_sd / 1000)} | {down} |")
        js.append({"segment": seg, "shooter": by, "victim": victim, "kinds": kinds, "shots": len(xs), "hits": dict(cnt),
                   "net_per_hit": mean(nets), "per_min": dpm, "cadence_s": cad / 1000 if gaps else None,
                   "cadence_sd_s": cad_sd / 1000, "down_after_s": [f["ms"] / 1000 for f in downs]})
        seqs = defaultdict(list)
        for x in xs:
            d = x["damage"]
            seqs[x["run"]].append((x["t"], d["raw"] if d else 0, d["k"] if d else 0.0, bool(d and d["hit"]),
                                   x["draw"] if x["kind"] == "precise" else None, d["zone"] if d else None))
        if not any(v[1] > 0 for seq in seqs.values() for v in seq):
            continue  # blows: no weapon damage to replay
        for name, rule in RULES:
            cells, out = [], {}
            for vname, hp, prot in VICTIMS:
                cell, o = ttk_cell([v for seq in seqs.values() for v in to_kill(seq, hp, prot, rule)])
                cells.append(cell)
                out[vname] = o
            ttk.append(f"| {seg} | {by} | {victim} | {name} | " + " | ".join(cells) + " |")
            tjs.append({"segment": seg, "shooter": by, "victim": victim, "rule": rule, "victims": out})
    lines = ["## Damage and fights", "",
             "Per shooter and victim: hits by zone (`hit`: a dice's), net damage per hit, damage a minute (per shot "
             "times the cadence), the gap between the shooter's shots (`arrow shot`, `dice shot`), s; `fight over` of "
             "the victim.", "",
             "| segment | shooter | victim | kind | shots | hits | net per hit | damage /min | cadence s | down after s |",
             "|" + "---|" * 10] + rows + [""]
    other = [f for f in data["fights"] if id(f) not in seen]
    if other:
        lines += ["Fights over: " + "; ".join(f"{f['sym']} down after {f['ms'] / 1000:.1f} s ({f['seg']})" for f in other), ""]
    if ttk:
        lines += ["### Time to kill", "",
                  "The shooter's shots replayed on these victims (hit points / protection) from each shot on, round the "
                  "sequence: mean (5-95%). Per hit max(round(raw·draw) - prot, 0)·zone as the game (zone after armour), "
                  "or max(round(raw·k) - prot, 0) as until 2026-10-13 (zone before armour); at least 5 a hit.", "",
                  "| segment | shooter | victim | rule | " + " | ".join(f"{n} {hp}/{p}" for n, hp, p in VICTIMS) + " |",
                  "|" + "---|" * (4 + len(VICTIMS))] + ttk + [""]
    return lines, {"damage": clean(js), **({"time_to_kill": clean(tjs)} if tjs else {}),
                   "fights": clean([{"segment": f["seg"], "down": f["sym"], "after_s": f["ms"] / 1000} for f in data["fights"]])}


def combat_report(datas):
    """the tables of the arrows, the dice and the damage of one run, or of several pooled by segment name"""
    data = {k: [v for d in datas for v in d[k]] for k in ("shots", "rolls", "damage", "fights", "dumps")}
    lines, summary = [], {}
    for part in (archery, dice, damage):
        more, js = part(data)
        lines += more
        summary.update(js)
    return lines, summary


def aggregate(outs):
    """the arrows, the dice and the damage of several runs (repeats of a scenario) pooled by segment name"""
    lines, summary = combat_report([combat(o, i) for i, o in enumerate(outs)])
    print(f"# Arrows of {len(outs)} runs: " + ", ".join(os.path.basename(os.path.normpath(o)) for o in outs))
    print()
    print("\n".join(lines) if lines else "no arrows, dice or damage")


def sheet(video, frames, path, cols=4, width=400):
    """tile the given frame numbers of a video into one image"""
    if not frames:
        return None
    expr = "+".join(f"eq(n\\,{f})" for f in frames)
    rows = math.ceil(len(frames) / cols)
    vf = f"select='{expr}',scale={width}:-1,tile={cols}x{rows}:padding=4:color=black"
    cmd = ["ffmpeg", "-loglevel", "error", "-y", "-i", video, "-vf", vf, "-frames:v", "1", "-fps_mode", "vfr", path]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"ffmpeg failed for {path}: {res.stderr.strip()[:300]}", file=sys.stderr)
        return None
    return path


def spread(a, b, n):
    if b - a + 1 <= n:
        return list(range(a, b + 1))
    return sorted({a + round(i * (b - a) / (n - 1)) for i in range(n)})


def report(out):
    segs = load(out)
    os.makedirs(os.path.join(out, "sheets"), exist_ok=True)
    lines = [f"# Bench report: {os.path.basename(out)}", ""]
    host_path = os.path.join(out, "host.txt")
    if os.path.exists(host_path):
        with open(host_path) as f:
            lines += [f"Host: {f.read().strip()}", ""]
    summary = {"segments": {}, "events": []}
    log_path = os.path.join(out, "game.log")
    if os.path.exists(log_path):
        with open(log_path, errors="replace") as log:
            for line in log:
                if line.startswith("[bench] run mode: "):
                    summary["run_mode"] = line.removeprefix("[bench] run mode: ").strip()
                elif line.startswith("[bench] game wall seconds: "):
                    summary["game_wall_seconds"] = int(line.removeprefix("[bench] game wall seconds: "))
    if "run_mode" in summary:
        lines += [f"Run: {summary['run_mode']}", ""]
    if "game_wall_seconds" in summary:
        lines += [f"Game wall time: {summary['game_wall_seconds']} s", ""]
    all_events = []
    table = []

    for seg, rows in segs.items():
        stats, events = analyze(rows)
        video = os.path.join(out, seg + ".mp4")
        has_video = os.path.exists(video)
        overview = None
        if has_video and rows:
            n = len(rows)
            overview = sheet(video, spread(0, n - 1, 16), os.path.join(out, "sheets", f"{seg}.png"))
        summary["segments"][seg] = {**stats, "video": video if has_video else None, "overview": overview,
                                    "events": len(events)}
        for ev in events:
            ev["seg"] = seg
        all_events += events

        counts = defaultdict(int)
        for ev in events:
            counts[ev["flag"]] += 1
        table.append((seg, stats, ", ".join(f"{k} {v}" for k, v in sorted(counts.items()))))

    lines.append("| segment | frames | speed cm/s | IK on % | legs hidden % | contact p90 / max cm | spread cm | limp cm | gaze on % (looks, why) | gaze err p90 deg | lean roll / pitch deg | anim pop p99 / max cm (switches: n, max) | aim err p90 / bend deg | lips corr | hits, peak deg, back s | gestures talk / listen % (started, over one, stopped) | blinks /min (in a pause, at a shift) | events |")
    lines.append("|" + "---|" * 18)
    for seg, st, ev in table:
        still = " (still)" if st["speed"] < 15 else ""
        p90 = st.get("contact_p90")
        mx = st.get("contact_max")
        contact = "-" if p90 is None else f"{p90:.1f} / {mx:.1f}"
        limp = "-" if "limp" not in st else f"{st['limp']:.1f}"
        gap = "-" if "spread" not in st else f"{st['spread']:.1f}"
        gz = "-" if "gaze_p90" not in st else f"{st['gaze_p90']:.1f}"
        gon = "-" if "gaze_on" not in st else \
            f"{st['gaze_on'] * 100:.0f} ({st['gaze_looks']}{', ' + ' '.join(st['gaze_src']) if st['gaze_src'] else ''})"
        ln = "-" if "lean_roll" not in st else \
            f"{st['lean_roll']:.1f} / {st['lean_pitch'][0]:.1f}..{st['lean_pitch'][1]:.1f}"
        an = "-" if "anim_p99" not in st else \
            f"{st['anim_p99']:.1f} / {st['anim_max']:.1f} ({st['anim_switches']}, {st['anim_switch_max']:.1f})"
        am = "-" if "aim_p90" not in st else f"{st['aim_p90']:.1f} / {st['aim_bend']:.0f}"
        lc = "-" if "lip_corr" not in st else f"{st['lip_corr']:.2f}"
        gl = "-" if st.get("ges_listen") is None else f"{st['ges_listen'] * 100:.0f}"
        gs = "-" if "ges_talk" not in st else \
            f"{st['ges_talk'] * 100:.0f} / {gl} ({st['ges_starts']}, {st['ges_cuts']}, {st['ges_stops']})" + \
            ("" if "ges_nods" not in st else f" nods {st['ges_nods']}")
        if "ges_lnods" in st:
            gs = ("" if gs == "-" else gs + "; ") + f"listening: nods {st['ges_lnods']}, eyes aside {st['ges_aside']:.1f}"
        bk = "-" if "blink_rate" not in st else \
            f"{st['blink_rate']:.0f} ({st['blink_pause']}, {st['blink_shift']})"
        fl = "-" if "flinch_hits" not in st else \
            f"{st['flinch_hits']}, {st['flinch_peak']:.1f}, {st['flinch_settle']:.2f}"
        lines.append(f"| {seg} | {st['frames']} | {st['speed']:.0f}{still} | {st['ik_on'] * 100:.0f} | "
                     f"{st['hidden'] * 100:.0f} | {contact} | {gap} | {limp} | {gon} | {gz} | {ln} | {an} | {am} | {lc} | {fl} | {gs} | {bk} | {ev or '-'} |")
    lines.append("")
    lines.append(f"Overview sheets (16 frames each): {os.path.join(out, 'sheets', '<segment>.png')}")
    lines.append("")
    more, numbers = faces(segs)
    lines += more
    summary.update(numbers)
    more, numbers = eyes(segs)
    lines += more
    summary.update(numbers)
    lines += manual_aim(out, segs)
    lines += hit_zones(out)
    more, numbers = combat_report([combat(out)])
    lines += more
    summary.update(numbers)

    all_events.sort(key=lambda e: -e["severity"])
    lines.append("## Worst events")
    for i, ev in enumerate(all_events[:TOP_EVENTS]):
        thr, unit, what = FLAGS[ev["flag"]]
        video = os.path.join(out, ev["seg"] + ".mp4")
        path = None
        if os.path.exists(video):
            frames = spread(max(0, ev["start"] - 6), ev["end"] + 6, 16)
            path = sheet(video, frames, os.path.join(out, "sheets", f"ev{i:02d}-{ev['seg']}-{ev['flag']}-{ev['at']}.png"))
        ev["sheet"] = path
        where = f"bone={ev['leg']} [{ev.get('layers', '-')}]" if ev["flag"] == "anim_pop" else f"leg={ev['leg']}"
        lines.append(f"{i:2d}. {ev['seg']} {ev['flag']} {where} frames {ev['start']}-{ev['end']} "
                     f"worst {ev['worst']:.1f} {unit} at {ev['at']} ({what})" + (f"\n    {path}" if path else ""))
    if not all_events:
        lines.append("none")
    summary["events"] = all_events

    with open(os.path.join(out, "report.md"), "w") as fo:
        fo.write("\n".join(lines) + "\n")
    with open(os.path.join(out, "report.json"), "w") as fo:
        json.dump(summary, fo, indent=1)
    print("\n".join(lines))


def compare(a, b, first=None, last=None):
    """a and b side by side: the same scenario before and after a change, or IK off and on"""
    name = lambda v: os.path.basename(os.path.splitext(v)[0])
    base = os.path.join(os.path.dirname(b), f"{name(a)}-vs-{name(b)}")
    trim = ""
    if first is not None:
        trim = f"trim=start_frame={first}:end_frame={last + 1},setpts=PTS-STARTPTS,"
    vf = f"[0:v]{trim}scale=640:-2[a];[1:v]{trim}scale=640:-2[b];[a][b]hstack=inputs=2"
    video = base + ".mp4"
    cmd = ["ffmpeg", "-loglevel", "error", "-y", "-i", a, "-i", b, "-filter_complex", vf,
           "-c:v", "libx264", "-preset", "veryfast", "-crf", "20", "-pix_fmt", "yuv420p", video]
    if subprocess.run(cmd).returncode != 0:
        print("ffmpeg failed")
        return
    n = int(subprocess.run(["ffprobe", "-v", "error", "-count_frames", "-select_streams", "v:0", "-show_entries",
                            "stream=nb_read_frames", "-of", "csv=p=0", video], capture_output=True, text=True).stdout.strip() or 0)
    print(video)
    print(sheet(video, spread(0, max(0, n - 1), 8), base + ".png", cols=2, width=800) or "sheet failed")


def diff(a, b):
    """event counts and contact error of two runs of the same scenario"""
    sa, sb = load(a), load(b)
    total = defaultdict(lambda: [0, 0])
    print("| segment | contact p90/max a | contact p90/max b | events a -> b |")
    print("|---|---|---|---|")
    for seg in sa:
        if seg not in sb:
            continue
        res = []
        for rows in (sa[seg], sb[seg]):
            st, ev = analyze(rows)
            cnt = defaultdict(int)
            for e in ev:
                cnt[e["flag"]] += 1
            res.append((st, cnt))
        flags = sorted(set(res[0][1]) | set(res[1][1]))
        for f in flags:
            total[f][0] += res[0][1][f]
            total[f][1] += res[1][1][f]
        changes = ", ".join(f"{f} {res[0][1][f]}->{res[1][1][f]}" for f in flags if res[0][1][f] != res[1][1][f])

        def c(st):
            ret = "-" if "contact_p90" not in st else f"{st['contact_p90']:.1f}/{st['contact_max']:.1f}"
            ret += "" if "spread" not in st else f" spread {st['spread']:.1f}"
            ret += "" if "anim_p99" not in st else \
                f" pop {st['anim_p99']:.1f}/{st['anim_max']:.1f} sw {st['anim_switch_max']:.1f}"
            ret += "" if "ges_talk" not in st else \
                f" gest {st['ges_talk'] * 100:.0f}/{'-' if st['ges_listen'] is None else round(st['ges_listen'] * 100)}%" \
                f" n{st['ges_starts']} over{st['ges_cuts']}"
            return ret + ("" if "limp" not in st else f" limp {st['limp']:.1f}")
        print(f"| {seg} | {c(res[0][0])} | {c(res[1][0])} | {changes or 'same'} |")
    print()
    print("total: " + ", ".join(f"{f} {v[0]}->{v[1]}" for f, v in sorted(total.items())))


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "diff":
        diff(sys.argv[2], sys.argv[3])
        return
    if len(sys.argv) >= 3 and sys.argv[1] == "aggregate":
        aggregate(sys.argv[2:])
        return
    if len(sys.argv) >= 3 and sys.argv[1] == "compare":
        rng = [int(x) for x in sys.argv[4:6]]
        compare(sys.argv[2], sys.argv[3], *(rng if len(rng) == 2 else []))
        return
    if len(sys.argv) >= 2 and sys.argv[1] == "frames":
        out, seg, a, b = sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
        step = int(sys.argv[6]) if len(sys.argv) > 6 else 1
        frames = list(range(a, b + 1, step))
        path = os.path.join(out, "sheets", f"{seg}-{a}-{b}-{step}.png")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        print(sheet(os.path.join(out, seg + ".mp4"), frames, path) or "failed")
        return
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    report(sys.argv[1])


if __name__ == "__main__":
    main()
