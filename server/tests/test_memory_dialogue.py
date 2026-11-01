import base64
import json
import logging

import httpx
import numpy as np
import pytest

from gothic_ai import dialogue as dialogue_mod
from gothic_ai.dialogue import DialogueManager
from gothic_ai.embeddings import EmbedClient
from gothic_ai.llm import ToolCall, ToolResult
from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.memory import MemoryStore
from gothic_ai.persona import PersonaProvider
from gothic_ai.talk import TalkDesk
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import XARDAS, _until


async def test_memories_follow_save_and_load():
    store = MemoryStore(":memory:")
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        dialogue = DialogueManager(
            link, FakeBackend("Запомню тебя.", delay=0), PersonaProvider(None), memory=store
        )
        assert dialogue.branch == "A"
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Меня зовут Тестер")
        await _until(lambda: fake.heads[-1:] == [("A", 2)])  # the game would write (A, 2) into a save
        await dialogue.say_as_player(XARDAS, "Это уже после сохранения")
        await _until(lambda: fake.heads[-1:] == [("A", 4)])

        # the player loads that save: the game starts branch B off (A, 2)
        await fake.send(type="session", branch="B", parent="A", parent_seq=2, reason="load")
        await _until(lambda: dialogue.branch == "B")
        conv = dialogue.open(XARDAS, "Ксардас")
        assert [m.content for m in conv.history] == ["Меня зовут Тестер", "Запомню тебя."]
        await _until(lambda: fake.heads[-1:] == [("B", 2)])

    # a reconnect (new link, same branch) reads the same memories back
    fake2 = FakeGame(branch="B")
    async with connected(fake2) as link:
        again = DialogueManager(link, FakeBackend(delay=0), PersonaProvider(None), memory=store)
        assert len(again.open(XARDAS, "Ксардас").history) == 2


async def test_a_new_conversation_is_noted_as_a_new_meeting():
    fake = FakeGame()
    llm = FakeBackend("Опять ты.", delay=0)
    xardas = {"id": XARDAS, "name": "Ксардас", "distance": 300.0, "in_view": True}
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        talk = TalkDesk(link, dialogue, end_silence_s=0)
        await link.snapshot(XARDAS)  # game time for the note
        await fake.roster(xardas)
        await fake.send(type="player_text", text="Привет", focus=XARDAS)
        await _until(lambda: fake.statuses[-2:] == ["thinking", "idle"])
        await fake.roster({**xardas, "distance": 2000.0})  # the hero walks off mid-line: it is over
        await _until(lambda: talk.partner is None and XARDAS not in talk.attached)
        await fake.roster(xardas)
        await fake.send(type="player_text", text="Это снова я", focus=XARDAS)
        await _until(lambda: len(llm.calls) == 2 and fake.statuses.count("idle") == 2)

    contents = [m.content for m in dialogue.conversations[XARDAS].history]
    assert contents[:2] == ["Привет", "Опять ты."]
    assert contents[2] == "[Герой ушёл, не дослушав тебя]"
    assert contents[3] == "[Новая встреча с героем, день 1, 08:00]"
    assert contents[4:] == ["Это снова я", "Опять ты."]


async def test_old_messages_are_folded_into_a_summary(monkeypatch):
    monkeypatch.setattr(dialogue_mod, "MAX_HISTORY", 6)
    monkeypatch.setattr(dialogue_mod, "KEEP_RECENT", 2)
    store = MemoryStore(":memory:")
    llm = FakeBackend("Герой много спрашивал о драконах.", delay=0)
    async with connected(FakeGame()) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None), memory=store)
        conv = dialogue.open(XARDAS, "Ксардас")
        for i in range(4):
            await dialogue.say_as_player(XARDAS, f"Вопрос {i}")
        await _until(lambda: conv.memory.summary != "")

    instructions, messages, tools = llm.calls[-1]
    assert "память персонажа" in instructions and not tools
    assert "Вопрос 0" in messages[0].content and "Вопрос 2" in messages[0].content
    assert [e.content for e in conv.memory.entries] == ["Вопрос 3", "Герой много спрашивал о драконах."]
    assert conv.history[0].role == "developer" and "драконах" in conv.history[0].content
    # stored: a later session reads the summary and only the recent messages
    mem = store.load("test-branch", XARDAS)
    assert mem.summary == conv.memory.summary and len(mem.entries) == 2


async def test_game_without_sessions_still_works():
    async with connected(FakeGame(branch=None)) as link:
        dialogue = DialogueManager(link, FakeBackend("Да.", delay=0), PersonaProvider(None))
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Привет")
        assert dialogue.branch is not None and dialogue.branch.startswith("unsaved-")


