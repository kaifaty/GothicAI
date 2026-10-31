import asyncio
import base64
import hashlib
import json
import logging
import os
import sys
from types import SimpleNamespace
from unittest.mock import AsyncMock

import httpx
import numpy as np
import pytest

from gothic_tts import cli, service
from gothic_tts.cli import load_env_file
from gothic_tts.providers import TtsError, Voice, inworld
from gothic_tts.providers.fake import FakeProvider
from gothic_tts.providers.inworld import CHECK_VOICE, PREVIEW_TEXTS, InworldTTS
from gothic_tts.service import TtsService
from gothic_tts.synthesizer import Synthesizer

XARDAS = Voice("NONE_100_XARDAS", "An old man with a deep voice")
NO_CREDITS = "You have no credits remaining. Please add credits to continue using the service."


class FakeInworld:
    """Stands in for api.inworld.ai: voice design and publishing, NDJSON speech streams."""

    def __init__(self) -> None:
        self.requests: list[tuple[str, str, dict]] = []
        self.voices: set[str] = {"Dmitry"}
        self.designed = 0
        self.cloned = 0
        self.sample_errors: list[str] = []
        self.fail_line = False
        self.credits = True
        self.odd_check = False  # the check line gets an answer the provider does not know

    def __call__(self, req: httpx.Request) -> httpx.Response:
        body = json.loads(req.content) if req.content else {}
        self.requests.append((req.method, req.url.path, body))
        if req.headers.get("authorization") != "Basic test-key":
            return httpx.Response(401, json={"code": 16, "message": "Unauthenticated"})
        match req.url.path:
            case "/tts/v1/voices":
                return httpx.Response(200, json={"voices": [{"voiceId": v} for v in self.voices]})
            case "/voices/v1/voices:design":
                self.designed += 1
                preview = f"ws__design-voice-{self.designed}"
                return httpx.Response(200, json={"previewVoices": [{"voiceId": preview, "previewAudio": ""}]})
            case "/voices/v1/voices:clone":
                self.cloned += 1
                voice_id = f"ws__clone-voice-{self.cloned}"
                if not self.sample_errors:
                    self.voices.add(voice_id)
                checked = {
                    "warnings": [{"text": "short sample"}],
                    "errors": [{"text": e} for e in self.sample_errors],
                }
                return httpx.Response(
                    200,
                    json={
                        "voice": {"voiceId": voice_id, "languageCode": "ru-RU", "source": "IVC"},
                        "audioSamplesValidated": [checked],
                    },
                )
            case path if path.endswith(":publish"):
                voice_id = path.removeprefix("/voices/v1/voices/").removesuffix(":publish")
                self.voices.add(voice_id)
                return httpx.Response(200, json={"voiceId": voice_id, "tags": body["tags"]})
            case "/tts/v1/voice:stream":
                if not self.credits:  # asked before the voice is looked up, as the real one does
                    return httpx.Response(
                        402, json={"error": {"code": 7, "message": NO_CREDITS, "details": []}}
                    )
                if self.odd_check and body["voiceId"] == CHECK_VOICE:
                    return httpx.Response(400, json={"code": 3, "message": "Invalid voice id"})
                if body["voiceId"] not in self.voices:
                    return httpx.Response(
                        404, json={"code": 5, "message": f"Unknown voice: {body['voiceId']}"}
                    )
                # odd-sized pieces: a sample is split between two lines
                pcm = b"\1\0" * 100
                lines = [
                    {"result": {"audioContent": base64.b64encode(p).decode()}} for p in (pcm[:51], pcm[51:])
                ]
                if self.fail_line:
                    lines.append({"error": {"code": 13, "message": "synthesis broke"}})
                return httpx.Response(200, content="\n".join(json.dumps(x) for x in lines).encode())
        return httpx.Response(404, json={"message": "no such path"})

    def streams(self) -> list[dict]:
        """Lines, without warmup's checks."""
        return [body for body in self._streams() if body["voiceId"] != CHECK_VOICE]

    def checks(self) -> int:
        return sum(body["voiceId"] == CHECK_VOICE for body in self._streams())

    def _streams(self) -> list[dict]:
        return [body for _, path, body in self.requests if path == "/tts/v1/voice:stream"]

    def voice_lists(self) -> int:
        return sum(path == "/tts/v1/voices" for _, path, _ in self.requests)


@pytest.fixture
def api(monkeypatch) -> FakeInworld:
    monkeypatch.setenv("INWORLD_API_KEY", "test-key")
    return FakeInworld()


def _provider(api: FakeInworld, tmp_path, **opts) -> InworldTTS:
    return InworldTTS(cache=tmp_path / "voices.json", transport=httpx.MockTransport(api), **opts)


@pytest.fixture
def original_base(tmp_path, monkeypatch):
    selected = tmp_path / "base"
    selected.mkdir()
    manifest = json.dumps({"format": "gothic-base-files", "version": 1, "files": []}).encode()
    (selected / "base-files.json").write_bytes(manifest)
    (selected / "base.json").write_text(json.dumps({
        "format": "gothic-base", "version": 2, "fingerprint": "a" * 64,
        "manifest": {
            "path": "base-files.json", "size": len(manifest),
            "sha256": hashlib.sha256(manifest).hexdigest(),
        },
    }))
    monkeypatch.setenv("OPENGOTHIC_BASE", str(selected))
    monkeypatch.setenv("OPENGOTHIC_BASE_CACHE", str(tmp_path / "cache"))
    return selected


async def _speak(p: InworldTTS, voice: Voice = XARDAS) -> tuple[bytes, float]:
    parts: list[bytes] = []

    async def on_chunk(pcm: bytes) -> None:
        assert len(pcm) % 2 == 0
        parts.append(pcm)

    seconds = await p.synthesize("Ты снова здесь.", voice, on_chunk)
    return b"".join(parts), seconds


