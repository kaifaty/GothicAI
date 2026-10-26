from __future__ import annotations

import asyncio
from types import SimpleNamespace
from typing import Any

import pytest

from gothic_ai.chronicle import Chronicle
from gothic_ai.concerns import Concerns
from gothic_ai.crime_memory import memory_value
from gothic_ai.crimes import Crimes
from gothic_ai.dialogue import DialogueManager
from gothic_ai.llm.fake import FakeBackend
from gothic_ai.memory import MemoryStore
from gothic_ai.persona import PersonaProvider
from gothic_ai.places import PlaceBook
from gothic_ai.protocol import Session
from gothic_ai.sightings import Sighting, Sightings
from gothic_ai.trust import Trust
from tests.test_crimes import HILDA, LOBART, Link, deed
from tests.test_memory import close_memory_stores as close_memory_stores

MALETH = "BAU_952_MALETH"
CHEST = "CHEST_LOBART"
AT = 3 * 1440 + 9 * 60 + 15
NAMES = {LOBART: "Лобарт", HILDA: "Хильда", MALETH: "Малет", "PLAYER": "герой"}
BOOK = PlaceBook(
    {
        "points": {"FARM1_IN": 0, "FARM1_OUT": 1},
        "places": [
            {"id": "LOBART_HOUSE", "ru": "дом Лобарта", "where_ru": "в доме Лобарта",
             "en": "Lobart's house", "where_en": "in Lobart's house"},
            {"id": "LOBART_YARD", "ru": "двор Лобарта", "where_ru": "во дворе Лобарта",
             "en": "Lobart's yard", "where_en": "in Lobart's yard"},
        ],
    }
)  # fmt: skip


@pytest.fixture
async def knowledge():
    link = Link()
    memory = await MemoryStore.open(":memory:")
    chronicle = Chronicle(link, memory)  # type: ignore[arg-type]
    await chronicle.initialize()
    crimes = Crimes(link, chronicle, places=BOOK, names=lambda npc: NAMES.get(npc, npc))  # type: ignore[arg-type]
    desk = SimpleNamespace(on_view=[])
    sightings = Sightings(link, memory, desk, chronicle, crimes=crimes)  # type: ignore[arg-type]
    crimes.sightings = sightings
    yield SimpleNamespace(
        link=link,
        memory=memory,
        chronicle=chronicle,
        crimes=crimes,
        sightings=sightings,
        trust=Trust(chronicle, crimes=crimes),
    )
    await crimes.close()
    if chronicle._background:
        await asyncio.gather(*chronicle._background)
    await memory.close()


@pytest.fixture
async def attitude_dialogue(knowledge):
    knowledge.link.snapshots = {}

    async def identity(npc, expressions):
        return [8, 0]

    knowledge.link.story_eval = identity
    dialogue = DialogueManager(knowledge.link, FakeBackend(), PersonaProvider(None), memory=knowledge.memory)
    await dialogue.initialize()
    knowledge.crimes.dialogue = dialogue
    knowledge.crimes.people[LOBART] = {"guild": 8, "npctype": 0}
    yield dialogue
    await dialogue.close()


async def theft(knowledge, **fields: Any):
    result = await knowledge.crimes.record(deed(owner=LOBART, owner_npc_guild=8, **fields))
    assert result is not None
    return result


async def learn(knowledge, npc: str, crime: str, degree: str, **fields: Any) -> None:
    if degree == "seen":
        fact = {"fact": "crime_seen", "npc": npc, "crime": crime}
    elif degree == "heard":
        fact = {"fact": "rumor", "to": npc, "from": HILDA, "crime": crime, "kind": "theft", "hops": 2}
    else:
        fact = {"fact": "crime_suspected", "npc": npc, "crime": crime, "suspect": "PLAYER"}
    await knowledge.chronicle.record({**fact, **fields})


async def presence(
    knowledge,
    who: str = "PLAYER",
    *,
    start: int = AT - 5,
    end: int = AT,
    place: str = "LOBART_HOUSE",
    **fields: Any,
) -> Sighting:
    sighting = Sighting(who, NAMES.get(who, who), start, end, place_ids=[place], **fields)
    await knowledge.sightings.prepare(LOBART)
    knowledge.sightings.mind(LOBART).sightings.append(sighting)
    return sighting


