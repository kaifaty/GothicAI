import asyncio
import json
from pathlib import Path
from typing import Any

import pytest

from gothic_ai.chronicle import Chronicle
from gothic_ai.director import Director, Pacing, _rest
from gothic_ai.gamedata.storygraph import StoryGraph, load, walk
from gothic_ai.llm import ToolCall
from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.memory import MemoryStore
from gothic_ai.questbook import Goal, Quest, QuestBook, Stage, Talk
from gothic_ai.story import StoryPatches
from gothic_ai.validator import Caps, Patches, Rules, Story, WorldState, validate
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import _until

LOBART, HILDA, VINO, XARDAS = "BAU_950_LOBART", "BAU_951_HILDA", "BAU_952_VINO", "NONE_100_XARDAS"
QUEST = "MIS_LOBART_RUEBEN"
RUNNING = ["==", ["var", QUEST], ["int", 1, "LOG_RUNNING"]]
HAS = [">=", ["call", "NPC_HASITEMS", ["inst", "OTHER"], ["sym", "ITPL_BEET"]], ["int", 1]]
HAS_20 = [">=", ["call", "NPC_HASITEMS", ["inst", "OTHER"], ["sym", "ITPL_BEET"]], ["int", 20]]
LESS_20 = ["<", ["call", "NPC_HASITEMS", ["inst", "OTHER"], ["sym", "ITPL_BEET"]], ["int", 20]]
KAPITEL_LT_3 = ["<", ["var", "KAPITEL"], ["int", 3]]


def _node(id: str, npc: str, effects: list[dict[str, Any]], **extra: Any) -> dict[str, Any]:
    return {
        "id": id,
        "kind": "dialog",
        "npc": npc,
        "description": extra.pop("description", id),
        "effects": effects,
        "writes": [],
        "nr": 1,
        "important": False,
        "permanent": False,
        "trade": False,
        **extra,
    }


def _line(text: str, speaker: str = "SELF", when: Any = None) -> dict[str, Any]:
    line = {
        "do": "line",
        "id": "X",
        "text": text,
        "speaker": ["inst", speaker],
        "listener": ["inst", "OTHER"],
    }
    return {**line, "when": when} if when else line


# Lobart's turnips, as storygraph lifts them, cut down
GRAPH = {
    "version": 1,
    "nodes": [
        _node(
            "DIA_LOBART_WORKNOW_OK",
            LOBART,
            [
                {
                    "do": "journal_entry",
                    "topic": "TOPIC_RUEBEN",
                    "text": "Лобарт хочет, чтобы я собрал репу.",
                },
                {"do": "set", "var": QUEST, "value": ["int", 1, "LOG_RUNNING"]},
            ],
            kind="choice",
            parents=["DIA_LOBART_WORKNOW"],
        ),
        _node("DIA_LOBART_WORKNOW", LOBART, [], condition=KAPITEL_LT_3),
        _node(
            "DIA_LOBART_RUEBENRUNNING",
            LOBART,
            [
                _line("Вот твоя репа!", "OTHER"),
                _line("Да, похоже, ты не такой уж бездельник.", when=HAS_20),
                {"do": "set", "var": QUEST, "value": ["int", 2, "LOG_SUCCESS"], "when": HAS_20},
                {"do": "xp", "amount": ["int", 50, "XP_LOBARTHOLRUEBEN"], "when": HAS_20},
                {
                    "do": "choice",
                    "node": "DIA_LOBART_RUEBENRUNNING_GOLD",
                    "text": "Дай 5 золотых!",
                    "when": HAS_20,
                },
                {
                    "do": "choice",
                    "node": "DIA_LOBART_RUEBENRUNNING_BILLIGER",
                    "text": "Лучше продай одежду дешевле!",
                    "when": HAS_20,
                },
                _line("Но ты не собрал и двадцати!", when=LESS_20),
                {"do": "end", "when": LESS_20},
            ],
            condition=["&&", KAPITEL_LT_3, RUNNING, HAS],
            description="Вот твоя репа!",
            permanent=True,
        ),
        _node(
            "DIA_LOBART_RUEBENRUNNING_GOLD",
            LOBART,
            [
                _line("Держи."),
                {
                    "do": "give",
                    "from": ["inst", "SELF"],
                    "to": ["inst", "OTHER"],
                    "item": ["sym", "ITMI_GOLD"],
                    "amount": ["int", 5],
                },
            ],
            kind="choice",
            parents=["DIA_LOBART_RUEBENRUNNING"],
            description="Дай мне 5 золотых монет!",
        ),
        _node(
            "DIA_LOBART_RUEBENRUNNING_BILLIGER",
            LOBART,
            [
                _line("Лучше продай одежду дешевле!", "OTHER"),
                _line("Хорошо! Я отдам тебе ее на 10 золотых дешевле."),
            ],
            kind="choice",
            parents=["DIA_LOBART_RUEBENRUNNING"],
            description="Лучше продай одежду дешевле!",
        ),
        _node("DIA_HILDA_HALLO", HILDA, [], condition=["!=", ["var", QUEST], ["int", 2]], important=True),
        _node("DIA_XARDAS_FIRSTEXIT", XARDAS, [{"do": "chapter", "value": ["int", 1]}], condition=None),
    ],
    "functions": {},
    "instances": {"LOBART": LOBART, "HILDA": HILDA},
    "vars": {
        QUEST: {
            "readers": ["DIA_HILDA_HALLO", "DIA_LOBART_RUEBENRUNNING"],
            "writers": ["DIA_LOBART_WORKNOW_OK", "DIA_LOBART_RUEBENRUNNING"],
            "others": [],
        },
        "KAPITEL": {"readers": ["DIA_LOBART_WORKNOW"], "writers": [], "others": ["B_KAPITELWECHSEL"]},
    },
    "quests": [
        {
            "var": QUEST,
            "topics": [{"topic": "TOPIC_RUEBEN", "title": "Убрать поле репы"}],
            "running": ["DIA_LOBART_WORKNOW_OK"],
            "success": ["DIA_LOBART_RUEBENRUNNING"],
            "failed": [],
            "obsolete": [],
            "other": [],
        }
    ],
}

