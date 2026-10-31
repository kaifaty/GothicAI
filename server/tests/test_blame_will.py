from __future__ import annotations

import asyncio
from types import SimpleNamespace
from typing import Any

import pytest

from gothic_ai.blame import Blame
from gothic_ai.protocol import Session
from gothic_ai.social import Social
from gothic_ai.trust import Trust
from tests import test_npc_will as base
from tests.test_crimes import HILDA, LOBART
from tests.test_crimes import deed as hero_deed
from tests.test_npc_crimes import NAMES, THIEF

will_system = base.system


class MemoryDialogue:
    def __init__(self, system: Any) -> None:
        self.system = system
        self.conversations: dict[str, Any] = {}

    def open(self, npc: str, name: str) -> Any:
        return self.conversations.setdefault(npc, SimpleNamespace(
            persona=SimpleNamespace(name=name), intent=SimpleNamespace(attitude="neutral"),
        ))

    def remember(self, npc: str, name: str, text: str) -> None:
        self.open(npc, name)
        self.system.memory.append(self.system.chronicle.branch, npc, "system", text)

    def change_intent(self, npc: str, name: str, **fields: Any) -> None:
        conv = self.open(npc, name)
        for key, value in fields.items():
            setattr(conv.intent, key, value)


@pytest.fixture
async def system(will_system):
    dialogue = MemoryDialogue(will_system)
    blame = Blame(
        will_system.link, will_system.chronicle, will_system.crimes, will_system.deeds, dialogue,
    )
    will_system.blame, will_system.dialogue = blame, dialogue
    will_system.will.blame = blame
    yield will_system
    await blame.close()


def believe(system, crime, *, culprit=HILDA, because="hero_words"):
    fact = {
        "fact": "blamed", "id": f"BLAME_{len(system.chronicle.facts('blamed')) + 1}",
        "npc": LOBART, "name": NAMES[LOBART], "culprit": culprit,
        "culprit_name": NAMES[culprit], "crime": crime["id"], "because": because,
        "truth": crime["fact"] == "npc_crime" and crime["culprit"] == culprit,
        "title": "украл(а) ITFO_BREAD", "day": 3,
    }
    if because == "hero_words":
        fact["said"] = f"Это {NAMES[culprit]} взял хлеб."
    system.chronicle.record(fact)
    return fact


async def settle(system):
    # A fight result schedules Blame, which can in turn wake NpcWill. Drain those real callbacks.
    for _ in range(10):
        tasks = [task for owner in (system.will, system.deeds, system.blame)
                 for task in owner._tasks if not task.done()]
        if not tasks:
            return
        await asyncio.gather(*tasks)
    pytest.fail("crime and belief callbacks did not settle")


def far_scene(system, *, target=HILDA):
    base.scene(system, target=target, distance=10000)
    social = Social([
        {"npc": npc, "name": NAMES[npc], "guild": 12, "routine": [
            {"start": 0, "end": 0, "state": "ZS_STAND", "waypoint": "FARM1_IN",
             "pos": [10000, 0, 0]},
        ]}
        for npc in (LOBART, target)
    ])
    system.will.rumors = SimpleNamespace(social=social, clock=base.NOW)
    for npc, snapshot in system.link.snapshots.items():
        system.link.snapshots[npc] = snapshot.model_copy(update={
            "npc": snapshot.npc.model_copy(update={"pos": (10000, 0, 0)}),
        })


@pytest.mark.parametrize("courage,action", [(1, "say_svm"), (2, "say_svm"), (3, "fight"), (5, "fight")])
async def test_belief_launches_reclaim_without_original_knowledge_using_character_courage(
    system, courage, action,
):
    crime = base.theft(system)
    belief = believe(system, crime)
    system.cards[LOBART] = base.character(LOBART, courage=courage)
    base.scene(system, target=HILDA)
    await system.will.tick(base.NOW)
    await settle(system)
    [motive] = system.chronicle.facts("npc_motive")
    [attempt] = system.chronicle.facts("npc_will")
    assert (motive["kind"], motive["target"], motive["blamed"]) == ("reclaim", HILDA, belief["id"])
    assert (attempt["kind"], attempt["target"], attempt["act"]) == (
        "reclaim", HILDA, "fight" if courage >= 3 else "accuse",
    )
    assert [(npc, command) for npc, command, _ in system.link.actions] == [(LOBART, action)]
    assert system.link.actions[0][2]["npc"] == HILDA
    assert crime["culprit"] == THIEF and system.deeds.crime(crime["id"])["culprit"] == THIEF
    assert system.deeds.seen(LOBART) == []
    assert all(entry.kind != "npc_theft" for entry in system.people.of(LOBART))


async def test_hero_crime_can_launch_reclaim_against_accused_without_changing_truth(system):
    crime = system.crimes.record(hero_deed(owner=LOBART, chest="CHEST_LOBART", chest_owner=LOBART))
    assert crime is not None
    believe(system, crime, culprit=THIEF)
    base.scene(system)
    await system.will.tick(base.NOW)
    await settle(system)
    assert [(npc, command, args["npc"]) for npc, command, args in system.link.actions] == [
        (LOBART, "fight", THIEF),
    ]
    assert system.crimes.crime(crime["id"]) == crime
    assert "culprit" not in crime or crime["culprit"] == "PLAYER"


