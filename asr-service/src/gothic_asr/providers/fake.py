"""Canned results for tests and offline runs."""

from __future__ import annotations

import asyncio

import numpy as np

from .base import AsrError, PartialCallback, Transcript


def _flag(v: str | bool) -> bool:
    return v in (True, "1", "true", "yes")


class FakeProvider:
    """Answers every utterance with `text`. With `stream`, it also streams: one more word of `text`
    as a partial per fed chunk; `stream_fail` breaks the stream at the end (to test fallbacks)."""

    name = "fake"
    languages = None

    def __init__(
        self,
        text: str = "Привет, Ксардас.",
        delay: str | float = 0.0,
        fail: str | bool = False,
        stream: str | bool = False,
        stream_fail: str | bool = False,
    ) -> None:
        self.text = text
        self.delay = float(delay)
        self.fail = _flag(fail)
        self.streaming = _flag(stream)
        self.stream_fail = _flag(stream_fail)
        self.calls: list[tuple[int, str]] = []  # batch calls: (samples, lang)
        self.streams: list[_FakeStream] = []

    async def warmup(self) -> None:
        pass

    async def transcribe(self, audio: np.ndarray, lang: str) -> Transcript:
        self.calls.append((len(audio), lang))
        await asyncio.sleep(self.delay)
        if self.fail:
            raise AsrError("fake provider failure")
        return Transcript(self.text, self.name)

    async def open_stream(
        self,
        lang: str,
        on_partial: PartialCallback | None = None,
        *,
        hints: list[str] | None = None,
    ) -> _FakeStream:
        stream = _FakeStream(self, on_partial)
        self.streams.append(stream)
        return stream

    async def aclose(self) -> None:
        pass


class _FakeStream:
    def __init__(self, owner: FakeProvider, on_partial: PartialCallback | None) -> None:
        self.owner = owner
        self.on_partial = on_partial
        self.samples = 0
        self.cancelled = False
        self._words = owner.text.split()
        self._shown = 0

    async def feed(self, audio: np.ndarray) -> None:
        self.samples += len(audio)
        if self._shown < len(self._words):
            self._shown += 1
            if self.on_partial is not None:
                self.on_partial(" ".join(self._words[: self._shown]))

    async def finish(self) -> Transcript:
        await asyncio.sleep(self.owner.delay)
        if self.owner.stream_fail:
            raise AsrError("fake stream failure")
        return Transcript(self.owner.text, self.owner.name)

    async def cancel(self) -> None:
        self.cancelled = True
