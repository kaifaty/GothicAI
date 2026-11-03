"""Primary provider with an optional fallback, a silence gate and latency logging.

An utterance streams into the primary provider while it is spoken (if the provider can stream) and
is also kept whole: when the stream fails, the fallback gets the complete recording. When the stream
reads its name hints back instead of the audio, the recording is heard again without them.
"""

from __future__ import annotations

import asyncio
import logging
import re
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, replace
from typing import cast

import numpy as np

from .audio import RATE, rms_dbfs
from .laughter import HOP, WINDOW, RemoteLaughter, window_starts
from .providers import AsrError, AsrProvider
from .providers.base import AsrStream, PartialCallback, StreamingProvider

log = logging.getLogger(__name__)

SILENCE_DBFS = -55.0  # quieter than this is treated as "nothing said"
MIN_SECONDS = 0.3
LAUGHTER_WAIT = 0.15  # maximum extra wait once the transcript is ready


@dataclass(frozen=True)
class Recognition:
    text: str
    provider: str  # "" when the audio was skipped as silence
    audio_s: float
    latency_s: float
    laughing: float | None = None


class Recognizer:
    def __init__(
        self,
        primary: AsrProvider,
        fallback: AsrProvider | None = None,
        timeout: float = 20.0,
        laughter: RemoteLaughter | None = None,
    ) -> None:
        self.primary = primary
        self.fallback = fallback
        self.timeout = timeout
        self.laughter = laughter
        self._laughter_worker = ThreadPoolExecutor(max_workers=1) if laughter is not None else None

    @property
    def providers(self) -> list[AsrProvider]:
        return [p for p in (self.primary, self.fallback) if p is not None]

    async def warmup(self) -> None:
        for p in self.providers:
            started = time.monotonic()
            try:
                await p.warmup()
                log.info("%s ready in %.1fs", p.name, time.monotonic() - started)
            except AsrError as err:
                log.warning("%s warmup failed: %s", p.name, err)

    def utterance(
        self,
        lang: str,
        on_partial: PartialCallback | None = None,
        *,
        hints: list[str] | None = None,
    ) -> Utterance:
        return Utterance(self, lang, on_partial, hints)

    async def recognize(self, audio: np.ndarray, lang: str) -> Recognition:
        """Text of 16 kHz mono audio. Raises AsrError when every provider failed."""
        utterance = self.utterance(lang)
        await utterance.feed(audio)
        return await utterance.finish()

    async def _batch(
        self, audio: np.ndarray, lang: str, providers: list[AsrProvider], started: float
    ) -> Recognition:
        audio_s = len(audio) / RATE
        errors: list[str] = []
        for p in providers:
            if p.languages is not None and lang not in p.languages:
                errors.append(f"{p.name}: language {lang!r} is not supported")
                continue
            try:
                result = await asyncio.wait_for(p.transcribe(audio, lang), self.timeout)
            except (AsrError, TimeoutError) as err:
                errors.append(f"{p.name}: {err or 'timeout'}")
                log.warning("%s failed: %s", p.name, err or "timeout")
                continue
            latency = time.monotonic() - started
            log.info("%s: %.2fs of audio in %.2fs: %s", p.name, audio_s, latency, result.text)
            return Recognition(result.text, p.name, audio_s, latency)
        raise AsrError("; ".join(errors) or "no providers")

    async def aclose(self) -> None:
        for p in self.providers:
            await p.aclose()
        if self._laughter_worker is not None:
            await asyncio.to_thread(self._laughter_worker.shutdown, wait=True, cancel_futures=True)
        if self.laughter is not None:
            await asyncio.to_thread(self.laughter.close)

    def _laughing(self, audio: np.ndarray) -> float | None:
        try:
            return self.laughter.window(audio) if self.laughter is not None else None
        except Exception as err:
            log.warning("laughter detection failed: %s", err)
            return None


