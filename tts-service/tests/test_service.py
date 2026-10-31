import asyncio
import json
import logging

import httpx
import pytest
from websockets.asyncio.client import connect
from websockets.asyncio.server import serve

from gothic_tts.providers import TtsError, Voice, make_provider, parse_spec
from gothic_tts.providers.fake import FakeProvider
from gothic_tts.providers.voxcpm import VoxCPM
from gothic_tts.service import TtsService, load_original_voices
from gothic_tts.synthesizer import Synthesizer

XARDAS = Voice("xardas", "An old man with a deep voice")


def test_specs() -> None:
    assert parse_spec("openai:voice=onyx,model=gpt-4o-mini-tts") == (
        "openai",
        {"voice": "onyx", "model": "gpt-4o-mini-tts"},
    )
    with pytest.raises(ValueError):
        make_provider("nope")


async def _collect(synth: Synthesizer, text: str) -> tuple[bytes, list[str]]:
    parts: list[bytes] = []
    providers: list[str] = []

    async def chunk(pcm: bytes, p) -> None:
        parts.append(pcm)
        providers.append(p.name)

    await synth.speak(text, XARDAS, chunk)
    return b"".join(parts), providers


async def test_fallback_only_before_audio() -> None:
    primary, fallback = FakeProvider(fail=True), FakeProvider()
    synth = Synthesizer(primary, fallback)
    pcm, providers = await _collect(synth, "Привет")
    assert pcm and primary.calls and fallback.calls
    await synth.aclose()

    class HalfSpoken(FakeProvider):
        async def synthesize(self, text, voice, on_chunk):
            await on_chunk(b"\0\0" * 100)
            raise TtsError("broke mid-line")

    second = FakeProvider()
    synth = Synthesizer(HalfSpoken(), second)
    with pytest.raises(TtsError, match="mid-line"):
        await _collect(synth, "Привет")
    assert not second.calls  # no voice switch halfway through a line
    assert synth.available() == [second]  # but the next line is the fallback's
    await synth.aclose()


async def test_failed_provider_left_out_until_it_is_back() -> None:
    primary, fallback = FakeProvider(), FakeProvider()
    synth = Synthesizer(primary, fallback, retry_s=0.05)
    primary.fail = True
    await _collect(synth, "Раз")
    assert len(primary.calls) == 1 and len(fallback.calls) == 1
    await _collect(synth, "Два")
    assert len(primary.calls) == 1  # not tried again: the line goes straight to the fallback
    assert len(fallback.calls) == 2

    primary.fail = False  # the background check finds it working again
    for _ in range(50):
        if primary in synth.available():
            break
        await asyncio.sleep(0.02)
    await _collect(synth, "Три")
    assert len(primary.calls) == 2 and len(fallback.calls) == 2
    await synth.aclose()


async def test_no_voice_at_once_when_every_provider_is_out() -> None:
    primary, fallback = FakeProvider(fail=True), FakeProvider(fail=True)
    synth = Synthesizer(primary, fallback, retry_s=60)
    await synth.warmup()  # both fail their warmup
    assert synth.available() == []
    with pytest.raises(TtsError, match="no voice: fake, fake unavailable"):
        await _collect(synth, "Привет")
    assert not primary.calls and not fallback.calls  # the game gets a subtitle without waiting
    await synth.aclose()


@pytest.fixture
async def service():
    fake = FakeProvider()
    svc = TtsService(Synthesizer(fake), port=0, original_voices_dir=None)
    started = asyncio.Event()
    task = asyncio.create_task(svc.serve_forever(started))
    await started.wait()
    yield svc, fake
    task.cancel()
    await asyncio.gather(task, return_exceptions=True)


