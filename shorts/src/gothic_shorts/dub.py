"""English dialogue over the Russian short's lossless effects, bed and ending stems.

    gothic-shorts dub <shot> [take]  -> <take>/dub.en.wav, dub.en.mp4, dub.en.levels.json
    gothic-shorts dub <explainer>    -> <workspace>/render/dub.en.*

The Russian short must already have passed levels.publish. Its picture and Russian
AAC are copied; the same cut/Remotion stems are published with an English voice
stem through levels.publish. The WAV is decoded from that checked English AAC.
Voices and cached lines keep their existing Inworld/VoxCPM cloning paths.

Lines keep their slots on the Russian timeline, sped up by MAX_TEMPO at most.
A line still too long is reported for translation changes; lip sync stays Russian.
"""

from __future__ import annotations

import fcntl
import hashlib
import json
import math
import os
import shutil
import subprocess
import tempfile
import wave
from collections.abc import Callable, Iterator
from contextlib import contextmanager
from dataclasses import dataclass, field
from functools import partial
from pathlib import Path

from gothic_launcher.commands import child_env, tools
from gothic_launcher.settings import Config

from . import bed, compare, cut, explain, levels, srt, stems
from .cut import CutError, load_subs
from .film import shorts_dir
from .gpu import GpuError, job
from .shot import Shot
from .timeline import Timeline, build, read_events
from .voice import LEAD_IN_S, VOICE_ID, VoiceError, say
from .words import LEAD, clean_chain

LOCALES = {"en": "en-US"}  # a dub's language -> the TTS's
MAX_TEMPO = 1.15  # a line may be sped up this much to fit its slot
GAP_S = 0.15  # an English line ends this long before the next one starts
TAIL_S = 0.3  # and the last one this long before the short ends
LEAD_S = 0.05  # silence kept before a line's first sound (the line is placed this much early)
# of the user's recorded lines, for the cloned voice: at least, at most (VoxCPM2's encoder takes up to 15 s;
# Inworld clones best from 10-30 s)
REFERENCE_S = (8.0, 14.0)
REFERENCE_INWORLD_S = (20.0, 29.0)
REFERENCE_GAP_S = 0.3  # between two lines of it
# the voices the dubs speak in, kept for every short and every checkout: the user's recorded one
# (user.<provider>.wav), an npc's Russian sample Inworld clones (*.ru.wav), one VoxCPM designed (*.ref.wav)
VOICES = Path.home() / ".local/share/gothic-ai/voices"
USER_VOICE = "USER"  # the Inworld clone of user.inworld.wav, in Inworld's library and the TTS's voice cache
# what VoxCPM designs a voice reading (the voxcpm bridge's VOICE_SAMPLE, in the dub's language)
SAMPLES = {"en": "You're back. Good. We don't have much time, so listen carefully: I won't say it twice."}
# what an npc's Russian voice reads for Inworld to clone it (10-30 s is best)
RUSSIAN_SAMPLE = (
    "Слушай внимательно, повторять я не стану. Дорога к городу идёт мимо фермы и дальше через лес, где "
    "водятся волки. Если встретишь стражу, не беги и не хватайся за оружие: скажи, что идёшь к паладинам. "
    "И возвращайся, когда узнаешь что-нибудь стоящее."
)

# the silence at both ends of a TTS line trimmed to LEAD_S before and a breath after
TRIM = (
    f"silenceremove=start_periods=1:start_threshold=-50dB:start_silence={LEAD_S},areverse,"
    "silenceremove=start_periods=1:start_threshold=-50dB:start_silence=0.08,areverse"
)


class DubError(RuntimeError):
    pass


@dataclass(frozen=True)
class Slot:
    """A line of the dub: where its English starts and by when it has to be over (seconds of the short)."""

    who: str  # "hero", an npc's symbol, or "voice" (an explainer's)
    ru: str
    en: str
    start: float
    end: float

    @property
    def room(self) -> float:
        return max(self.end - self.start, 0.01)


@dataclass(frozen=True)
class Fit:
    tempo: float  # 1 as voiced, above 1 sped up
    seconds: float  # how long the line is at that tempo
    fits: bool


@dataclass
class Dub:
    wav: Path
    preview: Path | None
    duration: float
    ru_duration: float | None  # of the Russian short, when it is there
    lufs: float | None
    ru_lufs: float | None
    rows: list[str] = field(default_factory=list)  # a line each: where, how long, tempo, gain, text
    too_long: list[str] = field(default_factory=list)  # lines to shorten: the report