SYMBOLS = {
    LOBART: "npc",
    HILDA: "npc",
    VINO: "npc",
    XARDAS: "npc",
    "ITPL_BEET": "item",
    "ITMI_GOLD": "item",
    QUEST: "var",
    "KAPITEL": "var",
    "MIS_OTHER": "var",
    "NPC_HASITEMS": "func",
    "NPC_KNOWSINFO": "func",
}
NAMES = {LOBART: "Лобарт", HILDA: "Хильда", VINO: "Вино", XARDAS: "Ксардас"}


@pytest.fixture
def graph(tmp_path: Path) -> StoryGraph:
    path = tmp_path / "story.json"
    path.write_text(json.dumps(GRAPH, ensure_ascii=False), encoding="utf-8")
    return load(path)


def _state(dead: set[str] = frozenset(), running: bool = True) -> WorldState:
    return WorldState(
        vars={QUEST: 1 if running else 0, "KAPITEL": 1},
        npcs={n: "dead" if n in dead else "alive" for n in NAMES},
        names=dict(NAMES),
        symbols=dict(SYMBOLS),
    )


def _repair_node(npc: str = HILDA, **change: Any) -> dict[str, Any]:
    node = {
        "id": "DIR_MIS_LOBART_RUEBEN_1",
        "npc": npc,
        "description": "Лобарт мёртв. Я принёс репу.",
        "condition": ["&&", RUNNING, HAS_20],
        "lines": [{"speaker": "other", "text": "Я принёс репу."}, {"speaker": "self", "text": "Давай сюда."}],
        "effects": [
            {"do": "give", "item": "ITPL_BEET", "count": 20, "to": "self"},
            {"do": "give", "item": "ITMI_GOLD", "count": 5},
            {"do": "xp", "amount": 50},
            {"do": "set", "var": QUEST, "value": 2},
        ],
    }
    return {"op": "add_node", "node": {**node, **change}}


CAPS = Caps(xp=50, items={"ITMI_GOLD": 5})


def test_a_dead_giver_leaves_the_quest_without_an_ending(graph):
    assert Story(graph, _state()).dead_ends() == []
    assert Story(graph, _state(running=False)).dead_ends() == []  # not taken: nothing to end
    story = Story(graph, _state(dead={LOBART}))
    assert story.dead_ends() == [QUEST]
    assert story.endings(QUEST) == ["DIA_LOBART_RUEBENRUNNING"]


def test_the_validator_takes_a_repair_that_gives_the_quest_an_ending(graph):
    story = Story(graph, _state(dead={LOBART}))
    rules = Rules.of(graph)
    ops = [_repair_node()]
    assert validate(ops, story, rules, repairs=QUEST, caps=CAPS) == []
    assert story.then(ops).dead_ends() == []


@pytest.mark.parametrize(
    ("op", "why"),
    [
        (_repair_node(LOBART), "мёртв"),
        (_repair_node(XARDAS), "сюжетный NPC"),
        (_repair_node("NOBODY"), "нет такого NPC"),
        (_repair_node(effects=[{"do": "set", "var": "KAPITEL", "value": 2}]), "главы"),
        (
            _repair_node(effects=[{"do": "give", "item": "ITMI_GOLD", "count": 500}]),
            "исходная концовка давала ×5",
        ),
        (_repair_node(effects=[{"do": "xp", "amount": 1000}]), "опыта 1000"),
        (_repair_node(effects=[{"do": "attack"}]), "недоступен"),
        (_repair_node(condition=["<", ["var", "KAPITEL"], ["int", 1]]), "никогда не будет предложен"),
        (_repair_node(condition=["==", ["var", "MIS_NOPE"], ["int", 1]]), "неизвестные имена"),
        (_repair_node(id="MY_NODE"), "начинается с DIR_"),
        (
            _repair_node(effects=[{"do": "set", "var": "MIS_OTHER", "value": 1}]),
            "так и остаётся без концовки",
        ),
        ({"op": "disable", "node": "DIA_XARDAS_FIRSTEXIT"}, "меняет главу"),
        ({"op": "told", "node": "DIA_LOBART_WORKNOW"}, "недоступна"),
    ],
)
def test_the_validator_says_why_not(graph, op, why):
    story = Story(graph, _state(dead={LOBART}))
    repairs = QUEST if op.get("op") == "add_node" else None
    errors = validate([op], story, Rules.of(graph), repairs=repairs, caps=CAPS)
    assert any(why in e for e in errors), errors


