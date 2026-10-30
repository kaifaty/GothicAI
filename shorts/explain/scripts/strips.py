#!/usr/bin/env python3
"""Motion check of a rendered explainer: a strip of frames for every movement in the plan, and the frame-to-frame
change of the whole render with the jumps nobody planned.

    python3 scripts/strips.py "$W/render/draft.mp4" "$W/plan.json" "$W/render/strips" [--still 3]
 -> strips/NN-<beat>-<what>.png   consecutive frames of one movement, left to right (every frame up to 14)
    strips/report.md              every strip with its time, and the jumps: a change much bigger than the frames
                                  around it, away from a planned cut, and one-frame flashes (a jump straight back);
                                  the motion language of the plan (durations off the set, two things entering at
                                  once); and the stretches where the picture holds still longer than `--still`

A strip shows what one still cannot: whether a thing eases in or snaps, overshoots, flickers for a frame, or
arrives on the word. The render may be a `--scale 0.5` draft. Only ffmpeg runs here.
"""

import json
import math
import re
import statistics
import subprocess
import sys
from pathlib import Path

MAX_FRAMES = 14
WIDTH = 180  # of one frame in a strip
# a beat's transition without `dur`, as src/parts/timing.ts: what travels across the frame takes longer
TRANSITION_DUR = {"fade": 0.35, "flash": 0.35, "zoom": 0.35, "slide": 0.5, "wipe": 0.5}
FOLLOW = 0.06  # MOTION.follow: a tag's box after its pin, a text's words after their band
TAG_LINE_ABOVE_PIN = 72  # a one-line tag's text centre over its pin (parts/carryMath.ts), near enough for a strip


def travel_dur(px: float) -> float:
    """timing.ts travelDur: a carry's flight, longer the farther it goes."""
    return min(0.6, max(0.35, 0.35 * math.sqrt(px / 300)))


def carry_dur(beat: dict, prev: dict) -> float:
    c = beat["carry"]
    if "dur" in c:
        return c["dur"]
    src = c["from"]
    if isinstance(src, dict):
        tag = prev["tags"][src["tag"]]
        src = (tag["x"], tag["y"] - TAG_LINE_ABOVE_PIN)
    return travel_dur(math.dist(src, c["to"]))
# the series' durations (parts/timing.ts MOTION: fast, base, frame, slow) and the gap between two entrances
DURATIONS = (0.2, 0.35, 0.5, 0.6)
STAGGER = 0.08


# timing.ts holdPush: a held picture of 2 s or more pushes in by itself, 1 % a second, 3-5 %
HOLD_PUSH = 2.0
PUSH_MIN, PUSH_MAX = 0.03, 0.05


def hold_push(beat: dict) -> tuple[float, float] | None:
    """(from, scale) of the beat's own slow push, as timing.ts holdPush, or None."""
    if beat.get("zoom") or beat.get("push") == 0 or beat.get("kind") == "split":
        return None
    at = beat.get("freeze") if beat.get("kind") == "footage" else 0.0
    if at is None or beat["dur"] - at < HOLD_PUSH:
        return None
    return at, 1 + beat.get("push", min(PUSH_MAX, max(PUSH_MIN, 0.01 * (beat["dur"] - at))))


def _fits(dur: float) -> bool:
    return any(abs(dur - d) < 1e-6 for d in DURATIONS)