def slots(lines: list[tuple[str, str, str, float, float]], duration: float) -> list[Slot]:
    """(who, Russian, English, where its speech starts, where the Russian ends) per line, in the short's time
    and order -> each line's slot: up to the next line's start. Where the Russian lines overlap (one speaks
    over another), the English may too, as long as the Russian one went on."""
    out = []
    for i, (who, ru, en, start, ru_end) in enumerate(lines):
        after = [line[3] for line in lines[i + 1 :] if line[3] > start]
        end = duration - TAIL_S if not after else max(min(after) - GAP_S, ru_end)
        out.append(Slot(who, ru, en, start, min(end, duration - TAIL_S)))
    return out


def fit(seconds: float, slot: Slot, max_tempo: float = MAX_TEMPO) -> Fit:
    """The tempo that fits a line of `seconds` of speech into its slot, if a slight one does."""
    if seconds <= slot.room:
        return Fit(1.0, seconds, True)
    tempo = seconds / slot.room
    if tempo <= max_tempo:
        return Fit(round(tempo, 3), slot.room, True)
    return Fit(max_tempo, seconds / max_tempo, False)


def take_slots(tl: Timeline, subs: dict[str, str]) -> list[Slot]:
    """The take's lines in the short's time: the hero's speech starts after its recording's lead-in."""
    rows = [
        (
            line.who,
            line.text,
            subs[line.text],
            tl.to_cut(line.start) + (LEAD_IN_S if line.hero else 0.0),
            tl.to_cut(line.end),
        )
        for line in tl.lines
    ]
    return slots(rows, tl.cut_duration)


def english_provider(spec: str, locale: str) -> str:
    """The launcher's TTS provider spec for voices designed in `locale`: Inworld designs a voice per language
    (VoxCPM's are designed reading Russian), so another provider gives way to it."""
    name, _, rest = spec.partition(":")
    kept = rest.split(",") if name.strip() == "inworld" else []
    opts = [o for o in kept if o and not o.startswith("language=")]
    return "inworld:" + ",".join([*opts, f"language={locale}"])


def reference_count(durations: list[float], want: tuple[float, float] = REFERENCE_S) -> int:
    """How many of the recorded lines, from the first, make a reference of at least want[0] s and at most
    want[1] (with REFERENCE_GAP_S after each); at least one (the reference is cut to want[1] then)."""
    least, most = want
    total, n = 0.0, 0
    for d in durations:
        if n and total + d + REFERENCE_GAP_S > most:
            break
        total += d + REFERENCE_GAP_S
        n += 1
        if total >= least:
            break
    return max(n, 1)


def english_script(ws: Path, lang: str) -> list[str]:
    path = ws / f"script.{lang}.txt"
    if not path.is_file():
        raise DubError(f"no {path}: the translated script, a line per voiced line")
    rows = [row for row in path.read_text().splitlines() if row.strip()]
    return [" ".join(c.strip() for c in row.split(srt.CHUNK) if c.strip()) for row in rows]


# --- sound --------------------------------------------------------------------------------------------------


def _ffmpeg(args: list[str], what: str) -> str:
    done = subprocess.run(["ffmpeg", "-hide_banner", "-nostats", "-y", *args], capture_output=True, text=True)
    if done.returncode != 0:
        raise DubError(f"{what}: ffmpeg failed:\n{done.stderr.strip()[-2500:]}")
    return done.stderr


def _seconds(path: Path) -> float:
    with wave.open(str(path), "rb") as w:
        return w.getnframes() / w.getframerate()


def _probe(path: Path, stream: str = "a:0") -> float | None:
    done = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", stream, "-show_entries", "stream=duration",
         "-of", "csv=p=0", str(path)],
        capture_output=True, text=True,
    )  # fmt: skip
    try:
        return float(done.stdout.strip().splitlines()[0])
    except (ValueError, IndexError):
        return None