@pytest.mark.parametrize("degree", ["seen", "heard", "suspects"])
async def test_a_deed_is_known_only_with_this_npcs_evidence(knowledge, degree):
    crime = await theft(knowledge)
    # Being its owner does not identify the thief, even though the chronicle holds the truth.
    assert knowledge.crimes.known(LOBART) == []
    assert knowledge.crimes.situation(LOBART) is None
    await learn(knowledge, LOBART, crime["id"], degree)

    (known,) = knowledge.crimes.known(LOBART.lower())
    assert known.crime["id"] == crime["id"] and known.degree == degree
    assert known.suspect == "PLAYER"
    if degree == "heard":
        assert (known.source, known.hops) == (HILDA, 2)
    assert knowledge.crimes.known(MALETH) == []


async def test_direct_evidence_wins_over_later_rumor_and_suspicion(knowledge):
    crime = await theft(knowledge)
    await learn(knowledge, LOBART, crime["id"], "suspects")
    await learn(knowledge, LOBART, crime["id"], "heard")
    assert knowledge.crimes.known(LOBART)[0].degree == "heard"
    await learn(knowledge, LOBART, crime["id"], "seen")
    await learn(knowledge, LOBART, crime["id"], "heard", **{"from": MALETH})
    await learn(knowledge, LOBART, crime["id"], "suspects")
    (known,) = knowledge.crimes.known(LOBART)
    assert (known.degree, known.source, known.hops) == ("seen", LOBART, 0)


@pytest.mark.parametrize("justified_first", [False, True])
async def test_justification_wins_over_plain_hearsay_but_never_direct_evidence(knowledge, justified_first):
    crime = await knowledge.crimes.record(deed(kind="murder", victim=LOBART, victim_guild=8))
    excuse = {"excuse": "fought", "excuse_by": HILDA, "excuse_by_name": "Хильда"}
    versions = [excuse, {}] if justified_first else [{}, excuse]
    for fields in versions:
        await learn(knowledge, MALETH, crime["id"], "heard", **fields)
    (known,) = knowledge.crimes.known(MALETH)
    assert (known.degree, known.excuse, known.excuse_by, known.excuse_by_name) == (
        "heard", "fought", HILDA, "Хильда",
    )
    assert knowledge.crimes.excuse(MALETH, crime["id"]) == excuse
    assert "Хильда говорит: Лобарт дрался с ним" in knowledge.crimes.situation(MALETH)
    # Without CrimeReact only a recorded personal side suppresses consequences.
    assert not knowledge.crimes.does_not_blame(MALETH, crime)
    await learn(knowledge, MALETH, crime["id"], "seen")
    await learn(knowledge, MALETH, crime["id"], "heard", **excuse)
    (known,) = knowledge.crimes.known(MALETH)
    assert (known.degree, known.excuse, known.excuse_by) == ("seen", "", "")


@pytest.mark.parametrize("snapshot", [False, True])
async def test_ally_testimony_justification_is_knowledge_and_can_be_retold(knowledge, snapshot):
    knowledge.crimes.sides.note_fight(LOBART, HILDA)
    crime = await knowledge.crimes.record(
        deed(kind="murder", victim=LOBART, victim_guild=8, witnesses=[HILDA])
    )
    excuse = {"excuse": "fought", "excuse_by": HILDA, "excuse_by_name": "Хильда"}
    fields = {"crime_excuses": {crime["id"]: excuse}} if snapshot else {}
    await knowledge.chronicle.record({
        "fact": "testimony", "npc": MALETH, "witness": HILDA, "crime_told": [crime["id"]], **fields,
    })
    (known,) = knowledge.crimes.known(MALETH)
    assert (known.degree, known.excuse, known.excuse_by) == ("heard", "fought", HILDA)
    assert knowledge.crimes.excuse(MALETH, crime["id"]) == excuse
    # A later source does not replace the original source; a personal side does.
    await learn(knowledge, MALETH, crime["id"], "heard", excuse="quest", excuse_by=LOBART)
    assert knowledge.crimes.excuse(MALETH, crime["id"]) == excuse
    knowledge.crimes.sides.note_fight(LOBART, MALETH)
    await knowledge.crimes.learn_side(MALETH, crime)
    assert knowledge.crimes.excuse(MALETH, crime["id"])["excuse_by"] == MALETH


