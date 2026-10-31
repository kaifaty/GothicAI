import asyncio

import pytest

from gothic_ai.chronicle import Chronicle
from gothic_ai.dialogue import DialogueManager
from gothic_ai.llm import ToolCall
from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.memory import MemoryStore
from gothic_ai.persona import PersonaProvider
from gothic_ai.quests import QuestBoard
from gothic_ai.trust import WORD_BROKEN
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import XARDAS, _until

PAY = FakeStep("Держи, заслужил.", [ToolCall("pay_reward", {"quest": "Q1"}, "c1")])


async def _setup(fake: FakeGame, llm: FakeBackend, store: MemoryStore):
    link_cm = connected(fake)
    link = await link_cm.__aenter__()
    dialogue = DialogueManager(link, llm, PersonaProvider(None), memory=store)
    chronicle = Chronicle(link, store)
    board = QuestBoard(link, dialogue, chronicle, hero_distance=lambda npc: 300.0)
    await link.attach(XARDAS)  # the fake game hands items over only for attached npc's
    await link.snapshot(XARDAS)
    return link_cm, dialogue, chronicle, board


async def test_a_lie_is_paid_found_out_and_spread():
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(script=[PAY, FakeStep("Ну?")], delay=0)
    link_cm, dialogue, chronicle, board = await _setup(fake, llm, store)
    try:
        q = await board.give(XARDAS, "Ксардас", "перебей волков", "WOLF", 2, "ITPO_HEALTH_01", "Эликсир", 2)
        spawn = next(m for m in fake.story if m["type"] == "story_spawn")
        assert (spawn["instance"], spawn["count"], spawn["near"]) == ("WOLF", 2, XARDAS)
        assert q.tag == spawn["tag"] == "Q1_WOLF"
        await _until(lambda: any(m["type"] == "story_watch" and m["npcs"] == [XARDAS] for m in fake.story))

        # the hero kills one wolf of two and says he killed them all
        await fake.send(type="npc_died", npc="WOLF", tag=q.tag, killer="PLAYER")
        await _until(lambda: chronicle.killed(q.tag, "PLAYER") == 1)
        dialogue.open(XARDAS, "Ксардас")
        assert "pay_reward" in [t.name for t in dialogue._offered(XARDAS)]
        await dialogue.say_as_player(XARDAS, "Я перебил волков [Герой отдаёт шкуры], плати.")
        await _until(lambda: q.paid and q.lie)
        assert "(Герой отдаёт шкуры)" in llm.calls[0][1][-1].content  # his words, not a remark
        assert any(a["action"] == "give_item" for a in fake.received if a["type"] == "action")
        assert chronicle.facts("lie")[0]["truth"] == "killed 1 of 2"
        assert "Ты поверил герою" in board.situation(XARDAS)
        assert "pay_reward" not in [t.name for t in dialogue._offered(XARDAS)]  # paid already

        # a wolf that lives turns up 8 m from the giver, behind a wall: nothing yet
        fake.people_near = [{"npc": "PC_THIEF", "name": "Диего", "distance": 500, "guild": 0}]
        near = [{"npc": XARDAS, "distance": 800}]
        await fake.send(type="story_world", tags=[{"tag": q.tag, "alive": 1, "dead": 1, "near": near}])
        run = asyncio.create_task(board.run())
        await asyncio.sleep(1.5)
        assert chronicle.facts("found_out") == []
        # it comes out where the giver sees it: the lie is found out
        near = [{"npc": XARDAS, "distance": 800, "seen": 800}]
        await fake.send(type="story_world", tags=[{"tag": q.tag, "alive": 1, "dead": 1, "near": near}])
        await _until(lambda: chronicle.facts("rumor") != [], 5)  # the last of the consequences
        run.cancel()
        await _until(lambda: any(m["type"] == "story_effect" for m in fake.story))
        effects = next(m for m in fake.story if m["type"] == "story_effect")["effects"]
        assert [e["do"] for e in effects] == ["refuse_talk", "attack"]  # the hero is 3 m away
        assert dialogue.conversations[XARDAS].intent.attitude == "hostile"
        told = [e.content for e in dialogue.conversations[XARDAS].memory.entries]
        assert any("герой солгал тебе" in t and "8 м" in t for t in told)
        heard = [e.content for e in dialogue.conversations["PC_THIEF"].memory.entries]
        assert any(t.startswith("[Слух: Ксардас") for t in heard)
        assert "солгал тебе об этом" in board.situation(XARDAS)
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("killer", ["PLAYER", "NONE_100_XARDAS"])
async def test_the_truth_is_no_lie_and_quests_follow_the_save(killer):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(script=[PAY, FakeStep("Ну?")], delay=0)
    link_cm, dialogue, chronicle, board = await _setup(fake, llm, store)
    try:
        q = await board.give(XARDAS, "Ксардас", "перебей волков", "WOLF", 1, "ITPO_HEALTH_01", "Эликсир", 1)
        await fake.send(type="npc_died", npc="WOLF", tag=q.tag, killer=killer)
        await _until(lambda: board.done(q))
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Волк мёртв.")
        await _until(lambda: chronicle.facts("paid") != [])
        assert not q.lie and chronicle.facts("lie") == []

        # a load of a save made before the quest: it is gone; one made after it: it is back, paid
        head = store.head("A")
        await fake.send(type="session", branch="B", parent="A", parent_seq=0, reason="load")
        await _until(lambda: board.quests == {})
        await fake.send(type="session", branch="C", parent="A", parent_seq=head, reason="load")
        await _until(lambda: "Q1" in board.quests and board.quests["Q1"].paid)
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("lied", [False, True])
async def test_pay_reward_names_the_actual_trust_reason(lied):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(delay=0)
    link_cm, dialogue, chronicle, board = await _setup(fake, llm, store)
    try:
        q = await board.give(XARDAS, "Ксардас", "перебей волков", "WOLF", 1, "ITPO_HEALTH_01", "Эликсир", 1)
        chronicle.record({"fact": "gate_vouch_broken", "npc": XARDAS, "crime_kind": "theft"})
        if lied:
            chronicle.record({"fact": "caught", "npc": XARDAS, "quest": "OLD_QUEST"})
        record = board.trust.of(XARDAS)
        assert record.crimes and not record.broke
        assert bool(record.lied) is lied
        conv = dialogue.open(XARDAS, "Ксардас")
        result = await board.handle(dialogue, conv, ToolCall("pay_reward", {"quest": q.id}, "c1"), None)
        assert result is not None and result.status == "rejected"
        reason = (
            "его дела подорвали твоё доверие "
            "(ты поручился за него, а он совершил преступление: «theft»)"
        )
        if lied:
            reason = f"он уже лгал тебе; {reason}"
        else:
            assert "лгал" not in result.output
        assert result.output == f"Отклонено: {reason}. {WORD_BROKEN}"
        assert not q.paid and chronicle.facts("paid") == []
    finally:
        await link_cm.__aexit__(None, None, None)
