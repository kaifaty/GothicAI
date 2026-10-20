from __future__ import annotations

import asyncio
from types import SimpleNamespace

import pytest

from gothic_ai.chronicle import Chronicle
from gothic_ai.crime_law import LAW_COMMUNITIES, REPORT_DELAYS, CrimeLaw
from gothic_ai.crimes import Crimes
from gothic_ai.memory import MemoryStore
from gothic_ai.protocol import Session
from tests.test_crimes import Link

AT = 3 * 1440 + 9 * 60 + 15
MATTEO = "VLK_406_MATTEO"
CITIZEN = "VLK_400_CITIZEN"
GUARD = "MIL_305_JERGEL"
ANDRE = "MIL_311_ANDRE"


@pytest.fixture
async def law():
    link = Link()
    memory = MemoryStore(":memory:")
    chronicle = Chronicle(link, memory)  # type: ignore[arg-type]
    crimes = Crimes(link, chronicle)  # type: ignore[arg-type]
    system = SimpleNamespace(
        link=link, memory=memory, chronicle=chronicle, crimes=crimes, law=CrimeLaw(crimes)
    )
    yield system
    await crimes.close()
    if chronicle._background:
        await asyncio.gather(*chronicle._background)
    memory.close()


def case(system, *, crime="theft-1", kind="theft", community="city", via=CITIZEN, guild=3, npctype=0):
    fact = {
        "fact": "crime",
        "id": crime,
        "kind": kind,
        "community": community,
        "victim": MATTEO,
        "day": AT // 1440,
        "hour": AT % 1440 // 60,
        "minute": AT % 60,
        "witnesses": [via] if via else [],
        "item": "ITFO_BREAD",
        "count": 1,
    }
    system.chronicle.record(fact)
    if via:
        system.chronicle.record(
            {"fact": "crime_seen", "crime": crime, "npc": via, "guild": guild, "npctype": npctype}
        )
    return fact


def blocker(system, kind, *, at=AT + 30, via=CITIZEN):
    if kind == "died":
        fact = {"fact": "died", "npc": via}
    elif kind == "silenced":
        fact = {"fact": "silenced", "npc": via, "crime": "theft-1"}
    else:
        fact = {"fact": "forgiven", "npc": MATTEO, "crime": "theft-1", "paid": kind == "repaid"}
    if at is not None:
        fact["at"] = at
    system.chronicle.record(fact)


@pytest.mark.parametrize("cover", [True, False])
async def test_cover_silence_stops_law_only_with_cover_enabled(law, cover):
    law.crimes.cover = cover
    case(law)
    law.chronicle.record({
        "fact": "silenced", "npc": CITIZEN, "crime": "theft-1", "cover": "D1", "at": AT + 30,
    })
    [delivery] = law.law.deliveries()
    assert delivery.status == ("stopped" if cover else "pending")


@pytest.mark.parametrize("promise", ["silence", "cover"])
@pytest.mark.parametrize("keeps", [False, True])
async def test_broken_promise_reaches_law_and_records_one_actual_betrayal(law, promise, keeps):
    case(law)
    law.chronicle.record({
        "fact": "npc_intent", "npc": CITIZEN, "deal": "D1", "keeps": keeps, "how": [promise],
    })
    law.chronicle.record({
        "fact": "deal", "npc": CITIZEN, "deal": "D1", "at": AT + 30, **(
            {"cover": {"crime": "theft-1"}} if promise == "cover" else {"silence": ["crime:theft-1"]}
        ),
    })
    [delivery] = law.law.deliveries()
    assert delivery.status == ("stopped" if keeps else "pending")
    law.law.tick(AT + REPORT_DELAYS["theft"])
    law.law.tick(AT + REPORT_DELAYS["theft"] + 1)
    assert bool(law.law.known("city")) is (not keeps)
    assert law.chronicle.facts("betrayed") == ([] if keeps else [{
        "fact": "betrayed", "npc": CITIZEN, "deal": "D1", "how": "reported", "to": "city",
    }])