async def test_missing_crime_references_cannot_invent_knowledge(knowledge):
    for degree in ("seen", "heard", "suspects"):
        await learn(knowledge, LOBART, "not-in-this-branch", degree)
    assert knowledge.crimes.known(LOBART) == []
    assert knowledge.crimes.situation(LOBART) is None


async def test_witness_testimony_teaches_its_recipient_the_exact_deed(knowledge):
    crime = await theft(knowledge, witnesses=[HILDA])
    await knowledge.chronicle.record(
        {"fact": "testimony", "npc": LOBART, "witness": HILDA, "crime_told": [crime["id"]]}
    )
    (known,) = knowledge.crimes.known(LOBART)
    assert (known.degree, known.source, known.hops) == ("heard", HILDA, 1)
    assert knowledge.crimes.known(MALETH) == []


@pytest.mark.parametrize(
    "degree,word", [("seen", "видел"), ("heard", "со слов"), ("suspects", "подозреваешь")]
)
async def test_summary_names_the_deed_time_place_and_source(knowledge, degree, word):
    crime = await theft(knowledge)
    await learn(knowledge, LOBART, crime["id"], degree)
    await theft(knowledge, id="secret-deed", item="ITFO_APPLE", hour=22)
    summary = knowledge.crimes.situation(LOBART)
    assert summary is not None
    assert word in summary and "ITFO_BREAD" in summary
    assert "в доме Лобарта" in summary and "день 3, 09:15" in summary
    assert "ITFO_APPLE" not in summary and "22:" not in summary
    if degree == "heard":
        assert "Хильд" in summary and "2" in summary
    if degree == "suspects":
        assert "догадка" in summary


async def test_forgiven_and_silenced_are_visible_without_erasing_knowledge(knowledge):
    crime = await theft(knowledge, witnesses=[LOBART])
    await knowledge.chronicle.record({"fact": "forgiven", "npc": LOBART, "crime": crime["id"]})
    await knowledge.chronicle.record({"fact": "silenced", "npc": LOBART, "crime": crime["id"]})
    (known,) = knowledge.crimes.known(LOBART)
    assert known.degree == "seen" and known.forgiven and known.silenced
    summary = knowledge.crimes.situation(LOBART)
    assert summary is not None and "простил" in summary and "молчать" in summary
    assert knowledge.crimes.attitude(LOBART) is None
    assert not knowledge.trust.of(LOBART).broken


@pytest.mark.parametrize(
    "npc,degree,kind,community,expected",
    [
        (LOBART, "heard", "theft", "farm", "wary"),
        (HILDA, "seen", "theft", "farm", "wary"),
        (HILDA, "heard", "theft", "farm", None),
        (LOBART, "suspects", "theft", "farm", None),
        (HILDA, "heard", "murder", "farm", "hostile"),
        (HILDA, "heard", "murder", "city", None),
        (HILDA, "seen", "murder", "city", "wary"),
    ],
)
async def test_attitude_depends_on_proof_victim_and_murdered_community(
    knowledge, npc, degree, kind, community, expected
):
    knowledge.crimes.people[HILDA] = {"guild": 8}
    crime = await theft(knowledge)
    if kind == "murder":
        await knowledge.chronicle.record({**crime, "id": "murder", "kind": kind, "community": community})
        crime = {"id": "murder"}
    await learn(knowledge, npc, crime["id"], degree)
    assert knowledge.crimes.attitude(npc) == expected


