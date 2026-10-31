from __future__ import annotations

import asyncio
from types import SimpleNamespace

import pytest

from gothic_ai.amends import Amends
from gothic_ai.blame import BLAME_TOOL, Blame
from gothic_ai.chronicle import Chronicle
from gothic_ai.crime_meet import CrimeMeet
from gothic_ai.crimes import Crimes
from gothic_ai.llm import Message, ToolCall
from gothic_ai.memory import MemoryStore
from gothic_ai.npc_crimes import NpcCrimes
from gothic_ai.people_knowledge import PeopleKnowledge, source_event
from gothic_ai.protocol import Session
from gothic_ai.social import DAY, Rumors, Social
from gothic_ai.trust import Trust
from tests.test_crimes import HILDA, LOBART
from tests.test_crimes import deed as hero_deed
from tests.test_npc_crimes import NAMES, THIEF, NpcLink, deed

WORDS = "Это Вино взял кубок."


class Dialogue:
    def __init__(self):
        self.conversations = {}
        self.notes = []

    def open(self, npc, name):
        return self.conversations.setdefault(npc, SimpleNamespace(
            npc=npc, persona=SimpleNamespace(name=name), intent=SimpleNamespace(attitude="neutral"),
            memory=SimpleNamespace(entries=[]), trigger=Message("user", WORDS),
        ))

    def remember(self, npc, name, text):
        self.notes.append((npc, text))

    def change_intent(self, npc, name, **fields):
        conv = self.open(npc, name)
        for key, value in fields.items():
            setattr(conv.intent, key, value)


@pytest.fixture
async def system():
    link, memory, dialogue = NpcLink(), MemoryStore(":memory:"), Dialogue()
    chronicle = Chronicle(link, memory)
    crimes = Crimes(link, chronicle, names=lambda npc: NAMES.get(npc, npc))
    cards = {npc: SimpleNamespace(name=name) for npc, name in NAMES.items()}
    deeds = NpcCrimes(link, chronicle, crimes=crimes, cards=cards)
    blame = Blame(link, chronicle, crimes, deeds, dialogue)
    yield SimpleNamespace(
        link=link, memory=memory, dialogue=dialogue, chronicle=chronicle, crimes=crimes,
        deeds=deeds, blame=blame,
    )
    await blame.close()
    await deeds.close()
    await crimes.close()
    if chronicle._background:
        await asyncio.gather(*chronicle._background)
    memory.close()


def hero_case(s, *, heard=False, seen=False):
    crime = s.crimes.record(hero_deed(
        owner=LOBART, owner_npc_guild=8, chest="CHEST", item_name="Кубок",
        witnesses=[LOBART] if seen else [],
    ))
    if heard:
        s.chronicle.record({
            "fact": "rumor", "crime": crime["id"], "to": LOBART, "from": HILDA, "kind": "theft",
        })
    elif not seen:
        s.chronicle.record({"fact": "crime_suspected", "npc": LOBART, "crime": crime["id"]})
    return crime


def npc_case(s, *, seen=False):
    crime = s.deeds.record(deed(owner=LOBART, witnesses=[LOBART] if seen else [], chest="CHEST"))
    if not seen:
        s.chronicle.record({
            "fact": "npc_crime_suspected", "npc": LOBART, "crime": crime["id"], "suspect": HILDA,
        })
    return crime


async def accuse(s, cid, who="Вино"):
    conv = s.dialogue.open(LOBART, "Лобарт")
    return await s.blame.handle(s.dialogue, conv, ToolCall("blame", {"who": who, "crime": cid}, "b1"), None)


async def drain(s):
    while s.blame._tasks:
        await asyncio.gather(*tuple(s.blame._tasks))


@pytest.mark.parametrize("heard", [False, True])
async def test_hero_known_or_suspected_case_accepts_false_belief_without_showing_truth(system, heard):
    s = system
    crime = hero_case(s, heard=heard)
    result = await accuse(s, crime["id"])
    assert result.status == "accepted"
    [belief] = s.chronicle.facts("blamed")
    assert (belief["npc"], belief["culprit"], belief["crime"]) == (LOBART, THIEF, crime["id"])
    assert belief["because"] == "hero_words" and belief["said"] == WORDS and belief["truth"] is False
    assert "ты считаешь" in s.blame.situation(LOBART)
    assert "truth" not in result.output + s.blame.situation(LOBART)
    assert s.blame.crime_for(belief)["culprit"] == THIEF
    assert s.crimes.crime(crime["id"]).get("culprit") is None
    assert (await accuse(s, crime["id"])).status == "accepted"
    assert len(s.chronicle.facts("blamed")) == 1


