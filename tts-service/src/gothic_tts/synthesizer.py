"""Primary provider with an optional fallback and latency logging.

A provider that fails is left out until a check in the background (its `warmup`) finds it working
again, so lines do not each wait for it to fail. With every provider out a line fails at once, and
the game shows it as a subtitle. The fallback takes over only before any audio was produced:
switching mid-line would change the voice halfway through a sentence.

Every line is brought to the loudness of the game's own dialogue (see loudness.py). Only lines in the
voice's usual manner teach how loud it speaks: an angry line is louder and stays louder, and the next
calm line must not come out quieter for it.
"""

from __future__ import annotations

import asyncio
import logging
import time
from collections.abc import Awaitable, Callable
from dataclasses import dataclass

from .loudness import Leveler, Line
from .providers import TtsError, TtsProvider, Voice
from .providers.inworld import InworldTTS, _UnknownVoice

log = logging.getLogger(__name__)

# (pcm chunk, the provider producing it: its name and sample rate)
SpeechCallback = Callable[[bytes, TtsProvider], Awaitable[None]]

RETRY_S = 5.0  # the first check of a failed provider; the wait doubles while it keeps failing
RETRY_MAX_S = 120.0


@dataclass(frozen=True)
class Speech:
    provider: str
    rate: int
    audio_s: float
    first_chunk_s: float
    latency_s: float  # until the last chunk


class Synthesizer:
    def __init__(
        self,
        primary: TtsProvider,
        fallback: TtsProvider | None = None,
        *,
        retry_s: float = RETRY_S,
        retry_max_s: float = RETRY_MAX_S,
        leveler: Leveler | None = None,
    ) -> None:
        self.primary = primary
        self.fallback = fallback
        self.leveler = leveler  # None: as loud as the provider speaks
        self.retry_s = retry_s
        self.retry_max_s = retry_max_s
        self._down: dict[int, asyncio.Task[None]] = {}  # id(provider) -> its recovery check
        self._failures: dict[int, int] = {}  # failures in a row, for the wait before the next check

    @property
    def providers(self) -> list[TtsProvider]:
        return [p for p in (self.primary, self.fallback) if p is not None]

    def available(self) -> list[TtsProvider]:
        return [p for p in self.providers if id(p) not in self._down]

    async def warmup(self) -> None:
        """Gets the primary ready; the fallback only if the primary is not (a warm fallback may cost)."""
        for p in self.providers:
            started = time.monotonic()
            try:
                await p.warmup()
                log.info("%s ready in %.1fs", p.name, time.monotonic() - started)
                return
            except TtsError as err:
                log.warning("%s warmup failed: %s", p.name, err)
                self._mark_down(p)

    async def speak(
        self,
        text: str,
        voice: Voice,
        on_chunk: SpeechCallback,
        *,
        original_voice: Voice | None = None,
    ) -> Speech:
        """Raises TtsError when every provider failed or is out (or one failed after it started speaking)."""
        started = time.monotonic()
        providers = self.available()
        if not providers:
            raise TtsError(f"no voice: {', '.join(p.name for p in self.providers)} unavailable")
        errors: list[str] = []
        leveler = self.leveler
        for p in providers:
            selected = original_voice if original_voice is not None and p.supports_cloning else voice
            first: float | None = None
            line = leveler.start(p.name, selected.id, p.rate) if leveler is not None else None

            async def chunk(pcm: bytes, p: TtsProvider = p, line: Line | None = line) -> None:
                nonlocal first
                if first is None:
                    first = time.monotonic() - started
                if line is not None:
                    pcm = line.process(pcm)
                if pcm:
                    await on_chunk(pcm, p)

            try:
                audio_s = await p.synthesize(text, selected, chunk)
            except TtsError as err:
                if (
                    selected is original_voice and first is None
                    and isinstance(p, InworldTTS) and isinstance(err, _UnknownVoice)
                ):
                    p._forget(selected.id)
                    raise  # only this clone is gone; the service retries with the designed voice
                log.warning("%s failed: %s", p.name, err)
                self._mark_down(p)
                if first is not None:
                    raise  # the line is half spoken in this voice
                errors.append(f"{p.name}: {err}")
                continue
            self._failures.pop(id(p), None)
            gain = ""
            if leveler is not None and line is not None:
                if tail := line.flush():
                    await on_chunk(tail, p)
                if not selected.style:
                    leveler.learn(p.name, selected.id, line)
                gain = f", {line.gain_db:+.1f} dB"
            if selected.style:
                gain += f", {selected.style}"
            took = time.monotonic() - started
            log.info(
                "%s: %.2fs of audio in %.2fs (first chunk %.2fs) [%s%s]: %s",
                p.name,
                audio_s,
                took,
                first or 0.0,
                selected.id,
                gain,
                text[:60],
            )
            return Speech(p.name, p.rate, audio_s, first or 0.0, took)
        raise TtsError("; ".join(errors) or "no providers")

    def _mark_down(self, p: TtsProvider) -> None:
        if id(p) in self._down:
            return
        n = self._failures[id(p)] = self._failures.get(id(p), 0) + 1
        wait = min(self.retry_s * 2 ** (n - 1), self.retry_max_s)
        others = [q.name for q in self.available() if q is not p]
        log.warning(
            "%s left out, next check in %.0fs; %s",
            p.name,
            wait,
            f"voice from {others[0]} meanwhile" if others else "no voice meanwhile (subtitles only)",
        )
        self._down[id(p)] = asyncio.create_task(self._recover(p, wait))

    async def _recover(self, p: TtsProvider, wait: float) -> None:
        while True:
            await asyncio.sleep(wait)
            try:
                await p.warmup()
            except Exception as err:  # any failure means it is still down
                wait = min(wait * 2, self.retry_max_s)
                log.debug("%s still unavailable (%s), next check in %.0fs", p.name, err, wait)
                continue
            log.info("%s is back", p.name)
            del self._down[id(p)]
            return

    async def aclose(self) -> None:
        checks = list(self._down.values())
        for task in checks:
            task.cancel()
        await asyncio.gather(*checks, return_exceptions=True)
        self._down.clear()
        for p in self.providers:
            await p.aclose()
