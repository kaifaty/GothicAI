"""Provider interface: text and a voice in, 16-bit mono PCM out (streamed in chunks)."""

from __future__ import annotations

from collections.abc import Awaitable, Callable
from dataclasses import dataclass
from typing import Protocol

ChunkCallback = Callable[[bytes], Awaitable[None]]


@dataclass(frozen=True)
class Voice:
    """Who speaks, and how this line is said. Providers use what they understand: VoxCPM designs a
    voice from `description` (and keeps it per `id`), preset-voice APIs take `preset`. `style` is an
    English instruction for this line's delivery ("say angrily with force"), empty for the voice's
    usual manner; the voice stays the same. `sample`: a recording (WAV or MP3) to clone the voice from
    instead (Inworld; original game voices and the shorts' English dub), in any language the
    provider's lines are not."""

    id: str
    description: str = ""
    preset: str | None = None
    style: str = ""
    sample: str = ""


class TtsError(RuntimeError):
    """A provider failed (unreachable, quota, bad credentials, bad text)."""


class TtsProvider(Protocol):
    name: str
    rate: int  # sample rate of the PCM it produces
    supports_cloning: bool  # automatic original voices are offered only to these providers

    async def warmup(self) -> None: ...

    async def synthesize(self, text: str, voice: Voice, on_chunk: ChunkCallback) -> float:
        """Speak `text`, passing PCM chunks as they are generated. Returns the audio length in seconds."""
        ...

    async def aclose(self) -> None: ...
