"""The remote embedding service; failures always leave the caller a lexical fallback."""

from __future__ import annotations

import asyncio
import base64
from typing import Literal

import httpx
import numpy as np
from numpy.typing import NDArray


class EmbedError(Exception):
    """The embedding service did not return usable vectors."""


class EmbedClient:
    def __init__(self, url: str, *, transport: httpx.AsyncBaseTransport | None = None) -> None:
        self._http = httpx.AsyncClient(base_url=url.rstrip("/") + "/", transport=transport)
        self.model: str | None = None
        self.dim: int | None = None

    async def close(self) -> None:
        await self._http.aclose()

    async def health(self) -> dict:
        data = await self._request("GET", "health", timeout=2.0)
        model, dim = self._identity(data)
        if not all(isinstance(data.get(key), str) and data[key] for key in ("dtype", "device")):
            raise EmbedError("Invalid embedding health response")
        self.model, self.dim = model, dim
        return data

    async def embed(self, texts: list[str], kind: Literal["query", "document"]) -> NDArray[np.float32]:
        if kind not in ("query", "document"):
            raise EmbedError("Invalid embedding kind")
        if len(texts) > 256 or any(not isinstance(text, str) or len(text) > 4000 for text in texts):
            raise EmbedError("Invalid embedding texts")
        expected = (self.model, self.dim)
        data = await self._request(
            "POST", "embed", timeout=2.0 if kind == "query" else 30.0,
            json={"texts": texts, "kind": kind},
        )
        model, dim = self._identity(data)
        if expected[0] is not None and (model, dim) != expected:
            raise EmbedError("Embedding model or dimension changed during the request")
        try:
            encoded = data["vectors"]
            if not isinstance(encoded, str):
                raise ValueError("vectors must be base64 text")
            raw = base64.b64decode(encoded, validate=True)
            if len(raw) != len(texts) * dim * 4:
                raise ValueError("vector byte length does not match the dimensions")
            vectors = np.frombuffer(raw, dtype="<f4").reshape(len(texts), dim).astype(np.float32)
            if not np.isfinite(vectors).all():
                raise ValueError("vectors contain non-finite values")
        except (KeyError, TypeError, ValueError) as exc:
            raise EmbedError(f"Invalid embedding vectors: {exc}") from exc
        self.model, self.dim = model, dim
        return vectors

    async def _request(self, method: str, path: str, *, timeout: float, **kwargs) -> dict:
        try:
            async with asyncio.timeout(timeout):
                response = await self._http.request(method, path, timeout=timeout, **kwargs)
            if response.status_code != 200:
                raise EmbedError(f"Embedding {path} returned HTTP {response.status_code}")
            data = response.json()
            if not isinstance(data, dict):
                raise ValueError("expected a JSON object")
            return data
        except (httpx.HTTPError, TimeoutError, TypeError, ValueError) as exc:
            raise EmbedError(f"Embedding {path} failed: {exc}") from exc

    @staticmethod
    def _identity(data: dict) -> tuple[str, int]:
        model, dim = data.get("model"), data.get("dim")
        if not isinstance(model, str) or not model or type(dim) is not int or dim <= 0:
            raise EmbedError("Invalid embedding model or dimension")
        return model, dim