def test_a_change_may_not_leave_another_quest_without_an_ending(graph):
    story = Story(graph, _state())
    errors = validate([{"op": "disable", "node": "DIA_LOBART_RUEBENRUNNING"}], story, Rules.of(graph))
    assert errors == [f"после правки квест {QUEST} остаётся без концовки"]
    # a condition it can never meet does the same
    never = {
        "op": "condition",
        "node": "DIA_LOBART_RUEBENRUNNING",
        "expr": ["<", ["var", "KAPITEL"], ["int", 1]],
    }
    assert validate([never], story, Rules.of(graph)) == [f"после правки квест {QUEST} остаётся без концовки"]


def test_a_repair_ends_when_its_npc_dies_too(graph):
    patches = Patches.of([_repair_node()])
    assert Story(graph, _state(dead={LOBART}), patches).dead_ends() == []
    assert Story(graph, _state(dead={LOBART, HILDA}), patches).dead_ends() == [QUEST]
    told = patches.then([{"op": "told", "node": "DIR_MIS_LOBART_RUEBEN_1"}])
    assert Story(graph, _state(dead={LOBART}), told).dead_ends() == [QUEST]  # used up while still running


def _proposal(npc: str, **change: Any) -> ToolCall:
    args = {
        "kind": "handover",
        "ending": "DIA_LOBART_RUEBENRUNNING",
        "npc": npc,
        "description": "Лобарт мёртв. Я принёс репу.",
        "lines": [
            {"speaker": "hero", "text": "Я принёс репу."},
            {"speaker": "npc", "text": "Давай. Он бы заплатил."},
        ],
        "effects": ["E1", "E2", "E4"],
        "answers": [{"effect": "E2", "text": "Держи, он бы так и сделал."}],
        "status": "obsolete",
        "journal_now": "Лобарт мёртв. Может, репу примет Хильда.",
        "journal_done": "Я отдал репу Хильде.",
        "reason": "жена ведёт хозяйство",
    }
    return ToolCall("propose_repair", {**args, **change}, "c1")


def _out(speaker: str, line: str) -> dict[str, Any]:
    listener = "SELF" if speaker == "OTHER" else "OTHER"
    return {"do": "call", "call": ["call", "AI_OUTPUT", ["inst", speaker], ["inst", listener], ["str", line]]}


GIVE_5 = ["fn", "B_GIVEINVITEMS", ["inst", "SELF"], ["inst", "OTHER"], ["sym", "ITMI_GOLD"], ["int", 5]]
# Lobart's choice "Дай мне 5 золотых монет!" as story_program gives it
GOLD_CHOICE = [
    _out("OTHER", "DIA_Lobart_RuebenRunning_Gold_15_00"),
    _out("SELF", "DIA_Lobart_RuebenRunning_Gold_05_01"),
    {"do": "call", "call": GIVE_5},
    {
        "do": "if",
        "cond": ["==", ["field", ["inst", "HERO"], "GUILD"], ["int", 0, "GIL_NONE"]],
        "then": [_out("SELF", "DIA_Lobart_RuebenRunning_Gold_05_02")],
    },
    {"do": "call", "call": ["call", "INFO_CLEARCHOICES", ["sym", "DIA_LOBART_RUEBENRUNNING"]]},
    {"do": "return"},
]


async def _director(fake: FakeGame, llm: FakeBackend, graph: StoryGraph, store: MemoryStore):
    fake.vars = {QUEST: 1, "KAPITEL": 1}
    fake.npc_states = {n: {"state": "alive", "name": name} for n, name in NAMES.items()}
    fake.npc_states[LOBART]["state"] = "dead"
    fake.symbols = {**SYMBOLS, "AI_OUTPUT": "func", "B_GIVEINVITEMS": "func"}
    fake.programs = {("DIA_LOBART_RUEBENRUNNING_GOLD", "information"): GOLD_CHOICE}
    fake.people_near = [{"npc": VINO, "name": "Вино", "distance": 1200, "guild": 0}]
    link_cm = connected(fake)
    link = await link_cm.__aenter__()
    patches = StoryPatches(link, store)
    chronicle = Chronicle(link, store)
    director = Director(link, graph, patches, chronicle, llm, pacing=Pacing(cooldown_s=0), settle_s=0)
    await _until(lambda: patches.branch is not None and chronicle.branch is not None)
    return link_cm, director, chronicle