async def test_voice_designed_once_then_reused(api, tmp_path) -> None:
    p = _provider(api, tmp_path)
    await p.warmup()
    pcm, seconds = await _speak(p)
    assert pcm == b"\1\0" * 100 and seconds == pytest.approx(100 / 24000)
    await _speak(p)
    await p.aclose()

    assert api.designed == 1
    design = next(body for _, path, body in api.requests if path == "/voices/v1/voices:design")
    assert design["designPrompt"] == XARDAS.description and design["languageCode"] == "ru-RU"
    first, second = api.streams()
    assert first["voiceId"] == second["voiceId"] == "ws__design-voice-1"
    assert first["audioConfig"] == {"audioEncoding": "PCM", "sampleRateHertz": 24000}
    assert "instruction" not in first  # the description is already in the designed voice

    # the next run remembers it too; a new description is a new voice
    again = _provider(api, tmp_path)
    await _speak(again)
    assert api.designed == 1
    await _speak(again, Voice(XARDAS.id, "A young woman"))
    assert api.designed == 2
    await again.aclose()


async def test_voice_designed_in_its_language(api, tmp_path) -> None:
    """An English voice (the shorts' dub) is designed on an English script; the language is part of the
    voice, so the same id in another language is designed again."""
    english = _provider(api, tmp_path, language="en-US")
    await _speak(english)
    russian = _provider(api, tmp_path)
    await _speak(russian)
    for p in (english, russian):
        await p.aclose()
    en, ru = [body for _, path, body in api.requests if path == "/voices/v1/voices:design"]
    assert en["languageCode"] == "en-US" and en["previewText"] == PREVIEW_TEXTS["en"]
    assert ru["languageCode"] == "ru-RU" and ru["previewText"] == PREVIEW_TEXTS["ru"]
    first, second = api.streams()
    assert first["language"] == "en-US" and second["language"] == "ru-RU"


async def test_voice_cloned_from_a_recording_speaks_the_providers_language(api, tmp_path) -> None:
    """The shorts' English dub: a Russian recording cloned once, its lines said in English; another
    recording is another clone; a refused sample is an error, not a designed voice."""
    sample = tmp_path / "me.wav"
    sample.write_bytes(b"RIFF me")
    me = Voice("USER@bow", "the user", sample=str(sample))
    p = _provider(api, tmp_path, language="en-US")
    await _speak(p, me)
    await _speak(p, me)
    assert (api.cloned, api.designed) == (1, 0)
    clone = next(body for _, path, body in api.requests if path == "/voices/v1/voices:clone")
    assert base64.b64decode(clone["voiceSamples"][0]["audioData"]) == b"RIFF me"
    assert "languageCode" not in clone  # Inworld hears the recording's language itself
    first, second = api.streams()
    assert first["voiceId"] == second["voiceId"] == "ws__clone-voice-1" and first["language"] == "en-US"

    sample.write_bytes(b"RIFF me again")
    await _speak(p, me)
    assert api.cloned == 2
    api.sample_errors = ["no speech"]
    sample.write_bytes(b"RIFF noise")
    with pytest.raises(TtsError, match="no speech"):
        await _speak(p, me)
    with pytest.raises(TtsError, match="no sample"):
        await _speak(p, Voice("x", sample=str(tmp_path / "missing.wav")))
    await p.aclose()


@pytest.mark.parametrize(("headers", "detail", "delay"), [
    ({"Retry-After": "13"}, "Please try again in 99 seconds.", 14),
    ({"Retry-After": "Sat, 01 Jan 2000 00:00:13 GMT"}, "Rate limited", 14),
    ({}, "Please try again in 13 seconds, or upgrade your plan.", 14),
    ({}, "Rate limited", 31),
])
async def test_clone_waits_then_repeats_the_same_request(api, tmp_path, monkeypatch, headers, detail, delay):
    sample = tmp_path / "sample.wav"
    sample.write_bytes(b"RIFF original")
    requests, sleeps = [], []

    def transport(request):
        requests.append(request.content)
        if len(requests) == 1:
            return httpx.Response(429, headers=headers, json={"code": 8, "message": detail})
        return api(request)

    async def sleep(seconds):
        assert not p.cache.exists()  # a rate-limited attempt must never become a cached clone
        sleeps.append(seconds)

    monkeypatch.setattr(inworld.time, "time", lambda: 946684800)
    monkeypatch.setattr(inworld.asyncio, "sleep", sleep)
    p = _provider(transport, tmp_path, keep_warm=0)
    try:
        assert await p.clone(Voice("ORIGINAL", sample=str(sample))) == "ws__clone-voice-1"
        assert sleeps == [delay] and len(requests) == 2 and requests[0] == requests[1]
        assert json.loads(p.cache.read_text())["ORIGINAL"]["voice_id"] == "ws__clone-voice-1"
    finally:
        await p.aclose()


async def test_clone_stops_after_six_rate_limited_attempts(api, tmp_path, monkeypatch):
    sample = tmp_path / "sample.wav"
    sample.write_bytes(b"RIFF original")
    attempts, sleeps = [], []

    def transport(request):
        attempts.append(request.content)
        return httpx.Response(429, text="Please try again in 13 seconds.")

    async def sleep(seconds):
        sleeps.append(seconds)

    monkeypatch.setattr(inworld.asyncio, "sleep", sleep)
    p = _provider(transport, tmp_path, keep_warm=0)
    try:
        with pytest.raises(TtsError) as failed:
            await p.clone(Voice("ORIGINAL", sample=str(sample)))
        assert str(failed.value) == "inworld returned 429: Please try again in 13 seconds."
        assert len(attempts) == 6 and len(set(attempts)) == 1 and sleeps == [14] * 5
        assert not p.cache.exists()
    finally:
        await p.aclose()