def trim(src: Path, dst: Path, chain: str = TRIM) -> float:
    """The line as 48 kHz mono with its silence trimmed (`chain`); returns its length."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    _ffmpeg(["-i", str(src), "-af", chain, "-ar", "48000", "-ac", "1", "-c:a", "pcm_s24le", str(dst)],
            f"trim {src.name}")  # fmt: skip
    return _seconds(dst)


def _clear_outputs(out: Path) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    for path in (out, out.with_suffix(".wav"), out.with_suffix(".levels.json")):
        path.unlink(missing_ok=True)


def _failed_outputs(out: Path, error: BaseException) -> None:
    out.unlink(missing_ok=True)
    out.with_suffix(".wav").unlink(missing_ok=True)
    report_path = out.with_suffix(".levels.json")
    if report_path.is_file():
        try:
            report = json.loads(report_path.read_text())
        except ValueError:
            report = {}
        if not isinstance(report, dict):
            report = {}
        if report.get("ok") is not False:
            # Also invalidate a successful master interrupted during mux/decode.
            report.update(ok=False, error=str(error) or type(error).__name__)
            report_path.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")


@dataclass
class _Publication:
    out: Path
    work: Path
    started: bool = False

    def start(self) -> None:
        """Replace previous outputs only once every input check has passed."""
        self.started = True
        _clear_outputs(self.out)


@contextmanager
def _publication(out: Path) -> Iterator[_Publication]:
    """Lock through input checks, then clean up only an assembly that started."""
    out.parent.mkdir(parents=True, exist_ok=True)
    # Keep this inode after release: unlinking it would let waiters lock different files.
    with out.with_name(f".{out.stem}.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise DubError(f"Дубляж уже собирается: {out}") from error
        publication = None
        try:
            with tempfile.TemporaryDirectory(prefix=f".{out.stem}-", dir=out.parent) as tmp:
                publication = _Publication(out, Path(tmp))
                yield publication
        except BaseException as error:
            if publication is not None and publication.started:
                _failed_outputs(out, error)
            raise
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)


def _file_version(info: os.stat_result) -> tuple[int, int, int, int, int]:
    return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns


def _published_russian(directory: Path, command: str, snapshot: Path) -> tuple[Path, dict, float]:
    ru = directory / "short-ru.mp4"
    invocation = command if command.endswith(".py") else f"gothic-shorts {command}"
    message = (f"Сначала опубликуйте русский ролик через {invocation}: "
               f"{ru} и его levels.json с ok: true")
    try:
        report_path = ru.with_suffix(".levels.json")
        report_version = _file_version(report_path.stat())
        with ru.open("rb") as source:
            version = _file_version(os.fstat(source.fileno()))
            report = json.loads(report_path.read_text())
            if not isinstance(report, dict) or report.get("ok") is not True:
                raise ValueError(message)
            with snapshot.open("wb") as destination:
                shutil.copyfileobj(source, destination)
            if (version != _file_version(os.fstat(source.fileno()))
                    or version != _file_version(ru.stat())
                    or report_version != _file_version(report_path.stat())):
                raise DubError("Русский ролик изменился при чтении; повторите дубляж после его публикации")
        duration = _probe(snapshot)
        if duration is None or not math.isfinite(duration) or duration <= 0:
            raise ValueError(message)
        _voice_mastered(report)
    except (OSError, ValueError, KeyError, TypeError, StopIteration) as error:
        raise DubError(message) from error
    return snapshot, report, duration


def _voice_mastered(report: dict) -> float:
    return float(next(row["measured_lufs"] for row in report["rows"] if row["id"] == "voice_mastered"))


def _check_duration(length: float, ru_duration: float) -> None:
    if abs(length - ru_duration) > 1 / 30 + 1e-6:
        raise DubError(f"Длина стемов {length:.6f} с расходится с short-ru.mp4 ({ru_duration:.6f} с) "
                       "больше чем на кадр; сначала заново опубликуйте русский ролик через "
                       "gothic-shorts cut / gothic-shorts explain")


def _join_picture(ru: Path, effects: Path, out: Path) -> Path:
    _ffmpeg(["-i", str(ru), "-i", str(effects), "-map", "0:v:0", "-map", "1:a:0",
             "-c", "copy", str(out)], "the Russian picture and effects")
    return out


def _english_stem(dub: Dub, line_slots: list[Slot], recordings: list[Path], names: list[str],
                  total: float, work: Path, *, clean: bool = False) -> tuple[Path, list[levels.Window]]:
    """Fit the lines before pooling each speaker's gain, then place them on cut time."""
    clips, fitted, windows = [], [], []
    lead = LEAD if clean else LEAD_S
    for index, (slot, raw) in enumerate(zip(line_slots, recordings, strict=True)):
        trimmed = work / f"line-{index:02d}.wav"
        duration = trim(raw, trimmed, clean_chain(raw, declick=False) if clean else TRIM)
        timing = fit(max(0.01, duration - lead), slot)
        path = work / f"fit-{index:02d}.wav"
        _ffmpeg(["-i", str(trimmed), "-af", f"atempo={timing.tempo:.4f}",
                 "-ar", "48000", "-ac", "1", "-c:a", "pcm_s24le", str(path)], "fit the English line")
        duration = _seconds(path)
        start = slot.start - lead / timing.tempo
        clips.append(stems.Clip(slot.who, path, start, start + duration))
        windows.append((slot.start, max(slot.start + 1 / stems.SAMPLE_RATE, start + duration)))
        fitted.append(timing)
    gains = stems.speaker_gains(clips)
    filters = []
    for index, (clip, slot, timing, name) in enumerate(zip(clips, line_slots, fitted, names, strict=True)):
        duration = clip.end - clip.start
        fade = min(stems.CLIP_FADE_S, duration / 4)
        trim_start = f"atrim=start={-clip.start:.9f},asetpts=PTS-STARTPTS," if clip.start < 0 else ""
        delay = max(0, round(clip.start * stems.SAMPLE_RATE))
        filters.append(
            f"[{index}:a]aformat=sample_rates=48000:channel_layouts=stereo,{levels.DUAL_MONO},"
            "asetpts=PTS-STARTPTS,"
            f"volume={gains[clip.who]:.9f}dB,adeclick=t=1:b=3,"
            f"afade=t=in:d={fade:.9f},afade=t=out:st={duration - fade:.9f}:d={fade:.9f},"
            f"{trim_start}adelay=delays={delay}S:all=1[v{index}]"
        )
        _report(dub, slot, timing, gains[clip.who], name)
    filters.append("".join(f"[v{index}]" for index in range(len(clips))) +
                   f"amix=inputs={len(clips)}:duration=longest:normalize=0,"
                   f"apad=whole_dur={total:.9f},atrim=duration={total:.9f},"
                   # An overlong final line can reach the body edge before its
                   # own fade. Fade that cut too, before the ending's silence.
                   f"afade=t=out:st={max(0, total - stems.CLIP_FADE_S):.9f}:"
                   f"d={min(stems.CLIP_FADE_S, total):.9f}[voice]")
    voice = work / "voice.en.wav"
    _ffmpeg([*_inputs([clip.path for clip in clips]), "-filter_complex", ";".join(filters),
             "-map", "[voice]", "-c:a", "pcm_f32le", str(voice)], "the English voice stem")
    return voice, levels.merge_speech_windows(windows, total)


