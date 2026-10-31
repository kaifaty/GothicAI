"""Voxtral Mini 4B Realtime, self-hosted behind audio.cpp's live transcription endpoint (on the GPU
server, never on this machine).

audio.cpp protocol: one HTTP/1.1 request per utterance,
`POST /v1/audio/transcriptions/live?model=…&sample_rate=16000&channels=1&sample_format=s16le`. Its body
is raw PCM16 sent in chunks (`Transfer-Encoding: chunked`) while the player speaks. The same
connection answers with SSE as the model transcribes with a fixed delay (480 ms):
`transcript.text.delta{delta}`, after the terminating chunk one `transcript.text.done{text}`, then
`[DONE]`; failures come as `error{error{message}}`. audio.cpp does not pad the end of a stream, so
`finish` sends `tail` seconds of silence first, or the last word stays inside the model's delay.

The model takes no language and at times writes Russian speech as Polish or Portuguese. `language=`
is our patch to audio.cpp (deploy/voxtral/audiocpp-language.patch): the decoder never picks a token
with letters outside that language. An unpatched server ignores it.
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
from urllib.parse import quote, urlsplit

import numpy as np

from ..audio import RATE, float_to_pcm16
from .base import AsrError, PartialCallback, Transcript

log = logging.getLogger(__name__)

DEFAULT_URL = "http://127.0.0.1:47830"  # GOTHIC_VOXTRAL_URL: the GPU server running audio.cpp
DEFAULT_MODEL = "voxtral-realtime"  # model id in the audio.cpp server config
LANGUAGES = frozenset("en fr es de ru zh ja it pt nl ar hi ko".split())
BATCH_CHUNK = 16000  # samples per chunk when a whole recording is sent at once
TAIL_S = 0.96  # silence that flushes the delayed end of speech; 0.64 s still cuts last words at times


class VoxtralRealtime:
    name = "voxtral"
    streaming = True
    languages = LANGUAGES

    def __init__(
        self,
        url: str = "",
        model: str = DEFAULT_MODEL,
        timeout: str | float = 10.0,
        tail: str | float = TAIL_S,
    ) -> None:
        self.url = url or os.environ.get("GOTHIC_VOXTRAL_URL", DEFAULT_URL)
        parts = urlsplit(self.url)
        if parts.scheme != "http" or not parts.hostname:
            raise ValueError(f"voxtral url must be http://host:port, got {self.url!r}")
        self._host = parts.hostname
        self._port = parts.port or 80
        self._prefix = parts.path.rstrip("/")
        self.model = model
        self.timeout = float(timeout)
        self.tail = float(tail)
        self._lock = asyncio.Lock()  # the server runs one utterance per model at a time

    async def warmup(self) -> None:
        reader, writer = await self._connect()
        try:
            writer.write(self._head("GET", "/health"))
            await writer.drain()
            status, headers = await asyncio.wait_for(_read_head(reader), self.timeout)
            body = await asyncio.wait_for(_read_body(reader, headers), self.timeout)
        except (OSError, TimeoutError, asyncio.IncompleteReadError, ValueError) as err:
            raise AsrError(f"voxtral {self.url}: {type(err).__name__}: {err}") from err
        finally:
            writer.close()
        if status != 200:
            raise AsrError(f"voxtral {self.url}: health {status} {body[:200]!r}")
        log.info("voxtral server ready (%s)", self.url)

    async def open_stream(
        self,
        lang: str,
        on_partial: PartialCallback | None = None,
        *,
        hints: list[str] | None = None,
    ) -> _Stream:
        await self._lock.acquire()
        try:
            reader, writer = await self._connect()
            query = (
                f"model={quote(self.model)}&sample_rate={RATE}&channels=1&sample_format=s16le"
                f"&language={quote(lang)}"
            )
            writer.write(
                self._head(
                    "POST",
                    f"/v1/audio/transcriptions/live?{query}",
                    "Content-Type: application/octet-stream\r\nTransfer-Encoding: chunked\r\n"
                    "Accept: text/event-stream\r\n",
                )
            )
            await writer.drain()
        except AsrError:
            self._lock.release()
            raise
        except OSError as err:
            self._lock.release()
            raise AsrError(f"voxtral {self.url}: {type(err).__name__}: {err}") from err
        except BaseException:
            self._lock.release()
            raise
        return _Stream(self, reader, writer, on_partial)

    async def transcribe(self, audio: np.ndarray, lang: str) -> Transcript:
        stream = await self.open_stream(lang)
        try:
            for i in range(0, len(audio), BATCH_CHUNK):
                await stream.feed(audio[i : i + BATCH_CHUNK])
        except BaseException:
            await stream.cancel()
            raise
        return await stream.finish()

    async def aclose(self) -> None:
        pass  # no connection outlives an utterance

    async def _connect(self) -> tuple[asyncio.StreamReader, asyncio.StreamWriter]:
        try:
            return await asyncio.wait_for(asyncio.open_connection(self._host, self._port), self.timeout)
        except (OSError, TimeoutError) as err:
            raise AsrError(f"voxtral {self.url}: {type(err).__name__}: {err}") from err

    def _head(self, method: str, path: str, extra: str = "") -> bytes:
        return (
            f"{method} {self._prefix}{path} HTTP/1.1\r\nHost: {self._host}:{self._port}\r\n"
            f"{extra}Connection: close\r\n\r\n"
        ).encode()


class _Stream:
    def __init__(
        self,
        owner: VoxtralRealtime,
        reader: asyncio.StreamReader,
        writer: asyncio.StreamWriter,
        on_partial: PartialCallback | None,
    ) -> None:
        self._owner = owner
        self._writer = writer
        self._on_partial = on_partial
        self._text = ""
        self._done: asyncio.Future[str] = asyncio.get_running_loop().create_future()
        self._reader = asyncio.create_task(self._read(reader))
        self._closed = False

    async def _read(self, reader: asyncio.StreamReader) -> None:
        try:
            status, headers = await _read_head(reader)
            if status != 200:
                body = await asyncio.wait_for(_read_body(reader, headers), self._owner.timeout)
                _fail(self._done, AsrError(f"voxtral: HTTP {status}: {_error_message(body)}"))
                return
            buf = b""
            async for data in _body_parts(reader, headers):
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if self._event(line.strip()):
                        return
        except (OSError, asyncio.IncompleteReadError, ValueError) as err:
            _fail(self._done, AsrError(f"voxtral connection lost: {type(err).__name__}: {err}"))
            return
        _fail(self._done, AsrError("voxtral: the stream ended without a transcript"))

    def _event(self, line: bytes) -> bool:
        """Handle one SSE line; True when the stream is over."""
        if not line.startswith(b"data:"):
            return False
        payload = line[5:].strip()
        if payload == b"[DONE]":
            _fail(self._done, AsrError("voxtral: the stream ended without a transcript"))
            return True
        ev = json.loads(payload)
        match ev.get("type"):
            case "transcript.text.delta":
                self._text += ev.get("delta", "")
                if self._on_partial is not None and self._text.strip():
                    self._on_partial(self._text.strip())
            case "transcript.text.done":
                _resolve(self._done, str(ev.get("text", self._text)))
                return True
            case "error":
                _fail(self._done, AsrError(f"voxtral: {_error_message(payload)}"))
                return True
        return False

    async def feed(self, audio: np.ndarray) -> None:
        if self._done.done():
            self._done.result()  # raises the reader's error
        await self._send(float_to_pcm16(audio))

    async def finish(self) -> Transcript:
        try:
            if self._owner.tail > 0:
                await self._send(bytes(2 * round(self._owner.tail * RATE)))
            self._writer.write(b"0\r\n\r\n")
            await self._writer.drain()
            text = await asyncio.wait_for(asyncio.shield(self._done), self._owner.timeout)
        except (OSError, TimeoutError) as err:
            await self._close()
            raise AsrError(f"voxtral: no transcription ({type(err).__name__})") from err
        except AsrError:
            await self._close()
            raise
        await self._close()
        return Transcript(text.strip(), self._owner.name)

    async def cancel(self) -> None:
        # dropping the connection without the terminating chunk aborts the request on the server
        await self._close()

    async def _send(self, pcm: bytes) -> None:
        if not pcm:
            return
        try:
            self._writer.write(f"{len(pcm):x}\r\n".encode() + pcm + b"\r\n")
            await self._writer.drain()
        except OSError as err:
            raise AsrError(f"voxtral connection lost: {err}") from err

    async def _close(self) -> None:
        if self._closed:
            return
        self._closed = True
        self._reader.cancel()
        self._writer.close()
        await asyncio.gather(self._reader, return_exceptions=True)
        self._owner._lock.release()


async def _read_head(reader: asyncio.StreamReader) -> tuple[int, dict[str, str]]:
    status_line = (await reader.readuntil(b"\r\n")).decode("latin-1").split()
    if len(status_line) < 2 or not status_line[0].startswith("HTTP/"):
        raise ValueError(f"not an HTTP response: {' '.join(status_line)!r}")
    headers: dict[str, str] = {}
    while (line := (await reader.readuntil(b"\r\n")).decode("latin-1").strip()) != "":
        key, _, value = line.partition(":")
        headers[key.strip().lower()] = value.strip()
    return int(status_line[1]), headers


async def _body_parts(reader: asyncio.StreamReader, headers: dict[str, str]):
    """The response body as it arrives: chunked, sized or until the server closes."""
    if "chunked" in headers.get("transfer-encoding", "").lower():
        while True:
            size = int((await reader.readuntil(b"\r\n")).split(b";")[0], 16)
            if size == 0:
                return
            data = await reader.readexactly(size)
            await reader.readexactly(2)
            yield data
    elif "content-length" in headers:
        yield await reader.readexactly(int(headers["content-length"]))
    else:
        while data := await reader.read(65536):
            yield data


async def _read_body(reader: asyncio.StreamReader, headers: dict[str, str]) -> bytes:
    return b"".join([part async for part in _body_parts(reader, headers)])


def _error_message(body: bytes) -> str:
    try:
        err = json.loads(body).get("error", body.decode(errors="replace"))
    except (ValueError, AttributeError):
        return body[:200].decode(errors="replace")
    return str(err.get("message", err) if isinstance(err, dict) else err)


def _resolve(fut: asyncio.Future[str], value: str) -> None:
    if not fut.done():
        fut.set_result(value)


def _fail(fut: asyncio.Future[str], err: Exception) -> None:
    if not fut.done():
        fut.set_exception(err)
        fut.exception()  # mark retrieved: the stream may be cancelled without reading it