async def test_the_director_repairs_after_a_rejected_proposal(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(script=[FakeStep("", [_proposal(LOBART)]), FakeStep("", [_proposal(HILDA)])], delay=0)
    link_cm, director, chronicle = await _director(fake, llm, graph, store)
    try:
        assert await director.look([LOBART]) == [QUEST]

        brief = llm.calls[0][1][0].content
        assert "Лобарт (BAU_950_LOBART) мёртв" in brief and "E1: опыт +50" in brief
        # a choice with what the dead said to it, so that the model sees what it promised
        assert (
            "E2: вариант ответа «Дай мне 5 золотых монет!», в ответ: «Держи.» — герою: ITMI_GOLD ×5" in brief
        )
        cheaper = "E3: вариант ответа «Лучше продай одежду дешевле!», в ответ: «Хорошо! Я отдам тебе ее на 10"
        assert cheaper in brief and "дешевле.» — сам ничего не даёт" in brief
        assert "E4: забрать у героя ITPL_BEET ×20" in brief
        assert "Умерший сам снял пункты уговора" not in brief
        assert f"{HILDA} — Хильда: этот квест есть в диалогах" in brief
        assert f"{VINO} — Вино: был рядом (12 м)" in brief

        # Lobart is dead: rejected with the reason, which the model gets back
        rejected = chronicle.facts("repair_rejected")
        assert len(rejected) == 1 and any("мёртв" in why for why in rejected[0]["why"])
        assert "Отклонено" in llm.calls[1][1][-1].content and "мёртв" in llm.calls[1][1][-1].content
        assert llm.cache_keys == ["director", "director"]  # its own connection (openai_responses.py)

        # Hilda takes the ending over: a dialog of hers, from the lifted ending
        await _until(lambda: any(op["op"] == "add_node" for m in fake.graph for op in m["ops"]))
        node = next(op["node"] for m in fake.graph for op in m["ops"] if op["op"] == "add_node")
        assert (node["id"], node["npc"], node["nr"]) == ("DIR_MIS_LOBART_RUEBEN_1", HILDA, 1)
        assert node["condition"] == ["&&", RUNNING, KAPITEL_LT_3, HAS, HAS_20]
        assert node["lines"][0] == {"speaker": "other", "text": "Я принёс репу."}
        assert node["effects"] == [
            {"do": "xp", "amount": 50},
            {"do": "give", "item": "ITPL_BEET", "count": 20, "to": "self"},
            {"do": "set", "var": QUEST, "value": 2},
            {
                "do": "journal",
                "topic": "Убрать поле репы",
                "text": "Я отдал репу Хильде.",
                "status": "LOG_SUCCESS",
            },
        ]
        # the gold is the hero's choice, as Lobart offered it: his own line, Hilda's answer, her gold
        (choice,) = node["program"]
        assert (choice["do"], choice["text"]) == ("choice", "Дай мне 5 золотых монет!")
        assert choice["steps"] == [
            GOLD_CHOICE[0],
            {"do": "say", "speaker": "self", "text": "Держи, он бы так и сделал."},
            {"do": "call", "call": GIVE_5},
            {"do": "clear_choices"},
            {"do": "return"},
        ]
        await _until(lambda: any(m["type"] == "story_effect" for m in fake.story))
        effect = next(m for m in fake.story if m["type"] == "story_effect")
        assert (
            effect["npc"] == HILDA
            and effect["effects"][0]["text"] == "Лобарт мёртв. Может, репу примет Хильда."
        )
        repair = chronicle.facts("repair")[0]
        assert (repair["quest"], repair["npc"], repair["from"]) == (QUEST, HILDA, LOBART)
        # the patch belongs to the branch: a load brings it back
        assert [op["op"] for op in director.patches.ops()] == ["add_node"]
        # repaired: the next look finds nothing to do
        assert await director.look() == [] and len(llm.calls) == 2
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("transfer", [None, "give", "remove_items"])
async def test_repair_keeps_the_dead_givers_item_terms(graph, transfer):
    if transfer is not None:
        graph.nodes["DIA_LOBART_RUEBENRUNNING"].effects.append(
            {
                "do": transfer,
                "from": ["inst", "OTHER"],
                "to": ["inst", "SELF"],
                "npc": ["inst", "OTHER"],
                "item": ["sym", "ITPL_BEET"],
                "amount": ["int", 20],
            }
        )
    fake = FakeGame(branch="A")
    fake.item_names["ITPL_BEET"] = "Репа"
    llm = FakeBackend(script=[FakeStep("", [_proposal(HILDA, effects=["E1", "E2"])])], delay=0)
    link_cm, director, chronicle = await _director(fake, llm, graph, MemoryStore(":memory:"))
    try:
        chronicle.record(
            {
                "fact": "quest_terms",
                "quest": QUEST,
                "npc": LOBART,
                "name": "Лобарт",
                "drop": ["ITPL_BEET"],
                "why": "Оставь репу себе",
            }
        )
        assert await director.look([LOBART]) == [QUEST]
        assert len(director.repairs) == 1
        brief = llm.calls[0][1][0].content
        assert (
            "Умерший сам снял пункты уговора: Репа — «Оставь репу себе». Преемник держит этот уговор"
            in brief
        )
        assert "забрать у героя ITPL_BEET" not in brief
        node = director.repairs[0].ops[0]["node"]
        assert not any(e.get("to") == "self" for e in node["effects"])
        assert not any(e[:2] == ["call", "NPC_HASITEMS"] for e in walk(node["condition"]))
        assert node["effects"][0] == {"do": "xp", "amount": 50}
        # The gold choice must keep the waived item out of its guard as well.
        assert node["program"][0]["do"] == "choice"
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("waived", [False, True])
async def test_repair_keeps_cavalorns_spared_bandit_out_of_the_helper(graph, waived):
    cavalorn = "BAU_4300_ADDON_CAVALORN"
    bandits = ["BDT_1013_BANDIT_L", "BDT_1014_BRAGO", "BDT_1015_BANDIT_L"]
    helper = ["fn", "C_BRAGOBANDITSDEAD"]
    deaths = [["call", "NPC_ISDEAD", ["inst", f"AMBUSHER_{i}"]] for i in (1013, 1014, 1015)]
    graph.instances.update({f"AMBUSHER_{i}": b for i, b in zip((1013, 1014, 1015), bandits, strict=True)})
    graph.functions[helper[1]] = {"params": 0, "body": ["&&", *deaths], "lifted": True}
    node = graph.nodes["DIA_LOBART_RUEBENRUNNING"]
    node.npc = cavalorn
    node.condition = ["&&", RUNNING, helper]
    node.effects = [{"do": "set", "var": QUEST, "value": ["int", 2, "LOG_SUCCESS"]}]
    fake = FakeGame(branch="A")
    link_cm, director, chronicle = await _director(fake, FakeBackend(delay=0), graph, MemoryStore(":memory:"))
    fake.npc_states[cavalorn] = {"state": "dead", "name": "Кавалорн"}
    fake.npc_states[bandits[0]] = {"state": "alive", "name": "Бандит у дороги"}
    try:
        if waived:
            chronicle.record(
                {
                    "fact": "quest_terms",
                    "quest": QUEST,
                    "npc": cavalorn,
                    "name": "Кавалорн",
                    "drop": [bandits[0]],
                    "why": "Он просил пощады",
                }
            )
        brief = await director.brief(QUEST, await director.story())
        assert brief is not None
        ending = brief.endings[0]
        if waived:
            assert [e for e in walk(ending.condition) if e[:2] == ["call", "NPC_ISDEAD"]] == deaths[1:]
            assert helper not in list(walk(ending.condition))
            assert (
                "Умерший сам снял пункты уговора: Бандит у дороги — «Он просил пощады». "
                "Преемник держит этот уговор" in brief.text()
            )
        else:
            assert ending.condition == ["&&", RUNNING, helper]
            assert "Умерший сам снял пункты уговора" not in brief.text()
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_repair_accumulates_only_the_dead_givers_terms_for_this_quest(graph):
    dialog = "DIA_LOBART_WORKNOW"
    knows = ["call", "NPC_KNOWSINFO", ["inst", "OTHER"], ["sym", dialog]]
    node = graph.nodes["DIA_LOBART_RUEBENRUNNING"]
    node.condition = ["&&", node.condition, knows]
    fake = FakeGame(branch="A")
    link_cm, director, chronicle = await _director(fake, FakeBackend(delay=0), graph, MemoryStore(":memory:"))
    try:
        for quest, npc, dropped, why in [
            (QUEST, LOBART, ["ITPL_BEET"], "Репа не нужна"),
            (QUEST, LOBART, [dialog], "Мы уже договорились"),
            (QUEST, HILDA, ["KAPITEL"], "Чужой уговор"),
            ("MIS_OTHER", LOBART, ["KAPITEL"], "Другое дело"),
        ]:
            chronicle.record(
                {
                    "fact": "quest_terms",
                    "quest": quest,
                    "npc": npc,
                    "name": NAMES[npc],
                    "drop": dropped,
                    "why": why,
                }
            )
        brief = await director.brief(QUEST, await director.story())
        assert brief is not None
        condition = brief.endings[0].condition
        assert not any(
            e[:2] in (["call", "NPC_HASITEMS"], ["call", "NPC_KNOWSINFO"]) for e in walk(condition)
        )
        assert KAPITEL_LT_3 in list(walk(condition))
        text = brief.text()
        assert text.count("Умерший сам снял пункты уговора") == 2
        assert "Репа не нужна" in text and "DIA_LOBART_WORKNOW — «Мы уже договорились»" in text
        assert "Чужой уговор" not in text and "Другое дело" not in text
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_the_director_closes_a_quest_no_one_can_take_over(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    close = ToolCall(
        "propose_repair",
        {**_proposal("").arguments, "kind": "close", "ending": "", "status": "obsolete", "effects": []},
        "c1",
    )
    llm = FakeBackend(script=[FakeStep("", [close])], delay=0)
    link_cm, director, chronicle = await _director(fake, llm, graph, store)
    try:
        await director.look([LOBART])
        await _until(lambda: any(m["type"] == "story_effect" for m in fake.story))
        effect = next(m for m in fake.story if m["type"] == "story_effect")
        assert effect["npc"] == LOBART
        assert effect["effects"][0] == {"do": "set", "var": QUEST, "value": 4}
        assert effect["effects"][1]["status"] == "LOG_OBSOLETE"
        assert chronicle.facts("repair")[0]["kind"] == "close"
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_a_death_wakes_the_director_and_it_waits_out_a_dialog(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(script=[FakeStep("", [_proposal(HILDA)])], delay=0)
    link_cm, director, chronicle = await _director(fake, llm, graph, store)
    fake.npc_states[LOBART]["state"] = "alive"
    director.pacing.poll_s = 0.05
    run = asyncio.create_task(director.run())
    try:
        await _until(lambda: director._queue.empty())  # the look at the session's start: nothing broken
        await asyncio.sleep(0.2)
        fake.in_dialog = True
        fake.npc_states[LOBART]["state"] = "dead"
        await fake.send(type="npc_died", npc=LOBART, name="Лобарт", killer="PLAYER")
        await _until(lambda: chronicle.facts("died") != [])
        await asyncio.sleep(0.3)
        assert llm.calls == []  # a dialog runs: the director waits
        fake.in_dialog = False
        await _until(lambda: chronicle.facts("repair") != [], 5)
        assert "убил его герой" in llm.calls[0][1][0].content
    finally:
        run.cancel()
        await link_cm.__aexit__(None, None, None)


async def test_after_a_load_only_the_deaths_the_branch_saw_are_repaired(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(script=[FakeStep("", [_proposal(HILDA)])], delay=0)
    link_cm, director, chronicle = await _director(fake, llm, graph, store)
    run = asyncio.create_task(director.run())
    try:
        # Lobart died before the director watched: a load leaves it to `look` from the console
        await _until(lambda: director._queue.empty())
        await asyncio.sleep(0.2)
        assert llm.calls == []
        # the branch saw him die (a save after the death, before the repair): repaired after the load
        death = {"fact": "died", "npc": LOBART, "name": "Лобарт", "tag": None, "killer": ""}
        store.add_fact("A", json.dumps(death, ensure_ascii=False))
        director._queue.put_nowait(None)  # as a session does
        await _until(lambda: chronicle.facts("repair") != [], 5)
        assert len(llm.calls) == 1
    finally:
        run.cancel()
        await link_cm.__aexit__(None, None, None)


def test_a_choice_keeps_its_own_condition_but_what_the_dialog_asked():
    # Lobart offers the cheaper clothes to a hero of no guild; the 20 turnips the dialog itself asks for
    guild = ["==", ["field", ["inst", "HERO"], "GUILD"], ["int", 0, "GIL_NONE"]]
    assert _rest(["&&", HAS_20, guild], ["&&", RUNNING, HAS_20]) == guild
    assert _rest(HAS_20, ["&&", RUNNING, HAS_20]) is None
    assert _rest(["&&", guild, ["cases", guild, ["int", 1]]], None) is None  # not a program's expression


OWN_HELPER = "DIR_NPC_7"


class _QuestMemory:
    def __init__(self) -> None:
        self.extensions: list[Any] = []
        self.remembered: list[tuple[str, str, str]] = []

    def remember(self, npc: str, name: str, text: str) -> None:
        self.remembered.append((npc, name, text))


def _own_quest(*, middle_dialog: bool = False) -> Quest:
    goals = [Goal("have", npc=OWN_HELPER, item="ITPL_BEET", journal="Репа у меня.")]
    if middle_dialog:
        goals.insert(
            0,
            Goal(
                "talk",
                npc=HILDA,
                talk=Talk("Где искать репу?", [{"speaker": "self", "text": "У работника."}]),
            ),
        )
    return Quest(
        id="DIR_Q1",
        giver=HILDA,
        title="Репа для Хильды",
        give=Talk("Нужна помощь?", [{"speaker": "self", "text": "Принеси мне репу."}]),
        stages=[
            Stage("Хильда просит принести репу. Сначала надо дойти до Вино.", [Goal("reach", npc=VINO)]),
            Stage("Я дошёл до двора. Репа у работника.", goals),
            Stage(
                "Надо отнести репу Хильде.",
                [
                    Goal(
                        "bring",
                        npc=HILDA,
                        item="ITPL_BEET",
                        talk=Talk("Вот репа.", [{"speaker": "self", "text": "Держи плату за репу."}]),
                    )
                ],
            ),
        ],
        done_journal="Я отдал репу Хильде.",
        asked="принести репу.",
        gold=5,
        xp=40,
        names={**NAMES, OWN_HELPER: "Работник"},
    )


def _own_proposal(
    npc: str = LOBART, *, ending: str = "DIR_Q1_DONE", effects: list[str] | None = None
) -> ToolCall:
    return _proposal(
        npc,
        ending=ending,
        description="Хильда погибла. Я принёс репу.",
        effects=["E1", "E2"] if effects is None else effects,
        answers=[],
        journal_now="Хильда погибла. Её поручение принял Лобарт.",
        journal_done="Я отдал репу Лобарту.",
        reason="Лобарт ведёт это хозяйство",
    )


async def _own_director(
    fake: FakeGame, backend: FakeBackend, graph: StoryGraph, *, middle_dialog: bool = False
):
    fake.vars = {QUEST: 0, "KAPITEL": 1, "XP_AMBIENT": 50}
    fake.npc_states = {n: {"state": "alive", "name": name} for n, name in NAMES.items()}
    fake.npc_states[OWN_HELPER] = {"state": "alive", "name": "Работник"}
    fake.symbols = {
        **SYMBOLS,
        OWN_HELPER: "npc",
        **dict.fromkeys(("NPC_GETDISTTONPC", "HLP_GETNPC", "LOG_SETTOPICSTATUS", "B_LOGENTRY"), "func"),
    }
    fake.people_near = [
        {"npc": HILDA, "name": "Хильда", "distance": 0},
        {"npc": XARDAS, "name": "Ксардас", "distance": 100},
        {"npc": LOBART, "name": "Лобарт", "distance": 1200},
    ]
    link_cm = connected(fake)
    link = await link_cm.__aenter__()
    store = MemoryStore(":memory:")
    patches = StoryPatches(link, store)
    chronicle = Chronicle(link, store)
    director = Director(link, graph, patches, chronicle, backend, pacing=Pacing(cooldown_s=0), settle_s=0)
    memory = _QuestMemory()
    book = QuestBook(link, graph, patches, chronicle, director, dialogue=memory)
    director.book = book
    await _until(lambda: patches.branch is not None and chronicle.branch is not None)
    assert await book.make(_own_quest(middle_dialog=middle_dialog)) == []
    # The save has reached stage 2; quest events were missed while the server was away.
    fake.vars.update({"DIR_Q1": 1, "DIR_Q1_STAGE": 2, "DIR_Q1_S1G1": 1, "DIR_Q1_S1": 1})
    fake.npc_states[HILDA]["state"] = "dead"
    return link_cm, director, book, chronicle, memory


@pytest.mark.parametrize(("reward", "gold", "xp"), [("E1", 5, 0), ("E2", 0, 40)])
async def test_the_director_hands_over_a_book_quest_and_the_book_finishes_it(graph, reward, gold, xp):
    fake = FakeGame(branch="A")
    backend = FakeBackend(
        script=[FakeStep("", [_own_proposal(HILDA)]), FakeStep("", [_own_proposal(effects=[reward])])],
        delay=0,
    )
    link_cm, director, book, chronicle, memory = await _own_director(fake, backend, graph)
    try:
        original = Patches.of(director.patches.ops()).nodes["DIR_Q1_DONE"]
        await fake.send(type="npc_died", npc=HILDA, name="Хильда", killer="PLAYER")
        await _until(lambda: bool(chronicle.facts("died")))
        assert await director.look([HILDA]) == ["DIR_Q1"]

        brief = backend.calls[0][1][0].content
        assert "Репа для Хильды" in brief and "убил его герой" in brief
        assert "Хильда просит принести репу" in brief and "Я дошёл до двора" in brief
        assert "Стадия 2 из 3" in brief and "заполучить ITPL_BEET ×1" in brief
        assert "принести ITPL_BEET ×1" in brief and "Держи плату за репу." in brief
        assert "E1: золото герою: 5" in brief and "E2: опыт +40" in brief and "E3:" not in brief
        candidates = brief.split("Кандидаты в преемники:")[1]
        assert f"{OWN_HELPER} — Работник" in candidates  # the director's people can inherit too
        assert f"{LOBART} — Лобарт: был рядом (12 м)" in candidates
        assert HILDA not in candidates and XARDAS not in candidates
        assert len(chronicle.facts("repair_rejected")) == 1
        assert "Отклонено" in backend.calls[1][1][-1].content

        node = Patches.of(director.patches.ops()).nodes["DIR_Q1_DONE"]
        assert node["id"] == original["id"] and node["npc"] == LOBART
        assert node["condition"] == original["condition"]
        assert node["lines"] == [
            {"speaker": "other", "text": "Я принёс репу."},
            {"speaker": "self", "text": "Давай. Он бы заплатил."},
        ]
        for required in (
            {"do": "give", "item": "ITPL_BEET", "count": 1, "to": "self"},
            {"do": "set", "var": "DIR_Q1", "value": 2},
            {"do": "set", "var": "DIR_Q1_STAGE", "value": 4},
            {
                "do": "journal",
                "topic": "Репа для Хильды",
                "text": "Я отдал репу Лобарту.",
                "status": "LOG_SUCCESS",
            },
        ):
            assert required in node["effects"]
        assert sum(e.get("count", 0) for e in node["effects"] if e.get("item") == "ITMI_GOLD") == gold
        assert sum(e.get("amount", 0) for e in node["effects"] if e["do"] == "xp") == xp
        q = book.all()["DIR_Q1"]
        assert (q.gold, q.xp, q.turn_in().npc) == (gold, xp, LOBART)
        assert q.turn_in().talk.description == node["description"]
        assert q.repairs == [
            {"goal": "DIR_Q1_S3G1", "dead": HILDA, "npc": LOBART, "reason": "Лобарт ведёт это хозяйство"}
        ]
        assert len(director.patches.records("quest")) == 2
        repair = chronicle.facts("repair")[0]
        assert (repair["quest"], repair["from"], repair["npc"], repair["node"]) == (
            "DIR_Q1",
            HILDA,
            LOBART,
            "DIR_Q1_DONE",
        )
        assert memory.remembered[-1][0] == LOBART and "Ты принял" in memory.remembered[-1][2]
        assert "Ты принял поручение умершего (Хильда)" in book.situation(LOBART)

        # The same stage triggers and turn-in id keep driving the repaired quest.
        fake.vars["DIR_Q1_STAGE"] = 3
        await fake.send(type="trigger_fired", id="DIR_Q1_S2", count=1, npc="PC_HERO", other="PC_HERO")
        await _until(lambda: book.progress("DIR_Q1").stage == 3)
        fake.vars.update({"DIR_Q1": 2, "DIR_Q1_STAGE": 4})
        await fake.send(type="dialog_chosen", npc=LOBART, node="DIR_Q1_DONE", made=True)
        await _until(lambda: book.progress("DIR_Q1").state == "done")
        assert (
            book.situation(LOBART)
            == f"Поручение «Репа для Хильды» герой выполнил, ты заплатил: {q.reward()}."
        )
        assert memory.remembered[-1][:2] == (LOBART, "Лобарт")
        assert memory.remembered[-1][2].startswith("Герой пришёл к тебе с поручением «Репа для Хильды»")
        assert all(
            not fake.evaluate(n["condition"], n["npc"])
            for n in Patches.of(director.patches.ops()).nodes.values()
            if n["id"].startswith("DIR_Q1_")
        )
        assert await director.look() == [] and len(backend.calls) == 2
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_the_director_repairs_each_dead_dialog_of_a_book_quest_in_order(graph):
    fake = FakeGame(branch="A")
    backend = FakeBackend(
        script=[
            FakeStep("", [_own_proposal(ending="DIR_Q1_S2G1_D", effects=[])]),
            FakeStep("", [_own_proposal()]),
        ],
        delay=0,
    )
    link_cm, director, book, chronicle, _ = await _own_director(fake, backend, graph, middle_dialog=True)
    try:
        original = Patches.of(director.patches.ops()).nodes["DIR_Q1_S2G1_D"]
        assert await director.look([HILDA]) == ["DIR_Q1"]
        assert [r.node for r in director.repairs] == ["DIR_Q1_S2G1_D", "DIR_Q1_DONE"]
        assert [f["endings"] for f in chronicle.facts("dead_end")] == [["DIR_Q1_S2G1_D"], ["DIR_Q1_DONE"]]
        q = book.all()["DIR_Q1"]
        assert [r["goal"] for r in q.repairs] == ["DIR_Q1_S2G1", "DIR_Q1_S3G1"]
        assert q.stages[1].goals[0].npc == LOBART and q.turn_in().npc == LOBART
        node = Patches.of(director.patches.ops()).nodes["DIR_Q1_S2G1_D"]
        assert node["condition"] == original["condition"]
        assert {"do": "set", "var": "DIR_Q1_S2G1", "value": 1} in node["effects"]
        assert fake.vars["DIR_Q1_S2G1"] == 0 and book.progress("DIR_Q1").stage == 2
        assert await director.look([HILDA]) == [] and len(backend.calls) == 2
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_loading_repairs_only_recorded_deaths_in_the_book_and_does_not_repeat(graph):
    fake = FakeGame(branch="A")
    backend = FakeBackend(script=[FakeStep("", [_own_proposal()])], delay=0)
    link_cm, director, book, chronicle, _ = await _own_director(fake, backend, graph)
    try:
        await director._look_after_load()
        assert backend.calls == []
        chronicle.record({"fact": "died", "npc": HILDA, "name": "Хильда", "tag": None, "killer": "PLAYER"})
        await director._look_after_load()
        assert len(backend.calls) == 1 and book.all()["DIR_Q1"].turn_in().npc == LOBART
        # Loading replays both records of the same id in their original order.
        graph_count = len(fake.graph)
        await fake.send(type="session", branch="A", reason="load")
        await _until(lambda: len(fake.graph) > graph_count and fake.graph[-1].get("replace") is True)
        replayed = Patches.of(fake.graph[-1]["ops"])
        assert replayed.nodes["DIR_Q1_DONE"]["npc"] == LOBART
        assert book.all()["DIR_Q1"].turn_in().npc == LOBART
        await director._look_after_load()
        assert len(backend.calls) == 1 and len(chronicle.facts("repair")) == 1
    finally:
        await link_cm.__aexit__(None, None, None)


def test_only_a_repair_of_its_own_book_quest_may_replace_an_existing_dialog(graph):
    done = _repair_node(HILDA, id="DIR_Q1_DONE", condition=None, effects=[])
    give = _repair_node(HILDA, id="DIR_Q1_GIVE", condition=None, effects=[])
    story = Story(
        graph, _state(running=False), Patches.of([{"op": "vars", "vars": {"DIR_Q1": 1}}, done, give])
    )
    replacement = _repair_node(VINO, id="DIR_Q1_DONE", condition=None, effects=[])
    assert validate([replacement], story, Rules.of(graph), repairs="DIR_Q1") == []
    for repairs in (None, "DIR_Q2"):
        errors = validate([replacement], story, Rules.of(graph), repairs=repairs)
        assert "диалог DIR_Q1_DONE уже есть" in errors
    errors = validate([give], story, Rules.of(graph), repairs="DIR_Q1")
    assert "диалог DIR_Q1_GIVE уже есть" in errors
