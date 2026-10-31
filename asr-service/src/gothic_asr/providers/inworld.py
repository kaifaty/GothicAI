"""Inworld STT: PCM16 over a persistent WebSocket, with push-to-talk turn boundaries.

The connection is configured once for a language and name prompts. `endTurn` flushes a phrase;
`closeStream` retires the connection. Opening runs in the background so audio can arrive immediately.
With server VAD disabled, each turn must fit Inworld's roughly 30-second limit.
With `vadThreshold: 0`, finals arrive only after `endTurn`, except at that turn limit.
"""

from __future__ import annotations

import asyncio
import json
import logging
import os

import numpy as np
from websockets.asyncio.client import ClientConnection, connect
from websockets.exceptions import WebSocketException
from websockets.protocol import State

from ..audio import RATE, float_to_pcm16
from .base import AsrError, PartialCallback, Transcript

log = logging.getLogger(__name__)

DEFAULT_URL = "wss://api.inworld.ai/stt/v1/transcribe:streamBidirectional"
DEFAULT_MODEL = "inworld/inworld-stt-1"
LANGUAGES = frozenset(
    "ar yue zh cs da nl en fil fi fr de el hi hu id it ja ko mk ms fa pl pt ro ru es sv th tr vi".split()
)
PROMPT_PUNCTUATION = frozenset(" .,'():!?;-")


def _prompts(hints: list[str] | None) -> tuple[str, ...]:
    prompts: list[str] = []
    for hint in hints or []:
        # Check before stripping: tabs and newlines are invalid even at the edge of a prompt.
        if not all(c.isalpha() or c.isdecimal() or c in PROMPT_PUNCTUATION for c in hint):
            continue
        text = hint.strip()
        if text and text not in prompts:
            prompts.append(text)
            if len(prompts) == 100:
                break
    return tuple(prompts)


class InworldRealtime:
    name = "inworld"
    streaming = True
    languages = LANGUAGES

    def __init__(
        self,
        model: str = DEFAULT_MODEL,
        url: str = DEFAULT_URL,
        api_key_env: str = "INWORLD_API_KEY",
        timeout: str | float = 10.0,
    ) -> None:
        self.model = model
        self.url = url
        self.api_key_env = api_key_env
        self.timeout = float(timeout)
        self._lock = asyncio.Lock()
        self._ws: ClientConnection | None = None
        self._reader: asyncio.Task[None] | None = None
        self._settings: tuple[str, tuple[str, ...]] | None = None
        self._active: _Stream | None = None

    def _key(self) -> str:
        key = os.environ.get(self.api_key_env, "")
        if not key:
            raise AsrError(f"inworld: no API key; set {self.api_key_env}")
        return key

    async def warmup(self) -> None:
        self._key()
        async with self._lock:
            await self._session("ru", ())

    async def open_stream(
        self,
        lang: str,
        on_partial: PartialCallback | None = None,
        *,
        hints: list[str] | None = None,
    ) -> _Stream:
        self._key()
        await self._lock.acquire()
        stream = _Stream(self, lang, _prompts(hints), on_partial)
        self._active = stream
        return stream

    async def transcribe(self, audio: np.ndarray, lang: str) -> Transcript:
        stream = await self.open_stream(lang)
        try:
            for i in range(0, len(audio), RATE):
                await stream.feed(audio[i : i + RATE])
            return await stream.finish()
        except BaseException:
            await stream.cancel()
            raise

    async def _session(self, lang: str, prompts: tuple[str, ...]) -> ClientConnection:
        settings = (lang, prompts)
        if (
            self._ws is not None
            and self._ws.state is State.OPEN
            and self._reader is not None
            and not self._reader.done()
            and self._settings == settings
        ):
            return self._ws
        await self._drop()
        try:
            self._ws = await connect(
                self.url,
                additional_headers={"Authorization": f"Basic {self._key()}"},
                open_timeout=self.timeout,
                close_timeout=min(self.timeout, 1.0),
                max_size=2**22,
            )
            await asyncio.wait_for(
                self._ws.send(
                    json.dumps(
                        {
                            "transcribeConfig": {
                                "modelId": self.model,
                                "audioEncoding": "LINEAR16",
                                "sampleRateHertz": RATE,
                                "numberOfChannels": 1,
                                "language": lang,
                                "inworldSttV1Config": {"vadThreshold": 0},
                                "prompts": list(prompts),
                            },
                            "audioProtocol": "BINARY",
                        },
                        ensure_ascii=False,
                    )
                ),
                self.timeout,
            )
        except (OSError, TimeoutError, WebSocketException) as err:
            await self._drop(graceful=False)
            raise AsrError(f"inworld connection failed: {type(err).__name__}: {err}") from err
        self._settings = settings
        self._reader = asyncio.create_task(self._read(self._ws))
        log.info("inworld transcription session ready (%s, %s, %d prompts)", self.model, lang, len(prompts))
        return self._ws

    async def _read(self, ws: ClientConnection) -> None:
        try:
            async for raw in ws:
                event = json.loads(raw)
                if not isinstance(event, dict):
                    raise TypeError("expected a JSON object")
                if "error" in event:
                    error = event["error"]
                    raise AsrError(f"inworld: {error.get('code')}: {error.get('message', error)}")
                transcription = event.get("result", {}).get("transcription")
                stream = self._active
                if transcription is not None and stream is not None and stream._ws is ws:
                    stream._transcription(transcription)
        except (ValueError, KeyError, TypeError, AttributeError) as err:
            failure = AsrError(f"inworld: invalid response ({type(err).__name__}): {err}")
        except (OSError, WebSocketException, AsrError) as err:
            failure = err if isinstance(err, AsrError) else AsrError(f"inworld connection lost: {err}")
        else:
            failure = AsrError("inworld connection closed before the final transcript")
        stream = self._active
        if stream is not None and stream._ws is ws:
            stream._fail(failure)

    async def _drop(self, *, graceful: bool = True) -> None:
        # Keep ownership until closure completes: cancelling a reconnect must still let cancel()
        # find and close the old socket after it has stopped the sender.
        ws, reader = self._ws, self._reader
        self._settings = None
        if reader is not None:
            reader.cancel()
            await asyncio.gather(reader, return_exceptions=True)
            self._reader = None
        if ws is not None:
            if graceful and ws.state is State.OPEN:
                try:
                    await asyncio.wait_for(ws.send(json.dumps({"closeStream": {}})), self.timeout)
                except (OSError, TimeoutError, WebSocketException):
                    pass
            await ws.close()
            self._ws = None

    async def aclose(self) -> None:
        if self._active is not None:
            await self._active.cancel()
        async with self._lock:
            await self._drop()


