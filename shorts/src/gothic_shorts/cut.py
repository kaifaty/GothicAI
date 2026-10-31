"""The short from a take: AI pauses cut down, separate dialogue and effects, one game music bed,
subtitles and a hook; 1080x1920, calibrated and checked by the shared publication gate.

The texts are burnt in as images (Pillow), so ffmpeg needs no libass. A cut in another language takes
`subs.<lang>.json` from the take: every text of the short (hook, captions, lines) mapped to its translation;
`gothic-shorts subs` writes the list of texts to fill in. The same translations make a caption track for
the Russian short (`subtitles`, a SubRip file YouTube takes).
"""

from __future__ import annotations

import configparser
import json
import math
import shutil
import subprocess
import textwrap
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from . import bed, levels, srt, stems
from .shot import Shot
from .timeline import Timeline, build, read_events

W, H = 1080, 1920
SUB_FONT = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"
HOOK_FONT = "/System/Library/Fonts/Supplemental/Georgia Bold.ttf"
HOOK_S = 2.8
HERO_COLOR = (255, 214, 102)
NPC_COLOR = (255, 255, 255)
NAME_COLOR = (230, 190, 120)
BRAND_FONT = "/System/Library/Fonts/Supplemental/PTSerif.ttc"
BRAND = Path.home() / ".local/share/gothic-ai/brand/sting"
OUTLINE = (20, 12, 8)
# The Shorts interface covers these edges (shorts/explain/src/theme.ts).
SAFE = (60, 150, W - 150, H - 380)  # left, top, right, bottom


class CutError(RuntimeError):
    pass


@dataclass(frozen=True)
class Overlay:
    png: Path
    start: float
    end: float
    y: int  # top of the image on the frame


@dataclass(frozen=True)
class Shown:
    """A text of the cut, translated, and when it is on screen (seconds of the cut)"""

    kind: str  # "hook", "caption" or "line"
    text: str
    start: float
    end: float
    name: str = ""  # the npc's, over its line
    hero: bool = False


def texts(shot: Shot, tl: Timeline) -> list[str]:
    """Every text a cut shows, in order (what a translation has to cover)."""
    out = [shot.hook] if shot.hook else []
    out += [c.text for c in tl.captions] + [line.text for line in tl.lines]
    out += sorted({line.name for line in tl.lines if not line.hero and line.name})
    return list(dict.fromkeys(t for t in out if t))


def load_subs(take: Path, lang: str, needed: list[str]) -> dict[str, str]:
    if lang == "ru":
        return {t: t for t in needed}
    path = take / f"subs.{lang}.json"
    if not path.is_file():
        raise CutError(f"no {path}: gothic-shorts subs {take} --lang {lang} writes it to fill in")
    subs = json.loads(path.read_text())
    missing = [t for t in needed if not subs.get(t)]
    if missing:
        raise CutError(f"{path} has no translation for:\n" + "\n".join(f"  {t}" for t in missing))
    return subs


def write_subs_template(shot: Shot, take: Path, lang: str) -> Path:
    tl = build(read_events(take / "events.jsonl"))
    path = take / f"subs.{lang}.json"
    old = json.loads(path.read_text()) if path.is_file() else {}
    path.write_text(json.dumps({t: old.get(t, "") for t in texts(shot, tl)}, ensure_ascii=False, indent=1))
    return path


def shown(shot: Shot, tl: Timeline, subs: dict[str, str]) -> list[Shown]:
    out: list[Shown] = []
    if shot.hook:
        out.append(Shown("hook", subs[shot.hook], 0.0, min(HOOK_S, tl.cut_duration)))
    for c in tl.captions:
        start = max(tl.to_cut(c.start), HOOK_S if shot.hook else 0.0)
        out.append(Shown("caption", subs[c.text], start, tl.to_cut(c.end)))
    for line in tl.lines:
        start, stop = tl.to_cut(line.start), tl.to_cut(line.end)
        name = "" if line.hero else subs.get(line.name, line.name)
        out.append(Shown("line", subs[line.text], start, max(stop, start + 0.6), name, line.hero))
    return out