@pytest.mark.parametrize("evidence", ["seen", "heard", "testimony"])
async def test_learning_a_crime_reacts_once_and_preserves_the_models_decision_after_load(
    knowledge, attitude_dialogue, evidence
):
    crime = await theft(knowledge)

    async def tell():
        if evidence == "testimony":
            await knowledge.chronicle.record(
                {"fact": "testimony", "npc": LOBART, "witness": HILDA, "crime_told": [crime["id"]]}
            )
        else:
            await learn(knowledge, LOBART, crime["id"], evidence)

    await tell()
    assert (await attitude_dialogue.open(LOBART, "Лобарт")).intent.attitude == "wary"
    await attitude_dialogue.change_intent(LOBART, "Лобарт", body=False, attitude="friendly")
    await knowledge.crimes.prepare(LOBART)
    await tell()  # Hearing the same evidence again does not overturn the conversation.
    assert (await attitude_dialogue.open(LOBART, "Лобарт")).intent.attitude == "friendly"

    saved = await knowledge.memory.head("A")
    await knowledge.link.receive(
        Session(type="session", branch="loaded", parent="A", parent_seq=saved, reason="load")
    )
    await knowledge.crimes.prepare(LOBART)
    await tell()
    assert (await attitude_dialogue.open(LOBART, "Лобарт")).intent.attitude == "friendly"
    await knowledge.link.receive(Session(type="session", branch="A", reason="connect"))
    await knowledge.crimes.prepare(LOBART)
    assert (await attitude_dialogue.open(LOBART, "Лобарт")).intent.attitude == "friendly"


async def test_enemy_witness_keeps_truth_without_attitude_change_before_or_after_load(
    knowledge, attitude_dialogue,
):
    knowledge.crimes.people[HILDA] = {"guild": 8, "npctype": 0}
    knowledge.crimes.people[MALETH] = {"guild": 8, "npctype": 0}
    knowledge.crimes.sides.note_fight(LOBART, HILDA)
    crime = await knowledge.crimes.record(deed(
        kind="murder", victim=LOBART, victim_guild=8, witnesses=[HILDA, MALETH],
        witnesses_metadata=[{"npc": HILDA, "guild": 8}, {"npc": MALETH, "guild": 8}],
    ))
    assert crime is not None and crime["witnesses"] == [HILDA, MALETH]
    assert knowledge.crimes.side(HILDA, crime["id"])["why"] == "fought"
    assert knowledge.crimes.known(HILDA)[0].degree == "seen"
    assert "Ты видел сам" in knowledge.crimes.situation(HILDA)
    assert knowledge.crimes.attitude(HILDA) is None
    assert knowledge.crimes.distrust(HILDA) == []
    assert knowledge.crimes.distrust(MALETH)
    assert (await attitude_dialogue.open(HILDA, "Хильда")).intent.attitude == "neutral"
    assert [fact["npc"] for fact in knowledge.chronicle.facts("crime_attitude")] == [MALETH]
    assert (await attitude_dialogue.open(MALETH, "Малет")).intent.attitude == "hostile"
    assert memory_value(knowledge.crimes, HILDA) == (0, 0)
    assert memory_value(knowledge.crimes, MALETH) == (4, 3)

    saved = await knowledge.memory.head("A")
    await knowledge.link.receive(
        Session(type="session", branch="loaded", parent="A", parent_seq=saved, reason="load")
    )
    assert knowledge.crimes.sides.fights == {}
    await knowledge.crimes.prepare(HILDA)
    assert knowledge.crimes.side(HILDA, crime["id"]) is not None
    assert knowledge.crimes.attitude(HILDA) is None
    assert knowledge.crimes.distrust(HILDA) == []
    assert knowledge.crimes.distrust(MALETH)
    assert (await attitude_dialogue.open(HILDA, "Хильда")).intent.attitude == "neutral"
    assert [fact["npc"] for fact in knowledge.chronicle.facts("crime_attitude")] == [MALETH]
    assert memory_value(knowledge.crimes, HILDA) == (0, 0)
    assert memory_value(knowledge.crimes, MALETH) == (4, 3)


