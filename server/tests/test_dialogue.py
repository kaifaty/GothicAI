import asyncio
import json
import time
from collections.abc import AsyncIterator
from contextlib import asynccontextmanager
from copy import deepcopy
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest
from websockets.asyncio.server import serve

from gothic_ai import dialogue as dialogue_module
from gothic_ai.dialogue import QUIET_STYLE, SCENE_GAP_S, TTS_PAUSE, DialogueManager, FirstReaction
from gothic_ai.game import GameLink
from gothic_ai.journal import Journal
from gothic_ai.llm import Done, LLMError, LLMEvent, Message, TextDelta, Tool, ToolCall, ToolResult
from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.persona import MOOD_DELIVERY, PersonaProvider
from gothic_ai.protocol import Hello, LineStarted, NpcDied, NpcRoster, Snapshot, SvmLine
from gothic_ai.scenarios import basics as scenarios
from gothic_ai.talk import TalkDesk
from gothic_ai.trace import TraceWriter
from gothic_ai.tts import Speech, TtsClient, TtsError
from tests.fake_game import FakeGame, connected

XARDAS = "NONE_100_XARDAS"
LOBART = "BAU_950_LOBART"
HILDA = "BAU_951_HILDA"


async def _until(predicate, timeout: float = 3.0) -> None:
    async with asyncio.timeout(timeout):
        while not predicate():  # noqa: ASYNC110 - polling a fake is fine in tests
            await asyncio.sleep(0.01)


@pytest.mark.parametrize(("quick", "enabled", "available", "replace"), [
    (True, True, True, True), (False, True, True, False),
    (True, False, True, False), (True, True, False, False),
])
async def test_svm_mode_waits_for_hello_and_requires_an_available_quick_model(
    quick, enabled, available, replace,
):
    conn = SimpleNamespace(send=AsyncMock())
    link = GameLink(conn)
    llm = FakeBackend()
    dialogue = DialogueManager(
        link, llm, PersonaProvider(None), quick=llm if quick else None,
        svm_lines=enabled,
    )
    dialogue._svm_available = available
    conn.send.assert_not_awaited()
    link._dispatch(Hello(type="hello", protocol=50, client="fake", codepage="cp1251"))
    await asyncio.gather(*dialogue._background)
    assert [json.loads(call.args[0]) for call in conn.send.await_args_list] == [
        {"type": "svm_mode", "replace": replace},
    ]
    assert llm.calls == []


@pytest.mark.parametrize(("svm", "target", "event", "to", "native", "prompt_text", "target_state"), [
    (
        "$ENEMYKILLED", LOBART, "ты только что убил противника — Лобарт", "обращаясь к Лобарт",
        "(дьявольский смех) Ты показал себя!", "Ты показал себя!", None,
    ),
    *[
        (
            "$ENEMYKILLED", LOBART, "ты только что убил противника — Лобарт", "обращаясь к Лобарт",
            "Ты заслужил это!", "Ты заслужил это!", state,
        )
        for state in ("died", "down", "snapshot", "revived")
    ],
    ("$HELP", None, "на тебя напали, ты зовёшь на помощь", "вслух", "Помогите!", "Помогите!", None),
    ("$AWAKE", XARDAS.lower(), "ты проснулся и зеваешь", "вслух", "(зевает)", "зевает", None),
    (
        "$HELP", "PC_HERO", "на тебя напали, ты зовёшь на помощь", "обращаясь к герой",
        "Помогите!", "Помогите!", None,
    ),
    ("$UNKNOWN", LOBART, "", "обращаясь к Лобарт", "(зевает)", "зевает", None),
])
async def test_svm_uses_persona_and_situation_without_history_and_voices_twelve_words(
    tmp_path, svm, target, event, to, native, prompt_text, target_state,
):
    phrase = "Я ещё покажу тебе как надо встречать незваных гостей у моих ворот."
    quick = FakeBackend(f"[angry] (потягиваюсь) {phrase} Это уже вторая фраза.", delay=0, chunk=200)
    llm = FakeBackend()
    fake = FakeGame()
    writer = TraceWriter(tmp_path)
    line = SvmLine(
        type="svm_line", id="svm-1", npc=XARDAS, svm=svm,
        text=native, target=target, overlay=False,
    )
    async with scene_tts(per_char=0.0001) as (url, requests), connected(fake) as link:
        dialogue = DialogueManager(
            link, llm, PersonaProvider(None), quick=quick, tts=TtsClient(url), trace=writer,
        )
        link.snapshots[XARDAS] = Snapshot.model_validate(fake.snapshot(XARDAS))
        dialogue._nearby[XARDAS] = ("Ксардас", 3000)
        dialogue._nearby[LOBART] = ("Лобарт", 100)
        if target_state in {"died", "revived"}:
            link._dispatch(NpcDied(type="npc_died", npc=LOBART, name="Лобарт", killer=XARDAS))
        if target_state in {"down", "revived"}:
            link._dispatch(NpcRoster.model_validate({
                "type": "npc_roster", "t": 1, "full": False,
                "npcs": [{"id": LOBART, "name": "Лобарт", "distance": 100,
                          "activity": "down" if target_state == "down" else "routine"}],
            }))
        if target_state == "snapshot":
            target_snapshot = fake.snapshot(LOBART)
            target_snapshot["npc"].update(name="Лобарт", hp=0)
            link._dispatch(Snapshot.model_validate(target_snapshot))
        dialogue.remember(XARDAS, "Ксардас", "Ты защищаешь свою башню от зверей")
        conv = dialogue.conversations[XARDAS]
        assert conv.history
        brief = dialogue._brief(conv)
        started = time.monotonic()
        dialogue._on_message(line)
        await _until(lambda: not dialogue._svm_tasks and bool(fake.say_ends))

    assert fake.said == [phrase] and fake.voiced == [True]
    (say,) = [message for message, _ in fake.says]
    recipient = target if (target or "").upper() != XARDAS else None
    if target_state in {"died", "down", "snapshot"}:
        recipient = None
    assert say["overlay"] is True and say.get("to") == recipient and say["mood"] == "angry"
    assert not say.get("reply") and not say.get("filler")
    assert [request["text"] for request in requests] == [phrase]
    assert llm.calls == [] and quick.temperatures == [0.7]
    (instructions, messages, tools), = quick.calls
    assert instructions == conv.persona.instructions and len(messages) == 1
    assert messages[0].role == "developer"
    context = messages[0].content
    assert brief in context
    assert "Ты защищаешь свою башню от зверей" not in context
    moment = f"Сейчас: {event}. " if event else ""
    assert (
        f"[{moment}В этот момент обычно говорят: «{prompt_text}» — эти слова не повторяй. "
        f"Скажи то же по смыслу по-своему, одной короткой фразой — как сказал бы ты, {to}.]"
    ) in context
    assert tools == [] and conv.reply is None and conv.talked_at == float("-inf")
    assert conv.history[-1] == Message("developer", f"[Ты сказал вслух: «{phrase}»]")
    assert conv.said_at >= started and len(conv.svm_phrases) == 1
    (record,) = [json.loads(raw) for raw in writer.path().read_text().splitlines()]
    assert record.items() >= line.model_dump(mode="json").items()
    assert record["outcome"] == "said" and record["answer"] == phrase
    assert 0 <= record["elapsed_ms"] < 2000 and "t" in record
    assert 0 <= record["first_phrase_ms"] <= record["voice_ms"] <= record["elapsed_ms"]


@pytest.mark.parametrize(("phrase", "answer", "outcome"), [
    (
        "Я ещё покажу тебе как надо встречать незваных гостей у моих старых ворот.",
        "Я ещё покажу тебе как надо встречать незваных гостей у моих старых ворот.", "too_long",
    ),
    ("(дьявольский смех)", "", "nothing"),
])
async def test_svm_thirteen_words_or_only_stage_directions_are_silent_and_traced(
    tmp_path, phrase, answer, outcome,
):
    quick = FakeBackend(phrase, delay=0, chunk=200)
    conn = SimpleNamespace(send=AsyncMock())
    writer = TraceWriter(tmp_path)
    dialogue = DialogueManager(
        GameLink(conn), FakeBackend(), PersonaProvider(None), quick=quick, trace=writer,
    )
    dialogue._nearby[XARDAS] = ("Ксардас", 100)
    dialogue._on_message(SvmLine(
        type="svm_line", id="long", npc=XARDAS, svm="$THREATEN", text="Берегись!", overlay=True,
    ))
    await _until(lambda: not dialogue._svm_tasks)
    conn.send.assert_not_awaited()
    assert dialogue.conversations[XARDAS].history == []
    (record,) = [json.loads(raw) for raw in writer.path().read_text().splitlines()]
    assert record["outcome"] == outcome and record["answer"] == answer
    assert record["elapsed_ms"] >= 0


async def test_svm_deadline_is_two_seconds_and_lateness_does_not_count_as_an_error(tmp_path):
    assert dialogue_module.SVM_REPLY_S == 2.0
    quick = FakeBackend("Убирайся!", delay=0, first=2.2)
    conn = SimpleNamespace(send=AsyncMock())
    writer = TraceWriter(tmp_path)
    dialogue = DialogueManager(
        GameLink(conn), FakeBackend(), PersonaProvider(None), quick=quick, trace=writer,
    )
    dialogue._nearby[XARDAS] = ("Ксардас", 100)
    dialogue._svm_errors = 2
    dialogue._on_message(SvmLine(
        type="svm_line", id="late", npc=XARDAS, svm="$THREATEN", text="Берегись!", overlay=True,
    ))
    await _until(lambda: not dialogue._svm_tasks)
    conn.send.assert_not_awaited()
    assert dialogue._svm_errors == 2 and dialogue._svm_available
    assert dialogue.conversations[XARDAS].history == []
    (record,) = [json.loads(raw) for raw in writer.path().read_text().splitlines()]
    assert record["outcome"] == "late" and record["answer"] == ""
    assert 1950 <= record["elapsed_ms"] < 2800


@pytest.mark.parametrize("busy", ["turn", "recent", "scene"])
async def test_svm_does_not_interrupt_a_turn_or_a_line_said_in_the_last_three_seconds(tmp_path, busy):
    assert dialogue_module.SVM_QUIET_S == 3.0
    quick = FakeBackend("Убирайся!", delay=0)
    conn = SimpleNamespace(send=AsyncMock())
    writer = TraceWriter(tmp_path)
    dialogue = DialogueManager(
        GameLink(conn), FakeBackend(), PersonaProvider(None), quick=quick, trace=writer,
    )
    dialogue._nearby[XARDAS] = ("Ксардас", 100)
    if busy == "scene":
        dialogue._on_message(LineStarted(type="line_started", npc=XARDAS, line="scene", voiced=True))
    conv = dialogue.open(XARDAS, "Ксардас", warm=False)
    if busy == "turn":
        conv.reply = asyncio.create_task(asyncio.Event().wait())
    elif busy == "recent":
        await dialogue._say(conv, "Стой.", None)
        assert time.monotonic() - conv.said_at < 3
        conn.send.reset_mock()
    try:
        dialogue._on_message(SvmLine(
            type="svm_line", id="busy", npc=XARDAS, svm="$THREATEN", text="Берегись!", overlay=True,
        ))
        assert dialogue._svm_tasks == {} and quick.calls == []
        conn.send.assert_not_awaited()
        (record,) = [json.loads(raw) for raw in writer.path().read_text().splitlines()]
        assert record["outcome"] == "busy" and record["elapsed_ms"] >= 0
    finally:
        if conv.reply is not None:
            conv.reply.cancel()
            await asyncio.gather(conv.reply, return_exceptions=True)


@pytest.mark.parametrize("source", ["roster", "snapshot"])
async def test_svm_beyond_thirty_metres_is_silent_without_opening_a_conversation(tmp_path, source):
    conn = SimpleNamespace(send=AsyncMock())
    link = GameLink(conn)
    quick = FakeBackend(delay=0)
    writer = TraceWriter(tmp_path)
    dialogue = DialogueManager(link, FakeBackend(), PersonaProvider(None), quick=quick, trace=writer)
    if source == "roster":
        dialogue._nearby[XARDAS] = ("Ксардас", 3000.1)
    else:
        snapshot = FakeGame().snapshot(XARDAS)
        snapshot["player"]["distance"] = 3000.1
        link.snapshots[XARDAS] = Snapshot.model_validate(snapshot)
    dialogue._on_message(SvmLine(
        type="svm_line", id="far", npc=XARDAS, svm="$THREATEN", text="Берегись!", overlay=True,
    ))
    assert dialogue._svm_tasks == {} and quick.calls == [] and dialogue.conversations == {}
    conn.send.assert_not_awaited()
    (record,) = [json.loads(raw) for raw in writer.path().read_text().splitlines()]
    assert record["outcome"] == "far" and record["elapsed_ms"] >= 0


async def test_svm_has_one_pending_line_per_npc_three_slots_and_yields_to_quick_words(tmp_path):
    quick = FakeBackend("К оружию!", delay=0, first=60)
    conn = SimpleNamespace(send=AsyncMock())
    writer = TraceWriter(tmp_path)
    dialogue = DialogueManager(
        GameLink(conn), FakeBackend(), PersonaProvider(None), quick=quick, trace=writer,
    )
    for npc in (XARDAS, LOBART, HILDA, "MIL_320_SPEAKER"):
        dialogue._nearby[npc] = (npc, 100)
    line = SvmLine(type="svm_line", id="first", npc=XARDAS, svm="$THREATEN", text="Берегись!", overlay=True)
    dialogue._on_message(line)
    dialogue._on_message(line.model_copy(update={"id": "same-npc"}))
    for npc in (LOBART, HILDA):
        dialogue._on_message(line.model_copy(update={"id": npc, "npc": npc}))
    dialogue._on_message(line.model_copy(update={"id": "fourth", "npc": "MIL_320_SPEAKER"}))
    assert len(dialogue._svm_tasks) == 3
    tasks = list(dialogue._svm_tasks.values())
    await _until(lambda: len(quick.calls) == 3)
    quick.first = 0
    conv = dialogue.open("MIL_320_SPEAKER", "Стражник", warm=False)
    words, _ = await asyncio.wait_for(dialogue._quick_words(conv, []), 0.5)
    await asyncio.gather(*tasks, return_exceptions=True)
    await _until(lambda: not dialogue._svm_tasks)
    assert words == "К оружию!" and all(task.cancelled() for task in tasks)
    conn.send.assert_not_awaited()
    records = [json.loads(raw) for raw in writer.path().read_text().splitlines()]
    assert {r["id"]: r["outcome"] for r in records} == {
        "first": "busy", "same-npc": "busy", LOBART: "busy", HILDA: "busy", "fourth": "busy",
    }
    assert all(r["elapsed_ms"] >= 0 for r in records)


async def test_svm_repeats_are_normalized_and_only_the_last_five_said_phrases_count(tmp_path, monkeypatch):
    replies = ["Первый.", "Второй.", "Третий.", "Четвёртый.", "Пятый.", "ПЕРВЫЙ!!!", "Шестой.", "Первый."]
    quick = FakeBackend(script=[FakeStep(text) for text in replies], delay=0)
    writer = TraceWriter(tmp_path)
    dialogue = DialogueManager(
        GameLink(SimpleNamespace(send=AsyncMock())), FakeBackend(), PersonaProvider(None),
        quick=quick, trace=writer,
    )
    dialogue._nearby[XARDAS] = ("Ксардас", 100)
    monkeypatch.setattr(dialogue, "_say", AsyncMock(wraps=dialogue._say))
    conv = dialogue.open(XARDAS, "Ксардас", warm=False)
    line = SvmLine(type="svm_line", id="repeat", npc=XARDAS, svm="$THREATEN", text="Берегись!", overlay=True)
    for index in range(len(replies)):
        conv.said_at = float("-inf")
        conv.overlay_free_at = 0
        for sent in dialogue._sent:
            dialogue._sent[sent] = float("-inf")  # earlier sends expired during the skipped cooldown
        dialogue._on_message(line.model_copy(update={"id": str(index)}))
        await _until(lambda: not dialogue._svm_tasks)
    records = [json.loads(raw) for raw in writer.path().read_text().splitlines()]
    assert [r["outcome"] for r in records] == ["said"] * 5 + ["repeat", "said", "said"]
    assert [r["answer"] for r in records] == replies
    assert dialogue._say.await_count == 7 and len(conv.svm_phrases) == 5
    assert len(conv.history) == 7 and "ПЕРВЫЙ!!!" not in str(conv.history)


