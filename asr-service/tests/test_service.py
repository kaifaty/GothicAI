import asyncio
import json
import os
from unittest.mock import AsyncMock

import httpx
import numpy as np
import pytest
from websockets.asyncio.client import connect

from gothic_asr import cli
from gothic_asr.audio import RATE, float_to_pcm16, resample
from gothic_asr.providers import AsrError, make_provider, parse_spec
from gothic_asr.providers.fake import FakeProvider
from gothic_asr.providers.remote import OpenAICompatible
from gothic_asr.recognizer import Recognizer
from gothic_asr.service import AsrService


def tone(seconds: float, rate: int = RATE) -> np.ndarray:
    t = np.arange(int(seconds * rate)) / rate
    return (0.3 * np.sin(2 * np.pi * 220 * t)).astype(np.float32)


def test_parse_spec() -> None:
    assert parse_spec("chatgpt") == ("chatgpt", {})
    assert parse_spec("openai:model=whisper-large-v3-turbo,base_url=https://x/v1") == (
        "openai",
        {"model": "whisper-large-v3-turbo", "base_url": "https://x/v1"},
    )
    with pytest.raises(ValueError):
        parse_spec("openai:model")
    with pytest.raises(ValueError):
        make_provider("nope")
    assert make_provider("fake:text=Да").text == "Да"


async def test_silence_is_skipped() -> None:
    fake = FakeProvider()
    r = await Recognizer(fake).recognize(np.zeros(RATE * 2, dtype=np.float32), "ru")
    assert r.text == "" and r.provider == "" and not fake.calls


async def test_fallback_on_failure() -> None:
    primary, fallback = FakeProvider(fail=True), FakeProvider(text="запасной")
    r = await Recognizer(primary, fallback).recognize(tone(1), "ru")
    assert (r.text, r.provider) == ("запасной", "fake")
    assert primary.calls and fallback.calls

    with pytest.raises(AsrError, match="fake provider failure"):
        await Recognizer(FakeProvider(fail=True)).recognize(tone(1), "ru")


async def test_fallback_on_timeout() -> None:
    r = await Recognizer(FakeProvider(delay=1), FakeProvider(text="быстрый"), timeout=0.05).recognize(
        tone(1), "ru"
    )
    assert r.text == "быстрый"


def test_resample_length() -> None:
    assert len(resample(tone(1, 48000), 48000)) == RATE
    assert len(resample(tone(1, 8000), 8000)) == RATE


@pytest.fixture
async def service():
    fake = FakeProvider(text="Привет, Ксардас.")
    svc = AsrService(Recognizer(fake), port=0)
    started = asyncio.Event()
    task = asyncio.create_task(svc.serve_forever(started))
    await started.wait()
    yield svc, fake
    task.cancel()
    await asyncio.gather(task, return_exceptions=True)


async def test_health(service) -> None:
    svc, _ = service
    async with httpx.AsyncClient() as http:
        r = await http.get(f"http://127.0.0.1:{svc.port}/health")
    assert r.headers["content-type"] == "application/json"
    assert r.json() == {"ok": True, "service": "gothic-asr", "providers": [{"name": "fake"}]}


async def test_websocket_roundtrip(service) -> None:
    svc, fake = service
    async with connect(f"ws://127.0.0.1:{svc.port}") as ws:
        hello = json.loads(await ws.recv())
        assert hello["type"] == "hello" and hello["providers"] == ["fake"]

        # 48 kHz input is resampled to 16 kHz for the provider
        await ws.send(json.dumps({"type": "start", "lang": "ru", "sample_rate": 48000, "format": "pcm16"}))
        pcm = float_to_pcm16(tone(1.5, 48000))
        for i in range(0, len(pcm), 9600):
            await ws.send(pcm[i : i + 9600])
        await ws.send(json.dumps({"type": "end"}))
        final = json.loads(await ws.recv())
        assert final["type"] == "final" and final["text"] == "Привет, Ксардас."
        assert final["audio_s"] == pytest.approx(1.5, abs=0.01)
        assert fake.calls == [(int(1.5 * RATE), "ru")]

        # cancelled utterances are dropped, the next one works
        await ws.send(json.dumps({"type": "start", "sample_rate": RATE}))
        await ws.send(float_to_pcm16(tone(1)))
        await ws.send(json.dumps({"type": "cancel"}))
        await ws.send(json.dumps({"type": "end"}))
        assert json.loads(await ws.recv())["type"] == "error"
        assert len(fake.calls) == 1


