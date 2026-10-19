"""NPC news must not become evidence against the hero, even with legacy hero-shaped payloads."""

import asyncio
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest

from gothic_ai.amends import Amends
from gothic_ai.chains import ChainAuthor
from gothic_ai.concerns import _delivers_crime
from gothic_ai.crime_law import CrimeLaw
from gothic_ai.crime_law_runtime import CrimeLawRuntime
from gothic_ai.crime_meet import CrimeMeet
from gothic_ai.crime_memory import CrimeMemoryRuntime, memory_value
from gothic_ai.daylife import DayDesk, DayWorld, _chat_trip
from gothic_ai.errands import Errands
from gothic_ai.gossip import _news
from gothic_ai.showrunner import Showrunner
from gothic_ai.social import DAY, Rumor
from gothic_ai.trust import Trust
from tests.fake_game import FakeGame
from tests.test_concerns import _setup
from tests.test_director import HILDA, LOBART, QUEST, VINO
from tests.test_gossip import AT, _gossip, _tick, _together

ANDRE = "MIL_311_ANDRE"


@pytest.fixture
async def systems():
    cm, chronicle, deals, concerns = await _setup(FakeGame(branch="A"), with_crimes=True)
    assert deals.crimes is not None
    yield SimpleNamespace(
        chronicle=chronicle, deals=deals, concerns=concerns, crimes=deals.crimes,
        trust=Trust(chronicle, crimes=deals.crimes), law=CrimeLaw(deals.crimes),
    )
    await deals.crimes.close()
    await cm.__aexit__(None, None, None)


def rumor(**changes):
    return {
        "fact": "rumor", "kind": "caught", "subject": HILDA, "subject_name": "Хильда",
        "to": LOBART, "from": VINO, "from_name": "Вино", "about": LOBART,
        "about_name": "Лобарт", "quest": QUEST, "title": "Убрать поле репы", "hops": 1,
        **changes,
    }


def crime(systems):
    deed = {
        "fact": "crime", "id": "theft-1", "kind": "theft", "community": "city",
        "victim": LOBART, "item": "ITFO_BREAD", "item_name": "Хлеб", "count": 1,
        "day": 3, "hour": 9, "minute": 0, "witnesses": [],
    }
    systems.chronicle.record(deed)
    return deed


@pytest.mark.parametrize("kind", ["cover", "caught", "found_out", "broken"])
async def test_npc_news_is_neither_hero_trust_lie_deal_option_nor_concern(systems, kind):
    systems.chronicle.record(rumor(kind=kind, subject=HILDA.lower()))
    record = systems.trust.of(LOBART)
    assert not record.broken and not record.heard
    assert systems.trust.situation(LOBART) is None
    assert systems.deals.lies_of(LOBART) == ({}, set(), set())
    assert systems.concerns.of(LOBART) == []


@pytest.mark.parametrize("subject", [None, "", "player"])
async def test_legacy_hero_news_keeps_trust_deal_and_concern_semantics(systems, subject):
    fact = rumor(subject=subject)
    if subject is None:
        fact.pop("subject")
    systems.chronicle.record(fact)
    assert systems.trust.of(LOBART).heard
    assert systems.deals.lies_of(LOBART)[0]
    assert systems.concerns.top(LOBART) is not None


async def test_npc_news_cannot_supply_a_title_for_the_heros_claim_or_lie(systems):
    systems.chronicle.record(rumor(title="Чужая ложь"))
    systems.chronicle.record({"fact": "claim", "quest": QUEST, "npc": LOBART, "truth": True})
    systems.chronicle.record({"fact": "caught", "quest": QUEST, "npc": LOBART})
    record = systems.trust.of(LOBART)
    assert record.kept == [f"«{QUEST}»"]
    assert record.lied == [f"«{QUEST}»"]


