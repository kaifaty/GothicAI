import json
from pathlib import Path
from typing import Any

import pytest

from gothic_ai.authoring import PROPOSE, Author
from gothic_ai.chronicle import Chronicle
from gothic_ai.director import Director, Pacing
from gothic_ai.gamedata.storygraph import StoryGraph, load
from gothic_ai.llm import ToolCall
from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.memory import MemoryStore
from gothic_ai.places import VERSION, PlaceBook
from gothic_ai.story import StoryPatches
from gothic_ai.validator import Patches, Rules, Story, WorldState, validate
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import _until
from tests.test_director import GRAPH, HILDA, NAMES, QUEST, SYMBOLS
from tests.test_memory import close_memory_stores as close_memory_stores

BOOK = {
    "version": VERSION,
    "world": "NEWWORLD.ZEN",
    "points": {
        "NW_FARM1_INHOUSE_01": 0,
        "NW_FARM1_OUT_01": 1,
        "NW_FARM1_OUT_02": 1,
        "NW_FARM1_FIELD_01": 2,
        "NW_FARM1_FIELD_02": 2,
        "NW_CITY_01": 3,
    },
    "places": [
        {"id": "FARM1_INHOUSE", "ru": "дом Лобарта", "where_ru": "в доме Лобарта", "center": [0, 0, 0]},
        {
            "id": "FARM1_ENTRANCE",
            "ru": "двор",
            "where_ru": "во дворе перед домом Лобарта",
            "center": [800, 0, 0],
        },
        {"id": "FARM1_FIELD", "ru": "поля", "where_ru": "на полях фермы Лобарта", "center": [2500, 0, 0]},
        {"id": "CITY", "ru": "город", "where_ru": "в городе", "center": [90000, 0, 0]},
    ],
}
for _p in BOOK["places"]:
    _p.update(en=_p["ru"], where_en=_p["where_ru"])


def _quest(**change: Any) -> ToolCall:
    args = {
        "title": "Пила Хильды",
        "item": "ITMI_SAW",
        "place": "FARM1_FIELD",
        "give_description": "Тебе нужна помощь?",
        "give_lines": [
            {"speaker": "hero", "text": "Тебе нужна помощь?"},
            {"speaker": "npc", "text": "Пилу потеряла, где-то на поле. Найдёшь — заплачу."},
        ],
        "done_description": "Вот твоя пила.",
        "done_lines": [{"speaker": "npc", "text": "Нашёл всё-таки. Держи."}],
        "gold": 20,
        "xp": 50,
        "journal_given": "Хильда потеряла пилу на поле Лобарта.",
        "journal_done": "Я вернул Хильде пилу.",
        "reason": "по хозяйству",
        **change,
    }
    return ToolCall(PROPOSE, args, "q1")


@pytest.fixture
def graph(tmp_path: Path) -> StoryGraph:
    path = tmp_path / "story.json"
    path.write_text(json.dumps(GRAPH, ensure_ascii=False), encoding="utf-8")
    return load(path)


async def _author(fake: FakeGame, llm: FakeBackend, graph: StoryGraph, store: MemoryStore):
    fake.vars = {"KAPITEL": 1, "XP_AMBIENT": 50}
    fake.npc_states = {n: {"state": "alive", "name": name} for n, name in NAMES.items()}
    fake.symbols = {**SYMBOLS, "ITMI_SAW": "item", "ITMI_RAKE": "item", "WLD_INSERTITEM": "func"}
    fake.item_names = {"ITMI_SAW": "Пила", "ITMI_RAKE": "Грабли"}
    fake.routines = {
        HILDA: {
            "entries": [
                {
                    "start": "08:00",
                    "end": "20:00",
                    "state": "ZS_COOK_STOVE",
                    "waypoint": "NW_FARM1_INHOUSE_01",
                },
                {"start": "20:00", "end": "08:00", "state": "ZS_SLEEP", "waypoint": "NW_FARM1_OUT_01"},
            ]
        }
    }
    link_cm = connected(fake)
    link = await link_cm.__aenter__()
    patches = StoryPatches(link, store)
    await patches.initialize()
    chronicle = Chronicle(link, store)
    await chronicle.initialize()
    director = Director(link, graph, patches, chronicle, llm, pacing=Pacing(cooldown_s=0), settle_s=0)
    author = Author(link, graph, patches, chronicle, director, llm, places=PlaceBook(BOOK))
    await _until(lambda: patches.branch is not None and chronicle.branch is not None)
    return link_cm, author, chronicle