async def test_an_older_branch_can_react_to_the_same_crime_independently(knowledge, attitude_dialogue):
    crime = await theft(knowledge)
    saved = await knowledge.memory.head("A")
    await learn(knowledge, LOBART, crime["id"], "seen")
    await attitude_dialogue.change_intent(LOBART, "Лобарт", body=False, attitude="friendly")

    await knowledge.link.receive(
        Session(type="session", branch="older", parent="A", parent_seq=saved, reason="load")
    )
    assert (await attitude_dialogue.open(LOBART, "Лобарт")).intent.attitude == "neutral"
    await learn(knowledge, LOBART, crime["id"], "seen")
    assert (await attitude_dialogue.open(LOBART, "Лобарт")).intent.attitude == "wary"

    await knowledge.link.receive(Session(type="session", branch="A", reason="connect"))
    await knowledge.crimes.prepare(LOBART)
    assert (await attitude_dialogue.open(LOBART, "Лобарт")).intent.attitude == "friendly"


@pytest.mark.parametrize("initial", ["wary", "hostile"])
async def test_existing_caution_still_consumes_the_crime_reaction(knowledge, attitude_dialogue, initial):
    await attitude_dialogue.change_intent(LOBART, "Лобарт", body=False, attitude=initial)
    crime = await theft(knowledge)
    await learn(knowledge, LOBART, crime["id"], "seen")
    assert (await attitude_dialogue.open(LOBART, "Лобарт")).intent.attitude == initial

    await attitude_dialogue.change_intent(LOBART, "Лобарт", body=False, attitude="friendly")
    await learn(knowledge, LOBART, crime["id"], "heard")
    await knowledge.crimes.prepare(LOBART)
    assert (await attitude_dialogue.open(LOBART, "Лобарт")).intent.attitude == "friendly"


async def test_oldcamp_murder_can_apply_delayed_hostility_only_once(knowledge, attitude_dialogue):
    knowledge.crimes.people[HILDA] = {"guild": 1}
    crime = await knowledge.crimes.record(
        deed(kind="murder", victim=LOBART, victim_guild=1, victim_npctype=3, witnesses=[HILDA])
    )
    assert crime is not None
    assert (await attitude_dialogue.open(HILDA, "Хильда")).intent.attitude == "wary"

    async def identity(npc, expressions):
        assert npc == HILDA
        assert [e[2] for e in expressions] == ["GUILD", "NPCTYPE"]
        return [1, 3]

    knowledge.link.story_eval = identity
    await knowledge.crimes.prepare(HILDA)
    assert (await attitude_dialogue.open(HILDA, "Хильда")).intent.attitude == "hostile"
    await attitude_dialogue.change_intent(HILDA, "Хильда", body=False, attitude="friendly")
    await knowledge.crimes.prepare(HILDA)
    await learn(knowledge, HILDA, crime["id"], "seen")
    assert (await attitude_dialogue.open(HILDA, "Хильда")).intent.attitude == "friendly"

    saved = await knowledge.memory.head("A")
    await knowledge.link.receive(
        Session(type="session", branch="loaded", parent="A", parent_seq=saved, reason="load")
    )
    await knowledge.crimes.prepare(HILDA)
    assert (await attitude_dialogue.open(HILDA, "Хильда")).intent.attitude == "friendly"


@pytest.mark.parametrize(
    "npc,degree,broken",
    [(LOBART, "heard", True), (HILDA, "seen", True), (HILDA, "heard", False), (LOBART, "suspects", False)],
)
async def test_trust_breaks_for_a_witness_or_informed_victim_only(knowledge, npc, degree, broken):
    crime = await theft(knowledge)
    await learn(knowledge, npc, crime["id"], degree)
    assert knowledge.trust.of(npc).broken is broken
    if broken:
        assert knowledge.trust.situation(npc) is not None
        await knowledge.chronicle.record({"fact": "forgiven", "npc": npc, "crime": crime["id"]})
        assert not knowledge.trust.of(npc).broken


