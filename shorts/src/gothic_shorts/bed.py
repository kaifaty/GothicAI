"""One continuous game music bed, from the local theme library's clean start."""

from __future__ import annotations

import csv
import math
import subprocess
from pathlib import Path

from . import levels

MUSIC = Path.home() / ".local/share/gothic-ai/music/game"


class BedError(RuntimeError):
    pass


def _clean_start(theme: str) -> float:
    catalog = MUSIC / "catalog.tsv"
    try:
        with catalog.open(newline="") as source:
            row = next(
                (row for row in csv.DictReader(source, delimiter="\t") if row.get("file") == f"{theme}.flac"),
                {},
            )
    except OSError as err:
        raise BedError(f"cannot read music catalog {catalog}: {err}; set bed_start in the shot") from err
    try:
        start = float(row.get("clean_from_s", ""))
    except (TypeError, ValueError):
        start = math.nan
    if not math.isfinite(start) or start < 0:
        raise BedError(f"music theme {theme!r} has no clean start in {catalog}; set bed_start in the shot")
    return start


def render(theme: str, start: float | None, duration: float, out: Path) -> Path:
    """Extract exactly the body's duration; calibration and fades belong to `levels.publish`."""
    if not theme or Path(theme).name != theme or theme in {".", ".."}:
        raise BedError("bed must be a music theme name, without a path")
    source = MUSIC / f"{theme}.flac"
    if not source.is_file():
        raise BedError(f"missing music theme {source}; choose a bed from {MUSIC / 'catalog.tsv'}")
    if start is None:
        start = _clean_start(theme)
    if not math.isfinite(start) or start < 0:
        raise BedError("bed_start must be a nonnegative number of seconds")
    if not math.isfinite(duration) or duration <= 0:
        raise BedError("music bed duration must be positive")
    probe = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", str(source)],
        capture_output=True, text=True,
    )
    try:
        length = float(probe.stdout.strip()) if probe.returncode == 0 else math.nan
    except ValueError:
        length = math.nan
    if not math.isfinite(length):
        raise BedError(f"cannot read music theme duration: {source}")
    if start + duration > length + 1 / 48000:
        raise BedError(
            f"music theme {theme!r} has only {max(0.0, length - start):.2f} s after {start:g} s, "
            f"but the cut needs {duration:.2f} s; choose another bed or bed_start in the shot"
        )
    out.parent.mkdir(parents=True, exist_ok=True)
    done = subprocess.run(
        [
            "ffmpeg", "-nostdin", "-v", "error", "-y", "-ss", f"{start:.9f}", "-i", str(source),
            "-t", f"{duration:.9f}", "-af",
            f"aformat=sample_rates=48000:channel_layouts=stereo,{levels.DUAL_MONO}",
            "-c:a", "pcm_s16le", str(out),
        ],
        capture_output=True, text=True,
    )
    if done.returncode:
        raise BedError(f"cannot render music bed {theme!r}: {done.stderr.strip()}")
    return out
