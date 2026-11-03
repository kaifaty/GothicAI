import asyncio
import json
import os
import socket
import threading
import time
from types import SimpleNamespace
from unittest.mock import AsyncMock, Mock

import httpx
import numpy as np
import pytest
from aiohttp import web
from websockets.asyncio.client import connect
from websockets.exceptions import InvalidStatus

from gothic_asr import cli
from gothic_asr.audio import RATE, float_to_pcm16, resample
from gothic_asr.laughter import DEFAULT_URL, WINDOW, RemoteLaughter
from gothic_asr.providers import AsrError, make_provider, parse_spec
from gothic_asr.providers.base import Transcript
from gothic_asr.providers.fake import FakeProvider
from gothic_asr.providers.remote import OpenAICompatible
from gothic_asr.recognizer import LAUGHTER_WAIT, Recognizer, _echoes
from gothic_asr.service import AsrService


def tone(seconds: float, rate: int = RATE) -> np.ndarray:
    t = np.arange(int(seconds * rate)) / rate
    return (0.3 * np.sin(2 * np.pi * 220 * t)).astype(np.float32)


def _laughter(score: float = 0.98) -> RemoteLaughter:
    detector = RemoteLaughter()
    detector.close()
    detector._client = SimpleNamespace(
        request=Mock(return_value=httpx.Response(200, json={"laughing": score})), close=Mock(),
    )
    return detector


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
    detector = _laughter()
    recognizer = Recognizer(fake, laughter=detector)
    try:
        r = await recognizer.recognize(np.zeros(RATE * 5, dtype=np.float32), "ru")
        assert r.text == "" and r.provider == "" and not fake.calls
        assert r.laughing is None
        cancelled = recognizer.utterance("ru")
        await cancelled.feed(tone(1))
        await cancelled.cancel()
        detector._client.request.assert_not_called()
    finally:
        await recognizer.aclose()


async def test_fallback_on_failure() -> None:
    primary, fallback = FakeProvider(fail=True), FakeProvider(text="запасной")
    r = await Recognizer(primary, fallback).recognize(tone(1), "ru")
    assert (r.text, r.provider) == ("запасной", "fake")
    assert r.laughing is None
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
    await svc.recognizer.aclose()


@pytest.mark.parametrize("origin", [None, "https://evil.example"])
async def test_health(service, origin) -> None:
    svc, _ = service
    async with httpx.AsyncClient() as http:
        r = await http.get(
            f"http://127.0.0.1:{svc.port}/health", headers={"Origin": origin} if origin else {}
        )
    assert r.status_code == 200
    assert r.headers["content-type"] == "application/json"
    assert r.json() == {"ok": True, "service": "gothic-asr", "providers": [{"name": "fake"}]}


@pytest.mark.parametrize("bridge", [True, False])
async def test_local_origin_connects(service, bridge) -> None:
    svc, _ = service
    url = f"ws://127.0.0.1:{svc.port}"
    async with connect(url, origin=url if bridge else None) as ws:
        assert json.loads(await ws.recv())["type"] == "hello"


async def test_browser_origin_is_forbidden(service) -> None:
    svc, _ = service
    with pytest.raises(InvalidStatus) as exc:
        async with connect(f"ws://127.0.0.1:{svc.port}", origin="https://evil.example"):
            pytest.fail("browser origin connected")
    assert exc.value.response.status_code == 403


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
        assert final["laughing"] is None
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
    await svc.recognizer.aclose()


@pytest.mark.parametrize("streaming_service", [{}], indirect=True)
async def test_streaming_partials_then_final(streaming_service) -> None:
    svc, primary, fallback = streaming_service
    svc.recognizer = Recognizer(primary, fallback, laughter=_laughter())
    events = await _roundtrip(svc.port, float_to_pcm16(tone(1)))
    assert events[0]["streaming"] is True
    partials = [e["text"] for e in events if e["type"] == "partial"]
    assert partials[:2] == ["Стой", "Стой здесь"] and partials[-1] == "Стой здесь и никуда не уходи."
    assert events[-1]["type"] == "final" and events[-1]["provider"] == "fake"
    assert events[-1]["laughing"] == 0.98
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


