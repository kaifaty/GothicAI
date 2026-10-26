import copy
import json
from pathlib import Path
from typing import Any

import pytest

from gothic_ai.chronicle import Chronicle
from gothic_ai.consequences import LieConsequences, rewrite, strip_reward
from gothic_ai.director import Director, Pacing
from gothic_ai.gamedata.storygraph import StoryGraph, load
from gothic_ai.llm.fake import FakeBackend
from gothic_ai.memory import MemoryStore
from gothic_ai.story import StoryPatches
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import _until
from tests.test_director import GRAPH, HILDA, LOBART, NAMES, QUEST, SYMBOLS, _node
from tests.test_memory import close_memory_stores as close_memory_stores

CLOTHES, PRICE = "DIA_LOBART_BUYCLOTHES", "WERT_LOBARTSRUESTUNG"
ENDING, GOLD_CHOICE = "DIA_LOBART_RUEBENRUNNING", "DIA_LOBART_RUEBENRUNNING_GOLD"
DONE = ["==", ["var", QUEST], ["int", 2, "LOG_SUCCESS"]]


def _out(line: str, speaker: str = "SELF") -> dict[str, Any]:
    other = "OTHER" if speaker == "SELF" else "SELF"
    return {"do": "call", "call": ["call", "AI_OUTPUT", ["inst", speaker], ["inst", other], ["str", line]]}


def _gold(n: int, src: str = "SELF", dst: str = "OTHER") -> dict[str, Any]:
    give = ["fn", "B_GIVEINVITEMS", ["inst", src], ["inst", dst], ["sym", "ITMI_GOLD"], ["int", n]]
    return {"do": "call", "call": give}


# Lobart's work clothes as the scripts price them, cut down: 80, 10 off for the turnips
CLOTHES_INFO = [
    {"do": "set", "target": ["var", PRICE], "value": ["int", 80]},
    {
        "do": "if",
        "cond": DONE,
        "then": [
            _out("DIA_Lobart_BuyClothes_05_02"),
            {"do": "set", "target": ["var", PRICE], "value": ["-", ["var", PRICE], ["int", 10]]},
        ],
    },
    {"do": "return"},
]
GOLD_INFO = [_out("DIA_Lobart_RuebenRunning_Gold_05_01"), _gold(5), {"do": "return"}]
XP = {"do": "call", "call": ["fn", "B_GIVEPLAYERXP", ["int", 50, "XP_LOBARTHOLRUEBEN"]]}
PAYING = {
    "do": "call",
    "call": [
        "call",
        "INFO_ADDCHOICE",
        ["sym", "DIA_LOBART_RUEBENRUNNING"],
        ["str", "Дай мне 5 золотых!"],
        ["sym", GOLD_CHOICE],
    ],
}
ENDING_INFO = [
    {"do": "set", "target": ["var", QUEST], "value": ["int", 2, "LOG_SUCCESS"]},
    {"do": "if", "cond": ["==", ["field", ["inst", "HERO"], "GUILD"], ["int", 0]], "then": [XP]},
    _out("DIA_Lobart_RuebenRunning_05_04"),
    PAYING,
    {"do": "return"},
]


def test_the_quest_reads_as_running_and_the_pay_is_halved():
    made = rewrite(CLOTHES_INFO, {QUEST}, halve=False)
    assert made is not None
    steps, why = made
    assert why == ["quest"]
    assert steps[1]["cond"] == ["==", ["int", 1, "LOG_RUNNING"], ["int", 2, "LOG_SUCCESS"]]
    assert steps[0] == CLOTHES_INFO[0] and steps[1]["then"] == CLOTHES_INFO[1]["then"]  # only what it reads
    assert CLOTHES_INFO[1]["cond"] == DONE  # the scripts' program is left as it was

    assert rewrite(GOLD_INFO, {QUEST}, halve=False) is None  # nothing of the quest, no halving: no change
    steps, why = rewrite(GOLD_INFO, {QUEST}, halve=True) or ([], [])
    assert why == ["pay"] and steps[1] == _gold(2)
    assert rewrite([_gold(1)], set(), halve=True) == ([_gold(1)], ["pay"])  # at least one
    # the hero paying the npc is not its pay; a store into the quest is not a read of it
    assert rewrite([_gold(80, "OTHER", "SELF")], set(), halve=True) is None
    assert rewrite(ENDING_INFO, {QUEST}, halve=True) is None
    # choices of its own pay less too
    choice = {"do": "choice", "text": "Плата", "steps": [_gold(20)]}
    steps, why = rewrite([choice], set(), halve=True) or ([], [])
    assert steps[0]["steps"] == [_gold(10)]


def test_a_second_chance_pays_nothing_again():
    steps = strip_reward(ENDING_INFO, {GOLD_CHOICE})
    assert steps is not None
    # the quest ends and the lines are said as the scripts have it; no experience, no choice that pays
    assert steps == [ENDING_INFO[0], {**ENDING_INFO[1], "then": []}, ENDING_INFO[2], ENDING_INFO[4]]
    assert strip_reward([_gold(5), _gold(80, "OTHER", "SELF")], set()) == [_gold(80, "OTHER", "SELF")]
    assert strip_reward(GOLD_INFO[:1], set()) is None  # nothing paid: the scripts' own


