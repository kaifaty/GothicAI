"""`gothic-shorts`: shorts from the game, see shorts/README.md."""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

from gothic_launcher.commands import LaunchError, find_layout, load_config, tools
from gothic_launcher.settings import Config, SettingsError

from . import compare, cover, srt, youtube
from . import cut as cutting
from .align import AlignError, align
from .dub import DubError, dub_compare, dub_explainer, dub_take
from .explain import ExplainError
from .explain import publish as publish_explainer
from .film import FilmError, film, shorts_dir
from .gpu import GpuError
from .shot import Shot, ShotError, load
from .studio import StudioError, serve, workspace
from .timeline import build, read_events
from .voice import VoiceError, voice_lines
from .words import prepare
from .youtube import YouTubeError

SHOTS = Path(__file__).resolve().parents[2] / "shots"


def _shot(arg: str) -> Shot:
    path = Path(arg)
    if not path.is_file():
        path = SHOTS / f"{arg}.toml"
    if not path.is_file():
        raise ShotError(f"no shot {arg} (a .toml file, or a name from {SHOTS})")
    return load(path)


def _config() -> Config:
    config, _ = load_config()
    return config


def _take(config: Config, shot: Shot, arg: str | None) -> Path:
    if arg and Path(arg).is_dir():
        return Path(arg)
    repo, _ = tools(config)
    takes = sorted((shorts_dir(repo) / shot.name).glob("take-*"))
    if arg:
        want = f"take-{int(arg):02d}" if arg.isdigit() else arg
        for t in takes:
            if t.name == want:
                return t
        raise ShotError(f"no take {arg} of {shot.name}")
    if not takes:
        raise ShotError(f"{shot.name} has no takes yet: gothic-shorts film {shot.name}")
    return takes[-1]


def cmd_voice(args: argparse.Namespace) -> int:
    shot, config = _shot(args.shot), _config()
    repo, _ = tools(config)
    voiced = voice_lines(config, shorts_dir(repo) / "voice", shot.hero_voice, shot.hero_lines)
    for text, path in voiced.items():
        print(f"{path}  {text}")
    return 0


def cmd_film(args: argparse.Namespace) -> int:
    shot, config = _shot(args.shot), _config()
    layout = find_layout(config)
    decider = args.decider_url or os.environ.get("GOTHIC_DECIDER_URL")
    for _ in range(args.takes):
        take = film(shot, config, layout, decider_url=decider, verbose=args.verbose)
        _summary(take)
        if args.cut:
            print(cutting.cut(shot, take, args.lang))
    return 0


def cmd_cut(args: argparse.Namespace) -> int:
    shot, config = _shot(args.shot), _config()
    take = _take(config, shot, args.take)
    out = cutting.cut(shot, take, args.lang, Path(args.out) if args.out else None)
    print(out)
    print(out.with_suffix(".md").read_text())
    return 0


def cmd_cover(args: argparse.Namespace) -> int:
    source = Path(args.source)
    out = Path(args.out) if args.out else source.with_name("cover.jpg")
    print(cover.save(cover.render(cover.frame(source, args.at), args.title, args.sub), out))
    return 0


def cmd_subs(args: argparse.Namespace) -> int:
    shot, config = _shot(args.shot), _config()
    take = _take(config, shot, args.take)
    print(cutting.write_subs_template(shot, take, args.lang))
    return 0