class _Stream:
    def __init__(
        self,
        owner: InworldRealtime,
        lang: str,
        prompts: tuple[str, ...],
        on_partial: PartialCallback | None,
    ) -> None:
        self._owner = owner
        self._on_partial = on_partial
        self._ws: ClientConnection | None = None
        self._audio: asyncio.Queue[bytes | None] = asyncio.Queue()
        self._finals: list[str] = []
        self._ending = False
        self._closed = False
        self._done: asyncio.Future[str] = asyncio.get_running_loop().create_future()
        self._sender = asyncio.create_task(self._send(lang, prompts))

    async def _send(self, lang: str, prompts: tuple[str, ...]) -> None:
        try:
            self._ws = await self._owner._session(lang, prompts)
            while True:
                pcm = await self._audio.get()
                if pcm is None:
                    async with asyncio.timeout(self._owner.timeout):
                        # No task switch between marking the boundary and writing its frame.
                        self._ending = True
                        await self._ws.send(json.dumps({"endTurn": {}}))
                    return
                await asyncio.wait_for(self._ws.send(pcm), self._owner.timeout)
        except (AsrError, OSError, TimeoutError, WebSocketException) as err:
            self._fail(err if isinstance(err, AsrError) else AsrError(f"inworld stream failed: {err}"))

    def _transcription(self, transcription: dict) -> None:
        if self._done.done():
            return
        text = transcription.get("transcript", "").strip()
        final = transcription.get("isFinal", False)
        if final:
            if text:
                self._finals.append(text)
            partial = " ".join(self._finals)
        else:
            partial = " ".join([*self._finals, text]).strip()
        if self._on_partial is not None:
            self._on_partial(partial)
        if final and self._ending:
            self._done.set_result(" ".join(self._finals))

    def _fail(self, err: AsrError) -> None:
        if not self._done.done():
            self._done.set_exception(err)
            self._done.exception()  # a cancelled phrase may never await the result

    async def feed(self, audio: np.ndarray) -> None:
        if self._closed:
            raise AsrError("inworld: the phrase is closed")
        if self._done.done():
            self._done.result()
        if len(audio):
            self._audio.put_nowait(float_to_pcm16(audio))

    async def finish(self) -> Transcript:
        self._audio.put_nowait(None)
        try:
            text = await asyncio.wait_for(asyncio.shield(self._done), self._owner.timeout)
        except TimeoutError as err:
            await self._close(reuse=False)
            raise AsrError(f"inworld: no final transcript within {self._owner.timeout:g}s") from err
        except BaseException:
            await self._close(reuse=False)
            raise
        await self._close(reuse=True)
        return Transcript(text, self._owner.name)

    async def cancel(self) -> None:
        await self._close(reuse=False)

    async def _close(self, *, reuse: bool) -> None:
        if self._closed:
            return
        self._closed = True
        self._sender.cancel()
        await asyncio.gather(self._sender, return_exceptions=True)
        if not reuse:
            await self._owner._drop(graceful=False)
        self._owner._active = None
        self._owner._lock.release()