def language(plan: dict) -> list[str]:
    """Rows of the motion-language check (references/motion.md): a duration the plan sets off the series' set, a
    line drawn longer than `slow`, a linear zoom that is not a push across its beat, two things of one beat
    entering less than a stagger apart, a thing entering under its beat's own transition. Durations the plan leaves
    to the schema's defaults are the series' own and are not checked."""
    rows = []
    t = 0.0
    for bi, beat in enumerate(plan["beats"]):
        bid = beat["id"]
        row = lambda what, value, rule: rows.append(f"| {bid} | {t:.2f} | {what} | {value} | {rule} |")  # noqa: E731
        enter = beat.get("enter")
        if enter and "dur" in enter and not _fits(enter["dur"]):
            row(f"enter-{enter['type']}", f"{enter['dur']:g} s", "not from the set: 0.35 fade/flash/zoom, 0.5 slide/wipe")
        for kind in ("strokes", "fans", "paths"):
            for i, x in enumerate(beat.get(kind, [])):
                if "dur" not in x:
                    continue
                if x["dur"] > DURATIONS[-1]:
                    row(f"{kind[:-1]}{i}", f"{x['dur']:g} s", "longer than slow 0.6: only a line following a real flight")
                elif not _fits(x["dur"]):
                    row(f"{kind[:-1]}{i}", f"{x['dur']:g} s", "not from the set: 0.2 / 0.35 / 0.5 / 0.6")
        zoom = beat.get("zoom")
        if zoom:
            if zoom.get("ease") == "linear":
                if zoom["dur"] < 0.8 * beat["dur"]:
                    row("zoom", f"{zoom['dur']:g} s of {beat['dur']:g}", "a linear zoom is a slow push across the beat; ease it otherwise")
            elif zoom["dur"] > 0.04 and not _fits(zoom["dur"]):  # up to a frame: a punch-in, as good as a cut
                row("zoom", f"{zoom['dur']:g} s", "an eased zoom takes slow 0.6, or is a linear push across the beat")
        push = beat.get("push")
        if push and push < PUSH_MIN - 1e-6:
            row("push", f"{push:g}", "below 0.03 the push does not read: 0.03-0.05, or 0 for none")
        carry = beat.get("carry")
        if carry and "dur" in carry and not (DURATIONS[1] - 1e-6 <= carry["dur"] <= DURATIONS[-1] + 1e-6):
            row("carry", f"{carry['dur']:g} s", "a carry takes 0.35-0.6 s by its travel; leave dur out")
        # entrances of the beat, by time: two closer than a stagger compete for the eye
        entries = [(x.get("at", 0), f"{kind[:-1]}{i}") for kind in ("texts", "tags", "strokes", "fans", "paths")
                   for i, x in enumerate(beat.get(kind, []))]
        if zoom and zoom["dur"] > 0.04:
            entries.append((zoom["at"], "zoom"))
        entries.sort()
        for (a, what_a), (b, what_b) in zip(entries, entries[1:]):
            if b - a < STAGGER - 1e-6:
                row(f"{what_a} + {what_b}", f"{b - a:.2f} s apart", "two things at once: stagger by 0.08 in reading order")
        if enter and enter["type"] != "flash":
            dur = enter.get("dur", TRANSITION_DUR[enter["type"]])
            for at, what in entries:
                if at < dur - 1e-6:
                    row(what, f"at {at:g} s", f"enters under the {enter['type']} ({dur:g} s): after it, or a cut")
        t += beat["dur"]
    return rows


STILL = 3.0  # seconds of a picture that does not change at all, past which the report lists it
# mean grey change per pixel (of 255) below which two frames count as the same picture: a card or a frozen frame
# changes by 0.000-0.001, a hero standing idle in footage by 0.05-0.1 (measured on the bow short)
STILL_CHANGE = 0.03
# the voice's caption band, left out of the stillness: a word turning gold is not the picture moving
CAPTION_BAND = {"tall": (0.59, 0.77), "wide": (0.79, 0.96)}


def stills(frames: list[bytes], fps: float, band: tuple[float, float], least: float = STILL) -> list[tuple[int, int]]:
    """(first, last) frames of every stretch of at least `least` seconds where the picture outside the caption
    band changes by less than STILL_CHANGE from frame to frame."""
    w, h = SMALL
    keep = [r for r in range(h) if not band[0] * h <= r < band[1] * h]
    rows = lambda f: b"".join(f[r * w:(r + 1) * w] for r in keep)  # noqa: E731
    picture = [rows(f) for f in frames]
    out = []
    start = 0
    for i in range(1, len(picture) + 1):
        if i == len(picture) or change(picture[i - 1], picture[i]) >= STILL_CHANGE:
            if (i - 1 - start) / fps >= least:
                out.append((start, i - 1))
            start = i
    return out