def test_realtime_cli_hints_and_provider_defaults(monkeypatch, tmp_path, capsys) -> None:
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
    assert "laughing=None] Кавалорн" in capsys.readouterr().out
    open_stream.assert_awaited_once_with("ru", None, hints=["Кавалорн", "Ватрас"])


@pytest.mark.parametrize("streaming_service", [{"stream_fail": True}], indirect=True)
async def test_broken_stream_falls_back_with_the_whole_recording(streaming_service) -> None:
    svc, primary, fallback = streaming_service
    events = await _roundtrip(svc.port, float_to_pcm16(tone(1.5)))
    assert events[-1]["text"] == "запасной"
    assert fallback.calls == [(int(1.5 * RATE), "ru")]
    assert primary.streams[0].cancelled


HINTS = ["Ксардас", "Ксардаса", "Ксардасу", "Иннос", "Инноса", "Аданос", "Белиар", "Хоринис", "Хориниса"]


@pytest.mark.parametrize(
    ("text", "echo"),
    [
        # read back from the 2026-11-01 session: laughter, then the hints in their order
        ("Благо, Иннос, Инноса, Аданос, Белиар, Хорин", True),
        ("Ксардас, Ксардаса, Ксардасу, Иннос", True),
        ("ксардас, ксардаса", True),
        ("Белиар, Хоринис, Хор", True),
        ("Эээ, Ксардас, привет.", False),
        ("Иннос, Белиар и Аданос — три бога.", False),
        ("Ксардас, Иннос с тобой.", False),
        ("Иннос, Инн тут ни при чём.", False),  # a cut-off hint counts only at the very end
        ("Ксардас", False),
        ("", False),
    ],
)
def test_echoed_hints(text, echo) -> None:
    assert _echoes(text, HINTS) is echo
    assert not _echoes(text, None)


async def _utterance(recognizer: Recognizer, audio: np.ndarray):
    utt = recognizer.utterance("ru", hints=HINTS)
    await utt.begin()
    await utt.feed(audio)
    return await utt.finish()


async def test_hints_read_back_are_heard_again_without_them() -> None:
    primary = FakeProvider(text="Благо, Иннос, Инноса, Аданос, Белиар, Хорин", stream=True)
    primary.transcribe = AsyncMock(return_value=Transcript("Ха-ха-ха.", "fake"))
    detector = _laughter()
    recognizer = Recognizer(primary, laughter=detector)
    try:
        r = await _utterance(recognizer, tone(2))
        assert (r.text, r.provider, r.laughing) == ("Ха-ха-ха.", "fake", 0.98)
        assert primary.transcribe.await_args.args[0].shape == (2 * RATE,)
        detector._client.request.assert_called_once()
    finally:
        await recognizer.aclose()

    plain = FakeProvider(text="Эээ, Ксардас, привет.", stream=True)
    assert (await _utterance(Recognizer(plain), tone(1))).text == "Эээ, Ксардас, привет."
    assert not plain.calls


@pytest.mark.parametrize("failure", [AsrError("down"), TimeoutError()])
async def test_failed_hint_free_retry_keeps_empty_text(caplog, failure) -> None:
    primary = FakeProvider(text="Иннос, Инноса", stream=True)
    primary.transcribe = AsyncMock(side_effect=failure)
    recognizer = Recognizer(primary, laughter=_laughter())
    try:
        result = await _utterance(recognizer, tone(2))
        assert result.text == "" and result.laughing == 0.98
        assert "no transcript without hints" in caplog.text
    finally:
        await recognizer.aclose()


