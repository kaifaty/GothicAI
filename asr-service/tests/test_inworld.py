import asyncio
import json

import numpy as np
import pytest
from websockets.asyncio.server import ServerConnection, serve
from websockets.exceptions import ConnectionClosed

from gothic_asr.audio import RATE, float_to_pcm16
from gothic_asr.providers import AsrError, make_provider
from gothic_asr.providers.fake import FakeProvider
from gothic_asr.providers.inworld import InworldRealtime
from gothic_asr.recognizer import Recognizer


def tone(seconds: float = 0.5) -> np.ndarray:
    t = np.arange(int(seconds * RATE)) / RATE
    return (0.3 * np.sin(2 * np.pi * 220 * t)).astype(np.float32)


def transcription(text: str, final: bool = False) -> str:
    return json.dumps({"result": {"transcription": {"transcript": text, "isFinal": final}}})


class InworldServer:
    """A local peer that records the wire protocol and can delay or fail an utterance."""

    def __init__(self) -> None:
        self.url = ""
        self.connections: list[ServerConnection] = []
        self.configs: list[dict] = []
        self.audio: list[list[bytes]] = []
        self.configured: asyncio.Queue[int] = asyncio.Queue()
        self.received: asyncio.Queue[tuple[int, bytes | dict]] = asyncio.Queue()
        self.providers: list[InworldRealtime] = []
        self.reply: str | None = "Кавалорн."
        self.close_on_request = True
        self.handshake_gate: asyncio.Event | None = None
        self.handshake_started = asyncio.Event()

    def provider(self, **options) -> InworldRealtime:
        provider = InworldRealtime(url=self.url, **{"timeout": 1.0, **options})
        self.providers.append(provider)
        return provider

    async def process_request(self, connection, request):
        self.handshake_started.set()
        if self.handshake_gate is not None:
            await self.handshake_gate.wait()

    async def handle(self, ws: ServerConnection) -> None:
        try:
            config = json.loads(await ws.recv())
        except ConnectionClosed:
            return
        index = len(self.connections)
        self.connections.append(ws)
        self.audio.append([])
        self.configs.append(config)
        self.configured.put_nowait(index)
        try:
            async for raw in ws:
                message = raw if isinstance(raw, bytes) else json.loads(raw)
                self.received.put_nowait((index, message))
                if isinstance(message, bytes):
                    self.audio[index].append(message)
                elif "endTurn" in message and self.reply is not None:
                    await ws.send(transcription(self.reply, final=True))
                elif "closeStream" in message and self.close_on_request:
                    return
        except ConnectionClosed:
            pass

    async def next_config(self) -> int:
        return await asyncio.wait_for(self.configured.get(), 2.0)

    async def next_message(self) -> tuple[int, bytes | dict]:
        return await asyncio.wait_for(self.received.get(), 2.0)


@pytest.fixture
async def inworld_server(monkeypatch):
    monkeypatch.setenv("INWORLD_API_KEY", "test-inworld-key")
    server = InworldServer()
    async with serve(
        server.handle, "127.0.0.1", 0, process_request=server.process_request
    ) as listener:
        port = listener.sockets[0].getsockname()[1]
        server.url = f"ws://127.0.0.1:{port}"
        try:
            yield server
        finally:
            if server.handshake_gate is not None:
                server.handshake_gate.set()
            for provider in server.providers:
                await provider.aclose()


def test_inworld_registration() -> None:
    provider = make_provider(
        "inworld:model=test-model,url=ws://127.0.0.1:1,api_key_env=TEST_INWORLD_KEY,timeout=0.5"
    )
    assert isinstance(provider, InworldRealtime)
    assert provider.name == "inworld" and provider.streaming is True
    assert (provider.model, provider.url, provider.api_key_env, provider.timeout) == (
        "test-model", "ws://127.0.0.1:1", "TEST_INWORLD_KEY", 0.5
    )


async def test_inworld_missing_key_fails_in_warmup(monkeypatch) -> None:
    monkeypatch.delenv("TEST_INWORLD_MISSING_KEY", raising=False)
    provider = InworldRealtime(api_key_env="TEST_INWORLD_MISSING_KEY")
    with pytest.raises(AsrError, match="TEST_INWORLD_MISSING_KEY"):
        await provider.warmup()
    await provider.aclose()