def _ops(fake: FakeGame) -> list[dict[str, Any]]:
    return [op for m in fake.graph for op in m["ops"]]


async def test_the_author_makes_a_quest_after_a_rejected_proposal(graph):
    fake = FakeGame(branch="A")
    greedy = _quest(gold=500, place="CITY")
    llm = FakeBackend(script=[FakeStep("", [greedy]), FakeStep("", [_quest()])], delay=0)
    link_cm, author, chronicle = await _author(fake, llm, graph, await MemoryStore.open(":memory:"))
    try:
        premise = "Хильда потеряла пилу после ночного переполоха. Работу надо закончить до дождя."
        q = await author.quest(HILDA, premise)
        assert q is not None and (q.id, q.giver, q.gold, q.xp) == ("DIR_Q1", HILDA, 20, 50)
        # one stage: bring the saw back, it lies at a point of the field
        [stage] = q.stages
        [saw] = stage.goals
        assert (saw.kind, saw.npc, saw.item, saw.point) == ("bring", HILDA, "ITMI_SAW", "NW_FARM1_FIELD_02")

        brief = llm.calls[0][1][0].content
        assert f"Кто просит: Хильда ({HILDA})." in brief and "Где живёт и работает: в доме Лобарта." in brief
        assert "- ITMI_SAW — Пила" in brief and "- ITMI_RAKE — Грабли" in brief
        # the places around hers, not her own; the yard has a point she does not sleep at
        assert "- FARM1_FIELD — на полях фермы Лобарта, 25 м" in brief and "FARM1_INHOUSE —" not in brief
        assert "- FARM1_ENTRANCE — во дворе перед домом Лобарта, 8 м" in brief and "CITY" not in brief
        assert "Награда: опыт до 50, золото до 50." in brief
        assert f"Замысел режиссёра: {premise}" in brief
        assert f"Замысел режиссёра: {premise}" in llm.calls[1][1][0].content
        rejected = llm.calls[1][1][-1].content
        assert "места 'CITY' нет в сводке" in rejected and "золота 500, а можно от 0 до 50" in rejected

        # the book built it: its status and its stage, the dialog that gives it and the one that ends it
        ops = _ops(fake)
        assert ops[0] == {"op": "vars", "vars": {"DIR_Q1": 0, "DIR_Q1_STAGE": 0}} and fake.vars["DIR_Q1"] == 0
        give, done = ops[1]["node"], ops[2]["node"]
        assert give["id"] == "DIR_Q1_GIVE" and give["condition"] == ["==", ["var", "DIR_Q1"], ["int", 0]]
        assert give["program"] == [
            {
                "do": "call",
                "call": ["call", "WLD_INSERTITEM", ["sym", "ITMI_SAW"], ["str", "NW_FARM1_FIELD_02"]],
            }
        ]
        assert give["effects"][:2] == [
            {"do": "set", "var": "DIR_Q1", "value": 1},
            {"do": "set", "var": "DIR_Q1_STAGE", "value": 1},
        ]
        assert give["lines"][0] == {"speaker": "other", "text": "Тебе нужна помощь?"}
        assert done["id"] == "DIR_Q1_DONE" and "ITMI_SAW" in str(done["condition"])
        assert ["==", ["var", "DIR_Q1_STAGE"], ["int", 1]] in done["condition"][1:]
        # he opens the talk with what he picked in the menu, unless the model has him say his own words
        assert done["lines"] == [
            {"speaker": "other", "text": "Вот твоя пила."},
            {"speaker": "self", "text": "Нашёл всё-таки. Держи."},
        ]
        assert done["effects"][:3] == [
            {"do": "give", "item": "ITMI_SAW", "count": 1, "to": "self"},
            {"do": "give", "item": "ITMI_GOLD", "count": 20},
            {"do": "xp", "amount": 50},
        ]
        assert done["effects"][3:] == [
            {"do": "set", "var": "DIR_Q1", "value": 2},
            {"do": "set", "var": "DIR_Q1_STAGE", "value": 2},
            {
                "do": "journal",
                "topic": "Пила Хильды",
                "text": "Я вернул Хильде пилу.",
                "status": "LOG_SUCCESS",
            },
        ]
        assert len(ops) == 3  # one stage, its one goal the turn-in: no triggers
        facts = [f["fact"] for f in chronicle.facts()]
        assert facts == ["authored_rejected", "story_quest"]

        # the hero takes it, then brings the saw: the giver knows where it stands
        book = author.book
        assert await book.situation(HILDA) is None and author.situation(HILDA) is None
        await fake.send(type="dialog_chosen", npc=HILDA, node="DIR_Q1_GIVE", made=True)
        await _until(lambda: book.progress("DIR_Q1").state == "given")
        assert "найти твою пропавшую вещь — Пила: она лежит на полях фермы Лобарта." in (
            await book.situation(HILDA) or ""
        )
        await fake.send(type="dialog_chosen", npc=HILDA, node="DIR_Q1_DONE", made=True)
        await _until(lambda: book.progress("DIR_Q1").state == "done")
        assert "ты заплатил: 20 золотых, опыт +50" in (await book.situation(HILDA) or "")

        # a load of the branch brings it back as it stands: the description from the log, the rest from facts
        book.quests.clear()
        book._version = -1
        assert (await book.all())["DIR_Q1"] == q and book.progress("DIR_Q1").state == "done"

        # another: the next variable, and the model knows what she asked before
        llm.script.append(FakeStep("", [_quest(title="Грабли", item="ITMI_RAKE")]))
        again = await author.quest(HILDA)
        assert again is not None and again.id == "DIR_Q2"
        assert "уже были, не повторяйся: «Пила Хильды»" in llm.calls[-1][1][0].content
        assert "Замысел режиссёра:" not in llm.calls[-1][1][0].content
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_no_quest_from_the_plot_or_the_dead(graph):
    fake = FakeGame(branch="A")
    llm = FakeBackend(script=[], delay=0)
    link_cm, author, _ = await _author(fake, llm, graph, await MemoryStore.open(":memory:"))
    try:
        fake.npc_states[HILDA]["state"] = "dead"
        assert await author.quest(HILDA) is None
        assert await author.quest("NONE_100_XARDAS") is None
        assert llm.calls == []
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_the_brief_excludes_items_named_anywhere_in_scripts_and_unknown_to_the_game(graph):
    # Even a dialog outside Hilda's story reserves the item, in each part of the node.
    graph.node("DIA_LOBART_WORKNOW").condition = [
        "call",
        "NPC_HASITEMS",
        ["inst", "OTHER"],
        ["sym", "ITMI_SAW"],
    ]
    graph.node("DIA_XARDAS_FIRSTEXIT").condition_effects = [
        {"do": "call", "call": ["call", "CREATEINVITEM", ["inst", "SELF"], ["sym", "ITMI_RAKE"]]}
    ]
    graph.node("DIA_LOBART_WORKNOW_OK").effects.append(
        {"do": "give", "item": ["sym", "ITMI_HAMMER"], "amount": ["int", 1]}
    )
    fake = FakeGame(branch="A")
    link_cm, author, _ = await _author(
        fake, FakeBackend(script=[], delay=0), graph, await MemoryStore.open(":memory:")
    )
    try:
        fake.symbols.update({"ITMI_HAMMER": "item", "ITAT_MEATBUGFLESH": "item", "ITPL_FORESTBERRY": "var"})
        fake.item_names.update(
            {"ITAT_MEATBUGFLESH": "Мясо жука", "ITPL_FORESTBERRY": "Ягода", "ITFO_CORAGONSBEER": "Пиво"}
        )
        brief = await author.brief(HILDA)
        assert brief is not None and brief.items == {"ITAT_MEATBUGFLESH": "Мясо жука"}
        assert "Вещи, одна из которых у него пропала" in brief.text()
        neutral = brief.text(errand=False)
        assert "Вещи для поручения (id — название):" in neutral
        assert "Места рядом (id — где, как далеко от его места):" in neutral
        assert "пропала" not in neutral and "где она может лежать" not in neutral
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_a_vanilla_response_keeps_its_opening_and_the_givers_knowledge(graph):
    fake = FakeGame(branch="A")
    llm = FakeBackend(script=[FakeStep("", [_quest()])], delay=0)
    link_cm, author, _ = await _author(fake, llm, graph, await MemoryStore.open(":memory:"))
    try:
        fake.vars[QUEST] = 2
        requires = [{"kind": "quest", "quest": QUEST, "state": "success"}]
        premise = "После уборки репы Хильда хочет починить ограду, но потеряла пилу."
        q = await author.quest(HILDA, premise, requires=requires, follows=QUEST.lower())
        assert q is not None and q.requires == requires
        assert "У тебя пропала вещь: Пила." in q.knows
        assert graph.quests[QUEST].title in q.knows and premise in q.knows and QUEST not in q.knows
        brief = llm.calls[0][1][0].content
        assert "Поручение отвечает на дело «Убрать поле репы»." in brief and QUEST not in brief
        give = next(op["node"] for op in _ops(fake) if op.get("node", {}).get("id") == f"{q.id}_GIVE")
        assert fake.evaluate(give["condition"], HILDA) == 1
        fake.vars[QUEST] = 1
        assert fake.evaluate(give["condition"], HILDA) == 0
        author.book.quests.clear()
        author.book._version = -1
        assert (await author.book.all())[q.id] == q
    finally:
        await link_cm.__aexit__(None, None, None)