def _recall_client(failure=None):
    calls = []

    def respond(request):
        body = json.loads(request.content) if request.content else {}
        stage = body.get("kind", "health")
        calls.append((stage, body.get("texts", [])))
        if failure and stage == failure[0]:
            match failure[1]:
                case "timeout":
                    raise httpx.ReadTimeout("embedding timed out", request=request)
                case "network":
                    raise httpx.ConnectError("embedding unavailable", request=request)
                case "500" | "503":
                    return httpx.Response(int(failure[1]))
                case "base64":
                    return httpx.Response(200, json={"model": "test", "dim": 2, "vectors": "!"})
                case "dimension":
                    return httpx.Response(200, json={"model": "test", "dim": 3, "vectors": "AAAAAA=="})
        if stage == "health":
            return httpx.Response(200, json={
                "model": "test", "dim": 2, "dtype": "bfloat16", "device": "cuda",
            })
        vectors = np.array([
            [1, 0] if stage == "query" or "кинжал" in text else [0, 1]
            for text in body["texts"]
        ], dtype="<f4")
        return httpx.Response(200, json={
            "model": "test", "dim": 2,
            "vectors": base64.b64encode(vectors.tobytes()).decode(),
        })

    return EmbedClient("http://embed", transport=httpx.MockTransport(respond)), calls


def _old_recall_memory(client):
    store = MemoryStore(":memory:")
    store.ensure_branch("test-branch")
    store.append("test-branch", XARDAS, "assistant", "Я уступил тебе кинжал за пять монет.")
    for i in range(8):
        store.append("test-branch", XARDAS, "user", f"Погода сегодня ясная, запись {i}.")
    store.add_summary("test-branch", XARDAS, "Мы говорили о торговле.", covers=9)
    store.embeddings = client
    return store


async def test_recall_tool_finds_paraphrase_and_embeds_missing_candidates(monkeypatch, caplog):
    client, calls = _recall_client()
    store = _old_recall_memory(client)
    queued = []
    monkeypatch.setattr(store, "queue_embeddings", lambda *args: queued.append(args))
    llm = FakeBackend(script=[
        FakeStep(tool_calls=[ToolCall("recall", {"query": "клинок"}, "r1")]),
        FakeStep("Теперь вспомнил."),
    ], delay=0)
    caplog.set_level(logging.INFO, logger="gothic_ai.dialogue")
    try:
        async with connected(FakeGame()) as link:
            dialogue = DialogueManager(link, llm, PersonaProvider(None), memory=store)
            dialogue.open(XARDAS, "Ксардас")
            assert queued == [("test-branch", XARDAS)]
            assert all("кинжал" not in r.content for r in store.search("test-branch", XARDAS, "клинок", 9))
            await dialogue.say_as_player(XARDAS, "Что это был за клинок?")
            result = next(m for m in llm.calls[1][1] if isinstance(m, ToolResult))
            assert "Ты вспоминаешь:\n[?] Ты: Я уступил тебе кинжал за пять монет." in result.output
            assert len([c for c in calls if c[0] == "document"]) == 1
            assert len(next(texts for kind, texts in calls if kind == "document")) == 9
            assert [c[0] for c in calls] == ["health", "document", "query"]
        logs = [r.message for r in caplog.records if "recalls" in r.message]
        assert len(logs) == 1 and "path=semantic reason=none" in logs[0]
    finally:
        store.close()
        await client.close()


@pytest.mark.parametrize("failure", [
    ("health", "timeout"), ("health", "network"), ("health", "503"),
    ("document", "500"), ("document", "base64"), ("document", "dimension"),
    ("query", "timeout"), ("query", "500"), ("query", "base64"), ("query", "dimension"),
])
async def test_recall_falls_back_to_words_on_service_failure(monkeypatch, caplog, failure):
    client, calls = _recall_client(failure)
    store = _old_recall_memory(client)
    monkeypatch.setattr(store, "queue_embeddings", lambda *args: None)
    caplog.set_level(logging.INFO, logger="gothic_ai.dialogue")
    try:
        async with connected(FakeGame()) as link:
            dialogue = DialogueManager(link, FakeBackend(delay=0), PersonaProvider(None), memory=store)
            conv = dialogue.open(XARDAS, "Ксардас")
            expected = store.search(conv.branch, XARDAS, "монеты", 9)
            result = await dialogue._recall(conv, "монеты")
            assert result == "Ты вспоминаешь:\n" + "\n".join(
                f"[?] {'Ты' if r.role == 'assistant' else 'Герой'}: {r.content}" for r in expected
            )
        logs = [r.message for r in caplog.records if "recalls" in r.message]
        assert len(logs) == 1 and "path=lexical reason=" in logs[0]
        assert calls[-1][0] == failure[0]
    finally:
        store.close()
        await client.close()