@pytest.mark.parametrize("promise", ["silence", "cover"])
async def test_disabling_cheating_restores_raw_promises_for_crime_knowledge_and_law(law, promise):
    case(law)
    law.chronicle.record({
        "fact": "npc_intent", "npc": CITIZEN, "deal": "D1", "keeps": False, "how": [promise],
    })
    law.chronicle.record({
        "fact": "deal", "npc": CITIZEN, "deal": "D1", "at": AT + 30, **(
            {"cover": {"crime": "theft-1"}} if promise == "cover" else {"silence": ["crime:theft-1"]}
        ),
    })
    assert not law.crimes.known(CITIZEN)[0].silenced
    law.chronicle.npc_cheat_enabled = False
    assert law.crimes.known(CITIZEN)[0].silenced
    assert law.law.deliveries()[0].status == "stopped"
    assert law.law.tick(AT + REPORT_DELAYS["theft"]) == []
    assert law.chronicle.facts("betrayed") == []


async def test_a_report_that_arrived_before_the_promise_does_not_betray_it(law):
    case(law)
    law.chronicle.record({
        "fact": "npc_intent", "npc": CITIZEN, "deal": "D1", "keeps": False, "how": ["silence"],
    })
    law.chronicle.record({
        "fact": "deal", "npc": CITIZEN, "deal": "D1", "at": AT + REPORT_DELAYS["theft"] + 1,
        "silence": ["crime:theft-1"],
    })
    assert len(law.law.tick(AT + REPORT_DELAYS["theft"] + 2)) == 1
    assert law.chronicle.facts("betrayed") == []


@pytest.mark.parametrize("cover", [True, False])
async def test_old_covered_testimony_reaches_law_when_cover_is_disabled(law, cover):
    law.crimes.cover = cover
    case(law, via="")
    law.chronicle.record({
        "fact": "testimony", "npc": GUARD, "witness": CITIZEN,
        "crime_saw": ["theft-1"], "crime_told": [], "cover": "D1", "at": AT + 30,
        "guild": 2, "npctype": 0,
    })
    deliveries = law.law.deliveries()
    assert len(deliveries) == int(not cover)
    if deliveries:
        assert (deliveries[0].crime["id"], deliveries[0].via) == ("theft-1", GUARD)


@pytest.mark.parametrize("npc,guild", [(GUARD, 2), ("PAL_205_LORD", 1), (ANDRE, -1)])
async def test_guard_and_lawkeeper_witnesses_inform_law_immediately(law, npc, guild):
    crime = case(law, via=npc, guild=guild)
    (arrival,) = law.law.tick(AT)
    assert arrival == {
        "fact": "crime_known",
        "community": "city",
        "crime": crime["id"],
        "how": "guard",
        "via": npc,
        "at": AT,
    }
    assert law.law.unpaid("city") == [crime]
    assert law.law.tick(AT + 1) == []


@pytest.mark.parametrize("npc,guild", [(CITIZEN, 3), (GUARD, 2), (ANDRE, -1)])
async def test_allied_witness_does_not_deliver_even_after_loading(law, npc, guild):
    crime = case(law, kind="murder", via=npc, guild=guild)
    law.chronicle.record({"fact": "crime_side", "npc": npc, "crime": crime["id"], "why": "fought"})
    head = law.memory.head("A")
    law.link.receive(Session(type="session", branch="B", parent="A", parent_seq=head))
    assert law.crimes.side(npc, crime["id"]) is not None
    assert law.crimes.known(npc)[0].degree == "seen"
    assert law.law.deliveries() == []
    assert law.law.tick(AT + 2000) == []
    assert law.law.known("city") == []
    assert law.law.debt("city") == 0


async def test_another_witness_still_delivers_when_one_witness_is_an_ally(law):
    crime = case(law, kind="murder", via=GUARD, guild=2)
    law.chronicle.record({"fact": "crime_side", "npc": GUARD, "crime": crime["id"], "why": "hostile"})
    law.chronicle.record({
        "fact": "crime_seen", "crime": crime["id"], "npc": CITIZEN, "guild": 3, "npctype": 0,
    })
    assert law.law.tick(AT) == []
    (delivery,) = law.law.deliveries()
    assert (delivery.via, delivery.how, delivery.due) == (CITIZEN, "witness", AT + 120)
    (arrival,) = law.law.tick(AT + 120)
    assert arrival["via"] == CITIZEN


@pytest.mark.parametrize("kind,delay", [("theft", 1440), ("sheep", 1440), ("attack", 720), ("murder", 120)])
async def test_community_witness_report_waits_for_severity_deadline(law, kind, delay):
    case(law, kind=kind)
    assert REPORT_DELAYS[kind] == delay
    assert law.law.tick(AT + delay - 1) == []
    (report,) = law.law.deliveries()
    assert report.status == "pending" and report.due == AT + delay
    (arrival,) = law.law.tick(AT + delay)
    assert arrival["how"] == "witness" and arrival["via"] == CITIZEN
    assert law.law.deliveries()[0].status == "known"