def hold_beats(plan: dict, a: float, b: float) -> tuple[str, str]:
    """The beats a still stretch from a to b s lies in, and whether their own slow push runs there: "yes" for all
    of them, "part" for some, "no" for none. A push keeps the hold alive, not new: the voice still carries it."""
    ids, pushed = [], []
    t = 0.0
    for beat in plan["beats"]:
        if t < b and a < t + beat["dur"]:
            ids.append(beat["id"])
            push = hold_push(beat)
            pushed.append(bool(push) and t + push[0] < b)
        t += beat["dur"]
    return ", ".join(ids), "yes" if pushed and all(pushed) else "part" if any(pushed) else "no"


PAD_BEFORE = 0.1
PAD_AFTER = 0.15


def probe_fps(video: Path) -> float:
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=r_frame_rate", "-of", "default=nw=1:nk=1",
         str(video)], capture_output=True, text=True, check=True).stdout.strip()
    num, den = out.split("/")
    return float(num) / float(den)


def events(plan: dict) -> list[tuple[float, float, str, str]]:
    """(start, duration, beat id, what) in seconds of the short, for every planned movement."""
    out = []
    t = 0.0
    for bi, beat in enumerate(plan["beats"]):
        bid = beat["id"]
        enter = beat.get("enter")
        if enter:
            out.append((t, enter.get("dur", TRANSITION_DUR[enter["type"]]), bid, f"enter-{enter['type']}"))
        elif t > 0:
            out.append((t, 0.0, bid, "cut"))
        for i, x in enumerate(beat.get("texts", [])):
            out.append((t + x.get("at", 0), 0.35 + FOLLOW, bid, f"text{i}-{x.get('enter', 'rise')}"))
        for i, x in enumerate(beat.get("tags", [])):
            out.append((t + x.get("at", 0), 0.35 + FOLLOW, bid, f"tag{i}-{x.get('enter', 'rise')}"))
        for kind in ("strokes", "fans", "paths"):
            for i, x in enumerate(beat.get(kind, [])):
                out.append((t + x.get("at", 0), x.get("dur", 0.6), bid, f"{kind[:-1]}{i}"))
        if beat.get("zoom"):
            z = beat["zoom"]
            out.append((t + z["at"], z["dur"], bid, "zoom"))
        if beat.get("carry") and bi > 0:
            out.append((t + beat["carry"].get("at", 0), carry_dur(beat, plan["beats"][bi - 1]), bid, "carry"))
        if beat.get("freeze") is not None:
            out.append((t + beat["freeze"], 0.0, bid, "freeze"))
        t += beat["dur"]
    return sorted(out)


def strip(video: Path, start: float, dur: float, fps: float, out: Path) -> list[int]:
    """Frames from just before `start` to just after `start + dur`, side by side; returns their frame numbers."""
    first = max(0, math.floor((start - PAD_BEFORE) * fps))
    last = math.ceil((start + dur + PAD_AFTER) * fps)
    count = last - first + 1
    step = max(1, math.ceil(count / MAX_FRAMES))
    frames = list(range(first, last + 1, step))
    subprocess.run(
        ["ffmpeg", "-v", "error", "-y", "-ss", f"{first / fps:.4f}", "-i", str(video), "-vf",
         f"trim=end_frame={count},select='not(mod(n\\,{step}))',scale={WIDTH}:-2,"
         f"tile={len(frames)}x1:padding=4:color=black", "-frames:v", "1", str(out)], check=True)
    return frames


SMALL = (68, 120)  # a grey thumbnail per frame: enough to see a jump, small enough to compare in Python


def thumbnails(video: Path) -> list[bytes]:
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", str(video), "-vf", f"scale={SMALL[0]}:{SMALL[1]},format=gray",
         "-f", "rawvideo", "-"], capture_output=True, check=True).stdout
    size = SMALL[0] * SMALL[1]
    return [raw[i:i + size] for i in range(0, len(raw) - size + 1, size)]