async def test_original_voice_cache_preserves_the_designed_voice(api, tmp_path, monkeypatch) -> None:
    sample = tmp_path / "xardas.wav"
    sample.write_bytes(b"RIFF Xardas")
    (tmp_path / "manifest.json").write_text(json.dumps([
        {"id": XARDAS.id, "aliases": ["OTHER_XARDAS"], "file": sample.name},
    ]))
    p = _provider(api, tmp_path, keep_warm=0)
    synth = Synthesizer(p)
    designed = TtsService(synth, original_voices_dir=None)
    await p.clone(Voice(f"{XARDAS.id}@original", sample=str(sample)))
    original = TtsService(synth, original_voices_dir=tmp_path)

    def prepare(*args, **kwargs):
        raise AssertionError("prepared snapshots must work without a game directory")

    monkeypatch.setattr(service.originals, "prepare", prepare)
    await original._prepare_originals()
    assert original.original_voices and api.cloned == 1
    messages = []

    async def send(message):
        messages.append(message)

    conn = SimpleNamespace(send=send)
    request = {"id": "1", "text": "Да.", "voice": {"id": XARDAS.id, "description": XARDAS.description}}
    try:
        await designed._speak(conn, request)
        cached = json.loads(p.cache.read_text())[XARDAS.id]
        await original._speak(conn, request)
        request["voice"]["id"] = "other_xardas"
        await original._speak(conn, request)
        request["voice"]["id"] = XARDAS.id
        await designed._speak(conn, request)
    finally:
        await synth.aclose()
    assert (api.designed, api.cloned) == (1, 1)
    assert [line["voiceId"] for line in api.streams()] == [
        "ws__design-voice-1", "ws__clone-voice-1", "ws__clone-voice-1", "ws__design-voice-1",
    ]
    entries = json.loads(p.cache.read_text())
    assert entries[XARDAS.id] == cached
    assert entries[f"{XARDAS.id}@original"]["voice_id"] == "ws__clone-voice-1"
    assert sum(isinstance(message, bytes) for message in messages) == 8


async def test_original_preparation_never_delays_a_game_line(api, tmp_path, monkeypatch) -> None:
    import threading

    sample = tmp_path / "xardas.wav"
    sample.write_bytes(b"RIFF Xardas")
    (tmp_path / "manifest.json").write_text(json.dumps({"characters": [
        {"id": XARDAS.id, "aliases": ["OTHER_XARDAS"], "file": sample.name},
    ]}))
    snapshots_started, clone_started, finish_clone = asyncio.Event(), asyncio.Event(), asyncio.Event()
    finish_snapshots = threading.Event()
    loop = asyncio.get_running_loop()

    def prepare(*args, progress):
        loop.call_soon_threadsafe(snapshots_started.set)
        assert finish_snapshots.wait(5)
        progress(36, 36)
        return tmp_path / "manifest.json"

    async def transport(request):
        if request.url.path == "/voices/v1/voices:clone":
            clone_started.set()
            await finish_clone.wait()
        return api(request)

    monkeypatch.setattr(service.originals, "prepare", prepare)
    p = _provider(transport, tmp_path, keep_warm=0)
    synth = Synthesizer(p)
    svc = TtsService(synth, port=0, original_voices_dir=tmp_path, game_dir=tmp_path / "game")
    listening = asyncio.Event()
    server = asyncio.create_task(svc.serve_forever(listening))
    request = {"id": "1", "text": "Да.", "voice": {"id": XARDAS.id, "description": XARDAS.description}}
    messages = []

    async def send(message):
        messages.append(message)

    conn = SimpleNamespace(send=send)
    try:
        await listening.wait()
        await asyncio.wait_for(snapshots_started.wait(), 2)
        await asyncio.wait_for(svc._speak(conn, request), 0.5)
        assert api.cloned == 0 and api.streams()[-1]["voiceId"] == "ws__design-voice-1"
        finish_snapshots.set()
        await asyncio.wait_for(clone_started.wait(), 2)
        await asyncio.wait_for(svc._speak(conn, request), 0.5)
        assert not finish_clone.is_set() and api.streams()[-1]["voiceId"] == "ws__design-voice-1"
        finish_clone.set()
        for _ in range(100):
            if svc.original_voices:
                break
            await asyncio.sleep(0.01)
        request["voice"]["id"] = "other_xardas"
        await svc._speak(conn, request)
        assert api.streams()[-1]["voiceId"] == "ws__clone-voice-1"
        assert "instruction" not in api.streams()[-1]
        request["style"] = "say calmly"
        await svc._speak(conn, request)
        assert api.streams()[-1]["instruction"] == "say calmly"
        assert api.cloned == 1
        # A deleted clone falls back to its designed voice without disabling Inworld or recloning.
        api.voices.discard("ws__clone-voice-1")
        await svc._speak(conn, request)
        assert json.loads(messages[-1])["type"] == "done"
        assert api.streams()[-1]["voiceId"] == "ws__design-voice-2"
        assert synth.available() == [p] and api.cloned == 1
        assert not svc.original_voices  # remove all aliases of the deleted clone
        assert f"{XARDAS.id}@original" not in json.loads(p.cache.read_text())
        deleted_requests = sum(line["voiceId"] == "ws__clone-voice-1" for line in api.streams())
        request["voice"]["id"] = XARDAS.id
        await svc._speak(conn, request)
        assert json.loads(messages[-1])["type"] == "done"
        assert api.streams()[-1]["voiceId"] == "ws__design-voice-1"
        assert sum(line["voiceId"] == "ws__clone-voice-1" for line in api.streams()) == deleted_requests
        request["voice"]["id"] = "diego"
        await svc._speak(conn, request)
        assert json.loads(messages[-1])["type"] == "done"
        assert api.streams()[-1]["voiceId"] == "ws__design-voice-3"
        assert synth.available() == [p] and api.cloned == 1
    finally:
        finish_snapshots.set()
        finish_clone.set()
        server.cancel()
        await asyncio.gather(server, return_exceptions=True)
        await synth.aclose()