async def test_svm_three_errors_restore_original_lines_then_probes_restore_replacement(tmp_path, monkeypatch):
    assert dialogue_module.SVM_RECOVER_S == 30.0
    monkeypatch.setattr(dialogue_module, "SVM_RECOVER_S", 0.05)
    quick = FakeBackend("Снова на ногах!", delay=0)
    monkeypatch.setattr(quick, "stream", FailingBackend().stream)
    probe = AsyncMock(side_effect=[LLMError("still unavailable"), None])
    monkeypatch.setattr(quick, "prewarm", probe)
    conn = SimpleNamespace(send=AsyncMock())
    link = GameLink(conn)
    writer = TraceWriter(tmp_path)
    dialogue = DialogueManager(link, FakeBackend(), PersonaProvider(None), quick=quick, trace=writer)
    dialogue._nearby[XARDAS] = ("Ксардас", 100)
    link._dispatch(Hello(type="hello", protocol=50, client="fake", codepage="cp1251"))
    await asyncio.gather(*dialogue._background)
    line = SvmLine(type="svm_line", id="error", npc=XARDAS, svm="$THREATEN", text="Берегись!", overlay=True)
    for index in range(3):
        dialogue._on_message(line.model_copy(update={"id": str(index)}))
        await _until(lambda: not dialogue._svm_tasks)
        assert dialogue._svm_errors == index + 1
        assert dialogue._svm_available is (index < 2)
    await _until(lambda: len(conn.send.await_args_list) == 2)
    assert [json.loads(c.args[0]) for c in conn.send.await_args_list] == [
        {"type": "svm_mode", "replace": True}, {"type": "svm_mode", "replace": False},
    ]
    dialogue._on_message(line.model_copy(update={"id": "unavailable"}))
    assert dialogue._svm_tasks == {}
    await _until(lambda: dialogue._svm_available)
    await _until(lambda: len(conn.send.await_args_list) == 3)
    assert json.loads(conn.send.await_args.args[0]) == {"type": "svm_mode", "replace": True}
    assert probe.await_count == 2 and dialogue._svm_errors == 0
    assert all(call.kwargs["messages"] == [] for call in probe.await_args_list)
    records = [json.loads(raw) for raw in writer.path().read_text().splitlines()]
    assert len(records) == 4 and all(r["outcome"] == "error" for r in records)
    assert all(r["answer"] == "" and r["elapsed_ms"] >= 0 for r in records)
    assert dialogue.conversations[XARDAS].history == []


async def test_player_phrases_get_replies_line_by_line():
    fake = FakeGame()
    llm = FakeBackend("Ты наконец вернулся из храма. Хорошо, теперь слушай меня внимательно.", delay=0)
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Привет")
        await _until(lambda: fake.statuses[-2:] == ["thinking", "idle"])
        assert fake.said == ["Ты наконец вернулся из храма.", "Хорошо, теперь слушай меня внимательно."]

        await dialogue.say_as_player(XARDAS, "Где я?")
        await _until(lambda: len(fake.said) == 4 and fake.statuses[-1] == "idle")

    history = dialogue.conversations[XARDAS].history
    assert [m.role for m in history] == ["user", "assistant", "user", "assistant"]
    assert history[0].content == "Привет" and history[2].content == "Где я?"
    # the llm saw the whole conversation and the persona
    instructions, messages, tools = llm.calls[-1]
    assert "Ксардас" in instructions and messages[-1] == Message("user", "Где я?")
    assert messages[-2].role == "developer" and "Обстановка" in messages[-2].content
    assert {t.name for t in tools} == {
        "set_intent",
        "give_item",
        "recall",
        "end_conversation",
    }  # no body tools


async def test_cavalorns_fragmented_tool_text_is_logged_without_being_said_or_remembered(
    caplog, monkeypatch
):
    npc = "BAU_4300_ADDON_CAVALORN"
    spoken = [
        "Вот это разговор.",
        "Внизу по дороге их пещера — там и вернём моё добро.",
        "Прикроешь мне спину, только не лезь под удар.",
    ]
    clean = " ".join(spoken)
    leak = "protoc function give_quest?"
    llm = FakeBackend(clean + "\n " + leak + " \n", chunk=3, delay=0)
    fake = FakeGame()
    entries: list[dict] = []
    monkeypatch.setattr(dialogue_module, "CHAT_TEXT_EVERY", 0)
    async with scene_tts(per_char=0.0001) as (url, requests), connected(fake) as link:
        dialogue = DialogueManager(
            link, llm, PersonaProvider(None), tts=TtsClient(url), journal=Journal(entries.append)
        )
        conv = dialogue.open(npc, "Кавалорн")
        await dialogue.say_as_player(npc, "Помочь тебе?")
        await _until(lambda: len(fake.say_ends) == 3 and fake.statuses[-1:] == ["idle"])

    assert fake.said == spoken
    assert [r["text"] for r in requests] == spoken
    texts = [m["text"] for m in fake.received if m["type"] == "chat_text"]
    assert texts[-1] == clean
    assert all(clean.startswith(text.strip()) for text in texts)  # no partial leaked prefix was shown
    assert conv.history[-1] == Message("assistant", clean)
    assert dialogue.memory.load("test-branch", npc).entries[-1].content == clean
    assert [entry["text"] for entry in entries if entry["kind"] == "npc"] == [clean]
    warnings = [
        r.getMessage()
        for r in caplog.records
        if r.name == "gothic_ai.dialogue" and r.levelname == "WARNING"
    ]
    assert any(leak in warning and (npc in warning or "Кавалорн" in warning) for warning in warnings)


class HiddenQuestTool:
    all_tools = (Tool("give_quest", "Assign a quest."),)

    async def prepare(self, npc: str) -> None:
        pass

    def situation(self, npc: str) -> None:
        return None

    def tools(self, npc: str) -> list[Tool]:
        return []


async def test_extensions_share_tool_names_once_and_the_first_definition_wins():
    class QuestTools(HiddenQuestTool):
        def __init__(self, description):
            self.all_tools = (Tool("amend_quest", description),)

        def tools(self, npc):
            return list(self.all_tools)

    fake = FakeGame()
    llm = FakeBackend("Ладно.", delay=0)
    first, second = QuestTools("First definition."), QuestTools("Second definition.")
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        dialogue.extensions.extend([first, second])
        offered = dialogue._offered(XARDAS)
        assert [t.name for t in offered].count("amend_quest") == 1
        assert next(t for t in offered if t.name == "amend_quest") is first.all_tools[0]
        request = dialogue._request_tools(
            [*offered, Tool("turn_signal", "First signal."), Tool("turn_signal", "Second signal.")]
        )
        assert [t.name for t in request].count("turn_signal") == 1
        assert next(t for t in request if t.name == "turn_signal").description == "First signal."
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Не нужно никуда идти.")
        await _until(lambda: fake.statuses[-1:] == ["idle"])
        requested = llm.calls[0][2]
        assert [t.name for t in requested].count("amend_quest") == 1
        assert next(t for t in requested if t.name == "amend_quest") is first.all_tools[0]


@pytest.mark.parametrize("own", [False, True])
async def test_amend_quest_routes_to_the_extension_owning_its_id(tmp_path, own):
    from gothic_ai.gamedata.storygraph import load
    from gothic_ai.memory import MemoryStore
    from gothic_ai.questbook import goal_id
    from gothic_ai.vanilla import VanillaQuests
    from tests.test_director import GRAPH, QUEST
    from tests.test_questbook import _book, _quest

    path = tmp_path / "story.json"
    path.write_text(json.dumps(GRAPH), encoding="utf-8")
    graph = load(path)
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, book, _, chronicle = await _book(fake, store, graph)
    try:
        q = _quest()
        quest, npc, name, drop = (
            (q.id, HILDA, "Хильда", goal_id(q, 1, 1)) if own else (QUEST, LOBART, "Лобарт", "ITPL_BEET")
        )
        llm = FakeBackend(
            script=[
                FakeStep(
                    "Договорились.",
                    [
                        ToolCall(
                            "amend_quest", {"quest": quest, "drop": [drop], "why": "Больше не нужно."}, "c1"
                        )
                    ],
                )
            ],
            delay=0,
        )
        dialogue = DialogueManager(book.link, llm, PersonaProvider(None), memory=store)
        VanillaQuests(book.link, dialogue, chronicle, graph)
        book.dialogue = dialogue
        dialogue.extensions.append(book)
        fake.vars[QUEST] = 1
        if own:
            assert await book.make(q) == []
            fake.vars.update({q.id: 1, "DIR_Q1_STAGE": 1})
            chronicle.record({"fact": "story_quest_given", "id": q.id})
        dialogue.open(npc, name)
        await dialogue.say_as_player(npc, "Не нужно никуда идти.")
        await _until(lambda: bool(chronicle.facts("quest_terms")))
        assert chronicle.facts("quest_terms")[0]["quest"] == quest
        assert chronicle.facts("quest_terms")[0]["drop"] == [drop]
        if own:
            assert any(
                m["type"] == "story_effect"
                and m["effects"] == [{"do": "set", "var": drop, "value": 1}]
                for m in fake.story
            )
        else:
            assert fake.vars[QUEST] == 1  # the terms change, the scripts' quest still runs
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("remaining_alive,explicit_tools", [(False, False), (True, False), (False, True)])
async def test_amending_refreshes_automatic_tools_and_continues_only_when_accept_opens(
    tmp_path, remaining_alive, explicit_tools
):
    from gothic_ai.gamedata.storygraph import load
    from gothic_ai.llm import ToolResult
    from gothic_ai.memory import MemoryStore
    from gothic_ai.trace import TraceWriter, find_turn
    from gothic_ai.vanilla import ACCEPT_NOW
    from tests.test_director import GRAPH, QUEST, RUNNING
    from tests.test_vanilla import BANDIT, _setup

    raw = deepcopy(GRAPH)
    dead = [
        "&&",
        ["call", "NPC_ISDEAD", ["inst", "BANDIT"]],
        ["call", "NPC_ISDEAD", ["inst", "SECOND_BANDIT"]],
    ]
    ending = next(n for n in raw["nodes"] if n["id"] == "DIA_LOBART_RUEBENRUNNING")
    ending.update(condition=RUNNING, effects=[
        {"do": "set", "var": QUEST, "value": ["int", 2, "LOG_SUCCESS"], "when": dead},
        {"do": "xp", "amount": ["int", 100], "when": dead},
    ])
    raw["instances"].update(BANDIT=BANDIT, SECOND_BANDIT="BDT_2_BANDIT")
    path = tmp_path / "story.json"
    path.write_text(json.dumps(raw), encoding="utf-8")
    llm = FakeBackend(script=[
        FakeStep("Его пощадим.", [ToolCall(
            "amend_quest", {"quest": QUEST, "drop": [BANDIT], "why": "Пощадил."}, "m1"
        )]),
        FakeStep("Дело сделано.", [ToolCall("accept_quest", {"quest": QUEST}, "a1")]),
    ], delay=0)
    fake = FakeGame(branch="A")
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, llm, load(path), MemoryStore(":memory:"))
    fake.instances["SECOND_BANDIT"] = "BDT_2_BANDIT"
    fake.npc_states.update({
        BANDIT: {"state": "alive", "name": "Бандит", "distance": 500, "seen": True},
        "BDT_2_BANDIT": {
            "state": "alive" if remaining_alive else "dead", "name": "Бандит",
            "distance": 500, "seen": True,
        },
    })
    dialogue.trace = TraceWriter(tmp_path / "traces")
    try:
        conv = dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        offered = dialogue._offered(LOBART)
        assert "accept_quest" not in {t.name for t in offered}
        await dialogue._turn(
            conv, Message("user", "Пощади его, с остальными покончено."), "player",
            tools=offered if explicit_tools else None,
        )
        trace, _ = find_turn(dialogue.trace.path(), None)
        assert trace["steps"][0]["calls"][0]["outcome"] == "accepted"
        assert chronicle.facts("quest_terms")[0]["drop"] == [BANDIT]
        assert fake.npc_states[BANDIT]["state"] == "alive"
        if not remaining_alive and not explicit_tools:
            await _until(lambda: bool(chronicle.facts("quest_outcome")))
            assert len(llm.calls) == len(trace["steps"]) == 2
            assert "accept_quest" in {t.name for t in llm.calls[1][2]}
            assert llm.sent_tools[0] == llm.sent_tools[1]  # the schema prefix stays fixed
            result = next(m for m in llm.calls[1][1] if isinstance(m, ToolResult))
            assert result.output.endswith(ACCEPT_NOW)
            assert trace["steps"][1]["calls"][0]["outcome"] == "accepted"
            change = next(e for e in trace["events"] if e["kind"] == "tools_changed")
            assert "accept_quest" not in change["before"] and "accept_quest" in change["after"]
            effects = next(i for i, m in enumerate(fake.received) if m["type"] == "story_effect")
            assert {"do": "set", "var": QUEST, "value": 2} in fake.received[effects]["effects"]
            assert {"do": "xp", "amount": 100} in fake.received[effects]["effects"]
            assert chronicle.facts("quest_outcome")[0]["how"] == "amended"
            said = [i for i, m in enumerate(fake.received) if m["type"] == "say"]
            assert fake.said == ["Его пощадим.", "Дело сделано."] and max(said) < effects
        else:
            assert len(llm.calls) == len(trace["steps"]) == 1
            assert not trace["steps"][0]["calls"][0]["result"].endswith(ACCEPT_NOW)
            assert fake.vars[QUEST] == 1 and chronicle.facts("quest_outcome") == []
            if explicit_tools:
                assert not any(e["kind"] == "tools_changed" for e in trace["events"])
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize(
    "leak", ["give_item?", "set_intent?", "give_quest?", "turn_signal?", "functions.give_quest({...})"]
)
async def test_tool_names_are_filtered_from_base_extension_and_turn_tools(leak, caplog):
    fake = FakeGame()
    llm = FakeBackend(leak, chunk=2, delay=0)
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None), tools=False)
        dialogue.extensions.append(HiddenQuestTool())
        conv = dialogue.open(XARDAS, "Ксардас")
        await dialogue._turn(
            conv, Message("user", "Что скажешь?"), "player", tools=[Tool("turn_signal", "Signal.")]
        )
        await _until(lambda: fake.statuses[-1:] == ["idle"])

    assert fake.said == []
    assert not [m for m in fake.received if m["type"] == "chat_text" and m["text"]]
    assert conv.history == [Message("user", "Что скажешь?")]
    assert any(leak in r.getMessage() and r.levelname == "WARNING" for r in caplog.records)


@pytest.mark.parametrize(
    ("reply", "clean", "leak"),
    [
        (
            "Ладно, пошли. give_item(item=ITMW_MACE)? После боя поговорим.",
            "Ладно, пошли. После боя поговорим.",
            "give_item(item=ITMW_MACE)?",
        ),
        ("Жди здесь. set_intent", "Жди здесь.", "set_intent"),
        (
            "Держи. give_item(item=ITMI_GOLD)\nЛадно, пошли.",
            "Держи. Ладно, пошли.",
            "give_item(item=ITMI_GOLD)",
        ),
        (
            "Держи. give_item(item=ITMI_GOLD)\n\nЛадно, пошли.",
            "Держи. Ладно, пошли.",
            "give_item(item=ITMI_GOLD)",
        ),
        (
            "Вот это разговор\nprotoc function give_quest?",
            "Вот это разговор",
            "protoc function give_quest?",
        ),
        (
            "Держи. functions.give_quest({...})\nЛадно, пошли.",
            "Держи. Ладно, пошли.",
            "functions.give_quest({...})",
        ),
    ],
)
async def test_only_the_service_sentence_is_removed_from_a_merged_line_or_tail(
    reply, clean, leak, caplog
):
    fake = FakeGame()
    async with connected(fake) as link:
        dialogue = DialogueManager(link, FakeBackend(reply, chunk=3, delay=0), PersonaProvider(None))
        conv = dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Ладно")
        await _until(lambda: fake.statuses[-1:] == ["idle"])

    assert " ".join(fake.said) == clean
    assert conv.history[-1] == Message("assistant", clean)
    assert [m["text"] for m in fake.received if m["type"] == "chat_text"][-1] == clean
    assert any(
        leak in r.getMessage() and XARDAS in r.getMessage() and r.levelname == "WARNING"
        for r in caplog.records
    )


async def test_ordinary_speech_is_not_filtered(caplog):
    reply = (
        "Дай мне функцию подумать. Это задание как раз по тебе. Отдам тебе булаву после боя. "
        "Xardas знает больше. Ладно, пошли."
    )
    fake = FakeGame()
    async with connected(fake) as link:
        dialogue = DialogueManager(link, FakeBackend(reply, chunk=3, delay=0), PersonaProvider(None))
        conv = dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Что скажешь?")
        await _until(lambda: fake.statuses[-1:] == ["idle"])

    assert " ".join(fake.said) == reply
    assert conv.history[-1] == Message("assistant", reply)
    assert not [r for r in caplog.records if r.name == "gothic_ai.dialogue" and r.levelname == "WARNING"]


async def test_new_player_text_interrupts_the_reply_in_flight():
    fake = FakeGame()
    llm = FakeBackend("Слишком долгий ответ, который никто не дослушает до конца. " * 5, delay=0.05)
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        dialogue.open(XARDAS, "Ксардас")
        first = dialogue.say_as_player(XARDAS, "Расскажи всё")
        await _until(lambda: any(m["type"] == "chat_text" for m in fake.received))
        assert not first.done()
        second = dialogue.say_as_player(XARDAS, "Стоп")
        await second
        assert first.cancelled()

    roles = [m.role for m in dialogue.conversations[XARDAS].history]
    assert roles == ["user", "assistant", "user", "assistant"]  # partial reply kept, turns stay paired


class FailingBackend:
    name = "failing"

    async def stream(
        self, *, instructions: str, messages: list[Message], tools=None, tool_choice=None, **_
    ) -> AsyncIterator[LLMEvent]:
        raise LLMError("plan limit reached")
        yield  # pragma: no cover