def _publish(dub: Dub, ru: Path, joined: Path, music: Path | None, voice: Path,
             speech: list[levels.Window], total: float, length: float, out: Path, work: Path,
             **kwargs) -> None:
    print("  start who            length/slot  tempo  gain  text")
    for row in dub.rows:
        print(f"  {row}")
    report = levels.publish(joined, music, [(voice, 0.0)], speech, total, length, out=out, **kwargs)
    dub.lufs = _voice_mastered(report)
    preview = work / "preview.mp4"
    # Copy the checked AAC without another encode, alongside the original Russian AAC.
    _ffmpeg(["-i", str(out), "-i", str(ru), "-map", "0:v:0", "-map", "0:a:0", "-map", "1:a:0",
             "-c", "copy", "-metadata:s:a:0", "language=eng", "-metadata:s:a:1", "language=rus",
             "-disposition:a:0", "default", "-disposition:a:1", "0", "-movflags", "+faststart",
             str(preview)], "the bilingual preview")
    preview.replace(out)
    samples = round(dub.duration * stems.SAMPLE_RATE)
    _ffmpeg(["-i", str(out), "-map", "0:a:0", "-af", f"apad,atrim=end_sample={samples}",
             "-ar", "48000", "-ac", "2", "-c:a", "pcm_s24le", str(dub.wav)], "the checked AAC as WAV")
    dub.preview = out


def _key(*parts: object) -> str:
    return hashlib.sha1("\n".join(map(str, parts)).encode()).hexdigest()[:16]


# --- a take -------------------------------------------------------------------------------------------------


def npc_voices(config: Config, npcs: list[str]) -> dict[str, dict[str, str]]:
    """{npc: {"voice_id", "voice"}}: the voices the game speaks them in (`gothic-ai voices`)."""
    if not npcs:
        return {}
    repo, uv = tools(config)
    argv = [str(uv), "run", "--project", str(repo / "server"), "gothic-ai", "voices", *npcs]
    if game := config.settings.game.dir:
        argv += ["--game-dir", str(game)]
    done = subprocess.run(argv, capture_output=True, text=True, env=child_env({}))
    if done.returncode != 0:
        raise DubError(f"gothic-ai voices failed:\n{done.stderr.strip()[-2000:]}")
    return json.loads(done.stdout)


