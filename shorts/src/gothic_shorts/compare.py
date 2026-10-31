"""Cuts a before/after short from the two takes of shorts/compare/film.sh: each beat (a recorded segment)
the take with the animation work off on top, the one with it on below, 1080x1920; a hook over the first
seconds, a title per beat, the game's sound of the "on" take, -14 LUFS. Optional in the plan: a beat's
"slow" {"from", "to", "<lang>"} (seconds of its segment) marks a slowed stretch in the corner; "music"
{"file", "volume"} (the file under the takes' folder, a theme of the game) lies under the whole short; a beat's
"lines" [{"at", "file", "words": [[word, from, to], ...]}] are voiced lines laid at that second of its segment,
captioned word by word in place of the beat's title. Voiced music is calibrated automatically (its plan volume
is ignored), with music and game ducked by word windows and checked before publication. A plan
with a voice lives outside the repository (the explainer's workspace): give its path, the short goes into its
render/ folder. The channel's brand is on unless the plan says "brand": false (the shorts published before it,
kept as they went out): the end sting after the last beat (shorts/brand/sting.py makes it in
~/.local/share/gothic-ai/brand/sting/), PT Serif on warm bands instead of Arial and Georgia on black.

    uv run --project shorts python shorts/compare/cut.py bodies ru
    uv run --project shorts python shorts/compare/cut.py bodies en
    uv run --project shorts python shorts/compare/cut.py hits ru ~/.local/share/gothic-ai/explain/hits/plan.json
"""

# Keep the existing visual/caption code and its long FFmpeg commands unchanged.
# ruff: noqa: E501, B905

from __future__ import annotations

import json
import math
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from gothic_shorts import clicks, cover, levels

ROOT = Path(__file__).resolve().parents[3]
W, H, HALF = 1080, 1920, 960
FPS = 30
SUP = "/System/Library/Fonts/Supplemental/"
# the texts' faces, the band under them and their outline: the brand's (PT Serif Bold — ParaType, OFL, ships with
# macOS: Cyrillic, bookish like the channel's letters — on the night of the brand), or the look before it
STYLES = {True: ((SUP + "PTSerif.ttc", 3), (SUP + "PTSerif.ttc", 3), (23, 17, 13, 165), (20, 12, 8, 255)),
          False: ((SUP + "Arial Bold.ttf", 0), (SUP + "Georgia Bold.ttf", 0), (0, 0, 0, 150), (0, 0, 0, 255))}
LABEL_FONT, HOOK_FONT, BAND, OUTLINE = STYLES[True]
# the brand's end sting (shorts/brand/sting.py): generated art and a sound of the game, not in git
BRAND = Path.home() / ".local/share/gothic-ai/brand/sting"
GRADE = "eq=gamma=1.18:saturation=1.08"  # the game's noon is dark on a phone; the same for both takes
WHITE, ACCENT = (255, 255, 255, 255), (255, 214, 120, 255)  # a caption's words, the one being said
PAGE_WORDS = 3  # a caption page: at most this many words, a new one after a comma or a full stop


def is_compare_plan(plan: dict) -> bool:
    """Comparison beats name recorded segments; explainer beats do not."""
    beats = plan.get("beats") if isinstance(plan, dict) else None
    return isinstance(beats, list) and any(isinstance(beat, dict) and "segment" in beat for beat in beats)


def placed_lines(plan: dict) -> list[dict]:
    """Voiced lines and words on the cut's timeline, in words.<lang>.json form."""
    lines, offset = [], 0.0
    for beat in plan["beats"]:
        for line in beat.get("lines", []):
            at = offset + line["at"] - beat["start"]
            lines.append({
                "n": len(lines) + 1,
                "file": line["file"],
                "start": round(at, 3),
                "words": [
                    {"text": word, "start": round(at + start, 3), "end": round(at + end, 3)}
                    for word, start, end in line["words"]
                ],
            })
        offset += beat["end"] - beat["start"]
    return lines


