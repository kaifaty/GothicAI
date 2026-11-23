import asyncio
import base64
import struct
import threading

import numpy as np
import pytest
import pytest_asyncio
from aiohttp.test_utils import TestClient, TestServer

from gothic_embed import cli
from gothic_embed.encoder import encode
from gothic_embed.service import EmbedService, create_app


class FakeEncoder:
    prompts = {}

    def __init__(self):
        self.calls = []
        self.threads = []
        self.started = threading.Event()
        self.release = threading.Event()
        self.release.set()

    def get_sentence_embedding_dimension(self):
        return 1536

    def encode(self, texts, **kwargs):
        self.calls.append(("encode", texts, kwargs))
        self.threads.append(threading.get_ident())
        self.started.set()
        if not self.release.wait(5):
            raise TimeoutError("Fake encoder was not released")
        vectors = np.zeros((len(texts), 1536), dtype=np.float64)
        for i, row in enumerate(vectors):
            row[i:i + 2] = (3, 4) if i == 0 else (5, 12)
        return vectors

    def encode_query(self, texts, **kwargs):
        vectors = self.encode(texts, **kwargs)
        self.calls[-1] = ("encode_query", texts, kwargs)
        return vectors

    def encode_document(self, texts, **kwargs):
        vectors = self.encode(texts, **kwargs)
        self.calls[-1] = ("encode_document", texts, kwargs)
        return vectors


@pytest_asyncio.fixture
async def endpoint():
    encoder = FakeEncoder()
    service = EmbedService(loader=lambda *args: encoder)
    async with TestClient(TestServer(create_app(service))) as client:
        yield client, service, encoder


async def test_loading_health_and_warmup(endpoint):
    client, service, encoder = endpoint
    response = await client.get("/health")
    assert response.status == 503
    assert await response.json() == {"error": "loading"}
    response = await client.post("/embed", json={"texts": ["hello"], "kind": "query"})
    assert response.status == 503
    assert await response.json() == {"error": "loading"}

    encoder.release.clear()
    loading = asyncio.create_task(service.load())
    try:
        assert await asyncio.to_thread(encoder.started.wait, 2)
        response = await asyncio.wait_for(client.get("/health"), 1)
        assert response.status == 503  # Warmup has not finished; the event loop still responds.
    finally:
        encoder.release.set()
        await loading
    assert encoder.calls == [("encode", ["search_query: warmup"], {"normalize_embeddings": True})]
    response = await client.get("/health")
    assert response.status == 200
    assert await response.json() == {
        "model": "ai-forever/FRIDA", "dim": 1536, "dtype": "bfloat16", "device": "cuda",
    }


@pytest.mark.parametrize("kind,prefix", [("query", "search_query: "), ("document", "search_document: ")])
async def test_embed_wire_format(endpoint, kind, prefix):
    client, service, encoder = endpoint
    await service.load()
    response = await client.post("/embed", json={"texts": ["first", "second"], "kind": kind})
    assert response.status == 200
    body = await response.json()
    assert body.keys() == {"model", "dim", "vectors"}
    assert body["model"] == "ai-forever/FRIDA"
    assert body["dim"] == 1536
    wire = base64.b64decode(body["vectors"], validate=True)
    assert len(wire) == 2 * 1536 * 4
    assert struct.unpack_from("<ff", wire) == pytest.approx((0.6, 0.8))
    assert struct.unpack_from("<fff", wire, 1536 * 4) == pytest.approx((0, 5 / 13, 12 / 13))
    vectors = np.frombuffer(wire, dtype="<f4").reshape(2, 1536)
    np.testing.assert_allclose(np.linalg.norm(vectors, axis=1), 1, atol=1e-6)
    assert encoder.calls[-1] == ("encode", [prefix + "first", prefix + "second"], {
        "normalize_embeddings": True,
    })