async def test_npc_case_accepts_actual_or_false_culprit_and_keeps_truth_in_code(system):
    s = system
    crime = npc_case(s)
    assert (await accuse(s, crime["id"])).status == "accepted"
    assert s.chronicle.facts("blamed")[-1]["truth"] is True
    assert (await accuse(s, crime["id"], "Хильда")).status == "accepted"
    assert s.blame.active(LOBART, crime["id"])["truth"] is False
    assert s.deeds.crime(crime["id"])["culprit"] == THIEF


async def test_hunch_without_hero_words_is_refuted_without_catching_the_hero(system):
    s = system
    crime = hero_case(s)
    conv = s.dialogue.open(LOBART, "Лобарт")
    conv.trigger = None
    conv.memory.entries = [Message("assistant", "Наверное, это Вино взял кубок.")]
    assert (await accuse(s, crime["id"])).status == "accepted"
    belief = s.blame.active(LOBART, crime["id"])
    assert belief["because"] == "hunch" and "said" not in belief
    s.chronicle.record({
        "fact": "npc_crime_defense", "crime": crime["id"], "culprit": THIEF,
        "blamed": belief["id"], "fighter": LOBART, "outcome": "no_goods",
    })
    await drain(s)
    assert s.blame.active(LOBART, crime["id"]) is None
    assert len(s.chronicle.facts("blame_refuted")) == 1
    assert not s.chronicle.facts("caught") and not s.chronicle.facts("claim")
    assert not Trust(s.chronicle).of(LOBART).lied


@pytest.mark.parametrize("current", [False, True])
async def test_declined_name_selects_the_exact_recent_hero_line(system, current):
    s = system
    crime = hero_case(s)
    s.deeds.cards[THIEF].name = "Малак"
    s.crimes.names = lambda npc: "Малак" if npc == THIEF else NAMES.get(npc, npc)
    conv = s.dialogue.open(LOBART, "Лобарт")
    said = "Это Малака работа."
    conv.memory.entries = [Message("user", "Малак был у дороги.")]
    if not current:
        conv.memory.entries.extend([Message("user", said), Message("user", "Хорошая погода.")])
    conv.trigger = Message("user", said if current else "Привет.")
    assert (await accuse(s, crime["id"], "Малак")).status == "accepted"
    belief = s.blame.active(LOBART, crime["id"])
    assert belief["because"] == "hero_words" and belief["said"] == said
    await s.blame.refute(belief)
    assert s.chronicle.facts("caught")[0]["title"] == said


@pytest.mark.parametrize("words", ["Привет.", "Кто-то взял кубок.", "Я виноват."])
async def test_hero_words_without_the_accused_name_are_a_hunch(system, words):
    s = system
    crime = hero_case(s)
    conv = s.dialogue.open(LOBART, "Лобарт")
    conv.trigger = Message("user", words)
    conv.memory.entries = [Message("assistant", WORDS), Message("system", WORDS)]
    assert (await accuse(s, crime["id"])).status == "accepted"
    belief = s.blame.active(LOBART, crime["id"])
    assert belief["because"] == "hunch" and "said" not in belief
    await s.blame.refute(belief)
    assert not s.chronicle.facts("caught") and not s.chronicle.facts("claim")


async def test_old_hero_words_outside_recent_lines_do_not_turn_a_hunch_into_a_claim(system):
    s = system
    crime = hero_case(s)
    conv = s.dialogue.open(LOBART, "Лобарт")
    conv.trigger = Message("user", "Привет.")
    conv.memory.entries = [Message("user", WORDS), *[Message("user", "Погода.") for _ in range(8)]]
    assert (await accuse(s, crime["id"])).status == "accepted"
    belief = s.blame.active(LOBART, crime["id"])
    assert belief["because"] == "hunch" and "said" not in belief


@pytest.mark.parametrize("kind", ["hero", "npc"])
async def test_eyewitness_victim_cannot_blame_anyone(system, kind):
    crime = hero_case(system, seen=True) if kind == "hero" else npc_case(system, seen=True)
    result = await accuse(system, crime["id"])
    assert result.status == "rejected" and "сам видел" in result.output
    assert system.chronicle.facts("blamed") == []


@pytest.mark.parametrize("who", ["PLAYER", "PC_HERO", "Герой", "Лобарт", "несуществующий"])
async def test_blame_requires_another_person(system, who):
    crime = hero_case(system)
    assert (await accuse(system, crime["id"], who)).status == "rejected"
    assert not system.blame.beliefs()


async def test_unknown_or_someone_elses_case_rejected(system):
    s = system
    crime = s.deeds.record(deed(owner=HILDA))
    assert (await accuse(s, crime["id"])).status == "rejected"
    crime = s.deeds.record(deed(id="unknown", owner=LOBART))
    assert (await accuse(s, crime["id"])).status == "rejected"
    assert (await accuse(s, "missing-id")).status == "rejected"