async def test_llm_error_is_reported_to_the_game_and_history_untouched():
    fake = FakeGame()
    async with connected(fake) as link:
        dialogue = DialogueManager(link, FailingBackend(), PersonaProvider(None))
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Привет")
        await _until(lambda: fake.statuses[-1:] == ["error"])

    assert dialogue.conversations[XARDAS].history == []
    assert fake.said == []


class StallingBackend:
    """Goes quiet on its first `stalls` calls (after `said`, if any), then answers. Counts the streams that
    were closed: a stalled one must not be left holding its connection."""

    name = "stalling"

    def __init__(self, reply: str, stalls: int = 1, said: str = "") -> None:
        self.reply, self.stalls, self.said = reply, stalls, said
        self.calls = self.closed = 0

    async def stream(
        self, *, instructions: str, messages: list[Message], tools=None, tool_choice=None, **_
    ) -> AsyncIterator[LLMEvent]:
        self.calls += 1
        try:
            if self.calls <= self.stalls:
                if self.said:
                    yield TextDelta(self.said)
                await asyncio.sleep(3600)
            yield TextDelta(self.reply)
            yield Done(self.reply, (Message("assistant", self.reply),))
        finally:
            self.closed += 1


async def _quick(dialogue: DialogueManager) -> DialogueManager:
    dialogue.llm_first_s = dialogue.llm_idle_s = 0.2
    return dialogue


async def test_a_model_gone_quiet_is_asked_again():
    fake = FakeGame()
    llm = StallingBackend("Я здесь, говори.")
    async with connected(fake) as link:
        dialogue = await _quick(DialogueManager(link, llm, PersonaProvider(None)))
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Ты тут?")
        await _until(lambda: fake.statuses[-2:] == ["thinking", "idle"])

    assert fake.said == ["Я здесь, говори."]
    assert (llm.calls, llm.closed) == (2, 2)  # the stalled stream was closed, not left hanging


async def test_a_model_gone_quiet_after_some_words_is_not_asked_again():
    fake = FakeGame()
    llm = StallingBackend("Это повтор.", said="Сейчас скажу. ")
    async with connected(fake) as link:
        dialogue = await _quick(DialogueManager(link, llm, PersonaProvider(None)))
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Ну?")
        await _until(lambda: fake.statuses[-1:] == ["error"])

    assert llm.calls == 1  # asked again, it would say its first words twice
    assert fake.said == ["Сейчас скажу."]
    history = dialogue.conversations[XARDAS].history
    assert [(m.role, m.content) for m in history] == [("user", "Ну?"), ("assistant", "Сейчас скажу.")]


async def test_a_model_quiet_twice_gives_up():
    fake = FakeGame()
    llm = StallingBackend("Поздно.", stalls=2)
    async with connected(fake) as link:
        dialogue = await _quick(DialogueManager(link, llm, PersonaProvider(None)))
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Эй")
        await _until(lambda: fake.statuses[-1:] == ["error"])

    assert (llm.calls, fake.said) == (2, [])  # one retry a turn


async def test_m1_scenario_with_fake_llm():
    fake = FakeGame()
    async with connected(fake) as link:
        dialogue = DialogueManager(link, FakeBackend(delay=0), PersonaProvider(None))
        lines = await asyncio.wait_for(scenarios.m1_chat(link, dialogue), 10)

    assert len(lines) >= 2
    assert fake.attached == set()  # returned to routine


async def test_tool_call_runs_in_the_game_and_its_outcome_becomes_a_note():
    fake = FakeGame()
    llm = FakeBackend(
        "Возьми это зелье, оно тебе пригодится.",
        delay=0,
        tool_calls=[ToolCall("give_item", {"item": "ITPO_HEALTH_01", "count": 2})],
    )
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        await link.attach(XARDAS)
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Дай что-нибудь")
        await _until(lambda: any(m.role == "developer" for m in dialogue.conversations[XARDAS].history))

    give = next(m for m in fake.received if m["type"] == "action")
    assert give["action"] == "give_item" and give["args"] == {"item": "ITPO_HEALTH_01", "count": 2}
    history = dialogue.conversations[XARDAS].history
    assert [m.role for m in history] == ["user", "assistant", "developer"]
    assert history[-1].content == "[Ты отдал герою: Эликсир лечения ×2]"


async def test_end_conversation_waits_for_the_goodbye_to_be_spoken():
    fake = FakeGame()
    llm = FakeBackend(
        "Довольно. Ступай, мне нужно работать.", delay=0, tool_calls=[ToolCall("end_conversation", {})]
    )
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        ended: list[tuple[str, int]] = []
        dialogue.on_end = lambda npc: ended.append((npc, len(fake.said)))
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Ты старый дурак")
        assert not ended  # goodbye not spoken yet
        await _until(lambda: bool(ended), timeout=6)

    assert ended == [(XARDAS, 1)] and fake.said  # once the goodbye was said
    assert dialogue.conversations[XARDAS].history[-1].content == "[Ты закончил разговор]"


async def test_reply_text_streams_to_the_chat_before_its_lines():
    fake = FakeGame()
    llm = FakeBackend(
        "Ты наконец вернулся из храма. Хорошо, теперь слушай меня внимательно.", chunk=4, delay=0.02
    )
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Где я?")
        await _until(lambda: any(m["type"] == "chat_text" and m["final"] for m in fake.received))

    texts = [m for m in fake.received if m["type"] == "chat_text"]
    says = [m for m in fake.received if m["type"] == "say"]
    assert len(texts) > 2 and texts[-1]["text"] == llm.reply and texts[-1]["final"]
    assert all(len(a["text"]) <= len(b["text"]) for a, b in zip(texts, texts[1:], strict=False))
    assert {m["reply"] for m in texts} == {s["reply"] for s in says}
    # the first words were on screen before the first line was complete
    first_text = fake.received.index(texts[0])
    assert first_text < fake.received.index(says[0]) and len(texts[0]["text"]) < len(says[0]["text"])


class CountedBackend(FakeBackend):
    """Counts model chunks before dialogue consumes them, independently of its line buffering."""

    chunks = 0

    async def stream(self, **kwargs) -> AsyncIterator[LLMEvent]:
        async for event in super().stream(**kwargs):
            if isinstance(event, TextDelta):
                self.chunks += 1
            yield event


@pytest.mark.parametrize(
    ("reply", "expected", "sentence_end_chunk"),
    [
        (
            "Ты вернулся из храма, и это хорошо. Теперь слушай меня внимательно.",
            [
                ("Ты вернулся из храма,", 8),
                ("и это хорошо. Теперь слушай меня внимательно.", 23),
            ],
            12,
        ),
        (
            "Когда ты пойдёшь в город через северные ворота мимо стражи найди там Лестера.",
            [
                ("Когда ты пойдёшь в город через северные ворота мимо стражи", 20),
                ("найди там Лестера.", 26),
            ],
            26,
        ),
    ],
    ids=["first-comma", "first-words-max"],
)
async def test_ordinary_speech_reaches_say_on_the_same_chunks_as_main(
    reply, expected, sentence_end_chunk, monkeypatch
):
    # Constants were measured through origin/main's DialogueManager with the same three-character chunks.
    fake = FakeGame()
    llm = CountedBackend(reply, chunk=3, delay=0)
    said: list[tuple[str, int]] = []
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None), tts=TtsClient("ws://unused"))

        async def record_say(conv, text, *args, **kwargs):
            said.append((text, llm.chunks))

        monkeypatch.setattr(dialogue, "_say", record_say)
        conv = dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Что скажешь?")
        await _until(lambda: fake.statuses[-1:] == ["idle"])

    assert said == expected
    assert said[0][1] < sentence_end_chunk
    assert conv.history[-1] == Message("assistant", reply)


async def test_a_fragmented_tool_call_drops_its_arguments_after_a_short_sentence(caplog, monkeypatch):
    clean = "Держи."
    leak = 'functions.give_item({"item": "ITMI_GOLD", "count": 50})'
    fake = FakeGame()
    entries: list[dict] = []
    monkeypatch.setattr(dialogue_module, "CHAT_TEXT_EVERY", 0)
    async with connected(fake) as link:
        dialogue = DialogueManager(
            link,
            FakeBackend(clean + " " + leak, chunk=3, delay=0),
            PersonaProvider(None),
            journal=Journal(entries.append),
        )
        conv = dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Дашь золота?")
        await _until(lambda: fake.statuses[-1:] == ["idle"])

    assert fake.said == [clean]
    texts = [m["text"] for m in fake.received if m["type"] == "chat_text"]
    assert texts[-1] == clean
    assert all(clean.startswith(text.strip()) for text in texts)
    assert conv.history[-1] == Message("assistant", clean)
    assert dialogue.memory.load("test-branch", XARDAS).entries[-1].content == clean
    assert [entry["text"] for entry in entries if entry["kind"] == "npc"] == [clean]
    assert any(
        leak in r.getMessage() and XARDAS in r.getMessage() and r.levelname == "WARNING"
        for r in caplog.records
    )


@pytest.mark.parametrize(
    ("leak", "suffix"),
    [
        ('give_item({"reason": "Ты помог. Бери."})', ""),
        ('give_item({"reason": "Ты помог. Бери."})', " Ну, бывай."),
        (
            'give_item({"reasons": ["Ты помог.",\n"Забирай обещанное золото."]})',
            " Ну, бывай.",
        ),
        ('give_item "Ты помог. Бери.', ""),
    ],
    ids=["reason", "reason-before-speech", "multiline-array-before-speech", "unclosed-quote"],
)
async def test_a_tool_call_does_not_end_inside_its_arguments(leak, suffix, caplog, monkeypatch):
    clean = "Держи." + suffix
    fake = FakeGame()
    entries: list[dict] = []
    monkeypatch.setattr(dialogue_module, "CHAT_TEXT_EVERY", 0)
    async with connected(fake) as link:
        dialogue = DialogueManager(
            link,
            FakeBackend("Держи. " + leak + suffix, chunk=3, delay=0),
            PersonaProvider(None),
            journal=Journal(entries.append),
        )
        conv = dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Дашь золота?")
        await _until(lambda: fake.statuses[-1:] == ["idle"])

    assert " ".join(fake.said) == clean
    texts = [m["text"] for m in fake.received if m["type"] == "chat_text"]
    assert texts[-1] == clean
    assert all(clean.startswith(text.strip()) for text in texts)
    assert conv.history[-1] == Message("assistant", clean)
    assert dialogue.memory.load("test-branch", XARDAS).entries[-1].content == clean
    assert [entry["text"] for entry in entries if entry["kind"] == "npc"] == [clean]
    assert any(
        leak in r.getMessage() and XARDAS in r.getMessage() and r.levelname == "WARNING"
        for r in caplog.records
    )


async def test_a_late_tool_marker_keeps_the_spoken_clause_and_removes_the_pending_sentence(caplog):
    prefix = "Ты вернулся из храма,"
    leak = "пока ждал тебя function give_item(item=ITMI_GOLD)?"
    clean = prefix + " После боя поговорим."
    fake = FakeGame()
    async with scene_tts(per_char=0.0001) as (url, requests), connected(fake) as link:
        dialogue = DialogueManager(
            link,
            FakeBackend(prefix + " " + leak + " После боя поговорим.", chunk=3, delay=0),
            PersonaProvider(None),
            tts=TtsClient(url),
        )
        conv = dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Что скажешь?")
        await _until(lambda: len(fake.say_ends) == 2 and fake.statuses[-1:] == ["idle"])

    assert fake.said == [prefix, "После боя поговорим."]
    assert [r["text"] for r in requests] == fake.said
    assert conv.history[-1] == Message("assistant", clean)
    assert [m["text"] for m in fake.received if m["type"] == "chat_text"][-1] == clean
    assert any(leak in r.getMessage() and r.levelname == "WARNING" for r in caplog.records)


async def test_mood_tags_set_the_face_of_the_lines_and_are_not_said():
    fake = FakeGame()
    llm = FakeBackend(
        "[angry] Опять ты здесь? Чего тебе нужно? [friendly] Ладно, проходи, раз уж пришёл.", delay=0
    )
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Привет")
        await _until(lambda: fake.statuses[-2:] == ["thinking", "idle"])

    says = [m for m in fake.received if m["type"] == "say"]
    assert [(s["text"], s.get("mood")) for s in says] == [
        ("Опять ты здесь? Чего тебе нужно?", "angry"),
        ("Ладно, проходи, раз уж пришёл.", "friendly"),
    ]
    final = [m for m in fake.received if m["type"] == "chat_text"][-1]
    assert final["text"] == "Опять ты здесь? Чего тебе нужно? Ладно, проходи, раз уж пришёл."
    assert dialogue.conversations[XARDAS].history[-1].content == final["text"]
    instructions, _, _ = llm.calls[-1]
    assert "[angry]" in instructions  # the model is told how to tag


async def test_lines_without_a_tag_have_no_mood():
    fake = FakeGame()
    llm = FakeBackend("Ты наконец вернулся из храма. Хорошо, теперь слушай меня внимательно.", delay=0)
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Привет")
        await _until(lambda: fake.statuses[-2:] == ["thinking", "idle"])
    assert all(m.get("mood") is None for m in fake.received if m["type"] == "say")


class OneVoiceGame(FakeGame):
    """Keeps selected ordinary lines queued in the game instead of starting them at once."""

    def __init__(self, queued: tuple[str, ...] = ()) -> None:
        super().__init__()
        self.queued = set(queued)

    async def handle(self, msg) -> None:
        if msg["type"] == "say" and msg["text"] in self.queued:
            self.drop_says.add(msg["npc"])
            try:
                await super().handle(msg)
            finally:
                self.drop_says.discard(msg["npc"])
            return
        await super().handle(msg)


class OneVoiceTts(TtsClient):
    """Streams a short fake voice; selected lines wait before their first sound."""

    def __init__(self, audio_s: float) -> None:
        super().__init__()
        self.audio_s = audio_s
        self.requested: list[str] = []
        self.held: dict[str, asyncio.Event] = {}
        self.finishing: dict[str, asyncio.Event] = {}
        self.failed: set[str] = set()

    async def stream(self, text, voice_id, description, on_start, on_chunk, style="") -> Speech:
        self.requested.append(text)
        if text in self.held:
            await self.held[text].wait()
        if text in self.failed:
            raise TtsError("no voice")
        await on_start(16000, "fake")
        await on_chunk(b"\0\1" * 16)
        if text in self.finishing:
            await self.finishing[text].wait()
        return Speech(b"", self.audio_s, "fake", 0.0)


@asynccontextmanager
async def one_voice_dialogue(with_tts, llm, *, queued=(), audio_s=0.1):
    fake = OneVoiceGame(queued)
    async with connected(fake) as link:
        dialogue = DialogueManager(
            link, llm, PersonaProvider(None), tts=OneVoiceTts(audio_s) if with_tts else None
        )
        conv = dialogue.open(XARDAS, "Ксардас")
        try:
            yield fake, dialogue, conv
        finally:
            tasks = conv.tasks()
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)


@pytest.mark.parametrize("trigger", ["player", "event"])
@pytest.mark.parametrize("with_tts", [True, False])
async def test_a_turn_cancels_svm_waiting_to_be_heard(tmp_path, trigger, with_tts):
    words = "Вон отсюда!"
    note = Message("developer", f"[Ты сказал вслух: «{words}»]")
    llm = FakeBackend("Теперь я тебя слушаю.", delay=0, first=60)
    writer = TraceWriter(tmp_path)
    async with one_voice_dialogue(with_tts, llm) as (fake, dialogue, conv):
        dialogue.trace = writer
        if with_tts:
            held = dialogue.tts.held[words] = asyncio.Event()
        else:
            await dialogue._say(conv, "Сначала я должен закончить свою прежнюю мысль.", None, overlay=True)
            await _until(lambda: bool(fake.said))
            await dialogue.link.snapshot(XARDAS, fresh=True)
            conv.said_at -= 3.05  # the cooldown passed while the long overlay is still running
            dialogue._spoke_at[XARDAS] -= 3.05
        previous = list(fake.said)
        dialogue.quick = FakeBackend(words, delay=0)
        dialogue._nearby[XARDAS] = ("Ксардас", 100)
        conv.talked_at = float("-inf")
        svm = SvmLine(
            type="svm_line", id="svm-held", npc=XARDAS, svm="$GETOUTOFHERE",
            text="Убирайся отсюда!", overlay=True,
        )
        dialogue._on_message(svm)
        if with_tts:
            await _until(lambda: words in dialogue.tts.requested and not dialogue._svm_tasks)
            assert conv.voice.empty()  # TTS already owns the line
        else:
            await _until(lambda: any(text == words for text, _ in dialogue._lines.values()))
            assert XARDAS in dialogue._svm_tasks  # _say is waiting for the previous overlay
        assert fake.said == previous and not writer.path().exists()
        assert not conv.svm_phrases and note not in conv.history

        dialogue.quick = None  # isolate the held SVM from the new turn's first words
        turn = (dialogue.say_as_player(XARDAS, "Подожди, поговорим.", overlay=True)
                if trigger == "player"
                else dialogue.event_turn(XARDAS, "[Событие: герой заговорил]", overlay=True))
        await _until(lambda: bool(llm.calls))
        if with_tts:
            held.set()
            await asyncio.wait_for(conv.voice.join(), 1)
        else:
            dialogue._reset_overlay(conv)
            await _until(lambda: not dialogue._svm_tasks)
        await dialogue.link.snapshot(XARDAS, fresh=True)  # drain preceding game messages
        assert not turn.done()
        assert fake.said == previous and not fake.say_chunks
        assert not conv.svm_phrases and note not in conv.history
        (record,) = [r for r in map(json.loads, writer.path().read_text().splitlines())
                     if r["type"] == "svm_line"]
        outcome = "dropped" if with_tts and trigger == "player" else "busy"
        assert record["id"] == "svm-held" and record["outcome"] == outcome
        assert record["answer"] == words and record["elapsed_ms"] >= 0

        turn.cancel()
        await asyncio.gather(turn, return_exceptions=True)
        dialogue.quick = FakeBackend(words, delay=0)
        conv.said_at = conv.talked_at = float("-inf")
        conv.overlay_free_at = 0
        dialogue._on_message(svm.model_copy(update={"id": "svm-retry"}))
        await _until(lambda: fake.said == [*previous, words] and bool(conv.svm_phrases))
        assert len(conv.svm_phrases) == 1 and conv.history.count(note) == 1
        records = [r for r in map(json.loads, writer.path().read_text().splitlines())
                   if r["type"] == "svm_line"]
        assert [(r["id"], r["outcome"]) for r in records] == [
            ("svm-held", outcome), ("svm-retry", "said"),
        ]


