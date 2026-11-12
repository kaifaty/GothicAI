import asyncio
import base64
import threading

import numpy as np
import pytest
import pytest_asyncio
from aiohttp.test_utils import TestClient, TestServer

from gothic_a2f.service import FaceService, create_app
from gothic_a2f.track import BadRequest, parse_emotion

EMOTIONS = [
    "amazement", "anger", "cheekiness", "disgust", "fear", "grief", "joy", "outofbreath", "pain", "sadness",
]
SHAPES = [f"shape{i}" for i in range(52)]


class FakeEngine:
    """60 fps frames like the network: frame i of a line has every weight i / 1000, capped at 1."""

    emotions, shapes = EMOTIONS, SHAPES

    def __init__(self):
        self.runs, self.threads = [], set()
        self.active = 0
        self.overlap = False
        self.lock = threading.Lock()

    def run(self, audio, emotion):
        with self.lock:
            self.active += 1
            self.overlap |= self.active > 1
        self.threads.add(threading.get_ident())
        self.runs.append((len(audio), emotion.copy()))
        threading.Event().wait(0.05)
        frames = int(np.ceil(len(audio) * 60 / 16000))
        with self.lock:
            self.active -= 1
        return np.repeat(np.minimum(np.arange(frames) / 1000, 1.0)[:, None], len(self.shapes), axis=1)


@pytest_asyncio.fixture
async def client():
    engine = FakeEngine()
    service = FaceService(lambda: engine)
    async with TestClient(TestServer(create_app(service))) as client:
        yield client, service, engine


def pcm(seconds, rate):
    return (np.sin(np.arange(int(seconds * rate)) / 7) * 8000).astype("<i2").tobytes()


async def test_health_loading_then_ready(client):
    client, service, engine = client
    assert (await client.get("/health")).status == 503
    await service.load()
    assert engine.runs[0][0] == 16000  # warmup on a second of silence
    health = await (await client.get("/health")).json()
    assert health == {"ok": True, "model": "v3.0", "face": "Mark", "fps": 30}


async def test_track_every_second_frame_as_bytes(client):
    client, service, engine = client
    await service.load()
    response = await client.post("/v1/face?rate=24000&emotion=anger:0.8,disgust:0.3", data=pcm(3, 24000))
    assert response.status == 200
    body = await response.json()
    assert body["fps"] == 30 and body["shapes"] == SHAPES and body["frames"] == 90 and body["ms"] >= 0
    data = np.frombuffer(base64.b64decode(body["data"]), dtype=np.uint8).reshape(90, 52)
    # frame k of the track is frame 2k of the network: weight 2k / 1000 -> round(2k * 0.255)
    assert data[:, 0].tolist() == [round(2 * k * 0.255) for k in range(90)]
    length, emotion = engine.runs[-1]
    assert length == 48000  # resampled to 16 kHz
    assert emotion[1] == pytest.approx(0.8) and emotion[3] == pytest.approx(0.3)
    assert emotion.sum() == pytest.approx(1.1)


@pytest.mark.parametrize("query, body, status", [
    ("rate=16000", b"", 400),
    ("", b"\0\0", 400),
    ("rate=7999", b"\0\0", 400),
    ("rate=48001", b"\0\0", 400),
    ("rate=16k", b"\0\0", 400),
    ("rate=16000", b"\0\0\0", 400),
    ("rate=16000&emotion=rage:0.5", b"\0\0", 400),
    ("rate=16000&emotion=anger:1.5", b"\0\0", 400),
    ("rate=16000&emotion=anger:-0.1", b"\0\0", 400),
    ("rate=16000&emotion=anger", b"\0\0", 400),
    ("rate=8000", b"\0\0" * (8000 * 30 + 1), 413),
    ("rate=48000", b"\0\0" * (48000 * 30 + 1), 413),
])
async def test_rejects(client, query, body, status):
    client, service, engine = client
    await service.load()
    response = await client.post(f"/v1/face?{query}", data=body)
    assert response.status == status
    assert (await response.json())["error"]
    assert len(engine.runs) == 1  # only the warmup


async def test_two_requests_queue_on_one_worker(client):
    client, service, engine = client
    await service.load()
    line = pcm(1, 16000)
    responses = await asyncio.gather(*(client.post("/v1/face?rate=16000", data=line) for _ in range(2)))
    assert [r.status for r in responses] == [200, 200]
    assert not engine.overlap and len(engine.threads) == 1


def test_neutral_and_repeated_emotion():
    assert not parse_emotion("", EMOTIONS).any() and not parse_emotion(None, EMOTIONS).any()
    with pytest.raises(BadRequest):
        parse_emotion("joy:0.5,joy:0.2", EMOTIONS)
