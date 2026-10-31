"""Separate dialogue on recording time, with one measured gain per speaker."""

from __future__ import annotations

import math
import subprocess
import tempfile
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

from . import levels
from .timeline import Timeline, npc_voice_events, wav_duration

SAMPLE_RATE = 48_000
JOIN_FADE_S = 0.02
CLIP_FADE_S = 0.005


@dataclass(frozen=True)
class Clip:
    who: str
    path: Path
    start: float
    end: float


def collect(events: list[dict], tl: Timeline) -> list[Clip]:
    """Find every hero/NPC voice on the original recording's clock.

    Old takes cannot safely be separated after recording: their effects track
    still contains NPC voices. Never silently omit a missing voice recording.
    """
    paired = npc_voice_events(events)
    for index, event in enumerate(events):
        if event["kind"] == "npc_line" and index not in paired:
            raise ValueError("Нет отдельного npc_voice для реплики NPC: переснимите дубль")

    clips = []
    for event in events:
        if event["kind"] not in ("hero", "npc_voice"):
            continue
        who = "hero" if event["kind"] == "hero" else event.get("npc", "")
        path = Path(event.get("wav") or "")
        length = wav_duration(path) if path.is_file() else None
        if length is None or length <= 0:
            raise ValueError(f"Нет доступного WAV голоса {who}: {path}; переснимите дубль")
        start = (event.get("sample", event["frame"] * 1600) / SAMPLE_RATE
                 if event["kind"] == "npc_voice" else event["frame"] / 30)
        if not math.isfinite(start) or not math.isfinite(length):
            raise ValueError(f"Некорректное время голоса {who}: переснимите дубль")
        clips.append(Clip(who, path, start, start + length))
    if not clips:
        raise ValueError("В дубле нет отдельных голосов: переснимите дубль")
    return sorted(clips, key=lambda clip: clip.start)


def speech_windows(clips: list[Clip], tl: Timeline) -> list[levels.Window]:
    """Map complete hero and NPC utterances through the same kept intervals."""
    windows = []
    for clip in clips:
        for start, end in tl.keep():
            left, right = max(start, clip.start), min(end, clip.end)
            if right > left:
                windows.append((tl.to_cut(left), tl.to_cut(right)))
    return levels.merge_speech_windows(windows, tl.cut_duration)


def speaker_gains(clips: list[Clip]) -> dict[str, float]:
    """Measure all of each speaker's utterances together, preserving line dynamics."""
    speakers: dict[str, list[Clip]] = defaultdict(list)
    for clip in clips:
        speakers[clip.who].append(clip)
    gains = {}
    for who, lines in speakers.items():
        inputs, filters = [], []
        for index, clip in enumerate(lines):
            inputs += ["-i", str(clip.path)]
            filters.append(f"[{index}:a]aformat=sample_rates={SAMPLE_RATE}:"
                           f"channel_layouts=stereo,{levels.DUAL_MONO},"
                           f"asetpts=PTS-STARTPTS[v{index}]")
        samples = sum(round((clip.end - clip.start) * SAMPLE_RATE) for clip in lines)
        repeat = ""
        if 0 < samples < round(0.4 * SAMPLE_RATE):
            # EBU R128 needs a complete 400 ms block. Repeat the speaker's
            # pooled audio only for metering; silence padding would dilute a
            # short reply's level. The rendered utterances keep their length.
            copies = math.ceil(SAMPLE_RATE / samples)
            repeat = f",aloop=loop={copies - 1}:size={samples}"
        filters.append("".join(f"[v{index}]" for index in range(len(lines))) +
                       f"concat=n={len(lines)}:v=0:a=1{repeat}[speaker]")
        loudness = levels.measure(inputs, ";".join(filters), "speaker")
        if not math.isfinite(loudness):
            raise ValueError(f"Голос {who} не имеет измеримой громкости EBU R128")
        gains[who] = levels.BASE_LUFS - loudness
    return gains


def _run(inputs: list[str], filters: list[str], label: str, out: Path) -> None:
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *inputs,
         "-filter_complex", ";".join(filters), "-map", f"[{label}]", "-vn",
         "-c:a", "pcm_f32le", "-ar", str(SAMPLE_RATE), str(out)],
        check=True, capture_output=True, text=True,
    )


def render(clips: list[Clip], tl: Timeline, out: Path) -> Path:
    """Build dialogue on recording time, then cut/fade exactly as the effects.

    Floating-point PCM keeps headroom before the common publication master.
    The complete recording-time stem is rendered before any timeline cuts.
    """
    if not clips or tl.duration <= 0 or not tl.keep():
        raise ValueError("В дубле нет озвученного материала для нарезки")
    out.parent.mkdir(parents=True, exist_ok=True)
    gains = speaker_gains(clips)
    inputs, filters = [], []
    for index, clip in enumerate(clips):
        inputs += ["-i", str(clip.path)]
        duration = clip.end - clip.start
        fade = min(CLIP_FADE_S, duration / 4)
        trim = f"atrim=start={-clip.start:.9f},asetpts=PTS-STARTPTS," if clip.start < 0 else ""
        delay = max(0, round(clip.start * SAMPLE_RATE))
        filters.append(
            f"[{index}:a]aformat=sample_rates={SAMPLE_RATE}:channel_layouts=stereo,"
            f"{levels.DUAL_MONO},"
            f"asetpts=PTS-STARTPTS,volume={gains[clip.who]:.9f}dB,adeclick=t=1:b=3,"
            f"afade=t=in:d={fade:.9f},afade=t=out:st={duration - fade:.9f}:d={fade:.9f},"
            f"{trim}adelay=delays={delay}S:all=1[v{index}]"
        )
    filters.append("".join(f"[v{index}]" for index in range(len(clips))) +
                   f"amix=inputs={len(clips)}:duration=longest:normalize=0,"
                   f"apad=whole_dur={tl.duration:.9f},atrim=duration={tl.duration:.9f}[recording]")
    with tempfile.TemporaryDirectory(dir=out.parent) as work:
        recording = Path(work) / "voice-recording.wav"
        _run(inputs, filters, "recording", recording)
        keep = tl.keep()
        filters = [f"[0:a]asplit={len(keep)}" + "".join(f"[s{i}]" for i in range(len(keep)))]
        for index, (start, end) in enumerate(keep):
            fade = min(JOIN_FADE_S, (end - start) / 4)
            filters.append(
                f"[s{index}]atrim=start={start:.9f}:end={end:.9f},asetpts=PTS-STARTPTS,"
                f"afade=t=in:d={fade:.9f},"
                f"afade=t=out:st={end - start - fade:.9f}:d={fade:.9f}[p{index}]"
            )
        filters.append("".join(f"[p{i}]" for i in range(len(keep))) +
                       f"concat=n={len(keep)}:v=0:a=1[voice]")
        _run(["-i", str(recording)], filters, "voice", out)
    return out
