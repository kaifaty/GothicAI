"""A face track for each voiced npc line (protocol v63, step 10 of the hero head plan).

The line's whole voice goes to the Audio2Face service (`gothic-a2f`, a2f-service/) and the game gets the
weights of the 52 ARKit shapes per frame in `say_face`, before the line's audio. `fake` makes a track from
the loudness of the voice without a network (tests, e2e). A track that does not come in time, or at all,
costs the line nothing: it goes out as before, without a face track.
"""

from __future__ import annotations

import asyncio
import base64
import binascii
import logging
import math
import time
from collections.abc import Callable, Iterable, Mapping
from dataclasses import dataclass, field
from typing import Literal, Protocol
from urllib.parse import urlsplit

import httpx
import numpy as np

from .protocol import Mood, SayFaceCmd

log = logging.getLogger(__name__)

A2F_PORT = 47839
FPS = 30
DEFAULT_TIMEOUT_S = 1.5
MAX_AUDIO_S = 30.0  # the service answers 413 to a longer line
RETRY_S = 30.0  # an unavailable service is not asked again for this long; lines do not wait meanwhile
WARN_EVERY_S = 60.0

A2fMode = Literal["service", "fake", "off"]

# the solver's order of Audio2Face (its tongue weights are not passed on)
ARKIT_SHAPES = (
    "eyeBlinkLeft eyeLookDownLeft eyeLookInLeft eyeLookOutLeft eyeLookUpLeft eyeSquintLeft eyeWideLeft "
    "eyeBlinkRight eyeLookDownRight eyeLookInRight eyeLookOutRight eyeLookUpRight eyeSquintRight "
    "eyeWideRight "
    "jawForward jawLeft jawRight jawOpen mouthClose mouthFunnel mouthPucker mouthLeft mouthRight "
    "mouthSmileLeft mouthSmileRight mouthFrownLeft mouthFrownRight mouthDimpleLeft mouthDimpleRight "
    "mouthStretchLeft mouthStretchRight mouthRollLower mouthRollUpper mouthShrugLower mouthShrugUpper "
    "mouthPressLeft mouthPressRight mouthLowerDownLeft mouthLowerDownRight mouthUpperUpLeft "
    "mouthUpperUpRight "
    "browDownLeft browDownRight browInnerUp browOuterUpLeft browOuterUpRight cheekPuff cheekSquintLeft "
    "cheekSquintRight noseSneerLeft noseSneerRight tongueOut"
).split()

# emotions of Audio2Face v3.0
EMOTIONS = frozenset(
    ("amazement", "anger", "cheekiness", "disgust", "fear", "grief", "joy", "outofbreath", "pain", "sadness")
)

# the line's mood -> the label the track is made with; empty: neutral (no label)
DEFAULT_EMOTIONS: dict[str, str] = {
    "neutral": "",
    "friendly": "joy:0.6",
    "angry": "anger:0.8",
    "hostile": "anger:0.8,disgust:0.4",
    "afraid": "fear:0.8",
    "pain": "pain:0.8",
}

FAKE_FULL_RMS = 8000.0  # fake: this loudness of a frame opens the jaw fully

Words = list[tuple[int, float, float]]


def a2f_url(url: str | None, decider_url: str | None) -> str | None:
    """Explicit Audio2Face service, or HTTP port 47839 on the decider's host."""
    if url:
        return url
    host = urlsplit(decider_url).hostname if decider_url else None
    if host and ":" in host:
        host = f"[{host}]"
    return f"http://{host}:{A2F_PORT}" if host else None


def check_emotion(label: str) -> str:
    """`anger:0.8,disgust:0.3` as the service takes it; empty: neutral. ValueError for anything else."""
    label = label.strip()
    if not label:
        return ""
    for item in label.split(","):
        name, sep, value = item.strip().partition(":")
        if not sep or name not in EMOTIONS:
            raise ValueError(f"unknown emotion {item.strip()!r} (one of {', '.join(sorted(EMOTIONS))})")
        if not 0.0 <= float(value) <= 1.0:
            raise ValueError(f"emotion strength out of 0..1: {item.strip()!r}")
    return ",".join(item.strip() for item in label.split(","))


