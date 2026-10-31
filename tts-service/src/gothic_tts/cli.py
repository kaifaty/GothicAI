"""`gothic-tts`: the text-to-speech service and a benchmark for providers."""

from __future__ import annotations

import argparse
import asyncio
import logging
import os
import sys
from pathlib import Path

from .audio import to_wav
from .loudness import TARGET_LUFS, Leveler
from .providers import PROVIDERS, TtsError, TtsProvider, Voice, make_provider
from .providers.inworld import InworldTTS
from .service import DEFAULT_ORIGINAL_VOICES, TtsService, load_original_voices
from .synthesizer import Synthesizer

log = logging.getLogger("gothic_tts")


def load_env_file(start: Path | None = None) -> Path | None:
    """Reads the nearest `.env` at or above `start`.

    API keys and local settings live in the repository root's `.env`;
    variables already set in the environment win."""
    here = start or Path.cwd()
    for d in (here, *here.parents):
        path = d / ".env"
        if path.is_file():
            for line in path.read_text().splitlines():
                key, sep, value = line.strip().removeprefix("export ").partition("=")
                if sep and key and not key.startswith("#"):
                    os.environ.setdefault(key.strip(), value.strip().strip("'\""))
            return path
    return None


def _synthesizer(args: argparse.Namespace) -> Synthesizer:
    fallback = make_provider(args.fallback) if args.fallback and args.fallback != "none" else None
    leveler = Leveler(args.loudness) if args.loudness is not None else None
    return Synthesizer(make_provider(args.provider), fallback, leveler=leveler)


async def serve(args: argparse.Namespace) -> int:
    synth = _synthesizer(args)
    if not args.no_warmup:
        await synth.warmup()
    try:
        await TtsService(
            synth,
            args.host,
            args.port,
            original_voices_dir=None if args.no_original_voices else args.original_voices_dir,
            game_dir=args.game_dir,
        ).serve_forever()
    finally:
        await synth.aclose()
    return 0


async def say(args: argparse.Namespace) -> int:
    """Speak lines and report latency; --out keeps the audio."""
    synth = _synthesizer(args)
    voice = Voice(args.voice_id, args.voice or "", args.preset, args.style or "", args.clone or "")
    try:
        await synth.warmup()
        for text in args.text:
            parts: list[bytes] = []
            rate = 0

            async def chunk(pcm: bytes, p: TtsProvider, parts: list[bytes] = parts) -> None:
                nonlocal rate
                rate = p.rate
                parts.append(pcm)

            s = await synth.speak(text, voice, chunk)
            print(
                f"[{s.provider}] {s.audio_s:.2f}s audio, first chunk {s.first_chunk_s:.2f}s, "
                f"done {s.latency_s:.2f}s (RTF {s.latency_s / max(s.audio_s, 0.01):.2f}): {text}"
            )
            if args.out:
                out = (
                    args.out
                    if len(args.text) == 1
                    else args.out.replace(".wav", f"_{args.text.index(text)}.wav")
                )
                _write(out, to_wav(b"".join(parts), rate))
    except TtsError as err:
        print(f"error: {err}", file=sys.stderr)
        return 1
    finally:
        await synth.aclose()
    return 0


async def clone_originals(args: argparse.Namespace) -> int:
    voices = load_original_voices(args.dir)
    if not voices:
        print(
            f"error: no original voices in {args.dir} (run scripts/voice_snapshots.py)",
            file=sys.stderr,
        )
        return 1
    selected: dict[str, Voice] = {}
    failed = False
    if args.ids:
        for cid in args.ids:
            if voice := voices.get(cid.casefold()):
                selected[voice.id] = voice
            else:
                print(f"error: no original voice for {cid}", file=sys.stderr)
                failed = True
    else:
        selected = {voice.id: voice for voice in voices.values()}
    provider = InworldTTS()
    try:
        for voice in selected.values():
            cid = voice.id.removesuffix("@original")
            try:
                voice_id = await provider.clone(voice)
                print(f"{cid} → {voice_id}")
            except (TtsError, OSError) as err:
                print(f"error: {cid}: {err}", file=sys.stderr)
                failed = True
    finally:
        await provider.aclose()
    return int(failed)


def _write(path: str, data: bytes) -> None:
    Path(path).write_bytes(data)


def _add_provider_args(p: argparse.ArgumentParser) -> None:
    # Inworld TTS-2 with no fallback: while Inworld is unavailable the game shows subtitles. Self-hosted
    # VoxCPM2 (`--fallback voxcpm`) is off since the voices are Inworld's alone.
    # Never `codex` by default: its conversational voice paraphrases (see providers/codex.py)
    p.add_argument(
        "--provider",
        default=os.environ.get("GOTHIC_TTS_PROVIDER", "inworld"),
        help=f"provider spec: name[:key=value,...]; names: {', '.join(PROVIDERS)}",
    )
    p.add_argument(
        "--fallback",
        default=os.environ.get("GOTHIC_TTS_FALLBACK", "none"),
        help="fallback provider spec, or none",
    )
    p.add_argument(
        "--loudness",
        type=_loudness,
        default=TARGET_LUFS,
        help=f"loudness of the lines in LUFS (default {TARGET_LUFS:g}, as the game's own dialogue), "
        "or none to keep the provider's",
    )


def _loudness(value: str) -> float | None:
    return None if value == "none" else float(value)


def main() -> None:
    env_file = load_env_file()
    parser = argparse.ArgumentParser(prog="gothic-tts")
    parser.add_argument("-v", "--verbose", action="store_true")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("serve", help="run the WebSocket service")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=47820)
    p.add_argument("--no-warmup", action="store_true")
    p.add_argument("--game-dir", type=Path, help="Gothic II folder for automatic original voice preparation")
    p.add_argument("--original-voices-dir", type=Path, default=DEFAULT_ORIGINAL_VOICES)
    p.add_argument("--no-original-voices", action="store_true")
    _add_provider_args(p)

    p = sub.add_parser("clone-originals", help="clone the original game voices into the Inworld cache")
    p.add_argument("--dir", type=Path, default=DEFAULT_ORIGINAL_VOICES)
    p.add_argument("ids", metavar="ID", nargs="*")

    p = sub.add_parser("say", help="speak lines and measure latency")
    p.add_argument("text", nargs="+")
    p.add_argument("--voice-id", default="test")
    p.add_argument("--voice", help="voice description (VoxCPM voice design)")
    p.add_argument("--preset", help="preset voice name for preset-voice APIs")
    p.add_argument("--style", help='how to say the lines, in English: "say angrily with force"')
    p.add_argument("--clone", help="a recording to clone the voice from (Inworld), in any language")
    p.add_argument("--out", help="write the audio to this wav file (name_N.wav for several lines)")
    _add_provider_args(p)

    args = parser.parse_args()
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname).1s %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )
    for noisy in ("httpx", "httpcore", "websockets"):
        logging.getLogger(noisy).setLevel(logging.WARNING)
    if env_file is not None:
        log.info("environment from %s", env_file)
    try:
        match args.command:
            case "serve":
                code = asyncio.run(serve(args))
            case "say":
                code = asyncio.run(say(args))
            case "clone-originals":
                code = asyncio.run(clone_originals(args))
            case _:
                code = 2
    except ValueError as err:
        print(f"error: {err}", file=sys.stderr)
        code = 2
    except KeyboardInterrupt:
        code = 130
    sys.exit(code)
