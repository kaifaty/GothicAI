"""Canned speech for tests and offline runs: a quiet tone about as long as the text would take."""

from __future__ import annotations

import asyncio
import math
import struct

from .base import ChunkCallback, TtsError, Voice


def _flag(v: str | bool) -> bool:
    return v in (True, "1", "true", "yes")


class FakeProvider:
    name = "fake"
    rate = 16000
    supports_cloning = False
    tone_phase_step = 1 / 8
    tone_frequency_hz = rate * tone_phase_step / math.tau

    def __init__(
        self, delay: str | float = 0.0, fail: str | bool = False, ms_per_char: str | float = 60
    ) -> None:
        self.delay = float(delay)
        self.fail = _flag(fail)
        self.ms_per_char = float(ms_per_char)
        self.calls: list[tuple[str, Voice]] = []

    async def warmup(self) -> None:
        if self.fail:
            raise TtsError("fake provider failure")

    async def synthesize(self, text: str, voice: Voice, on_chunk: ChunkCallback) -> float:
        self.calls.append((text, voice))
        await asyncio.sleep(self.delay)
        if self.fail:
            raise TtsError("fake provider failure")
        n = int(len(text) * self.ms_per_char / 1000 * self.rate)
        pcm = struct.pack(f"<{n}h", *(int(3000 * math.sin(i * self.tone_phase_step)) for i in range(n)))
        for i in range(0, len(pcm), 3200):
            await on_chunk(pcm[i : i + 3200])
        return n / self.rate

    async def aclose(self) -> None:
        pass