@pytest.mark.parametrize("drop", ["queue", "interrupt"])
async def test_queued_svm_counts_for_cooldown_and_repeats_and_finishes_its_dropped_trace(tmp_path, drop):
    words = "Вон отсюда!"
    note = Message("developer", f"[Ты сказал вслух: «{words}»]")
    writer = TraceWriter(tmp_path)
    async with one_voice_dialogue(True, FakeBackend()) as (fake, dialogue, conv):
        dialogue.trace = writer
        blocker = "Сначала закончу мысль." if drop == "queue" else words
        held = dialogue.tts.held[blocker] = asyncio.Event()
        if drop == "queue":
            await dialogue._say(conv, blocker, None)
            await _until(lambda: blocker in dialogue.tts.requested)
        dialogue.quick = FakeBackend(words, delay=0)
        dialogue._nearby[XARDAS] = ("Ксардас", 100)
        conv.said_at = float("-inf")
        svm = SvmLine(
            type="svm_line", id="queued", npc=XARDAS, svm="$GETOUTOFHERE",
            text="Убирайся отсюда!", overlay=True,
        )
        dialogue._on_message(svm)
        await _until(lambda: bool(conv.svm_pending) and not dialogue._svm_tasks)
        assert conv.voice.qsize() == (1 if drop == "queue" else 0)

        conv.said_at = float("-inf")  # the pending line's own clock enforces its cooldown
        dialogue._on_message(svm.model_copy(update={"id": "recent"}))
        assert len(dialogue.quick.calls) == 1
        for line in conv.svm_pending:
            dialogue._sent[line] -= 3.05
        dialogue.quick = FakeBackend("ВОН ОТСЮДА!!!", delay=0)
        dialogue._on_message(svm.model_copy(update={"id": "repeat"}))
        await _until(lambda: not dialogue._svm_tasks)
        assert not conv.svm_phrases and note not in conv.history

        if drop == "queue":
            dialogue._drop_unvoiced(conv)
        else:
            assert dialogue.interrupt(XARDAS)
        assert not conv.svm_pending
        held.set()
        await asyncio.wait_for(conv.voice.join(), 1)
        await dialogue.link.snapshot(XARDAS, fresh=True)
        assert words not in fake.said and not conv.svm_phrases and note not in conv.history
        records = [r for r in map(json.loads, writer.path().read_text().splitlines())
                   if r["type"] == "svm_line"]
        assert [(r["id"], r["outcome"]) for r in records] == [
            ("recent", "busy"), ("repeat", "repeat"), ("queued", "dropped"),
        ]


