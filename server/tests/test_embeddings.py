import base64
import json

import httpx
import numpy as np
import pytest

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
        assert (await client.health())["dim"] == 2
        query = await client.embed(["Где живёт маг?"], "query")
        documents = await client.embed(["Маг живёт в башне.", "В горах."], "document")
        assert query.dtype == np.float32 and query.shape == (1, 2)
        np.testing.assert_allclose(documents, [[0.6, 0.8], [0.6, 0.8]])
        assert json.loads(calls[1].content) == {"texts": ["Где живёт маг?"], "kind": "query"}
        assert calls[1].extensions["timeout"]["read"] == 2.0
        assert calls[2].extensions["timeout"]["read"] == 30.0
    finally:
        await client.close()


@pytest.mark.parametrize("failure", [
    "timeout", "network", "500", "json", "base64", "dimension", "length", "model-switch",
])
async def test_embed_client_rejects_service_errors_without_retry(failure) -> None:
    calls = 0

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
        return httpx.Response(200, json=data)

    client = EmbedClient("http://embed", transport=httpx.MockTransport(respond))
    try:
        await client.health()
        with pytest.raises(EmbedError):
            await client.embed(["text"], "query")
        assert calls == 2
    finally:
        await client.close()


@pytest.mark.parametrize("response", [
    httpx.Response(503),
    httpx.Response(200, json={"model": "test", "dim": "2", "dtype": "bfloat16", "device": "cuda"}),
])
async def test_embed_health_rejects_unavailable_or_broken_service(response) -> None:
    client = EmbedClient("http://embed", transport=httpx.MockTransport(lambda _: response))
    try:
        with pytest.raises(EmbedError):
            await client.health()
    finally:
        await client.close()