def subtitles(shot: Shot, take: Path, lang: str) -> Path:
    """The take's texts in `lang` as a caption track over its Russian cut: <take>/subs.<lang>.srt. A line
    says whose it is (no colours there); the hook and the captions, burnt in in Russian, go in brackets."""
    events = take / "events.jsonl"
    if not events.is_file():
        raise CutError(f"the take has no {events.name}")
    tl = build(read_events(events))
    subs = load_subs(take, lang, texts(shot, tl))
    cues = []
    for s in shown(shot, tl, subs):
        text = s.text if s.kind == "line" else f"[{s.text}]"
        cues.append(srt.Cue(s.start, s.end, f"{s.name}: {text}" if s.name else text))
    path = take / f"subs.{lang}.srt"
    path.write_text(srt.render(cues))
    return path


def cut(shot: Shot, take: Path, lang: str = "ru", out: Path | None = None) -> Path:
    out = out or take / f"short-{lang}.mp4"
    # A rejected take must not leave an older published video under the same name.
    out.unlink(missing_ok=True)
    out.with_suffix(".levels.json").unlink(missing_ok=True)
    events = take / "events.jsonl"
    video, audio = take / "main.mp4", take / "main.wav"
    for p in (events, video, audio):
        if not p.is_file():
            raise CutError(f"the take has no {p.name}: was the segment recorded as `rec main`?")
    _validate_recording(take)
    rows = read_events(events)
    tl = build(rows)
    if not tl.keep():
        raise CutError("the take has no recorded body")
    subs = load_subs(take, lang, texts(shot, tl))
    sting_duration = 0.0
    if shot.brand:
        asset = BRAND / "sting.mp4"
        if not asset.is_file():
            raise CutError(f"missing brand asset: {asset}; brand = false disables the brand")
        sting_duration = _duration(BRAND / "sting.mp4")
    work = take / f"cut-{lang}"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir()

    try:
        clips = stems.collect(rows, tl)
        voice = stems.render(clips, tl, work / "voice.wav")
        overlays = _overlays(shown(shot, tl, subs), work, brand=shot.brand)
        joined, duration = _join(shot, video, audio, tl, overlays, work, sting_duration)
        music = bed.render(shot.bed, shot.bed_start, duration, work / "music.wav")
        levels.publish(joined, music, [(voice, 0.0)], stems.speech_windows(clips, tl),
                       duration, duration + sting_duration, out)
    except (bed.BedError, ValueError, OSError, subprocess.CalledProcessError) as error:
        raise CutError(str(error)) from error
    _sheet(out, out.with_suffix(".png"), duration + sting_duration,
           sting_start=duration if shot.brand else None)
    _transcript(shot, tl, subs, out.with_suffix(".md"), sting_duration, body_duration=duration)
    return out


def _validate_recording(take: Path) -> None:
    """The effects stem is usable only when the game's own music and the map's sounds were off."""
    path = take / "run" / "Gothic.ini"
    config = configparser.ConfigParser(inline_comment_prefixes=(";", "#"))
    try:
        config.read_string(path.read_text())
        volume = config.getfloat("SOUND", "musicVolume")
        if not math.isfinite(volume) or volume != 0:
            raise ValueError("musicVolume must be 0")
        if config.getint("SOUND", "worldSounds") != 0:
            raise ValueError("worldSounds must be 0")
    except (OSError, ValueError, configparser.Error) as error:
        raise CutError(f"{path}: не подтверждены musicVolume=0 и worldSounds=0; переснимите дубль "
                       "через film без музыки игры и звуков мест") from error


def body_duration(tl: Timeline) -> float:
    """One video-frame boundary, after audio joins have kept their exact duration."""
    return math.ceil(round(tl.cut_duration * 30, 6)) / 30


