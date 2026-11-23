import base64
import json
from types import SimpleNamespace

import httpx
import numpy as np
import pytest

from gothic_ai import embeddings
from gothic_ai.embeddings import EmbedClient, EmbedError


async def test_embed_client_decodes_vectors_and_sends_bare_text() -> None:
    calls = []

    def respond(request: httpx.Request) -> httpx.Response:
        calls.append(request)
        if request.url.path == "/health":
            return httpx.Response(200, json={
                "model": "test", "dim": 2, "dtype": "bfloat16", "device": "cuda",
            })
        body = json.loads(request.content)
        vectors = np.array([[0.6, 0.8]] * len(body["texts"]), dtype="<f4")
        return httpx.Response(200, json={
            "model": "test", "dim": 2, "vectors": base64.b64encode(vectors.tobytes()).decode(),
        })

    client = EmbedClient("http://embed", transport=httpx.MockTransport(respond))
    try:
        for texts, kind in [([None], "query"), (["text"] * 257, "document"), (["text"], "invalid")]:
            with pytest.raises(EmbedError):
                await client.embed(texts, kind)
        assert not calls  # local input errors do not contact the service or enter backoff
        assert (await client.health())["dim"] == 2
        query = await client.embed(["Где живёт маг?"], "query")
        texts = ["Маг живёт в башне." + "а" * 4000, "В горах."]
        documents = await client.embed(texts, "document")
        assert query.dtype == np.float32 and query.shape == (1, 2)
        np.testing.assert_allclose(documents, [[0.6, 0.8], [0.6, 0.8]])
        assert json.loads(calls[1].content) == {"texts": ["Где живёт маг?"], "kind": "query"}
        assert json.loads(calls[2].content)["texts"] == [texts[0][:4000], texts[1]]
        assert len(texts[0]) > 4000  # truncation does not mutate the text used for the memory hash
        assert calls[1].extensions["timeout"]["read"] == 2.0
        assert calls[2].extensions["timeout"]["read"] == 30.0
    finally:
        await client.close()


@pytest.mark.parametrize("failure", [
    "timeout", "network", "500", "json", "base64", "dimension", "length", "model-switch",
    "concurrent-reset",
])
async def test_embed_client_rejects_service_errors_without_retry(failure, monkeypatch) -> None:
    calls = 0
    now = 100.0
    monkeypatch.setattr(embeddings, "time", SimpleNamespace(monotonic=lambda: now))

    def respond(request: httpx.Request) -> httpx.Response:
        nonlocal calls
        calls += 1
        if request.url.path == "/health":
            return httpx.Response(200, json={
                "model": "test", "dim": 2, "dtype": "bfloat16", "device": "cuda",
            })
        if failure == "timeout":
            raise httpx.ReadTimeout("late", request=request)
        if failure == "network":
            raise httpx.ConnectError("offline", request=request)
        if failure == "500":
            return httpx.Response(500)
        if failure == "json":
            return httpx.Response(200, content=b"not JSON")
        data = {"model": "test", "dim": 2, "vectors": base64.b64encode(b"\0" * 8).decode()}
        if failure == "base64":
            data["vectors"] = "???"
        elif failure == "dimension":
            data["dim"] = 3
        elif failure == "length":
            data["vectors"] = base64.b64encode(b"\0" * 4).decode()
        elif failure == "model-switch":
            # A concurrent health call must not change this request's expected cache identity.
            client.model = "replacement"
            data["model"] = "replacement"
        elif failure == "concurrent-reset":
            # A late response must not restore metadata invalidated by another request.
            client.model, client.dim = None, None
        return httpx.Response(200, json=data)

    client = EmbedClient("http://embed", transport=httpx.MockTransport(respond))
    try:
        await client.health()
        with pytest.raises(EmbedError):
            await client.embed(["text"], "query")
        assert calls == 2
        if failure in ("dimension", "model-switch", "concurrent-reset"):
            assert client.model is None and client.dim is None
        now = 159.0
        with pytest.raises(EmbedError, match="^embeddings backoff$"):
            await client.health()
        with pytest.raises(EmbedError, match="^embeddings backoff$"):
            await client.embed(["text"], "document")
        assert calls == 2
        now = 160.0
        assert (await client.health())["model"] == "test"
        assert calls == 3  # blocked callers did not extend the shared deadline
    finally:
        await client.close()


@pytest.mark.parametrize("response", [
    httpx.Response(503),
    httpx.Response(200, json={"model": "test", "dim": "2", "dtype": "bfloat16", "device": "cuda"}),
])
async def test_embed_health_rejects_unavailable_or_broken_service(response) -> None:
    calls = 0

    def respond(_request):
        nonlocal calls
        calls += 1
        return response

    client = EmbedClient("http://embed", transport=httpx.MockTransport(respond))
    try:
        with pytest.raises(EmbedError):
            await client.health()
        with pytest.raises(EmbedError, match="^embeddings backoff$"):
            await client.embed(["text"], "query")
        assert calls == 1
    finally:
        await client.close()