async def test_openai_compatible() -> None:
    seen: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        seen.append(request)
        if b"bad-model" in request.content:
            return httpx.Response(400, json={"error": {"message": "model not found"}})
        return httpx.Response(200, json={"text": " Стой здесь. "})

    os.environ["TEST_ASR_KEY"] = "k"
    p = OpenAICompatible(api_key_env="TEST_ASR_KEY", transport=httpx.MockTransport(handler))
    t = await p.transcribe(tone(1), "ru")
    assert t.text == "Стой здесь."
    assert seen[0].url.path == "/v1/audio/transcriptions"
    assert seen[0].headers["authorization"] == "Bearer k"
    assert b'name="language"\r\n\r\nru' in seen[0].content

    bad = OpenAICompatible(
        model="bad-model", api_key_env="TEST_ASR_KEY", transport=httpx.MockTransport(handler)
    )
    with pytest.raises(AsrError, match="model not found"):
        await bad.transcribe(tone(1), "ru")

    nokey = OpenAICompatible(api_key_env="TEST_ASR_MISSING_KEY")
    with pytest.raises(AsrError, match="TEST_ASR_MISSING_KEY"):
        await nokey.transcribe(tone(1), "ru")


async def _roundtrip(
    port: int, pcm: bytes, chunk: int = 3200, *, hints: list[str] | None = None
) -> list[dict]:
    """One utterance sent in chunks; every event up to the final (or error)."""
    events = []
    async with connect(f"ws://127.0.0.1:{port}") as ws:
        events.append(json.loads(await ws.recv()))
        start = {"type": "start", "lang": "ru", "sample_rate": RATE}
        if hints is not None:
            start["hints"] = hints
        await ws.send(json.dumps(start))
        for i in range(0, len(pcm), chunk):
            await ws.send(pcm[i : i + chunk])
        await asyncio.sleep(0.05)  # partials of the last chunks
        await ws.send(json.dumps({"type": "end"}))
        while events[-1]["type"] not in ("final", "error"):
            events.append(json.loads(await ws.recv()))
    return events


@pytest.fixture
async def streaming_service(request):
    primary = FakeProvider(text="Стой здесь и никуда не уходи.", stream=True, **request.param)
    fallback = FakeProvider(text="запасной")
    svc = AsrService(Recognizer(primary, fallback), port=0)
    started = asyncio.Event()
    task = asyncio.create_task(svc.serve_forever(started))
    await started.wait()
    yield svc, primary, fallback
    task.cancel()
    await asyncio.gather(task, return_exceptions=True)


@pytest.mark.parametrize("streaming_service", [{}], indirect=True)
async def test_streaming_partials_then_final(streaming_service) -> None:
    svc, primary, fallback = streaming_service
    events = await _roundtrip(svc.port, float_to_pcm16(tone(1)))
    assert events[0]["streaming"] is True
    partials = [e["text"] for e in events if e["type"] == "partial"]
    assert partials[:2] == ["Стой", "Стой здесь"] and partials[-1] == "Стой здесь и никуда не уходи."
    assert events[-1]["type"] == "final" and events[-1]["provider"] == "fake"
    assert primary.streams[0].samples == RATE and not fallback.calls


@pytest.mark.parametrize("streaming_service", [{}], indirect=True)
async def test_start_hints_reach_streaming_provider(streaming_service, monkeypatch) -> None:
    svc, primary, _ = streaming_service
    open_stream = AsyncMock(wraps=primary.open_stream)
    monkeypatch.setattr(primary, "open_stream", open_stream)
    hints = ["Кавалорн", "Лотара", "Долина Рудников"]
    events = await _roundtrip(svc.port, float_to_pcm16(tone(0.5)), hints=hints)
    assert events[-1]["type"] == "final"
    assert open_stream.await_args.kwargs == {"hints": hints}


@pytest.mark.parametrize("hints", [None, "Кавалорн", {"name": "Кавалорн"}, [1], ["Лотар", None]])
@pytest.mark.parametrize("streaming_service", [{}], indirect=True)
async def test_malformed_hints_are_rejected_without_closing_client(streaming_service, hints) -> None:
    svc, primary, _ = streaming_service
    async with connect(f"ws://127.0.0.1:{svc.port}") as ws:
        await ws.recv()
        await ws.send(json.dumps({"type": "start", "hints": hints}))
        assert json.loads(await ws.recv()) == {
            "type": "error",
            "detail": "hints must be a list of strings",
        }
        assert not primary.streams
        await ws.send(json.dumps({"type": "start", "hints": []}))
        await ws.send(float_to_pcm16(tone(0.5)))
        await ws.send(json.dumps({"type": "end"}))
        while (event := json.loads(await ws.recv()))["type"] == "partial":
            pass
        assert event["type"] == "final"
        assert len(primary.streams) == 1


