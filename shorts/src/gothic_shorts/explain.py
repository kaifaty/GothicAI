"""Remotion supplies the picture and raw stems; levels owns voiced publication."""

from __future__ import annotations

import json
import math
import subprocess
import sys
import tempfile
import wave
from array import array
from pathlib import Path

from . import levels

PROJECT = Path(__file__).resolve().parents[2] / "explain"
# Match explain/src/theme.ts and Brand.tsx (also used by Root.tsx metadata).
FPS = 30
STING_FRAMES = round(2.2 * FPS)
# The footage sound fades in and out over this at each beat's edges, so a hard cut does not click.
EDGE_FADE = 0.03


class ExplainError(RuntimeError):
    pass


def speech_windows(plan: dict) -> tuple[list[levels.Window], float, float]:
    """Absolute first-to-last-word spans, clipped to the frame-rounded body."""
    durations = [float(beat["dur"]) for beat in plan["beats"]]
    if not durations or any(not math.isfinite(dur) or dur <= 0 for dur in durations):
        raise ValueError("The explainer needs beats with positive finite durations")
    total = math.floor(sum(durations) * FPS + 0.5) / FPS  # Remotion's Math.round
    length = total + (STING_FRAMES / FPS if plan.get("brand", True) else 0)
    windows = [(float(line[0]["start"]), float(line[-1]["end"]))
               for line in plan.get("lines", []) if line]
    return levels.merge_speech_windows(windows, total), total, length


def sound_spans(plan: dict) -> list[tuple[float, float]]:
    """Where each beat's footage may sound in the game stem, in seconds, with Remotion's frame rounding.

    A beat plays from its first frame to its last one, or until the first frame at or past `freeze`:
    the frozen picture is silent.
    """
    spans = []
    t = 0.0
    for beat in plan["beats"]:
        first = math.floor(t * FPS + 0.5)  # Explainer.tsx: Math.round(t * fps), with t summed beat by beat
        t += float(beat["dur"])
        last = math.floor(t * FPS + 0.5)
        if beat.get("freeze") is not None:
            freeze = float(beat["freeze"])
            frozen = max(0, math.ceil(freeze * FPS))
            while frozen > 0 and (frozen - 1) / FPS >= freeze:  # Picture: frame / fps >= freeze
                frozen -= 1
            while frozen / FPS < freeze:
                frozen += 1
            last = min(last, first + frozen)
        if last > first:
            spans.append((first / FPS, last / FPS))
    return spans


def fade_edges(path: Path, spans: list[tuple[float, float]], ramp: float = EDGE_FADE) -> None:
    """Fade a 16-bit WAV in place to silence at both ends of each span, sample by sample.

    Remotion evaluates a volume callback once per video frame, a step at 30 fps; a cut needs a ramp
    shorter than a frame. Samples outside the spans (the brand sting) stay as they are.
    """
    with wave.open(str(path), "rb") as source:
        params = source.getparams()
        data = source.readframes(params.nframes)
    if params.sampwidth != 2:
        raise ExplainError(f"{path.name}: expected 16-bit PCM, got {8 * params.sampwidth}-bit")
    samples = array("h", data)
    if sys.byteorder != "little":
        samples.byteswap()
    rate, channels = params.framerate, params.nchannels
    for start, end in spans:
        width = min(ramp, (end - start) / 2) * rate
        a, b = round(start * rate), min(round(end * rate), params.nframes)
        head = range(a, min(b, a + math.ceil(width)))
        tail = range(max(head.stop, b - math.ceil(width)), b)
        for index in (*head, *tail):
            gain = min(1.0, (index - a) / width, (b - index) / width)
            for offset in range(index * channels, (index + 1) * channels):
                samples[offset] = round(samples[offset] * gain)
    if sys.byteorder != "little":
        samples.byteswap()
    with wave.open(str(path), "wb") as target:
        target.setparams(params)
        target.writeframes(samples.tobytes())


def _run(command: list[str], *, cwd: Path | None = None) -> None:
    try:
        subprocess.run(command, cwd=cwd, check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as err:
        detail = err.stderr or err.stdout or str(err)
        raise ExplainError(f"{command[0]} failed ({err.returncode}):\n{detail[-3000:]}") from err
    except OSError as err:
        raise ExplainError(str(err)) from err


def render(ws: Path, plan: dict, directory: Path, *,
           stems: tuple[str | None, ...] = (None, "voice", "game", "music")) -> None:
    """Render selected WAVs, the game stem with faded beat edges; None selects the muted H.264 picture."""
    if any(stem not in (None, "voice", "game", "music") for stem in stems):
        raise ExplainError("unknown Remotion stem")
    format = plan.get("format", "tall")
    if format not in ("tall", "wide"):
        raise ExplainError("The plan's format must be tall or wide")
    width, height = (1920, 1080) if format == "wide" else (1080, 1920)
    for stem in stems:
        props = dict(plan)
        props.pop("stem", None)
        if stem is not None:
            props["stem"] = stem
        props_file = directory / f"{stem or 'picture'}.json"
        props_file.write_text(json.dumps(props, allow_nan=False))
        output = directory / (f"{stem}.wav" if stem else "picture.mp4")
        command = ["npx", "remotion", "render", "Explainer", "--public-dir", str(ws),
                   "--props", str(props_file), "--width", str(width), "--height", str(height)]
        command += ["--codec", "wav"] if stem else ["--codec", "h264", "--crf", "18", "--muted"]
        print(f"Remotion: {output.name}", flush=True)
        _run([*command, str(output)], cwd=PROJECT)
        if stem == "game":
            try:
                fade_edges(output, sound_spans(plan))
            except wave.Error as err:
                raise ExplainError(f"{output.name}: {err}") from err


def publish(ws: Path, plan_file: str = "plan.json", out: Path | None = None) -> Path:
    """Publish a workspace plan, keeping only a checked video and levels' report."""
    ws = ws.resolve()
    try:
        plan = json.loads((ws / plan_file).read_text())
        lang = plan["lang"]
        if lang not in ("ru", "en"):
            raise ValueError("The plan's lang must be ru or en")
        speech, total, length = speech_windows(plan)
        music = plan.get("music")
        # Remotion has already applied these fades to its music stem. Match
        # schema.ts defaults, preserving an explicitly disabled (zero) fade.
        music_fade_durations = (
            (float(music.get("fadeIn", 1.0)), float(music.get("fadeOut", 1.5)))
            if music is not None else (0.0, 0.0)
        )
        out = Path(out).expanduser().resolve() if out is not None else ws / f"render/short-{lang}.mp4"
        out.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".explain-", dir=out.parent) as tmp:
            directory = Path(tmp)
            render(ws, plan, directory)
            joined = directory / "joined.mkv"
            _run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                  "-i", str(directory / "picture.mp4"), "-i", str(directory / "game.wav"),
                  "-map", "0:v:0", "-map", "1:a:0", "-c", "copy", str(joined)])
            levels.publish(joined, directory / "music.wav" if music is not None else None,
                           [(directory / "voice.wav", 0)], speech, total, length, out,
                           music_fades=False, music_fade_durations=music_fade_durations)
    except (OSError, ValueError, KeyError, TypeError) as err:
        raise ExplainError(str(err)) from err
    return out