def dub_take(config: Config, shot: Shot, take: Path, lang: str = "en", provider: str | None = None) -> Dub:
    out = take / f"dub.{lang}.mp4"
    wav = out.with_suffix(".wav")
    with _publication(out) as publication:
        try:
            if lang not in LOCALES:
                raise DubError(f"no voices for {lang}: the dub speaks {', '.join(LOCALES)}")
            ru, ru_report, ru_duration = _published_russian(take, "cut", publication.work / "short-ru.mp4")
            for path in (take / "events.jsonl", take / "main.wav"):
                if not path.is_file():
                    raise DubError(f"the take has no {path.name}")
            cut._validate_recording(take)
            rows = read_events(take / "events.jsonl")
            tl = build(rows)
            if not tl.keep():
                raise DubError("the take has no recorded body")
            stems.collect(rows, tl)
            total = cut.body_duration(tl)
            sting_duration = cut._duration(cut.BRAND / "sting.mp4") if shot.brand else 0.0
            length = total + sting_duration
            _check_duration(length, ru_duration)
            subs = load_subs(take, lang, list(dict.fromkeys(line.text for line in tl.lines)))
            line_slots = take_slots(tl, subs)
            repo, _ = tools(config)
            work = take / f"dub-{lang}"
            publication.start()
            work.mkdir(exist_ok=True)
            effects, _ = cut.render_effects(shot, take / "main.wav", tl, work / "game.wav", sting_duration)
            joined = _join_picture(ru, effects, work / "joined.mkv")
            music = bed.render(shot.bed, shot.bed_start, total, work / "music.wav")

            npcs = sorted({line.who for line in tl.lines if not line.hero})
            described = {"hero": (VOICE_ID, shot.hero_voice)}
            for npc, voice in npc_voices(config, npcs).items():
                described[npc] = (voice["voice_id"], voice["voice"])
            said: dict[str, list[str]] = {}
            for line in tl.lines:
                said.setdefault(line.who, []).append(subs[line.text])
            recorded = english_voices(config, described, said, shorts_dir(repo) / "voice" / lang,
                                      lang, provider)
            dub = Dub(wav, None, ru_duration, ru_duration, None, _voice_mastered(ru_report))
            voice, speech = _english_stem(
                dub, line_slots, [recorded[(slot.who, slot.en)] for slot in line_slots],
                [line.name or "hero" for line in tl.lines], total, work,
            )
            _publish(dub, ru, joined, music, voice, speech, total, length, out, work)
            return dub
        except (CutError, bed.BedError, ValueError, OSError, subprocess.CalledProcessError) as error:
            raise DubError(str(error)) from error


def english_voices(
    config: Config,
    voices: dict[str, tuple[str, str]],
    lines: dict[str, list[str]],
    cache: Path,
    lang: str,
    tts: str | None = None,
) -> dict[tuple[str, str], Path]:
    """Each speaker's lines ({who: [text]}) in an English voice of its own ({who: (voice id, description)}),
    {(who, text): recording}, by the launcher's TTS provider, then its fallback (as the game falls back), or
    `tts`. Inworld clones the voice the game speaks Russian in (a sample of it voiced by the TTS service), so
    the English keeps its timbre, and designs an en-US one from the description when that fails; VoxCPM on
    the GPU server designs one reading an English sample (as the voxcpm bridge does with its Russian one).
    The lines are kept in `cache` by provider, voice and text; the voices themselves (the Russian samples
    Inworld clones, the ones VoxCPM designs) in VOICES, so a voice is cloned or designed once, whichever the
    checkout."""
    out: dict[tuple[str, str], Path] = {}
    for who, (voice_id, description) in voices.items():
        texts = list(dict.fromkeys(lines.get(who, [])))
        if not texts:
            continue
        errors = []
        for way, make in _ways(config, voice_id, description, texts, cache, lang, tts):
            try:
                wanted = make()
            except (VoiceError, GpuError, DubError) as err:
                errors.append(f"{way}: {str(err).strip().splitlines()[-1]}")
                print(f"{who}: {errors[-1]}; the next way")
                continue
            print(f"{who}: {len(texts)} line(s) in {lang}, {way}")
            out.update({(who, t): p for t, p in wanted.items()})
            break
        else:
            raise DubError(f"no {lang} voice for {who}:\n  " + "\n  ".join(errors))
    return out


def _providers(config: Config, tts: str | None) -> list[str]:
    specs = [tts] if tts else [config.settings.tts.provider, config.settings.tts.fallback]
    return [p for p in dict.fromkeys(specs) if p and p != "none"]


def _ways(config: Config, voice_id: str, description: str, texts: list[str], cache: Path, lang: str,
          tts: str | None) -> list[tuple[str, Callable[[], dict[str, Path]]]]:  # fmt: skip
    """The ways to voice a speaker's lines, in order: each makes the missing recordings and returns them."""

    def missing(wanted: dict[str, Path]) -> dict[str, Path]:
        return {t: p for t, p in wanted.items() if not p.is_file()}

    def inworld_clone(spec: str) -> dict[str, Path]:
        russian = VOICES / f"{voice_id}-{_key(spec, description, RUSSIAN_SAMPLE)}.ru.wav"
        if not russian.is_file():  # the game's own voice: the same id and description, so the same voice
            say(config, voice_id, description, {RUSSIAN_SAMPLE: russian}, provider=spec, fallback="none")
        en = english_provider(spec, LOCALES[lang])
        sample = _key(russian.read_bytes())
        wanted = {t: cache / f"{_key(en, 'clone', sample, t)}.wav" for t in texts}
        say(config, f"{voice_id}@{lang}~ru", description, missing(wanted), provider=en, fallback="none",
            clone=russian)  # fmt: skip
        return wanted

    def inworld_design(spec: str) -> dict[str, Path]:
        en = english_provider(spec, LOCALES[lang])
        wanted = {t: cache / f"{_key(en, voice_id, description, t)}.wav" for t in texts}
        say(config, f"{voice_id}@{lang}", description, missing(wanted), provider=en, fallback="none")
        return wanted

    def voxcpm_design() -> dict[str, Path]:
        ref = VOICES / f"{voice_id}-{_key(description, SAMPLES[lang])}.ref.wav"
        wanted = {t: cache / f"{_key('voxcpm', ref.name, t)}.wav" for t in texts}
        design = {"description": description, "sample": SAMPLES[lang], "seed": _seed(description)}
        voxcpm(ref, missing(wanted), cache, design)
        return wanted

    def unknown(name: str) -> dict[str, Path]:
        raise DubError(f"{name} makes no voices in {lang} (inworld or voxcpm do)")

    ways: list[tuple[str, Callable[[], dict[str, Path]]]] = []
    for spec in _providers(config, tts):
        name = _name(spec)
        if name == "inworld":
            ways += [
                ("its Russian voice cloned by Inworld", partial(inworld_clone, _russian(spec))),
                ("a voice designed by Inworld", partial(inworld_design, spec)),
            ]
        elif name == "voxcpm":
            ways.append(("a voice designed by VoxCPM", voxcpm_design))
        else:
            ways.append((name, partial(unknown, name)))
    return ways


