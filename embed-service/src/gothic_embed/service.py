"""HTTP contract and a single worker for model loading, warmup and encoding."""

import asyncio
import base64
import threading
from concurrent.futures import ThreadPoolExecutor

from aiohttp import web

from .encoder import encode, load_encoder, validate_settings


class EmbedService:
    def __init__(
        self, model: str = "ai-forever/FRIDA", dtype: str = "bfloat16", device: str = "cuda",
        *, loader=load_encoder,
    ) -> None:
        validate_settings(model, dtype)
        self.model, self.dtype, self.device = model, dtype, device
        self.dim: int | None = None
        self.encoder = None
        self._loader = loader
        self._worker = ThreadPoolExecutor(max_workers=1, thread_name_prefix="gothic-embed")
        self._lock = threading.Lock()

    async def load(self) -> None:
        self.encoder, self.dim = await asyncio.get_running_loop().run_in_executor(self._worker, self._load)

    def _load(self):
        with self._lock:
            encoder = self._loader(self.model, self.dtype, self.device)
            dim = encoder.get_sentence_embedding_dimension()
            if not isinstance(dim, int) or dim <= 0:
                raise ValueError("Encoder must report a positive embedding dimension")
            encode(encoder, self.model, ["warmup"], "query", dim)
            return encoder, dim

    async def health(self, request: web.Request) -> web.Response:
        if self.encoder is None:
            return web.json_response({"error": "loading"}, status=503)
        return web.json_response({
            "model": self.model, "dim": self.dim, "dtype": self.dtype, "device": self.device,
        })

    async def embed(self, request: web.Request) -> web.Response:
        try:
            body = await request.json()
        except (ValueError, UnicodeDecodeError, web.HTTPRequestEntityTooLarge):
            return web.json_response({"error": "Expected a JSON object"}, status=400)
        if not isinstance(body, dict):
            return web.json_response({"error": "Expected a JSON object"}, status=400)
        texts, kind = body.get("texts"), body.get("kind")
        if not isinstance(texts, list) or not texts or not all(isinstance(text, str) for text in texts):
            return web.json_response({"error": "texts must be a non-empty list of strings"}, status=400)
        if len(texts) > 256:
            return web.json_response({"error": "texts must contain at most 256 strings"}, status=400)
        if any(len(text) > 4000 for text in texts):
            return web.json_response({"error": "Each text must contain at most 4000 characters"}, status=400)
        if kind not in ("query", "document"):
            return web.json_response({"error": "kind must be query or document"}, status=400)
        if self.encoder is None:
            return web.json_response({"error": "loading"}, status=503)
        vectors = await asyncio.get_running_loop().run_in_executor(self._worker, self._embed, texts, kind)
        return web.json_response({"model": self.model, "dim": self.dim, "vectors": vectors})

    def _embed(self, texts: list[str], kind: str) -> str:
        with self._lock:
            vectors = encode(self.encoder, self.model, texts, kind, self.dim)
            return base64.b64encode(vectors.tobytes(order="C")).decode("ascii")

    async def close(self, app: web.Application) -> None:
        await asyncio.to_thread(self._worker.shutdown, wait=True)


def create_app(service: EmbedService) -> web.Application:
    # A non-BMP character can use twelve bytes as an escaped UTF-16 surrogate pair in JSON.
    app = web.Application(client_max_size=256 * 4000 * 12 + 65536)
    app.router.add_get("/health", service.health)
    app.router.add_post("/embed", service.embed)
    app.on_cleanup.append(service.close)
    return app