@pytest.mark.parametrize("changed", [False, True])
async def test_cached_originals_are_ready_before_source_export_finishes(
    api, tmp_path, monkeypatch, original_base, changed
):
    import threading

    source = service.originals
    game, out = tmp_path / "game", tmp_path / "original"
    game.mkdir()
    monkeypatch.setenv("STORYGRAPH_TOOL", __file__)
    monkeypatch.setattr(source, "CHARACTERS", {XARDAS.id: source.CHARACTERS[XARDAS.id]})
    monkeypatch.setattr(source, "speech_index", lambda game: {})
    messages = [["DIA_XARDAS_TEST_14_00.WAV", "Synthetic line."]]
    export_started, release_export = asyncio.Event(), threading.Event()
    loop = asyncio.get_running_loop()
    blocked = False

    def export(*args, **kwargs):
        if blocked:
            loop.call_soon_threadsafe(export_started.set)
            assert release_export.wait(5)
        return SimpleNamespace(stdout=json.dumps({"messages": messages}).encode())

    built = []

    def build(cid, index, texts, directory):
        built.append(cid)
        sample = directory / f"{cid}.wav"
        sample.write_bytes(f"RIFF synthetic {texts[messages[0][0][:-4]]}".encode())
        return {"id": cid, "aliases": ["OTHER_XARDAS"], "file": str(sample)}

    monkeypatch.setattr(source.subprocess, "run", export)
    monkeypatch.setattr(source, "build", build)
    source.prepare(game, out)
    p = _provider(api, tmp_path, keep_warm=0)
    await p.clone(Voice(f"{XARDAS.id}@original", sample=str(out / f"{XARDAS.id}.wav")))
    requests = len(api.requests)
    if changed:
        messages[0][1] = "Changed synthetic text."
    metadata = json.loads((original_base / "base.json").read_text())
    metadata["fingerprint"] = "b" * 64
    (original_base / "base.json").write_text(json.dumps(metadata))
    blocked = True
    synth = Synthesizer(p)
    svc = TtsService(synth, original_voices_dir=out, game_dir=game)
    preparation = asyncio.create_task(svc._prepare_originals())
    spoken = []

    async def send(message):
        spoken.append(message)

    try:
        await asyncio.wait_for(export_started.wait(), 2)
        assert not release_export.is_set() and not preparation.done()
        assert svc.original_voices[XARDAS.id.casefold()].preset == "ws__clone-voice-1"
        assert svc.original_voices["other_xardas"].preset == "ws__clone-voice-1"
        assert len(api.requests) == requests  # the fast path only reads the cache
        await svc._speak(SimpleNamespace(send=send), {
            "id": "1", "text": "Да.", "voice": {"id": XARDAS.id, "description": XARDAS.description},
        })
        assert json.loads(spoken[-1])["type"] == "done"
        assert api.streams()[-1]["voiceId"] == "ws__clone-voice-1"
        release_export.set()
        await asyncio.wait_for(preparation, 2)
        assert built == [XARDAS.id] * 2
        assert api.cloned == (2 if changed else 1)
        expected = "ws__clone-voice-2" if changed else "ws__clone-voice-1"
        assert svc.original_voices["other_xardas"].preset == expected
    finally:
        release_export.set()
        await asyncio.gather(preparation, return_exceptions=True)
        await synth.aclose()


async def test_base_snapshot_rebuild_reuses_all_original_clones(
    api, tmp_path, monkeypatch, original_base
):
    source = service.originals
    game, out = tmp_path / "game", tmp_path / "original"
    game.mkdir()
    # Use every pinned character and the real build/prepare pipeline, with synthetic speech only.
    lines = {line for selected in source.ORIGINAL_LINES.values() for line in selected}
    index = {line: (None, line) for line in lines}
    monkeypatch.setattr(source, "speech_index", lambda selected: index)
    monkeypatch.setattr(source, "_texts", lambda selected, ou: dict.fromkeys(lines, "Synthetic speech."))
    monkeypatch.setattr(source, "RATE", 100)
    monkeypatch.setattr(source, "decode", lambda *_: np.tile(np.array([-0.125, 0.125], np.float32), 100))
    manifest = source.prepare(game, out)
    previous = json.loads(manifest.read_text())
    entries = previous["characters"]
    assert len(entries) == len(source.CHARACTERS) == 36
    assert all("error" not in entry for entry in entries)
    before = {entry["id"]: (out / f"{entry['id']}.wav").read_bytes() for entry in entries}
    # Simulate the installation-era manifest and the existing Inworld clone cache.
    previous["algorithm_version"] = 1
    previous["source"] = {"game_dir": str(game), "speech": [{"name": "Data/Speech.vdf"}]}
    manifest.write_text(json.dumps(previous))
    p = _provider(api, tmp_path, keep_warm=0)
    p.cache.write_text(json.dumps({
        f"{cid}@original": {
            "voice_id": f"cached-{cid}", "sample": hashlib.sha1(sample).hexdigest()[:16],
        }
        for cid, sample in before.items()
    }))
    clone = AsyncMock(side_effect=AssertionError("a matching sample must not call clone"))
    monkeypatch.setattr(p, "clone", clone)
    real_build, built = source.build, []

    def build(cid, *args):
        built.append(cid)
        return real_build(cid, *args)

    monkeypatch.setattr(source, "build", build)
    synth = Synthesizer(p)
    svc = TtsService(synth, original_voices_dir=out, game_dir=game)
    try:
        await svc._prepare_originals()
        assert built == list(source.CHARACTERS)
        assert json.loads(manifest.read_text())["source"] == {"base_fingerprint": "a" * 64}
        for cid, sample in before.items():
            assert (out / f"{cid}.wav").read_bytes() == sample
            assert svc.original_voices[cid.casefold()].preset == f"cached-{cid}"
            voice = Voice(f"{cid}@original", sample=str(out / f"{cid}.wav"))
            assert p.cached_clone(voice) == f"cached-{cid}"
        clone.assert_not_called()
        assert api.requests == [] and api.cloned == 0
    finally:
        await synth.aclose()


