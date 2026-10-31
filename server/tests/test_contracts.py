"""Contracts (docs/contracts.md): rewards past the inventory — experience, teaching, the stash — and tasks
the code pays for."""

from typing import Any

from gothic_ai.chronicle import Chronicle
from gothic_ai.contracts import Contracts, Give, Kill, Reward, Task, symbol_by_name
from gothic_ai.deals import Deals
from gothic_ai.dialogue import DialogueManager
from gothic_ai.llm import ToolCall
from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.memory import MemoryStore
from gothic_ai.persona import PersonaProvider
from gothic_ai.protocol import RosterEntry
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import LOBART, XARDAS, _until

GOLD, RUNE, FEAR, FUR = "ITMI_GOLD", "ITRU_SUMSKEL", "ITRU_FEAR", "ITAT_WOLFFUR"
WOLF = 27  # GIL_WOLF, as the fake scripts number it


def _terms(**more: Any) -> dict[str, Any]:
    """propose_deal's arguments: all of them, as a strict tool schema has the model send them."""
    args: dict[str, Any] = {
        "npc_gives": [],
        "hero_gives": [],
        "forgive": [],
        "silence": [],
        "hero_later": [],
        "repay": [],
        "npc_xp": 0,
        "npc_teach": [],
        "task": {"kill": [], "bring": [], "days": 0},
        "reward": {"npc_gives": [], "xp": 0, "teach": []},
    }
    args.update(more)
    return args


async def _setup(fake: FakeGame, llm: FakeBackend, store: MemoryStore):
    fake.symbols = {GOLD: "item", FUR: "item", LOBART: "npc"}
    fake.item_names = {GOLD: "Золото", RUNE: "Вызов скелета", FEAR: "Страх", FUR: "Волчья шкура"}
    fake.vars = {"GIL_WOLF": WOLF, "SPL_SUMMONSKELETON": 47}
    fake.npc_states = {LOBART: {"state": "alive", "name": "Лобарт"}}
    fake.npc_items = {XARDAS: {}}
    link_cm = connected(fake)
    link = await link_cm.__aenter__()
    dialogue = DialogueManager(link, llm, PersonaProvider(None), memory=store)
    chronicle = Chronicle(link, store)
    deals = Deals(link, dialogue, chronicle, names=lambda name: {"Лобарт": LOBART}.get(name))
    dialogue.extensions += [deals, deals.contracts]
    await link.snapshot(XARDAS)
    dialogue.open(XARDAS, "Ксардас")
    await deals.contracts.prepare(XARDAS)  # the stash's names
    return link_cm, link, dialogue, chronicle, deals


async def _propose_accept(fake: FakeGame, link, deals: Deals, args: dict[str, Any]) -> dict[str, Any]:
    out = await deals.propose(XARDAS, "Ксардас", args, link.snapshots.get(XARDAS))
    assert out.status == "accepted", out.output
    before = len(fake.deals)
    await _until(lambda: len(fake.deals) > before or any(m["type"] == "deal_show" for m in fake.deals))
    shown = [m for m in fake.deals if m["type"] == "deal_show"][-1]
    await fake.send(type="deal_accept", deal=shown["deal"], version=shown["version"])
    return shown


def _effects(fake: FakeGame) -> list[dict[str, Any]]:
    return [m for m in fake.story if m["type"] == "story_effect"][-1]["effects"]