def change(a: bytes, b: bytes) -> float:
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def jumps(frames: list[bytes], planned: list[tuple[int, int]], fps: float) -> list[str]:
    """Snaps: a change several times the frames around it, outside a planned cut or transition; one-frame
    flashes: a frame unlike both its neighbours while they are alike (anywhere but a planned flash)."""
    diff = [0.0] + [change(frames[i - 1], frames[i]) for i in range(1, len(frames))]
    inside = lambda i: any(a - 1 <= i <= b + 1 for a, b in planned)  # noqa: E731
    found = []
    for i in range(1, len(frames) - 1):
        around = diff[max(1, i - 6):i] + diff[i + 2:i + 8]
        base = statistics.median(around) if around else 0.0
        if diff[i] < max(4.0, 4 * base):
            continue
        across = change(frames[i - 1], frames[i + 1])
        if diff[i + 1] >= 0.5 * diff[i] and across < 0.3 * diff[i]:
            what = "one-frame flash: the next frame goes back"
        elif inside(i):
            continue
        else:
            what = "snap: changes in one frame while the frames around it hold"
        found.append(f"| {i} | {i / fps:.2f} | {diff[i]:.1f} | {base:.1f} | {what} |")
    return found


def main() -> None:
    args = sys.argv[1:]
    least = STILL
    if len(args) == 5 and args[3] == "--still":
        least = float(args[4])
        args = args[:3]
    if len(args) != 3:
        sys.exit(__doc__)
    video, plan_path, out = Path(args[0]), Path(args[1]), Path(args[2])
    plan = json.loads(plan_path.read_text())
    fps = probe_fps(video)
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    planned = []  # frames of cuts and transitions between beats, where a big change is the point
    for n, (start, dur, bid, what) in enumerate(events(plan), 1):
        if what == "cut" or what.startswith("enter-"):
            planned.append((round(start * fps), round((start + dur) * fps)))
        name = f"{n:02d}-{re.sub(r'[^\w-]', '_', bid)}-{what}.png"
        frames = strip(video, start, dur, fps, out / name)
        rows.append(f"| {name} | {start:.2f} | {dur:.2f} | {frames[0]}–{frames[-1]} step {frames[1] - frames[0] if len(frames) > 1 else 1} |")
    # the end of the content: the brand sting starts with a cut
    end = round(sum(b["dur"] for b in plan["beats"]) * fps)
    planned.append((end, end))
    frames = thumbnails(video)[:end + 1]  # the brand sting after `end` is not the plan's
    found = jumps(frames, planned, fps)
    lang = language(plan)
    held = stills(frames, fps, CAPTION_BAND[plan.get("format", "tall")], least)
    report = ["# Motion strips", "", f"{video.name}, {fps:g} fps. Strips run left to right.", "",
              "| strip | at, s | dur, s | frames |", "|---|---|---|---|", *rows, "",
              "## Jumps nobody planned", ""]
    report += (["| frame | s | change | around | what |", "|---|---|---|---|---|", *found]
               if found else ["None."])
    report += ["", "## Motion language", "",
               "The plan against references/motion.md: a row is a choice to justify or fix, not always a defect.", ""]
    report += (["| beat | beat at, s | what | value | rule |", "|---|---|---|---|---|", *lang] if lang else ["None."])
    report += ["", f"## Still longer than {least:g} s", "",
               "Nothing new outside the caption band (a slow push alone changes too little to count): a hold the voice "
               "must carry, or a place for the next move. `push` says whether the beat's own push runs there.", ""]
    report += (["| from, s | to, s | length, s | beats | push |", "|---|---|---|---|---|",
                *[f"| {a / fps:.2f} | {b / fps:.2f} | {(b - a) / fps:.1f} | {' | '.join(hold_beats(plan, a / fps, b / fps))} |"
                  for a, b in held]] if held else ["None."])
    (out / "report.md").write_text("\n".join(report) + "\n")
    print(f"{len(rows)} strips, {len(found)} jumps nobody planned, {len(lang)} motion-language notes, "
          f"{len(held)} stills over {least:g} s -> {out / 'report.md'}")


if __name__ == "__main__":
    main()