async def test_background_clones_survive_every_third_request_rate_limit(api, tmp_path, monkeypatch, caplog):
    characters = [f"NPC_{i}" for i in range(36)]
    for cid in characters:
        (tmp_path / f"{cid}.wav").write_bytes(f"RIFF {cid}".encode())
    (tmp_path / "manifest.json").write_text(json.dumps([
        {"id": cid, "file": f"{cid}.wav"} for cid in characters
    ]))
    attempts, sleeps = [], []

    def transport(request):
        attempts.append(request.content)
        if len(attempts) % 3 == 0:
            return httpx.Response(429, json={"message": "Please try again in 13 seconds."})
        return api(request)

    async def sleep(seconds):
        sleeps.append(seconds)

    monkeypatch.setattr(inworld.asyncio, "sleep", sleep)
    caplog.set_level(logging.INFO, logger="gothic_tts")
    p = _provider(transport, tmp_path, keep_warm=0)
    synth = Synthesizer(p)
    svc = TtsService(synth, original_voices_dir=tmp_path)
    try:
        await svc._prepare_originals()
        assert api.cloned == 36 and len(attempts) == 53 and sleeps == [14] * 17
        assert set(svc.original_voices) == {cid.casefold() for cid in characters}
        assert len(json.loads(p.cache.read_text())) == 36
        assert all(attempts[i] == attempts[i + 1] for i in range(2, len(attempts), 3))
        assert caplog.text.count("original voices: Inworld rate limit, waiting 14 s") == 17
        assert "original voices: 36/36 ready, 0 failed" in caplog.text
        assert not any(r.levelno >= logging.WARNING and r.name == service.__name__ for r in caplog.records)
    finally:
        await synth.aclose()


@pytest.mark.parametrize("failure", ["network", "empty_voice", "refused", 401, 402, 403, 503])
async def test_background_clones_continue_and_retry_only_next_start(
    api, tmp_path, monkeypatch, caplog, failure
):
    for cid in ("FIRST", "BAD", "LAST"):
        (tmp_path / f"{cid}.wav").write_bytes(f"RIFF {cid}".encode())
    (tmp_path / "manifest.json").write_text(json.dumps({"characters": [
        {"id": cid, "file": f"{cid}.wav"} for cid in ("FIRST", "BAD", "LAST")
    ]}))
    monkeypatch.setattr(service.originals, "prepare", lambda *args, **kwargs: tmp_path / "manifest.json")
    attempts = []
    failing = True

    def transport(request):
        body = json.loads(request.content)
        attempts.append(body["displayName"])
        if failing and body["displayName"] == "gothic-ai BAD@original":
            if failure == "network":
                raise httpx.ReadTimeout("test timeout", request=request)
            if isinstance(failure, int):
                return httpx.Response(failure, text="test rejection")
            if failure == "refused":
                return httpx.Response(200, json={
                    "audioSamplesValidated": [{"errors": [{"text": "no speech"}]}],
                })
            return httpx.Response(200, json={"voice": {"voiceId": ""}})
        return api(request)

    p = _provider(transport, tmp_path, keep_warm=0)
    synth = Synthesizer(p)
    svc = TtsService(synth, original_voices_dir=tmp_path, game_dir=tmp_path / "game")
    try:
        await svc._prepare_originals()
        assert attempts == ["gothic-ai FIRST@original", "gothic-ai BAD@original", "gothic-ai LAST@original"]
        assert set(svc.original_voices) == {"first", "last"}
        assert "original voices: BAD:" in caplog.text
        if isinstance(failure, int):
            expected = (
                f"inworld rejected the API key ({failure})" if failure in (401, 403)
                else f"inworld returned {failure}"
            )
            assert f"{expected}: test rejection" in caplog.text
        assert set(json.loads(p.cache.read_text())) == {"FIRST@original", "LAST@original"}
        failing = False
        # A new service run retries the failure, while successful sample hashes produce no requests.
        again = TtsService(synth, original_voices_dir=tmp_path, game_dir=tmp_path / "game")
        await again._prepare_originals()
        assert attempts[-1] == "gothic-ai BAD@original" and len(attempts) == 4
        assert set(again.original_voices) == {"first", "bad", "last"}
    finally:
        await synth.aclose()


