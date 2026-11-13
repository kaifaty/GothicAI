"""Audio capture and the independent, silent player-face PCM stream (protocol v64)."""

from __future__ import annotations

import asyncio
import base64
import logging
import wave
from collections.abc import Callable
from pathlib import Path
from typing import Any, Protocol

from .game import GameError, GameLink
from .protocol import PttAudioCmd

log = logging.getLogger(__name__)


class VoiceError(RuntimeError):
    pass


ChunkCallback = Callable[[bytes], None]  # 16-bit mono PCM; may be called from an audio thread


class AudioSource(Protocol):
    rate: int

    def start(self, on_chunk: ChunkCallback) -> None: ...

    def stop(self) -> None: ...


class Microphone:
    """The default input device (or `device`) via PortAudio, in 20 ms chunks."""

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
                blocksize=self.rate // 50,
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

    def __init__(self, path: str | Path, chunk_s: float = 0.02) -> None:
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
        loop = asyncio.get_running_loop()
        started = loop.time()
        for i in range(0, len(self.pcm), step):
            on_chunk(self.pcm[i : i + step])
            end = min(i + step, len(self.pcm)) / 2 / self.rate
            await asyncio.sleep(max(0.0, started + end - loop.time()))

    def stop(self) -> None:
        if self._task is not None:
            self._task.cancel()
            self._task = None


class PlayerAudio:
    """One PTT utterance; game transport never waits for ASR, or ASR for transport."""

    def __init__(self, link: GameLink, rate: int) -> None:
        self.link = link
        self.rate = rate
        self._open = True
        self._sequence = 0
        self._loop = asyncio.get_running_loop()
        self._queue: asyncio.Queue[tuple[bytes, int]] = asyncio.Queue()
        self._task = asyncio.create_task(self._send())

    def feed(self, pcm: bytes, received_at_us: int) -> None:
        # Source callbacks may run on PortAudio's thread. The gate is checked on the event loop,
        # including callbacks that were already posted when stop() closed this utterance.
        self._loop.call_soon_threadsafe(self._enqueue, pcm, received_at_us)

    def _enqueue(self, pcm: bytes, received_at_us: int) -> None:
        if self._open:
            self._queue.put_nowait((pcm, received_at_us))

    def stop(self) -> None:
        self._open = False
        self._task.cancel()

    async def _send(self) -> None:
        try:
            while True:
                pcm, received_at_us = await self._queue.get()
                sequence = self._sequence
                self._sequence += 1
                log.debug("ptt_audio sequence=%d received_at_us=%d", sequence, received_at_us)
                await self.link.send(PttAudioCmd(
                    data=base64.b64encode(pcm).decode("ascii"), rate=self.rate,
                    sequence=sequence, received_at_us=received_at_us,
                ))
        except GameError:
            self._open = False
