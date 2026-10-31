"""Push-to-talk: while the player holds the key (anywhere, protocol v7), the server records the Mac
microphone and streams it to the ASR service (asr-service/) as it is spoken. Partial text goes to the
game right away; on release only the recognizer's short tail is left, and the final text is the hero's
phrase: talk.py decides whom it is said to (the hero's focus at the key is one of the hints).

The game only reports the key (`ptt_start` / `ptt_stop` / `ptt_cancel`); no audio passes through it.
"""

from __future__ import annotations

import asyncio
import json
import logging
import time
import wave
from collections.abc import Callable, Coroutine
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Protocol

from websockets.asyncio.client import ClientConnection, connect
from websockets.exceptions import WebSocketException

from .game import GameError, GameLink
from .journal import Journal
from .protocol import (
    ChatStatus,
    ChatStatusCmd,
    GameMessage,
    Loading,
    PttCancel,
    PttStart,
    PttStop,
    TranscriptCmd,
)

log = logging.getLogger(__name__)

DEFAULT_ASR_URL = "ws://127.0.0.1:47819"
MAX_SECONDS = 30  # a stuck key (focus lost before release) does not record forever

MIC_DENIED = (
    "микрофон молчит: разрешите доступ к микрофону приложению, в котором запущен сервер "
    "(Системные настройки → Конфиденциальность и безопасность → Микрофон)"
)


class VoiceError(RuntimeError):
    pass


ChunkCallback = Callable[[bytes], None]  # 16-bit mono PCM; may be called from an audio thread


class AudioSource(Protocol):
    rate: int

    def start(self, on_chunk: ChunkCallback) -> None: ...

    def stop(self) -> None: ...


class Microphone:
    """The default input device (or `device`) via PortAudio, in 50 ms chunks."""

    def __init__(self, device: str | int | None = None, rate: int = 16000) -> None:
        self.device = device
        self.rate = rate
        self._stream: Any = None

    def start(self, on_chunk: ChunkCallback) -> None:
        try:
            import sounddevice as sd
        except (ImportError, OSError) as err:
            raise VoiceError(f"нет доступа к аудио: {err}") from err
        try:
            self._stream = sd.RawInputStream(
                samplerate=self.rate,
                channels=1,
                dtype="int16",
                blocksize=self.rate // 20,
                device=self.device,
                callback=lambda data, _frames, _time, _status: on_chunk(bytes(data)),
            )
            self._stream.start()
        except Exception as err:  # PortAudioError and friends
            self._stream = None
            raise VoiceError(f"микрофон не открылся: {err}") from err

    def stop(self) -> None:
        stream, self._stream = self._stream, None
        if stream is not None:
            stream.stop()
            stream.close()


class WavSource:
    """A recording "spoken" into the microphone at real-time pace (tests, end-to-end checks).
    Stopping early drops the rest, like releasing the key mid-sentence."""

    def __init__(self, path: str | Path, chunk_s: float = 0.1) -> None:
        with wave.open(str(path), "rb") as w:
            if w.getsampwidth() != 2 or w.getnchannels() != 1:
                raise ValueError(f"{path}: need 16-bit mono wav")
            self.rate = w.getframerate()
            self.pcm = w.readframes(w.getnframes())
        self.chunk_s = chunk_s
        self._task: asyncio.Task[None] | None = None

    @property
    def duration(self) -> float:
        return len(self.pcm) / 2 / self.rate

    def start(self, on_chunk: ChunkCallback) -> None:
        self._task = asyncio.get_running_loop().create_task(self._play(on_chunk))

    async def _play(self, on_chunk: ChunkCallback) -> None:
        step = int(self.chunk_s * self.rate) * 2
        for i in range(0, len(self.pcm), step):
            on_chunk(self.pcm[i : i + step])
            await asyncio.sleep(self.chunk_s)

    def stop(self) -> None:
        if self._task is not None:
            self._task.cancel()
            self._task = None