def emotion_table(overrides: Iterable[str] = ()) -> dict[str, str]:
    """The default table with `mood=label` overrides (`--a2f-emotion`); an empty label: a neutral mood."""
    table = dict(DEFAULT_EMOTIONS)
    for item in overrides:
        mood, sep, label = item.partition("=")
        if not sep or mood.strip() not in DEFAULT_EMOTIONS:
            moods = ", ".join(DEFAULT_EMOTIONS)
            raise ValueError(f"expected <mood>=<label> with a mood of {moods}: {item!r}")
        table[mood.strip()] = check_emotion(label)
    return table


class FaceError(RuntimeError):
    """No track for this line. `down`: the service is not there (connection, time-out, 5xx)."""

    def __init__(self, message: str, *, down: bool = False) -> None:
        super().__init__(message)
        self.down = down


@dataclass(frozen=True)
class FaceTrack:
    fps: int
    shapes: list[str]
    data: str  # base64: frames x shapes bytes
    frames: int


class FaceSource(Protocol):
    name: str

    async def track(self, pcm: bytes, rate: int, emotion: str) -> FaceTrack | None: ...

    async def aclose(self) -> None: ...


class A2fClient:
    """`POST /v1/face?rate=&emotion=` of `gothic-a2f`: the line's 16-bit mono PCM in, its track out.
    After the service was unavailable, it is not asked for `RETRY_S` (None at once)."""

    name = "service"

    def __init__(
        self,
        url: str,
        timeout: float = DEFAULT_TIMEOUT_S,
        *,
        transport: httpx.AsyncBaseTransport | None = None,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.url = url
        self.timeout = timeout
        self._clock = clock
        self._http = httpx.AsyncClient(base_url=url.rstrip("/") + "/", transport=transport, timeout=timeout)
        self._paused_until = 0.0

    async def track(self, pcm: bytes, rate: int, emotion: str) -> FaceTrack | None:
        if self._clock() < self._paused_until:
            return None
        params = {"rate": str(rate)} | ({"emotion": emotion} if emotion else {})
        try:
            async with asyncio.timeout(self.timeout):
                response = await self._http.post(
                    "v1/face", params=params, content=pcm,
                    headers={"Content-Type": "application/octet-stream"},
                )
        except (TimeoutError, httpx.TimeoutException):
            self._down()
            raise FaceError(f"no answer from {self.url} in {self.timeout:g}s", down=True) from None
        except httpx.TransportError as err:
            self._down()
            raise FaceError(f"{self.url}: {type(err).__name__}: {err}", down=True) from err
        if response.status_code >= 500:
            self._down()
            raise FaceError(f"{self.url}: HTTP {response.status_code}", down=True)
        if response.status_code != 200:
            raise FaceError(f"{self.url}: HTTP {response.status_code}: {response.text[:200]}")
        try:
            return _parse(response.json())
        except (ValueError, TypeError, KeyError, binascii.Error) as err:
            raise FaceError(f"{self.url}: bad track: {err}") from err

    def _down(self) -> None:
        self._paused_until = self._clock() + RETRY_S

    async def aclose(self) -> None:
        await self._http.aclose()


def _parse(body: dict) -> FaceTrack:
    fps, shapes, frames, data = body["fps"], body["shapes"], body["frames"], body["data"]
    if not isinstance(fps, int) or fps <= 0:
        raise ValueError(f"fps {fps!r}")
    if not isinstance(shapes, list) or not shapes or not all(isinstance(s, str) and s for s in shapes):
        raise ValueError("shapes must be a list of names")
    if not isinstance(frames, int) or frames < 0 or not isinstance(data, str):
        raise ValueError("frames and data")
    if len(base64.b64decode(data, validate=True)) != frames * len(shapes):
        raise ValueError(f"{frames} frames x {len(shapes)} shapes do not match the data")
    return FaceTrack(fps, shapes, data, frames)


class FakeFace:
    """A track without a network, the same from the same voice: `jawOpen` follows the loudness of each
    frame, the other shapes stay 0."""

    name = "fake"

    async def track(self, pcm: bytes, rate: int, emotion: str) -> FaceTrack | None:
        samples = np.frombuffer(pcm[: len(pcm) // 2 * 2], dtype="<i2").astype(np.float64)
        frames = math.ceil(len(samples) * FPS / rate)
        weights = np.zeros((frames, len(ARKIT_SHAPES)), dtype=np.uint8)
        jaw = ARKIT_SHAPES.index("jawOpen")
        for i in range(frames):
            window = samples[i * rate // FPS : (i + 1) * rate // FPS]
            rms = math.sqrt(float(np.mean(window * window))) if len(window) else 0.0
            weights[i, jaw] = round(255 * min(1.0, rms / FAKE_FULL_RMS))
        return FaceTrack(FPS, list(ARKIT_SHAPES), base64.b64encode(weights.tobytes()).decode(), frames)

    async def aclose(self) -> None:
        pass


def make_source(mode: A2fMode, url: str | None, timeout: float = DEFAULT_TIMEOUT_S) -> FaceSource | None:
    """`service` without an address is off: there is nowhere to ask."""
    if mode == "fake":
        return FakeFace()
    if mode == "service" and url:
        return A2fClient(url, timeout)
    return None


class FaceVoice:
    """Which lines wait for a face track, the emotion of their mood, and the track as `say_face`.
    Failures are warned about at most once a minute."""

    def __init__(
        self,
        source: FaceSource,
        emotions: Mapping[str, str] | None = None,
        *,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.source = source
        self.emotions = dict(DEFAULT_EMOTIONS if emotions is None else emotions)
        self._clock = clock
        self._warned_at: float | None = None

    @staticmethod
    def wants(line: str, *, filler: bool = False) -> bool:
        """Quick first reactions keep their speed; the original's lines (`DIA_*`) are voiced by the game."""
        return not filler and not line.startswith("DIA_")

    def emotion(self, mood: Mood | None) -> str:
        return self.emotions.get(mood or "neutral", "")

    async def say_face(
        self, npc: str, line: str, pcm: bytes, rate: int, mood: Mood | None
    ) -> SayFaceCmd | None:
        if not pcm or rate <= 0:
            return None
        if len(pcm) / 2 / rate > MAX_AUDIO_S:
            self._warn(f"{line}: {len(pcm) / 2 / rate:.1f}s of voice is longer than {MAX_AUDIO_S:g}s")
            return None
        emotion = self.emotion(mood)
        started = time.monotonic()
        try:
            track = await self.source.track(pcm, rate, emotion)
        except FaceError as err:
            self._warn(f"{line}: {err}" + (f"; not asked for {RETRY_S:g}s" if err.down else ""))
            return None
        if track is None:
            return None
        log.info(
            "%s face %s: %d frames in %.3fs (%s%s)", npc, line, track.frames, time.monotonic() - started,
            self.source.name, f", {emotion}" if emotion else "",
        )
        return SayFaceCmd(
            npc=npc, line=line, fps=track.fps, shapes=track.shapes, data=track.data, emotion=emotion or None
        )

    def _warn(self, text: str) -> None:
        now = self._clock()
        if self._warned_at is not None and now - self._warned_at < WARN_EVERY_S:
            log.debug("no face track: %s", text)
            return
        self._warned_at = now
        log.warning("no face track, the line goes without one: %s", text)


@dataclass
class HeldVoice:
    """A line's voice kept until it is whole: the face track is made from all of it, and the game gets
    `say`, `say_face`, then these chunks with their word timings as they came."""

    rate: int = 0
    provider: str = ""
    chunks: list[tuple[bytes, Words | None]] = field(default_factory=list)

    async def start(self, rate: int, provider: str) -> None:
        self.rate, self.provider = rate, provider

    def pcm(self) -> bytes:
        return b"".join(pcm for pcm, _ in self.chunks)