async def test_testimony_is_belief_source_and_does_not_prove_hero_crime(system):
    s = system
    crime = s.deeds.record(deed(owner=LOBART))
    testimony = {
        "fact": "testimony", "id": "T1", "npc": LOBART, "witness": HILDA, "about": THIEF,
        "crime_told": [crime["id"]],
    }
    s.chronicle.record(testimony)
    assert s.crimes.testimony_crimes(testimony) == []
    assert (await accuse(s, crime["id"])).status == "accepted"
    belief = s.blame.active(LOBART, crime["id"])
    assert belief["because"] == "testimony:T1" and "said" not in belief and belief["truth"]
    await s.blame.refute(belief)
    assert not s.chronicle.facts("caught")


async def test_own_trace_takes_priority_over_hero_words(system):
    from gothic_ai.sightings import Sighting

    s = system
    crime = hero_case(s)
    at = 3 * 1440 + 9 * 60 + 15
    sight = Sighting(THIEF, "Вино", at, at, place_ids=["FARM"])
    s.crimes.sightings = SimpleNamespace(mind=lambda _: SimpleNamespace(sightings=[sight]))
    s.crimes.places = SimpleNamespace(of=lambda _: SimpleNamespace(id="FARM"))
    assert (await accuse(s, crime["id"])).status == "accepted"
    belief = s.blame.active(LOBART, crime["id"])
    assert belief["because"] == "own" and "said" not in belief
    await s.blame.refute(belief)
    assert not s.chronicle.facts("caught")


async def test_testimony_about_presence_is_a_belief_source_without_proving_the_deed(system):
    from gothic_ai.sightings import Sighting

    s = system
    crime = hero_case(s)
    at = 3 * 1440 + 9 * 60 + 15
    sight = Sighting(THIEF, "Вино", at, at, place_ids=["FARM"])
    s.crimes.places = SimpleNamespace(of=lambda _: SimpleNamespace(id="FARM"))
    s.chronicle.record({
        "fact": "testimony", "id": "T_presence", "npc": LOBART, "about": THIEF,
        "witness": HILDA, "told": [sight.record()], "crime_told": [],
    })
    assert (await accuse(s, crime["id"])).status == "accepted"
    belief = s.blame.active(LOBART, crime["id"])
    assert belief["because"] == "testimony:T_presence" and belief["truth"] is False
    await s.blame.refute(belief)
    assert not s.chronicle.facts("caught")


async def test_no_goods_refutes_false_words_and_uses_existing_caught_trust_rumor_path(system):
    s = system
    crime = hero_case(s)
    await accuse(s, crime["id"])
    belief = s.blame.active(LOBART, crime["id"])
    s.chronicle.record({
        "fact": "npc_crime_defense", "crime": crime["id"], "culprit": THIEF,
        "blamed": belief["id"], "fighter": LOBART, "outcome": "no_goods",
    })
    await drain(s)
    assert s.blame.active(LOBART, crime["id"]) is None
    [refuted] = s.chronicle.facts("blame_refuted")
    assert refuted["how"] == "no_goods"
    [caught] = s.chronicle.facts("caught")
    assert caught["said"] and caught["title"] == WORDS and "со слов" not in caught["how"]
    assert "crime" not in caught  # disproving a frame is not eyewitness proof of the hero's theft
    assert Trust(s.chronicle).of(LOBART).lied
    assert s.dialogue.open(LOBART, "Лобарт").intent.attitude == "wary"
    assert any("вещи не оказалось" in note for _, note in s.dialogue.notes)
    rumors = Rumors(s.link, s.dialogue, s.chronicle)
    assert any(r.kind == "caught" and r.is_player and r.title == WORDS for r in rumors.rumors.values())


async def test_reclaimed_true_hero_words_count_as_true_claim_once(system):
    s = system
    crime = npc_case(s)
    await accuse(s, crime["id"])
    belief = s.blame.active(LOBART, crime["id"])
    for kind in ["npc_crime_restitution", "npc_crime_defense"]:
        s.chronicle.record({
            "fact": kind, "crime": crime["id"], "culprit": THIEF, "npc": LOBART,
            "blamed": belief["id"], "outcome": "reclaimed",
        })
    await drain(s)
    assert len(s.chronicle.facts("claim")) == 1
    assert Trust(s.chronicle).of(LOBART).kept == [f"«{WORDS}»"]
    assert not s.chronicle.facts("caught")


async def test_true_belief_missing_goods_does_not_catch_a_true_statement(system):
    s = system
    crime = npc_case(s)
    await accuse(s, crime["id"])
    await s.blame.refute(s.blame.active(LOBART, crime["id"]))
    assert len(s.chronicle.facts("blame_refuted")) == 1
    assert not s.chronicle.facts("caught") and not s.chronicle.facts("claim")