async def test_npc_news_never_teaches_a_hero_crime_or_native_memory_or_law(systems):
    crime(systems)
    systems.chronicle.record(rumor(kind="theft", crime="theft-1", to=ANDRE, at=5000))
    assert systems.crimes.known(ANDRE) == []
    assert systems.crimes.distrust(ANDRE) == []
    assert memory_value(systems.crimes, ANDRE) == (0, 0)
    assert systems.law.deliveries() == []
    assert systems.law.tick(10000) == []
    assert systems.law.known("city") == []
    assert systems.law.debt("city") == 0
    assert not systems.chronicle.facts("crime_law_time")


async def test_npc_news_to_victim_does_not_resolve_a_witness_concern(systems):
    crime(systems)
    systems.crimes.people[VINO] = {"guild": 3, "npctype": 0}
    systems.chronicle.record({"fact": "crime_seen", "crime": "theft-1", "npc": VINO})
    assert systems.concerns.top(VINO).key == "crime_witness:theft-1"
    fact = rumor(kind="theft", crime="theft-1")
    systems.chronicle.record(fact)
    assert not _delivers_crime(fact, "theft-1", LOBART, systems.crimes)
    systems.concerns._cache.clear()
    assert systems.concerns.top(VINO).key == "crime_witness:theft-1"


async def test_legacy_crime_rumor_still_teaches_the_guard_and_law(systems):
    crime(systems)
    fact = rumor(kind="theft", crime="theft-1", to=ANDRE, at=5000)
    fact.pop("subject")
    systems.chronicle.record(fact)
    assert systems.crimes.known(ANDRE)[0].suspect == "PLAYER"
    assert memory_value(systems.crimes, ANDRE) == (3, 3)
    assert systems.law.tick(5000)[0]["crime"] == "theft-1"


async def test_npc_news_never_queues_amends_for_the_hero(systems):
    amends = object.__new__(Amends)
    amends._queue = asyncio.Queue()
    amends.crimes = systems.crimes
    amends.chronicle = systems.chronicle
    amends.lies = {}
    amends.storylets, amends.looked = {}, {}
    crime(systems)
    for kind in ("cover", "caught", "found_out", "broken"):
        amends._on_fact(rumor(kind=kind))
    amends._on_fact(rumor(kind="theft", crime="theft-1"))
    assert amends._queue.empty() and amends.lies == {}
    systems.chronicle.record(rumor(kind="theft", crime="theft-1"))
    amends._load()
    assert amends._queue.empty()
    old = rumor()
    old.pop("subject")
    amends._on_fact(old)
    assert amends._queue.get_nowait()[1] == LOBART


async def test_npc_news_is_absent_from_chain_and_showrunner_hero_crime_knowledge(systems):
    deed = crime(systems)
    systems.chronicle.record(rumor(kind="theft", crime="theft-1", to=ANDRE))
    chain = object.__new__(ChainAuthor)
    chain.crimes, chain.chronicle = systems.crimes, systems.chronicle
    assert ANDRE not in chain._crime_context(deed, {})
    showrunner = object.__new__(Showrunner)
    showrunner.crimes, showrunner.law = systems.crimes, systems.law
    briefs = showrunner._crime_briefs({}, systems.chronicle.facts())
    assert all(k["npc"] != ANDRE for k in briefs["theft-1"]["knowledge"])
    old = rumor(kind="theft", crime="theft-1", to=ANDRE)
    old.pop("subject")
    systems.chronicle.record(old)
    assert ANDRE in chain._crime_context(deed, {})
    briefs = showrunner._crime_briefs({}, systems.chronicle.facts())
    assert any(k["npc"] == ANDRE and k["suspect"] == "PLAYER"
               for k in briefs["theft-1"]["knowledge"])


