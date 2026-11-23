"""HTTP contract of gothic-a2f; one worker thread loads the model and runs the lines one after another."""

import asyncio
import logging
import time
from concurrent.futures import ThreadPoolExecutor

import numpy as np
from aiohttp import web

from .engine import SAMPLE_RATE
from .track import (
    FPS,
    MAX_RATE,
    MAX_SECONDS,
    BadRequest,
    decode_pcm,
    parse_emotion,
    parse_rate,
    to_track,
    too_long,
)

log = logging.getLogger(__name__)
MAX_BODY = int(MAX_SECONDS * MAX_RATE * 2)


class FaceService:
    """`loader()` returns an engine with `shapes`, `emotions` and `run(audio16k, emotion) -> weights60`."""

    def __init__(self, loader, *, model: str = "v3.0", face: str = "Mark") -> None:
        self.model, self.face = model, face
        self.engine = None
        self._loader = loader
        # one thread: the model is used by one line at a time, a second request waits in the queue
        self._worker = ThreadPoolExecutor(max_workers=1, thread_name_prefix="gothic-a2f")

    async def load(self) -> None:
        self.engine = await asyncio.get_running_loop().run_in_executor(self._worker, self._load)

    def _load(self):
        engine = self._loader()
        # warmup: the first run allocates the rest of the buffers
        engine.run(np.zeros(SAMPLE_RATE, dtype=np.float32), np.zeros(len(engine.emotions), dtype=np.float32))
        return engine

    async def health(self, request: web.Request) -> web.Response:
        if self.engine is None:
            return web.json_response({"ok": False, "error": "loading"}, status=503)
        return web.json_response({"ok": True, "model": self.model, "face": self.face, "fps": FPS})

    async def face_track(self, request: web.Request) -> web.Response:
        try:
            body = await request.read()
        except web.HTTPRequestEntityTooLarge:
            return web.json_response({"error": f"audio longer than {MAX_SECONDS:g} s"}, status=413)
        try:
            rate = parse_rate(request.query.get("rate"))
            if self.engine is None:
                return web.json_response({"error": "loading"}, status=503)
            emotion = parse_emotion(request.query.get("emotion"), self.engine.emotions)
            if too_long(body, rate):
                return web.json_response({"error": f"audio longer than {MAX_SECONDS:g} s"}, status=413)
            audio = decode_pcm(body, rate)
        except BadRequest as exc:
            return web.json_response({"error": str(exc)}, status=400)
        try:
            weights, ms = await asyncio.get_running_loop().run_in_executor(
                self._worker, self._run, audio, emotion,
            )
        except Exception:
            log.exception("face track failed")
            return web.json_response({"error": "face track failed"}, status=500)
        frames, data = to_track(weights)
        return web.json_response(
            {"fps": FPS, "shapes": self.engine.shapes, "frames": frames, "data": data, "ms": round(ms, 1)}
        )

    def _run(self, audio: np.ndarray, emotion: np.ndarray):
        started = time.perf_counter()
        weights = self.engine.run(audio, emotion)
        return weights, (time.perf_counter() - started) * 1000.0

    async def close(self, app: web.Application) -> None:
        await asyncio.to_thread(self._worker.shutdown, wait=True)


def create_app(service: FaceService) -> web.Application:
    app = web.Application(client_max_size=MAX_BODY)
    app.router.add_get("/health", service.health)
    app.router.add_post("/v1/face", service.face_track)
    app.on_cleanup.append(service.close)
    return app
