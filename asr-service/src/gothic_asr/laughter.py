"""HTTP laughter detection; window placement stays local to recognition."""

from __future__ import annotations

import asyncio
import logging
from time import monotonic

import httpx
import numpy as np

log = logging.getLogger(__name__)

DEFAULT_URL = "http://127.0.0.1:47838"
WINDOW = 64_000
HOP = 32_000
RETRY_DELAY = 10.0


def window_starts(n: int) -> list[int]:
    """Four-second windows every two seconds, with the last aligned to the recording's end."""
    starts = list(range(0, max(n - WINDOW, 0) + 1, HOP))
    if starts[-1] + WINDOW < n:
        starts.append(n - WINDOW)
    return starts


class RemoteLaughter:
    def __init__(self, url: str = DEFAULT_URL) -> None:
        self.url = url.rstrip("/")
        self._client = httpx.Client(timeout=httpx.Timeout(1.0, connect=0.5))
        self._warned = False
        self._retry_at = 0.0

    def _warn(self, reason: object) -> None:
        if not self._warned:
            self._warned = True
            log.warning("laughter detector unavailable: %s", reason)

    def _ready(self) -> None:
        if self._warned:
            log.info("laughter detector recovered at %s", self.url)
        self._warned = False
        self._retry_at = 0.0

    def _request(self, method: str, path: str, **kwargs) -> httpx.Response | None:
        if monotonic() < self._retry_at:
            return None
        try:
            response = self._client.request(method, self.url + path, **kwargs)
            if response.status_code != 200:
                self._warn(f"HTTP {response.status_code}")
                return None
            return response
        except (httpx.ConnectError, httpx.ConnectTimeout) as err:
            self._retry_at = monotonic() + RETRY_DELAY
            self._warn(err)
        except httpx.HTTPError as err:
            self._warn(err)
        return None

    async def prepare(self) -> bool:
        """Check health off the event loop; the caller starts this as a background task."""
        response = await asyncio.to_thread(self._request, "GET", "/health")
        if response is None:
            return False
        try:
            if not isinstance(response.json(), dict):
                raise ValueError("invalid laughter health response")
        except ValueError as err:
            self._warn(err)
            return False
        self._ready()
        log.info("laughter detector ready at %s", self.url)
        return True

    def window(self, audio: np.ndarray) -> float | None:
        """Score one window in the recognizer's existing worker thread."""
        if not len(audio):
            return None
        response = self._request(
            "POST", "/window", content=np.asarray(audio, dtype="<f4").tobytes(),
            headers={"Content-Type": "application/octet-stream"},
        )
        if response is None:
            return None
        try:
            body = response.json()
            value = body.get("laughing") if isinstance(body, dict) else None
            if isinstance(value, bool) or not isinstance(value, (int, float)):
                raise ValueError("invalid laughter probability")
            probability = float(value)
            if not np.isfinite(probability):
                raise ValueError("invalid laughter probability")
        except (ValueError, OverflowError) as err:
            self._warn(err)
            return None
        self._ready()
        return probability

    def score(self, audio: np.ndarray) -> float | None:
        """The maximum score over the recording's windows, or None if detection failed."""
        if not len(audio):
            return None
        scores = [self.window(audio[start : start + WINDOW]) for start in window_starts(len(audio))]
        if any(score is None for score in scores):
            return None
        return max(score for score in scores if score is not None)

    def close(self) -> None:
        self._client.close()