@pytest.mark.parametrize("provider_kind", ["disabled", "no_key", "other_provider"])
async def test_background_preparation_gates(api, tmp_path, monkeypatch, provider_kind):
    preparations = []

    def prepare(*args, **kwargs):
        preparations.append(True)
        (tmp_path / "manifest.json").write_text("[]")

    monkeypatch.setattr(service.originals, "prepare", prepare)
    p = _provider(api, tmp_path, keep_warm=0)
    if provider_kind == "no_key":
        monkeypatch.delenv("INWORLD_API_KEY")
    synth = Synthesizer(FakeProvider() if provider_kind == "other_provider" else p)
    svc = TtsService(
        synth, port=0, game_dir=tmp_path / "game",
        original_voices_dir=None if provider_kind == "disabled" else tmp_path,
    )
    started = asyncio.Event()
    task = asyncio.create_task(svc.serve_forever(started))
    try:
        await started.wait()
        await asyncio.sleep(0.05)
        assert preparations == ([] if provider_kind == "disabled" else [True])
        assert api.requests == []
    finally:
        task.cancel()
        await asyncio.gather(task, return_exceptions=True)
        await synth.aclose()
        if provider_kind == "other_provider":
            await p.aclose()


def test_no_original_voices_flag_skips_snapshots_and_clones(api, tmp_path, monkeypatch):
    def prepare(*args, **kwargs):
        raise AssertionError("disabled preparation ran")

    class OneShotService(TtsService):
        async def serve_forever(self):
            await self._prepare_originals()

    monkeypatch.setattr(service.originals, "prepare", prepare)
    monkeypatch.setattr(cli, "TtsService", OneShotService)
    monkeypatch.setattr(cli, "_synthesizer", lambda args: Synthesizer(_provider(api, tmp_path, keep_warm=0)))
    monkeypatch.setattr(cli, "load_env_file", lambda: None)
    monkeypatch.setattr(sys, "argv", [
        "gothic-tts", "serve", "--game-dir", str(tmp_path / "game"),
        "--no-original-voices", "--no-warmup",
    ])
    with pytest.raises(SystemExit) as stopped:
        cli.main()
    assert stopped.value.code == 0 and api.requests == []


async def test_rate_limited_original_clone_does_not_delay_other_voices(api, tmp_path, monkeypatch) -> None:
    sample = tmp_path / "original.wav"
    sample.write_bytes(b"RIFF original voice")
    original = Voice(f"{XARDAS.id}@original", sample=str(sample))
    cached_clone = Voice("VATRAS@original", sample=str(sample))
    waiting, resume = asyncio.Event(), asyncio.Event()
    attempts = []

    def transport(request):
        if request.url.path == "/voices/v1/voices:clone":
            body = json.loads(request.content)
            if body["displayName"] == f"gothic-ai {original.id}":
                attempts.append(request.content)
                if len(attempts) == 1:
                    return httpx.Response(429, headers={"Retry-After": "13"})
        return api(request)

    async def sleep(seconds):
        assert seconds == 14
        waiting.set()
        await resume.wait()

    monkeypatch.setattr(inworld.asyncio, "sleep", sleep)
    p = _provider(transport, tmp_path, keep_warm=0)
    pending = None
    try:
        await _speak(p)  # the already designed voice must keep speaking during the clone
        await p.clone(cached_clone)
        pending = asyncio.create_task(_speak(p, original))
        await asyncio.wait_for(waiting.wait(), 2)
        for voice in (XARDAS, cached_clone, Voice("NEW_NPC", "A quiet young man")):
            first_chunk = asyncio.Event()

            async def on_chunk(pcm, first_chunk=first_chunk):
                if pcm and not first_chunk.is_set():
                    assert not resume.is_set() and not pending.done()
                    first_chunk.set()

            await asyncio.wait_for(p.synthesize("Да.", voice, on_chunk), 0.5)
            assert first_chunk.is_set()
        resume.set()
        await pending
        assert len(attempts) == 2 and attempts[0] == attempts[1]
        assert (api.designed, api.cloned) == (2, 2)
    finally:
        if pending is not None:
            pending.cancel()
            await asyncio.gather(pending, return_exceptions=True)
        await p.aclose()


@pytest.mark.parametrize("state", ["missing_dir", "missing_manifest", "empty_manifest"])
def test_clone_originals_requires_voices(api, tmp_path, monkeypatch, capsys, state) -> None:
    directory = tmp_path / "missing" if state == "missing_dir" else tmp_path
    if state == "empty_manifest":
        (directory / "manifest.json").write_text("[]")
    monkeypatch.setattr(cli, "InworldTTS", lambda: _provider(api, tmp_path, keep_warm=0))
    monkeypatch.setattr(cli, "load_env_file", lambda: None)
    monkeypatch.setattr(sys, "argv", ["gothic-tts", "clone-originals", "--dir", str(directory)])
    with pytest.raises(SystemExit) as failed:
        cli.main()
    assert failed.value.code == 1
    output = capsys.readouterr()
    assert output.out == ""
    assert output.err == (
        f"error: no original voices in {directory} (run scripts/voice_snapshots.py)\n"
    )
    assert api.requests == []