async def test_unheard_murder_and_forgiven_murder_do_not_make_a_community_hostile(knowledge):
    knowledge.crimes.people[HILDA] = {"guild": 8}
    crime = await knowledge.crimes.record(deed(kind="murder", victim=LOBART, victim_guild=8))
    assert crime is not None
    assert knowledge.crimes.attitude(HILDA) is None
    await learn(knowledge, HILDA, crime["id"], "heard")
    assert knowledge.crimes.attitude(HILDA) == "hostile"
    assert knowledge.trust.broken(HILDA)
    await knowledge.chronicle.record({"fact": "forgiven", "npc": HILDA, "crime": crime["id"]})
    assert knowledge.crimes.attitude(HILDA) is None
    assert not knowledge.trust.broken(HILDA)


@pytest.mark.parametrize("victim_type,expected", [(3, "hostile"), (0, None)])
async def test_conversation_reads_npctype_for_oldcamp_membership(knowledge, victim_type, expected):
    crime = await knowledge.crimes.record(
        deed(kind="murder", victim=LOBART, victim_guild=1, victim_npctype=victim_type)
    )
    assert crime is not None
    knowledge.crimes.people[HILDA] = {"guild": 1}  # story_people has no NPCTYPE
    await learn(knowledge, HILDA, crime["id"], "heard")
    assert knowledge.crimes.attitude(HILDA) is None

    async def identity(npc, expressions):
        assert npc == HILDA
        assert [e[2] for e in expressions] == ["GUILD", "NPCTYPE"]
        return [1, 3]

    knowledge.link.story_eval = identity
    await knowledge.crimes.prepare(HILDA)
    assert knowledge.crimes.attitude(HILDA) == expected
    assert knowledge.trust.broken(HILDA) is (expected == "hostile")


async def test_loading_a_saved_branch_restores_only_its_knowledge(knowledge):
    crime = await theft(knowledge, witnesses=[HILDA])
    saved = await knowledge.memory.head("A")
    await learn(knowledge, LOBART, crime["id"], "heard")
    await knowledge.chronicle.record({"fact": "forgiven", "npc": HILDA, "crime": crime["id"]})
    assert knowledge.crimes.known(LOBART)
    assert knowledge.crimes.known(HILDA)[0].forgiven

    await knowledge.link.receive(
        Session(type="session", branch="B", parent="A", parent_seq=saved, reason="load")
    )
    assert knowledge.crimes.known(LOBART) == []
    assert knowledge.crimes.situation(LOBART) is None
    assert not knowledge.trust.of(LOBART).broken
    assert knowledge.crimes.known(HILDA)[0].degree == "seen"
    assert not knowledge.crimes.known(HILDA)[0].forgiven
    await learn(knowledge, LOBART, crime["id"], "suspects")
    assert knowledge.crimes.known(LOBART)[0].degree == "suspects"

    await knowledge.link.receive(Session(type="session", branch="A", reason="connect"))
    assert knowledge.crimes.known(LOBART)[0].degree == "heard"
    assert knowledge.crimes.known(HILDA)[0].forgiven


@pytest.mark.parametrize("who", ["PLAYER", MALETH, None])
async def test_missing_chest_property_suspects_only_whom_the_owner_saw(knowledge, who):
    crime = await theft(knowledge, chest=CHEST)
    sighting = await presence(knowledge, who) if who else None
    facts = await knowledge.crimes.suspect_chest(LOBART, CHEST, {"ITFO_BREAD": 1})
    assert len(facts) == (1 if who else 0)
    assert knowledge.chronicle.facts("crime_seen") == []
    if who:
        (known,) = knowledge.crimes.known(LOBART)
        assert known.degree == "suspects" and known.suspect == who
        assert facts[0]["crime"] == crime["id"] and facts[0]["sighting"] == sighting.record()
    else:
        assert knowledge.crimes.known(LOBART) == []
    assert bool(knowledge.crimes.concerns(LOBART)) is (who == "PLAYER")
    assert bool(knowledge.crimes.situation(LOBART)) is (who == "PLAYER")
    assert knowledge.crimes.attitude(LOBART) is None
    assert not knowledge.trust.of(LOBART).broken


