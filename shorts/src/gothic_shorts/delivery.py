"""Delivery check of a finished short: what YouTube asks of the file, and our drawing clear of the Shorts interface.

The file (any short: an explainer, a cut take, a comparison): H.264 High, 4:2:0 in limited range, BT.709 matrix,
primaries and transfer, progressive, a constant frame rate, AAC at 48 kHz in stereo, the index at the front, at most
3 minutes for a vertical Short, and the loudness `levels` publishes (-14 LUFS, true peak under -1 dBTP). YouTube's
recommended bitrate (8 Mbit/s for 1080p30) is only a note: it re-encodes everything anyway.

The safe zones (a tall explainer): the composition renders with `mask` on, at a quarter of its size: only what we
draw, over black. Anything bright in the bands the Shorts interface covers (SAFE in explain/src/theme.ts: the search
bar at the top, the title and channel at the bottom, the buttons at the right; its left margin is ours, nothing
covers it) is a finding with its time and beat; a thing that only passes through in a transition is listed apart.
A wide video plays between the interface, not under it.
"""

from __future__ import annotations

import json
import re
import struct
import subprocess
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

from . import levels

PROJECT = Path(__file__).resolve().parents[2] / "explain"
FPS = 30  # explain/src/theme.ts

SHORT_MAX_SECONDS = 180.0
BITRATE_NOTE = 8_000_000  # YouTube's recommendation for 1080p at 24-30 fps
LUFS_TOLERANCE = 1.0
# limited range: a frame with more than this share of luma outside 12..240 holds full-range values
RANGE_SAMPLES = 12
RANGE_OUTSIDE = 0.005

MASK_SCALE = 0.25
BRIGHT = 60  # luma over black: text, bars and pins are far above; the card's ground (~17) and its grid (~50) below
LEAST_DEPTH = 8  # px of the full frame: less is a glyph's edge, not a thing in the zone
LEAST_HOLD = 0.25  # s: a shorter stay is a thing passing through in a transition


class DeliveryError(RuntimeError):
    pass