async def test_websocket_speak(service) -> None:
    svc, fake = service
    async with connect(f"ws://127.0.0.1:{svc.port}") as ws:
        await ws.send(
            json.dumps({"type": "speak", "id": "1", "text": "Драконы уже здесь.", "voice": {"id": "x"}})
        )
        start = json.loads(await ws.recv())
        assert start == {"type": "start", "id": "1", "sample_rate": 16000, "provider": "fake"}
        size = 0
        while isinstance(msg := await ws.recv(), bytes):
            size += len(msg)
        done = json.loads(msg)
        assert done["type"] == "done" and done["audio_s"] == pytest.approx(size / 2 / 16000)
        assert fake.calls[0][1] == Voice("x", "", None)

        await ws.send(json.dumps({"type": "speak", "id": "2", "text": "  "}))
        assert json.loads(await ws.recv())["type"] == "error"

        await ws.send(
            json.dumps(
                {"type": "speak", "id": "3", "text": "Вон!", "voice": {"id": "x"}, "style": "say angrily"}
            )
        )
        while not isinstance(msg := await ws.recv(), str) or json.loads(msg)["type"] != "done":
            pass
        assert fake.calls[1][1] == Voice("x", "", None, "say angrily")


async def test_original_voices_preserve_other_requests(service, tmp_path, caplog) -> None:
    svc, fake = service
    sample = tmp_path / "NONE_100_XARDAS.wav"
    sample.write_bytes(b"snapshot")
    (tmp_path / "manifest.json").write_text(
        json.dumps(
            [
                {"id": "NONE_100_XARDAS", "aliases": ["KDF_100_XARDAS"], "file": sample.name},
                {"id": "MISSING", "file": "missing.wav"},
            ]
        )
    )
    svc.original_voices = load_original_voices(tmp_path)
    assert len(svc.original_voices) == 2
    assert "MISSING skipped: no sample" in caplog.text
    original = Voice("NONE_100_XARDAS@original", style="say calmly", sample=str(sample))
    async with connect(f"ws://127.0.0.1:{svc.port}") as ws:
        for cid, preset, clones, enabled, expected in [
            ("none_100_xardas", None, True, True, original),
            ("kDf_100_XaRdAs", None, True, True, original),
            ("NONE_100_XARDAS", "Dmitry", True, True, None),
            ("unknown", None, True, True, None),
            ("KDF_100_XARDAS", None, False, True, None),
            ("NONE_100_XARDAS", None, True, False, None),
        ]:
            fake.supports_cloning = clones
            if not enabled:
                svc.original_voices = {}
            await ws.send(
                json.dumps(
                    {
                        "type": "speak", "id": "1", "text": "Да.",
                        "voice": {"id": cid, "description": "Old man", "preset": preset},
                        "style": "say calmly",
                    }
                )
            )
            while not isinstance(msg := await ws.recv(), str) or json.loads(msg)["type"] != "done":
                pass
            assert fake.calls[-1][1] == (expected or Voice(cid, "Old man", preset, "say calmly"))


def test_original_voices_missing_manifest_disables_once(tmp_path, caplog) -> None:
    caplog.set_level(logging.INFO, logger="gothic_tts.service")
    for directory in (tmp_path / "absent", tmp_path):
        caplog.clear()
        assert load_original_voices(directory) == {}
        assert len(caplog.records) == 1 and caplog.records[0].levelno == logging.INFO


async def test_health_shows_providers_left_out() -> None:
    synth = Synthesizer(FakeProvider(fail=True), FakeProvider())
    await _collect(synth, "Привет")  # the primary fails and waits for its check in the background
    svc = TtsService(synth, port=0)
    started = asyncio.Event()
    task = asyncio.create_task(svc.serve_forever(started))
    await started.wait()
    try:
        async with httpx.AsyncClient() as http:
            r = await http.get(f"http://127.0.0.1:{svc.port}/health")
        assert r.json() == {
            "ok": True,
            "service": "gothic-tts",
            "providers": [{"name": "fake", "ok": False}, {"name": "fake", "ok": True}],
        }
    finally:
        task.cancel()
        await asyncio.gather(task, return_exceptions=True)
        await synth.aclose()