def _effects_filters(tl: Timeline, duration: float, audio_input: str,
                     sting_input: str | None, sting_duration: float) -> tuple[list[str], list[str]]:
    """The shared cut/dub effects graph, split at the body/ending boundary."""
    keep = tl.keep()
    filters = [
        f"[{audio_input}]aformat=sample_rates=48000:channel_layouts=stereo,"
        f"{levels.DUAL_MONO},"
        f"apad=whole_dur={tl.duration:.9f},atrim=duration={tl.duration:.9f},"
        f"asplit={len(keep)}" + "".join(f"[as{i}]" for i in range(len(keep))),
    ]
    for i, (start, end) in enumerate(keep):
        fade = min(0.02, (end - start) / 4)
        filters += [
            f"[as{i}]atrim=start={start:.9f}:end={end:.9f},asetpts=PTS-STARTPTS,"
            f"afade=t=in:d={fade:.9f},afade=t=out:st={end - start - fade:.9f}:d={fade:.9f}[a{i}]",
        ]
    filters.append("".join(f"[a{i}]" for i in range(len(keep)))
                   + f"concat=n={len(keep)}:v=0:a=1[ac]")
    ending = [
        f"[ac]apad,atrim=duration={duration:.9f},"
        f"afade=t=out:st={max(0, duration - 0.02):.9f}:d=0.02[bodya]",
    ]
    if sting_input is not None:
        ending.append(
            f"[{sting_input}]aformat=sample_rates=48000:channel_layouts=stereo,"
            f"{levels.DUAL_MONO},"
            f"apad,atrim=duration={sting_duration:.9f},asetpts=PTS-STARTPTS,"
            "afade=t=in:d=0.02,"
            f"afade=t=out:st={max(0, sting_duration - 0.02):.9f}:d=0.02[stinga]"
        )
    return filters, ending


def render_effects(shot: Shot, audio: Path, tl: Timeline, out: Path,
                   sting_duration: float) -> tuple[Path, float]:
    """Render the same lossless effects and ending as the Russian cut, without picture."""
    duration = body_duration(tl)
    inputs = ["-i", str(audio)]
    if shot.brand:
        inputs += ["-i", str(BRAND / "sting.mp4")]
    body, ending = _effects_filters(tl, duration, "0:a", "1:a" if shot.brand else None,
                                    sting_duration)
    filters = body + ending
    filters.append("[bodya][stinga]concat=n=2:v=0:a=1[aout]" if shot.brand
                   else "[bodya]anull[aout]")
    done = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *inputs,
         "-filter_complex", ";".join(filters), "-map", "[aout]", "-c:a", "pcm_f32le", str(out)],
        capture_output=True, text=True,
    )
    if done.returncode:
        raise CutError(f"ffmpeg failed:\n{done.stderr.strip()[-3000:]}")
    return out, duration


def _join(shot: Shot, video: Path, audio: Path, tl: Timeline, overlays: list[Overlay],
          work: Path, sting_duration: float) -> tuple[Path, float]:
    """Join picture and effects independently, so video rounding cannot insert audio gaps.

    Only effects (and the ending's own sound) go here. Voice and the continuous
    bed remain lossless separate stems until levels publishes the final AAC.
    """
    keep = tl.keep()
    duration = body_duration(tl)
    inputs = ["-i", str(video), "-i", str(audio)]
    for overlay in overlays:
        inputs += ["-i", str(overlay.png)]
    sting_input = 2 + len(overlays)
    if shot.brand:
        inputs += ["-i", str(BRAND / "sting.mp4")]
    selected = "+".join(f"gte(t,{start:.9f})*lt(t,{end:.9f})" for start, end in keep)
    removed = "+".join(f"if(gte(PTS*TB,{end:.9f}),{end - start:.9f},0)"
                       for start, end in tl.cuts) or "0"
    effects, ending = _effects_filters(tl, duration, "1:a",
                                       f"{sting_input}:a" if shot.brand else None, sting_duration)
    filters = [
        # Map every retained frame through to_cut directly. Concatenating rounded
        # video segments would accumulate a frame fraction at every audio join.
        f"[0:v]setpts=PTS-STARTPTS,select='{selected}',setpts='PTS-({removed})/TB'[vc]",
        *effects,
        f"[vc]scale={W}:{H}:force_original_aspect_ratio=increase:flags=lanczos,"
        f"crop={W}:{H},setsar=1[v]",
    ]
    last = "v"
    for i, overlay in enumerate(overlays):
        filters.append(f"[{last}][{2 + i}:v]overlay=x=0:y={overlay.y}:"
                       f"enable='between(t,{overlay.start:.9f},{overlay.end:.9f})'[o{i}]")
        last = f"o{i}"
    filters += [
        f"[{last}]fps=30,tpad=stop_mode=clone:stop_duration={duration:.9f},"
        f"trim=duration={duration:.9f},settb=AVTB,setpts=PTS-STARTPTS[bodyv]",
        ending[0],
    ]
    if shot.brand:
        filters += [
            f"[{sting_input}:v]scale={W}:{H},fps=30,setsar=1,"
            "settb=AVTB,setpts=PTS-STARTPTS[stingv]",
            ending[1],
            "[bodyv][bodya][stingv][stinga]concat=n=2:v=1:a=1[vout][aout]",
        ]
    else:
        filters += ["[bodyv]null[vout]", "[bodya]anull[aout]"]
    joined = work / "joined.mkv"
    command = [
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *inputs,
        "-filter_complex", ";".join(filters), "-map", "[vout]", "-map", "[aout]",
        "-c:v", "libx264", "-preset", "medium", "-crf", "19", "-pix_fmt", "yuv420p", "-r", "30",
        "-c:a", "pcm_f32le", str(joined),
    ]
    (work / "ffmpeg.txt").write_text(" ".join(command))
    done = subprocess.run(command, capture_output=True, text=True)
    if done.returncode:
        raise CutError(f"ffmpeg failed:\n{done.stderr.strip()[-3000:]}")
    return joined, duration


