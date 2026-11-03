"""HTTP contract with one worker for model download, loading, warmup and inference."""

import asyncio
import logging
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np
from aiohttp import web

from .model import DEFAULT_MODEL, MODEL_REVISION, WINDOW, load_session, score_window

log = logging.getLogger(__name__)


class LaughterService:
    def __init__(
        self, model: Path = DEFAULT_MODEL, device: str = "cuda", threads: int = 4, *, loader=load_session,
    ) -> None:
        if device not in ("cuda", "cpu"):
            raise ValueError("device must be cuda or cpu")
        if threads < 1:
            raise ValueError("threads must be positive")
        self.model, self.device, self.threads = Path(model).expanduser(), device, threads
        self.session = None
        self.provider: str | None = None
        self._loader = loader
        self._worker = ThreadPoolExecutor(max_workers=1, thread_name_prefix="gothic-laughter")

    async def load(self) -> None:
        self.session, self.provider = await asyncio.get_running_loop().run_in_executor(
            self._worker, self._load,
        )

    def _load(self):
        session = self._loader(self.model, self.device, self.threads)
        score_window(session, np.zeros(WINDOW, dtype=np.float32))
        return session, session.get_providers()[0]

    async def health(self, request: web.Request) -> web.Response:
        if self.session is None:
            return web.json_response({"error": "loading"}, status=503)
        return web.json_response({
            "model_revision": MODEL_REVISION, "device": self.device, "provider": self.provider,
        })

    async def window(self, request: web.Request) -> web.Response:
        try:
            body = await request.read()
        except web.HTTPRequestEntityTooLarge:
            return web.json_response({"error": "Expected at most 64000 float32 samples"}, status=400)
        if not body or len(body) % 4 or len(body) > WINDOW * 4:
            return web.json_response({"error": "Expected 1 to 64000 float32 samples"}, status=400)
        audio = np.frombuffer(body, dtype="<f4")
        if not np.isfinite(audio).all():
            return web.json_response({"error": "Samples must be finite numbers"}, status=400)
        if self.session is None:
            return web.json_response({"error": "loading"}, status=503)
        try:
            probability = await asyncio.get_running_loop().run_in_executor(
                self._worker, score_window, self.session, audio,
            )
        except Exception:
            log.exception("Laughter inference failed")
            return web.json_response({"error": "Laughter inference failed"}, status=500)
        return web.json_response({"laughing": probability})

    async def close(self, app: web.Application) -> None:
        await asyncio.to_thread(self._worker.shutdown, wait=True)


def create_app(service: LaughterService) -> web.Application:
    app = web.Application(client_max_size=WINDOW * 4 + 1)
    app.router.add_get("/health", service.health)
    app.router.add_post("/window", service.window)
    app.on_cleanup.append(service.close)
    return app