async def test_npc_news_does_not_wake_hero_crime_runtime_readers(systems):
    for runtime_type in (CrimeMemoryRuntime, CrimeLawRuntime, CrimeMeet):
        runtime = object.__new__(runtime_type)
        runtime._wake = asyncio.Event()
        runtime._on_fact(rumor(kind="theft", crime="theft-1"))
        assert not runtime._wake.is_set()

    memory = object.__new__(CrimeMemoryRuntime)
    memory.chronicle, memory._projection = systems.chronicle, {}
    memory.project = AsyncMock()
    crime(systems)
    systems.chronicle.record(rumor(kind="theft", crime="theft-1", to=ANDRE))
    await memory.tick()
    memory.project.assert_not_awaited()

    law = object.__new__(CrimeLawRuntime)
    law.chronicle, law.crimes = systems.chronicle, systems.crimes
    law._generation = 0
    law.link = SimpleNamespace(story_eval=AsyncMock())
    await law._people()
    law.link.story_eval.assert_not_awaited()
    assert law._news_levels() == {}


async def test_npc_gossip_passes_as_fact_without_a_hero_scene(tmp_path):
    async with _gossip(tmp_path) as g:
        await _together(g)
        r = Rumor("exposed:T1", "cover", LOBART, "Лобарт", "", known={LOBART: 0},
                  since={LOBART: AT // DAY}, subject=VINO, subject_name="Вино")
        g.rumors.rumors = {r.key: r}
        staging = g.desk.stage(r, LOBART, HILDA)
        assert staging.tier == "fact" and staging.why == "news about an npc"
        assert _tick(g) == 1
        [fact] = g.chronicle.facts("rumor")
        assert fact["subject"] == VINO and fact["to"] == HILDA
        assert VINO in _news(r) or "Вино" in _news(r)
        assert g.desk.last is None and not g.llm.calls


def test_day_trip_matching_distinguishes_subjects_and_errands_do_not_choose_own_news():
    r = Rumor(QUEST, "caught", LOBART, "Лобарт", "Репа", known={HILDA: 0},
              subject=HILDA, subject_name="Хильда")
    desk = object.__new__(DayDesk)
    fact = rumor(subject="PLAYER", **{"from": HILDA}, to=VINO)
    desk.chronicle = SimpleNamespace(facts=lambda kind: [fact])
    assert not desk._told(r, HILDA, VINO, 0)
    fact["subject"] = HILDA
    assert desk._told(r, HILDA, VINO, 0)
    errands = object.__new__(Errands)
    errands.rumors = lambda: SimpleNamespace(rumors={r.key: r})
    assert errands._rumor(HILDA, VINO) is None


def test_day_trip_news_describes_the_npc_subject():
    from gothic_ai.social import Social
    from tests.test_daylife import PEOPLE_DAY, P, Q

    social = Social(PEOPLE_DAY)
    now = DAY + 9 * 60
    r = Rumor("exposed:T1", "cover", LOBART, "Лобарт", "", known={P: 1},
              since={P: 1}, subject=HILDA, subject_name="Хильда")
    world = DayWorld(social, [r], lambda _, a, b: (a, b) == (P, Q), set(), lambda _: [], now, None)
    here = social.spot(P, now % DAY)
    trip = _chat_trip(P, "ZS_STAND_ARMSCROSSED", here, world)
    assert trip is not None
    assert "Хильда" in trip[1].text and "the hero lied" not in trip[1].text


async def test_errand_description_memory_and_reload_keep_the_npc_subject():
    from tests.test_errands import DAWN, _desk

    errands, link, _, _, rumors, chronicle, notes = _desk()
    r = Rumor("exposed:T1", "cover", LOBART, "Лобарт", "", known={LOBART: 0},
              subject=HILDA, subject_name="Хильда")
    rumors.rumors = {r.key: r}
    goal, why = await errands.plan(LOBART, "Лобарт", DAWN, link.snapshots[LOBART])
    assert goal is not None and not why
    assert goal.subject == HILDA and goal.title == r.description()
    assert "Хильда" in goal.what() and "рассказать о герое" not in goal.what()
    again, *_ = _desk(chronicle.facts())
    assert again.goals[goal.id].subject == HILDA
    errands._done(goal, ANDRE, "far")
    assert "Хильда" in notes[-1] and "Ты рассказал о герое" not in notes[-1]