def test_clone_originals_command_reuses_sample_hash(api, tmp_path, monkeypatch, capsys) -> None:
    for cid in ("XARDAS", "VATRAS"):
        (tmp_path / f"{cid}.wav").write_bytes(f"RIFF {cid}".encode())
    (tmp_path / "manifest.json").write_text(json.dumps([
        {"id": cid, "aliases": [f"{cid}_DI"], "file": f"{cid}.wav"} for cid in ("XARDAS", "VATRAS")
    ]))
    attempts, sleeps = [], []

    def transport(request):
        attempts.append(request.content)
        if len(attempts) == 1:
            return httpx.Response(429, headers={"Retry-After": "13"})
        return api(request)

    async def sleep(seconds):
        sleeps.append(seconds)

    monkeypatch.setattr(inworld.asyncio, "sleep", sleep)
    monkeypatch.setattr(cli, "InworldTTS", lambda: _provider(transport, tmp_path, keep_warm=0))
    monkeypatch.setattr(cli, "load_env_file", lambda: None)
    monkeypatch.setattr(sys, "argv", ["gothic-tts", "clone-originals", "--dir", str(tmp_path)])
    with pytest.raises(SystemExit) as first:
        cli.main()
    assert first.value.code == 0 and api.cloned == 2
    assert sleeps == [14] and len(attempts) == 3 and attempts[0] == attempts[1]
    assert capsys.readouterr().out.splitlines() == [
        "XARDAS → ws__clone-voice-1", "VATRAS → ws__clone-voice-2",
    ]
    requests = len(api.requests)
    with pytest.raises(SystemExit) as repeated:
        cli.main()
    assert repeated.value.code == 0 and len(api.requests) == requests
    assert api.designed == 0 and api.streams() == []

    (tmp_path / "XARDAS.wav").write_bytes(b"RIFF Xardas again")
    monkeypatch.setattr(sys, "argv", [
        "gothic-tts", "clone-originals", "--dir", str(tmp_path), "xardas_di", "XARDAS",
    ])
    capsys.readouterr()
    with pytest.raises(SystemExit) as changed:
        cli.main()
    assert changed.value.code == 0 and api.cloned == 3
    assert capsys.readouterr().out == "XARDAS → ws__clone-voice-3\n"


@pytest.mark.parametrize("failure", ["network", "empty_voice"])
def test_clone_originals_continues_after_failed_voice(api, tmp_path, monkeypatch, capsys, failure) -> None:
    for cid in ("FIRST", "BAD", "LAST"):
        (tmp_path / f"{cid}.wav").write_bytes(f"RIFF {cid}".encode())
    (tmp_path / "manifest.json").write_text(json.dumps([
        {"id": cid, "aliases": [], "file": f"{cid}.wav"} for cid in ("FIRST", "BAD", "LAST")
    ]))

    def transport(request):
        body = json.loads(request.content)
        if body["displayName"] == "gothic-ai BAD@original":
            if failure == "network":
                raise httpx.ReadTimeout("test timeout", request=request)
            return httpx.Response(200, json={"voice": {"voiceId": ""}})
        return api(request)

    monkeypatch.setattr(cli, "InworldTTS", lambda: _provider(transport, tmp_path, keep_warm=0))
    monkeypatch.setattr(cli, "load_env_file", lambda: None)
    monkeypatch.setattr(sys, "argv", ["gothic-tts", "clone-originals", "--dir", str(tmp_path)])
    with pytest.raises(SystemExit) as failed:
        cli.main()
    assert failed.value.code == 1
    output = capsys.readouterr()
    assert output.out.splitlines() == ["FIRST → ws__clone-voice-1", "LAST → ws__clone-voice-2"]
    assert "error: BAD:" in output.err
    assert set(json.loads((tmp_path / "voices.json").read_text())) == {"FIRST@original", "LAST@original"}


async def test_library_voice_steered_by_the_description(api, tmp_path) -> None:
    p = _provider(api, tmp_path, voice="Dmitry")
    await _speak(p)
    flash = _provider(api, tmp_path, voice="Dmitry", model="inworld-tts-2-flash")
    await _speak(flash)
    await _speak(flash, Voice("x", "calm", preset="Dmitry"))
    for provider in (p, flash):
        await provider.aclose()

    assert api.designed == 0
    tts2, *flash_bodies = api.streams()
    assert tts2["voiceId"] == "Dmitry" and tts2["instruction"] == XARDAS.description
    assert all("instruction" not in body for body in flash_bodies)  # Flash has no steering


async def test_the_line_style_steers_any_voice(api, tmp_path) -> None:
    angry = Voice(XARDAS.id, XARDAS.description, style="say angrily with force.")
    designed = _provider(api, tmp_path)
    await _speak(designed, angry)
    await _speak(designed)
    library = _provider(api, tmp_path, voice="Dmitry")
    await _speak(library, angry)
    flash = _provider(api, tmp_path, voice="Dmitry", model="inworld-tts-2-flash")
    await _speak(flash, angry)
    for provider in (designed, library, flash):
        await provider.aclose()

    angry_designed, calm_designed, angry_library, angry_flash = api.streams()
    assert angry_designed["instruction"] == "say angrily with force"  # the voice itself is designed
    assert "instruction" not in calm_designed
    assert angry_library["instruction"] == f"{XARDAS.description}. say angrily with force"
    assert "instruction" not in angry_flash


async def test_vanished_voice_is_designed_again(api, tmp_path) -> None:
    p = _provider(api, tmp_path)
    await _speak(p)
    api.voices.discard("ws__design-voice-1")  # deleted in the portal, or another workspace's key
    pcm, _ = await _speak(p)
    await p.aclose()
    assert pcm and api.designed == 2
    assert json.loads((tmp_path / "voices.json").read_text())[XARDAS.id]["voice_id"] == "ws__design-voice-2"


async def test_errors(api, tmp_path, monkeypatch) -> None:
    p = _provider(api, tmp_path, voice="Dmitry")
    api.fail_line = True
    with pytest.raises(TtsError, match="synthesis broke"):
        await _speak(p)
    monkeypatch.setenv("INWORLD_API_KEY", "wrong")
    with pytest.raises(TtsError, match="rejected the API key"):
        await p.warmup()
    monkeypatch.delenv("INWORLD_API_KEY")
    with pytest.raises(TtsError, match="INWORLD_API_KEY"):
        await _speak(p)
    await p.aclose()