async def test_a_started_svm_is_audible_and_can_be_interrupted():
    words = "Вон отсюда!"
    previous = "Наш разговор пока закончен."
    llm = FakeBackend(previous, delay=0)
    async with one_voice_dialogue(True, llm, audio_s=2.0) as (fake, dialogue, conv):
        talk = TalkDesk(dialogue.link, dialogue)
        assert talk.begin(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Расскажи.")
        await _until(
            lambda: bool(fake.say_ends) and dialogue.started_at(fake.says[0][0]["line"]) is not None
        )
        old_line = fake.says[0][0]["line"]
        assert conv.last_lines == [old_line] and conv.reply_id
        old_at = time.monotonic() - 4  # the earlier answer ended and its cooldown passed
        dialogue._lines[old_line] = (previous, old_at)
        dialogue._spoke_at[XARDAS] = conv.said_at = conv.overlay_free_at = old_at
        dialogue.quick = FakeBackend(words, delay=0)
        dialogue._nearby[XARDAS] = ("Ксардас", 100)
        dialogue._on_message(SvmLine(
            type="svm_line", id="speaking", npc=XARDAS, svm="$GETOUTOFHERE",
            text="Убирайся отсюда!", overlay=True,
        ))
        await _until(
            lambda: len(fake.say_ends) == 2 and dialogue.started_at(fake.says[1][0]["line"]) is not None
        )
        assert not conv.svm_pending
        assert dialogue.audible(XARDAS) and dialogue.speaking(XARDAS)
        history = list(conv.history)
        await fake.send(type="ptt_start", focus=XARDAS)
        await _until(lambda: bool(fake.hushed))
        assert fake.hushed == [{"type": "hush", "npc": XARDAS}]
        assert conv.history == history
        assert not dialogue.audible(XARDAS) and not dialogue.speaking(XARDAS)


@pytest.mark.parametrize("with_tts", [True, False])
@pytest.mark.parametrize("first_started", [True, False])
async def test_an_overlay_event_cuts_the_unheard_previous_reply(with_tts, first_started):
    old_texts = (
        "Я обещал ему пощаду и сдержу слово.",
        "Не лезь в драку раньше времени.",
        "Мы ещё поговорим об этом позже.",
    )
    old_reply = " ".join(
        f"[{mood}] {text}" for mood, text in zip(("neutral", "angry", "friendly"), old_texts, strict=True)
    )
    urgent = "Бандиты уже напали на героя!"
    llm = FakeBackend(script=[FakeStep(old_reply), FakeStep(urgent)], delay=0)
    queued = old_texts[1:] if first_started else old_texts
    async with one_voice_dialogue(with_tts, llm, queued=queued, audio_s=10.0) as (fake, dialogue, conv):
        await dialogue.say_as_player(XARDAS, "Что ты обещал?")
        await _until(lambda: len(fake.says) == 3 and (not with_tts or len(fake.say_ends) == 3))
        lines, reply = conv.last_lines[:], conv.reply_id
        if first_started:
            await _until(lambda: dialogue.started_at(lines[0]) is not None)
        assert all(dialogue.started_at(line) is None for line in lines[int(first_started) :])

        turn = dialogue.event_turn(XARDAS, "[Событие: герой напал на бандита]", overlay=True)
        assert turn is not None
        await turn
        await _until(lambda: any(m["text"] == urgent for m, _ in fake.says))

        said = old_texts[0] if first_started else ""
        expected = {"type": "hush", "npc": XARDAS, "reply": reply}
        if said:
            expected["said"] = said
        assert fake.hushed == [expected]
        overlay = next(m for m, _ in fake.says if m["text"] == urgent)
        assert fake.received.index(expected) < fake.received.index(overlay)
        assert set(lines[int(first_started) :]) <= conv.hushed
        assert overlay["line"] not in conv.hushed and overlay["reply"] != reply
        note = f"Ты оборвал себя на полуслове{f'; успел сказать: «{said}»' if said else ''}"
        assert Message("developer", f"[{note}]") in conv.history


@pytest.mark.parametrize("with_tts", [True, False])
async def test_an_overlay_does_not_hush_an_already_heard_reply(with_tts):
    llm = FakeBackend(script=[FakeStep("Я сдержу своё слово."), FakeStep("Бандиты напали!")], delay=0)
    async with one_voice_dialogue(with_tts, llm) as (fake, dialogue, conv):
        await dialogue.say_as_player(XARDAS, "Ты обещаешь?")
        await _until(lambda: bool(fake.says) and (not with_tts or bool(fake.say_ends)))
        (line,) = conv.last_lines
        await _until(lambda: dialogue.started_at(line) is not None)
        dialogue._lines[line] = (dialogue._lines[line][0], time.monotonic() - 30.0)

        turn = dialogue.event_turn(XARDAS, "[Событие: нападение]", overlay=True)
        assert turn is not None
        await turn
        await _until(lambda: len(fake.says) == 2)

        assert fake.hushed == [] and conv.hushed == set()
        assert not any("Ты оборвал себя" in m.content for m in conv.history)


@pytest.mark.parametrize("with_tts", [True, False])
async def test_two_overlays_of_one_turn_wait_without_hushing_each_other(with_tts, monkeypatch):
    monkeypatch.setattr(dialogue_module, "line_duration", lambda text: 0.2)
    llm = FakeBackend("[angry] Бандиты уже напали на героя! [friendly] Я обязательно тебя прикрою!", delay=0)
    async with one_voice_dialogue(with_tts, llm) as (fake, dialogue, conv):
        turn = dialogue.event_turn(XARDAS, "[Событие: нападение]", overlay=True)
        assert turn is not None
        await _until(lambda: len(fake.says) == 1 and (not with_tts or len(fake.say_ends) == 1))
        first_free_at = conv.overlay_free_at
        await turn
        await _until(lambda: len(fake.says) == 2)

        assert fake.says[1][1] >= first_free_at - 0.005
        assert all(m["overlay"] for m, _ in fake.says)
        assert fake.hushed == [] and conv.hushed == set()
        assert not any("Ты оборвал себя" in m.content for m in conv.history)


@pytest.mark.parametrize("with_tts", [True, False])
async def test_an_ordinary_reply_waits_for_the_sounding_overlay(with_tts, monkeypatch):
    monkeypatch.setattr(dialogue_module, "line_duration", lambda text: 0.2)
    llm = FakeBackend(
        script=[FakeStep("Я обязательно тебя прикрою!"), FakeStep("Теперь можно поговорить.")], delay=0
    )
    async with one_voice_dialogue(with_tts, llm) as (fake, dialogue, conv):
        overlay = dialogue.event_turn(XARDAS, "[Событие: нападение]", overlay=True)
        assert overlay is not None
        await overlay
        await _until(lambda: len(fake.says) == 1 and (not with_tts or len(fake.say_ends) == 1))
        free_at = conv.overlay_free_at
        assert time.monotonic() < free_at

        ordinary = dialogue.event_turn(XARDAS, "[Событие: герой подошёл]")
        assert ordinary is not None
        await ordinary
        await _until(lambda: len(fake.says) == 2)

        assert fake.says[1][1] >= free_at - 0.005
        assert "overlay" not in fake.says[1][0]
        assert fake.hushed == []


@pytest.mark.parametrize("with_tts", [True, False])
async def test_interrupt_releases_the_overlay_wait_for_the_next_reply(with_tts, monkeypatch):
    monkeypatch.setattr(dialogue_module, "line_duration", lambda text: 2.0)
    second = "Я обязательно тебя прикрою!"
    answer = "Теперь я тебя слушаю."
    llm = FakeBackend(
        script=[FakeStep(f"[angry] Бандиты уже напали на героя! [friendly] {second}"), FakeStep(answer)],
        delay=0,
    )
    async with one_voice_dialogue(with_tts, llm, audio_s=2.0) as (fake, dialogue, conv):
        overlay = dialogue.event_turn(XARDAS, "[Событие: нападение]", overlay=True)
        assert overlay is not None
        await _until(
            lambda: len(conv.last_lines) == 2
            and len(fake.says) == 1
            and (not with_tts or (len(fake.say_ends) == 1 and conv.voice.empty()))
        )
        old_free_at = conv.overlay_free_at
        assert dialogue.interrupt(XARDAS)
        assert conv.overlay_free_at <= time.monotonic()
        await asyncio.gather(overlay, return_exceptions=True)

        reply = dialogue.event_turn(XARDAS, "[Событие: герой заговорил]")
        assert reply is not None
        await reply
        await _until(lambda: any(m["text"] == answer for m, _ in fake.says), timeout=1.0)

        assert next(at for m, at in fake.says if m["text"] == answer) < old_free_at
        assert not any(m["text"] == second for m, _ in fake.says)


async def test_an_event_turn_that_must_act_requires_a_call_on_its_first_step_only():
    # a witness's answer (witnesses.py): the npc decides by catch_lie or set_intent, not by words alone
    intent = ToolCall("set_intent", {"goal": "none", "target": None, "attitude": "wary", "agenda": None})
    llm = FakeBackend(script=[FakeStep("", [intent]), FakeStep("Ну смотри у меня.")], delay=0)
    fake = FakeGame()
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        dialogue.open(XARDAS, "Ксардас", warm=False)
        turn = dialogue.event_turn(XARDAS, "[Ответ: …]", tools=("set_intent",), act=True)
        assert turn is not None
        await turn
        assert llm.tool_choices == ["required", None]
        assert dialogue.conversations[XARDAS].intent.attitude == "wary"
        # with no tool offered there is nothing to require
        turn = dialogue.event_turn(XARDAS, "[Событие: …]", tools=(), act=True)
        assert turn is not None
        await turn
        assert llm.tool_choices[2] is None


async def test_an_overlay_hushes_at_first_sound_and_drops_only_old_unvoiced_lines():
    old = "Я обещал ему пощаду и сдержу слово."
    urgent, following = "Бандиты уже напали на героя!", "Я обязательно тебя прикрою!"
    llm = FakeBackend(script=[FakeStep(old), FakeStep(f"[angry] {urgent} [friendly] {following}")], delay=0)
    async with one_voice_dialogue(True, llm, audio_s=0.5) as (fake, dialogue, conv):
        await dialogue.say_as_player(XARDAS, "Что ты обещал?")
        await _until(lambda: bool(fake.say_ends) and dialogue.started_at(conv.last_lines[0]) is not None)
        dialogue._audio[conv.last_lines[0]] = 10.0
        old_reply = conv.reply_id
        dialogue.tts.held[urgent] = asyncio.Event()
        turn = dialogue.event_turn(XARDAS, "[Событие: нападение]", overlay=True)
        assert turn is not None
        await turn
        await _until(lambda: urgent in dialogue.tts.requested)

        pending = []
        for text in ("Не лезь в драку раньше времени.", "Мы ещё поговорим об этом позже."):
            await dialogue._say(conv, text, old_reply)
            line = conv.last_lines.pop()
            conv.earlier_lines.append(line)
            pending.append((line, text))
        assert fake.hushed == [] and not conv.hushed

        dialogue.tts.held[urgent].set()
        await asyncio.wait_for(conv.voice.join(), 2.0)
        await _until(lambda: any(m["text"] == following for m, _ in fake.says))

        assert fake.hushed == [{"type": "hush", "npc": XARDAS, "reply": old_reply, "said": old}]
        assert {line for line, _ in pending} <= conv.hushed
        assert all(text not in dialogue.tts.requested for _, text in pending)
        assert all(m["text"] not in {text for _, text in pending} for m, _ in fake.says)
        assert [m["text"] for m, _ in fake.says] == [old, urgent, following]


async def test_a_stream_finishing_after_interrupt_does_not_restore_the_overlay_wait():
    urgent, answer = "Я обязательно тебя прикрою!", "Теперь я тебя слушаю."
    llm = FakeBackend(script=[FakeStep(urgent), FakeStep(answer)], delay=0)
    async with one_voice_dialogue(True, llm, audio_s=10.0) as (fake, dialogue, conv):
        finishing = dialogue.tts.finishing[urgent] = asyncio.Event()
        overlay = dialogue.event_turn(XARDAS, "[Событие: нападение]", overlay=True)
        assert overlay is not None
        await overlay
        await _until(lambda: bool(fake.says) and dialogue.started_at(conv.last_lines[0]) is not None)
        assert not fake.say_ends
        assert dialogue.interrupt(XARDAS)
        reset_at = conv.overlay_free_at

        reply = dialogue.event_turn(XARDAS, "[Событие: герой заговорил]")
        assert reply is not None
        await reply
        finishing.set()
        await _until(lambda: any(m["text"] == answer for m, _ in fake.says), timeout=1.0)

        assert conv.overlay_free_at == reset_at


async def test_tts_failure_fallback_hushes_old_speech_and_holds_the_next_reply(monkeypatch):
    monkeypatch.setattr(dialogue_module, "line_duration", lambda text: 0.2)
    old, urgent, answer = "Я сдержу своё слово.", "Бандиты уже напали!", "Теперь я тебя слушаю."
    llm = FakeBackend(script=[FakeStep(old), FakeStep(urgent), FakeStep(answer)], delay=0)
    async with one_voice_dialogue(True, llm, audio_s=10.0) as (fake, dialogue, conv):
        await dialogue.say_as_player(XARDAS, "Ты обещаешь?")
        await _until(lambda: bool(fake.say_ends) and dialogue.started_at(conv.last_lines[0]) is not None)
        old_reply = conv.reply_id
        dialogue.tts.failed.add(urgent)
        overlay = dialogue.event_turn(XARDAS, "[Событие: нападение]", overlay=True)
        assert overlay is not None
        await overlay
        await _until(lambda: len(fake.says) == 2)
        free_at = conv.overlay_free_at

        assert fake.hushed == [{"type": "hush", "npc": XARDAS, "reply": old_reply, "said": old}]
        assert fake.received.index(fake.hushed[0]) < fake.received.index(fake.says[1][0])
        assert "stream" not in fake.says[1][0]
        reply = dialogue.event_turn(XARDAS, "[Событие: герой заговорил]")
        assert reply is not None
        await reply
        await _until(lambda: len(fake.says) == 3)

        assert fake.says[2][1] >= free_at - 0.005
        assert dialogue.tts.requested == [old, urgent]


async def test_a_new_mood_starts_a_new_line():
    fake = FakeGame()
    llm = FakeBackend("[friendly] Ты вернулся. [angry] Времени мало, слушай внимательно.", delay=0)
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Привет")
        await _until(lambda: fake.statuses[-2:] == ["thinking", "idle"])
    # "Ты вернулся." is short and would be merged with the next sentence, but its mood is another
    says = [(m["text"], m.get("mood")) for m in fake.received if m["type"] == "say"]
    assert says == [("Ты вернулся.", "friendly"), ("Времени мало, слушай внимательно.", "angry")]


# --- two npc's talk to each other (play_scene) ---------------------------------------------

SCENE = [
    (LOBART, "Лобарт", HILDA, "Слыхала? Этот бродяга мне наврал.", "angry"),
    (HILDA, "Хильда", LOBART, "Да ну? А с виду смирный.", None),
    (LOBART, "Лобарт", HILDA, "Глаз с него не спускай.", None),
]


@asynccontextmanager
async def scene_tts(per_char: float = 0.01, fail: bool = False):
    """TTS service stand-in: `per_char` seconds of audio per character in two chunks, or an error."""
    requests: list[dict] = []

    async def handler(ws) -> None:
        async for raw in ws:
            msg = json.loads(raw)
            requests.append(msg)
            if fail:
                await ws.send(json.dumps({"type": "error", "id": msg["id"], "detail": "no voice"}))
                continue
            start = {"type": "start", "id": msg["id"], "sample_rate": 16000, "provider": "fake"}
            await ws.send(json.dumps(start))
            pcm = b"\0\1" * int(16000 * per_char * len(msg["text"]))
            half = len(pcm) // 4 * 2
            await ws.send(pcm[:half])
            await ws.send(pcm[half:])
            await ws.send(json.dumps({"type": "done", "id": msg["id"]}))

    async with serve(handler, "127.0.0.1", 0) as server:
        yield f"ws://127.0.0.1:{server.sockets[0].getsockname()[1]}", requests


def _scene_dialogue(link, pace: float = 0.05, **kwargs) -> DialogueManager:
    dialogue = DialogueManager(link, FakeBackend(), PersonaProvider(None), **kwargs)
    dialogue.scene_pace = lambda text: pace
    return dialogue


async def test_a_scene_is_said_in_turn_each_line_whole_and_to_the_other():
    fake = FakeGame()
    async with scene_tts() as (url, requests), connected(fake) as link:
        dialogue = _scene_dialogue(link, tts=TtsClient(url))
        play = dialogue.play_scene(SCENE, quiet=True)
        await asyncio.wait_for(play.task, 10)

    says = [m for m, _ in fake.says]
    assert [(m["npc"], m["to"], m["text"]) for m in says] == [(n, to, t) for n, _, to, t, _ in SCENE]
    assert all(m["stream"] and "audio" not in m and "reply" not in m for m in says)
    # each line goes out whole: the game knows its length when it starts (never a WAV: a hush can't stop it)
    flow = [m["type"] for m in fake.received if m["type"] in ("say", "say_audio", "say_end")]
    assert flow == ["say", "say_audio", "say_audio", "say_end"] * 3
    assert fake.say_ends == {line.line: round(line.audio_s, 3) for line in play.lines}
    # the next line only once the one before has been heard: started, its voice over, a breath
    for line, (_, sent_at) in zip(play.lines[:-1], fake.says[1:], strict=True):
        assert line.audio_s == pytest.approx(0.01 * len(line.text), abs=0.01)
        assert sent_at >= line.started + line.audio_s + SCENE_GAP_S - 0.005
    assert (play.sent, play.why, play.said()) == (3, "", play.lines)
    # each in its own voice, lowered, the mood heard; on a connection of its own (answers don't wait)
    assert [r["voice"]["id"] for r in requests] == [LOBART, HILDA, LOBART]
    assert all(QUIET_STYLE in r["style"] for r in requests)
    assert MOOD_DELIVERY["angry"] in requests[0]["style"]
    assert dialogue.tts._ws is None and dialogue._scene_tts is not None
    assert dialogue.conversations == {}  # nobody remembers it here: gossip.py writes the notes


async def test_without_a_voice_a_line_waits_for_the_one_before_to_start_and_be_said():
    fake = FakeGame()
    fake.start_delay = 0.3
    async with connected(fake) as link:
        play = _scene_dialogue(link).play_scene(SCENE[:2])
        await asyncio.wait_for(play.task, 5)

    (first, t0), (second, t1) = fake.says
    assert "stream" not in first and first["to"] == HILDA and first["mood"] == "angry"
    assert second["to"] == LOBART and "mood" not in second
    assert t1 - t0 >= 0.3 + 0.05 + SCENE_GAP_S - 0.005  # started late, said, then the breath
    assert [line.audio_s for line in play.lines] == [None, None]


async def test_a_line_the_game_never_starts_ends_the_scene(monkeypatch):
    monkeypatch.setattr(dialogue_module, "SCENE_START_S", 0.3)
    fake = FakeGame()
    fake.drop_says = {HILDA}  # she can't speak now: the bridge drops her line without a word
    async with connected(fake) as link:
        dialogue = _scene_dialogue(link)
        play = dialogue.play_scene(SCENE)
        await asyncio.wait_for(play.task, 5)
        await _until(lambda: bool(fake.hushed))

    assert [m["npc"] for m, _ in fake.says] == [LOBART, HILDA]  # the third is never sent
    assert fake.hushed == [{"type": "hush", "npc": HILDA}]  # if it starts later, it is cut at once
    assert (play.sent, play.why, play.said()) == (2, "not_started", play.lines[:1])
    assert dialogue._starts == {}


async def test_the_hero_cuts_a_scene_both_stop():
    fake = FakeGame()
    async with connected(fake) as link:
        dialogue = _scene_dialogue(link, pace=0.5)
        play = dialogue.play_scene(SCENE)
        await _until(lambda: play.sent == 2 and dialogue.started_at(play.lines[1].line) is not None)
        said = play.cut()
        assert said == play.lines[:2] and play.cut() == said  # once
        await asyncio.wait({play.task})  # a cut play does not raise
        await asyncio.sleep(0.9)  # the third line's time

    second = play.lines[1]
    assert fake.hushed == [
        {"type": "hush", "npc": HILDA, "line": second.line, "said": second.text},  # keeps her subtitle
        {"type": "hush", "npc": LOBART},
    ]
    assert len(fake.says) == 2 and play.why == "cut"


async def test_a_line_sent_but_not_started_is_hushed_plainly_and_a_load_hushes_nothing():
    fake = FakeGame()
    fake.start_delay = 0.5
    async with connected(fake) as link:
        dialogue = _scene_dialogue(link)
        play = dialogue.play_scene(SCENE)
        await _until(lambda: play.sent == 1)
        assert play.cut() == []
        await _until(lambda: len(fake.hushed) == 2)
        assert fake.hushed == [{"type": "hush", "npc": LOBART}, {"type": "hush", "npc": HILDA}]

        fake.hushed.clear()
        fake.start_delay = 0
        loaded = dialogue.play_scene(SCENE)
        await _until(lambda: dialogue.started_at(loaded.lines[0].line) is not None)
        assert loaded.cut(hush=False) == loaded.lines[:1]  # the world is gone: nothing to stop
        await asyncio.sleep(0.4)

    assert fake.hushed == [] and [m["text"] for m, _ in fake.says][-1] == SCENE[0][3]
    assert len(fake.says) == 2


async def test_a_voice_that_fails_leaves_the_scene_in_subtitles():
    fake = FakeGame()
    journal: list[dict] = []
    async with scene_tts(fail=True) as (url, requests), connected(fake) as link:
        dialogue = _scene_dialogue(link, tts=TtsClient(url), journal=Journal(journal.append))
        play = dialogue.play_scene(SCENE)
        await asyncio.wait_for(play.task, 5)

    assert [m["text"] for m, _ in fake.says] == [t for _, _, _, t, _ in SCENE]
    assert not any(m.get("stream") or m.get("audio") for m, _ in fake.says)
    assert len(requests) == 1  # the voice is off for a while: the next lines don't try it
    assert dialogue._tts_paused_until - time.monotonic() > TTS_PAUSE - 5
    assert [e["kind"] for e in journal] == ["tts_off"]


async def test_notes_do_not_warm_the_quick_model_but_talking_to_the_npc_does(monkeypatch):
    monkeypatch.setattr(dialogue_module, "PREWARM_DELAY_S", 0)
    fake = FakeGame()
    quick = FakeBackend("Хм...", delay=0)
    async with connected(fake) as link:
        dialogue = DialogueManager(link, FakeBackend(), PersonaProvider(None), quick=quick)
        dialogue.remember(HILDA, "Хильда", "Лобарт рассказал тебе о герое")  # a rumor
        dialogue.overheard(HILDA, "Хильда", "Привет", None)
        said = [("Лобарт", "Слыхала?"), ("Хильда", "Да ну?")]
        dialogue.overheard_talk(XARDAS, "Ксардас", ("Лобарт", "Хильда"), said)
        await asyncio.sleep(0.05)
        assert quick.prewarms == []
        dialogue.open(HILDA, "Хильда")  # the hero talks to her: her history, notes and all, is read now
        await _until(lambda: len(quick.prewarms) == 1)
        assert quick.prewarms[0][1] == dialogue.conversations[HILDA].history

    (heard,) = [m.content for m in dialogue.conversations[XARDAS].history]
    assert heard == "[Ты слышал, как Лобарт и Хильда говорили о герое: Лобарт: «Слыхала?» — Хильда: «Да ну?»]"


async def test_quick_slots_evict_the_least_recent_idle_npc(monkeypatch):
    monkeypatch.setattr(dialogue_module, "PREWARM_DELAY_S", 0)
    quick = FakeBackend("Есть!", delay=0)
    dialogue = DialogueManager(GameLink(SimpleNamespace(send=AsyncMock())), FakeBackend(),
                               PersonaProvider(None), quick=quick)
    for npc in (XARDAS, LOBART, HILDA):
        conv = dialogue.open(npc, npc, warm=False)
        dialogue._start_prewarm(conv, [XARDAS, LOBART, HILDA])
    await asyncio.gather(*list(dialogue._prewarms.values()))
    assert dialogue._quick_slots == {XARDAS: 0, LOBART: 1, HILDA: 2}
    assert quick.prewarm_slot_ids == [0, 1, 2]
    async with dialogue._quick_request(XARDAS):
        await dialogue._quick_words(dialogue.conversations[LOBART], [])
        await dialogue._quick_words(dialogue.conversations[HILDA], [])
        newcomer = dialogue.open("NEW", "Стражник", warm=False)
        await dialogue._quick_words(newcomer, [])
        assert dialogue._quick_slots == {XARDAS: 0, HILDA: 2, "NEW": 1}
    await dialogue._quick_words(dialogue.open("LAST", "Последний", warm=False), [])
    assert dialogue._quick_slots == {HILDA: 2, "NEW": 1, "LAST": 0}
    assert quick.slot_ids == [1, 2, 1, 0]


async def test_a_real_quick_request_cancels_its_warm_and_blocks_new_warms(monkeypatch):
    from gothic_ai.llm.base import QUICK_SLOT

    monkeypatch.setattr(dialogue_module, "PREWARM_DELAY_S", 0)
    quick = FakeBackend("Есть!", delay=0)
    started, cancelled = asyncio.Event(), asyncio.Event()
    warm_slots = []

    async def held_warm(**kwargs):
        warm_slots.append(QUICK_SLOT.get())
        started.set()
        try:
            await asyncio.Event().wait()
        finally:
            cancelled.set()

    monkeypatch.setattr(quick, "prewarm", held_warm)
    dialogue = DialogueManager(GameLink(SimpleNamespace(send=AsyncMock())), FakeBackend(),
                               PersonaProvider(None), quick=quick)
    conv = dialogue.open(LOBART, "Лобарт", warm=False)
    dialogue._start_prewarm(conv, [LOBART])
    await asyncio.wait_for(started.wait(), 1)
    words, _ = await asyncio.wait_for(dialogue._quick_words(conv, []), 1)
    await _until(lambda: cancelled.is_set() and not dialogue._prewarms)
    assert words == "Есть!" and quick.slot_ids == warm_slots == [0]
    async with dialogue._quick_request(LOBART):
        dialogue._start_prewarm(conv, [LOBART])
        await asyncio.sleep(0)
        assert not dialogue._prewarms and warm_slots == [0]


@pytest.mark.parametrize("kind", ["first_words", "svm", "combat_bark", "fight", "order"])
async def test_all_npc_quick_requests_use_the_prewarmed_slot(kind, monkeypatch):
    from gothic_ai.combat_barks import CombatBarks
    from gothic_ai.protocol import CombatHit

    monkeypatch.setattr(dialogue_module, "PREWARM_DELAY_S", 0)
    quick = FakeBackend("[pain] Ай!", delay=0)
    async with connected(FakeGame()) as link:
        dialogue = DialogueManager(link, FakeBackend("", delay=0), PersonaProvider(None), quick=quick)
        conv = dialogue.open(LOBART, "Лобарт", warm=False)
        dialogue._nearby[LOBART] = ("Лобарт", 100)
        dialogue._start_prewarm(conv, [LOBART])
        await _until(lambda: bool(quick.prewarms))
        if kind == "first_words":
            await dialogue._quick_words(conv, [])
        elif kind == "svm":
            dialogue._start_svm(SvmLine(type="svm_line", id="s", npc=LOBART,
                                        svm="$NOTNOW", text="Нет!", overlay=True))
            await asyncio.gather(*list(dialogue._svm_tasks.values()))
        elif kind == "combat_bark":
            barks = CombatBarks(dialogue)
            link._dispatch(NpcRoster(type="npc_roster", t=0, npcs=[
                {"id": LOBART, "name": "Лобарт", "distance": 100},
            ]))
            link._dispatch(CombatHit(type="combat_hit", npc=LOBART, by="PLAYER", damage=5,
                                    hp=95, hp_max=100, down=False, dead=False, kind="melee"))
            await barks.task
        elif kind == "fight":
            dialogue.in_fight = lambda npc: True
            await dialogue.event_turn(LOBART, "[Бой!]")
        else:
            await dialogue.acknowledge(LOBART, "Лобарт")
        assert quick.slot_ids and set(quick.slot_ids) == {quick.prewarm_slot_ids[0]}


@pytest.mark.parametrize("distance", [600, 1200])
async def test_a_new_focused_npc_is_warmed_only_within_eight_metres(distance, monkeypatch):
    monkeypatch.setattr(dialogue_module, "PREWARM_DELAY_S", 0)
    fake = FakeGame()
    quick = FakeBackend(delay=0)
    warm = quick.prewarm
    attempts = 0

    async def prewarm(*, instructions, messages):
        nonlocal attempts
        attempts += 1
        if attempts == 1:
            raise LLMError("temporary prewarm failure")
        await warm(instructions=instructions, messages=messages)

    monkeypatch.setattr(quick, "prewarm", prewarm)
    view = {
        "npc": HILDA,
        "name": "Хильда",
        "hero": {"id": "PLAYER", "name": "Я", "distance": distance, "focus": HILDA},
    }
    async with connected(fake) as link:
        dialogue = DialogueManager(link, FakeBackend(), PersonaProvider(None), quick=quick)
        await fake.push_views(view)
        if distance > 800:
            await asyncio.sleep(0.05)
            assert quick.prewarms == [] and HILDA not in dialogue.conversations
            return

        await _until(lambda: attempts == 1 and not dialogue._prewarms)
        assert quick.prewarms == []
        await fake.push_views(view)  # retry the unchanged history after the service recovers
        await _until(lambda: len(quick.prewarms) == 1)
        assert attempts == 2
        conv = dialogue.conversations[HILDA]
        assert quick.prewarms == [(conv.persona.instructions, [])]
        assert conv.history == [] and conv.reply is None and conv.talked_at == float("-inf")
        assert dialogue.memory.load("test-branch", HILDA).entries == []
        assert dialogue.llm.calls == [] and fake.said == [] and fake.statuses == []

        await fake.push_views(view)
        await asyncio.sleep(0.05)
        assert len(quick.prewarms) == 1  # unchanged views do not reread the same history

        dialogue.note(HILDA, "Ты заметила героя")
        dialogue.note(HILDA, "Герой остановился рядом")
        await _until(lambda: len(quick.prewarms) == 2)
        assert quick.prewarms[-1][1] == conv.history  # the newer history replaces the pending warm


async def test_the_talk_key_warms_a_new_focused_npc(monkeypatch):
    monkeypatch.setattr(dialogue_module, "PREWARM_DELAY_S", 0)
    fake = FakeGame()
    quick = FakeBackend(delay=0)
    async with connected(fake) as link:
        dialogue = DialogueManager(link, FakeBackend(), PersonaProvider(None), quick=quick)
        await fake.roster({"id": HILDA, "name": "Хильда", "distance": 1200})
        await fake.send(type="ptt_start", focus=HILDA)
        await _until(lambda: len(quick.prewarms) == 1)
        conv = dialogue.conversations[HILDA]
        assert quick.prewarms == [(conv.persona.instructions, [])]
        assert conv.history == [] and conv.reply is None and fake.said == []


async def test_three_nearest_listeners_are_warmed_with_the_focus_last(monkeypatch):
    monkeypatch.setattr(dialogue_module, "PREWARM_DELAY_S", 0)
    fake = FakeGame()
    quick = FakeBackend(delay=0)
    people = [
        {"id": HILDA, "name": "Хильда", "distance": 700},
        {"id": "PC_THIEF", "name": "Диего", "distance": 500},
        {"id": LOBART, "name": "Лобарт", "distance": 200},
        {"id": XARDAS, "name": "Ксардас", "distance": 100},
        {"id": "BAU_4300_ADDON_CAVALORN", "name": "Кавалорн", "distance": 300},
    ]
    async with connected(fake) as link:
        dialogue = DialogueManager(link, FakeBackend(), PersonaProvider(None), quick=quick)
        for person in people:
            dialogue.open(person["id"], person["name"], warm=False).talked_at = time.monotonic()
        dialogue._on_message(NpcRoster(type="npc_roster", t=1000, full=True, npcs=people))
        dialogue.prewarm_listeners(HILDA)
        await _until(lambda: len(quick.prewarms) >= 3)
        assert [instructions for instructions, _ in quick.prewarms] == [
            dialogue.conversations[npc].persona.instructions for npc in (XARDAS, LOBART, HILDA)
        ]

        quick.prewarms.clear()
        wolf = fake.snapshot("WOLF")
        wolf["npc"]["name"], wolf["player"]["distance"] = "Волк", 600
        await fake.send(**wolf)
        await fake.push_views(
            {
                "npc": HILDA,
                "name": "Хильда",
                "hero": {"id": "PLAYER", "name": "Я", "distance": 700, "focus": "WOLF"},
            }
        )
        await fake.send(type="ptt_start", focus="WOLF")
        await _until(lambda: len(quick.prewarms) >= 3)
        assert "WOLF" not in dialogue.conversations
        assert [instructions for instructions, _ in quick.prewarms] == [
            dialogue.conversations[npc].persona.instructions
            for npc in (XARDAS, LOBART, "BAU_4300_ADDON_CAVALORN")
        ]


async def test_a_running_npcs_turn_does_not_start_a_prewarm(monkeypatch):
    monkeypatch.setattr(dialogue_module, "PREWARM_DELAY_S", 0)
    fake = FakeGame()
    quick = FakeBackend("Хм...", delay=0)
    llm = StallingBackend("Слушаю.")
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None), quick=quick)
        dialogue.open(HILDA, "Хильда", warm=False)
        turn = dialogue.say_as_player(HILDA, "Что скажешь?")
        try:
            await _until(lambda: llm.calls == 1)
            dialogue.prewarm_listeners(HILDA)
            await asyncio.sleep(0.05)
            assert not turn.done() and quick.prewarms == []
        finally:
            turn.cancel()
            await asyncio.gather(turn, return_exceptions=True)