async def test_stash_teaching_and_experience_now():
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(reply="Бери.", delay=0)
    link_cm, link, dialogue, chronicle, deals = await _setup(fake, llm, store)
    try:
        assert "Вызов скелета (ITRU_SUMSKEL) ×1" in (deals.contracts.situation(XARDAS) or "")
        fake.hero_items, fake.hero_lp = {GOLD: 200}, 10
        shown = await _propose_accept(
            fake,
            link,
            deals,
            _terms(
                npc_gives=[{"item": RUNE, "count": 1}],
                hero_gives=[{"item": GOLD, "count": 100}],
                npc_xp=100,
                npc_teach=[{"what": "attribute", "arg": "ATR_STRENGTH", "count": 5}],
            ),
        )
        texts = [t["text"] for t in shown["npc_gives"]]
        assert texts == ["Вызов скелета", "Опыт +100", "Учит: сила +5"]
        await _until(lambda: chronicle.facts("deal") != [])
        # teaching first, then the things (the rune out of the stash first), experience last
        assert [e["do"] for e in _effects(fake)] == ["teach", "give", "create", "give", "xp"]
        assert fake.hero_lp == 5 and fake.hero_xp == 100 and fake.hero_items[RUNE] == 1
        assert fake.taught == [("attribute", "ATR_STRENGTH", 5)]
        fact = chronicle.facts("deal")[0]
        assert fact["npc_gives"][0]["stash"] == 1 and fact["xp"] == 100 and fact["teach"][0]["count"] == 5
        assert RUNE not in deals.stash.left(XARDAS)  # given for good
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_greg_sees_his_pirate_stash_and_can_offer_it():
    greg = "PIR_1300_ADDON_GREG_NW"
    items = {
        GOLD: (80, "Золото"),
        "ITFO_ADDON_RUM": (3, "Ром"),
        "ITMI_ADDON_WHITEPEARL": (1, "Белая жемчужина"),
        "ITMW_ADDON_PIR1HSWORD": (1, "Пиратская сабля"),
    }
    fake = FakeGame(branch="A")
    link_cm, _link, _dialogue, _chronicle, deals = await _setup(
        fake, FakeBackend(delay=0), MemoryStore(":memory:")
    )
    try:
        fake.item_names.update({item: name for item, (_, name) in items.items()})
        await deals.contracts.prepare(greg)
        situation = deals.contracts.situation(greg) or ""
        assert "Твой тайник" in situation
        assert "80 золотых (ITMI_GOLD) ×80" in situation
        for item, (count, name) in items.items():
            if item != GOLD:
                assert f"{name} ({item}) ×{count}" in situation
        out = await deals.propose(
            greg,
            "Грег",
            _terms(npc_gives=[{"item": item, "count": count} for item, (count, _) in items.items()]),
            None,
        )
        assert out.status == "accepted", out.output
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_too_few_learning_points_the_offer_goes():
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(reply="Тогда в другой раз.", delay=0)
    link_cm, link, dialogue, chronicle, deals = await _setup(fake, llm, store)
    try:
        fake.hero_lp = 2
        fake.learn_cost = {("circle", ""): 20}
        await _propose_accept(
            fake, link, deals, _terms(npc_teach=[{"what": "circle", "arg": "", "count": 1}])
        )
        await _until(lambda: XARDAS not in deals.open)
        assert chronicle.facts("deal") == [] and fake.taught == []
        await _until(lambda: any("очков обучения" in m.content for _, msgs, _ in llm.calls for m in msgs))
        assert "notice" in fake.statuses
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_kill_task_paid_when_he_comes_back():
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(
        script=[
            FakeStep(
                "Двух волков — и руна страха твоя.",
                [
                    ToolCall(
                        "propose_deal",
                        _terms(
                            task={"kill": [{"target": "GIL_WOLF", "count": 2}], "bring": [], "days": 3},
                            reward={"npc_gives": [{"item": FEAR, "count": 1}], "xp": 500, "teach": []},
                        ),
                        "c1",
                    )
                ],
            ),
            FakeStep("Ступай."),
        ],
        delay=0,
    )
    link_cm, link, dialogue, chronicle, deals = await _setup(fake, llm, store)
    try:
        await dialogue.say_as_player(XARDAS, "Дай работу.")
        await _until(lambda: any(m["type"] == "deal_show" for m in fake.deals))
        shown = [m for m in fake.deals if m["type"] == "deal_show"][-1]
        assert [t["text"] for t in shown["hero_gives"]] == [
            "Поручение: убить: волки x2",
            "Срок: до конца дня 5",
        ]
        assert [t["text"] for t in shown["npc_gives"]] == ["За поручение: Страх", "За поручение: опыт +500"]
        await fake.send(type="deal_accept", deal=shown["deal"], version=shown["version"])
        await _until(lambda: chronicle.facts("contract") != [])
        c = chronicle.facts("contract")[0]
        assert (c["due"], c["kill"][0]["guild"]) == (4, WOLF) and c["reward"]["xp"] == 500
        assert "C1: убить: волки — 0 из 2" in (deals.contracts.situation(XARDAS) or "")

        await fake.send(type="hero_kill", npc="SHEEP", name="Овца", guild=18, monster=True)
        await fake.send(type="hero_kill", npc="WOLF", name="Волк", guild=WOLF, monster=True)
        await _until(lambda: len(chronicle.facts("contract_kill")) == 1)
        assert chronicle.facts("contract_done") == []
        await fake.send(type="hero_kill", npc="YWOLF", name="Молодой волк", guild=WOLF, monster=True)
        await _until(lambda: chronicle.facts("contract_done") != [])
        assert fake.hero_items.get(FEAR, 0) == 0  # not paid until he comes back

        await dialogue.say_as_player(XARDAS, "Волки мертвы.")
        await _until(lambda: chronicle.facts("contract_paid") != [])
        assert fake.hero_items[FEAR] == 1 and fake.hero_xp == 500
        assert [e["do"] for e in _effects(fake)] == ["create", "give", "xp"]
        notes = [e.content for e in dialogue.conversations[XARDAS].memory.entries]
        assert any("Герой исполнил твоё поручение C1" in n for n in notes)
        assert "C1" not in (deals.contracts.situation(XARDAS) or "")
        # later kills count for nothing more
        await fake.send(type="hero_kill", npc="WOLF", name="Волк", guild=WOLF, monster=True)
        await dialogue.say_as_player(XARDAS, "Ещё волк.")
        assert len(chronicle.facts("contract_paid")) == 1 and fake.hero_items[FEAR] == 1
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_a_person_by_name_and_the_task_void_when_another_kills_him():
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(reply="Хорошо.", delay=0)
    link_cm, link, dialogue, chronicle, deals = await _setup(fake, llm, store)
    try:
        task = {"kill": [{"target": "Лобарт", "count": 3}], "bring": [], "days": 0}
        await _propose_accept(fake, link, deals, _terms(task=task, npc_xp=50))
        await _until(lambda: chronicle.facts("contract") != [])
        kill = chronicle.facts("contract")[0]["kill"][0]
        assert (kill["target"], kill["count"], kill["label"]) == (LOBART, 1, "Лобарт")
        assert chronicle.facts("contract")[0]["due"] is None
        await fake.send(type="npc_died", npc=LOBART, name="Лобарт", killer="BAU_951_HILDA")
        await _until(lambda: chronicle.facts("contract_void") != [])
        assert "исполнить уже нельзя" in (deals.contracts.situation(XARDAS) or "")

        # one already dead is no task
        fake.npc_states[LOBART] = {"state": "dead", "name": "Лобарт"}
        out = await deals.propose(XARDAS, "Ксардас", _terms(task=task), link.snapshots.get(XARDAS))
        assert out.status == "rejected" and "уже мёртв" in out.output
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_bring_task_paid_once_he_has_it_from_the_stash():
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(reply="Хорошо.", delay=0)
    link_cm, link, dialogue, chronicle, deals = await _setup(fake, llm, store)
    try:
        task = {"kill": [], "bring": [{"item": FUR, "count": 3}], "days": 0}
        reward = {"npc_gives": [{"item": GOLD, "count": 100}], "xp": 0, "teach": []}
        await _propose_accept(fake, link, deals, _terms(task=task, reward=reward))
        await _until(lambda: chronicle.facts("contract") != [])

        await deals.contracts.prepare(XARDAS)
        assert chronicle.facts("contract_paid") == []
        assert "он ещё не принёс: Волчья шкура ×3" in (deals.contracts.situation(XARDAS) or "")

        fake.hero_items = {FUR: 3}
        await deals.contracts.prepare(XARDAS)
        assert chronicle.facts("contract_paid") != []
        assert fake.hero_items == {FUR: 0, GOLD: 100} and fake.npc_items[XARDAS][FUR] == 3
        assert deals.stash.left(XARDAS)[GOLD] == 400  # the gold came out of the stash
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_dishonest_task_refuses_only_when_done_and_never_pays_after_a_load():
    fake = FakeGame(branch="A")
    link_cm, link, dialogue, chronicle, deals = await _setup(
        fake, FakeBackend(reply="Хорошо.", delay=0), MemoryStore(":memory:")
    )
    try:
        deal = "dishonest-contract"
        chronicle.record(
            {
                "fact": "npc_intent",
                "npc": XARDAS,
                "deal": deal,
                "keeps": False,
                "how": ["contract"],
                "why": "dishonesty",
            }
        )
        contract = deals.contracts.add(
            deal,
            XARDAS,
            "Ксардас",
            Task(kill=(Kill("GIL_WOLF", WOLF, "волки", 1),), bring=(Give(FUR, 3, "Волчья шкура"),)),
            Reward(gives=(Give(GOLD, 100, "Золото"),), xp=50),
            fake.day,
        )
        roster = {
            XARDAS: RosterEntry(id=XARDAS, name="Ксардас", distance=100),
            LOBART: RosterEntry(id=LOBART, name="Лобарт", distance=300),
            "BEHIND_WALL": RosterEntry(id="BEHIND_WALL", name="Сосед", distance=300, heard=2000),
        }
        deals.contracts.roster = lambda: roster
        assert not await deals.contracts.pay(contract)
        assert chronicle.facts("npc_broken") == []  # no kill, so no reward is due

        await fake.send(type="hero_kill", npc="WOLF", name="Волк", guild=WOLF, monster=True)
        await _until(lambda: contract.state == "done")
        await deals.contracts.prepare(XARDAS)
        assert chronicle.facts("npc_broken") == []  # the furs still have to be brought
        assert "он ещё не принёс" in (deals.contracts.situation(XARDAS) or "")

        fake.hero_items[FUR] = 3
        await deals.contracts.prepare(XARDAS)
        fact = chronicle.facts("npc_broken")[0]
        assert (fact["how"], fact["deal"], fact["contract"], fact["npc_name"]) == (
            "unpaid",
            deal,
            contract.id,
            "Ксардас",
        )
        assert fact["witnesses"] == [LOBART]
        cue = "[Герой выполнил поручение и ждёт платы; ты решил не платить]"
        assert cue in (deals.contracts.situation(XARDAS) or "")
        assert fake.hero_items == {FUR: 3} and fake.hero_xp == 0
        assert chronicle.facts("contract_paid") == []
        assert deals.stash.left(XARDAS)[GOLD] == 500

        await deals.contracts.prepare(XARDAS)
        assert not await deals.contracts.pay(contract)
        again = Contracts(link, dialogue, chronicle, deals.stash)
        assert again.contracts[contract.id].state == "unpaid"
        assert again.contracts[contract.id].deal == deal
        assert cue in (again.situation(XARDAS) or "")
        await again.prepare(XARDAS)
        assert len(chronicle.facts("npc_broken")) == 1 and chronicle.facts("contract_paid") == []
        assert fake.hero_items == {FUR: 3} and fake.hero_xp == 0

        # Another accepted exchange can settle it without reopening the dishonest obligation.
        fake.hero_items[GOLD] = 50
        before = len(fake.deals)
        out = await deals.propose(
            XARDAS,
            "Ксардас",
            _terms(
                npc_gives=[{"item": GOLD, "count": 100}], hero_gives=[{"item": GOLD, "count": 50}]
            ),
            link.snapshots[XARDAS],
            hero_words=("Дай немного золота",),
        )
        assert out.status == "accepted"
        await _until(lambda: len(fake.deals) > before)
        shown = [m for m in fake.deals if m["type"] == "deal_show"][-1]
        await fake.send(type="deal_accept", deal=shown["deal"], version=shown["version"])
        await _until(lambda: fake.hero_items.get(GOLD) == 100)
        assert fake.hero_items[FUR] == 3 and len(chronicle.facts("npc_broken")) == 1
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_a_task_past_its_day_is_a_broken_word_and_survives_a_reload():
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(reply="Хорошо.", delay=0)
    link_cm, link, dialogue, chronicle, deals = await _setup(fake, llm, store)
    try:
        task = {"kill": [{"target": "GIL_WOLF", "count": 2}], "bring": [], "days": 1}
        await _propose_accept(fake, link, deals, _terms(task=task, npc_xp=10))
        await _until(lambda: chronicle.facts("contract") != [])
        await fake.send(type="hero_kill", npc="WOLF", name="Волк", guild=WOLF, monster=True)
        await _until(lambda: chronicle.facts("contract_kill") != [])

        # the same branch read again: the progress is back
        again = Contracts(link, dialogue, chronicle, deals.stash)
        assert again.contracts["C1"].kill[0].done == 1 and again.contracts["C1"].state == "open"

        assert deals.contracts.overdue(2) == []  # due by the end of day 2 (0-based)
        assert [c.id for c in deals.contracts.overdue(3)] == ["C1"]
        assert chronicle.facts("broken")[0]["contract"] == "C1"
        assert "он не сдержал слово" in (deals.contracts.situation(XARDAS) or "")
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_what_the_terms_may_not_be():
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(reply="Хорошо.", delay=0)
    link_cm, link, dialogue, chronicle, deals = await _setup(fake, llm, store)
    try:
        snap = link.snapshots.get(XARDAS)

        async def why(**terms: Any) -> str:
            out = await deals.propose(XARDAS, "Ксардас", _terms(**terms), snap)
            assert out.status == "rejected"
            return out.output

        reward = {"npc_gives": [], "xp": 100, "teach": []}
        assert "только за поручение" in await why(reward=reward)
        assert "в игре нет SPL_NOPE" in await why(npc_teach=[{"what": "rune", "arg": "SPL_NOPE", "count": 1}])
        assert "ATR_STRENGTH" in await why(npc_teach=[{"what": "attribute", "arg": "ATR_LUCK", "count": 1}])
        assert "круг магии" in await why(npc_teach=[{"what": "circle", "arg": "", "count": 7}])
        kill = {"kill": [{"target": "Кто-то", "count": 1}], "bring": [], "days": 0}
        assert "не знаю, кто это" in await why(task=kill)
        assert "у тебя нет ITRU_ZAP" in await why(npc_gives=[{"item": "ITRU_ZAP", "count": 1}])
        assert "у тебя только 1" in await why(npc_gives=[{"item": RUNE, "count": 2}])

        # a rune is taught by its spell, named after its rune when the scripts have one
        out = await deals.propose(
            XARDAS,
            "Ксардас",
            _terms(npc_teach=[{"what": "rune", "arg": "SPL_SUMMONSKELETON", "count": 1}]),
            snap,
        )
        assert out.status == "accepted" and "руна «SPL_SUMMONSKELETON»" in out.output
    finally:
        await link_cm.__aexit__(None, None, None)


def test_symbol_by_name_only_when_one():
    class Card:
        def __init__(self, name: str) -> None:
            self.name = name

    cards = {LOBART: Card("Лобарт"), "VLK_1": Card("Стражник"), "VLK_2": Card("Стражник")}
    assert symbol_by_name(cards, " лобарт ") == LOBART
    assert symbol_by_name(cards, "Стражник") is None
    assert symbol_by_name(cards, "Хильда") is None