@pytest.fixture
def graph(tmp_path: Path) -> StoryGraph:
    """Lobart's turnips and his work clothes."""
    g = copy.deepcopy(GRAPH)
    g["nodes"].append(
        _node(CLOTHES, LOBART, [], description="Сколько стоит эта рабочая одежда?", permanent=True)
    )
    path = tmp_path / "story.json"
    path.write_text(json.dumps(g, ensure_ascii=False), encoding="utf-8")
    return load(path)


def _ops(fake: FakeGame) -> list[dict[str, Any]]:
    return [op for m in fake.graph for op in m["ops"] if op.get("op") == "program"]


async def test_a_lie_found_out_changes_the_dialogs_of_the_one_lied_to(graph):
    fake = FakeGame(branch="A")
    fake.vars = {QUEST: 2, "KAPITEL": 1}  # the lie: the scripts have the quest done
    fake.npc_states = {n: {"state": "alive", "name": name} for n, name in NAMES.items()}
    fake.symbols = {**SYMBOLS, PRICE: "var", "AI_OUTPUT": "func", "B_GIVEINVITEMS": "func"}
    fake.programs = {
        (CLOTHES, "information"): CLOTHES_INFO,
        (CLOTHES, "condition"): [{"do": "return", "value": ["int", 1]}],
        (GOLD_CHOICE, "information"): GOLD_INFO,
        (ENDING, "information"): ENDING_INFO,  # the quest's own: as the lie left it
        ("DIA_HILDA_HALLO", "condition"): [{"do": "return", "value": ["!=", ["var", QUEST], ["int", 2]]}],
    }
    store = await MemoryStore.open(":memory:")
    async with connected(fake) as link:
        patches = StoryPatches(link, store)
        await patches.initialize()
        chronicle = Chronicle(link, store)
        await chronicle.initialize()
        llm = FakeBackend("", delay=0)
        director = Director(link, graph, patches, chronicle, llm, pacing=Pacing(cooldown_s=0), settle_s=0)
        lies = LieConsequences(link, graph, patches, chronicle, director)
        await _until(lambda: patches.branch is not None and chronicle.branch is not None)

        found = {
            "fact": "found_out",
            "quest": QUEST,
            "npc": LOBART,
            "name": "Лобарт",
            "title": "Убрать поле репы",
        }
        await chronicle.record({**found, "how": "наутро не досчитался репы", "day": 2})
        await _until(lambda: chronicle.facts("lie_consequences") != [])
        ops = {(op["node"], op["part"]): op["steps"] for op in _ops(fake)}
        # the clothes cost what they cost without the turnips, his gold for the turnips is halved
        assert set(ops) == {(CLOTHES, "information"), (GOLD_CHOICE, "information")}
        assert ops[(CLOTHES, "information")][1]["cond"][1] == ["int", 1, "LOG_RUNNING"]
        assert ops[(GOLD_CHOICE, "information")][1] == _gold(2)
        fact = chronicle.facts("lie_consequences")[-1]
        assert (fact["npc"], fact["quests"], fact["halved"]) == (LOBART, [QUEST], True)
        assert HILDA not in {graph.nodes[n].npc for n, _ in ops}  # only the one lied to

        # the hero makes amends: Lobart pays as before, and the turnips are his job again, for real; the
        # ending pays nothing a second time
        before = len(_ops(fake))
        await chronicle.record(
            {"fact": "amends", "npc": LOBART, "to": LOBART, "about": LOBART, "quest": QUEST, "gold": 5}
        )
        await _until(lambda: len(_ops(fake)) >= before + 2)
        assert {(op["node"], op["steps"] is None) for op in _ops(fake)[before:]} == {
            (GOLD_CHOICE, True),
            (ENDING, False),
        }
        ending = next(op["steps"] for op in _ops(fake)[before:] if op["node"] == ENDING)
        assert "B_GIVEPLAYERXP" not in str(ending) and PAYING not in ending and ending[0] == ENDING_INFO[0]
        reopen = next(m for m in fake.story if m.get("type") == "story_effect")
        assert reopen["npc"] == LOBART and reopen["effects"][0] == {"do": "set", "var": QUEST, "value": 1}
        assert (
            reopen["effects"][1]["topic"] == "Убрать поле репы"
            and reopen["effects"][1]["status"] == "LOG_RUNNING"
        )
        lie = lies.lies(LOBART)
        assert (lie.unbelieved, lie.halve, lie.second_chance) == ({QUEST}, False, {QUEST})
        # the branch has them: once more, nothing to change
        assert await lies.update(LOBART) == []

        # he brings the turnips for real: the lie is made good, Lobart's dialogs are the scripts' own again
        fake.vars[QUEST] = 2
        before = len(_ops(fake))
        await fake.send(type="dialog_chosen", npc=LOBART, node=ENDING, made=False)
        await _until(lambda: chronicle.facts("lie_made_good") != [])
        await _until(lambda: len(_ops(fake)) >= before + 2)
        assert {(op["node"], op["steps"]) for op in _ops(fake)[before:]} == {(CLOTHES, None), (ENDING, None)}
        assert lies.lies(LOBART).unbelieved == set()