def cmd_srt(args: argparse.Namespace) -> int:
    """A shot's take, or an explainer/comparison workspace: timed Russian voice and translated script."""
    try:
        shot = _shot(args.short)
    except ShotError:
        shot = None
    if shot is not None:
        print(cutting.subtitles(shot, _take(_config(), shot, args.take), args.lang))
        return 0
    ws = workspace(args.short)
    script = ws / f"script.{args.lang}.txt"
    if not script.is_file():
        raise StudioError(f"{args.short} is no shot, and its workspace has no {script.name}")
    try:
        plan = json.loads((ws / args.plan).read_text())
    except (OSError, ValueError):
        plan = {}
    try:
        if compare.is_compare_plan(plan):
            timed = {"lines": compare.placed_lines(plan)}
            if not timed["lines"]:
                raise StudioError(f"В плане сравнения {ws / args.plan} нет озвученных строк")
        else:
            words = ws / "words.ru.json"
            if not words.is_file():
                raise StudioError(f"{args.short} is no shot, and its workspace has no {words.name}")
            timed = json.loads(words.read_text())
        cues = srt.explainer(timed, script.read_text().strip().splitlines())
    except ValueError as err:
        raise StudioError(f"{script}: {err}") from err
    out = ws / f"subs.{args.lang}.srt"
    out.write_text(srt.render(cues))
    print(out)
    return 0


def cmd_dub(args: argparse.Namespace) -> int:
    """A shot's take, or an explainer/comparison workspace: its English sound track"""
    try:
        shot = _shot(args.short)
    except ShotError:
        shot = None
    if shot is not None:
        config = _config()
        done = dub_take(config, shot, _take(config, shot, args.take), args.lang, args.tts)
    else:
        ws = workspace(args.short)
        try:
            plan = json.loads((ws / args.plan).read_text())
        except (OSError, ValueError):
            plan = {}  # Let the publisher validate its inputs under the publication lock.
        if compare.is_compare_plan(plan):
            done = dub_compare(_config(), ws, args.lang, args.plan, args.tts, args.reclone, args.takes)
        else:
            done = dub_explainer(_config(), ws, args.lang, args.plan, args.tts, args.reclone)
    ru = f"{done.ru_duration:.3f} s" if done.ru_duration is not None else "no Russian short to compare"
    print(f"{done.wav}: {done.duration:.3f} s (the Russian short: {ru}), "
          f"voice_mastered: EN {_lufs(done.lufs)}, RU {_lufs(done.ru_lufs)}")
    if done.preview:
        print(f"{done.preview}: the Russian short with it as the first sound track, the Russian second")
    if done.too_long:
        print("too long even sped up: shorten them in the translation and run dub again")
        for row in done.too_long:
            print(f"  {row}")
        return 1
    return 0


def _lufs(value: float | None) -> str:
    return "?" if value is None else f"{value:.1f} LUFS"


def cmd_align(args: argparse.Namespace) -> int:
    wav = Path(args.wav)
    if (args.text is None) == (args.text_file is None):
        raise AlignError("give the text, or --text-file")
    try:
        text = args.text if args.text is not None else Path(args.text_file).read_text()
    except OSError as err:
        raise AlignError(f"can not read the text: {err}") from err
    out = Path(args.out) if args.out else wav.with_suffix(".words.json")
    if out.suffix != ".json":
        out = out.with_name(out.name + ".json")
    done = align(wav, text, out, args.language)
    for w in done["words"]:
        print(f"  {w['start']:6.2f} {w['end']:6.2f}  {w['text']}")
    print(f"{out}: {len(done['words'])} words over {done['duration']:.2f} s of audio; "
          f"{done['ssh_s']:.1f} s on the GPU server ({done['metrics'] or 'no metrics'})")
    return 0


def cmd_studio(args: argparse.Namespace) -> int:
    serve(workspace(args.short), args.lang, args.port, open_browser=not args.no_browser)
    return 0


def cmd_words(args: argparse.Namespace) -> int:
    done = prepare(workspace(args.short), args.lang, args.language)
    for line in done["lines"]:
        w = line["words"]
        print(f"{line['n']:2d} {w[0]['start']:6.2f}-{w[-1]['end']:6.2f}  " + " ".join(x["text"] for x in w))
    print(f"words.{done['lang']}.json: {len(done['lines'])} lines, {done['duration']:.1f} s")
    return 0


def cmd_explain(args: argparse.Namespace) -> int:
    print(publish_explainer(workspace(args.short), args.plan, Path(args.out) if args.out else None))
    return 0


