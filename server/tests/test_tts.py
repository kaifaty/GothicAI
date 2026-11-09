import asyncio
import base64
import json
from contextlib import asynccontextmanager

import pytest
from websockets.asyncio.server import serve

from gothic_ai.dialogue import DialogueManager
from gothic_ai.llm import ToolCall
from gothic_ai.llm.fake import FakeBackend
from gothic_ai.persona import MOOD_DELIVERY, PersonaProvider
from gothic_ai.tts import TtsClient, WordMatcher
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import XARDAS, _until


@asynccontextmanager
async def fake_tts(delay: float = 0.0, *, words: bool = False):
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
            pcm = b"\0\1" * (800 * len(msg["text"]))
            if words:
                timings = [[word, i / 10, (i + 1) / 10] for i, word in enumerate(msg["text"].split())]
                split = len(timings) // 2
                half = len(pcm) // 4 * 2
                await ws.send(pcm[:half])
                if split:
                    await ws.send(json.dumps({
                        "type": "audio", "id": msg["id"], "data": "", "words": timings[:split],
                    }))
                await ws.send(pcm[half:])
                await ws.send(json.dumps({
                    "type": "audio", "id": msg["id"], "data": "", "words": timings[split:],
                }))
            else:
                await ws.send(pcm)
            await ws.send(json.dumps({"type": "done", "id": msg["id"], "audio_s": 0.05 * len(msg["text"])}))

    async with serve(handler, "127.0.0.1", 0) as server:
        yield f"ws://127.0.0.1:{server.sockets[0].getsockname()[1]}", requests


async def test_lines_are_voiced_in_order_and_actions_wait_for_them():
    fake = FakeGame()
    llm = FakeBackend(
        "Держи, [doubt] это поможет тебе. Береги его, другого не будет.",
        delay=0,
        tool_calls=[ToolCall("give_item", {"item": "ITPO_HEALTH_01", "count": 1})],
    )
    async with fake_tts(delay=0.3, words=True) as (url, requests), connected(fake) as link:
        await link.attach(XARDAS)
        dialogue = DialogueManager(link, llm, PersonaProvider(None), tts=TtsClient(url))
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Дай зелье")
        await _until(lambda: any(m["type"] == "action" for m in fake.received))

    assert fake.said == ["Держи, это поможет тебе.", "Береги его, другого не будет."]
    assert fake.voiced == [True, True]
    flow = [m for m in fake.received if m["type"] in ("say", "say_audio", "say_end", "action")]
    kinds = [m["type"] for m in flow]
    # each line: say with its first audio, the rest of the audio, its end; the action after both lines
    assert kinds == ["say", "say_audio", "say_audio", "say_audio", "say_end"] * 2 + ["action"]
    assert requests[0]["voice"]["id"] == XARDAS and "old man" in requests[0]["voice"]["description"]

    say, first, second, tail, end = flow[:5]
    assert say["stream"] is True and say["rate"] == 16000 and "audio" not in say
    assert say["cues"] == [{"cue": "doubt", "word": 1}]
    assert requests[0]["text"] == say["text"]
    assert first["line"] == second["line"] == tail["line"] == say["line"] == end["line"]
    assert sum(len(base64.b64decode(chunk["data"])) for chunk in (first, second, tail)) == (
        2 * 800 * len(say["text"])
    )
    assert end["audio_s"] == pytest.approx(0.05 * len(say["text"]))
    count = len(say["text"].split())
    assert "words" not in first and tail["data"] == ""
    assert second["words"] == [[i, i / 10, (i + 1) / 10] for i in range(count // 2)]
    assert tail["words"] == [[i, i / 10, (i + 1) / 10] for i in range(count // 2, count)]


async def test_the_mood_of_a_line_steers_its_voice():
    fake = FakeGame()
    llm = FakeBackend("[friendly] Ты вернулся. [angry] Времени мало, слушай! [neutral] Иди.", delay=0)
    async with fake_tts() as (url, requests), connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None), tts=TtsClient(url))
        dialogue.open(XARDAS, "Ксардас")
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
        dialogue = DialogueManager(link, llm, PersonaProvider(None), tts=TtsClient("ws://127.0.0.1:9"))
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Привет")
        await _until(lambda: len(fake.said) == 2)
    assert fake.voiced == [False, False]


async def test_a_line_of_the_director_gets_the_npcs_voice():
    # the game asks (v26): the line is queued in a dialog of the story graph, the server only voices it
    fake = FakeGame()
    llm = FakeBackend("", delay=0)
    async with fake_tts(words=True) as (url, requests), connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None), tts=TtsClient(url))
        await fake.send(type="line_voice", npc=XARDAS, name="Ксардас", line="AIB_G1", text="Выбирай.")
        await fake.send(type="line_voice", npc=XARDAS, name="Ксардас", line="AIB_G2", text="Ну?")
        await _until(lambda: sum(m["type"] == "say_end" for m in fake.received) == 2)

    flow = [(m["type"], m["line"]) for m in fake.received if m["type"] in ("say", "say_audio", "say_end")]
    assert flow == [
        (t, line) for line in ("AIB_G1", "AIB_G2")
        for t in ("say", "say_audio", "say_audio", "say_audio", "say_end")
    ]
    say = next(m for m in fake.received if m["type"] == "say")
    assert say["stream"] is True and say["rate"] == 16000 and say["text"] == "Выбирай."
    assert requests[0]["voice"]["id"] == XARDAS and "old man" in requests[0]["voice"]["description"]
    assert dialogue.story_voiced("AIB_G1") == pytest.approx(0.05 * len("Выбирай."))
    chunks = [m for m in fake.received if m["type"] == "say_audio"]
    assert [m.get("words") for m in chunks] == [None, None, [[0, 0.0, 0.1]]] * 2
    assert all(chunk["data"] == "" for chunk in chunks[2::3])


def test_word_alignment_uses_subtitle_indices_and_skips_insertions():
    matcher = WordMatcher("СЛУШАЙ, (смеясь) кто-то — вернулся! Потом уйдём.")
    assert matcher.match([
        ("[angry]", 0.0, 0.0), ("слушай", 0.01234, 0.45678),
        (" ", 0.45678, 0.45678), ("КТО-ТО", 0.5, 0.8),
    ]) == [(0, 0.012, 0.457), (2, 0.5, 0.8)]
    assert matcher.match([
        ("лишнее", 0.8, 0.9), ("вернулся", 0.9, 1.1), ("слушай", 1.1, 1.2),
        ("потом", 1.2, 1.4), ("уйдём.", 1.4, 1.7),
    ]) == [(4, 0.9, 1.1), (5, 1.2, 1.4), (6, 1.4, 1.7)]


@pytest.mark.parametrize("words", [True, False])
async def test_full_audio_retains_optional_word_timings(words):
    async with fake_tts(words=words) as (url, _requests):
        client = TtsClient(url)
        try:
            speech = await client.speak("Ты вернулся.", "test", "")
        finally:
            await client.aclose()
    assert speech.wav.startswith(b"RIFF")
    assert speech.words == ([("Ты", 0.0, 0.1), ("вернулся.", 0.1, 0.2)] if words else None)


async def test_without_a_voice_the_directors_line_stays_a_subtitle():
    fake = FakeGame()
    async with connected(fake) as link:
        dialogue = DialogueManager(link, FakeBackend("", delay=0), PersonaProvider(None))
        assert await dialogue.voice_story_line(XARDAS, "Ксардас", "AIB_G1", "Выбирай.") is None
    assert not [m for m in fake.received if m["type"].startswith("say")]