@pytest.mark.parametrize("kind", ["rumor", "testimony"])
async def test_hearing_a_crime_teaches_the_guard_or_lawkeeper(law, kind):
    crime = case(law, via="")
    fact = {"fact": kind, "at": AT + 50}
    if kind == "rumor":
        fact.update(crime=crime["id"], to=ANDRE, **{"from": CITIZEN})
    else:
        fact.update(npc=ANDRE, crime_told=[crime["id"]], witness=CITIZEN)
    law.chronicle.record(fact)
    (arrival,) = law.law.tick(AT + 50)
    assert arrival["how"] == "rumor" and arrival["via"] == ANDRE
    assert arrival["at"] == AT + 50


async def test_rumor_to_another_community_or_civilian_does_not_teach_the_law(law):
    case(law, via="")
    for npc in (CITIZEN, "SLD_800_LEE"):
        law.chronicle.record({"fact": "rumor", "crime": "theft-1", "to": npc, "at": AT + 1})
    assert law.law.tick(AT + 2000) == []
    assert law.law.known("city") == []


async def test_guard_call_teaches_heard_and_arrives_through_common_path_once(law):
    crime = case(law, via="")
    law.crimes.people[GUARD] = {"guild": 2, "npctype": 0}
    law.crimes.people[CITIZEN] = {"guild": 3, "npctype": 0}
    facts = law.law.called(crime["id"], CITIZEN, [GUARD, GUARD, CITIZEN], at=AT + 1)
    rumor, arrival = facts
    knowledge, = law.crimes.known(GUARD)
    assert knowledge.degree == "heard" and knowledge.source == CITIZEN
    assert rumor["how"] == arrival["how"] == "called"
    assert rumor["quest"] == "crime:theft-1" and rumor["kind"] == "theft"
    assert arrival["fact"] == "crime_known" and arrival["at"] == AT + 1
    assert arrival["via"] == CITIZEN
    assert law.crimes.known(CITIZEN) == []
    assert law.law.called(crime["id"], CITIZEN, [GUARD], at=AT + 2) == []
    assert law.law.tick(AT + 2000) == []
    assert law.law.unpaid("city") == [crime]


async def test_guard_call_without_a_guard_or_real_law_does_not_deliver(law):
    case(law, via="")
    assert law.law.called("missing", CITIZEN, [GUARD], at=AT) == []
    assert law.law.called("theft-1", CITIZEN, [], at=AT) == []
    assert law.law.called("theft-1", CITIZEN, [CITIZEN], at=AT) == []
    case(law, crime="loner", community=None, via="")
    assert law.law.called("loner", CITIZEN, [GUARD], at=AT) == []
    assert law.chronicle.facts("crime_known") == []


async def test_ally_cannot_call_guards_or_inform_law_directly(law):
    crime = case(law, kind="murder")
    law.crimes.people[GUARD] = {"guild": 2, "npctype": 0}
    law.chronicle.record({"fact": "crime_side", "npc": CITIZEN, "crime": crime["id"], "why": "quest"})
    assert law.law.called(crime["id"], CITIZEN, [GUARD], at=AT) == []
    assert law.law.inform(crime["id"], via=CITIZEN.lower(), how="called", at=AT) is None
    assert law.chronicle.facts("rumor") == []
    assert law.chronicle.facts("crime_known") == []


async def test_a_call_or_rumor_does_not_make_an_allied_guard_report(law):
    crime = case(law, kind="murder", via="")
    law.crimes.people[GUARD] = {"guild": 2, "npctype": 0}
    law.chronicle.record({"fact": "crime_side", "npc": GUARD, "crime": crime["id"], "why": "fought"})
    assert law.law.called(crime["id"], CITIZEN, [GUARD], at=AT) == []
    law.chronicle.record({
        "fact": "rumor", "crime": crime["id"], "to": GUARD, "from": CITIZEN, "at": AT + 1,
    })
    assert law.law.tick(AT + 2000) == []
    assert law.law.known("city") == []
    law.chronicle.record({
        "fact": "rumor", "crime": crime["id"], "to": ANDRE, "from": CITIZEN, "at": AT + 2,
    })
    (arrival,) = law.law.tick(AT + 2000)
    assert arrival["via"] == ANDRE and arrival["how"] == "rumor"