def test_realtime_cli_hints_and_provider_defaults(monkeypatch, tmp_path) -> None:
    primary = FakeProvider(text="Кавалорн", stream=True)
    open_stream = AsyncMock(wraps=primary.open_stream)
    monkeypatch.setattr(primary, "open_stream", open_stream)
    parsed = []

    def recognizer(args):
        parsed.append(args)
        return Recognizer(primary)

    monkeypatch.setattr(cli, "_recognizer", recognizer)
    monkeypatch.setattr(cli, "load_env_file", lambda: None)
    monkeypatch.setattr(cli, "read_wav", lambda _: (tone(0.4), RATE))
    monkeypatch.delenv("GOTHIC_ASR_PROVIDER", raising=False)
    monkeypatch.delenv("GOTHIC_ASR_FALLBACK", raising=False)
    monkeypatch.setattr(
        "sys.argv",
        ["gothic-asr", "transcribe", "--realtime", "--hints", "Кавалорн, Ватрас", str(tmp_path / "a.wav")],
    )
    with pytest.raises(SystemExit) as exit_info:
        cli.main()
    assert exit_info.value.code == 0
    assert parsed[0].provider == "inworld" and parsed[0].fallback == "chatgpt"
    open_stream.assert_awaited_once_with("ru", None, hints=["Кавалорн", "Ватрас"])


@pytest.mark.parametrize("streaming_service", [{"stream_fail": True}], indirect=True)
async def test_broken_stream_falls_back_with_the_whole_recording(streaming_service) -> None:
    svc, primary, fallback = streaming_service
    events = await _roundtrip(svc.port, float_to_pcm16(tone(1.5)))
    assert events[-1]["text"] == "запасной"
    assert fallback.calls == [(int(1.5 * RATE), "ru")]
    assert primary.streams[0].cancelled


async def test_voxtral_speaks_audiocpp_live() -> None:
    """The provider against a stand-in for audio.cpp's /v1/audio/transcriptions/live."""
    from gothic_asr.providers.voxtral import TAIL_S, VoxtralRealtime

    requests: list[tuple[str, dict[str, str]]] = []

    async def audiocpp(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        method, target, _ = (await reader.readuntil(b"\r\n")).decode().split()
        headers = {}
        while (line := (await reader.readuntil(b"\r\n")).decode().strip()) != "":
            key, _, value = line.partition(":")
            headers[key.strip().lower()] = value.strip()
        requests.append((f"{method} {target}", headers))
        if method == "GET":
            writer.write(b'HTTP/1.1 200 OK\r\nContent-Length: 15\r\nConnection: close\r\n\r\n{"status":"ok"}')
        elif "model=busy" in target:
            body = b'{"error":{"message":"server_busy","type":"server_busy"}}'
            writer.write(b"HTTP/1.1 503 Busy\r\nContent-Length: %d\r\n\r\n%s" % (len(body), body))
        else:
            writer.write(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n")

            def sse(ev: dict) -> None:
                data = f"data: {json.dumps(ev, ensure_ascii=False)}\n\n".encode()
                writer.write(b"%x\r\n%s\r\n" % (len(data), data))

            samples = 0
            try:
                while size := int(await reader.readuntil(b"\r\n"), 16):
                    samples += len(await reader.readexactly(size)) // 2
                    await reader.readexactly(2)
                    if samples == size // 2:
                        sse({"type": "transcript.text.delta", "delta": " Стой"})
                        await writer.drain()
                await reader.readexactly(2)
            except asyncio.IncompleteReadError:
                writer.close()  # the client dropped the request: no transcript
                return
            sse({"type": "transcript.text.delta", "delta": " здесь."})
            sse({"type": "transcript.text.done", "text": f" Стой здесь. {samples}"})
            writer.write(b"e\r\ndata: [DONE]\n\n\r\n0\r\n\r\n")
        await writer.drain()
        writer.close()

    server = await asyncio.start_server(audiocpp, "127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]
    tail = round(TAIL_S * RATE)
    p = VoxtralRealtime(url=f"http://127.0.0.1:{port}", model="voxtral-test")
    await p.warmup()
    partials: list[str] = []
    stream = await p.open_stream("ru", partials.append)
    await stream.feed(tone(0.5))
    await stream.feed(tone(0.5))
    t = await stream.finish()
    assert t.text == f"Стой здесь. {RATE + tail}" and t.provider == "voxtral"
    assert partials == ["Стой", "Стой здесь."]
    # a cancelled stream frees the model for the next utterance
    cancelled = await p.open_stream("ru")
    await cancelled.feed(tone(0.2))
    await cancelled.cancel()
    assert (await p.transcribe(tone(1), "ru")).text == f"Стой здесь. {RATE + tail}"
    with pytest.raises(AsrError, match="503: server_busy"):
        await VoxtralRealtime(url=f"http://127.0.0.1:{port}", model="busy").transcribe(tone(0.5), "ru")
    await p.aclose()
    server.close()
    await server.wait_closed()

    assert requests[0][0] == "GET /health"
    target, headers = requests[1]
    path, _, query = target.partition("?")
    assert path == "POST /v1/audio/transcriptions/live"
    assert query == "model=voxtral-test&sample_rate=16000&channels=1&sample_format=s16le&language=ru"
    assert headers["transfer-encoding"] == "chunked"