@pytest.mark.parametrize("because", ["hero_words", "testimony:TESTIMONY_1", "own", "hunch"])
async def test_no_goods_refutes_belief_and_only_hero_words_expose_a_lie(system, because):
    crime = base.theft(system)
    belief = believe(system, crime, because=because)
    assert ("said" in belief) is (because == "hero_words")
    system.link.values = [1, 0, 0, 0, 1, 0, 0]
    base.scene(system, target=HILDA)
    await system.will.tick(base.NOW)
    await settle(system)
    await system.will.tick(base.NOW + 60)
    await settle(system)
    [defense] = system.chronicle.facts("npc_crime_defense")
    [refuted] = system.chronicle.facts("blame_refuted")
    assert (defense["outcome"], defense["culprit"]) == ("no_goods", HILDA)
    assert (refuted["npc"], refuted["culprit"], refuted["crime"], refuted["how"]) == (
        LOBART, HILDA, crime["id"], "no_goods",
    )
    assert system.blame.active(LOBART, crime["id"]) is None and system.will.pending() == []
    assert len(system.link.actions) == 1 and system.link.effects == []
    memories = [entry.content for entry in system.memory.load("A", LOBART).entries]
    assert any("Хильда" in text and "вещи не оказалось" in text for text in memories)
    assert len(system.chronicle.facts("caught")) == int(because == "hero_words")
    assert bool(Trust(system.chronicle).of(LOBART).lied) is (because == "hero_words")
    if because == "hero_words":
        [caught] = system.chronicle.facts("caught")
        assert caught["title"] == belief["said"] and "со слов:" not in caught["title"]
        assert system.dialogue.open(LOBART, NAMES[LOBART]).intent.attitude == "wary"


@pytest.mark.parametrize("because", ["hero_words", "testimony:TESTIMONY_1", "hunch"])
async def test_true_accusation_returns_real_goods_and_credits_only_the_hero_claim(system, because):
    crime = base.theft(system)
    belief = believe(system, crime, culprit=THIEF, because=because)
    assert ("said" in belief) is (because == "hero_words")
    base.scene(system)
    await system.will.tick(base.NOW)
    await settle(system)
    await system.will.tick(base.NOW + 60)
    await settle(system)
    [restitution] = system.chronicle.facts("npc_crime_restitution")
    assert (restitution["crime"], restitution["culprit"], restitution["npc"], restitution["count"]) == (
        crime["id"], THIEF, LOBART, 1,
    )
    assert system.link.effects == [(THIEF, [
        {"do": "give", "item": "ITFO_BREAD", "count": 1, "to": "npc", "npc": LOBART},
    ], True)]
    assert system.will.pending() == [] and len(system.link.actions) == 1
    assert not system.chronicle.facts("blame_refuted") and not system.chronicle.facts("caught")
    assert len(system.chronicle.facts("claim")) == int(because == "hero_words")
    assert bool(Trust(system.chronicle).of(LOBART).kept) is (because == "hero_words")
    if because == "hero_words":
        [claim] = system.chronicle.facts("claim")
        assert claim["truth"] is True and claim["title"] == belief["said"]
        assert Trust(system.chronicle).of(LOBART).kept == [f"«{belief['said']}»"]


@pytest.mark.parametrize("completed", ["waiting", "reclaimed", "refuted"])
async def test_loading_keeps_one_belief_motive_and_does_not_repeat_a_finished_fight(system, completed):
    crime = base.theft(system)
    culprit = THIEF if completed == "reclaimed" else HILDA
    believe(system, crime, culprit=culprit)
    if completed == "refuted":
        system.link.values = [1, 0, 0, 0, 1, 0, 0]
    base.scene(system, target=culprit, distance=4000 if completed == "waiting" else 500)
    await system.will.tick(base.NOW)
    await settle(system)
    saved = system.memory.head("A")
    system.link.receive(Session(type="session", branch="loaded", parent="A", parent_seq=saved, reason="load"))
    base.scene(system, target=culprit, now=base.NOW + 60)
    await system.will.tick(base.NOW + 60)
    await settle(system)
    await system.will.tick(base.NOW + 120)
    await settle(system)
    assert len(system.chronicle.facts("blamed")) == 1
    assert len(system.chronicle.facts("npc_motive")) == 1
    assert len(system.chronicle.facts("npc_will")) == 1
    assert len(system.link.actions) == 1
    assert len(system.chronicle.facts("caught")) == int(completed == "refuted")
    assert len(system.chronicle.facts("claim")) == int(completed == "reclaimed")


@pytest.mark.parametrize("courage", [2, 3])
async def test_distant_empty_accused_refutes_belief_even_without_courage_to_fight(system, courage):
    crime = base.theft(system)
    believe(system, crime)
    system.cards[LOBART] = base.character(LOBART, courage=courage)
    far_scene(system)
    system.link.values = [0]
    await system.will.tick(base.NOW)
    await settle(system)
    [refuted] = system.chronicle.facts("blame_refuted")
    assert (refuted["crime"], refuted["culprit"], refuted["how"]) == (crime["id"], HILDA, "no_goods")
    assert not system.chronicle.facts("npc_crime_defense") and not system.link.effects
    assert system.link.actions == [] and system.will.pending() == []
    assert len(system.chronicle.facts("caught")) == 1
    assert (LOBART, [["call", "NPC_HASITEMS", ["inst", HILDA], ["sym", "ITFO_BREAD"]]]) in (
        system.link.evaluations
    )
