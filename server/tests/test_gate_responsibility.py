"""A real broken guarantee reaches the voucher and the city's rumor network."""

from gothic_ai.people_knowledge import PeopleKnowledge
from gothic_ai.social import Rumors
from gothic_ai.trust import Trust
from tests.test_gate_rules import HARAD, LOTHAR, _vouch
from tests.test_gates import CITY, GatesFake, setup_gate
from tests.test_social import _person

ANDRE, HAGEN = "MIL_311_ANDRE", "PAL_200_HAGEN"


async def test_known_city_crime_notifies_voucher_and_spreads_its_real_breach():
    fake = GatesFake()
    fake.people = [
        _person(npc, name, (0, 1440, [0, 0, 0]), guild=guild)
        for npc, name, guild in [(ANDRE, "Андре", 2), (HAGEN, "Хаген", 1), (HARAD, "Гарад", 3)]
    ]
    async with setup_gate(fake) as system:
        link, dialogue, chronicle = system[:3]
        rumors = Rumors(link, dialogue, chronicle)
        rumors.gate = lambda *args: True
        await rumors.tick()
        vouch = await _vouch(system)
        chronicle.record({
            "fact": "crime", "id": "city-theft", "kind": "theft", "community": "city",
            "victim": HARAD, "owner": HARAD, "day": fake.day, "hour": fake.hour, "minute": fake.minute,
        })
        assert chronicle.facts("gate_vouch_broken") == []
        chronicle.record({
            "fact": "crime_known", "community": "city", "crime": "city-theft",
            "via": CITY, "how": "witness", "at": vouch["at"],
        })

        [broken] = chronicle.facts("gate_vouch_broken")
        assert (broken["vouch"], broken["crime"], broken["npc"]) == (vouch["id"], "city-theft", LOTHAR)
        assert broken["crime_kind"] == "theft"
        [distrust] = Trust(chronicle).of(LOTHAR).crimes
        assert "«theft»" in distrust and broken["title"] not in distrust
        memory = chronicle.memory.load(chronicle.branch, LOTHAR)
        assert any("Ты поручился за него" in entry.content for entry in memory.entries)
        [rumor] = rumors.rumors.values()
        assert rumor.known == {ANDRE: 0, HAGEN: 0}

        fake.minute += 1
        await rumors.tick()

        [heard] = [f for f in chronicle.facts("rumor") if f["to"] == HARAD]
        assert heard["kind"] == "gate_vouch_broken" and heard["subject"] == LOTHAR
        assert heard["title"] == "Лотар поручился за преступника"
        assert heard["from"] in {ANDRE, HAGEN} and heard["hops"] == 1
        [knowledge] = PeopleKnowledge(chronicle).of(HARAD)
        assert knowledge.subject == LOTHAR and not knowledge.direct
