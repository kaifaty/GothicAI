#!/usr/bin/env python3
"""Motion check of a rendered explainer: a strip of frames for every movement in the plan, and the frame-to-frame
change of the whole render with the jumps nobody planned.

    python3 scripts/strips.py "$W/render/draft.mp4" "$W/plan.json" "$W/render/strips"
 -> strips/NN-<beat>-<what>.png   consecutive frames of one movement, left to right (every frame up to 14)
    strips/report.md              every strip with its time, and the jumps: a change much bigger than the frames
                                  around it, away from a planned cut, and one-frame flashes (a jump straight back)

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
    for beat in plan["beats"]:
        bid = beat["id"]
        enter = beat.get("enter")
        if enter:
            out.append((t, enter.get("dur", 0.35), bid, f"enter-{enter['type']}"))
        elif t > 0:
            out.append((t, 0.0, bid, "cut"))
        for i, x in enumerate(beat.get("texts", [])):
            out.append((t + x.get("at", 0), 0.35, bid, f"text{i}-{x.get('enter', 'rise')}"))
        for i, x in enumerate(beat.get("tags", [])):
            out.append((t + x.get("at", 0), 0.35, bid, f"tag{i}-{x.get('enter', 'rise')}"))
        for kind in ("strokes", "fans", "paths"):
            for i, x in enumerate(beat.get(kind, [])):
                out.append((t + x.get("at", 0), x.get("dur", 0.6), bid, f"{kind[:-1]}{i}"))
        if beat.get("zoom"):
            z = beat["zoom"]
            out.append((t + z["at"], z["dur"], bid, "zoom"))
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
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    video, plan_path, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
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
    found = jumps(thumbnails(video)[:end + 1], planned, fps)  # the brand sting after `end` is not the plan's
    report = ["# Motion strips", "", f"{video.name}, {fps:g} fps. Strips run left to right.", "",
              "| strip | at, s | dur, s | frames |", "|---|---|---|---|", *rows, "",
              "## Jumps nobody planned", ""]
    report += (["| frame | s | change | around | what |", "|---|---|---|---|---|", *found]
               if found else ["None."])
    (out / "report.md").write_text("\n".join(report) + "\n")
    print(f"{len(rows)} strips, {len(found)} jumps nobody planned -> {out / 'report.md'}")


if __name__ == "__main__":
    main()
