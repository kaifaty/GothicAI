import asyncio
import copy

import pytest

import gothic_ai.dialogue
from gothic_ai.chests import BACK, CHEST, ChestDesk
from gothic_ai.chronicle import Chronicle
from gothic_ai.deals import Deals
from gothic_ai.dialogue import DialogueManager
from gothic_ai.llm import ToolCall
from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.memory import MemoryStore
from gothic_ai.persona import PersonaProvider
from gothic_ai.talk import TalkDesk
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import _until

BRAGO = "BDT_1014_BANDIT_L"
CAVE = "MOB_18030_3240_-21880"
BOW, GOLD, ARROW = "ITRW_BOW_L_01", "ITMI_GOLD", "ITRW_ARROW"
START = [
    {"item": BOW, "name": "Короткий лук", "count": 1},
    {"item": GOLD, "name": "Золото", "count": 29},
    {"item": ARROW, "name": "Стрела", "count": 15},
]


def chest(items=START) -> dict:
    """Brago's chest in the bandits' cave, as the bridge has it: nobody owns it, it stands by his routine."""
    return {
        "id": CAVE,
        "why": "home",
        "pos": [18030.0, 3240.0, -21880.0],
        "distance": 700.0,
        "waypoint": "NW_XARDAS_BANDITS_05",
        "locked": True,
        "key": "ITKE_BANDIT",
        "has_key": False,
        "items": copy.deepcopy(items),
        "start": copy.deepcopy(START),
    }


@pytest.fixture(autouse=True)
def short_lines(monkeypatch):
    monkeypatch.setattr(gothic_ai.dialogue, "line_duration", lambda text: 0.1)


async def _desk(fake: FakeGame, llm, store: MemoryStore):
    cm = connected(fake)
    link = await cm.__aenter__()
    dialogue = DialogueManager(link, llm, PersonaProvider(None))
    TalkDesk(link, dialogue, end_silence_s=30)  # it holds the body for the walk
    chests = ChestDesk(link, store)
    dialogue.extensions.append(chests)
    dialogue.open(BRAGO, "Браго")
    return cm, dialogue, chests


def situation(llm: FakeBackend, call: int = 0) -> str:
    return "\n".join(m.content for m in llm.calls[call][1] if m.role == "developer")


def notes(dialogue: DialogueManager) -> list[str]:
    return [m.content for m in dialogue.conversations[BRAGO].history if m.role == "developer"]


def take(*items: tuple[str, int]) -> FakeStep:
    moves = [{"item": i, "count": n} for i, n in items]
    return FakeStep("Сейчас принесу.", [ToolCall(CHEST, {"take": moves, "put": [], "chest": None}, "c1")])


async def test_it_knows_what_lies_in_its_chest(tmp_path):
    fake = FakeGame()
    fake.chests[BRAGO] = [chest()]
    llm = FakeBackend("Чего надо?", delay=0)
    cm, dialogue, chests = await _desk(fake, llm, MemoryStore(tmp_path / "m.sqlite3"))
    try:
        dialogue.say_as_player(BRAGO, "Что у тебя есть?")
        await _until(lambda: len(llm.calls) == 1 and not dialogue.speaking(BRAGO))
    finally:
        await cm.__aexit__(None, None, None)

    assert (
        "Сундук там, где ты живёшь — в 7 м отсюда, заперт. В нём, насколько ты знаешь: Короткий лук "
        "(ITRW_BOW_L_01), Золото (ITMI_GOLD) ×29, Стрела (ITRW_ARROW) ×15."
    ) in situation(llm)
    assert CHEST in [t.name for t in llm.calls[0][2]]  # offered: it has a chest


async def test_no_chest_no_tool(tmp_path):
    fake = FakeGame()
    llm = FakeBackend("Чего надо?", delay=0)
    cm, dialogue, chests = await _desk(fake, llm, MemoryStore(tmp_path / "m.sqlite3"))
    try:
        dialogue.say_as_player(BRAGO, "Что у тебя есть?")
        await _until(lambda: len(llm.calls) == 1 and not dialogue.speaking(BRAGO))
    finally:
        await cm.__aexit__(None, None, None)
    assert "сундук" not in situation(llm).lower() and CHEST not in [t.name for t in llm.calls[0][2]]


async def test_it_goes_to_its_chest_and_takes_it_out(tmp_path):
    fake = FakeGame()
    fake.chests[BRAGO] = [chest()]
    llm = FakeBackend(script=[take((BOW, 1)), FakeStep("")])
    cm, dialogue, chests = await _desk(fake, llm, MemoryStore(tmp_path / "m.sqlite3"))
    try:
        dialogue.say_as_player(BRAGO, "Принеси свой лук.")
        await _until(lambda: len(llm.calls) == 2 and not dialogue.speaking(BRAGO), 5)
    finally:
        await cm.__aexit__(None, None, None)

    acts = [m for m in fake.received if m.get("type") == "action" and m["action"] == "use_chest"]
    assert acts and acts[0]["args"] == {"chest": CAVE, "take": [{"item": BOW, "count": 1}], "put": []}
    assert fake.npc_items[BRAGO] == {BOW: 1}
    assert "[Ты сходил к своему сундуку и открыл его; достал: Короткий лук.]" in notes(dialogue)
    assert BOW not in chests.known(BRAGO)[CAVE].items  # it knows it took it out
    # back from the chest it goes on talking (the bow in hand: a give, a deal), its chest's tool offered too
    assert llm.calls[-1][1][-1].content == BACK and CHEST in [t.name for t in llm.calls[-1][2]]