def font(spec: tuple[str, int], size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(spec[0], size, index=spec[1])


def text_image(path: Path, items: list[tuple[str, tuple[str, int], int, int, tuple, str]]) -> None:
    """items: (text, font, size, y, color, anchor x: 'l' at 40 px, 'r' at W - 40 or 'c' centered); a dark band under
    each"""
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    for text, font_file, size, y, color, ax in items:
        face = font(font_file, size)
        x = {"l": 40, "r": W - 40}.get(ax, W // 2)
        anchor = {"l": "la", "r": "ra"}.get(ax, "ma")
        box = draw.textbbox((x, y), text, font=face, anchor=anchor, stroke_width=0)
        pad = size // 4
        draw.rounded_rectangle((box[0] - pad, box[1] - pad, box[2] + pad, box[3] + pad), radius=pad,
                               fill=BAND)
        draw.text((x, y), text, font=face, fill=color, anchor=anchor, stroke_width=2, stroke_fill=OUTLINE)
    img.save(path)


def caption_image(path: Path, words: list[str], current: int) -> None:
    """A caption page in the title's place, the word being said in the accent colour, smaller when it is wide."""
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    size = 76
    while True:
        face = font(LABEL_FONT, size)
        space, widths = draw.textlength(" ", font=face), [draw.textlength(w, font=face) for w in words]
        total = sum(widths) + space * (len(words) - 1)
        if total <= W - 140 or size <= 48:
            break
        size -= 4
    x, y, pad = (W - total) / 2, HALF - 160, size // 4
    ascent, descent = face.getmetrics()
    draw.rounded_rectangle((x - pad, y - pad, x + total + pad, y + ascent + descent + pad), radius=pad, fill=BAND)
    for k, (word, width) in enumerate(zip(words, widths)):
        draw.text((x, y), word, font=face, fill=ACCENT if k == current else WHITE, stroke_width=6,
                  stroke_fill=OUTLINE)
        x += width + space
    img.save(path)


def caption_states(line: dict, start: float, end: float) -> list[tuple[list[str], int, float, float]]:
    """(page words, the current one, from, to) in seconds of the beat, for a voiced line at line["at"] of the
    segment: pages of PAGE_WORDS, each word lit from its start to the next one's, the last held a little."""
    words = [(w, line["at"] + a - start, line["at"] + b - start) for w, a, b in line["words"]]
    pages: list[list[tuple[str, float, float]]] = [[]]
    for word in words:
        if len(pages[-1]) == PAGE_WORDS:
            pages.append([])
        pages[-1].append(word)
        if word[0][-1] in ",.!?:;":
            pages.append([])
    pages = [page for page in pages if page]
    states = []
    for n, page in enumerate(pages):
        page_end = pages[n + 1][0][1] if n + 1 < len(pages) else min(page[-1][2] + 0.4, end - start)
        for k, (_, a, _b) in enumerate(page):
            to = page[k + 1][1] if k + 1 < len(page) else page_end
            states.append(([w for w, _, _ in page], k, a, to))
    return states


def run(cmd: list[str]) -> None:
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def render_cover(plan: dict, takes: Path, lang: str, out: Path) -> Path | None:
    """A clean off/on pair at a segment's second, graded exactly like the cut, then template B."""
    spec = plan.get("cover")
    if spec is None:
        print("cover: none (plan has no cover)")
        return None
    at = float(spec["at"])
    if not math.isfinite(at) or at < 0:
        raise cover.CoverError("cover.at must be a finite, nonnegative second of the segment")
    inputs = []
    for take in ("off", "on"):
        inputs += ["-ss", str(at), "-i", str(takes / take / f"{spec['segment']}.mp4")]
    graph = ";".join([
        f"[0:v]scale={W}:{HALF},{GRADE}[a]",
        f"[1:v]scale={W}:{HALF},{GRADE}[b]",
        "[a][b]vstack,setsar=1[v]",
    ])
    with tempfile.TemporaryDirectory() as tmp_dir:
        frame = Path(tmp_dir) / "cover-frame.png"
        run(["ffmpeg", "-nostdin", "-y", *inputs, "-filter_complex", graph,
             "-map", "[v]", "-frames:v", "1", str(frame)])
        sub = spec.get("sub", {}).get(lang, "БЫЛО → СТАЛО" if lang == "ru" else "BEFORE → AFTER")
        with Image.open(frame) as image:
            result = cover.save(cover.render(image, spec["title"][lang], sub), out.parent / f"cover-{lang}.jpg")
    print(f"cover: {result}")
    return result


def master(inputs: list[str], mix: str, out: Path, length: float, *, voiced: bool = False,
           audio: levels.AudioGraph | None = None, report: dict | None = None) -> None:
    """Master voiced audio through levels; retain unvoiced two-pass loudnorm."""
    if voiced:
        levels._master_voiced(audio, report, out, length, inputs=inputs, run=run, detect=clicks.detect)
        return
    loud = "loudnorm=I=-14:TP=-1.5:LRA=11"
    pre = (f"{mix},aformat=sample_rates=48000:channel_layouts=stereo,{levels.DUAL_MONO},"
           "alimiter=limit=0.75:level=false")
    probe = subprocess.run(["ffmpeg", "-hide_banner", *inputs, "-filter_complex", f"{pre},{loud}:print_format=json",
                            "-f", "null", "-"], check=True, capture_output=True, text=True).stderr
    key = probe.rindex('"input_i"')  # the json loudnorm prints, among ffmpeg's other lines
    m = json.loads(probe[probe.rindex("{", 0, key):probe.index("}", key) + 1])
    loud += (f":measured_I={m['input_i']}:measured_TP={m['input_tp']}:measured_LRA={m['input_lra']}"
             f":measured_thresh={m['input_thresh']}:offset={m['target_offset']}:linear=true")
    fade = f"afade=t=out:st={length - 0.08:.3f}:d=0.08"
    run(["ffmpeg", "-y", *inputs, "-filter_complex", f"{pre},{loud},{fade}[a]", "-map", "0:v", "-map", "[a]",
         "-c:v", "copy",
         *levels.aac_encoder(), "-ar", "48000", str(out)])
    difference_db = levels.dual_mono_peak(out)
    if difference_db > levels.DUAL_MONO_MAX_DIFF_DB:
        out.unlink(missing_ok=True)
        raise levels.LevelsError(
            f"Mastering gate dual_mono failed: L - R {difference_db:g} dBFS "
            f"> {levels.DUAL_MONO_MAX_DIFF_DB:g} dBFS"
        )


@dataclass(frozen=True)
class Built:
    joined: Path
    voice: list[tuple[Path, float]]
    speech: list[levels.Window]
    total: float
    length: float
    music: Path | None


def build_ending(tmp: Path) -> tuple[Path, float]:
    """Encode the ending like a beat and report its exact container duration."""
    sting = tmp / "sting.mp4"
    run(["ffmpeg", "-y", "-i", str(BRAND / "sting.mp4"), "-r", str(FPS), "-c:v", "libx264", "-preset", "slow",
         "-crf", "18", "-pix_fmt", "yuv420p",
         "-af", f"aformat=sample_rates=48000:channel_layouts=stereo,{levels.DUAL_MONO}",
         *levels.aac_encoder(), "-ar", "48000", "-ac", "2", str(sting)])
    duration = float(subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of",
                                    "csv=p=0", str(sting)], check=True, capture_output=True, text=True).stdout)
    return sting, duration


