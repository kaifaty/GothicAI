import asyncio
import base64
import json
import logging
import time
from contextlib import asynccontextmanager

import httpx
import pytest

from gothic_ai.dialogue import DialogueManager
from gothic_ai.face_track import (
    ARKIT_SHAPES,
    RETRY_S,
    WARN_EVERY_S,
    A2fClient,
    FaceVoice,
    FakeFace,
    emotion_table,
    make_source,
)
from gothic_ai.llm.fake import FakeBackend
from gothic_ai.persona import PersonaProvider
from gothic_ai.tts import TtsClient
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import SCENE, XARDAS, _until, scene_tts
from tests.test_tts import fake_tts

VOICE_FLOW = ("say", "say_face", "say_audio", "say_end")


def _flow(fake: FakeGame) -> list[dict]:
    return [m for m in fake.received if m["type"] in VOICE_FLOW]


async def _answer(face: FaceVoice | None, reply: str, lines: int, **tts) -> FakeGame:
    fake = FakeGame()
    async with fake_tts(**tts) as (url, _requests), connected(fake) as link:
        dialogue = DialogueManager(
            link, FakeBackend(reply, delay=0), PersonaProvider(None), tts=TtsClient(url), face=face
        )
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Привет")
        await _until(lambda: len(fake.say_ends) == lines)
    return fake


class A2fStub:
    """gothic-a2f stand-in: answers with a track of `frames` frames, or as `reply` says."""

    def __init__(self, reply=None, *, delay: float = 0.0) -> None:
        self.reply, self.delay = reply, delay
        self.requests: list[httpx.Request] = []

    async def __call__(self, request: httpx.Request) -> httpx.Response:
        self.requests.append(request)
        await asyncio.sleep(self.delay)
        if self.reply is not None:
            return self.reply(request)
        frames = 3
        data = base64.b64encode(bytes(range(frames * len(ARKIT_SHAPES)))).decode()
        body = {"fps": 30, "shapes": ARKIT_SHAPES, "frames": frames, "data": data, "ms": 5}
        return httpx.Response(200, json=body)

    def client(self, timeout: float = 1.5, **kw) -> A2fClient:
        return A2fClient("http://a2f:47839", timeout, transport=httpx.MockTransport(self), **kw)