async def test_a_line_that_never_starts_is_not_spoken_forever(monkeypatch):
    """The game does not start a line of an npc far from the hero (or one the bridge refused): after a while
    the npc is not counted as speaking any more, or whatever waits for it (a scene) would wait forever."""
    monkeypatch.setattr(dialogue_module, "LINE_LOST_S", 0.3)
    fake = FakeGame()
    fake.drop_says.add(XARDAS)
    async with connected(fake) as link:
        dialogue = DialogueManager(link, FakeBackend("Ступай.", delay=0), PersonaProvider(None))
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Привет.")
        await _until(lambda: dialogue.conversations[XARDAS].reply.done())
        assert dialogue.speaking(XARDAS)  # its line may still come
        await asyncio.sleep(0.4)
        assert not dialogue.speaking(XARDAS)


async def test_an_npc_that_means_to_fight_the_hero_attacks_him_through_the_scripts():
    fake = FakeGame()
    fight = {"goal": "fight", "target": "PLAYER", "attitude": "hostile", "agenda": None}
    llm = FakeBackend("Кошелёк или жизнь!", delay=0, tool_calls=[ToolCall("set_intent", fight)])
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        dialogue.guild_of = lambda npc: 9  # a bandit
        await link.attach(XARDAS)
        dialogue.open(XARDAS, "Бандит")
        await dialogue.say_as_player(XARDAS, "Отойди с дороги")
        await _until(lambda: any(m.get("type") == "story_effect" for m in fake.story))
        await _until(lambda: dialogue.conversations[XARDAS].history[-1].content == "[Ты напал на героя.]")

    (attack,) = [m for m in fake.story if m["type"] == "story_effect"]
    assert attack["npc"] == XARDAS and attack["effects"] == [
        {"do": "attack", "reason": "AR_SUDDENENEMYINFERNO"}
    ]
    assert not any(
        m["type"] == "action" and m["action"] == "fight" for m in fake.received
    )  # the scripts fight


# --- the quick model of a fight ----------------------------------------------------------------------


def _situation_of(messages) -> str:
    return next(m.content for m in reversed(messages) if m.role == "developer" and "Обстановка" in m.content)


async def test_a_line_in_a_fight_is_the_quick_models_and_the_turns_model_only_acts():
    """The bandit's "Держи его!" comes from the quick model at once; the conversational model, told what was
    said, decides the attack, and its own words are not said."""
    fake = FakeGame()
    fight = {"goal": "fight", "target": "PLAYER", "attitude": "hostile", "agenda": None}
    quick = FakeBackend("[hostile] Это он! Держи его!", delay=0)
    llm = FakeBackend("Слова, которых никто не услышит.", delay=0, tool_calls=[ToolCall("set_intent", fight)])
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None), quick=quick)
        dialogue.in_fight = lambda npc: True
        dialogue.guild_of = lambda npc: 9  # a bandit
        await link.attach(XARDAS)
        dialogue.open(XARDAS, "Бандит")
        await dialogue.event_turn(XARDAS, "[Событие: герой сказал рядом: «Давай поговорим»]")
        await _until(lambda: any(m.get("type") == "story_effect" for m in fake.story))

    assert " ".join(fake.said) == "Это он! Держи его!"
    assert quick.temperatures == [None]
    (_, quick_messages, quick_tools), (_, messages, tools) = quick.calls[0], llm.calls[0]
    assert quick_tools == [] and "set_intent" in {t.name for t in tools}
    brief, full = _situation_of(quick_messages), _situation_of(messages)
    assert dialogue_module.QUICK_HINT in brief and "Цели для намерения" not in brief
    assert "Цели для намерения" in full and "Ты уже сказал вслух: «Это он! Держи его!»" in full
    assert "Твою реплику уже произносят вслух." not in full
    history = dialogue.conversations[XARDAS].history
    assert history[-1].content.startswith("[Ты напал") or history[-2].content == "Это он! Держи его!"
    assert any(m.role == "assistant" and m.content == "Это он! Держи его!" for m in history)
    assert dialogue.conversations[XARDAS].intent.goal == "fight"


async def test_the_turns_model_speaks_when_the_quick_model_fails():
    fake = FakeGame()
    llm = FakeBackend("Сам справлюсь!", delay=0)
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None), quick=FailingBackend())
        dialogue.in_fight = lambda npc: True
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.event_turn(XARDAS, "[Событие: Волк нападает на тебя]")
        await _until(lambda: fake.said == ["Сам справлюсь!"])

    _, messages, _ = llm.calls[0]
    assert dialogue_module.QUICK_HINT not in _situation_of(messages)  # the whole situation, as usual


async def test_out_of_a_fight_the_quick_model_only_says_the_first_words():
    """The hero's phrase: the quick model's first words, then the conversational model's answer. An event
    outside a fight is the conversational model's alone."""
    fake = FakeGame()
    quick = FakeBackend("[angry] Тьфу! И это всё?", delay=0)
    llm = FakeBackend("Не спеши.", delay=0)
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None), quick=quick)
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Ты старый дурак")
        await dialogue.event_turn(XARDAS, "[Событие: герой подошёл к тебе]")
        await _until(lambda: fake.said == ["Тьфу!", "Не спеши.", "Не спеши."])

    assert len(quick.calls) == 1  # the phrase's first words; the event: no
    assert quick.temperatures == [0.6]
    _, words_request, words_tools = quick.calls[0]
    assert words_tools == [] and dialogue_module.REACT_HINT in _situation_of(words_request)
    said = [m for m, _ in fake.says if m["text"] == "Тьфу!"]
    assert said[0]["filler"] is True and said[0]["mood"] == "angry" and said[0].get("reply") is None
    _, messages, _ = llm.calls[0]
    situation = _situation_of(messages)
    assert "Ты уже сказал вслух: «Тьфу!»" in situation and dialogue_module.REACT_HINT not in situation
    assert all("Твою реплику уже произносят вслух." not in _situation_of(call[1]) for call in llm.calls)
    assert [m.content for m in dialogue.conversations[XARDAS].history][:2] == ["Ты старый дурак", "Не спеши."]


@pytest.mark.parametrize("combat", [True, False])
async def test_sol_overlaps_first_words_only_for_a_hero_combat_phrase(combat, monkeypatch):
    fake = FakeGame()
    quick = FakeBackend("[angry] Стой!", delay=0)
    llm = FakeBackend("Сам справлюсь!", delay=0)
    probe_started, probe_done = asyncio.Event(), asyncio.Event()
    words_started, words_done = asyncio.Event(), asyncio.Event()
    full_started = asyncio.Event()
    stream = quick.stream

    async def held_words(**kwargs):
        words_started.set()
        await words_done.wait()
        async for event in stream(**kwargs):
            yield event

    async def probe(conv, request):
        probe_started.set()
        await probe_done.wait()

    monkeypatch.setattr(quick, "stream", held_words)
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None), quick=quick)
        dialogue.fights_hero = lambda npc: combat
        dialogue.surrender_hold = probe
        situation = dialogue._situation

        async def prepare_situation(*args, **kwargs):
            if not kwargs.get("brief"):
                full_started.set()
            return await situation(*args, **kwargs)

        monkeypatch.setattr(dialogue, "_situation", prepare_situation)
        dialogue.open(XARDAS, "Ксардас", warm=False)
        turn = dialogue.say_as_player(XARDAS, "Хватит!")
        try:
            if combat:
                await asyncio.wait_for(probe_started.wait(), 2)
                assert not words_started.is_set() and not llm.calls
                assert full_started.is_set()  # native context preparation overlaps the probe too
                probe_done.set()
            await asyncio.wait_for(words_started.wait(), 2)
            if combat:
                await _until(lambda: bool(llm.calls))
                assert not turn.done() and not fake.said
            else:
                await asyncio.sleep(0.02)
                assert not llm.calls
            words_done.set()
            await turn
            await _until(lambda: len(fake.said) == (1 if combat else 2))
        finally:
            words_done.set()
            turn.cancel()
            await asyncio.gather(turn, return_exceptions=True)

    situation = _situation_of(llm.calls[0][1])
    assert ("Ты уже сказал вслух: «Стой!»" in situation) is not combat
    instruction = (
        "Твою реплику уже произносят вслух. Не пиши текст: если нужно действие, сразу вызови инструмент; "
        "если делать ничего не нужно, ответь пустым сообщением."
    )
    assert (instruction in situation) is combat
    assert not combat or situation.endswith(instruction)
    assert fake.said == (["Стой!"] if combat else ["Стой!", "Сам справлюсь!"])


@pytest.mark.parametrize("failed", [False, True])
@pytest.mark.parametrize("silent", [False, True])
async def test_empty_parallel_first_words_voice_sol_once(failed, silent, tmp_path):
    from gothic_ai.trace import find_turn

    fake = FakeGame()
    reply = "[angry] Сам справлюсь!"
    llm = FakeBackend(script=[FakeStep("" if silent else reply), FakeStep(reply)], delay=0)
    writer = TraceWriter(tmp_path)
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None),
                                   quick=FailingBackend() if failed else FakeBackend("", delay=0),
                                   trace=writer)
        dialogue.fights_hero = lambda npc: True
        dialogue.open(XARDAS, "Ксардас", warm=False)
        await dialogue.say_as_player(XARDAS, "Хватит!")
        await _until(lambda: fake.said == ["Сам справлюсь!"])
    assert len(llm.calls) == (2 if silent else 1)
    assert "Твою реплику уже произносят вслух." in _situation_of(llm.calls[0][1])
    if silent:
        assert llm.tool_choices[-1] == "none"
        assert "Твою реплику уже произносят вслух." not in _situation_of(llm.calls[-1][1])
    assert dialogue.conversations[XARDAS].history[-1].content == "Сам справлюсь!"
    turn, _ = find_turn(writer.path(), None)
    assert turn["status"] == "completed"
    assert any(event["kind"] == "quick_fallback" for event in turn["events"])
    assert any(event["kind"] == "quick_failed" for event in turn["events"]) is failed


@pytest.mark.parametrize("reply", ["Держи свиток.", "Ишь ты! Держи свиток."])
async def test_a_rejection_remembers_the_first_words_and_the_main_reply(reply):
    fake = FakeGame()
    quick = FakeBackend("[angry] Ишь ты!", delay=0)
    llm = FakeBackend(
        script=[
            FakeStep(
                reply,
                [ToolCall("give_item", {"item": "ITSC_FIREBALL", "count": 1})],
            ),
            FakeStep(""),
        ],
        delay=0,
    )
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None), quick=quick)
        await link.attach(XARDAS)
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Дай мне свиток.")
        await _until(lambda: fake.statuses[-1:] == ["idle"])

    assert fake.said == ["Ишь ты!", "Держи свиток."]
    assert len(llm.calls) == 2
    result = next(i for i in llm.calls[1][1] if isinstance(i, ToolResult))
    assert result.output.startswith("Отклонено: У тебя нет предмета ITSC_FIREBALL")
    assert result.output.endswith(
        " Ты уже сказал вслух: «Ишь ты! Держи свиток.». "
        "Если отказ этого не меняет — ничего не говори. "
        "Если меняет — поправься одной короткой фразой, не повторяя сказанного."
    )
    assert result.output.count("Ишь ты!") == 1


async def test_first_words_do_not_end_a_turn_after_a_silent_accepted_call():
    fake = FakeGame()
    llm = FakeBackend(
        script=[
            FakeStep("", [ToolCall("give_item", {"item": "ITPO_HEALTH_01", "count": 1})]),
            FakeStep("Держи, это поможет."),
        ],
        delay=0,
    )
    async with connected(fake) as link:
        dialogue = DialogueManager(
            link, llm, PersonaProvider(None), quick=FakeBackend("Хм...", delay=0)
        )
        await link.attach(XARDAS)
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Дай мне зелье.")
        await _until(lambda: fake.statuses[-1:] == ["idle"])
        await _until(
            lambda: any(m["type"] == "action" and m["action"] == "give_item" for m in fake.received)
        )

    assert len(llm.calls) == 2
    assert fake.said == ["Хм...", "Держи, это поможет."]
    assert any(m["type"] == "action" and m["action"] == "give_item" for m in fake.received)


async def test_no_first_words_to_a_greeting_or_without_the_quick_model_in_time(monkeypatch):
    monkeypatch.setattr(dialogue_module, "REACTION_S", 0.2)
    fake = FakeGame()
    nothing = FakeBackend("[friendly] —", delay=0)
    late = FakeBackend("Хм...", delay=0, first=1.0)
    async with connected(fake) as link:
        greeted = DialogueManager(
            link, FakeBackend("Здравствуй.", delay=0), PersonaProvider(None), quick=nothing
        )
        greeted.open(XARDAS, "Ксардас")
        await greeted.say_as_player(XARDAS, "Привет")
        slow = DialogueManager(link, FakeBackend("Слушаю.", delay=0), PersonaProvider(None), quick=late)
        slow.open(LOBART, "Лобарт")
        await slow.say_as_player(LOBART, "Где мне найти работу?")
        await _until(lambda: fake.said == ["Здравствуй.", "Слушаю."])

    for dialogue in (greeted, slow):
        _, messages, _ = dialogue.llm.calls[0]
        assert "Ты уже сказал вслух" not in _situation_of(messages)