def build(plan: dict, takes: Path, lang: str, tmp: Path) -> Built:
    """Assemble the comparison picture, game audio and ending, before publication."""
    brand = plan.get("brand", True)
    global LABEL_FONT, HOOK_FONT, BAND, OUTLINE
    LABEL_FONT, HOOK_FONT, BAND, OUTLINE = STYLES[brand]
    speech, total = levels.speech_windows(plan)
    voice = []  # (the line's file, its second in the short)
    parts, offset = [], 0.0
    for i, beat in enumerate(plan["beats"]):
        seg, start, end = beat["segment"], beat["start"], beat["end"]
        items = [
            (plan["labels"][lang][0], LABEL_FONT, 40, 40, (230, 230, 230, 255), "l"),
            (plan["labels"][lang][1], LABEL_FONT, 40, HALF + 40, (255, 214, 120, 255), "l"),
        ]
        if not beat.get("lines"):  # a voiced beat has its captions there instead
            items.append((beat["title"][lang], LABEL_FONT, 64, HALF - 150, (255, 255, 255, 255), "c"))
        text_image(tmp / f"t{i}.png", items)
        layers = [f"[2]loop=-1:1,trim=0:{end - start}[t]"]
        overlay = "[s][t]overlay=0:0:shortest=1"
        inputs = ["-i", str(takes / "off" / f"{seg}.mp4"), "-i", str(takes / "on" / f"{seg}.mp4"),
                  "-i", str(tmp / f"t{i}.png")]
        if i == 0:
            hook = plan["hook"][lang]  # a line break in it: two lines, raised by the second
            text_image(tmp / "hook.png", [(hook, HOOK_FONT, 86, HALF - 330 - 100 * hook.count("\n"),
                                           (255, 255, 255, 255), "c")])
            inputs += ["-i", str(tmp / "hook.png")]
            layers.append(f"[3]loop=-1:1,trim=0:{plan['hook_s']}[h]")
            overlay += "[o];[o][h]overlay=0:0:eof_action=pass"
        if "slow" in beat:  # both takes are slowed: the mark in the corner of each half
            slow = beat["slow"]
            mark = (slow[lang], LABEL_FONT, 40, 40, (255, 255, 255, 255), "r")
            text_image(tmp / f"s{i}.png", [mark, (*mark[:3], HALF + 40, *mark[4:])])
            inputs += ["-i", str(tmp / f"s{i}.png")]
            n = len(inputs) // 2 - 1
            layers.append(f"[{n}]loop=-1:1,trim=0:{end - start}[m]")
            overlay += (f"[q];[q][m]overlay=0:0:shortest=1:"
                        f"enable='between(t,{max(slow['from'] - start, 0)},{slow['to'] - start})'")
        for j, line in enumerate(beat.get("lines", [])):
            voice.append((takes / line["file"], offset + line["at"] - start))
            for k, (words, current, a, b) in enumerate(caption_states(line, start, end)):
                caption_image(tmp / f"c{i}_{j}_{k}.png", words, current)
                inputs += ["-i", str(tmp / f"c{i}_{j}_{k}.png")]
                layers.append(f"[{len(inputs) // 2 - 1}]loop=-1:1,trim=0:{end - start}[c{j}_{k}]")
                overlay += (f"[q{j}_{k}];[q{j}_{k}][c{j}_{k}]overlay=0:0:shortest=1:"
                            f"enable='between(t,{a:.3f},{b:.3f})'")
        offset += end - start
        graph = ";".join([
            f"[0]trim={start}:{end},setpts=PTS-STARTPTS,scale={W}:{HALF},{GRADE}[a]",
            f"[1]trim={start}:{end},setpts=PTS-STARTPTS,scale={W}:{HALF},{GRADE}[b]",
            "[a][b]vstack[s]", *layers, overlay + ",format=yuv420p[v]",
        ])
        fade = 0.06
        dur = end - start
        audio = (f"[{len(inputs) // 2}]aformat=sample_rates=48000:channel_layouts=stereo,{levels.DUAL_MONO},"
                 f"atrim={start}:{end},asetpts=PTS-STARTPTS,"
                 f"afade=t=in:d={fade},afade=t=out:st={dur - fade}:d={fade}[aud]")
        inputs += ["-i", str(takes / "on" / f"{seg}.wav")]
        part = tmp / f"p{i}.mp4"
        run(["ffmpeg", "-y", *inputs, "-filter_complex", graph + ";" + audio, "-map", "[v]", "-map", "[aud]",
             "-r", str(FPS), "-c:v", "libx264", "-preset", "slow", "-crf", "18", *levels.aac_encoder(),
             "-ar", "48000", "-ac", "2", str(part)])
        parts.append(part)
    length = offset
    if brand:  # the end sting after the last beat, encoded like the beats so they join as they are
        sting, duration = build_ending(tmp)
        parts.append(sting)
        length += duration
    (tmp / "list.txt").write_text("".join(f"file '{p}'\n" for p in parts))
    joined = tmp / "joined.mp4"
    run(["ffmpeg", "-y", "-f", "concat", "-safe", "0", "-i", str(tmp / "list.txt"), "-c", "copy", str(joined)])
    music = plan.get("music")
    return Built(joined, voice, speech, total, length, takes / music["file"] if music else None)