async def test_a_line_goes_say_then_its_face_then_its_audio_with_word_timings():
    fake = await _answer(
        FaceVoice(FakeFace()), "[friendly] Ты вернулся ко мне. [angry] Не трать моё время.", 2,
        words=True,
    )

    flow = _flow(fake)
    line_flow = ["say", "say_face", "say_audio", "say_audio", "say_audio", "say_end"]
    assert [m["type"] for m in flow] == line_flow * 2
    for say, face, *chunks, end in (flow[:6], flow[6:]):
        assert say["line"] == face["line"] == end["line"] == chunks[0]["line"]
        # frames x shapes bytes: one frame per 1/30 s of the line's voice (16 kHz, 800 samples a character)
        weights = base64.b64decode(face["data"])
        frames = -(-800 * len(say["text"]) * 30 // 16000)
        assert (face["fps"], face["shapes"], len(weights)) == (30, ARKIT_SHAPES, frames * 52)
        jaw = ARKIT_SHAPES.index("jawOpen")
        assert all(weights[i * 52 + jaw] == 8 for i in range(frames - 1))  # loudness 256 of 8000
        assert sum(weights) == sum(weights[i * 52 + jaw] for i in range(frames))
        # the word timings come as they did without a face: late ones in the following chunk, the tail empty
        count = len(say["text"].split())
        assert "words" not in chunks[0] and chunks[2]["data"] == ""
        assert chunks[1]["words"] == [[i, i / 10, (i + 1) / 10] for i in range(count // 2)]
        assert chunks[2]["words"] == [[i, i / 10, (i + 1) / 10] for i in range(count // 2, count)]
    assert [m["emotion"] for m in flow if m["type"] == "say_face"] == ["joy:0.6", "anger:0.8"]


async def test_without_a_face_source_lines_stream_as_before():
    fake = await _answer(None, "Ты вернулся.", 1)
    assert [m["type"] for m in _flow(fake)] == ["say", "say_audio", "say_end"]
    assert make_source("off", "http://a2f:47839") is None and make_source("service", None) is None


async def test_the_service_gets_the_lines_voice_and_its_mood_as_an_emotion():
    stub = A2fStub()
    face = FaceVoice(stub.client(), emotion_table(["afraid=fear:0.5", "hostile="]))
    fake = await _answer(face, "[afraid] Уходи. [hostile] Прочь! [neutral] Ну.", 3)

    assert [m["type"] for m in _flow(fake)] == ["say", "say_face", "say_audio", "say_end"] * 3
    assert [dict(r.url.params) for r in stub.requests] == [
        {"rate": "16000", "emotion": "fear:0.5"}, {"rate": "16000"}, {"rate": "16000"},
    ]
    says = [m for m in fake.received if m["type"] == "say"]
    assert [len(r.content) for r in stub.requests] == [2 * 800 * len(m["text"]) for m in says]
    faces = [m for m in fake.received if m["type"] == "say_face"]
    assert [f.get("emotion") for f in faces] == ["fear:0.5", None, None]
    assert all(len(base64.b64decode(f["data"])) == 3 * 52 for f in faces)


def _refused(_request: httpx.Request) -> httpx.Response:
    raise httpx.ConnectError("refused")


@pytest.mark.parametrize(
    "stub",
    [
        A2fStub(_refused),
        A2fStub(delay=5.0),
        A2fStub(lambda r: httpx.Response(413, text="longer than 30 s")),
        A2fStub(
            lambda r: httpx.Response(200, json={"fps": 30, "shapes": ["jawOpen"], "frames": 2, "data": ""})
        ),
    ],
    ids=["unavailable", "too-slow", "refused", "bad-track"],
)
async def test_a_line_whose_face_fails_goes_without_it_and_does_not_wait(stub):
    started = time.monotonic()
    fake = await _answer(FaceVoice(stub.client(timeout=0.3)), "Ты вернулся.", 1)

    assert [m["type"] for m in _flow(fake)] == ["say", "say_audio", "say_end"]
    assert time.monotonic() - started < 3.0  # the slow service would answer after 5 s
    assert len(stub.requests) == 1


@pytest.mark.parametrize(
    "stub",
    [A2fStub(_refused), A2fStub(delay=1.0), A2fStub(lambda r: httpx.Response(503, text="busy"))],
    ids=["unavailable", "too-slow", "5xx"],
)
async def test_after_the_service_is_down_lines_do_not_ask_it_for_a_while(stub):
    clock = [100.0]
    face = FaceVoice(stub.client(timeout=0.05, clock=lambda: clock[0]))
    pcm = b"\0\1" * 1600

    assert await face.say_face(XARDAS, "AIB_1", pcm, 16000, None) is None
    assert await face.say_face(XARDAS, "AIB_2", pcm, 16000, None) is None
    assert len(stub.requests) == 1
    clock[0] += RETRY_S - 0.1
    assert await face.say_face(XARDAS, "AIB_2", pcm, 16000, None) is None
    assert len(stub.requests) == 1
    clock[0] += 0.1
    stub.reply, stub.delay = None, 0.0
    assert (await face.say_face(XARDAS, "AIB_3", pcm, 16000, None)) is not None
    assert len(stub.requests) == 2
    # longer than the service takes: not asked at all
    assert await face.say_face(XARDAS, "AIB_4", b"\0\0" * 16000 * 31, 16000, None) is None
    assert len(stub.requests) == 2


@asynccontextmanager
async def slow_a2f(delay: float):
    """A real HTTP stand-in for gothic-a2f that answers each line after `delay` s: httpx's own time-outs
    apply to it, unlike a mock transport."""
    asked: list[int] = []

    async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        head = await reader.readuntil(b"\r\n\r\n")
        lines = head.lower().split(b"\r\n")
        size = int(next(line.split(b":")[1] for line in lines if line.startswith(b"content-length")))
        asked.append(len(await reader.readexactly(size)))
        try:
            await asyncio.sleep(delay)
            data = base64.b64encode(bytes(3 * len(ARKIT_SHAPES))).decode()
            body = json.dumps({"fps": 30, "shapes": ARKIT_SHAPES, "frames": 3, "data": data}).encode()
            writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                         + str(len(body)).encode() + b"\r\nConnection: close\r\n\r\n" + body)
            await writer.drain()
        except (ConnectionError, asyncio.CancelledError):
            pass
        finally:
            writer.close()

    server = await asyncio.start_server(handle, "127.0.0.1", 0)
    async with server:
        yield f"http://127.0.0.1:{server.sockets[0].getsockname()[1]}", asked


async def test_a_long_line_waits_for_its_track_as_long_as_its_voice_needs():
    # 1.0 s + 0.2 s a second of voice: 20 s wait 5 s, 1 s waits 1.2 s; the service answers in 2.5 s
    clock = [100.0]
    async with slow_a2f(2.5) as (url, asked):
        client = A2fClient(url, clock=lambda: clock[0])
        face = FaceVoice(client)
        try:
            long = await face.say_face(XARDAS, "AIB_1", b"\0\1" * 16000 * 20, 16000, None)
            assert long is not None and len(asked) == 1
            short = await face.say_face(XARDAS, "AIB_2", b"\0\1" * 16000, 16000, None)
            assert short is None and len(asked) == 2  # not answered in its limit: the service is down
            assert await face.say_face(XARDAS, "AIB_3", b"\0\1" * 16000 * 20, 16000, None) is None
            assert len(asked) == 2  # paused
        finally:
            await client.aclose()


async def test_a_failing_service_is_warned_about_once_a_minute(caplog):
    clock = [100.0]
    stub = A2fStub(lambda r: httpx.Response(413, text="longer than 30 s"))  # no pause: asked every line
    face = FaceVoice(stub.client(), clock=lambda: clock[0])
    pcm = b"\0\1" * 1600

    def warnings() -> int:
        return sum(r.levelname == "WARNING" and "no face track" in r.message for r in caplog.records)

    with caplog.at_level(logging.DEBUG, logger="gothic_ai.face_track"):
        await face.say_face(XARDAS, "AIB_1", pcm, 16000, None)
        clock[0] += WARN_EVERY_S - 1
        await face.say_face(XARDAS, "AIB_2", pcm, 16000, None)
        assert (len(stub.requests), warnings()) == (2, 1)
        clock[0] += 1
        await face.say_face(XARDAS, "AIB_3", pcm, 16000, None)
        assert (len(stub.requests), warnings()) == (3, 2)


async def test_first_reactions_and_the_originals_lines_do_not_wait_for_a_face():
    fake = FakeGame()
    async with fake_tts() as (url, _requests), connected(fake) as link:
        face = FaceVoice(FakeFace())
        dialogue = DialogueManager(
            link, FakeBackend("", delay=0), PersonaProvider(None), tts=TtsClient(url), face=face
        )
        conv = dialogue.open(XARDAS, "Ксардас")
        await dialogue._say(conv, "Хм...", None, filler=True)
        await dialogue.voice_story_line(XARDAS, "Ксардас", "DIA_XARDAS_HELLO_14_01", "Ты пришёл.")
        await dialogue.voice_story_line(XARDAS, "Ксардас", "AIB_G1", "Выбирай.")
        await _until(lambda: len(fake.say_ends) == 3)

    faces = [m["line"] for m in fake.received if m["type"] == "say_face"]
    assert faces == ["AIB_G1"]  # the director's own line has one, the filler and the original's do not
    flow = [(m["type"], m["line"]) for m in _flow(fake) if m["line"] == "AIB_G1"]
    assert [t for t, _ in flow] == ["say", "say_face", "say_audio", "say_end"]


async def test_a_scene_line_carries_its_face_after_its_say():
    fake = FakeGame()
    async with scene_tts(words=True) as (url, _requests), connected(fake) as link:
        dialogue = DialogueManager(
            link, FakeBackend(), PersonaProvider(None), tts=TtsClient(url), face=FaceVoice(FakeFace())
        )
        dialogue.scene_pace = lambda text: 0.05
        play = dialogue.play_scene(SCENE)
        await asyncio.wait_for(play.task, 10)

    flow = [m["type"] for m in _flow(fake)]
    assert flow == ["say", "say_face", "say_audio", "say_audio", "say_audio", "say_end"] * 3
    faces = [m for m in fake.received if m["type"] == "say_face"]
    assert [f.get("emotion") for f in faces] == ["anger:0.8", None, None]


def test_emotion_labels_are_checked():
    assert emotion_table(["pain=pain:1, grief:0.2"])["pain"] == "pain:1,grief:0.2"
    for bad in ("pain=sorrow:0.5", "pain=pain:1.5", "calm=joy:0.5", "pain"):
        with pytest.raises(ValueError):
            emotion_table([bad])
