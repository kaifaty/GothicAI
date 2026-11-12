"""The face track format shared by the service, the AI server and the engine (step 10 of the head plan).

fps 30, frame 0 is the first sample of the line; data is base64 of uint8, frames one after another,
a byte per shape in the order of `shapes`; weight = byte / 255.
"""

import base64
import math
from fractions import Fraction

import numpy as np
from scipy.signal import resample_poly

from .engine import NETWORK_FPS, SAMPLE_RATE

FPS = 30
MIN_RATE, MAX_RATE = 8000, 48000
MAX_SECONDS = 30.0


class BadRequest(ValueError):
    pass


def parse_rate(text: str | None) -> int:
    if text is None:
        raise BadRequest("rate is required: POST /v1/face?rate=<Hz>")
    try:
        rate = int(text)
    except ValueError:
        raise BadRequest(f"rate must be an integer, got {text!r}") from None
    if not MIN_RATE <= rate <= MAX_RATE:
        raise BadRequest(f"rate must be {MIN_RATE}..{MAX_RATE} Hz, got {rate}")
    return rate


def parse_emotion(label: str | None, names: list[str]) -> np.ndarray:
    """`anger:0.8,disgust:0.3` -> the network's emotion vector; empty or missing is neutral."""
    vector = np.zeros(len(names), dtype=np.float32)
    if not label:
        return vector
    seen = set()
    for item in label.split(","):
        name, sep, value = item.strip().partition(":")
        if not sep:
            raise BadRequest(f"emotion item {item!r} is not name:strength")
        if name not in names:
            raise BadRequest(f"unknown emotion {name!r}; known: {', '.join(names)}")
        if name in seen:
            raise BadRequest(f"emotion {name!r} is given twice")
        try:
            strength = float(value)
        except ValueError:
            raise BadRequest(f"strength of {name!r} must be a number, got {value!r}") from None
        if not (math.isfinite(strength) and 0.0 <= strength <= 1.0):
            raise BadRequest(f"strength of {name!r} must be 0..1, got {value}")
        seen.add(name)
        vector[names.index(name)] = strength
    return vector


def decode_pcm(body: bytes, rate: int) -> np.ndarray:
    """PCM s16le mono at `rate` -> float32 at 16 kHz.

    s16 / 32767, as AudioFile, the wav reader of a2f-weights, does."""
    if not body:
        raise BadRequest("empty body: expected PCM s16le mono")
    if len(body) % 2:
        raise BadRequest("odd body length: expected PCM s16le mono")
    audio = np.frombuffer(body, dtype="<i2").astype(np.float32) / 32767.0
    if rate != SAMPLE_RATE:
        ratio = Fraction(SAMPLE_RATE, rate)
        audio = resample_poly(audio, ratio.numerator, ratio.denominator).astype(np.float32)
    return audio


def too_long(body: bytes, rate: int) -> bool:
    return len(body) / 2 / rate > MAX_SECONDS


def to_track(weights60: np.ndarray) -> tuple[int, str]:
    """60 fps weights -> every second frame from frame 0, one byte per weight; (frames, base64)."""
    step = NETWORK_FPS // FPS
    frames = weights60[::step]
    data = np.clip(np.rint(frames * 255.0), 0, 255).astype(np.uint8)
    return len(frames), base64.b64encode(data.tobytes()).decode("ascii")