async def test_belief_temporarily_suspends_hero_compensation(system):
    s = system
    crime = hero_case(s, heard=True)
    amends = Amends.__new__(Amends)
    amends.crimes, amends.chronicle = s.crimes, s.chronicle
    meet = CrimeMeet.__new__(CrimeMeet)
    meet.crimes, meet.chronicle = s.crimes, s.chronicle
    meet.law = SimpleNamespace(is_guard=lambda *_: False)
    assert amends.active_crime(LOBART, crime["id"]) and meet._cases(LOBART)
    await accuse(s, crime["id"])
    assert not amends.active_crime(LOBART, crime["id"]) and not meet._cases(LOBART)
    await s.blame.refute(s.blame.active(LOBART, crime["id"]))
    assert amends.active_crime(LOBART, crime["id"]) and meet._cases(LOBART)


async def test_source_and_rumor_describe_belief_without_visual_proof(system):
    s = system
    crime = hero_case(s)
    await accuse(s, crime["id"])
    belief = s.blame.active(LOBART, crime["id"])
    event = source_event(belief, s.chronicle.facts())
    assert event.kind == "blamed" and event.subject == THIEF
    assert "Лобарт считает, что Вино украл" in event.description()
    people = PeopleKnowledge(s.chronicle)
    own_context = people.context(LOBART)
    assert "ты считаешь, что Вино украл" in own_context and "видел" not in own_context
    assert "Лобарт считает, что" not in own_context
    assert "ты считаешь, что Вино украл" in s.blame.situation(LOBART)
    rumors = Rumors(s.link, s.dialogue, s.chronicle)
    rumor = next(r for r in rumors.rumors.values() if r.kind == "blamed")
    assert rumor.subject == THIEF and rumor.description() == event.description()
    rumors.social = Social([{"npc": npc, "name": NAMES[npc]} for npc in (LOBART, HILDA, THIEF)])
    rumors.social.tie(LOBART, HILDA).meets = [(0, DAY)]
    rumors.gate = lambda *_: True
    rumors.clock = 3 * DAY + 600
    assert rumors.spread(rumors.clock, rumors.clock + 1) == 1
    [learned] = s.chronicle.facts("rumor")
    assert (learned["from"], learned["to"], learned["subject"], learned["kind"]) == (
        LOBART, HILDA, THIEF, "blamed",
    )
    assert "Лобарт считает, что Вино" in people.context(HILDA)
    assert "ты считаешь, что Вино" not in people.context(HILDA)
    assert not Trust(s.chronicle).of(HILDA).heard


async def test_load_recovers_consequences_without_duplicate_facts(system):
    s = system
    crime = hero_case(s)
    await accuse(s, crime["id"])
    belief = s.blame.active(LOBART, crime["id"])
    s.chronicle.record({
        "fact": "blame_refuted", "blamed": belief["id"], "npc": LOBART,
        "culprit": THIEF, "crime": crime["id"], "how": "no_goods",
    })
    for _ in range(2):
        s.link.receive(Session(type="session", branch="A"))
        await drain(s)
    assert len(s.chronicle.facts("blamed")) == 1
    assert len(s.chronicle.facts("blame_refuted")) == len(s.chronicle.facts("caught")) == 1
    assert not s.blame.beliefs()
    s.link.receive(Session(type="session", branch="B"))
    await drain(s)
    assert s.chronicle.facts() == [] and s.blame.tools(LOBART) == []


async def test_switch_hides_tool_and_saved_belief_no_longer_suspends_hero_case(system):
    s = system
    crime = hero_case(s)
    assert s.blame.tools(LOBART) == [BLAME_TOOL]
    await accuse(s, crime["id"])
    s.deeds.enabled = False
    assert s.blame.tools(LOBART) == [] and s.blame.all_tools == ()
    assert s.blame.situation(LOBART) is None and not s.crimes.blamed(LOBART, crime["id"])
    assert (await accuse(s, crime["id"])).status == "rejected"


async def test_named_animal_cannot_be_blamed(system):
    s = system
    crime = hero_case(s)
    s.crimes.people[THIEF] = {"guild": 20}
    assert (await accuse(s, crime["id"])).status == "rejected"


async def test_legacy_defense_before_new_belief_does_not_refute_it_after_loading(system):
    s = system
    crime = hero_case(s)
    s.chronicle.record({
        "fact": "npc_crime_defense", "crime": crime["id"], "culprit": THIEF, "outcome": "no_goods",
    })
    await drain(s)
    await accuse(s, crime["id"])
    s.link.receive(Session(type="session", branch="A"))
    await drain(s)
    assert s.blame.active(LOBART, crime["id"]) is not None
    assert not s.chronicle.facts("blame_refuted")