def test_the_directors_variables(graph):
    state = WorldState(vars={"KAPITEL": 1}, symbols={**SYMBOLS}, npcs={HILDA: "alive"})
    story, rules = Story(graph, state), Rules.of(graph)
    node = {
        "id": "DIR_Q1_GIVE",
        "npc": HILDA,
        "description": "Помочь?",
        "condition": ["==", ["var", "DIR_Q1"], ["int", 0]],
        "lines": [{"speaker": "self", "text": "Да."}],
        "effects": [{"do": "set", "var": "DIR_Q1", "value": 1}],
    }
    # the dialog needs its variable made first
    assert "в условии неизвестные имена: DIR_Q1" in validate([{"op": "add_node", "node": node}], story, rules)
    made = [{"op": "vars", "vars": {"DIR_Q1": 0}}, {"op": "add_node", "node": node}]
    assert validate(made, story, rules) == []
    after = story.then(made)
    assert after.symbols["DIR_Q1"] == "var" and after.var_values("DIR_Q1") == frozenset({0, 1})

    assert validate([{"op": "vars", "vars": {"MIS_X": 0}}], story, rules) == [
        "переменная режиссёра называется DIR_…: MIS_X"
    ]
    taken = Story(graph, WorldState(symbols={"DIR_HILDA": "npc"}))
    assert validate([{"op": "vars", "vars": {"DIR_HILDA": 0}}], taken, rules) == ["имя DIR_HILDA уже занято"]
    assert "DIR_Q1: начальное значение — целое число" in validate(
        [{"op": "vars", "vars": {"DIR_Q1": "0"}}], story, rules
    )
    # made once: a later op does not reset it
    assert Patches.of([*made, {"op": "vars", "vars": {"DIR_Q1": 5}}]).vars == {"DIR_Q1": 0}