@pytest.mark.parametrize("body,error", [
    (None, "Expected a JSON object"),
    ([], "Expected a JSON object"),
    ({}, "texts must be a non-empty list of strings"),
    ({"texts": []}, "texts must be a non-empty list of strings"),
    ({"texts": "hello"}, "texts must be a non-empty list of strings"),
    ({"texts": ["hello", 1]}, "texts must be a non-empty list of strings"),
    ({"texts": ["hello"] * 257}, "texts must contain at most 256 strings"),
    ({"texts": ["a" * 4001]}, "Each text must contain at most 4000 characters"),
    ({"texts": ["hello"]}, "kind must be query or document"),
    ({"texts": ["hello"], "kind": "other"}, "kind must be query or document"),
    ({"texts": ["hello"], "kind": []}, "kind must be query or document"),
])
async def test_invalid_input(endpoint, body, error):
    client, _, _ = endpoint
    response = await client.post("/embed", json=body) if body is not None else await client.post(
        "/embed", data="not json",
    )
    assert response.status == 400
    assert await response.json() == {"error": error}


async def test_input_limits_are_inclusive(endpoint):
    client, service, _ = endpoint
    await service.load()
    response = await client.post("/embed", json={"texts": ["я" * 4000] * 256, "kind": "document"})
    assert response.status == 200
    assert len(base64.b64decode((await response.json())["vectors"])) == 256 * 1536 * 4


@pytest.mark.parametrize("model,query,document", [
    ("ai-forever/FRIDA", "search_query: ", "search_document: "),
    ("deepvk/USER2-base", "search_query: ", "search_document: "),
    ("deepvk/USER2-small", "search_query: ", "search_document: "),
    ("intfloat/multilingual-e5-large", "query: ", "passage: "),
    ("intfloat/multilingual-e5-base", "query: ", "passage: "),
    ("intfloat/multilingual-e5-large-instruct",
     "Instruct: Given a question, retrieve conversation lines that answer it\nQuery: ", ""),
])
def test_prefixes(model, query, document):
    encoder = FakeEncoder()
    encoder.prompts = {"query": "model prompt"}  # Explicit prefixes take precedence.
    for kind, prefix in (("query", query), ("document", document)):
        encode(encoder, model, ["hello"], kind, 1536)
        assert encoder.calls[-1] == ("encode", [prefix + "hello"], {"normalize_embeddings": True})


@pytest.mark.parametrize("prompts", [{}, {"query": "model prompt"}])
def test_unknown_model_prompts(prompts):
    encoder = FakeEncoder()
    encoder.prompts = prompts
    for kind in ("query", "document"):
        encode(encoder, "other/model", ["hello"], kind, 1536)
        assert encoder.calls[-1] == (
            "encode_" + kind if prompts else "encode", ["hello"], {"normalize_embeddings": True},
        )


def test_frida_float16_rejected_by_cli(monkeypatch, capsys):
    monkeypatch.setenv("EMBED_MODEL", "ai-forever/FRIDA")
    monkeypatch.setenv("EMBED_DTYPE", "float16")
    monkeypatch.setattr("sys.argv", ["gothic-embed", "serve"])
    with pytest.raises(SystemExit) as exc:
        cli.main()
    assert exc.value.code == 2
    assert "FRIDA does not support float16 reliably; use bfloat16 or float32" in capsys.readouterr().err


async def test_concurrent_requests_are_serialized(endpoint):
    client, service, encoder = endpoint
    await service.load()
    encoder.started.clear()
    encoder.release.clear()
    first = asyncio.create_task(client.post("/embed", json={"texts": ["first"], "kind": "query"}))
    second = None
    try:
        assert await asyncio.to_thread(encoder.started.wait, 2)
        second = asyncio.create_task(client.post("/embed", json={"texts": ["second"], "kind": "document"}))
        response = await asyncio.wait_for(client.get("/health"), 1)
        assert response.status == 200
        assert not first.done() and not second.done()
        assert len(encoder.calls) == 2  # Warmup and the first request; the second cannot enter yet.
    finally:
        encoder.release.set()
        responses = await asyncio.gather(first, *([second] if second is not None else []))
    assert len(responses) == 2
    assert [r.status for r in responses] == [200, 200]
    bodies = await asyncio.gather(*(r.json() for r in responses))
    assert all(body["vectors"] for body in bodies)
    assert [c[1] for c in encoder.calls] == [
        ["search_query: warmup"], ["search_query: first"], ["search_document: second"],
    ]
    assert len(set(encoder.threads)) == 1
    assert encoder.threads[0] != threading.get_ident()
