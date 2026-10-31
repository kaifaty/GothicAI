import asyncio
import json
import math
import struct
from contextlib import asynccontextmanager

import pytest
from websockets.asyncio.server import serve

from gothic_ai.dialogue import DialogueManager
from gothic_ai.journal import Journal
from gothic_ai.llm.fake import FakeBackend
from gothic_ai.persona import PersonaProvider
from gothic_ai.talk import TalkDesk
from gothic_ai.voice import MIC_DENIED, AsrClient, VoiceInput
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import XARDAS, _until

XARDAS_NEAR = {
    "id": XARDAS,
    "name": "Ксардас",
    "distance": 300.0,
    "in_view": True,
    "state": "ZS_STAND_ARMSCROSSED",
}


class FakeSource:
    """Says its whole recording the moment the key goes down."""

    rate = 16000

    def __init__(self, pcm: bytes) -> None:
        self.pcm = pcm
        self.recording = False

    def start(self, on_chunk) -> None:
        self.recording = True
        for i in range(0, len(self.pcm), 3200):
            on_chunk(self.pcm[i : i + 3200])

    def stop(self) -> None:
        self.recording = False


def speech(seconds: float = 1.0) -> bytes:
    n = int(seconds * 16000)
    return struct.pack(f"<{n}h", *(int(8000 * math.sin(i / 10)) for i in range(n)))