async def test_inworld_config_auth_and_binary_audio(inworld_server) -> None:
    server = inworld_server
    provider = server.provider(model="inworld/inworld-stt-1")
    hints = ["Кавалорн", "Долина Рудников"]
    stream = await provider.open_stream("ru", hints=hints)
    first, second = tone(0.2), tone(0.3)
    await stream.feed(first)
    await stream.feed(second)
    result = await stream.finish()

    assert (result.text, result.provider) == ("Кавалорн.", "inworld")
    assert server.configs == [
        {
            "transcribeConfig": {
                "modelId": "inworld/inworld-stt-1",
                "audioEncoding": "LINEAR16",
                "sampleRateHertz": RATE,
                "numberOfChannels": 1,
                "language": "ru",
                "inworldSttV1Config": {"vadThreshold": 0},
                "prompts": hints,
            },
            "audioProtocol": "BINARY",
        }
    ]
    assert server.connections[0].request.headers["Authorization"] == "Basic test-inworld-key"
    assert b"".join(server.audio[0]) == float_to_pcm16(np.concatenate([first, second]))
    messages = [await server.next_message() for _ in range(3)]
    assert messages[-1] == (0, {"endTurn": {}})


async def test_inworld_reuses_connection_for_matching_hints(inworld_server) -> None:
    server = inworld_server
    provider = server.provider()
    for _ in range(2):
        stream = await provider.open_stream("ru", hints=["Кавалорн", "Лотара"])
        await stream.feed(tone())
        assert (await stream.finish()).text == "Кавалорн."
    assert len(server.connections) == 1
    assert len(server.audio[0]) == 2


@pytest.mark.parametrize(
    ("lang", "hints"), [("ru", ["Ватрас"]), ("en", ["Кавалорн"])]
)
async def test_inworld_reconnects_when_config_changes(inworld_server, lang, hints) -> None:
    server = inworld_server
    provider = server.provider()
    first = await provider.open_stream("ru", hints=["Кавалорн"])
    await first.feed(tone())
    await first.finish()
    second = await provider.open_stream(lang, hints=hints)
    await second.feed(tone())
    await second.finish()

    assert len(server.connections) == 2
    assert server.configs[1]["transcribeConfig"]["language"] == lang
    assert server.configs[1]["transcribeConfig"]["prompts"] == hints
    await asyncio.wait_for(server.connections[0].wait_closed(), 1.0)


async def test_inworld_uses_sanitized_hints_for_connection_reuse(inworld_server) -> None:
    server = inworld_server
    provider = server.provider()
    for hints in (["Кавалорн", "bad_hint", "Кавалорн", ""], ["Кавалорн"]):
        stream = await provider.open_stream("ru", hints=hints)
        await stream.feed(tone())
        await stream.finish()
    assert len(server.connections) == 1


async def test_inworld_reopens_connection_closed_while_idle(inworld_server) -> None:
    server = inworld_server
    provider = server.provider()
    first = await provider.open_stream("ru")
    await first.feed(tone())
    await first.finish()
    await server.connections[0].close()

    second = await provider.open_stream("ru")
    await second.feed(tone())
    assert (await second.finish()).text == "Кавалорн."
    assert len(server.connections) == 2


async def test_inworld_buffers_audio_while_connection_is_opening(inworld_server) -> None:
    server = inworld_server
    server.handshake_gate = asyncio.Event()
    provider = server.provider()
    stream = await asyncio.wait_for(provider.open_stream("ru"), 0.2)
    await asyncio.wait_for(server.handshake_started.wait(), 1.0)
    first, second = tone(0.2), tone(0.3)
    await asyncio.wait_for(stream.feed(first), 0.2)
    await asyncio.wait_for(stream.feed(second), 0.2)
    assert not server.configs
    server.handshake_gate.set()

    assert (await stream.finish()).text == "Кавалорн."
    assert b"".join(server.audio[0]) == float_to_pcm16(np.concatenate([first, second]))