@pytest.mark.parametrize("limited_voice_list", [False, True])
async def test_rate_limit_does_not_retry_speech_or_key_checks(api, tmp_path, monkeypatch, limited_voice_list):
    requests = []

    def transport(request):
        requests.append(request.url.path)
        if request.url.path == "/tts/v1/voice:stream" or limited_voice_list:
            return httpx.Response(429, headers={"Retry-After": "13"}, text="Rate limited")
        return api(request)

    async def sleep(seconds):
        pytest.fail("speech and key checks must not wait on rate limits")

    monkeypatch.setattr(inworld.asyncio, "sleep", sleep)
    p = _provider(transport, tmp_path, voice="Dmitry", keep_warm=0)
    try:
        with pytest.raises(TtsError, match="^inworld returned 429: Rate limited$"):
            await _speak(p)
        if limited_voice_list:
            with pytest.raises(TtsError, match="^inworld returned 429: Rate limited$"):
                await p.warmup()
        else:
            await p.warmup()  # the base behavior: an unknown check answer defers to the voice list
        assert requests == ["/tts/v1/voice:stream", "/tts/v1/voice:stream", "/tts/v1/voices"]
    finally:
        await p.aclose()


async def test_idle_connection_kept_warm(api, tmp_path, monkeypatch, caplog) -> None:
    p = _provider(api, tmp_path, voice="Dmitry", keep_warm=0.1)
    await p.warmup()
    for _ in range(8):  # lines keep the connection open themselves
        await _speak(p)
        await asyncio.sleep(0.02)
    assert api.checks() == 1

    caplog.set_level(logging.DEBUG, logger="gothic_tts")
    await asyncio.sleep(0.55)  # silence: a free check every 0.1 s
    assert 3 <= api.checks() - 1 <= 6
    monkeypatch.setenv("INWORLD_API_KEY", "wrong")
    before = api.checks()
    await asyncio.sleep(0.35)  # a failed request neither stops it nor logs above debug
    assert api.checks() - before >= 2
    assert caplog.records and all(r.levelno == logging.DEBUG for r in caplog.records)

    await p.aclose()
    stopped = api.checks()
    await asyncio.sleep(0.25)
    assert api.checks() == stopped  # stops with the provider

    monkeypatch.setenv("INWORLD_API_KEY", "test-key")
    cold = _provider(api, tmp_path, voice="Dmitry", keep_warm=0)
    await cold.warmup()
    await asyncio.sleep(0.1)
    await cold.aclose()
    assert api.checks() == stopped + 1


async def test_out_of_credits_stays_out_until_topped_up(api, tmp_path, caplog) -> None:
    """An account without credits still gets the free voice list, but every line is refused with 402.
    Lines must go straight to the fallback, not into a 402 after each check, and quietly."""
    caplog.set_level(logging.DEBUG, logger="gothic_tts")
    inworld, fallback = _provider(api, tmp_path, voice="Dmitry", keep_warm=0), FakeProvider()
    synth = Synthesizer(inworld, fallback, retry_s=0.02, retry_max_s=0.05)

    async def line() -> str:
        async def chunk(pcm: bytes, p) -> None:
            pass

        return (await synth.speak("Да.", XARDAS, chunk)).provider

    def warnings() -> int:
        return sum(r.levelno >= logging.WARNING for r in caplog.records)

    async def checked(n: int) -> None:  # the background check asked Inworld n more times
        target = api.checks() + n
        for _ in range(100):
            if api.checks() >= target:
                return
            await asyncio.sleep(0.01)
        raise AssertionError("no check")

    api.credits = False  # empty from the start: the fallback speaks the first line
    await synth.warmup()
    assert synth.available() == [fallback] and warnings() == 2  # inworld failed, left out
    for _ in range(3):
        assert await line() == "fake"
        await checked(2)
    assert api.streams() == [] and api.voice_lists() == 0  # no line and no voice list asked Inworld
    assert warnings() == 2  # checks that keep failing say nothing

    api.credits = True  # topped up: the next check finds out, for free
    for _ in range(100):
        if inworld in synth.available():
            break
        await asyncio.sleep(0.01)
    assert await line() == "inworld"
    assert len(api.streams()) == 1

    api.credits = False  # spent mid-game: one line pays the 402 round trip, then they skip it
    assert await line() == "fake"
    assert warnings() == 4  # inworld failed, left out: once
    for _ in range(3):
        await checked(2)
        assert await line() == "fake"
    assert len(api.streams()) == 2 and warnings() == 4
    await synth.aclose()


async def test_an_unknown_answer_to_the_check_leaves_it_to_the_voice_list(api, tmp_path) -> None:
    p = _provider(api, tmp_path, voice="Dmitry", keep_warm=0)
    api.odd_check = True  # Inworld changed: the check line is refused for something else
    await p.warmup()  # the voice list still decides, so Inworld is not left out for good
    assert api.checks() == 1 and api.voice_lists() == 1
    api.credits = False
    with pytest.raises(TtsError, match="no credits remaining"):
        await p.warmup()
    assert api.voice_lists() == 1
    await p.aclose()


def test_env_file_from_a_parent_directory(tmp_path, monkeypatch) -> None:
    (tmp_path / ".env").write_text("# keys\nGOTHIC_TEST_KEY='from file'\nexport GOTHIC_TEST_SET=file\n")
    nested = tmp_path / "tts-service" / "src"
    nested.mkdir(parents=True)
    monkeypatch.delenv("GOTHIC_TEST_KEY", raising=False)
    monkeypatch.setenv("GOTHIC_TEST_SET", "shell")
    assert load_env_file(nested) == tmp_path / ".env"
    assert os.environ["GOTHIC_TEST_KEY"] == "from file"
    assert os.environ["GOTHIC_TEST_SET"] == "shell"  # the shell's value wins
    monkeypatch.delenv("GOTHIC_TEST_KEY")
