"""Client of the TTS service (tts-service/): an npc line in, its audio out, streamed as it is made."""

from __future__ import annotations

import asyncio
import base64
import io
import itertools
import json
import logging
import time
import wave
from collections.abc import Awaitable, Callable
from dataclasses import dataclass

from websockets.asyncio.client import ClientConnection, connect
from websockets.exceptions import WebSocketException

log = logging.getLogger(__name__)

DEFAULT_TTS_URL = "ws://127.0.0.1:47820"

StartCallback = Callable[[int, str], Awaitable[None]]  # sample rate, provider
ChunkCallback = Callable[[bytes], Awaitable[None]]  # 16-bit mono PCM
WordTiming = tuple[str, float, float]  # word, start and end from the line's first audio
WordCallback = Callable[[list[WordTiming]], Awaitable[None]]


class WordMatcher:
    """Match successive TTS words to whitespace-separated subtitle words, preserving their indices."""

    def __init__(self, text: str) -> None:
        self.words = [self._normalize(word) for word in text.split()]
        self.position = 0

    @staticmethod
    def _normalize(word: str) -> str:
        return "".join(c for c in word.lower() if c.isalnum())

    def match(self, words: list[WordTiming]) -> list[tuple[int, float, float]]:
        matched = []
        for word, start, end in words:
            normalized = self._normalize(word)
            if not normalized:
                continue
            for index in range(self.position, len(self.words)):
                if self.words[index] == normalized:
                    matched.append((index, round(start, 3), round(end, 3)))
                    self.position = index + 1
                    break
        return matched


class TtsError(RuntimeError):
    pass


@dataclass(frozen=True)
class Speech:
    wav: bytes  # empty when streamed
    audio_s: float
    provider: str
    latency_s: float
    first_chunk_s: float = 0.0
    words: list[WordTiming] | None = None


class TtsClient:
    """One connection, one line at a time, reopened when it drops."""

    def __init__(self, url: str = DEFAULT_TTS_URL, timeout: float = 10.0) -> None:
        self.url = url
        self.timeout = timeout  # per message: the first chunk, and every next one
        self._ws: ClientConnection | None = None
        self._lock = asyncio.Lock()
        self._ids = itertools.count(1)

    async def speak(self, text: str, voice_id: str, description: str, style: str = "") -> Speech:
        """The whole line as a WAV file."""
        pcm = bytearray()
        rate = 0

        async def start(r: int, _provider: str) -> None:
            nonlocal rate
            rate = r

        async def chunk(data: bytes) -> None:
            pcm.extend(data)

        s = await self.stream(text, voice_id, description, start, chunk, style)
        return Speech(_wav(bytes(pcm), rate), s.audio_s, s.provider, s.latency_s, s.first_chunk_s, s.words)

    async def stream(
        self,
        text: str,
        voice_id: str,
        description: str,
        on_start: StartCallback,
        on_chunk: ChunkCallback,
        style: str = "",
        *,
        on_words: WordCallback | None = None,
    ) -> Speech:
        """Speak `text`, passing audio on as it is generated (`on_start` comes before the first chunk).
        `style`: how to say it, in English ("say angrily"); empty for the voice's usual manner."""
        async with self._lock:
            started = time.monotonic()
            try:
                if self._ws is None:
                    self._ws = await connect(self.url, open_timeout=3, max_size=2**22)
                ws = self._ws
                req = str(next(self._ids))
                request = {
                    "type": "speak",
                    "id": req,
                    "text": text,
                    "voice": {"id": voice_id, "description": description},
                }
                if style:
                    request["style"] = style
                await ws.send(json.dumps(request, ensure_ascii=False))
                rate, provider, size, first = 0, "", 0, 0.0
                words: list[WordTiming] | None = None
                while True:
                    msg = await asyncio.wait_for(ws.recv(), self.timeout)
                    if isinstance(msg, bytes):
                        first = first or time.monotonic() - started
                        size += len(msg)
                        await on_chunk(msg)
                        continue
                    ev = json.loads(msg)
                    if ev.get("id") != req:
                        continue
                    match ev.get("type"):
                        case "start":
                            rate, provider = int(ev["sample_rate"]), str(ev.get("provider", ""))
                            await on_start(rate, provider)
                        case "done":
                            return Speech(
                                b"", size / 2 / rate, provider, time.monotonic() - started, first, words
                            )
                        case "audio":
                            pcm = base64.b64decode(ev["data"])
                            first = first or time.monotonic() - started
                            size += len(pcm)
                            timings = [(str(w), float(start), float(end)) for w, start, end in ev["words"]]
                            if words is None:
                                words = []
                            words.extend(timings)
                            if on_words is not None:
                                await on_words(timings)
                            await on_chunk(pcm)
                        case "error":
                            raise TtsError(str(ev.get("detail")))
            except (OSError, WebSocketException, TimeoutError) as err:
                await self._drop()
                raise TtsError(f"TTS service {self.url}: {type(err).__name__}: {err}") from err
            except BaseException:
                await self._drop()  # cancelled mid-line: its remaining audio must not reach the next one
                raise

    async def _drop(self) -> None:
        ws, self._ws = self._ws, None
        if ws is not None:
            await ws.close()

    async def aclose(self) -> None:
        await self._drop()


def _wav(pcm: bytes, rate: int) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)
    return buf.getvalue()
