"""`gothic-asr`: the speech recognition service and a benchmark for providers."""

from __future__ import annotations

import argparse
import asyncio
import logging
import os
import sys
import time
from pathlib import Path

from .audio import RATE, read_wav, resample
from .providers import PROVIDERS, AsrError, make_provider
from .recognizer import Recognizer
from .service import AsrService

log = logging.getLogger("gothic_asr")

# Inworld STT (streams, ~0.2 s after speech), ChatGPT plan when it is unreachable
DEFAULT_PROVIDER = "inworld"
DEFAULT_FALLBACK = "chatgpt"
CHUNK_S = 0.1


def load_env_file(start: Path | None = None) -> Path | None:
    """Reads the nearest `.env` at or above `start` (local settings live in the repository root's `.env`).
    Variables already set in the environment win."""
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


def _recognizer(args: argparse.Namespace) -> Recognizer:
    return Recognizer(
        make_provider(args.provider),
        make_provider(args.fallback) if args.fallback and args.fallback != "none" else None,
        timeout=args.timeout,
    )


async def serve(args: argparse.Namespace) -> int:
    recognizer = _recognizer(args)
    if not args.no_warmup:
        await recognizer.warmup()
    try:
        await AsrService(recognizer, args.host, args.port).serve_forever()
    finally:
        await recognizer.aclose()
    return 0


async def transcribe(args: argparse.Namespace) -> int:
    """Transcribe wav files and print latency: compare providers on the same audio."""
    recognizer = _recognizer(args)
    try:
        started = time.monotonic()
        await recognizer.warmup()
        print(f"warmup {time.monotonic() - started:.1f}s", file=sys.stderr)
        latencies: list[float] = []
        for path in args.files:
            audio, rate = read_wav(path)
            audio = resample(audio, rate)
            expected = Path(path).with_suffix(".txt")
            for _ in range(args.repeat):
                if args.realtime:
                    # as if spoken: 100 ms chunks at real-time pace; latency counts from the end of speech
                    utt = recognizer.utterance(args.lang, hints=args.hints)
                    await utt.begin()
                    step = int(CHUNK_S * RATE)
                    for i in range(0, len(audio), step):
                        await utt.feed(audio[i : i + step])
                        await asyncio.sleep(CHUNK_S)
                    r = await utt.finish()
                else:
                    r = await recognizer.recognize(audio, args.lang)
                latencies.append(r.latency_s)
            print(f"{Path(path).name}: [{r.provider} {r.audio_s:.1f}s audio, {r.latency_s:.2f}s] {r.text}")
            if expected.exists():
                print(f"{' ' * len(Path(path).name)}  expected: {expected.read_text().strip()}")
        if latencies:
            latencies.sort()
            print(
                f"latency p50 {latencies[len(latencies) // 2]:.2f}s, max {latencies[-1]:.2f}s",
                file=sys.stderr,
            )
    except AsrError as err:
        print(f"error: {err}", file=sys.stderr)
        return 1
    finally:
        await recognizer.aclose()
    return 0


def _add_provider_args(p: argparse.ArgumentParser) -> None:
    p.add_argument(
        "--provider",
        default=os.environ.get("GOTHIC_ASR_PROVIDER", DEFAULT_PROVIDER),
        help=f"provider spec: name[:key=value,...]; names: {', '.join(PROVIDERS)}",
    )
    p.add_argument(
        "--fallback",
        default=os.environ.get("GOTHIC_ASR_FALLBACK", DEFAULT_FALLBACK),
        help="fallback provider spec, or none",
    )
    p.add_argument("--timeout", type=float, default=4.0, help="seconds per provider before falling back")


def main() -> None:
    env_file = load_env_file()
    parser = argparse.ArgumentParser(prog="gothic-asr")
    parser.add_argument("-v", "--verbose", action="store_true")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("serve", help="run the WebSocket service")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=47819)
    p.add_argument("--no-warmup", action="store_true", help="load models on the first request")
    _add_provider_args(p)

    p = sub.add_parser("transcribe", help="transcribe 16-bit wav files and measure latency")
    p.add_argument("files", nargs="+")
    p.add_argument("--lang", default="ru")
    p.add_argument("--repeat", type=int, default=1, help="runs per file (latency statistics)")
    p.add_argument(
        "--hints",
        type=lambda value: [hint.strip() for hint in value.split(",") if hint.strip()],
        default=None,
        help="comma-separated names or phrases for streaming providers",
    )
    p.add_argument(
        "--realtime", action="store_true", help="feed audio at speaking pace (streaming providers)"
    )
    _add_provider_args(p)

    args = parser.parse_args()
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname).1s %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )
    for noisy in ("httpx", "httpx2", "httpcore", "huggingface_hub", "websockets"):
        logging.getLogger(noisy).setLevel(logging.WARNING)
    if env_file is not None:
        log.info("environment from %s", env_file)
    try:
        match args.command:
            case "serve":
                code = asyncio.run(serve(args))
            case "transcribe":
                code = asyncio.run(transcribe(args))
            case _:
                code = 2
    except ValueError as err:
        print(f"error: {err}", file=sys.stderr)
        code = 2
    except KeyboardInterrupt:
        code = 130
    sys.exit(code)