class Utterance:
    """One utterance fed piece by piece: `begin`, `feed`..., then `finish` or `cancel`."""

    def __init__(
        self,
        recognizer: Recognizer,
        lang: str,
        on_partial: PartialCallback | None,
        hints: list[str] | None,
    ) -> None:
        self.recognizer = recognizer
        self.lang = lang
        self.on_partial = on_partial
        self.hints = hints
        self._chunks: list[np.ndarray] = []
        self._stream: AsrStream | None = None
        self._primary_failed = False  # its stream failed: go straight to the fallback
        self._samples = 0
        self._next_window = 0
        self._laughter_starts: set[int] = set()
        self._laughter_jobs: list[asyncio.Future[float | None]] = []

    async def begin(self) -> None:
        p = self.recognizer.primary
        if not p.streaming or (p.languages is not None and self.lang not in p.languages):
            return
        try:
            self._stream = await cast(StreamingProvider, p).open_stream(
                self.lang, self.on_partial, hints=self.hints
            )
        except AsrError as err:
            log.warning("%s stream failed to open: %s", p.name, err)
            self._primary_failed = True

    async def feed(self, audio: np.ndarray) -> None:
        self._chunks.append(audio)
        self._samples += len(audio)
        if self.recognizer.laughter is not None and self._samples >= self._next_window + WINDOW:
            recording = np.concatenate(self._chunks)
            if rms_dbfs(recording) >= SILENCE_DBFS:
                while self._next_window + WINDOW <= self._samples:
                    self._queue_laughter(recording, self._next_window)
                    self._next_window += HOP
        if self._stream is None:
            return
        try:
            await self._stream.feed(audio)
        except AsrError as err:
            log.warning("%s stream broke: %s", self.recognizer.primary.name, err)
            self._primary_failed = True
            await self._drop_stream()

    async def finish(self) -> Recognition:
        """Raises AsrError when every provider failed."""
        started = time.monotonic()
        audio = np.concatenate(self._chunks) if self._chunks else np.zeros(0, dtype=np.float32)
        if (skipped := _silence(audio)) is not None:
            self._cancel_laughter()
            await self._drop_stream()
            return skipped
        if self.recognizer.laughter is not None:
            for start in window_starts(len(audio)):
                if start not in self._laughter_starts:
                    self._queue_laughter(audio, start)
        try:
            result = await self._transcribe(audio, started)
            laughing = await self._finish_laughter()
        except BaseException:
            self._cancel_laughter()
            raise
        return replace(result, laughing=laughing, latency_s=time.monotonic() - started)

    async def _transcribe(self, audio: np.ndarray, started: float) -> Recognition:
        providers = self.recognizer.providers
        if self._stream is not None:
            name = self.recognizer.primary.name
            try:
                result = await asyncio.wait_for(self._stream.finish(), self.recognizer.timeout)
                latency = time.monotonic() - started
                log.info(
                    "%s (stream): %.2fs of audio, %.2fs after its end: %s",
                    name,
                    len(audio) / RATE,
                    latency,
                    result.text,
                )
                if not _echoes(result.text, self.hints):
                    return Recognition(result.text, name, len(audio) / RATE, latency)
                log.warning("%s read the hints back: hearing it again without them", name)
                await self._drop_stream()
                try:
                    return await self.recognizer._batch(audio, self.lang, providers, started)
                except AsrError as err:
                    log.warning("no transcript without hints: %s", err)
                    return Recognition("", name, len(audio) / RATE, time.monotonic() - started)
            except (AsrError, TimeoutError) as err:
                log.warning("%s stream failed: %s", name, err or "timeout")
                self._primary_failed = True
                await self._drop_stream()
        if self._primary_failed:
            providers = providers[1:] or providers  # the fallback, or the primary once more
        return await self.recognizer._batch(audio, self.lang, providers, started)

    async def cancel(self) -> None:
        self._cancel_laughter()
        await self._drop_stream()

    def _queue_laughter(self, audio: np.ndarray, start: int) -> None:
        self._laughter_starts.add(start)
        window = audio[start : start + WINDOW]
        self._laughter_jobs.append(asyncio.get_running_loop().run_in_executor(
            self.recognizer._laughter_worker, self.recognizer._laughing, window
        ))

    async def _finish_laughter(self) -> float | None:
        if not self._laughter_jobs:
            return None
        try:
            scores = await asyncio.wait_for(asyncio.gather(*self._laughter_jobs), LAUGHTER_WAIT)
        except TimeoutError:
            log.warning("laughter detection exceeded %.2fs after the transcript", LAUGHTER_WAIT)
            return None
        if any(score is None for score in scores):
            return None
        return max(score for score in scores if score is not None)

    def _cancel_laughter(self) -> None:
        for job in self._laughter_jobs:
            job.cancel()

    async def _drop_stream(self) -> None:
        stream, self._stream = self._stream, None
        if stream is not None:
            await stream.cancel()


def _silence(audio: np.ndarray) -> Recognition | None:
    audio_s = len(audio) / RATE
    level = rms_dbfs(audio)
    if audio_s < MIN_SECONDS or level < SILENCE_DBFS:
        log.info("skipped %.2fs of audio at %.0f dBFS (silence)", audio_s, level)
        return Recognition("", "", audio_s, 0.0)
    return None


_PIECE = re.compile(r"[,.;:!?…]+")


def _fold(text: str) -> str:
    return " ".join(text.lower().replace("ё", "е").split()).strip(" '()-")


def _echoes(text: str, hints: list[str] | None) -> bool:
    """The text is two hints in a row, in their order: the model read its prompt back instead of the
    audio, as Inworld does on laughter and other sounds without words («Благо, Иннос, Инноса, Инносу,
    …»). The last piece may be cut off. Real speech never names two case forms of one name in a row."""
    if not hints:
        return False
    order = [_fold(hint) for hint in hints]
    pieces = [_fold(piece) for piece in _PIECE.split(text)]
    while pieces and not pieces[-1]:
        pieces.pop()
    for i, (a, b) in enumerate(zip(pieces, pieces[1:], strict=False)):
        if not a or not b:
            continue
        last = i + 2 == len(pieces)
        for j, hint in enumerate(order[:-1]):
            after = order[j + 1]
            if hint == a and (after == b or (last and len(b) >= 2 and after.startswith(b))):
                return True
    return False