@pytest.mark.parametrize(
    ("seconds", "starts", "quiet_start"),
    [(1, [0], False), (4, [0], False), (5, [0, 1], False),
     (9, [0, 2, 4, 5], False), (9, [0, 2, 4, 5], True)],
)
async def test_incremental_laughter_matches_whole_recording(seconds, starts, quiet_start) -> None:
    audio = np.linspace(0.1, 0.9, seconds * RATE, dtype=np.float32)
    if quiet_start:
        audio[:WINDOW] = 0
    detector = _laughter()
    entered = threading.Event()
    seen: list[float] = []

    def run(_method, _url, *, content, headers):
        entered.set()
        window = np.frombuffer(content, dtype="<f4")
        seen.append(float(window[0]))
        return httpx.Response(200, json={"laughing": float(window.mean()) if np.any(window) else 0.99})

    detector._client.request.side_effect = run
    expected = detector.score(audio)
    seen.clear()
    entered.clear()
    recognizer = Recognizer(FakeProvider(stream=True), laughter=detector)
    try:
        utterance = recognizer.utterance("ru")
        await utterance.begin()
        for start in range(0, len(audio), RATE // 10):
            await utterance.feed(audio[start : start + RATE // 10])
            if start + RATE // 10 == WINDOW and not quiet_start:
                assert await asyncio.to_thread(entered.wait, 0.5)  # inference started before finish
        result = await utterance.finish()
        assert result.laughing == expected
        assert seen == [float(audio[start * RATE]) for start in starts]
    finally:
        await recognizer.aclose()


async def test_slow_laughter_does_not_hold_the_transcript(caplog) -> None:
    release = threading.Event()
    detector = _laughter()

    def blocked(*args, **kwargs):
        release.wait(2)
        return httpx.Response(200, json={"laughing": 0.99})

    detector._client.request.side_effect = blocked
    recognizer = Recognizer(FakeProvider(text="Привет."), laughter=detector)
    try:
        started = time.monotonic()
        result = await recognizer.recognize(tone(1), "ru")
        elapsed = time.monotonic() - started
        assert result.text == "Привет." and result.laughing is None
        assert LAUGHTER_WAIT - 0.02 <= elapsed < LAUGHTER_WAIT + 0.06
        assert "laughter detection exceeded" in caplog.text
    finally:
        release.set()
        await recognizer.aclose()


async def test_laughter_failure_keeps_the_transcript(caplog) -> None:
    detector = _laughter()
    detector._client.request.side_effect = httpx.ReadError("detector failed")
    recognizer = Recognizer(FakeProvider(text="Привет."), laughter=detector)
    try:
        for _ in range(2):
            result = await recognizer.recognize(tone(1), "ru")
            assert result.text == "Привет." and result.laughing is None
        assert caplog.text.count("laughter detector unavailable") == 1
    finally:
        await recognizer.aclose()


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


@pytest.fixture
async def laughter_server():
    state = {"requests": [], "health": 0, "delay": 0.0, "status": 200, "body": {"laughing": 0.875}}

    async def health(request):
        state["health"] += 1
        return web.json_response({"device": "cuda", "provider": "CUDAExecutionProvider"})

    async def window(request):
        state["requests"].append(await request.read())
        await asyncio.sleep(state["delay"])
        if isinstance(state["body"], str):
            return web.Response(text=state["body"], status=state["status"])
        return web.json_response(state["body"], status=state["status"])

    app = web.Application()
    app.router.add_get("/health", health)
    app.router.add_post("/window", window)
    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", 0)
    await site.start()
    port = site._server.sockets[0].getsockname()[1]
    try:
        yield f"http://127.0.0.1:{port}", state
    finally:
        await runner.cleanup()


async def test_remote_laughter_score_and_prepare(laughter_server) -> None:
    url, state = laughter_server
    detector = RemoteLaughter(url)
    audio = tone(1)
    try:
        assert await detector.prepare()
        assert state["health"] == 1
        assert await asyncio.to_thread(detector.window, audio) == 0.875
        assert state["requests"] == [audio.astype("<f4").tobytes()]
        assert detector._client.timeout.connect == 0.5 and detector._client.timeout.read == 1.0
    finally:
        detector.close()


async def test_remote_laughter_refusal_backoff_and_recovery(monkeypatch, caplog) -> None:
    from gothic_asr import laughter

    # Reserve a local port without listening so the first connection is refused.
    reserved = socket.socket()
    reserved.bind(("127.0.0.1", 0))
    port = reserved.getsockname()[1]
    detector = RemoteLaughter(f"http://127.0.0.1:{port}")
    clock = [100.0]
    monkeypatch.setattr(laughter, "monotonic", lambda: clock[0])
    caplog.set_level("INFO", logger="gothic_asr.laughter")
    requests = []

    async def window(request):
        requests.append(await request.read())
        return web.json_response({"laughing": 0.95})

    app = web.Application()
    app.router.add_post("/window", window)
    runner = web.AppRunner(app)
    try:
        started = time.monotonic()
        assert await asyncio.to_thread(detector.window, tone(1)) is None
        assert time.monotonic() - started < 0.6
        reserved.close()
        await runner.setup()
        await web.TCPSite(runner, "127.0.0.1", port).start()
        for offset in (0.0, 1.0, 9.99):
            clock[0] = 100.0 + offset
            assert await asyncio.to_thread(detector.window, tone(1)) is None
        assert requests == []
        assert caplog.text.count("laughter detector unavailable") == 1
        clock[0] = 110.0
        assert await asyncio.to_thread(detector.window, tone(1)) == 0.95
        assert len(requests) == 1 and "laughter detector recovered" in caplog.text
    finally:
        reserved.close()
        detector.close()
        await runner.cleanup()


async def test_remote_laughter_timeout(laughter_server, caplog) -> None:
    url, state = laughter_server
    state["delay"] = 1.1
    detector = RemoteLaughter(url)
    try:
        started = time.monotonic()
        assert await asyncio.to_thread(detector.window, tone(1)) is None
        assert 0.9 < time.monotonic() - started < 1.5
        assert len(state["requests"]) == 1
        assert caplog.text.count("laughter detector unavailable") == 1
    finally:
        detector.close()


@pytest.mark.parametrize(("status", "body"), [
    (503, {"error": "loading"}), (201, {"laughing": 0.5}), (200, "invalid JSON"),
])
async def test_remote_laughter_bad_response_stays_out_of_recognition(laughter_server, caplog, status, body):
    url, state = laughter_server
    state.update(status=status, body=body)
    recognizer = Recognizer(FakeProvider(text="Привет."), laughter=RemoteLaughter(url))
    try:
        for _ in range(2):
            result = await recognizer.recognize(tone(1), "ru")
            assert result.text == "Привет." and result.laughing is None
        assert caplog.text.count("laughter detector unavailable") == 1
    finally:
        await recognizer.aclose()


async def test_no_laughter_skips_detector_creation(monkeypatch) -> None:
    def unexpected_detector(*args, **kwargs):
        pytest.fail("disabled laughter detector must not be created")

    monkeypatch.setattr(cli, "RemoteLaughter", unexpected_detector)
    args = SimpleNamespace(
        provider="fake", fallback="none", timeout=4.0, no_laughter=True, laughter_url=DEFAULT_URL,
    )
    recognizer = cli._recognizer(args)
    assert recognizer.laughter is None
    await recognizer.aclose()


@pytest.mark.parametrize(("explicit", "environment", "expected"), [
    ("http://explicit:47838", "http://environment:47838", "http://explicit:47838"),
    (None, "http://environment:47838", "http://environment:47838"),
    (None, None, DEFAULT_URL),
])
def test_laughter_cli_scores_each_wav(monkeypatch, capsys, explicit, environment, expected) -> None:
    detector = SimpleNamespace(
        prepare=AsyncMock(return_value=True), score=Mock(return_value=0.987), close=Mock(),
    )
    options = []

    def make_detector(url):
        options.append(url)
        return detector

    monkeypatch.setattr(cli, "RemoteLaughter", make_detector)
    monkeypatch.setattr(cli, "load_env_file", lambda: None)
    monkeypatch.setattr(cli, "read_wav", lambda _: (tone(1.0, 8000), 8000))
    if environment:
        monkeypatch.setenv("GOTHIC_LAUGHTER_URL", environment)
    else:
        monkeypatch.delenv("GOTHIC_LAUGHTER_URL", raising=False)
    options_arg = ["--laughter-url", explicit] if explicit else []
    monkeypatch.setattr("sys.argv", ["gothic-asr", "laughter", *options_arg, "first.wav", "second.wav"])
    with pytest.raises(SystemExit) as exit_info:
        cli.main()
    assert exit_info.value.code == 0
    assert options == [expected]
    detector.prepare.assert_awaited_once_with()
    detector.close.assert_called_once_with()
    assert detector.score.call_count == 2
    assert all(call.args[0].shape == (RATE,) for call in detector.score.call_args_list)
    assert capsys.readouterr().out.splitlines() == [
        "first.wav: laughing=0.987", "second.wav: laughing=0.987"
    ]