@dataclass
class Report:
    failures: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    lines: list[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.failures

    def check(self, ok: bool, line: str, *, note: bool = False) -> None:
        self.lines.append(("ok   " if ok else "NOTE " if note else "FAIL ") + line)
        if not ok:
            (self.notes if note else self.failures).append(line)


def _probe(video: Path) -> dict:
    out = subprocess.run(["ffprobe", "-v", "error", "-print_format", "json", "-show_format", "-show_streams", str(video)],
                         check=True, capture_output=True, text=True).stdout
    return json.loads(out)


def _rate(text: str) -> float:
    num, _, den = text.partition("/")
    return float(num) / float(den or 1) if float(den or 1) else 0.0


def atoms(video: Path) -> list[str]:
    """The MP4's top-level boxes in order."""
    names = []
    with video.open("rb") as f:
        while True:
            head = f.read(8)
            if len(head) < 8:
                return names
            size, name = struct.unpack(">I4s", head)
            names.append(name.decode("latin-1"))
            if size == 1:
                size = struct.unpack(">Q", f.read(8))[0] - 8
            elif size == 0:
                return names
            f.seek(size - 8, 1)


def luma_outside(video: Path, duration: float, samples: int = RANGE_SAMPLES) -> float:
    """The largest share of luma outside 12..240 over frames spread through the video, read as coded."""
    worst = 0.0
    for i in range(samples):
        at = duration * (i + 0.5) / samples
        raw = subprocess.run(["ffmpeg", "-v", "error", "-ss", f"{at:.3f}", "-i", str(video), "-frames:v", "1",
                              "-vf", "extractplanes=y", "-f", "rawvideo", "-pix_fmt", "gray", "-"],
                             check=True, capture_output=True).stdout
        if raw:
            outside = len(raw) - sum(raw.count(bytes([v])) for v in range(12, 241))
            worst = max(worst, outside / len(raw))
    return worst


def check_file(video: Path, report: Report | None = None) -> Report:
    report = report or Report()
    info = _probe(video)
    v = next((s for s in info["streams"] if s["codec_type"] == "video"), None)
    a = next((s for s in info["streams"] if s["codec_type"] == "audio"), None)
    duration = float(info["format"].get("duration", 0))
    if v is None:
        report.check(False, "no video stream")
        return report
    w, h = int(v["width"]), int(v["height"])
    tall = h > w
    report.check(info["format"]["format_name"].startswith("mov,mp4"), f"container {info['format']['format_name']}")
    report.check(v["codec_name"] == "h264" and v.get("profile") == "High",
                 f"video {v['codec_name']} {v.get('profile')} (H.264 High)")
    report.check(v.get("pix_fmt") == "yuv420p", f"pixels {v.get('pix_fmt')} (yuv420p: 4:2:0, limited range)")
    report.check(v.get("color_range") == "tv", f"range {v.get('color_range', 'unknown')} (tv)")
    tags = [v.get(k, "unknown") for k in ("color_space", "color_primaries", "color_transfer")]
    report.check(tags == ["bt709"] * 3, f"matrix/primaries/transfer {'/'.join(tags)} (bt709)")
    report.check(v.get("field_order", "progressive") == "progressive", f"scan {v.get('field_order', 'progressive')}")
    fps, avg = _rate(v["r_frame_rate"]), _rate(v.get("avg_frame_rate", "0/1"))
    report.check(fps in (24, 25, 30, 48, 50, 60), f"frame rate {fps:g}")
    # an average below the rate: gaps in the timestamps, usually where pieces were joined with `-c copy`
    report.check(abs(fps - avg) < 0.01, f"average frame rate {avg:.3f} (constant: {fps:g})", note=True)
    report.check((w, h) in ((1080, 1920), (1920, 1080)), f"size {w}x{h}")
    if tall:
        report.check(duration <= SHORT_MAX_SECONDS, f"length {duration:.1f} s (a Short: up to {SHORT_MAX_SECONDS:g})")
    outside = luma_outside(video, duration)
    report.check(outside <= RANGE_OUTSIDE, f"coded luma outside 12..240: {outside:.2%} of the worst sampled frame")
    names = atoms(video)
    fast = "moov" in names and "mdat" in names and names.index("moov") < names.index("mdat")
    report.check(fast, f"index before the data (fast start): {' '.join(names)}", note=True)
    rate = int(v.get("bit_rate") or 0)
    if rate:
        report.check(rate >= BITRATE_NOTE, f"video {rate / 1e6:.1f} Mbit/s (YouTube suggests 8)", note=True)
    if a is None:
        report.check(False, "no audio stream")
        return report
    report.check(a["codec_name"] == "aac" and a.get("profile") == "LC", f"audio {a['codec_name']} {a.get('profile')}")
    report.check(int(a["sample_rate"]) == 48000 and int(a["channels"]) == 2,
                 f"audio {a['sample_rate']} Hz, {a['channels']} channels (48000, 2)")
    lufs, peak = levels.meter_file(video)
    report.check(abs(lufs - levels.BASE_LUFS) <= LUFS_TOLERANCE and peak <= levels.TRUE_PEAK_DBTP,
                 f"loudness {lufs:.1f} LUFS, true peak {peak:.1f} dBTP")
    return report


# --- safe zones -------------------------------------------------------------------------------------------------

def theme_safe(name: str) -> dict[str, int]:
    """SAFE or WIDE_SAFE from explain/src/theme.ts, the composition's own margins."""
    text = (PROJECT / "src/theme.ts").read_text()
    body = re.search(rf"export const {name} = \{{([^}}]*)\}}", text)
    if not body:
        raise DeliveryError(f"theme.ts has no {name}")
    return {k: int(v) for k, v in re.findall(r"(\w+):\s*(\d+)", body.group(1))}


def zones(width: int, height: int, safe: dict[str, int]) -> dict[str, tuple[int, int, int, int]]:
    """The bands the Shorts interface covers, as (x0, y0, x1, y1) of the full frame."""
    return {
        "top": (0, 0, width, safe["top"]),
        "bottom": (0, height - safe["bottom"], width, height),
        "right": (width - safe["right"], 0, width, height),
    }


@dataclass
class Stay:
    zone: str
    first: int  # frames
    last: int
    depth: int  # px of the full frame into the zone, the deepest
    box: tuple[int, int, int, int]  # x0, y0, x1, y1 of the full frame


def bright_box(frame: bytes, w: int, rect: tuple[int, int, int, int], scale: float) -> tuple[int, int, int, int] | None:
    """The box of bright pixels of a gray frame inside a full-frame rectangle, in full-frame px."""
    x0, y0, x1, y1 = (round(c * scale) for c in rect)
    found = None
    for y in range(y0, y1):
        row = frame[y * w + x0: y * w + x1]
        if max(row, default=0) <= BRIGHT:
            continue
        xs = [i for i, b in enumerate(row) if b > BRIGHT]
        bx0, bx1 = x0 + xs[0], x0 + xs[-1] + 1
        found = (bx0, y, bx1, y + 1) if found is None else (
            min(found[0], bx0), found[1], max(found[2], bx1), y + 1)
    if found is None:
        return None
    return tuple(round(c / scale) for c in found)  # type: ignore[return-value]


def depth_into(zone: str, rect: tuple[int, int, int, int], box: tuple[int, int, int, int]) -> int:
    """How far past the safe area's edge the box reaches into the zone."""
    x0, y0, x1, y1 = rect
    return {"top": y1 - box[1], "bottom": box[3] - y0, "right": box[2] - x0}[zone]


def stays(frames: list[bytes], w: int, scale: float, rects: dict[str, tuple[int, int, int, int]]) -> list[Stay]:
    """Runs of frames with something bright in a zone, deeper than LEAST_DEPTH."""
    found: list[Stay] = []
    open_: dict[str, Stay] = {}
    for n, frame in enumerate(frames):
        for zone, rect in rects.items():
            box = bright_box(frame, w, rect, scale)
            depth = depth_into(zone, rect, box) if box else 0
            if depth < LEAST_DEPTH:
                if zone in open_:
                    found.append(open_.pop(zone))
                continue
            run = open_.get(zone)
            if run is None:
                open_[zone] = Stay(zone, n, n, depth, box)
            else:
                run.last = n
                run.depth = max(run.depth, depth)
                run.box = (min(run.box[0], box[0]), min(run.box[1], box[1]), max(run.box[2], box[2]), max(run.box[3], box[3]))
    found.extend(open_.values())
    return sorted(found, key=lambda s: (s.first, s.zone))


def beat_at(plan: dict, t: float) -> str:
    at = 0.0
    for beat in plan["beats"]:
        at += float(beat["dur"])
        if t < at:
            return beat["id"]
    return plan["beats"][-1]["id"]


def render_mask(ws: Path, plan: dict, out: Path, scale: float = MASK_SCALE) -> None:
    props = {k: v for k, v in plan.items() if k != "stem"} | {"mask": True}
    props_file = out.with_suffix(".json")
    props_file.write_text(json.dumps(props, allow_nan=False))
    width, height = (1920, 1080) if plan.get("format", "tall") == "wide" else (1080, 1920)
    command = ["npx", "remotion", "render", "Explainer", "--public-dir", str(ws), "--props", str(props_file),
               "--width", str(width), "--height", str(height), "--scale", str(scale), "--muted",
               "--codec", "h264", "--crf", "1", str(out)]
    try:
        subprocess.run(command, cwd=PROJECT, check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as err:
        raise DeliveryError(f"mask render failed:\n{(err.stderr or err.stdout)[-3000:]}") from err


def gray_frames(video: Path) -> tuple[list[bytes], int, int]:
    s = json.loads(subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                                   "stream=width,height", "-of", "json", str(video)],
                                  check=True, capture_output=True, text=True).stdout)["streams"][0]
    w, h = int(s["width"]), int(s["height"])
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", str(video), "-f", "rawvideo", "-pix_fmt", "gray", "-"],
                         check=True, capture_output=True).stdout
    return [raw[i:i + w * h] for i in range(0, len(raw) - w * h + 1, w * h)], w, h