def main() -> None:
    name, lang = sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "ru"
    plan_file = Path(sys.argv[3]).expanduser() if len(sys.argv) > 3 else ROOT / "shorts/compare" / f"{name}.json"
    plan = json.loads(plan_file.read_text())
    takes = Path(plan["takes"]).expanduser() if "takes" in plan else ROOT / ".shorts/compare" / name
    out = plan_file.parent / "render" / f"short-{lang}.mp4" if len(sys.argv) > 3 else takes / f"short-{lang}.mp4"
    out.parent.mkdir(parents=True, exist_ok=True)
    voiced = any(beat.get("lines") for beat in plan["beats"])
    if voiced:
        # A failed new check must not leave an older short under the normal name.
        out.unlink(missing_ok=True)
        out.with_suffix(".levels.json").unlink(missing_ok=True)
    with tempfile.TemporaryDirectory() as tmp_dir:
        built = build(plan, takes, lang, Path(tmp_dir))
        if voiced:
            try:
                levels.publish(built.joined, built.music, built.voice, built.speech, built.total,
                               built.length, out, _master=master)
            except levels.LevelsError as error:
                raise SystemExit(str(error)) from error
        else:
            music = plan.get("music")
            audio, report = levels.calibrate(
                built.joined, built.music, built.voice, built.speech, built.total, built.length,
                music_volume=music["volume"] if music else 1.0,
            )
            levels.print_balance(report)
            try:
                master(audio.inputs, audio.mix(), out, built.length, voiced=False)
            except levels.LevelsError as error:
                raise SystemExit(str(error)) from error
    render_cover(plan, takes, lang, out)
    # frames over the whole short, for a look
    run(["ffmpeg", "-y", "-i", str(out), "-vf", "fps=1,scale=270:-2,tile=6x3", "-frames:v", "1",
         str(out.with_suffix(".png"))])
    print(out)