async def test_voxcpm_client_against_a_stand_in_server() -> None:
    seen: list[dict] = []

    async def server(ws) -> None:
        async for raw in ws:
            msg = json.loads(raw)
            seen.append(msg)
            if msg["text"] == "сломайся":
                await ws.send(json.dumps({"type": "error", "id": msg["id"], "detail": "bad text"}))
                continue
            await ws.send(b"\1\0" * 480)
            await ws.send(b"\2\0" * 480)
            await ws.send(
                json.dumps({"type": "done", "id": msg["id"], "sample_rate": 48000, "audio_s": 0.02})
            )

    async with serve(server, "127.0.0.1", 0) as srv:
        port = srv.sockets[0].getsockname()[1]
        p = VoxCPM(url=f"ws://127.0.0.1:{port}", timeout=2)
        chunks: list[bytes] = []

        async def on_chunk(pcm: bytes) -> None:
            chunks.append(pcm)

        assert await p.synthesize("Привет", XARDAS, on_chunk) == pytest.approx(0.02)
        assert len(chunks) == 2
        with pytest.raises(TtsError, match="bad text"):
            await p.synthesize("сломайся", XARDAS, on_chunk)
        await p.synthesize("Вон!", Voice(XARDAS.id, XARDAS.description, style="say angrily"), on_chunk)
        primary = FakeProvider(fail=True)
        primary.supports_cloning = True
        synth = Synthesizer(primary, p)
        requested = Voice("KDF_100_Xardas", XARDAS.description, style="say calmly")
        original = Voice("NONE_100_XARDAS@original", requested.description, sample="snapshot.wav")

        async def fallback_chunk(pcm, provider) -> None:
            chunks.append(pcm)

        try:
            speech = await synth.speak("Да.", requested, fallback_chunk, original_voice=original)
            assert speech.provider == "voxcpm" and speech.audio_s == pytest.approx(0.02)
            assert primary.calls[-1][1] == original
        finally:
            await synth.aclose()
    assert seen[0]["voice"] == {"id": "xardas", "description": "An old man with a deep voice"}
    assert "style" not in seen[0] and seen[2]["style"] == "say angrily"
    assert seen[3]["voice"] == {"id": requested.id, "description": requested.description}
    assert seen[3]["style"] == requested.style


async def test_codex_provider_over_a_fake_app_server(tmp_path) -> None:
    import shlex
    import sys
    from pathlib import Path

    from gothic_tts.providers.codex import CodexRealtime

    log = tmp_path / "rpc.log"
    fake = Path(__file__).with_name("fake_codex.py")
    p = CodexRealtime(
        codex=f"{shlex.quote(sys.executable)} {shlex.quote(str(fake))} {shlex.quote(str(log))}",
        transport="websocket",
        timeout=5,
    )
    chunks: list[bytes] = []

    async def on_chunk(pcm: bytes) -> None:
        chunks.append(pcm)

    try:
        seconds = await p.synthesize("Привет", Voice("xardas", "An old man"), on_chunk)
        assert seconds == pytest.approx(6 * 0.02)  # 20 ms per character
        assert len(chunks) == 6
        assert await p.synthesize("Да", Voice("lady", "A young woman"), on_chunk) == pytest.approx(0.04)
    finally:
        await p.aclose()

    calls = [json.loads(line) for line in log.read_text().splitlines()]
    methods = [c["method"] for c in calls]
    assert methods[:4] == ["initialize", "initialized", "thread/start", "thread/realtime/start"]
    assert calls[0]["params"]["capabilities"]["experimentalApi"] is True
    start = calls[3]["params"]
    assert start["outputModality"] == "audio" and start["voice"] == "spruce" and start["version"] == "v3"
    assert "word for word" in start["prompt"]
    # another voice needs another realtime session
    restarts = [c["params"]["voice"] for c in calls if c["method"] == "thread/realtime/start"]
    assert restarts == ["spruce", "juniper"]
