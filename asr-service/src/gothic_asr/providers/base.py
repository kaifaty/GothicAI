"""Provider interface: audio in (float32, 16 kHz, mono), text out.

Batch providers get the whole utterance after it ends. Streaming providers (`streaming = True`) also
implement `open_stream`: audio is fed while the player is still speaking, partial text comes back
as it is recognized, and only a short tail is left when the utterance ends.
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from typing import Protocol

import numpy as np


@dataclass(frozen=True)
class Transcript:
    text: str
    provider: str
    confidence: float | None = None


class AsrError(RuntimeError):
    """A provider failed (model missing, network, quota, bad credentials)."""


PartialCallback = Callable[[str], None]  # text recognized so far


class AsrStream(Protocol):
    async def feed(self, audio: np.ndarray) -> None: ...

    async def finish(self) -> Transcript:
        """The audio has ended: the final text."""
        ...

    async def cancel(self) -> None: ...


class AsrProvider(Protocol):
    name: str
    streaming: bool  # implements open_stream
    languages: frozenset[str] | None  # None: any language the model knows

    async def warmup(self) -> None:
        """Load models / open connections so the first request is fast."""
        ...

    async def transcribe(self, audio: np.ndarray, lang: str) -> Transcript: ...

    async def aclose(self) -> None: ...


class StreamingProvider(AsrProvider, Protocol):
    async def open_stream(
        self,
        lang: str,
        on_partial: PartialCallback | None = None,
        *,
        hints: list[str] | None = None,
    ) -> AsrStream: ...