@pytest.mark.parametrize("failure", ["error", "disconnect"])
async def test_inworld_failure_falls_back_with_whole_recording(inworld_server, failure) -> None:
    server = inworld_server
    server.reply = None
    provider = server.provider()
    fallback = FakeProvider(text="запасной")
    utterance = Recognizer(provider, fallback).utterance("ru")
    await utterance.begin()
    await utterance.feed(tone(0.2))
    await utterance.feed(tone(0.3))
    assert await server.next_config() == 0
    assert isinstance((await server.next_message())[1], bytes)
    if failure == "error":
        await server.connections[0].send(json.dumps({"error": {"code": 3, "message": "bad prompt"}}))
    else:
        server.connections[0].transport.abort()

    result = await utterance.finish()
    assert (result.text, result.provider) == ("запасной", "fake")
    assert fallback.calls == [(RATE // 2, "ru")]


async def test_inworld_server_error_is_asr_error(inworld_server) -> None:
    server = inworld_server
    server.reply = None
    stream = await server.provider().open_stream("ru")
    await stream.feed(tone())
    await server.next_config()
    await server.connections[0].send(json.dumps({"error": {"code": 3, "message": "bad prompt"}}))
    with pytest.raises(AsrError, match="bad prompt"):
        await stream.finish()


@pytest.mark.parametrize(
    "event",
    [
        pytest.param([], id="event-list"),
        pytest.param({"result": []}, id="result-list"),
        pytest.param({"result": {"transcription": []}}, id="transcription-list"),
        pytest.param({"error": []}, id="error-list"),
        pytest.param({"result": {"transcription": {"transcript": []}}}, id="transcript-list"),
    ],
)
async def test_inworld_malformed_response_fails_without_reader_exception(inworld_server, event) -> None:
    server = inworld_server
    server.reply = None
    provider = server.provider()
    stream = await provider.open_stream("ru")
    await stream.feed(tone())
    await server.next_config()
    assert isinstance((await server.next_message())[1], bytes)
    reader = provider._reader
    assert reader is not None
    await server.connections[0].send(json.dumps(event))

    with pytest.raises(AsrError, match="invalid response"):
        await asyncio.wait_for(stream.finish(), 0.25)
    assert reader.done() and not reader.cancelled() and reader.exception() is None


async def test_inworld_missing_final_is_an_error(inworld_server) -> None:
    server = inworld_server
    server.reply = None
    stream = await server.provider(timeout=0.05).open_stream("ru")
    await stream.feed(tone())
    with pytest.raises(AsrError, match="final transcript"):
        await stream.finish()


async def test_inworld_partials_replace_and_include_prior_final_chunks(inworld_server) -> None:
    server = inworld_server
    server.reply = None
    partials: list[str] = []
    received: asyncio.Queue[str] = asyncio.Queue()

    def on_partial(text: str) -> None:
        partials.append(text)
        received.put_nowait(text)

    async def wait_partial(expected: str) -> None:
        async with asyncio.timeout(1.0):
            while await received.get() != expected:
                pass

    stream = await server.provider().open_stream("ru", on_partial)
    await stream.feed(tone())
    await server.next_config()
    ws = server.connections[0]
    for text in ("Кава", "Кавалорн"):
        await ws.send(transcription(text))
        await wait_partial(text)
    await ws.send(transcription("Кавалорн.", final=True))
    await ws.send(json.dumps({"result": {"speechStarted": {}}}))
    await ws.send(json.dumps({"result": {"speechStopped": {}}}))
    await ws.send(json.dumps({"result": {"usage": {"audioSeconds": 0.5}}}))
    await ws.send(transcription("ид"))
    await wait_partial("Кавалорн. ид")
    await ws.send(transcription("идём"))
    await wait_partial("Кавалорн. идём")
    assert partials[:2] == ["Кава", "Кавалорн"]
    assert partials[-1] == "Кавалорн. идём"

    finishing = asyncio.create_task(stream.finish())
    async with asyncio.timeout(1.0):
        while (await server.next_message())[1] != {"endTurn": {}}:
            pass
    await ws.send(transcription("со мной.", final=True))
    result = await finishing
    assert result.text == "Кавалорн. со мной."


async def test_inworld_filters_hints_and_limits_them_to_first_100(inworld_server) -> None:
    server = inworld_server
    expected = ["Кавалорн", "Лотара", "Арена. , ' ( ) : ! ? ; -"]
    expected += [f"Имя {index}" for index in range(97)]
    hints = [
        "", "   ", "bad_hint", "имя🗡", "имя\tимя", "имя—имя", "имя\nимя",
        "Кавалорн", "Кавалорн", "Лотара", "Арена. , ' ( ) : ! ? ; -",
        *[f"Имя {index}" for index in range(110)],
    ]
    stream = await server.provider().open_stream("ru", hints=hints)
    await stream.feed(tone())
    await stream.finish()
    assert server.configs[0]["transcribeConfig"]["prompts"] == expected


async def test_inworld_cancel_closes_connection_and_isolates_late_response(inworld_server) -> None:
    server = inworld_server
    server.reply = None
    provider = server.provider()
    old_partials: asyncio.Queue[str] = asyncio.Queue()
    cancelled = await provider.open_stream("ru", old_partials.put_nowait)
    await cancelled.feed(tone())
    await server.next_config()
    old_ws = server.connections[0]
    await old_ws.send(transcription("Отменённая реплика"))
    assert await asyncio.wait_for(old_partials.get(), 1.0) == "Отменённая реплика"
    await cancelled.cancel()
    await asyncio.wait_for(old_ws.wait_closed(), 1.0)

    new_partials: list[str] = []
    current = await provider.open_stream("ru", new_partials.append)
    await current.feed(tone())
    assert await server.next_config() == 1
    with pytest.raises(ConnectionClosed):
        await old_ws.send(transcription("Поздний ответ", final=True))
    server.reply = "Новая реплика"
    assert (await current.finish()).text == "Новая реплика"
    assert all("Отменённая" not in text and "Поздний" not in text for text in new_partials)
    assert len(server.connections) == 2


async def test_inworld_cancel_during_config_change_closes_old_connection(
    inworld_server, monkeypatch
) -> None:
    server = inworld_server
    server.close_on_request = False
    provider = server.provider()
    first = await provider.open_stream("ru", hints=["Кавалорн"])
    await first.feed(tone())
    await first.finish()
    old_ws = provider._ws
    assert old_ws is not None
    close_started = asyncio.Event()
    release_close = asyncio.Event()
    original_close = old_ws.close
    calls = 0

    async def delayed_first_close(*args, **kwargs) -> None:
        nonlocal calls
        calls += 1
        if calls == 1:
            close_started.set()
            await release_close.wait()
        await original_close(*args, **kwargs)

    monkeypatch.setattr(old_ws, "close", delayed_first_close)
    second = await provider.open_stream("ru", hints=["Ватрас"])
    await second.feed(tone())
    try:
        await asyncio.wait_for(close_started.wait(), 1.0)
        await asyncio.wait_for(second.cancel(), 1.0)
        await asyncio.wait_for(server.connections[0].wait_closed(), 1.0)
    finally:
        release_close.set()

    third = await asyncio.wait_for(provider.open_stream("ru", hints=["Ватрас"]), 1.0)
    await third.feed(tone())
    assert (await third.finish()).text == "Кавалорн."
    assert len(server.connections) == 2


async def test_inworld_cancel_during_initial_handshake_releases_stream(inworld_server) -> None:
    server = inworld_server
    server.handshake_gate = asyncio.Event()
    provider = server.provider()
    first = await provider.open_stream("ru")
    await first.feed(tone())
    await asyncio.wait_for(server.handshake_started.wait(), 1.0)
    await asyncio.wait_for(first.cancel(), 1.0)
    server.handshake_gate.set()

    second = await asyncio.wait_for(provider.open_stream("ru"), 1.0)
    await second.feed(tone())
    assert (await second.finish()).text == "Кавалорн."
    assert len(server.configs) == 1


async def test_inworld_explicit_empty_final_is_success(inworld_server) -> None:
    server = inworld_server
    server.reply = ""
    stream = await server.provider().open_stream("ru")
    await stream.feed(tone())
    result = await stream.finish()
    assert (result.text, result.provider) == ("", "inworld")


async def test_inworld_final_without_transcript_is_immediately_empty(inworld_server) -> None:
    server = inworld_server
    server.reply = None
    stream = await server.provider().open_stream("ru")
    await stream.feed(tone())
    await server.next_config()
    assert isinstance((await server.next_message())[1], bytes)
    finishing = asyncio.create_task(stream.finish())
    assert await server.next_message() == (0, {"endTurn": {}})
    await server.connections[0].send(json.dumps({"result": {"transcription": {"isFinal": True}}}))

    result = await asyncio.wait_for(finishing, 0.25)
    assert (result.text, result.provider) == ("", "inworld")


async def test_inworld_missing_is_final_defaults_to_interim(inworld_server) -> None:
    server = inworld_server
    partials: asyncio.Queue[str] = asyncio.Queue()
    stream = await server.provider().open_stream("ru", partials.put_nowait)
    await stream.feed(tone())
    await server.next_config()
    await server.connections[0].send(json.dumps({"result": {"transcription": {"transcript": "Кава"}}}))

    assert await asyncio.wait_for(partials.get(), 0.25) == "Кава"
    assert (await stream.finish()).text == "Кавалорн."