async def test_the_quick_model_is_kept_warm_with_the_history_of_an_npc_in_a_fight(monkeypatch):
    """Whenever the history of an npc in a fight grows, and when the hero starts to speak, the quick model
    reads the npc's instructions and history: its next line only reads the situation and the trigger."""
    monkeypatch.setattr(dialogue_module, "PREWARM_DELAY_S", 0)
    fake = FakeGame()
    quick = FakeBackend("Держись!", delay=0)
    fighting = {LOBART}
    async with connected(fake) as link:
        dialogue = DialogueManager(link, FakeBackend("", delay=0), PersonaProvider(None), quick=quick)
        dialogue.in_fight = lambda npc: npc in fighting
        dialogue.open(XARDAS, "Ксардас", warm=False)  # nobody talks to them yet
        dialogue.open(LOBART, "Лобарт", warm=False)
        dialogue.note(XARDAS, "Ты видел волка")  # not in a fight: nothing to warm
        dialogue.note(LOBART, "Волк нападает на тебя")
        await _until(lambda: len(quick.prewarms) == 1)
        instructions, history = quick.prewarms[0]
        assert "Лобарт" in instructions and history == dialogue.conversations[LOBART].history

        await dialogue.event_turn(LOBART, "[Событие: Волк нападает на тебя]")
        # not during its own turn (its words take priority over warming), once after it
        await _until(lambda: len(quick.prewarms) == 2)
        assert quick.prewarms[-1][1] == dialogue.conversations[LOBART].history
        assert quick.prewarms[-1][1][-1] == Message("assistant", "Держись!")

        await fake.send(type="ptt_start", focus=LOBART)  # the hero speaks: its slot is refreshed
        await _until(lambda: len(quick.prewarms) == 3)


async def test_an_npc_acting_on_its_quick_line_is_done_speaking():
    """While the conversational model decides what to do about the line, the npc is not speaking: the hero's
    next phrase is a new turn, not an interruption, and the line said is remembered as it was."""
    fake = FakeGame()
    quick = FakeBackend("Держись!", delay=0)
    llm = FakeBackend("Неслышные слова.", delay=0, first=5.0)  # still deciding when the hero speaks again
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None), quick=quick)
        dialogue.in_fight = lambda npc: True
        dialogue.open(XARDAS, "Ксардас")
        first = dialogue.event_turn(XARDAS, "[Событие: Волк нападает на тебя]")
        conv = dialogue.conversations[XARDAS]
        await _until(lambda: conv.acting and fake.statuses[-1:] == ["idle"])
        _, sounding, unsaid = dialogue._said(conv)
        assert dialogue.speaking(XARDAS) == (sounding or unsaid)  # the running turn does not count
        dialogue.say_as_player(XARDAS, "Бей его!")
        await _until(lambda: first.done())

    assert first.cancelled()
    history = [m.content for m in dialogue.conversations[XARDAS].history]
    assert history[:2] == ["[Событие: Волк нападает на тебя]", "Держись!"]
    assert not any("перебил" in h for h in history)


async def test_a_quick_line_ends_at_its_first_blank_line():
    """Gemma, seeing tool calls in the history, may write one out after its line: it is never said."""
    fake = FakeGame()
    quick = FakeBackend("[hostile] Это он! Держи его!\n\nset_intent(goal=fight, target=PLAYER)", delay=0)
    async with connected(fake) as link:
        dialogue = DialogueManager(link, FakeBackend("", delay=0), PersonaProvider(None), quick=quick)
        dialogue.in_fight = lambda npc: True
        dialogue.open(XARDAS, "Бандит")
        await dialogue.event_turn(XARDAS, "[Событие: герой сказал рядом: «Давай поговорим»]")
        await _until(lambda: fake.statuses[-1:] == ["idle"] and fake.said)

    assert " ".join(fake.said) == "Это он! Держи его!"
    assert dialogue.conversations[XARDAS].history[-1] == Message("assistant", "Это он! Держи его!")


async def test_the_first_words_go_out_before_the_gesture():
    """The game's queue holds a line until the gesture before it is over: the words go first."""
    fake = FakeGame()
    seen: list[bool] = []
    async with connected(fake) as link:
        dialogue = DialogueManager(
            link, FakeBackend("Слушаю.", delay=0), PersonaProvider(None), quick=FakeBackend("Хм...", delay=0)
        )

        async def first_reaction(npc: str, text: str) -> FirstReaction:
            return FirstReaction(lambda: seen.append(any(t == "Хм..." for t, _ in dialogue._lines.values())))

        dialogue.first_reaction = first_reaction
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Что ты знаешь о драконах?")
        await _until(lambda: fake.said == ["Хм...", "Слушаю."])

    assert seen == [True]


async def test_a_companion_answers_an_order_through_the_quick_model():
    fake = FakeGame()
    quick = FakeBackend("[neutral] Стою.", delay=0)
    async with connected(fake) as link:
        dialogue = DialogueManager(link, FakeBackend(), PersonaProvider(None), quick=quick)
        dialogue.remember(LOBART, "Лобарт", "Герой приказал тебе: «Стой!» — остановиться. Ты подчинился.")
        await dialogue.acknowledge(LOBART, "Лобарт")
        silent = DialogueManager(link, FakeBackend(), PersonaProvider(None))
        await silent.acknowledge(LOBART, "Лобарт")  # no quick model: no answer
        await _until(lambda: fake.said == ["Стою."])

    (_, messages, tools) = quick.calls[0]
    assert tools == [] and "Герой приказал тебе: «Стой!»" in messages[-2].content
    assert messages[-1].role == "developer" and dialogue_module.ACK_HINT in messages[-1].content
    (said,) = [m for m, _ in fake.says if m["text"] == "Стою."]
    assert said["overlay"] is True and not said.get("filler")  # on the move: the order is carried out
    assert quick.temperatures == [None]


async def test_first_words_accept_eight_words_but_not_nine_and_trace_rejections(tmp_path):
    from gothic_ai.trace import TraceWriter, read

    eight = "Твоё невежество явно граничит с самым настоящим безумием."
    nine = "Твоё невежество явно граничит с самым настоящим полным безумием."
    replies = [f"[angry] {eight}", nine, eight, "[neutral] —"]
    quick = FakeBackend(script=[FakeStep(text) for text in replies], delay=0)
    writer = TraceWriter(tmp_path)
    fake = FakeGame()
    async with connected(fake) as link:
        dialogue = DialogueManager(
            link, FakeBackend("Слушаю.", delay=0), PersonaProvider(None), quick=quick, trace=writer
        )
        dialogue.open(XARDAS, "Ксардас")
        for phrase in ("Невежда!", "Сам такой!", "Да ты сам!", "До встречи."):
            await dialogue.say_as_player(XARDAS, phrase)
        await _until(lambda: fake.said.count("Слушаю.") == 4)

    assert [m["text"] for m, _ in fake.says if m.get("filler")] == [eight]
    turns = [r for r in read(writer.path()) if r["type"] == "turn"]
    events = [e for t in turns for e in t["events"] if e["kind"] == "first_words"]
    assert [e["outcome"] for e in events] == ["said", "too_long", "same", "nothing"]
    assert [e["text"] for e in events] == replies
    assert [e["words"] for e in events] == [eight, nine, eight, ""]
    for event, (_, messages, _) in zip(events, quick.calls, strict=True):
        assert event["hint"] == dialogue_module.REACT_HINT
        assert event["situation"] + "\n" + event["hint"] == _situation_of(messages)
        assert 0 <= event["elapsed_ms"] < dialogue_module.REACTION_S * 1000


async def test_a_pure_sound_waits_four_player_turns_but_words_with_a_sound_do_not(tmp_path):
    from gothic_ai.trace import TraceWriter, read

    replies = ["Хммм...", "Ого!", "Хм, двести золотых?", "—", "Ааа!", "Гм!"]
    quick = FakeBackend(script=[FakeStep(text) for text in replies], delay=0)
    fake = FakeGame()
    writer = TraceWriter(tmp_path)
    async with connected(fake) as link:
        dialogue = DialogueManager(
            link, FakeBackend("Слушаю.", delay=0), PersonaProvider(None), quick=quick, trace=writer
        )
        dialogue.open(XARDAS, "Ксардас")
        for _ in replies:
            await dialogue.say_as_player(XARDAS, "Сколько стоит свиток?")
        await _until(lambda: fake.said.count("Слушаю.") == len(replies))
        quick.script = [FakeStep("Ого!")]
        dialogue.open(LOBART, "Лобарт")
        await dialogue.say_as_player(LOBART, "Двести золотых!")
        await _until(lambda: fake.said.count("Слушаю.") == len(replies) + 1)

    assert [m["text"] for m, _ in fake.says if m.get("filler")] == [
        "Хммм...", "Хм, двести золотых?", "Гм!", "Ого!"
    ]
    turns = [r for r in read(writer.path()) if r["type"] == "turn"]
    assert [e["outcome"] for t in turns for e in t["events"] if e["kind"] == "first_words"] == [
        "said", "sound", "said", "nothing", "sound", "said", "said"
    ]


@pytest.mark.parametrize("failure", ["late", "error"])
async def test_failed_first_words_attempts_are_traced(tmp_path, monkeypatch, failure):
    from gothic_ai.trace import TraceWriter, find_turn

    monkeypatch.setattr(dialogue_module, "REACTION_S", 0.02)
    quick = FakeBackend("Хм...", first=1) if failure == "late" else FailingBackend()
    fake = FakeGame()
    writer = TraceWriter(tmp_path)
    async with connected(fake) as link:
        dialogue = DialogueManager(
            link, FakeBackend("Слушаю.", delay=0), PersonaProvider(None), quick=quick, trace=writer
        )
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Как мне быть?")
        await _until(lambda: fake.said == ["Слушаю."])
    turn, _ = find_turn(writer.path(), None)
    (event,) = [e for e in turn["events"] if e["kind"] == "first_words"]
    assert event["outcome"] == failure and event["text"] == ""
    assert event["words"] == ""
    assert event["elapsed_ms"] >= 0 and event["hint"] == dialogue_module.REACT_HINT
    assert fake.said == ["Слушаю."]


@pytest.mark.parametrize("waiting_for_delivery", [False, True])
async def test_interrupted_first_words_are_traced_as_late_until_sent(
    tmp_path, monkeypatch, waiting_for_delivery
):
    from gothic_ai.trace import TraceWriter, find_turn

    fake = FakeGame()
    quick = FakeBackend("Ого!", delay=0, first=0 if waiting_for_delivery else 1)
    writer = TraceWriter(tmp_path)
    waiting = asyncio.Event()

    async def wait_to_say(*args, **kwargs):
        waiting.set()
        await asyncio.Event().wait()

    async with connected(fake) as link:
        dialogue = DialogueManager(link, FakeBackend(), PersonaProvider(None), quick=quick, trace=writer)
        if waiting_for_delivery:
            monkeypatch.setattr(dialogue, "_say", wait_to_say)
        dialogue.open(XARDAS, "Ксардас")
        task = dialogue.say_as_player(XARDAS, "Я убью дракона!")
        if waiting_for_delivery:
            await asyncio.wait_for(waiting.wait(), 1)
        else:
            await _until(lambda: bool(quick.calls))
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task

    turn, _ = find_turn(writer.path(), None)
    (event,) = [e for e in turn["events"] if e["kind"] == "first_words"]
    assert event["outcome"] == "late"
    assert event["text"] == ("Ого!" if waiting_for_delivery else "")
    assert event["words"] == event["text"]
    assert fake.said == []


async def test_first_words_are_short_and_not_the_same_twice_in_a_row():
    """Gemma now and then answers at once in a whole sentence, or says "Хм..." to everything: such first
    words are not said (the answer would follow a second one; the same sound grows tiresome)."""
    fake = FakeGame()
    quick = FakeBackend(
        script=[
            FakeStep("Хм..."),
            FakeStep("Хм..."),
            FakeStep("Ты ещё слишком слаб, чтобы полагаться на чужую силу."),
            FakeStep("[angry] Ишь ты!"),
        ],
        delay=0,
    )
    async with connected(fake) as link:
        dialogue = DialogueManager(link, FakeBackend("Слушаю.", delay=0), PersonaProvider(None), quick=quick)
        dialogue.open(XARDAS, "Ксардас")
        for phrase in (
            "Что это за башня?",
            "А кто тут живёт?",
            "Помоги мне.",
            "Я убью дракона голыми руками!",
        ):
            await dialogue.say_as_player(XARDAS, phrase)
        await _until(lambda: fake.said.count("Слушаю.") == 4)

    assert [m["text"] for m, _ in fake.says if m.get("filler")] == ["Хм...", "Ишь ты!"]


async def test_one_fighting_the_hero_may_call_it_off_and_keeps_to_it():
    fake = FakeGame()
    calm = {"goal": "none", "target": None, "attitude": "wary", "agenda": None}
    llm = FakeBackend("Ладно, ладно. Живи пока.", delay=0, tool_calls=[ToolCall("set_intent", calm)])
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        dialogue.fights_hero = lambda npc: True  # the scripts' attack on the hero
        calmed: list[str] = []
        dialogue.on_calm.append(calmed.append)
        dialogue.open(XARDAS, "Бандит")
        await dialogue.say_as_player(XARDAS, "Воу, воу, ты чего? Спокуха!")
        await _until(
            lambda: dialogue.conversations[XARDAS].history[-1].content == "[Ты прекратил драку с героем.]"
        )

    (effect,) = [m for m in fake.story if m["type"] == "story_effect"]
    assert effect["npc"] == XARDAS and effect["effects"] == [{"do": "calm", "attitude": "ATT_NEUTRAL"}]
    assert calmed == [XARDAS]
    situation = next(m.content for m in llm.calls[0][1] if m.role == "developer")
    assert "Ты дерёшься с героем. Прекратить драку" in situation


async def test_words_with_a_hostile_heart_do_not_end_the_fight():
    fake = FakeGame()
    still = {"goal": "none", "target": None, "attitude": "hostile", "agenda": None}
    llm = FakeBackend(
        "Спокуха? Сейчас я тебе покажу спокуху!", delay=0, tool_calls=[ToolCall("set_intent", still)]
    )
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        dialogue.fights_hero = lambda npc: True
        dialogue.open(XARDAS, "Бандит")
        await dialogue.say_as_player(XARDAS, "Спокуха!")
        await _until(lambda: dialogue.conversations[XARDAS].intent.attitude == "hostile")
        await asyncio.sleep(0.1)
    assert not [m for m in fake.story if m["type"] == "story_effect"]


async def test_a_rejection_after_the_quick_fight_line_keeps_the_next_step_silent():
    fake = FakeGame()
    quick = FakeBackend("[hostile] Держись!", delay=0)
    llm = FakeBackend(
        script=[
            FakeStep(
                "Этого никто не услышит.",
                [ToolCall("give_item", {"item": "ITSC_FIREBALL", "count": 1})],
            ),
            FakeStep("И эту поправку никто не услышит."),
        ],
        delay=0,
    )
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None), quick=quick)
        dialogue.in_fight = lambda npc: True
        await link.attach(XARDAS)
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.event_turn(XARDAS, "[Событие: Волк нападает на тебя]")
        await _until(lambda: fake.said == ["Держись!"])

    assert len(llm.calls) == 2
    result = next(i for i in llm.calls[1][1] if isinstance(i, ToolResult))
    assert result.output.startswith("Отклонено: У тебя нет предмета ITSC_FIREBALL")
    assert "Ты уже сказал вслух" not in result.output
    assert "ничего не говори" not in result.output
    history = dialogue.conversations[XARDAS].history
    assert history[-1].content == "Держись!"


async def test_surrender_probe_uses_a_separate_slot_and_leaves_the_npc_cache_intact():
    quick = FakeBackend(script=[FakeStep("да"), FakeStep("Ладно!")], delay=0)
    dialogue = DialogueManager(GameLink(SimpleNamespace(send=AsyncMock())), FakeBackend(),
                               PersonaProvider(None), quick=quick)
    conv = dialogue.open(LOBART, "Лобарт", warm=False)
    conv.talked_at = time.monotonic()
    slot = dialogue._quick_slot(LOBART)
    history = [Message("assistant", "Я тебя проучу!")]
    dialogue._prewarm_history[LOBART] = history
    request = [Message("assistant", "Я тебя проучу!"),
               Message("developer", dialogue._brief(conv) + "\n" + dialogue_module.QUICK_HINT),
               Message("user", "Сдаюсь, забирай золото, только не бей")]
    assert await dialogue._surrendering(conv, request)
    assert dialogue._quick_slots[LOBART] == slot
    assert dialogue._prewarm_history[LOBART] is history
    words, _ = await dialogue._quick_words(conv, request)
    assert words == "Ладно!"
    assert quick.slot_ids[0] != slot and quick.slot_ids[1] == slot
    assert quick.calls[0][0] != conv.persona.instructions
    assert quick.calls[1][0] == conv.persona.instructions
    assert request[-1].content in " ".join(m.content for m in quick.calls[0][1])
    assert quick.calls[1][1] == request
    assert quick.temperatures == [0, None]
    assert quick.max_token_limits == [4, None]
    assert quick.grammars == ['root ::= "да" | "нет"', None]
    assert not conv.history