def check_zones(ws: Path, plan: dict, report: Report | None = None, mask: Path | None = None) -> Report:
    report = report or Report()
    if plan.get("format", "tall") == "wide":
        report.lines.append("skip safe zones: a wide video plays between the Shorts interface, not under it")
        return report
    width, height = 1080, 1920
    safe = theme_safe("SAFE")
    rects = zones(width, height, safe)
    with tempfile.TemporaryDirectory(prefix=".mask-") as tmp:
        video = mask or Path(tmp) / "mask.mp4"
        if mask is None:
            render_mask(ws, plan, video)
        frames, w, _ = gray_frames(video)
    scale = w / width
    held = passing = 0
    for s in stays(frames, w, scale, rects):
        a, b = s.first / FPS, (s.last + 1) / FPS
        where = f"{s.zone} {a:6.2f}-{b:6.2f} s · {beat_at(plan, a)} · {s.depth} px in, box {s.box}"
        if b - a >= LEAST_HOLD:
            held += 1
            report.check(False, f"under the Shorts interface: {where}")
        else:
            passing += 1
            report.lines.append(f"pass {where} (in passing)")
    if not held:
        report.check(True, f"safe zones top {safe['top']}, bottom {safe['bottom']}, right {safe['right']}: "
                           f"nothing held under them ({passing} in passing)")
    return report


def write(report: Report, out: Path, title: str) -> None:
    out.write_text(f"# {title}\n\n" + "\n".join(f"- {line}" for line in report.lines) + "\n")
