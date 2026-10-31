"""Primary provider with an optional fallback, a silence gate and latency logging.

An utterance streams into the primary provider while it is spoken (if the provider can stream) and
is also kept whole: when the stream fails, the fallback gets the complete recording.
"""

from __future__ import annotations

import asyncio
import logging
import time
from dataclasses import dataclass
from typing import cast

import numpy as np

from .audio import RATE, rms_dbfs
from .providers import AsrError, AsrProvider
from .providers.base import AsrStream, PartialCallback, StreamingProvider

log = logging.getLogger(__name__)

SILENCE_DBFS = -55.0  # quieter than this is treated as "nothing said"
MIN_SECONDS = 0.3


@dataclass(frozen=True)
class Recognition:
    text: str
    provider: str  # "" when the audio was skipped as silence
    audio_s: float
    latency_s: float


class Recognizer:
    def __init__(
        self, primary: AsrProvider, fallback: AsrProvider | None = None, timeout: float = 20.0
    ) -> None:
        self.primary = primary
        self.fallback = fallback
        self.timeout = timeout

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
        started = time.monotonic()
        if (skipped := _silence(audio)) is not None:
            return skipped
        return await self._batch(audio, lang, self.providers, started)

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
            await self._drop_stream()
            return skipped
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
                return Recognition(result.text, name, len(audio) / RATE, latency)
            except (AsrError, TimeoutError) as err:
                log.warning("%s stream failed: %s", name, err or "timeout")
                self._primary_failed = True
                await self._drop_stream()
        if self._primary_failed:
            providers = providers[1:] or providers  # the fallback, or the primary once more
        return await self.recognizer._batch(audio, self.lang, providers, started)

    async def cancel(self) -> None:
        await self._drop_stream()

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