def cmd_youtube_login(args: argparse.Namespace) -> int:
    token = youtube.sign_in()
    cid, title = youtube.channel(token["access_token"])
    print(f"signed in: channel {title} ({cid}); token in {youtube.TOKEN}")
    if left := token.get("refresh_token_expires_in"):
        print(f"the app is in testing: sign in again in {left / 86400:.0f} days "
              f"(or publish it: Google Auth Platform, Audience, Publish app)")
    else:
        print("the token does not expire")
    return 0


def _captions(given: list[str]) -> dict[str, Path]:
    """--captions en=subs.en.srt ..."""
    out: dict[str, Path] = {}
    for item in given:
        lang, sep, file = item.partition("=")
        if not sep or not lang or not file:
            raise YouTubeError(f"--captions {item}: give it as <lang>=<file>, e.g. en=subs.en.srt")
        if not Path(file).is_file():
            raise YouTubeError(f"no subtitles {file}")
        out[lang] = Path(file)
    return out


def _channel(expected: str) -> str:
    token = youtube.access_token()
    cid, title = youtube.channel(token)
    youtube.check_channel(cid, title, expected)
    print(f"channel: {title} ({cid})")
    return token


def _add_captions(vid: str, captions: dict[str, Path]) -> None:
    for lang, file in captions.items():
        try:
            youtube.add_captions(youtube.access_token(), vid, lang, file)
        except YouTubeError as err:
            raise YouTubeError(f"{err}\nthe video is up; add the track again with: "
                               f"gothic-shorts captions {vid} {file} --lang {lang}") from err
        print(f"captions: {lang} ({file.name})")


def cmd_upload(args: argparse.Namespace) -> int:
    video = Path(args.video)
    if not video.is_file():
        raise YouTubeError(f"no video {video}")
    thumbnail = Path(args.thumbnail) if args.thumbnail else None
    if thumbnail and not thumbnail.is_file():
        raise YouTubeError(f"no thumbnail {thumbnail}")
    meta = youtube.load_meta(Path(args.meta), args.lang)
    meta.privacy = args.privacy
    captions = _captions(args.captions or [])
    token = _channel(args.channel)
    if captions:
        youtube.check_captions_scope(youtube.granted())
    print(f"video:   {video} ({video.stat().st_size / 1e6:.1f} MB)")
    print(f"title:   {meta.title}")
    print(f"sound:   {youtube.AUDIO_LANGUAGE}; title and description also in: "
          f"{', '.join(meta.localizations) or 'no other language'}")
    print(f"captions: {', '.join(f'{k} ({v.name})' for k, v in captions.items()) or 'none'}")
    print(f"privacy: {meta.privacy}, synthetic media: {meta.synthetic}")
    if not args.yes and input("upload? [y/N] ").strip().lower() != "y":
        print("not uploaded")
        return 1
    vid = youtube.upload(video, meta, token)
    print(f"uploaded: https://youtu.be/{vid}  (id {vid})")
    if thumbnail:
        try:
            youtube.set_thumbnail(youtube.access_token(), vid, thumbnail)
        except YouTubeError as err:
            print(f"thumbnail: {err}", file=sys.stderr)
            print("the video is up; choose a cover frame in the YouTube mobile app", file=sys.stderr)
        else:
            print(f"thumbnail: {thumbnail}")
    got = youtube.privacy(youtube.access_token(), vid)
    if got != meta.privacy:
        raise YouTubeError(f"YouTube made the video {got}, not {meta.privacy}: the project may need "
                           f"YouTube's audit (an email about a video locked as private says so)")
    print(f"privacy: {got}")
    if meta.localizations:
        missing = set(meta.localizations) - youtube.translated(youtube.access_token(), vid)
        if missing:
            raise YouTubeError(f"YouTube kept no translation into {', '.join(sorted(missing))}")
        print(f"translations: {', '.join(meta.localizations)}")
    _add_captions(vid, captions)
    return 0