async def test_fight_first_words_wait_for_the_surrender_probe():
    quick = FakeBackend("Ладно!", delay=0)
    started, release = asyncio.Event(), asyncio.Event()

    async def probe(conv, request):
        started.set()
        await release.wait()

    async with connected(FakeGame()) as link:
        dialogue = DialogueManager(link, FakeBackend("", delay=0), PersonaProvider(None), quick=quick)
        dialogue.in_fight = lambda npc: True
        dialogue.surrender_hold = probe
        conv = dialogue.open(LOBART, "Лобарт", warm=False)
        turn = dialogue.say_as_player(LOBART, "Сдаюсь, только не бей", overlay=True)
        try:
            await asyncio.wait_for(started.wait(), 1)
            assert not quick.calls
        finally:
            release.set()
            await turn
        assert quick.calls and conv.history[-1].content == "Ладно!"


async def test_surrender_probe_blocks_new_prewarming_svm_and_combat_barks(monkeypatch):
    from gothic_ai.combat_barks import CombatBarks
    from gothic_ai.protocol import CombatHit

    quick = FakeBackend("да", delay=0)
    rows = []
    dialogue = DialogueManager(GameLink(SimpleNamespace(send=AsyncMock())), FakeBackend(),
                               PersonaProvider(None), quick=quick,
                               trace=SimpleNamespace(write=rows.append))
    barks = CombatBarks(dialogue, rng=lambda: 0)
    conv = dialogue.open(LOBART, "Лобарт", warm=False)
    other = dialogue.open(HILDA, "Хильда", warm=False)
    dialogue.link.snapshots[HILDA] = Snapshot.model_validate(FakeGame().snapshot(HILDA))
    dialogue.link._dispatch(NpcRoster(type="npc_roster", t=0, npcs=[dict(
        id=HILDA, name="Хильда", guild=8, distance=100, activity="fight",
    )]))
    dialogue._quick_slot(LOBART)
    started, release = asyncio.Event(), asyncio.Event()
    stream = quick.stream

    async def held_probe(**options):
        started.set()
        await release.wait()
        async for event in stream(**options):
            yield event

    monkeypatch.setattr(quick, "stream", held_probe)
    conv.talked_at = time.monotonic()
    task = asyncio.create_task(dialogue._surrendering(conv, [Message("user", "Пощади!")]))
    try:
        await asyncio.wait_for(started.wait(), 1)
        dialogue._start_prewarm(other, [HILDA])
        dialogue._start_svm(SvmLine(type="svm_line", id="held", npc=HILDA,
                                  svm="$THREATEN", text="Берегись!", overlay=True))
        dialogue.link._dispatch(CombatHit(type="combat_hit", npc=HILDA, by="PLAYER", damage=10,
                                         hp=90, hp_max=100, down=False, dead=False, kind="melee"))
        assert not dialogue._prewarms and not dialogue._svm_tasks and barks.task is None
        assert not quick.prewarms and not quick.calls
        assert {(row["type"], row["outcome"]) for row in rows} == {
            ("svm_line", "busy"), ("combat_bark", "busy"),
        }
    finally:
        release.set()
        accepted = await task
    assert accepted and dialogue._surrender_probes == 0


async def test_surrender_probe_evicts_only_the_least_recent_idle_foreign_slot():
    quick = FakeBackend("да", delay=0)
    dialogue = DialogueManager(GameLink(SimpleNamespace(send=AsyncMock())), FakeBackend(),
                               PersonaProvider(None), quick=quick)
    for npc in (LOBART, HILDA, XARDAS):
        dialogue._quick_slot(npc)
        dialogue._prewarm_history[npc] = [Message("assistant", npc)]
    owner_slot, evicted_slot = dialogue._quick_slots[LOBART], dialogue._quick_slots[HILDA]
    conv = dialogue.open(LOBART, "Лобарт", warm=False)
    conv.talked_at = time.monotonic()
    assert await dialogue._surrendering(conv, [Message("user", "Пощади!")])
    assert quick.slot_ids == [evicted_slot]
    assert dialogue._quick_slots == {LOBART: owner_slot, XARDAS: 2}
    assert set(dialogue._prewarm_history) == {LOBART, XARDAS}
    assert set(dialogue._quick_used) == {LOBART, XARDAS}


@pytest.mark.parametrize("unavailable", ["busy", "locked"])
async def test_surrender_probe_skips_when_only_its_own_or_busy_slots_remain(unavailable):
    quick = FakeBackend("да", delay=0)
    dialogue = DialogueManager(GameLink(SimpleNamespace(send=AsyncMock())), FakeBackend(),
                               PersonaProvider(None), quick=quick)
    for npc in (LOBART, HILDA, XARDAS):
        dialogue._quick_slot(npc)
    slots = dict(dialogue._quick_slots)
    for slot in (1, 2):
        if unavailable == "busy":
            dialogue._quick_busy[slot] = 1
        else:
            await dialogue._quick_locks[slot].acquire()
    conv = dialogue.open(LOBART, "Лобарт", warm=False)
    conv.talked_at = time.monotonic()
    try:
        assert not await dialogue._surrendering(conv, [Message("user", "Пощади!")])
        assert dialogue._quick_slots == slots and not quick.calls
    finally:
        for slot in (1, 2):
            if unavailable == "locked":
                dialogue._quick_locks[slot].release()


@pytest.mark.parametrize("reply", ["нет", "Я тебя не пощажу", ""])
async def test_surrender_probe_requires_an_affirmative_model_answer(reply):
    quick = FakeBackend(reply, delay=0)
    dialogue = DialogueManager(GameLink(SimpleNamespace(send=AsyncMock())), FakeBackend(),
                               PersonaProvider(None), quick=quick)
    conv = dialogue.open(LOBART, "Лобарт", warm=False)
    conv.talked_at = time.monotonic()
    assert not await dialogue._surrendering(conv, [Message("user", "Сдаюсь! Не бей! Пощади!")])


async def test_surrender_probe_includes_the_opponents_current_combat_words():
    quick = FakeBackend("да", delay=0)
    dialogue = DialogueManager(GameLink(SimpleNamespace(send=AsyncMock())), FakeBackend(),
                               PersonaProvider(None), quick=quick)
    dialogue.surrender_context = lambda npc: "Сдаёшься?"
    conv = dialogue.open(LOBART, "Лобарт", warm=False)
    conv.talked_at = time.monotonic()
    assert await dialogue._surrendering(conv, [Message("user", "Да")])
    assert quick.calls[0][1] == [Message(
        "user", "Перед этим противник сказал: «Сдаёшься?»\nРеплика игрока: «Да»",
    )]


@pytest.mark.parametrize("failure", ["unavailable", "error", "deadline", "expired"])
async def test_surrender_probe_failure_keeps_the_current_combat(failure):
    quick = (None if failure == "unavailable" else FailingBackend() if failure == "error"
             else FakeBackend("да", first=0.1, delay=0))
    dialogue = DialogueManager(GameLink(SimpleNamespace(send=AsyncMock())), FakeBackend(),
                               PersonaProvider(None), quick=quick)
    conv = dialogue.open(LOBART, "Лобарт", warm=False)
    conv.talked_at = time.monotonic() - (2.1 if failure == "expired" else 1.99)
    started = time.monotonic()
    assert not await dialogue._surrendering(conv, [Message("user", "Пощади!")])
    assert time.monotonic() - started < 0.1
    if failure == "expired":
        assert not quick.calls


SURRENDER_PHRASES = [
    ("Сдаюсь, забирай золото, только не бей", True),
    ("Пацаны, обоссыте, но не бейте", True),
    ("Браго, сдаюсь! Обоссы, но не бей", True),
    ("Хватит, хватит, твоя взяла", True),
    ("Не бей, я заплачу", True),
    ("Пощади, я все понял", True),
    ("Ладно, сдаюсь. Больше не буду", True),
    ("Пощади меня!", True),
    ("Стой, я проиграл, оставь в живых", True),
    ("Я больше не сопротивляюсь, только отпусти", True),
    ("Возьми все, что у меня есть, и прекрати бить", True),
    ("Опускаю меч. Твоя победа", True),
    ("Не убивай меня, прошу", True),
    ("Ладно, твоя взяла. Хватит драться", True),
    ("Я не хочу умирать. Сдаюсь", True),
    ("Сдавайся!", False),
    ("Сдохни, тварь", False),
    ("Я сдаюсь? Еще чего!", False),
    ("Отдай мой меч", False),
    ("Ты кто такой?", False),
    ("Ты сдаёшься или мне тебя добить?", False),
    ("На колени, сейчас же!", False),
    ("Плати, или я тебя убью", False),
    ("Я тебя не пощажу", False),
    ("Клади оружие и отдай золото", False),
    ("Я заставлю тебя просить пощады", False),
    ("Ты пожалеешь, что связался со мной", False),
    ("Я не сдамся", False),
    ("Забирайся прочь, трус", False),
    ("Ты мне за все заплатишь", False),
]


async def test_surrender_recognition_live_gemma():
    import os
    import statistics
    import tomllib
    from contextlib import aclosing
    from pathlib import Path

    import httpx

    from gothic_ai.executor import Intent
    from gothic_ai.llm import make_quick, quick_url

    url = quick_url(os.environ.get("GOTHIC_QUICK_LLM_URL"), os.environ.get("GOTHIC_DECIDER_URL"))
    settings = Path.home() / ".config/gothic-ai/settings.toml"
    if url is None and settings.is_file():
        configured = tomllib.loads(settings.read_text(encoding="utf-8")).get("llm", {})
        url = quick_url(configured.get("quick_url"), configured.get("decider_url"))
    if url is None:
        pytest.skip("no quick model configured")
    async with httpx.AsyncClient(timeout=2) as client:
        try:
            response = await client.get(url.rstrip("/") + "/models")
            response.raise_for_status()
        except httpx.HTTPError:
            pytest.skip("the configured quick model is unavailable")
    quick = make_quick(url)
    dialogue = DialogueManager(GameLink(SimpleNamespace(send=AsyncMock())), FakeBackend(),
                               PersonaProvider(None), quick=quick)
    conv = dialogue.open("BDT_1014_BANDIT_L", "Браго", warm=False)
    conv.intent = Intent(goal="fight", target="PLAYER", attitude="hostile")
    dialogue.link.snapshots[conv.npc] = Snapshot.model_validate(FakeGame().snapshot(conv.npc))
    history = [Message("user", "Отстань от меня."), Message("assistant", "Я тебя проучу!")]
    situation = Message("developer", dialogue._brief(conv) + "\n" + dialogue_module.QUICK_HINT)
    observed, durations, cache_timings, overheads, pairs = [], [], [], [], []

    async def warm() -> None:
        async with dialogue._quick_request(conv.npc):
            await quick.prewarm(instructions=conv.persona.instructions, messages=history)

    async def first_delta(request) -> float:
        started = time.monotonic()
        async with aclosing(dialogue._quick_stream(
            conv.npc, instructions=conv.persona.instructions, messages=request, tools=[], temperature=0,
        )) as events:
            async for event in events:
                if isinstance(event, TextDelta) and event.text:
                    return time.monotonic() - started
        raise AssertionError("the live quick model produced no first words")

    try:
        await warm()
        owner_slot = dialogue._quick_slots[conv.npc]
        cases = [(phrase, expected, None) for phrase, expected in SURRENDER_PHRASES]
        cases.append(("Да", True, "Сдаёшься?"))
        for phrase, expected, context in cases:
            dialogue.surrender_context = lambda npc, words=context: words
            request = [*history, situation, Message("user", phrase)]
            conv.talked_at = started = time.monotonic()
            actual = await dialogue._surrendering(conv, request)
            durations.append(time.monotonic() - started)
            cache_timings.append({slot: dict(timing) for slot, timing in quick.timings.items()
                                  if slot != owner_slot})
            observed.append((phrase, expected, actual))
            assert dialogue._quick_slots[conv.npc] == owner_slot
        dialogue.surrender_context = lambda npc: None
        for index, (phrase, _) in enumerate(SURRENDER_PHRASES[:10]):
            request = [*history, situation, Message("user", phrase)]
            order = [True, False] if index % 2 == 0 else [False, True]
            measurements = {}
            for probe in order:
                await warm()
                if probe:
                    conv.talked_at = started = time.monotonic()
                    await dialogue._surrendering(conv, request)
                    probe_elapsed = time.monotonic() - started
                    elapsed = probe_elapsed + await first_delta(request)
                else:
                    elapsed = await first_delta(request)
                measurements[probe] = elapsed
            overheads.append(measurements[True] - measurements[False])
            pairs.append({
                "probe_first": order[0],
                "baseline_ms": round(measurements[False] * 1000, 1),
                "sequential_ms": round(measurements[True] * 1000, 1),
            })
    finally:
        await quick.aclose()
    positives = sum(expected for _, expected, _ in observed)
    correct_yes = sum(expected and actual for _, expected, actual in observed)
    false_yes = sum(not expected and actual for _, expected, actual in observed)
    print(json.dumps({
        "mode": "sequential",
        "phrases": len(observed), "correct_yes": correct_yes, "positive_count": positives,
        "false_yes": false_yes, "negative_count": len(observed) - positives,
        "probe_median_ms": round(statistics.median(durations) * 1000, 1),
        "probe_max_ms": round(max(durations) * 1000, 1),
        "first_words_overhead_ms": [round(value * 1000, 1) for value in overheads],
        "first_words_overhead_median_ms": round(statistics.median(overheads) * 1000, 1),
        "first_words_overhead_p90_ms": round(sorted(overheads)[8] * 1000, 1),
        "first_words_overhead_max_ms": round(max(overheads) * 1000, 1),
        "first_words_pairs": pairs,
        "cache_timings": cache_timings,
        "mismatches": [row for row in observed if row[1] != row[2]],
    }, ensure_ascii=False))
    assert false_yes == 0, observed
    assert correct_yes == positives, observed


async def test_surrender_probe_keeps_to_the_classifier_slot_when_the_server_has_one():
    quick = FakeBackend("да", delay=0)
    quick.slot_count = AsyncMock(return_value=dialogue_module.CLASSIFIER_SLOT + 1)
    dialogue = DialogueManager(GameLink(SimpleNamespace(send=AsyncMock())), FakeBackend(),
                               PersonaProvider(None), quick=quick)
    dialogue._check_slots()
    await dialogue._slot_check
    for npc in (LOBART, HILDA, XARDAS):
        dialogue._quick_slot(npc)
        dialogue._prewarm_history[npc] = [Message("assistant", npc)]
    slots = dict(dialogue._quick_slots)
    for slot in range(dialogue_module.PREWARM_SLOTS):
        dialogue._quick_busy[slot] = 1  # every npc slot busy: the classifier needs none of them
    conv = dialogue.open(LOBART, "Лобарт", warm=False)
    conv.talked_at = time.monotonic()
    assert await dialogue._surrendering(conv, [Message("user", "Пощади!")])
    assert await dialogue._surrendering(conv, [Message("user", "Пощади!")])
    assert quick.slot_ids == [dialogue_module.CLASSIFIER_SLOT] * 2
    assert dialogue._quick_slots == slots and set(dialogue._prewarm_history) == {LOBART, HILDA, XARDAS}


@pytest.mark.parametrize("count", [None, dialogue_module.CLASSIFIER_SLOT])
async def test_surrender_probe_borrows_an_npc_slot_without_a_classifier_slot(count):
    quick = FakeBackend("да", delay=0)
    quick.slot_count = AsyncMock(return_value=count)
    dialogue = DialogueManager(GameLink(SimpleNamespace(send=AsyncMock())), FakeBackend(),
                               PersonaProvider(None), quick=quick)
    dialogue._check_slots()
    await dialogue._slot_check
    conv = dialogue.open(LOBART, "Лобарт", warm=False)
    conv.talked_at = time.monotonic()
    assert await dialogue._surrendering(conv, [Message("user", "Пощади!")])
    assert quick.slot_ids[0] in range(dialogue_module.PREWARM_SLOTS)


async def test_surrender_probe_keeps_its_deadline_when_the_request_swallows_the_cancel(monkeypatch):
    quick = FakeBackend("да", delay=0)
    rows = []
    dialogue = DialogueManager(GameLink(SimpleNamespace(send=AsyncMock())), FakeBackend(),
                               PersonaProvider(None), quick=quick, trace=SimpleNamespace(write=rows.append))
    answered = asyncio.Event()

    async def deaf(**options):  # httpx swallows a cancel that lands while it connects
        try:
            await asyncio.sleep(5)
        except asyncio.CancelledError:
            await asyncio.sleep(0.3)
        answered.set()
        yield TextDelta("да")

    monkeypatch.setattr(quick, "stream", deaf)
    conv = dialogue.open(LOBART, "Лобарт", warm=False)
    conv.talked_at = time.monotonic() - 1.8
    started = time.monotonic()
    assert not await dialogue._surrendering(conv, [Message("user", "Пощади!")])
    assert time.monotonic() - started < 0.25 and not answered.is_set()
    assert rows[-1]["outcome"] == "late"
    slot = rows[-1]["slot"]
    assert dialogue._quick_busy[slot] == 1  # held until the request really ends
    await asyncio.wait_for(answered.wait(), 1)
    await _until(lambda: dialogue._quick_busy[slot] == 0)