@dataclass(frozen=True)
class Recognized:
    text: str
    provider: str
    latency_s: float  # from the end of the recording


PartialCallback = Callable[[str], None]


class AsrClient:
    """Client of the ASR service; one connection reused for every utterance, reopened when it drops."""

    def __init__(self, url: str = DEFAULT_ASR_URL, timeout: float = 15.0) -> None:
        self.url = url
        self.timeout = timeout
        self._ws: ClientConnection | None = None
        self._lock = asyncio.Lock()  # one utterance at a time

    async def open(
        self,
        rate: int,
        lang: str = "ru",
        on_partial: PartialCallback | None = None,
        hints: list[str] | None = None,
    ) -> AsrSession:
        """Start an utterance: feed audio as it is recorded, then `finish` or `cancel` it."""
        await self._lock.acquire()
        session = AsrSession(self, rate, lang, on_partial, hints)
        await session.begin()
        return session

    async def recognize(self, pcm: bytes, rate: int, lang: str = "ru") -> Recognized:
        session = await self.open(rate, lang)
        await session.feed(pcm)
        return await session.finish()

    async def _connection(self) -> ClientConnection:
        if self._ws is None:
            self._ws = await connect(self.url, open_timeout=5, max_size=2**22)
        return self._ws

    async def _drop(self) -> None:
        ws, self._ws = self._ws, None
        if ws is not None:
            await ws.close()

    async def aclose(self) -> None:
        await self._drop()


class AsrSession:
    """One utterance. The audio is also kept here: when the connection drops, `finish` sends the
    whole recording again over a fresh one."""

    def __init__(
        self,
        client: AsrClient,
        rate: int,
        lang: str,
        on_partial: PartialCallback | None,
        hints: list[str] | None = None,
    ) -> None:
        self.client = client
        self.rate = rate
        self.lang = lang
        self.on_partial = on_partial
        self.hints = list(hints) if hints else []
        self._pcm: list[bytes] = []
        self._ws: ClientConnection | None = None
        self._reader: asyncio.Task[None] | None = None
        self._final: asyncio.Future[Recognized] = asyncio.get_running_loop().create_future()
        self._ended_at = 0.0
        self._closed = False

    async def begin(self) -> None:
        try:
            await self._start()
        except (OSError, WebSocketException) as err:
            log.warning("ASR service not reachable yet (%s), buffering", err)
            await self.client._drop()
            self._ws = None

    async def _start(self) -> None:
        self._ws = await self.client._connection()
        start = {"type": "start", "lang": self.lang, "sample_rate": self.rate, "format": "pcm16"}
        if self.hints:
            start["hints"] = self.hints
        await self._ws.send(json.dumps(start))
        self._reader = asyncio.create_task(self._read(self._ws))

    async def _read(self, ws: ClientConnection) -> None:
        try:
            async for raw in ws:
                msg = json.loads(raw)
                match msg.get("type"):
                    case "partial":
                        if self.on_partial is not None:
                            self.on_partial(str(msg.get("text", "")))
                    case "final":
                        _resolve(
                            self._final,
                            Recognized(
                                str(msg.get("text", "")).strip(),
                                str(msg.get("provider", "")),
                                time.monotonic() - self._ended_at,
                            ),
                        )
                        return
                    case "error":
                        _fail(self._final, VoiceError(f"распознавание не удалось: {msg.get('detail')}"))
                        return
        except (OSError, WebSocketException) as err:
            _fail(self._final, ConnectionError(str(err)))
            return
        _fail(self._final, ConnectionError("connection closed"))

    async def feed(self, pcm: bytes) -> None:
        self._pcm.append(pcm)
        if self._ws is None:
            return
        try:
            await self._ws.send(pcm)
        except (OSError, WebSocketException) as err:
            log.warning("ASR connection lost while streaming (%s)", err)
            self._ws = None

    async def finish(self) -> Recognized:
        self._ended_at = time.monotonic()
        try:
            for attempt in range(2):
                try:
                    if self._ws is None or attempt:
                        await self._restart()
                    assert self._ws is not None
                    await self._ws.send(json.dumps({"type": "end"}))
                    return await asyncio.wait_for(asyncio.shield(self._final), self.client.timeout)
                except (OSError, WebSocketException, ConnectionError) as err:
                    if attempt:
                        raise VoiceError(f"ASR-сервис недоступен ({self.client.url}): {err}") from err
                    log.info("ASR connection dropped (%s), sending the recording again", err)
                except TimeoutError:
                    raise VoiceError("ASR-сервис не ответил вовремя") from None
        finally:
            await self._close()
        raise AssertionError("unreachable")

    async def _restart(self) -> None:
        """A fresh connection with the whole recording sent at once."""
        if self._reader is not None:
            self._reader.cancel()
        await self.client._drop()
        self._final = asyncio.get_running_loop().create_future()
        await self._start()
        assert self._ws is not None
        pcm = b"".join(self._pcm)
        for i in range(0, len(pcm), 64000):
            await self._ws.send(pcm[i : i + 64000])

    async def cancel(self) -> None:
        if self._ws is not None:
            try:
                await self._ws.send(json.dumps({"type": "cancel"}))
            except (OSError, WebSocketException):
                await self.client._drop()
        await self._close()

    async def _close(self) -> None:
        if self._closed:
            return
        self._closed = True
        if self._reader is not None and not self._reader.done():
            self._reader.cancel()
            await asyncio.gather(self._reader, return_exceptions=True)
        if not self._final.done():
            await self.client._drop()  # the service may still answer: do not mix it into the next one
        self.client._lock.release()