def cmd_thumbnail(args: argparse.Namespace) -> int:
    file = Path(args.file)
    if not file.is_file():
        raise YouTubeError(f"no thumbnail {file}")
    token = _channel(args.channel)
    print(f"video:   https://youtu.be/{args.video}")
    print(f"thumbnail: {file}")
    if not args.yes and input("set the thumbnail? [y/N] ").strip().lower() != "y":
        print("not set")
        return 1
    youtube.set_thumbnail(token, args.video, file)
    print(f"thumbnail set: {file}")
    return 0


def cmd_translate(args: argparse.Namespace) -> int:
    meta = youtube.load_meta(Path(args.meta), args.lang)
    if not meta.localizations:
        raise YouTubeError(f"{args.meta} has no language besides {args.lang}")
    token = _channel(args.channel)
    if youtube.CAPTIONS_SCOPE not in youtube.granted():
        raise YouTubeError("the sign-in can not change a video (it has no youtube.force-ssl): "
                           "gothic-shorts youtube login")
    print(f"video:   https://youtu.be/{args.video}")
    for lang, text in meta.localizations.items():
        print(f"{lang}:      {text['title']}")
    if not args.yes and input("set these translations? [y/N] ").strip().lower() != "y":
        print("not set")
        return 1
    have = youtube.set_translations(token, args.video, meta.localizations)
    missing = set(meta.localizations) - have
    if missing:
        raise YouTubeError(f"YouTube kept no translation into {', '.join(sorted(missing))}")
    print(f"translations: {', '.join(sorted(have))}")
    return 0


def cmd_captions(args: argparse.Namespace) -> int:
    captions = _captions([f"{args.lang}={args.file}"])
    _channel(args.channel)
    youtube.check_captions_scope(youtube.granted())
    print(f"video:   https://youtu.be/{args.video}")
    if not args.yes and input(f"add the {args.lang} captions? [y/N] ").strip().lower() != "y":
        print("not added")
        return 1
    _add_captions(args.video, captions)
    return 0


def _summary(take: Path) -> None:
    events = take / "events.jsonl"
    if not events.is_file():
        print(f"{take}: no events (did the scenario start? see {take / 'logs'})")
        return
    tl = build(read_events(events))
    print(f"{take}: {tl.duration:.1f} s recorded, {tl.cut_duration:.1f} s after the cut")
    heard = iter(tl.heard)
    for line in tl.lines:
        if line.hero:
            print(f"  {line.start:5.1f} hero: {line.text}  [heard: {next(heard, '?')}]")
        else:
            print(f"  {line.start:5.1f} {line.name}: {line.text}")
    if tl.timeouts:
        print(f"  waiting for an answer timed out {tl.timeouts} time(s)")
    log = take / "bench.log"
    if log.is_file():
        # a stray byte of the game's code page must not lose the summary of a finished take
        for row in log.read_text(encoding="utf-8", errors="replace").splitlines():
            if row.startswith(("realtime:", "ptt: not", "can not", "unknown", "near: no", "cam ")):
                print(f"  bench: {row}")