def _duration(video: Path) -> float:
    done = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", str(video)],
        capture_output=True, text=True,
    )
    if done.returncode:
        raise CutError(f"cannot read brand duration: {done.stderr.strip()}")
    return float(done.stdout)


def _overlays(on_screen: list[Shown], work: Path, *, brand: bool = True) -> list[Overlay]:
    out: list[Overlay] = []
    for i, s in enumerate(on_screen):
        png = work / f"{s.kind}{i}.png"
        if brand:
            size, chars, stroke = {"hook": (84, 18, 7), "caption": (64, 22, 6), "line": (62, 26, 6)}[s.kind]
            color = HERO_COLOR if s.hero or s.kind == "caption" else NPC_COLOR
            h = _brand_text_png(s.text, png, size, color, max_chars=chars, stroke=stroke, name=s.name)
            center = 1290 if s.kind == "line" else 300
            y = max(SAFE[1], min(center - h // 2, SAFE[3] - h))
            out.append(Overlay(png, s.start, s.end, y))
            continue
        if s.kind == "hook":
            h = _text_png(s.text, png, HOOK_FONT, 84, (255, 255, 255), max_chars=18, stroke=7)
            y = 300 - h // 2
        elif s.kind == "caption":
            h = _text_png(s.text, png, HOOK_FONT, 64, HERO_COLOR, max_chars=22, stroke=6)
            y = 300 - h // 2
        else:
            color = HERO_COLOR if s.hero else NPC_COLOR
            h = _text_png(s.text, png, SUB_FONT, 62, color, max_chars=26, name=s.name)
            y = 1290 - h // 2
        out.append(Overlay(png, s.start, s.end, y))
    return out


def _brand_text_png(
    text: str, path: Path, size: int, color: tuple[int, int, int], *,
    max_chars: int, stroke: int, name: str,
) -> int:
    """Fit the stroked text inside Shorts' safe area."""
    width = SAFE[2] - SAFE[0]
    while True:
        font = ImageFont.truetype(BRAND_FONT, size, index=3)
        name_font = ImageFont.truetype(BRAND_FONT, int(size * 0.62), index=3)
        rows = []
        for value, face, fill in [(name, name_font, NAME_COLOR), (text, font, color)]:
            if not value:
                continue
            chars = max_chars
            while True:
                wrapped = textwrap.wrap(value, chars)
                boxes = [face.getbbox(r, anchor="mt", stroke_width=stroke) for r in wrapped]
                if all(b[2] - b[0] <= width for b in boxes) or chars == 1:
                    break
                chars -= 1
            rows.extend((r, face, fill, box) for r, box in zip(wrapped, boxes, strict=True))
        gap = int(size * 0.18)
        height = sum(b[3] - b[1] for _, _, _, b in rows) + gap * max(0, len(rows) - 1)
        if height <= SAFE[3] - SAFE[1]:
            break
        size -= 2
        if size < 12:
            raise CutError("text is too long for the Shorts safe area")
    img = Image.new("RGBA", (W, max(1, height)), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    y = 0
    for row, face, fill, box in rows:
        # Account for the glyph bearings as well as the stroke when centering.
        x = (SAFE[0] + SAFE[2] - box[0] - box[2]) / 2
        draw.text((x, y - box[1]), row, font=face, fill=fill, anchor="mt",
                  stroke_width=stroke, stroke_fill=OUTLINE)
        y += box[3] - box[1] + gap
    img.save(path)
    return img.height


def _text_png(
    text: str,
    path: Path,
    font_file: str,
    size: int,
    color: tuple[int, int, int],
    *,
    max_chars: int,
    stroke: int = 6,
    name: str = "",
) -> int:
    """The text centered on a transparent strip as wide as the frame; returns the strip's height."""
    font = ImageFont.truetype(font_file, size)
    name_font = ImageFont.truetype(font_file, int(size * 0.62))
    rows = textwrap.wrap(text, max_chars) or [""]
    gap = int(size * 0.18)
    heights = [font.getbbox(r, stroke_width=stroke)[3] for r in rows]
    name_h = name_font.getbbox(name, stroke_width=stroke)[3] + gap if name else 0
    height = name_h + sum(heights) + gap * (len(rows) - 1) + stroke * 2
    img = Image.new("RGBA", (W, height), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    y = stroke
    if name:
        draw.text((W // 2, y), name, font=name_font, fill=NAME_COLOR, anchor="ma",
                  stroke_width=stroke, stroke_fill=(0, 0, 0))  # fmt: skip
        y += name_h
    for r, h in zip(rows, heights, strict=True):
        draw.text((W // 2, y), r, font=font, fill=color, anchor="ma",
                  stroke_width=stroke, stroke_fill=(0, 0, 0))  # fmt: skip
        y += h + gap
    img.save(path)
    return height


def _sheet(video: Path, png: Path, duration: float, n: int = 12, *, sting_start: float | None = None) -> None:
    """Frames over the whole short in one image, to look at it without playing it."""
    step = max(duration / n, 0.1)
    select = f"fps=1/{step:.3f}"
    if sting_start is not None:
        # Reserve the last cell for the sting even when it is shorter than the sampling interval.
        frames = [round(i * max(0, sting_start - 1 / 30) * 30 / (n - 2)) for i in range(n - 1)]
        frames.append(round((sting_start + (duration - sting_start) / 2) * 30))
        select = "select='" + "+".join(f"eq(n,{frame})" for frame in frames) + "'"
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(video),
         "-vf", f"{select},scale=270:480,tile=6x{(n + 5) // 6}", "-frames:v", "1", str(png)],
        check=False,
    )  # fmt: skip


def _transcript(
    shot: Shot, tl: Timeline, subs: dict[str, str], path: Path, sting_duration: float = 0.0,
    *, body_duration: float | None = None,
) -> None:
    rows = [f"# {shot.title}", "", f"Длина {tl.cut_duration:.1f} с (запись {tl.duration:.1f} с, вырезано "
            f"{tl.duration - tl.cut_duration:.1f} с пауз).", ""]  # fmt: skip
    if sting_duration:
        total = (tl.cut_duration if body_duration is None else body_duration) + sting_duration
        rows += [f"С финалом: {total:.1f} с (+{sting_duration:.1f} с).", ""]
    if shot.hook:
        rows += [f"Зацепка: {subs[shot.hook]}", ""]
    heard = iter(tl.heard)
    for line in tl.lines:
        who = "Герой" if line.hero else line.name
        rows.append(f"- {tl.to_cut(line.start):5.1f}–{tl.to_cut(line.end):5.1f} **{who}**: {subs[line.text]}")
        if line.hero:
            got = next(heard, None)
            if got is not None and got.strip().lower() != line.text.strip().lower():
                rows.append(f"  - распознано: {got}")
    if tl.timeouts:
        rows += ["", f"Таймаутов ожидания ответа: {tl.timeouts}"]
    path.write_text("\n".join(rows) + "\n")