def _name(spec: str) -> str:
    return spec.partition(":")[0].strip()


def _russian(spec: str) -> str:
    """Inworld's spec as the game has it: its own options, the language left at Russian."""
    opts = [o for o in spec.partition(":")[2].split(",") if o and not o.startswith("language=")]
    return ":".join(["inworld", ",".join(opts)]) if opts else "inworld"


def _seed(description: str) -> int:
    """The voice's seed, as the voxcpm bridge has it: a description designs the same voice every time."""
    return int(hashlib.sha1(description.encode()).hexdigest()[:10], 16) % 2**31


def voxcpm(ref: Path, lines: dict[str, Path], work: Path, design: dict | None = None) -> None:
    """Voices each text into its file in the voice of `ref`, in one job on the GPU server (VoxCPM2 of
    gothic-voxcpm, shorts/deploy/dub/clone.py); a missing `ref` is designed there first from `design`
    ({"description", "sample", "seed"}) and kept."""
    if not lines:
        return
    if not ref.is_file() and design is None:
        raise DubError(f"no {ref}")
    work.mkdir(parents=True, exist_ok=True)
    ids = {f"{i:02d}": (text, path) for i, (text, path) in enumerate(lines.items(), start=1)}
    spec = work / "lines.json"
    spec.write_text(json.dumps([{"id": k, "text": t} for k, (t, _) in ids.items()], ensure_ascii=False))
    send, fetch, extra = [spec], {f"out/{k}.wav": p for k, (_, p) in ids.items()}, ""
    if ref.is_file():
        send.append(ref)
    else:
        how = work / "design.json"
        how.write_text(json.dumps(design, ensure_ascii=False))
        send.append(how)
        fetch[ref.name] = ref
        extra = " --design {dir}/design.json"
    log = job(
        send,
        "if [ ! -x ~/gothic-ai/dub/clone.sh ]; then echo 'no ~/gothic-ai/dub/clone.sh: run "
        "scripts/deploy-gpu.sh dub' >&2; exit 1; fi; "
        f"~/gothic-ai/dub/clone.sh {{dir}}/{ref.name} {{dir}}/lines.json {{dir}}/out{extra}",
        fetch,
        "VoxCPM2 on the GPU server",
    )
    print(log.strip())


def _report(dub: Dub, slot: Slot, f: Fit, gain: float, who: str) -> None:
    tempo = "" if f.tempo == 1.0 else f" x{f.tempo:.2f}"
    dub.rows.append(f"{slot.start:6.2f} {who[:14]:14} {f.seconds:4.1f}/{slot.room:4.1f} s{tempo:6} "
                    f"{gain:+5.1f} dB  {slot.en}")  # fmt: skip
    if not f.fits:
        dub.too_long.append(
            f"{slot.start:.1f} s, {who}: {f.seconds:.1f} s at x{f.tempo:.2f} for {slot.room:.1f} s: {slot.en}"
        )


# --- an explainer -------------------------------------------------------------------------------------------