@pytest.mark.parametrize(
    "observation",
    [
        {"end": AT - 31, "start": AT - 40},
        {"start": AT + 31, "end": AT + 40},
        {"place": "LOBART_YARD"},
        {"trace_kind": "broken"},
    ],
)
async def test_suspicion_requires_a_person_at_the_same_place_near_the_deed(knowledge, observation):
    await theft(knowledge, chest=CHEST)
    await presence(knowledge, **observation)
    assert (await knowledge.crimes.suspect_chest(LOBART, CHEST, {"ITFO_BREAD": 1})) == []
    assert knowledge.crimes.known(LOBART) == []
    assert knowledge.crimes.concerns(LOBART) == []


@pytest.mark.parametrize(
    "change",
    [{"chest": "ANOTHER_CHEST"}, {"item": "ITFO_APPLE"}, {"npc": HILDA}, {"waypoint": "UNKNOWN"}],
)
async def test_suspicion_requires_this_owners_chest_and_actual_missing_property(knowledge, change):
    fields = {"chest": CHEST, "waypoint": "FARM1_IN"}
    if "waypoint" in change:
        fields["waypoint"] = change["waypoint"]
    await theft(knowledge, **fields)
    await presence(knowledge)
    assert (
        (await knowledge.crimes.suspect_chest(
            change.get("npc", LOBART), change.get("chest", CHEST), {change.get("item", "ITFO_BREAD"): 1}
        ))
        == []
    )


async def test_a_house_theft_without_chest_does_not_create_suspicion(knowledge):
    await theft(knowledge)
    await presence(knowledge)
    assert (await knowledge.crimes.suspect_chest(LOBART, CHEST, {"ITFO_BREAD": 1})) == []
    assert knowledge.crimes.known(LOBART) == []


async def test_suspicion_is_recorded_once_and_its_question_is_raised_once(knowledge):
    crime = await theft(knowledge, chest=CHEST)
    await presence(knowledge)
    assert len(await knowledge.crimes.suspect_chest(LOBART, CHEST, {"ITFO_BREAD": 1})) == 1
    assert (await knowledge.crimes.suspect_chest(LOBART, CHEST, {"ITFO_BREAD": 1})) == []
    assert len(knowledge.chronicle.facts("crime_suspected")) == 1
    (concern,) = knowledge.crimes.concerns(LOBART)
    assert concern.key == f"crime_suspected:{crime['id']}"
    assert concern.npc == LOBART and "подозреваешь" in concern.ru
    await knowledge.chronicle.record({"fact": "initiative", "npc": LOBART, "occasion": concern.key})
    assert knowledge.crimes.concerns(LOBART) == []
    assert knowledge.crimes.known(LOBART)[0].degree == "suspects"


async def test_proof_or_forgiveness_ends_the_suspicion_question(knowledge):
    crime = await theft(knowledge, chest=CHEST)
    await presence(knowledge)
    await knowledge.crimes.suspect_chest(LOBART, CHEST, {"ITFO_BREAD": 1})
    assert knowledge.crimes.concerns(LOBART)
    await knowledge.chronicle.record({"fact": "forgiven", "npc": LOBART, "crime": crime["id"]})
    assert knowledge.crimes.concerns(LOBART) == []

    second = await theft(knowledge, id="second-theft", chest=CHEST)
    await knowledge.crimes.suspect_chest(LOBART, CHEST, {"ITFO_BREAD": 1})
    assert knowledge.crimes.concerns(LOBART)
    await learn(knowledge, LOBART, second["id"], "heard")
    assert knowledge.crimes.concerns(LOBART) == []


async def test_hearing_the_witness_clears_the_cached_suspicion_question(knowledge):
    deals = SimpleNamespace(
        debts={},
        day=3,
        crimes=None,
        on_day=[],
        on_reload=[],
        lies_of=lambda npc: ({}, set(), set()),
    )
    concerns = Concerns(knowledge.link, knowledge.chronicle, deals)
    concerns.extra.append(knowledge.crimes.concerns)
    crime = await theft(knowledge, chest=CHEST)
    await presence(knowledge)
    await knowledge.crimes.suspect_chest(LOBART, CHEST, {"ITFO_BREAD": 1})
    assert concerns.of(LOBART)
    await learn(knowledge, LOBART, crime["id"], "heard")
    assert concerns.of(LOBART) == []