async def test_what_was_taken_unseen_is_missing_when_it_looks(tmp_path):
    # the hero emptied the chest of its gold before Brago ever spoke of it: Brago knows it as it was
    fake = FakeGame()
    fake.chests[BRAGO] = [chest([i for i in START if i["item"] != GOLD])]
    llm = FakeBackend(script=[take((BOW, 1)), FakeStep("")])
    cm, dialogue, chests = await _desk(fake, llm, MemoryStore(tmp_path / "m.sqlite3"))
    try:
        dialogue.say_as_player(BRAGO, "Принеси свой лук.")
        await _until(lambda: any("сундуку" in n for n in notes(dialogue)), 5)
    finally:
        await cm.__aexit__(None, None, None)

    assert (
        "[Ты сходил к своему сундуку и открыл его; достал: Короткий лук. Из сундука пропало: Золото ×29 — "
        "кто-то в нём рылся.]"
    ) in notes(dialogue)
    assert GOLD not in chests.known(BRAGO)[CAVE].items  # it knows now


async def test_npc_suspicion_runs_only_after_owner_opens_and_notices_missing(tmp_path):
    fake = FakeGame()
    fake.chests[BRAGO] = [chest([i for i in START if i["item"] != GOLD])]
    observed = []
    store = MemoryStore(tmp_path / "m.sqlite3")
    async with connected(fake) as link:
        desk = ChestDesk(link, store)

        def suspect(npc, chest_id, missing, **kwargs):
            observed.append((npc, chest_id, missing))
            return [{"suspect": "VINO"}]

        desk.npc_suspect = suspect
        await desk.prepare(BRAGO)
        assert observed == []  # cached world truth does not reveal the loss to its owner
        note = await desk.looked(BRAGO, CAVE, "")
        assert observed == [(BRAGO, CAVE, {GOLD: 29})]
        assert "подозрение, не доказательство" in note
        await desk.looked(BRAGO, CAVE, "")
        assert len(observed) == 1
        if desk._tasks:
            await asyncio.gather(*desk._tasks)
    store.close()


async def test_what_is_not_in_its_chest_it_cannot_take_out(tmp_path):
    fake = FakeGame()
    fake.chests[BRAGO] = [chest()]
    llm = FakeBackend(script=[take(("ITMW_1H_SWORD", 1)), FakeStep("Нет у меня меча.")])
    cm, dialogue, chests = await _desk(fake, llm, MemoryStore(tmp_path / "m.sqlite3"))
    try:
        dialogue.say_as_player(BRAGO, "Принеси меч.")
        await _until(lambda: len(llm.calls) == 2 and not dialogue.speaking(BRAGO))
    finally:
        await cm.__aexit__(None, None, None)

    result = next(i.output for i in llm.calls[1][1] if getattr(i, "output", None))
    assert result.startswith("Отклонено: ITMW_1H_SWORD нет в сундуке. Там: Короткий лук")
    assert not [m for m in fake.received if m.get("type") == "action" and m["action"] == "use_chest"]


async def test_what_it_knows_of_its_chest_is_kept_in_the_save_branch(tmp_path):
    store = MemoryStore(tmp_path / "m.sqlite3")
    fake = FakeGame()
    fake.chests[BRAGO] = [chest()]
    llm = FakeBackend(script=[take((GOLD, 10)), FakeStep("")])
    cm, dialogue, chests = await _desk(fake, llm, store)
    try:
        dialogue.say_as_player(BRAGO, "Дай денег.")
        await _until(lambda: any("сундуку" in n for n in notes(dialogue)), 5)
    finally:
        await cm.__aexit__(None, None, None)
    # the server starts again: what it knows comes back from the branch
    fake2 = FakeGame()
    cm2 = connected(fake2)
    link = await cm2.__aenter__()
    try:
        again = ChestDesk(link, store)
        assert again.known(BRAGO)[CAVE].items[GOLD] == (19, "Золото")
    finally:
        await cm2.__aexit__(None, None, None)


async def test_a_deal_points_to_what_it_keeps_in_its_chest(tmp_path):
    fake = FakeGame()
    cm = connected(fake)
    link = await cm.__aenter__()
    try:
        dialogue = DialogueManager(link, FakeBackend("Ну?", delay=0), PersonaProvider(None))
        deals = Deals(link, dialogue, Chronicle(link, MemoryStore(tmp_path / "m.sqlite3")))
        deals.in_chests = lambda npc: {BOW: (1, "Короткий лук")}
        snap = await link.snapshot(BRAGO)
        gives, why = deals._npc_gives(BRAGO, [{"item": BOW, "count": 1}], snap)
    finally:
        await cm.__aexit__(None, None, None)
    assert gives is None
    assert "Короткий лук (ITRW_BOW_L_01) ×1 лежит у тебя в сундуке: сначала достань (инструмент chest)" in why
