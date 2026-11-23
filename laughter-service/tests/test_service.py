import asyncio
import hashlib
import sys
import threading
from pathlib import Path
from types import SimpleNamespace

import httpx
import numpy as np
import pytest
import pytest_asyncio
from aiohttp.test_utils import TestClient, TestServer

from gothic_laughter import model
from gothic_laughter.model import DEFAULT_MODEL, MODEL_REVISION, WINDOW, load_session, score_window
from gothic_laughter.service import LaughterService, create_app


class FakeSession:
    def __init__(self):
        self.calls = []
        self.threads = []
        self.started = threading.Event()
        self.release = threading.Event()
        self.release.set()
        self.result = 0.75
        self.error = None

    def get_providers(self):
        return ["CPUExecutionProvider"]

    def run(self, outputs, inputs):
        self.calls.append((outputs, inputs))
        self.threads.append(threading.get_ident())
        self.started.set()
        if not self.release.wait(5):
            raise TimeoutError("Fake session was not released")
        if self.error is not None:
            raise self.error
        return [np.array([[self.result]], dtype=np.float32)]


@pytest_asyncio.fixture
async def endpoint():
    session = FakeSession()
    load_threads = []

    def loader(*args):
        load_threads.append(threading.get_ident())
        return session

    service = LaughterService(loader=loader)
    async with TestClient(TestServer(create_app(service))) as client:
        yield client, service, session, load_threads


async def test_loading_health_and_warmup(endpoint):
    client, service, session, load_threads = endpoint
    response = await client.get("/health")
    assert response.status == 503
    assert await response.json() == {"error": "loading"}
    response = await client.post("/window", data=np.zeros(16000, dtype="<f4").tobytes())
    assert response.status == 503
    assert await response.json() == {"error": "loading"}

    session.release.clear()
    loading = asyncio.create_task(service.load())
    try:
        assert await asyncio.to_thread(session.started.wait, 2)
        response = await asyncio.wait_for(client.get("/health"), 1)
        assert response.status == 503
    finally:
        session.release.set()
        await loading
    response = await client.get("/health")
    assert response.status == 200
    assert await response.json() == {
        "model_revision": MODEL_REVISION, "device": "cuda", "provider": "CPUExecutionProvider",
    }
    assert len(session.calls) == 1
    outputs, inputs = session.calls[0]
    assert outputs == ["clip_prob"]
    assert inputs["waveform"].shape == (1, WINDOW)
    assert not inputs["waveform"].any()
    np.testing.assert_array_equal(inputs["n_valid"], [WINDOW])
    assert load_threads == session.threads
    assert load_threads[0] != threading.get_ident()


@pytest.mark.parametrize("samples", [1, 16000, WINDOW])
async def test_window_score_padding_and_valid_length(endpoint, samples):
    client, service, session, load_threads = endpoint
    await service.load()
    audio = np.linspace(-0.5, 0.5, samples, dtype="<f4")
    response = await client.post("/window", data=audio.tobytes())
    assert response.status == 200
    assert await response.json() == {"laughing": 0.75}
    outputs, inputs = session.calls[-1]
    assert outputs == ["clip_prob"]
    waveform = inputs["waveform"]
    assert waveform.shape == (1, WINDOW)
    assert waveform.dtype == np.float32
    np.testing.assert_array_equal(waveform[0, :samples], audio)
    assert not waveform[0, samples:].any()
    np.testing.assert_array_equal(inputs["n_valid"], [samples])
    assert inputs["n_valid"].dtype == np.int64
    assert set(session.threads) == set(load_threads)


@pytest.mark.parametrize("body", [
    b"",
    b"123",
    b"\0" * ((WINDOW + 1) * 4),
    np.array([float("nan")], dtype="<f4").tobytes(),
    np.array([float("inf")], dtype="<f4").tobytes(),
])
async def test_invalid_body(endpoint, body):
    client, _, _, _ = endpoint
    response = await client.post("/window", data=body)
    assert response.status == 400
    assert isinstance((await response.json())["error"], str)