@pytest.mark.parametrize("kind", ["died", "silenced", "forgiven", "repaid"])
async def test_blocker_before_deadline_stops_witness_report(law, kind):
    case(law)
    blocker(law, kind)
    assert law.law.tick(AT + 2000) == []
    (report,) = law.law.deliveries()
    assert report.status == "stopped" and report.reason == kind
    assert law.law.debt("city") == 0


@pytest.mark.parametrize("kind", ["died", "silenced", "forgiven", "repaid"])
async def test_blocker_after_arrival_does_not_erase_law_knowledge(law, kind):
    case(law)
    law.law.tick(AT + 1440)
    blocker(law, kind, at=AT + 1441)
    assert law.law.tick(AT + 1442) == []
    assert [f["id"] for f in law.law.unpaid("city")] == ["theft-1"]
    assert law.law.debt("city") == 50


@pytest.mark.parametrize("offset", [0, 1])
async def test_late_poll_still_delivers_before_a_blocker_at_or_after_deadline(law, offset):
    case(law)
    blocker(law, "died", at=AT + 1440 + offset)
    assert len(law.law.tick(AT + 2000)) == 1


async def test_unstamped_events_get_branch_persistent_event_clocks_once(law):
    case(law)
    blocker(law, "silenced", at=None)
    assert law.law.tick(AT + 100) == []
    (stamp,) = law.chronicle.facts("crime_law_time")
    assert stamp == {"fact": "crime_law_time", "event": 2, "at": AT + 100}
    law.law.tick(AT + 3000)
    assert law.chronicle.facts("crime_law_time") == [stamp]
    assert law.law.deliveries()[0].reason == "silenced"


async def test_second_unblocked_witness_still_carries_the_news(law):
    case(law)
    law.chronicle.record(
        {"fact": "crime_seen", "crime": "theft-1", "npc": "VLK_OTHER", "guild": 3, "npctype": 0}
    )
    blocker(law, "silenced")
    (arrival,) = law.law.tick(AT + 1440)
    assert arrival["via"] == "VLK_OTHER"


async def test_forgiveness_by_someone_other_than_the_victim_does_not_suppress_law(law):
    case(law)
    law.chronicle.record({"fact": "forgiven", "npc": CITIZEN, "crime": "theft-1", "at": AT + 10})
    assert len(law.law.tick(AT + 1440)) == 1


@pytest.mark.parametrize("community", [None, "bl"])
async def test_no_law_without_community_or_among_bandits(law, community):
    case(law, community=community, via=ANDRE)
    assert law.law.tick(AT + 10000) == []
    assert law.law.deliveries() == []
    assert law.law.debt(community) == 0
    assert law.law.absolve(community, crimes=["theft-1"]) == []


@pytest.mark.parametrize(
    "community,npc,guild,npctype,expected",
    [
        ("city", GUARD, 2, 0, True),
        ("city", GUARD, 2, 3, False),
        ("oldcamp", GUARD, 2, 3, True),
        ("oldcamp", GUARD, 1, 4, True),
        ("farm", "SLD_OTHER", 7, 0, True),
        ("farm", "BAU_OTHER", 8, 0, False),
        ("monastery", "KDF_OTHER", 4, 0, True),
        ("monastery", "NOV_OTHER", 5, 0, False),
    ],
)
async def test_guard_community_rules(law, community, npc, guild, npctype, expected):
    assert law.law.is_guard(npc, community, {"guild": guild, "npctype": npctype}) is expected


async def test_missing_city_npctype_is_not_guessed_from_guard_guild(law):
    law.crimes.people[GUARD] = {"guild": 2}
    assert law.law.community_of(GUARD) is None
    assert not law.law.is_guard(GUARD, "city")
    assert not law.law.is_guard(GUARD, "oldcamp")