def main() -> None:
    p = argparse.ArgumentParser(prog="gothic-shorts", description=__doc__)
    sub = p.add_subparsers(dest="command", required=True)

    s = sub.add_parser("voice", help="voice the hero's lines of a shot (done by film too)")
    s.add_argument("shot", help="a shot file, or a name from shorts/shots/")
    s.set_defaults(fn=cmd_voice)

    s = sub.add_parser("film", help="film takes of a shot with the AI services")
    s.add_argument("shot")
    s.add_argument("--takes", type=int, default=1, help="how many takes, one after another")
    s.add_argument("--cut", action="store_true", help="cut each take right away")
    s.add_argument("--lang", default="ru", help="language of the cut (with --cut)")
    s.add_argument("--decider-url", help="npc decision service (default: GOTHIC_DECIDER_URL); off without it")
    s.add_argument("-v", "--verbose", action="store_true", help="print the services' and the game's output")
    s.set_defaults(fn=cmd_film)

    s = sub.add_parser("cut", help="cut a take into a short")
    s.add_argument("shot")
    s.add_argument("take", nargs="?", help="take folder or number (default: the newest)")
    s.add_argument("--lang", default="ru", help="ru, or a language with subs.<lang>.json in the take")
    s.add_argument("--out", help="the output file (default: <take>/short-<lang>.mp4)")
    s.set_defaults(fn=cmd_cut)

    s = sub.add_parser("cover", help="render the channel's cover from a video frame or an image")
    s.add_argument("source", help="a video or image")
    s.add_argument("--at", type=float, default=0, help="second in the video (default: 0)")
    s.add_argument("--title", required=True, help="the topic, rendered in uppercase")
    s.add_argument("--sub", help="optional second line in gold")
    s.add_argument("--out", help="the JPEG output (default: cover.jpg beside the input)")
    s.set_defaults(fn=cmd_cover)

    s = sub.add_parser("subs", help="write subs.<lang>.json of a take: the texts to translate")
    s.add_argument("shot")
    s.add_argument("take", nargs="?")
    s.add_argument("--lang", default="en")
    s.set_defaults(fn=cmd_subs)

    s = sub.add_parser("srt", help="the short's texts in another language as a caption track (SubRip): "
                                   "<take>/subs.<lang>.srt, or subs.<lang>.srt in a workspace")
    s.add_argument("short", help="a shot (its take's subs.<lang>.json), or a workspace (script.<lang>.txt)")
    s.add_argument("take", nargs="?", help="a shot's take folder or number (default: the newest)")
    s.add_argument("--lang", default="en")
    s.add_argument("--plan", default="plan.json", help="the workspace's comparison plan (default: plan.json)")
    s.set_defaults(fn=cmd_srt)

    s = sub.add_parser("dub", help="the English sound track of a shot, explainer or comparison")
    s.add_argument("short", help="a shot (its newest take, or `take`), or a workspace or name")
    s.add_argument("take", nargs="?", help="the shot's take folder or number")
    s.add_argument("--lang", default="en", help="the track's language: en")
    s.add_argument("--tts", help="inworld or voxcpm (default: the launcher's provider, then its fallback)")
    s.add_argument("--plan", default="plan.json", help="the workspace's plan, the one of its Russian render")
    s.add_argument("--takes", help="comparison takes folder (overrides takes in the plan)")
    s.add_argument("--reclone", action="store_true",
                   help="an explainer: the user's voice made anew from its recordings (cloned again)")
    s.set_defaults(fn=cmd_dub)

    s = sub.add_parser("align", help="word timings of a known text in a recording (on the GPU server)")
    s.add_argument("wav", help="the recording (any format ffmpeg reads)")
    s.add_argument("text", nargs="?", help="exactly what is said")
    s.add_argument("--text-file", help="the text in a file instead (line breaks are spaces)")
    s.add_argument("--language", default="Russian", help="the text's language: a name or a code (ru)")
    s.add_argument("--out", help="the words' JSON (default: <wav>.words.json)")
    s.set_defaults(fn=cmd_align)

    s = sub.add_parser("studio", help="record the voice of an explainer: a teleprompter over its draft video")
    s.add_argument("short", help="the short's folder, or its name in ~/.local/share/gothic-ai/explain")
    s.add_argument("--lang", default="ru", help="the script: script.<lang>.txt (ru)")
    s.add_argument("--port", type=int, default=47871)
    s.add_argument("--no-browser", action="store_true", help="do not open the browser")
    s.set_defaults(fn=cmd_studio)

    s = sub.add_parser("words", help="cleaned, aligned, laid out voice of an explainer: words.<lang>.json")
    s.add_argument("short", help="the short's folder, or its name in ~/.local/share/gothic-ai/explain")
    s.add_argument("--lang", default="ru", help="the script: script.<lang>[.said].txt (ru)")
    s.add_argument("--language", default="Russian", help="the aligner's language (Russian)")
    s.set_defaults(fn=cmd_words)

    s = sub.add_parser("explain", help="render and publish an explainer with calibrated, checked audio")
    s.add_argument("short", help="the short's folder, or its name in ~/.local/share/gothic-ai/explain")
    s.add_argument("--plan", default="plan.json", help="the plan in the workspace (default: plan.json)")
    s.add_argument("--out", help="the output file (default: <workspace>/render/short-<plan lang>.mp4)")
    s.set_defaults(fn=cmd_explain)

    s = sub.add_parser("youtube", help="YouTube: sign in (the browser opens), the channel, the token's life")
    s.add_argument("action", choices=["login"])
    s.set_defaults(fn=cmd_youtube_login)

    s = sub.add_parser("upload", help="upload a cut short to YouTube (private unless --privacy)")
    s.add_argument("video", help="the cut short, e.g. .shorts/compare/bodies/short-ru.mp4")
    s.add_argument("--meta", required=True,
                   help="the short's .shorts/<name>/youtube.json: <lang>: title, description, tags, "
                        "synthetic")
    s.add_argument("--lang", default="ru",
                   help="the section of youtube.json for the title and the description; the others go in as "
                        "their translations")
    s.add_argument("--captions", action="append", metavar="LANG=FILE",
                   help="a caption track to add, e.g. en=<take>/subs.en.srt (gothic-shorts srt); repeatable")
    s.add_argument("--thumbnail", metavar="FILE", help="set this JPEG cover after uploading")
    s.add_argument("--privacy", default="private", choices=["private", "unlisted", "public"],
                   help="public goes straight out, no audit needed; the status is read back after the upload")
    s.add_argument("--channel", default=youtube.CHANNEL,
                   help="upload only to this channel id (GOTHIC_YOUTUBE_CHANNEL, the project's GothicAI)")
    s.add_argument("--yes", action="store_true", help="do not ask before uploading")
    s.set_defaults(fn=cmd_upload)

    s = sub.add_parser("thumbnail", help="set a JPEG cover on a video already on the channel")
    s.add_argument("video", help="the video's id")
    s.add_argument("file", help="the JPEG cover (gothic-shorts cover), at most 2 MB")
    s.add_argument("--channel", default=youtube.CHANNEL, help="the channel (GOTHIC_YOUTUBE_CHANNEL)")
    s.add_argument("--yes", action="store_true", help="do not ask before setting")
    s.set_defaults(fn=cmd_thumbnail)

    s = sub.add_parser("translate", help="add the title and the description in other languages to a video "
                                         "already on the channel")
    s.add_argument("video", help="the video's id")
    s.add_argument("--meta", required=True, help="the short's youtube.json: every section but --lang goes in")
    s.add_argument("--lang", default="ru", help="the video's own language in youtube.json (default: ru)")
    s.add_argument("--channel", default=youtube.CHANNEL, help="the channel (GOTHIC_YOUTUBE_CHANNEL)")
    s.add_argument("--yes", action="store_true", help="do not ask before setting")
    s.set_defaults(fn=cmd_translate)

    s = sub.add_parser("captions", help="add a caption track to a video already on the channel")
    s.add_argument("video", help="the video's id")
    s.add_argument("file", help="a SubRip or WebVTT file (gothic-shorts srt)")
    s.add_argument("--lang", default="en")
    s.add_argument("--channel", default=youtube.CHANNEL, help="the channel (GOTHIC_YOUTUBE_CHANNEL)")
    s.add_argument("--yes", action="store_true", help="do not ask before adding")
    s.set_defaults(fn=cmd_captions)

    args = p.parse_args()
    try:
        sys.exit(args.fn(args))
    except (ShotError, FilmError, VoiceError, cutting.CutError, LaunchError, SettingsError,
            YouTubeError, AlignError, StudioError, DubError, GpuError, ExplainError, cover.CoverError) as err:
        print(f"error: {err}", file=sys.stderr)
        sys.exit(1)
