"""CPU laughter detection with the MIT-licensed zencastr/laughter-detection model.

Window placement follows ``laughter_core.window_audio`` at the pinned model revision:
https://huggingface.co/zencastr/laughter-detection/blob/849623f8338341d7e07be00a487337e0086ec14d/laughter_core.py
"""

from __future__ import annotations

import asyncio
import hashlib
import logging
from pathlib import Path
from typing import Any

import httpx
import numpy as np

log = logging.getLogger(__name__)

MODEL_REVISION = "849623f8338341d7e07be00a487337e0086ec14d"
MODEL_URL = f"https://huggingface.co/zencastr/laughter-detection/resolve/{MODEL_REVISION}/model.onnx"
MODEL_SHA256 = "fa6445a9738c66b319d36b716d860b21703fba1fe41e1137e82ca288b04cb74f"
MODEL_BYTES = 362_287_206
DEFAULT_MODEL = Path.home() / ".local/share/gothic-ai/asr/laughter/849623f83383/model.onnx"
WINDOW = 64_000
HOP = 32_000


def window_starts(n: int) -> list[int]:
    """Four-second windows every two seconds, with the last aligned to the recording's end."""
    starts = list(range(0, max(n - WINDOW, 0) + 1, HOP))
    if starts[-1] + WINDOW < n:
        starts.append(n - WINDOW)
    return starts


async def fetch_model(path: Path = DEFAULT_MODEL) -> Path:
    """Download the pinned model if absent, verifying it before publishing the file."""
    path = await asyncio.to_thread(Path(path).expanduser)
    if await asyncio.to_thread(path.is_file):
        return path
    await asyncio.to_thread(path.parent.mkdir, parents=True, exist_ok=True)
    partial = path.with_name(f"{path.name}.part")
    digest = hashlib.sha256()
    size = 0
    try:
        async with httpx.AsyncClient(follow_redirects=True, timeout=60.0) as client:
            async with client.stream("GET", MODEL_URL) as response:
                response.raise_for_status()
                with await asyncio.to_thread(partial.open, "wb") as output:
                    async for chunk in response.aiter_bytes(chunk_size=1024 * 1024):
                        await asyncio.to_thread(output.write, chunk)
                        digest.update(chunk)
                        size += len(chunk)
        if size != MODEL_BYTES or digest.hexdigest() != MODEL_SHA256:
            raise ValueError(f"laughter model verification failed: {size} bytes, sha256 {digest.hexdigest()}")
        await asyncio.to_thread(partial.replace, path)
        return path
    except BaseException:
        await asyncio.to_thread(partial.unlink, missing_ok=True)
        raise


class LaughterDetector:
    def __init__(self, model: Path = DEFAULT_MODEL, threads: int = 4) -> None:
        if threads < 1:
            raise ValueError("laughter threads must be positive")
        self.model = Path(model).expanduser()
        self.threads = threads
        self._session: Any = None
        self._warned = False

    def _warn(self, reason: object) -> None:
        if not self._warned:
            self._warned = True
            log.warning("laughter detector unavailable: %s", reason)

    def _load(self) -> None:
        import onnxruntime as ort

        options = ort.SessionOptions()
        options.intra_op_num_threads = self.threads
        self._session = ort.InferenceSession(
            str(self.model), sess_options=options, providers=["CPUExecutionProvider"]
        )
        self._warned = False

    async def prepare(self) -> bool:
        """Fetch and load off the event loop; recognition can continue while this runs."""
        try:
            await fetch_model(self.model)
            await asyncio.to_thread(self._load)
        except Exception as err:
            self._warn(err)
            return False
        return True

    def window(self, audio: np.ndarray) -> float | None:
        """Score one window. The caller serializes inference in its worker thread."""
        if not len(audio):
            return None
        if self._session is None:
            self._warn(f"model is not ready at {self.model}")
            return None
        try:
            waveform = np.zeros((1, WINDOW), dtype=np.float32)
            waveform[0, : len(audio)] = audio
            result = self._session.run(
                ["clip_prob"],
                {"waveform": waveform, "n_valid": np.array([len(audio)], dtype=np.int64)},
            )
            probability = float(np.asarray(result[0]).reshape(-1)[0])
            if not np.isfinite(probability):
                raise ValueError("non-finite laughter probability")
            return probability
        except Exception as err:
            self._warn(err)
            return None

    def score(self, audio: np.ndarray) -> float | None:
        """The maximum score over the recording's windows, or None if detection failed."""
        if not len(audio):
            return None
        scores = [self.window(audio[start : start + WINDOW]) for start in window_starts(len(audio))]
        if any(score is None for score in scores):
            return None
        return max(score for score in scores if score is not None)