async def test_vanilla_counters_and_fine_use_only_unpaid_known_cases(law):
    case(law, via=GUARD, guild=2)
    case(law, crime="pending", kind="murder")
    law.law.tick(AT)
    assert law.law.counters("city") == {"THEFT": 1, "ATTACK": 0, "MURDER": 0, "SHEEPKILLER": 0}
    assert law.law.debt("city") == 50
    case(law, crime="attack", kind="attack", via=GUARD, guild=2)
    case(law, crime="murder", kind="murder", via=GUARD, guild=2)
    case(law, crime="sheep", kind="sheep", via=GUARD, guild=2)
    law.law.tick(AT)
    assert law.law.debt("city") == 700
    law.law.tick(AT + 120)
    assert law.law.debt("city") == 750
    for number in range(5):
        case(law, crime=f"extra-{number}", via=GUARD, guild=2)
    law.law.tick(AT + 120)
    assert law.law.debt("city") == 1000


@pytest.mark.parametrize("sheep_count,thefts,expected", [(1, 0, 100), (3, 0, 100), (1, 1, 100), (3, 1, 200)])
async def test_sheep_only_fine_is_flat_but_theft_uses_total_count(law, sheep_count, thefts, expected):
    for number in range(sheep_count):
        case(law, crime=f"sheep-{number}", kind="sheep", via=GUARD, guild=2)
    for number in range(thefts):
        case(law, crime=f"theft-{number}", via=GUARD, guild=2)
    law.law.tick(AT)
    assert law.law.debt("city") == expected


async def test_absolution_closes_law_but_not_npc_knowledge(law):
    crime = case(law, via=MATTEO, guild=3)
    law.law.tick(AT + 1440)
    assert law.crimes.known(MATTEO)[0].crime == crime
    (payment,) = law.law.absolve("city")
    assert payment["community"] == "city" and payment["crimes"] == ["theft-1"]
    assert law.law.unpaid("city") == [] and law.law.debt("city") == 0
    assert law.law.known("city") == [crime]
    assert law.crimes.known(MATTEO)[0].crime == crime
    assert not law.crimes.known(MATTEO)[0].forgiven
    assert law.law.tick(AT + 3000) == [] and law.law.absolve("city") == []


async def test_loc_all_absolves_all_existing_cases_including_pending_reports(law):
    for community in LAW_COMMUNITIES:
        case(law, crime=community, community=community, via=ANDRE if community == "city" else "")
    law.law.tick(AT)
    payments = law.law.absolve(None, how="LOC_ALL")
    assert {f["community"] for f in payments} == set(LAW_COMMUNITIES)
    assert all(f["how"] == "LOC_ALL" and f["crimes"] == [f["community"]] for f in payments)
    assert law.law.tick(AT + 2000) == []
    assert all(law.law.debt(community) == 0 for community in LAW_COMMUNITIES)


async def test_save_branches_before_and_after_report_and_payment_replay_only_their_facts(law):
    crime = case(law)
    law.law.tick(AT)
    before = law.memory.head("A")
    law.law.tick(AT + 1440)
    after = law.memory.head("A")
    law.law.absolve("city")
    paid = law.memory.head("A")
    for branch, head, now, expected_known, expected_debt in (
        ("before", before, AT + 1439, False, 0),
        ("after", after, AT + 1441, True, 50),
        ("paid", paid, AT + 1442, True, 0),
    ):
        law.link.receive(Session(type="session", branch=branch, parent="A", parent_seq=head))
        assert law.law.now is None
        law.law.tick(now)
        assert law.law.known("city") == ([crime] if expected_known else [])
        assert law.law.debt("city") == expected_debt
    law.link.receive(Session(type="session", branch="before"))
    (arrival,) = law.law.tick(AT + 1440)
    assert arrival["crime"] == crime["id"] and law.law.debt("city") == 50


async def test_load_preserves_companion_blocker_time(law):
    case(law)
    blocker(law, "died", at=None)
    law.law.tick(AT + 100)
    head = law.memory.head("A")
    law.link.receive(Session(type="session", branch="B", parent="A", parent_seq=head))
    assert law.law.tick(AT + 5000) == []
    assert law.law.deliveries()[0].reason == "died"


async def test_console_includes_known_pending_stopped_and_debt(law):
    case(law)
    case(law, crime="immediate", via=ANDRE)
    case(law, crime="stopped", via="VLK_OTHER")
    law.chronicle.record({"fact": "silenced", "npc": "VLK_OTHER", "crime": "stopped", "at": AT + 1})
    law.law.tick(AT + 2)
    text = law.law.summary()
    assert "город; долг 50" in text
    assert "знает immediate" in text
    assert "theft-1 через" in text and "в пути до дня 4, 09:15" in text
    assert "stopped через" in text and "свидетель обещал молчать" in text