def _resolve(fut: asyncio.Future[Recognized], value: Recognized) -> None:
    if not fut.done():
        fut.set_result(value)


def _fail(fut: asyncio.Future[Recognized], err: Exception) -> None:
    if not fut.done():
        fut.set_exception(err)
        fut.exception()  # retrieved: a cancelled session never reads it


@dataclass
class _Utterance:
    focus: str | None  # the npc the hero looked at when he spoke
    source: AudioSource
    hints: list[str] | None = None
    queue: asyncio.Queue[bytes | None] = field(default_factory=asyncio.Queue)
    recognize: bool = False
    finished: bool = False  # the final transcript was sent: drop late partials
    task: asyncio.Task[None] | None = None


# the hero's recognized phrase: (text, focus, details for the journal)
Heard = Callable[[str, str | None, dict[str, Any]], None]


class VoiceInput:
    """Turns push-to-talk key events from the game into the hero's phrases (`heard`).

    `hints`: ASR vocabulary for the hero's focus, snapshotted once when the key is pressed.
    `files`: a `ptt_start` may name a recording (`audio`) the hero says instead of the microphone; the test
    bench speaks for the hero this way (scripts/shorts/). Off by default: the game is not to make the server
    read files."""

    def __init__(
        self,
        link: GameLink,
        heard: Heard,
        source: AudioSource,
        asr: AsrClient,
        *,
        lang: str = "ru",
        journal: Journal | None = None,
        files: bool = False,
        hints: Callable[[str | None], list[str]] | None = None,
    ) -> None:
        self.link = link
        self.heard = heard
        self.journal = journal or Journal()
        self.source = source
        self.asr = asr
        self.lang = lang
        self.files = files
        self.hints = hints
        self.active: _Utterance | None = None  # while the key is held
        self._watchdog: asyncio.TimerHandle | None = None
        self._tasks: set[asyncio.Task[None]] = set()
        link.listeners.append(self._on_message)

    def _on_message(self, msg: GameMessage) -> None:
        match msg:
            case PttStart():
                self.start(msg.focus, msg.audio)
            case PttStop():
                self.stop(recognize=True, focus=msg.focus)
            case PttCancel() | Loading():
                self.stop(recognize=False)

    def start(self, focus: str | None = None, audio: str | None = None) -> None:
        """`audio`: a recording to say instead of the microphone (only with `files`)."""
        if self.active is not None:
            self.stop(recognize=False)
        loop = asyncio.get_running_loop()
        try:
            source = self._source(audio)
            utt = _Utterance(focus.upper() if focus else None, source)
            if self.hints is not None:
                utt.hints = list(self.hints(utt.focus))
            source.start(lambda data: loop.call_soon_threadsafe(utt.queue.put_nowait, data))
        except VoiceError as err:
            log.error("voice: %s", err)
            self.journal.add("voice_error", detail=str(err))
            self._spawn(self._status("error", str(err)))
            return
        utt.task = asyncio.create_task(self._run(utt))
        self.active = utt
        self._watchdog = loop.call_later(MAX_SECONDS, self.stop, True)

    def _source(self, audio: str | None) -> AudioSource:
        if audio is None:
            return self.source
        if not self.files:
            raise VoiceError("сервер запущен без --voice-files: запись вместо микрофона не принимается")
        try:
            return WavSource(audio)
        except (OSError, EOFError, ValueError, wave.Error) as err:
            raise VoiceError(f"запись {audio} не читается: {err}") from err

    def stop(self, recognize: bool, focus: str | None = None) -> None:
        """The key is released (`recognize`) or it was a tap; `focus` at release wins over the one at
        the press (the hero may turn to whom he speaks)."""
        utt = self.active
        if utt is None:
            return
        self.active = None
        if self._watchdog is not None:
            self._watchdog.cancel()
            self._watchdog = None
        utt.source.stop()
        if focus:
            utt.focus = focus.upper()
        utt.recognize = recognize
        utt.queue.put_nowait(None)

    async def _run(self, utt: _Utterance) -> None:
        """Streams the recording into the ASR service while it lasts, then gets the final text."""
        rate = utt.source.rate

        def partial(text: str) -> None:
            if text and not utt.finished:
                self._spawn(self._partial(utt, text))

        session = await self.asr.open(rate, self.lang, partial, hints=utt.hints)
        size, heard = 0, False
        while (chunk := await utt.queue.get()) is not None:
            size += len(chunk)
            heard = heard or chunk.count(0) != len(chunk)
            await session.feed(chunk)
        if not utt.recognize:
            await session.cancel()
            return
        seconds = size / 2 / rate
        if size and not heard:
            await session.cancel()
            log.error("voice: %.1fs of pure silence, microphone access is probably denied", seconds)
            self.journal.add("voice_error", detail=MIC_DENIED)
            await self._status("error", MIC_DENIED)
            return
        await self._status("recognizing")
        try:
            result = await session.finish()
        except VoiceError as err:
            log.error("voice: %s", err)
            self.journal.add("voice_error", detail=str(err))
            await self._status("error", str(err))
            return
        finally:
            utt.finished = True
        log.info(
            "voice: %r (%.1fs of audio, %s, %.2fs after release, focus %s)",
            result.text,
            seconds,
            result.provider,
            result.latency_s,
            utt.focus or "-",
        )
        if not result.text:
            await self._status("notice", "Не расслышал. Удерживайте T, пока говорите")
            return
        self.heard(
            result.text,
            utt.focus,
            {"via": "voice", "asr": result.provider, "latency_s": round(result.latency_s, 2)},
        )

    async def _partial(self, utt: _Utterance, text: str) -> None:
        if not utt.finished:
            await self.link.send(TranscriptCmd(text=text, final=False))

    async def _status(self, status: ChatStatus, detail: str | None = None) -> None:
        await self.link.send(ChatStatusCmd(status=status, detail=detail))

    def _spawn(self, coro: Coroutine[Any, Any, None]) -> None:
        async def run() -> None:
            try:
                await coro
            except GameError:
                pass

        task = asyncio.create_task(run())
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)