@pytest.mark.parametrize("failure", [RuntimeError("model failed"), float("nan")])
async def test_model_failure_keeps_service_alive(endpoint, failure):
    client, service, session, _ = endpoint
    await service.load()
    if isinstance(failure, Exception):
        session.error = failure
    else:
        session.result = failure
    audio = np.zeros(16000, dtype="<f4").tobytes()
    response = await client.post("/window", data=audio)
    assert response.status == 500
    assert await response.json() == {"error": "Laughter inference failed"}
    session.error, session.result = None, 0.75
    response = await client.post("/window", data=audio)
    assert response.status == 200
    assert await response.json() == {"laughing": 0.75}


@pytest.mark.parametrize("device", ["cpu", "cuda"])
def test_onnx_provider_options(monkeypatch, tmp_path, device):
    seen = []

    def session(path, *, sess_options, providers):
        seen.append((path, sess_options, providers))
        return FakeSession()

    ort = SimpleNamespace(
        SessionOptions=SimpleNamespace,
        preload_dlls=lambda: seen.append("preload"),
        InferenceSession=session,
    )
    monkeypatch.setitem(sys.modules, "onnxruntime", ort)
    path = tmp_path / "model.onnx"
    path.touch()
    load_session(path, device, 4)
    actual_path, options, providers = seen[-1]
    assert actual_path == str(path)
    if device == "cuda":
        assert seen[0] == "preload"
        assert providers == [("CUDAExecutionProvider", {"arena_extend_strategy": "kSameAsRequested"})]
    else:
        assert len(seen) == 1
        assert options.intra_op_num_threads == 4
        assert providers == ["CPUExecutionProvider"]


@pytest.mark.parametrize("corrupt", [False, True])
def test_model_download_is_verified_before_publish(monkeypatch, tmp_path, corrupt):
    body = b"fake ONNX model"
    payload = bytes([body[0] ^ 1]) + body[1:] if corrupt else body
    monkeypatch.setattr(model, "MODEL_BYTES", len(body))
    monkeypatch.setattr(model, "MODEL_SHA256", hashlib.sha256(body).hexdigest())
    path = tmp_path / "laughter" / "model.onnx"
    partial = path.with_name("model.onnx.part")
    requests = []
    published = []

    def response(request):
        requests.append(str(request.url))
        assert not path.exists()
        return httpx.Response(200, content=payload)

    original_client = httpx.Client
    monkeypatch.setattr(
        model.httpx, "Client",
        lambda **kwargs: original_client(transport=httpx.MockTransport(response), **kwargs),
    )
    original_replace = Path.replace

    def publish(source, target):
        assert source == partial and target == path
        assert not path.exists() and source.read_bytes() == body
        published.append(source)
        return original_replace(source, target)

    monkeypatch.setattr(Path, "replace", publish)
    if corrupt:
        with pytest.raises(ValueError, match="verification failed"):
            model.fetch_model(path)
        assert not path.exists() and published == []
    else:
        assert model.fetch_model(path) == path
        assert path.read_bytes() == body and published == [partial]
        assert model.fetch_model(path) == path
    assert not partial.exists()
    assert requests == [model.MODEL_URL]


def test_real_model_if_available(monkeypatch):
    if not DEFAULT_MODEL.is_file():
        pytest.skip(f"Laughter model is absent: {DEFAULT_MODEL}")
    pytest.importorskip("onnxruntime")

    def unexpected_download(*args, **kwargs):
        pytest.fail("Installed model test must not download weights")

    monkeypatch.setattr(model.httpx, "Client", unexpected_download)
    session = load_session(DEFAULT_MODEL, "cpu", 4)
    probability = score_window(session, np.zeros(16000, dtype=np.float32))
    assert 0 <= probability <= 1