def clone(
    config: Config, ws: Path, texts_en: list[str], lang: str, tts: str | None = None, reclone: bool = False
) -> list[Path]:
    """The lines in the user's own voice: by Inworld through the TTS service (the clone USER of the recording
    user.inworld.wav), else by VoxCPM2 on the GPU server (user.voxcpm.wav), in the launcher's order or by
    `tts`. The recordings are kept in VOICES for every short (user_voice); `reclone` makes them anew from this
    explainer's. A line is voiced again only when its text, the recording or the provider change."""
    work = ws / "voice" / lang
    raw = work / "raw"
    raw.mkdir(parents=True, exist_ok=True)
    if reclone:
        for provider, want in (("inworld", REFERENCE_INWORLD_S), ("voxcpm", REFERENCE_S)):
            user_voice(ws, provider, want, anew=True)
    out = [raw / f"{i:02d}.wav" for i in range(1, len(texts_en) + 1)]
    errors = []
    for spec in _providers(config, tts):
        name = _name(spec)
        try:
            if name == "inworld":
                ref = user_voice(ws, name, REFERENCE_INWORLD_S)
                keys = [_key(name, _digest(ref), text) for text in texts_en]
                todo = [i for i, (wav, key) in enumerate(zip(out, keys, strict=True)) if not _fresh(wav, key)]
                say(config, USER_VOICE, "the author's own voice", {texts_en[i]: out[i] for i in todo},
                    provider=english_provider(spec, LOCALES[lang]), fallback="none", clone=ref)  # fmt: skip
            elif name == "voxcpm":
                ref = user_voice(ws, name, REFERENCE_S)
                keys = [_key(name, _digest(ref), text) for text in texts_en]
                todo = [i for i, (wav, key) in enumerate(zip(out, keys, strict=True)) if not _fresh(wav, key)]
                voxcpm(ref, {texts_en[i]: out[i] for i in todo}, work)
            else:
                raise DubError(f"{name} clones no voices (inworld or voxcpm do)")
        except (VoiceError, GpuError, DubError) as err:
            errors.append(f"{name}: {str(err).strip().splitlines()[-1]}")
            print(f"the user's voice: {errors[-1]}; the next provider")
            continue
        for i in todo:
            out[i].with_suffix(".key").write_text(keys[i])
        print(f"the user's voice cloned by {name}: {len(todo)} line(s) voiced, {len(out) - len(todo)} kept")
        return out
    raise DubError("no clone of the user's voice:\n  " + "\n  ".join(errors))


def user_voice(ws: Path, provider: str, want: tuple[float, float], anew: bool = False) -> Path:
    """The user's recorded voice as `provider` clones it, VOICES/user.<provider>.wav, kept for every short so
    that it is cloned once: made from this explainer's first recorded lines (voice/clean/, `want` seconds of
    them) only when it is not there, or `anew` (gothic-shorts dub --reclone)."""
    ref = VOICES / f"user.{provider}.wav"
    if ref.is_file() and not anew:
        return ref
    clean = sorted((ws / "voice/clean").glob("[0-9][0-9].wav"))
    if not clean:
        raise DubError(f"no recorded lines in {ws / 'voice/clean'} for {ref}: gothic-shorts words {ws.name}")
    takes = clean[: reference_count([_seconds(c) for c in clean], want)]
    pads = "".join(f"[{i}:a]apad=pad_dur={REFERENCE_GAP_S}[p{i}];" for i in range(len(takes)))
    joined = pads + "".join(f"[p{i}]" for i in range(len(takes)))
    joined += f"concat=n={len(takes)}:v=0:a=1,atrim=end={want[1]}"
    ref.parent.mkdir(parents=True, exist_ok=True)
    part = ref.with_name(ref.stem + ".part.wav")
    _ffmpeg([*_inputs(takes), "-filter_complex", joined, "-ar", "48000", "-ac", "1", "-c:a", "pcm_s16le",
             str(part)], "the voice's reference")  # fmt: skip
    part.replace(ref)
    print(f"the user's voice for {provider}: {ref}, {_seconds(ref):.1f} s of {ws.name}'s lines "
          f"{takes[0].stem}-{takes[-1].stem}")  # fmt: skip
    return ref


def _digest(path: Path) -> str:
    return hashlib.sha1(path.read_bytes()).hexdigest()[:16]


