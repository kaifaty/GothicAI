import asyncio
import base64
import json
from contextlib import asynccontextmanager

import pytest
from websockets.asyncio.server import serve

from gothic_ai.dialogue import DialogueManager
from gothic_ai.llm import ToolCall
from gothic_ai.llm.fake import FakeBackend
from gothic_ai.memory import MemoryStore
from gothic_ai.persona import MOOD_DELIVERY, PersonaProvider
from gothic_ai.tts import TtsClient
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import XARDAS, _until
from tests.test_memory import close_memory_stores as close_memory_stores


@asynccontextmanager
async def fake_tts(delay: float = 0.0):
    """TTS service stand-in: 50 ms of audio per character, the first line slower than the rest."""
    requests: list[dict] = []

    async def handler(ws) -> None:
        async for raw in ws:
            msg = json.loads(raw)
            requests.append(msg)
            await asyncio.sleep(delay if len(requests) == 1 else 0)
            await ws.send(
                json.dumps({"type": "start", "id": msg["id"], "sample_rate": 16000, "provider": "fake"})
            )
            await ws.send(b"\0\1" * (800 * len(msg["text"])))
            await ws.send(json.dumps({"type": "done", "id": msg["id"], "audio_s": 0.05 * len(msg["text"])}))

    async with serve(handler, "127.0.0.1", 0) as server:
        yield f"ws://127.0.0.1:{server.sockets[0].getsockname()[1]}", requests


async def test_lines_are_voiced_in_order_and_actions_wait_for_them():
    fake = FakeGame()
    llm = FakeBackend(
        "Держи, это поможет тебе. Береги его, другого не будет.",
        delay=0,
        tool_calls=[ToolCall("give_item", {"item": "ITPO_HEALTH_01", "count": 1})],
    )
    async with fake_tts(delay=0.3) as (url, requests), connected(fake) as link:
        await link.attach(XARDAS)
        dialogue = DialogueManager(
            link, llm, PersonaProvider(None), tts=TtsClient(url), memory=await MemoryStore.open(":memory:")
        )
        await dialogue.initialize()
        await dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Дай зелье")
        await _until(lambda: any(m["type"] == "action" for m in fake.received))

    assert fake.said == ["Держи, это поможет тебе.", "Береги его, другого не будет."]
    assert fake.voiced == [True, True]
    flow = [m for m in fake.received if m["type"] in ("say", "say_audio", "say_end", "action")]
    kinds = [m["type"] for m in flow]
    # each line: say with its first audio, the rest of the audio, its end; the action after both lines
    assert kinds == ["say", "say_audio", "say_end", "say", "say_audio", "say_end", "action"]
    assert requests[0]["voice"]["id"] == XARDAS and "old man" in requests[0]["voice"]["description"]

    say, chunk, end = flow[0], flow[1], flow[2]
    assert say["stream"] is True and say["rate"] == 16000 and "audio" not in say
    assert chunk["line"] == say["line"] == end["line"]
    assert len(base64.b64decode(chunk["data"])) == 2 * 800 * len(say["text"])
    assert end["audio_s"] == pytest.approx(0.05 * len(say["text"]))


async def test_the_mood_of_a_line_steers_its_voice():
    fake = FakeGame()
    llm = FakeBackend("[friendly] Ты вернулся. [angry] Времени мало, слушай! [neutral] Иди.", delay=0)
    async with fake_tts() as (url, requests), connected(fake) as link:
        dialogue = DialogueManager(
            link, llm, PersonaProvider(None), tts=TtsClient(url), memory=await MemoryStore.open(":memory:")
        )
        await dialogue.initialize()
        await dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Привет")
        await _until(lambda: len(fake.said) == 3)

    assert [(r["text"], r.get("style")) for r in requests] == [
        ("Ты вернулся.", MOOD_DELIVERY["friendly"]),
        ("Времени мало, слушай!", MOOD_DELIVERY["angry"]),
        ("Иди.", None),  # neutral: the voice's usual manner
    ]


async def test_without_a_tts_service_lines_go_out_silent():
    fake = FakeGame()
    llm = FakeBackend("Ты наконец вернулся из храма. Хорошо, теперь слушай меня внимательно.", delay=0)
    async with connected(fake) as link:
        dialogue = DialogueManager(
            link,
            llm,
            PersonaProvider(None),
            tts=TtsClient("ws://127.0.0.1:9"),
            memory=await MemoryStore.open(":memory:"),
        )
        await dialogue.initialize()
        await dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Привет")
        await _until(lambda: len(fake.said) == 2)
    assert fake.voiced == [False, False]


async def test_a_line_of_the_director_gets_the_npcs_voice():
    # the game asks (v26): the line is queued in a dialog of the story graph, the server only voices it
    fake = FakeGame()
    llm = FakeBackend("", delay=0)
    async with fake_tts() as (url, requests), connected(fake) as link:
        dialogue = DialogueManager(
            link, llm, PersonaProvider(None), tts=TtsClient(url), memory=await MemoryStore.open(":memory:")
        )
        await dialogue.initialize()
        await fake.send(type="line_voice", npc=XARDAS, name="Ксардас", line="AIB_G1", text="Выбирай.")
        await fake.send(type="line_voice", npc=XARDAS, name="Ксардас", line="AIB_G2", text="Ну?")
        await _until(lambda: sum(m["type"] == "say_end" for m in fake.received) == 2)

    flow = [(m["type"], m["line"]) for m in fake.received if m["type"] in ("say", "say_audio", "say_end")]
    assert flow == [(t, line) for line in ("AIB_G1", "AIB_G2") for t in ("say", "say_audio", "say_end")]
    say = next(m for m in fake.received if m["type"] == "say")
    assert say["stream"] is True and say["rate"] == 16000 and say["text"] == "Выбирай."
    assert requests[0]["voice"]["id"] == XARDAS and "old man" in requests[0]["voice"]["description"]
    assert dialogue.story_voiced("AIB_G1") == pytest.approx(0.05 * len("Выбирай."))


async def test_without_a_voice_the_directors_line_stays_a_subtitle():
    fake = FakeGame()
    async with connected(fake) as link:
        dialogue = DialogueManager(
            link, FakeBackend("", delay=0), PersonaProvider(None), memory=await MemoryStore.open(":memory:")
        )
        await dialogue.initialize()
        assert await dialogue.voice_story_line(XARDAS, "Ксардас", "AIB_G1", "Выбирай.") is None
    assert not [m for m in fake.received if m["type"].startswith("say")]