@asynccontextmanager
async def fake_asr(text: str = "Дай мне зелье"):
    """ASR service stand-in: a partial per audio chunk, then `text` as the final."""
    utterances: list[int] = []

    async def handler(ws) -> None:
        await ws.send(json.dumps({"type": "hello", "service": "fake-asr"}))
        size = 0
        async for raw in ws:
            if isinstance(raw, bytes):
                size += len(raw)
                words = text.split()[: size // 3200]
                await ws.send(json.dumps({"type": "partial", "text": " ".join(words)}))
                continue
            msg = json.loads(raw)
            if msg["type"] == "start":
                size = 0
            elif msg["type"] == "end":
                utterances.append(size)
                await ws.send(json.dumps({"type": "final", "text": text, "provider": "fake"}))

    async with serve(handler, "127.0.0.1", 0) as server:
        port = server.sockets[0].getsockname()[1]
        yield f"ws://127.0.0.1:{port}", utterances


async def test_push_to_talk_anywhere_becomes_the_heros_phrase_to_whom_he_looks_at():
    fake = FakeGame()
    llm = FakeBackend("Держи.", delay=0)
    entries: list[dict] = []
    async with fake_asr() as (url, utterances), connected(fake) as link:
        journal = Journal(entries.append)
        dialogue = DialogueManager(link, llm, PersonaProvider(None), journal=journal)
        talk = TalkDesk(link, dialogue)
        voice = VoiceInput(link, talk.hear, FakeSource(speech()), AsrClient(url), journal=journal)
        await fake.roster(XARDAS_NEAR)
        await fake.send(type="ptt_start", focus=XARDAS)
        await _until(lambda: voice.active is not None and voice.active.focus == XARDAS)
        await fake.send(type="ptt_stop")
        await _until(lambda: fake.said == ["Держи."])

        assert fake.transcripts == ["Дай мне зелье"]
        assert (fake.heard[0]["to"], fake.heard[0]["to_name"]) == (XARDAS, "Ксардас")
        assert fake.partials[:2] == ["Дай", "Дай мне"]  # streamed while the key was held
        assert utterances == [32000]
        assert "recognizing" in fake.statuses
        assert llm.calls[-1][1][-1].content == "Дай мне зелье"
        player = entries[0]
        assert (player["kind"], player["text"], player["via"]) == ("player", "Дай мне зелье", "voice")
        assert player["name"] == "Ксардас" and "asr" in player and player["latency_s"] >= 0
        assert player["by"] == "focus"


async def test_short_tap_is_cancelled_without_recognition():
    fake = FakeGame()
    async with fake_asr() as (url, utterances), connected(fake) as link:
        source = FakeSource(speech())
        voice = VoiceInput(link, lambda *a: None, source, AsrClient(url))
        await fake.send(type="ptt_start")
        await _until(lambda: voice.active is not None)
        await fake.send(type="ptt_cancel")
        await _until(lambda: voice.active is None)
        await asyncio.sleep(0.1)
        assert not source.recording and utterances == [] and fake.transcripts == []


async def test_silent_microphone_and_missing_service_are_reported():
    fake = FakeGame()
    entries: list[dict] = []
    async with connected(fake) as link:
        voice = VoiceInput(
            link,
            lambda *a: None,
            FakeSource(bytes(32000)),
            AsrClient("ws://127.0.0.1:9"),
            journal=Journal(entries.append),
        )
        await fake.send(type="ptt_start")
        await _until(lambda: voice.active is not None)
        await fake.send(type="ptt_stop")
        await _until(lambda: fake.statuses[-1:] == ["error"])
        assert any(m.get("detail") == MIC_DENIED for m in fake.received if m["type"] == "chat_status")

        voice.source = FakeSource(speech())
        await fake.send(type="ptt_start")
        await fake.send(type="ptt_stop")
        await _until(lambda: sum(s == "error" for s in fake.statuses) == 2)
        assert "ASR-сервис недоступен" in fake.received[-1]["detail"]
        assert [e["kind"] for e in entries] == ["voice_error", "voice_error"]
        assert entries[0]["detail"] == MIC_DENIED


async def test_the_bench_says_a_recording_for_the_hero_only_when_files_are_allowed(tmp_path):
    import wave

    rec = tmp_path / "hero.wav"
    with wave.open(str(rec), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(speech(0.3))
    fake = FakeGame()
    heard: list[str] = []
    async with fake_asr("Я от Ли") as (url, utterances), connected(fake) as link:
        mic = FakeSource(speech())
        voice = VoiceInput(link, lambda text, *a: heard.append(text), mic, AsrClient(url), files=True)
        await fake.send(type="ptt_start", audio=str(rec))
        await _until(lambda: voice.active is not None)
        await asyncio.sleep(0.5)  # the recording is said at speaking pace
        await fake.send(type="ptt_stop")
        await _until(lambda: heard == ["Я от Ли"])
        assert not mic.recording and utterances == [9600]

        voice.files = False
        await fake.send(type="ptt_start", audio=str(rec))
        await _until(lambda: fake.statuses[-1:] == ["error"])
        assert voice.active is None and "--voice-files" in fake.received[-1]["detail"]


@asynccontextmanager
async def capturing_asr(*, drop_first: bool = False):
    starts: list[dict] = []
    recordings: list[bytes] = []

    async def handler(ws):
        pcm = b""
        async for raw in ws:
            if isinstance(raw, bytes):
                pcm += raw
                continue
            msg = json.loads(raw)
            if msg["type"] == "start":
                starts.append(msg)
                pcm = b""
            elif msg["type"] == "end":
                recordings.append(pcm)
                if drop_first and len(starts) == 1:
                    await ws.close()
                    return
                await ws.send(json.dumps({"type": "final", "text": "Привет", "provider": "fake"}))

    async with serve(handler, "127.0.0.1", 0) as server:
        port = server.sockets[0].getsockname()[1]
        yield f"ws://127.0.0.1:{port}", starts, recordings


@pytest.mark.parametrize("hints", [None, [], ["Ватрас", "Лотара", "Хориниса"]])
async def test_asr_start_includes_only_nonempty_hints(hints):
    async with capturing_asr() as (url, starts, _):
        asr = AsrClient(url)
        session = await asr.open(24000, hints=hints)
        await session.finish()
        expected = {"type": "start", "lang": "ru", "sample_rate": 24000, "format": "pcm16"}
        if hints:
            expected["hints"] = hints
        assert starts == [expected]
        await asr.aclose()


async def test_asr_restarts_with_the_same_hints_and_recording():
    async with capturing_asr(drop_first=True) as (url, starts, recordings):
        asr = AsrClient(url)
        hints = ["Ксардас", "Ксардаса"]
        session = await asr.open(16000, hints=hints)
        hints.append("Декстер")
        pcm = speech()
        await session.feed(pcm)
        assert (await session.finish()).text == "Привет"
        assert len(starts) == 2
        assert starts[0] == starts[1]
        assert starts[0]["hints"] == ["Ксардас", "Ксардаса"]
        assert recordings == [pcm, pcm]
        await asr.aclose()


async def test_voice_without_a_hint_source_omits_hints():
    fake = FakeGame()
    heard = []
    async with capturing_asr() as (url, starts, _), connected(fake) as link:
        asr = AsrClient(url)
        voice = VoiceInput(link, lambda *args: heard.append(args), FakeSource(speech()), asr)
        await fake.send(type="ptt_start", focus=XARDAS)
        await _until(lambda: voice.active is not None)
        await fake.send(type="ptt_stop")
        await _until(lambda: len(heard) == 1)
        assert len(starts) == 1 and "hints" not in starts[0]
        await asr.aclose()


async def test_voice_snapshots_hints_once_per_start_with_the_heros_focus():
    fake = FakeGame()
    heard, focuses = [], []
    names = ["Ксардас", "Ксардаса"]

    def hints(focus):
        focuses.append(focus)
        return names

    async with capturing_asr() as (url, starts, _), connected(fake) as link:
        asr = AsrClient(url)
        voice = VoiceInput(
            link, lambda *args: heard.append(args), FakeSource(speech()), asr, hints=hints
        )
        voice.start(XARDAS.lower())
        assert focuses == [XARDAS]
        names[:] = ["Ватрас"]  # the roster may change before the streaming task starts
        voice.stop(recognize=True, focus="MIL_LOTHAR")
        await _until(lambda: len(heard) == 1)
        assert starts[0]["hints"] == ["Ксардас", "Ксардаса"]
        assert heard[0][1] == "MIL_LOTHAR"
        assert focuses == [XARDAS]

        voice.start()
        voice.stop(recognize=True)
        await _until(lambda: len(heard) == 2)
        assert focuses == [XARDAS, None]
        assert starts[1]["hints"] == ["Ватрас"]
        await asr.aclose()