def dub_explainer(
    config: Config,
    ws: Path,
    lang: str = "en",
    plan_file: str = "plan.json",
    tts: str | None = None,
    reclone: bool = False,
) -> Dub:
    ws = ws.resolve()
    out = ws / f"render/dub.{lang}.mp4"
    wav = out.with_suffix(".wav")
    with _publication(out) as publication:
        work = publication.work
        try:
            if lang not in LOCALES:
                raise DubError(f"no voices for {lang}: the dub speaks {', '.join(LOCALES)}")
            ru, ru_report, ru_duration = _published_russian(out.parent, "explain", work / "short-ru.mp4")
            said = json.loads((ws / "words.ru.json").read_text())["lines"]
            plan = json.loads((ws / plan_file).read_text())
            _, total, length = explain.speech_windows(plan)
            _check_duration(length, ru_duration)
            texts_en = english_script(ws, lang)
            if len(texts_en) != len(said):
                raise DubError(f"script.{lang}.txt has {len(texts_en)} lines for {len(said)} said ones")
            rows = [
                ("voice", " ".join(word["text"] for word in line["words"]), en,
                 line["words"][0]["start"], line["words"][-1]["end"])
                for line, en in zip(said, texts_en, strict=True)
            ]
            line_slots = slots(rows, total)
            music = plan.get("music")
            fades = ((float(music.get("fadeIn", 1.0)), float(music.get("fadeOut", 1.5)))
                     if music is not None else (0.0, 0.0))
            dub = Dub(wav, None, ru_duration, ru_duration, None, _voice_mastered(ru_report))
            publication.start()
            explain.render(ws, plan, work, stems=("game", "music"))
            joined = _join_picture(ru, work / "game.wav", work / "joined.mkv")
            voiced = clone(config, ws, texts_en, lang, tts, reclone)
            voice, speech = _english_stem(
                dub, line_slots, voiced, ["voice"] * len(rows), total, work, clean=True,
            )
            _publish(dub, ru, joined, work / "music.wav" if music is not None else None,
                     voice, speech, total, length, out, work,
                     music_fades=False, music_fade_durations=fades)
            return dub
        except (explain.ExplainError, ValueError, OSError, KeyError, IndexError,
                TypeError, subprocess.CalledProcessError) as error:
            raise DubError(str(error)) from error


def dub_compare(
    config: Config,
    ws: Path,
    lang: str = "en",
    plan_file: str = "plan.json",
    tts: str | None = None,
    reclone: bool = False,
    takes: str | Path | None = None,
) -> Dub:
    """Dub a comparison over its original take effects and brand ending."""
    ws = ws.resolve()
    out = ws / f"render/dub.{lang}.mp4"
    with _publication(out) as publication:
        work = publication.work
        try:
            if lang not in LOCALES:
                raise DubError(f"no voices for {lang}: the dub speaks {', '.join(LOCALES)}")
            ru, ru_report, ru_duration = _published_russian(
                out.parent, "shorts/compare/cut.py", work / "short-ru.mp4",
            )
            plan = json.loads((ws / plan_file).read_text())
            source = takes if takes is not None else plan.get("takes")
            if not source:
                raise DubError("Укажите папку дублей сравнения: --takes DIR или ключ takes в плане")
            takes_dir = Path(source).expanduser()
            if not takes_dir.is_dir():
                raise DubError(f"Нет папки дублей {takes_dir}: укажите --takes DIR")
            required = [takes_dir / side / f"{beat['segment']}.{ext}"
                        for beat in plan["beats"] for side, ext in
                        (("off", "mp4"), ("on", "mp4"), ("on", "wav"))]
            music = plan.get("music")
            if music:
                required.append(takes_dir / music["file"])
            brand = plan.get("brand", True)
            if brand:
                required.append(compare.BRAND / "sting.mp4")
            for path in required:
                if not path.is_file():
                    raise DubError(f"Нет файла сравнения: {path}")
            _, total = levels.speech_windows(plan)
            # AAC padding and frame rounding can change the ending's duration.
            # Measure the same encode as the Russian builder, before replacing outputs.
            ending_length = compare.build_ending(work)[1] if brand else 0.0
            length = total + ending_length
            _check_duration(length, ru_duration)
            texts_en = english_script(ws, lang)
            said = compare.placed_lines(plan)
            if len(texts_en) != len(said):
                raise DubError(f"script.{lang}.txt has {len(texts_en)} lines for {len(said)} said ones")
            if not said:
                raise DubError("В плане сравнения нет озвученных строк")
            rows = [
                ("voice", " ".join(word["text"] for word in line["words"]), text,
                 line["words"][0]["start"], line["words"][-1]["end"])
                for line, text in zip(said, texts_en, strict=True)
            ]
            line_slots = slots(rows, total)
            dub = Dub(out.with_suffix(".wav"), None, ru_duration, ru_duration,
                      None, _voice_mastered(ru_report))
            publication.start()
            built = compare.build(plan, takes_dir, "ru", work)
            joined = _join_picture(ru, built.joined, work / "picture.mp4")
            voiced = clone(config, ws, texts_en, lang, tts, reclone)
            voice, speech = _english_stem(
                dub, line_slots, voiced, ["voice"] * len(rows), total, work, clean=True,
            )
            _publish(dub, ru, joined, built.music, voice, speech, built.total, built.length,
                     out, work, music_fades=True)
            return dub
        except (CutError, ValueError, OSError, KeyError, IndexError,
                TypeError, subprocess.CalledProcessError) as error:
            raise DubError(str(error)) from error


def _inputs(files: list[Path]) -> list[str]:
    return [arg for f in files for arg in ("-i", str(f))]


def _fresh(wav: Path, key: str) -> bool:
    stamp = wav.with_suffix(".key")
    return wav.is_file() and stamp.is_file() and stamp.read_text() == key
