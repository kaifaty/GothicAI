"""World occasions are deterministic, branch-local facts; no model is involved."""

import asyncio
from contextlib import asynccontextmanager

import pytest

from gothic_ai.chronicle import Chronicle
from gothic_ai.game import GameError
from gothic_ai.memory import MemoryStore
from gothic_ai.occasions import OccasionFailed, Occasions, WaypointUnavailable
from gothic_ai.places import PlaceBook
from tests.fake_game import FakeGame, connected
from tests.fakes import Clock

LOBART, HILDA, VINO = "BAU_950_LOBART", "BAU_951_HILDA", "BAU_952_VINO"


@pytest.fixture
def places():
    spots = [("HOME", "house"), ("ROAD", "road"), ("CAMP", "camp"), ("CAVE", "cave")]
    return PlaceBook(
        {
            "version": 2,
            "world": "NEWWORLD.ZEN",
            "places": [
                {
                    "id": name,
                    "ru": name,
                    "where_ru": name,
                    "en": name,
                    "where_en": name,
                    "kind": kind,
                    "center": [i * 10000, 0, 0],
                }
                for i, (name, kind) in enumerate(spots)
            ],
            "points": {f"{name}_{n}": i for i, (name, _) in enumerate(spots) for n in (1, 2)},
        }
    )


@asynccontextmanager
async def world(places, *, gather=False, crimes=True, npc_crimes=True):
    fake, clock, store = FakeGame(), Clock(), MemoryStore(":memory:")
    fake.nearest_waypoint = "ROAD_1"
    fake.vars["KAPITEL"] = 1
    fake.people = [
        {
            "npc": npc,
            "name": npc,
            "guild": 0,
            "routine": [{"start": 0, "end": 1440, "state": "ZS_STAND", "waypoint": "HOME_1"}],
        }
        for npc in (LOBART, HILDA, VINO)
    ]
    fake.npc_states = {
        LOBART: {"state": "alive", "name": "Лобарт", "distance": 200},
        HILDA: {"state": "dead", "name": "Хильда", "distance": 100},
        VINO: {"state": "alive", "name": "Вино", "distance": 6000},
    }
    async with connected(fake) as link:
        chronicle = Chronicle(link, store)
        options = {} if gather else {"gather_s": 0}
        occasions = Occasions(
            link, chronicle, places, clock=clock, busy_poll_s=0.01, crimes=crimes,
            npc_crimes=npc_crimes, **options
        )
        yield fake, clock, chronicle, occasions


async def settle(fake, clock, occasions, place):
    fake.nearest_waypoint = f"{place}_1"
    await occasions.poll()
    clock.t += 10
    await occasions.poll()


async def load(fake, chronicle, branch="loaded", head=None):
    parent = chronicle.branch
    if head is None:
        head = chronicle.memory.head(parent)
    await fake.send(type="session", branch=branch, parent=parent, parent_seq=head, reason="load")
    # The reply follows the session message on the same socket: every listener has seen it now.
    await chronicle.link.story_state()


def kinds(occasions):
    return [f["kind"] for f in occasions.pending]


def vanilla_quest(**change):
    return {
        "fact": "vanilla_quest",
        "quest": "MIS_LOBART_RUEBEN",
        "title": "Убрать поле репы",
        "from": "open",
        "to": "running",
        "giver": LOBART,
        "day": 1,
        "hour": 8,
        "minute": 0,
        **change,
    }


async def test_a_place_must_hold_for_ten_seconds_not_just_one_waypoint(places):
    async with world(places) as (fake, clock, chronicle, occasions):
        fake.nearest_waypoint = "HOME_1"
        await occasions.poll()
        assert occasions.current is None
        clock.t += 5
        fake.nearest_waypoint = "HOME_2"  # another waypoint of the same place does not restart the clock
        await occasions.poll()
        assert chronicle.facts("visited") == []
        clock.t += 4.99
        await occasions.poll()
        assert chronicle.facts("visited") == []
        clock.t += 0.01
        await occasions.poll()
        assert occasions.current.id == "HOME"
        assert occasions.waypoint == "HOME_2"
        assert [f["place"] for f in chronicle.facts("visited")] == ["HOME"]
        assert kinds(occasions) == ["arrived"]


async def test_a_transient_place_does_not_count_as_a_visit(places):
    async with world(places) as (fake, clock, chronicle, occasions):
        await settle(fake, clock, occasions, "HOME")
        fake.nearest_waypoint = "ROAD_1"
        await occasions.poll()
        clock.t += 5
        fake.nearest_waypoint = "HOME_1"
        await occasions.poll()
        clock.t += 10
        await occasions.poll()
        assert occasions.current.id == "HOME"
        assert occasions.visited == {"HOME"}
        assert len(chronicle.facts("occasion")) == 1


async def test_every_visit_is_kept_but_only_inhabited_places_camps_and_caves_are_occasions(places):
    async with world(places) as (fake, clock, chronicle, occasions):
        for place in ("ROAD", "HOME", "CAMP", "CAVE"):
            await settle(fake, clock, occasions, place)
        assert occasions.visited == {"ROAD", "HOME", "CAMP", "CAVE"}
        assert [f["place"] for f in chronicle.facts("visited")] == ["ROAD", "HOME", "CAMP", "CAVE"]
        assert [f["place"] for f in chronicle.facts("occasion")] == ["HOME", "CAMP", "CAVE"]
        arrived = chronicle.facts("occasion")[0]
        assert arrived == {
            "fact": "occasion",
            "kind": "arrived",
            "place": "HOME",
            "waypoint": "HOME_1",
            "people": [LOBART],
            "day": 1,
            "hour": 8,
            "minute": 0,
        }
        assert sum(m["type"] == "story_people" for m in fake.received) == 1
        # story_eval needs a concrete SELF; story_state.near likewise needs HERO's actual instance.
        assert all(m["npc"] == LOBART for m in fake.received if m["type"] == "story_eval")
        assert all(m["near"] == "PC_HERO" for m in fake.received if m["type"] == "story_state")


async def test_return_and_load_keep_visits_without_repeating_or_requeueing_occasions(places):
    async with world(places) as (fake, clock, chronicle, occasions):
        await settle(fake, clock, occasions, "HOME")
        saved = chronicle.memory.head(chronicle.branch)
        original = chronicle.facts("occasion")
        await settle(fake, clock, occasions, "ROAD")
        await settle(fake, clock, occasions, "HOME")
        assert chronicle.facts("occasion") == original
        assert len(occasions.pending) == 1
        await load(fake, chronicle, head=saved)
        assert occasions.pending == []
        assert occasions.visited == {"HOME"}
        await settle(fake, clock, occasions, "HOME")
        assert chronicle.facts("occasion") == original
        assert occasions.pending == []
        assert [f["place"] for f in chronicle.facts("visited")] == ["HOME"]
        assert sum(m["type"] == "story_people" for m in fake.received) == 2


async def test_visits_after_a_save_are_new_on_its_loaded_branch(places):
    async with world(places) as (fake, clock, chronicle, occasions):
        await settle(fake, clock, occasions, "HOME")
        saved = chronicle.memory.head(chronicle.branch)
        await settle(fake, clock, occasions, "CAMP")
        await load(fake, chronicle, head=saved)
        assert occasions.visited == {"HOME"}
        await settle(fake, clock, occasions, "CAMP")
        assert [f["place"] for f in chronicle.facts("occasion")] == ["HOME", "CAMP"]
        assert kinds(occasions) == ["arrived"]


async def test_load_during_a_poll_discards_the_previous_branch_response(places, monkeypatch):
    async with world(places) as (fake, clock, chronicle, occasions):
        await settle(fake, clock, occasions, "HOME")
        saved, arrival = chronicle.memory.head(chronicle.branch), chronicle.facts("occasion")[0]
        chronicle.record({"fact": "found_out", "quest": "OLD_BRANCH"})
        state_received, resume = asyncio.Event(), asyncio.Event()
        original_state = chronicle.link.story_state

        async def delayed_state(*args, **kwargs):
            result = await original_state(*args, **kwargs)
            state_received.set()
            await resume.wait()
            return result

        monkeypatch.setattr(chronicle.link, "story_state", delayed_state)
        polling = asyncio.create_task(occasions.poll())
        try:
            await asyncio.wait_for(state_received.wait(), 1)
            await fake.send(
                type="session", branch="loaded", parent=chronicle.branch, parent_seq=saved, reason="load"
            )
            await chronicle.link.evaluate(["var", "KAPITEL"])
            assert chronicle.branch == "loaded"
            resume.set()
            await asyncio.wait_for(polling, 1)
            assert occasions.pending == []
            assert occasions.visited == {"HOME"}
            assert occasions.current is None and occasions.waypoint is None
            assert chronicle.facts("occasion") == [arrival]
            assert chronicle.facts("found_out") == []
        finally:
            resume.set()
            polling.cancel()
            await asyncio.gather(polling, return_exceptions=True)


async def test_an_unavailable_nearest_waypoint_stops_without_a_visit_or_occasion(places):
    async with world(places) as (fake, clock, chronicle, occasions):
        fake.nearest_waypoint = None
        with pytest.raises(WaypointUnavailable):
            await occasions.poll()
        assert chronicle.facts("visited") == []
        assert chronicle.facts("occasion") == []


async def test_chapter_is_baselined_then_checked_every_thirty_seconds_and_on_load(places):
    async with world(places) as (fake, clock, chronicle, occasions):
        await occasions.poll()
        assert chronicle.facts("chapter")[-1]["value"] == 1
        assert chronicle.facts("occasion") == []
        fake.vars["KAPITEL"] = 2
        clock.t += 29
        await occasions.poll()
        assert chronicle.facts("occasion") == []
        clock.t += 1
        await occasions.poll()
        assert kinds(occasions) == ["chapter"]
        assert chronicle.facts("chapter")[-1]["value"] == 2
        await occasions.poll()
        assert len(chronicle.facts("occasion")) == 1
        saved = chronicle.memory.head(chronicle.branch)
        await load(fake, chronicle, head=saved)
        await occasions.poll()
        assert occasions.pending == []
        fake.vars["KAPITEL"] = 3
        await load(fake, chronicle, branch="loaded-again")
        await occasions.poll()
        assert kinds(occasions) == ["chapter"]
        assert chronicle.facts("chapter")[-1]["value"] == 3


async def test_lower_chapter_does_not_create_an_occasion(places):
    async with world(places) as (fake, clock, chronicle, occasions):
        fake.vars["KAPITEL"] = 3
        await occasions.poll()
        fake.vars["KAPITEL"] = 2
        clock.t += 30
        await occasions.poll()
        assert chronicle.facts("occasion") == []


@pytest.mark.parametrize("kind", ["found_out", "story_quest_done", "authored_done"])
async def test_deeds_preserve_the_original_chronicle_fact(places, kind):
    async with world(places) as (fake, clock, chronicle, occasions):
        await settle(fake, clock, occasions, "HOME")
        await occasions.next()
        deed = {"fact": kind, "quest": "FIRST", "npc": LOBART}
        chronicle.record(deed)
        await occasions.poll()
        assert kinds(occasions) == ["deed"]
        result = await occasions.next()
        assert result["deed"] == deed
        assert result["people"] == [LOBART]
        assert result["place"] == "HOME"
        assert occasions.pending == []


async def test_every_vanilla_quest_change_is_an_occasion_with_the_heros_current_context(places):
    async with world(places) as (fake, clock, chronicle, occasions):
        await settle(fake, clock, occasions, "ROAD")
        started = vanilla_quest(node="DIA_LOBART_RUEBEN", npc=LOBART)
        completed = vanilla_quest(**{"from": "running", "to": "success"})
        chronicle.record(started)
        chronicle.record(completed)
        fake.nearest_waypoint = "HOME_1"
        await occasions.poll()
        assert occasions.current is None  # the event uses the live location, even before settling
        assert kinds(occasions) == ["vanilla", "vanilla"]
        result = await occasions.next()
        changes = [{k: v for k, v in result.items() if k != "also"}, *result["also"]]
        assert [f["vanilla"] for f in changes] == [started, completed]
        assert all(f["place"] == "HOME" and f["people"] == [LOBART] for f in changes)
        assert all(f["waypoint"] == "HOME_1" for f in changes)
        assert occasions.pending == []
        await load(fake, chronicle)
        await occasions.poll()
        assert occasions.pending == []
        assert chronicle.facts("occasion") == changes


async def test_vanilla_occasion_carries_latest_outcome_and_names_without_changing_event(places):
    async with world(places) as (fake, clock, chronicle, occasions):
        await settle(fake, clock, occasions, "ROAD")
        event = vanilla_quest(**{"from": "running", "to": "success"})
        started = {**event, "from": "open", "to": "running"}
        chronicle.record(started)
        chronicle.record(event)
        outcome = {
            "fact": "quest_outcome", "quest": event["quest"], "npc": LOBART,
            "how": "amended", "drop": [HILDA, "ITPL_BEET"],
            "why": ["Я снял эти пункты."], "pay": "none",
        }
        fake.item_names["ITPL_BEET"] = "Репа"
        chronicle.record({**outcome, "how": "done", "drop": [], "pay": "full"})
        chronicle.record(outcome)
        chronicle.record({**outcome, "quest": "MIS_OTHER", "how": "dropped"})
        await occasions.poll()
        result = await occasions.next()
        assert result["kind"] == "vanilla"
        assert [result["vanilla"], *(m["vanilla"] for m in result["also"])] == [
            started,
            {
                **event,
                "outcome": {
                    "how": "amended", "drop": ["Хильда", "Репа"],
                    "why": outcome["why"], "pay": "none",
                },
            },
        ]
        assert chronicle.facts("vanilla_quest") == [started, event]
        assert "outcome" not in started and "outcome" not in event


async def test_only_a_person_killed_by_the_hero_is_a_deed(places):
    async with world(places) as (fake, clock, chronicle, occasions):
        await occasions.poll()
        ignored = [
            {"npc": LOBART, "killer": "WOLF", "tag": None},
            {"npc": "WOLF", "killer": "PLAYER", "tag": "wolves", "monster": True},
            {"npc": "UNKNOWN_BEAST", "killer": "PLAYER", "tag": "wolves"},
        ]
        for fields in ignored:
            chronicle.record({"fact": "died", **fields})
        await occasions.poll()
        assert chronicle.facts("occasion") == []
        human = {"fact": "died", "npc": HILDA, "killer": "PLAYER", "tag": None}
        chronicle.record(human)
        await occasions.poll()
        await occasions.poll()
        assert kinds(occasions) == ["deed"]
        assert (await occasions.next())["deed"] == human
        tagged = {"fact": "died", "npc": "HUMAN", "killer": "PLAYER", "tag": "cast", "monster": False}
        chronicle.record(tagged)
        await occasions.poll()
        await occasions.poll()
        assert (await occasions.next())["deed"] == tagged


def crime_fact(kind="theft"):
    return {
        "fact": "crime", "id": "A/deed-1", "kind": kind, "victim": LOBART,
        "item": "ITFO_BREAD", "count": 1, "day": 1, "hour": 8, "minute": 0,
    }


async def test_a_crime_is_a_deed_with_its_id(places):
    async with world(places, crimes=True) as (_, _, chronicle, occasions):
        deed = crime_fact()
        chronicle.record(deed)
        await occasions.poll()
        [result] = occasions.pending
        assert result["kind"] == "deed" and result["crime"] == deed["id"]
        assert result["deed"] == deed


async def test_npc_deed_has_one_occasion_per_case_including_after_load(places):
    async with world(places) as (fake, _, chronicle, occasions):
        await occasions.poll()
        deed = {**crime_fact(), "fact": "npc_crime", "culprit": VINO}
        for _ in range(2):
            chronicle.record(deed)
        await occasions.poll()
        result = await occasions.next()
        assert result["kind"] == "deed" and result["deed"] == deed
        assert result["crime"] == deed["id"]
        assert len(chronicle.facts("occasion")) == 1
        await load(fake, chronicle)
        chronicle.record(deed)
        await occasions.poll()
        assert occasions.pending == []
        assert len(chronicle.facts("occasion")) == 1
        # A separate case still has its own occasion.
        chronicle.record({**deed, "id": "A/next-case"})
        await occasions.poll()
        assert len(chronicle.facts("occasion")) == 2


async def test_npc_breaches_exposure_and_betrayal_are_context_not_occasions(places):
    async with world(places) as (_, _, chronicle, occasions):
        await occasions.poll()
        for kind in ("npc_broken", "exposed", "betrayed"):
            fact = {"fact": kind, "npc": LOBART, "liar": HILDA, "deal": "A/deal"}
            assert not occasions._is_deed(fact)
            assert occasions._npc_deed_key(fact) is None
            chronicle.record(fact)
        await occasions.poll()
        assert occasions.pending == []
        assert chronicle.facts("occasion") == []


@pytest.mark.parametrize("how", ["unpaid", "no_show"])
async def test_broken_npc_promise_never_queues_a_deed_including_after_load(places, how):
    async with world(places) as (fake, _, chronicle, occasions):
        await occasions.poll()
        failure = {"fact": "npc_broken", "npc": VINO, "deal": "A/deal", "how": how}
        chronicle.record(failure)
        assert not occasions._events
        await occasions.poll()
        await load(fake, chronicle)
        chronicle.record(failure)
        await occasions.poll()
        assert occasions.pending == []
        assert chronicle.facts("occasion") == []


async def test_no_crimes_disables_npc_deed_occasions(places):
    async with world(places, crimes=False) as (_, _, chronicle, occasions):
        chronicle.record({**crime_fact(), "fact": "npc_crime", "culprit": VINO})
        chronicle.record({"fact": "npc_broken", "npc": VINO, "deal": "A/deal", "how": "unpaid"})
        await occasions.poll()
        assert occasions.pending == []


async def test_npc_deed_source_can_be_disabled_separately(places):
    async with world(places, npc_crimes=False) as (_, _, chronicle, occasions):
        chronicle.record({"fact": "npc_crime", "id": "A/crime", "culprit": VINO})
        chronicle.record(crime_fact())
        await occasions.poll()
        assert len(occasions.pending) == 1
        assert occasions.pending[0]["deed"]["fact"] == "crime"


@pytest.mark.parametrize("death_first", [False, True])
async def test_a_murder_and_its_death_have_one_crime_occasion(places, death_first):
    async with world(places, crimes=True) as (_, _, chronicle, occasions):
        crime = crime_fact("murder")
        death = {"fact": "died", "npc": LOBART, "killer": "PLAYER", "monster": False}
        for fact in ([death, crime] if death_first else [crime, death]):
            chronicle.record(fact)
        await occasions.poll()
        await occasions.poll()
        [result] = chronicle.facts("occasion")
        assert result["crime"] == crime["id"] and result["deed"] == crime
        assert chronicle.facts("died") == [death]


async def test_a_poll_between_death_and_crime_does_not_duplicate_the_occasion(places):
    async with world(places, crimes=True) as (_, _, chronicle, occasions):
        chronicle.record({"fact": "died", "npc": LOBART, "killer": "PLAYER", "monster": False})
        await occasions.poll()
        assert occasions.pending == []
        crime = crime_fact("murder")
        chronicle.record(crime)
        await occasions.poll()
        [result] = chronicle.facts("occasion")
        assert result["crime"] == crime["id"] and result["deed"] == crime


async def test_an_unpaired_death_falls_back_on_the_next_sample(places):
    async with world(places, crimes=True) as (_, _, chronicle, occasions):
        death = {"fact": "died", "npc": LOBART, "killer": "PLAYER", "monster": False}
        chronicle.record(death)
        await occasions.poll()
        assert occasions.pending == []
        await occasions.poll()
        [result] = occasions.pending
        assert result["deed"] == death and "crime" not in result


async def test_no_crimes_ignores_crime_facts_and_keeps_immediate_death_deeds(places):
    async with world(places, crimes=False) as (_, _, chronicle, occasions):
        chronicle.record(crime_fact("murder"))
        await occasions.poll()
        assert occasions.pending == []
        death = {"fact": "died", "npc": LOBART, "killer": "PLAYER", "monster": False}
        chronicle.record(death)
        await occasions.poll()
        [result] = occasions.pending
        assert result["deed"] == death and "crime" not in result


async def test_live_death_messages_keep_the_bridge_human_or_monster_distinction(places):
    async with world(places) as (fake, clock, chronicle, occasions):
        await occasions.poll()
        await fake.send(type="hero_kill", npc="WOLF", monster=True)
        await fake.send(type="npc_died", npc="WOLF", tag="wolves", killer="PLAYER")
        await chronicle.link.story_state()
        await occasions.poll()
        assert chronicle.facts("occasion") == []
        await fake.send(type="hero_kill", npc=LOBART, monster=False)
        await fake.send(type="npc_died", npc=LOBART, tag="cast", killer="PLAYER")
        await chronicle.link.story_state()
        await occasions.poll()
        await occasions.poll()
        assert kinds(occasions) == ["deed"]
        assert (await occasions.next())["deed"]["npc"] == LOBART


async def test_quiet_counts_a_full_game_day_including_hours_and_minutes(places):
    async with world(places) as (fake, clock, chronicle, occasions):
        fake.day, fake.hour, fake.minute = 4, 23, 45
        await settle(fake, clock, occasions, "ROAD")
        fake.day, fake.hour, fake.minute = 5, 23, 44
        await occasions.poll()
        assert chronicle.facts("occasion") == []
        fake.minute = 45
        await occasions.poll()
        assert kinds(occasions) == ["quiet"]
        assert chronicle.facts("occasion")[-1]["day"] == 5
        assert chronicle.facts("occasion")[-1]["hour"] == 23
        await occasions.poll()
        assert len(chronicle.facts("occasion")) == 1


@pytest.mark.parametrize("kind", ["story_quest", "chain", "authored"])
async def test_a_new_director_quest_restarts_the_quiet_day(places, kind):
    async with world(places) as (fake, clock, chronicle, occasions):
        await settle(fake, clock, occasions, "ROAD")
        fake.hour = 20
        await occasions.poll()
        chronicle.record({"fact": kind, "id": "NEW_LINE"})
        await occasions.poll()
        fake.day, fake.hour = 2, 8
        await occasions.poll()
        assert chronicle.facts("occasion") == []
        fake.hour = 20
        await occasions.poll()
        assert kinds(occasions) == ["quiet"]


async def test_quiet_clock_survives_a_load_and_any_occasion_restarts_it(places):
    async with world(places) as (fake, clock, chronicle, occasions):
        await settle(fake, clock, occasions, "ROAD")
        fake.hour = 20
        await occasions.poll()
        await load(fake, chronicle)
        await settle(fake, clock, occasions, "ROAD")
        fake.day, fake.hour = 2, 8
        await occasions.poll()
        assert kinds(occasions) == ["quiet"]
        chronicle.record({"fact": "found_out", "quest": "LIE"})
        await occasions.poll()
        fake.day, fake.hour = 3, 7
        await occasions.poll()
        assert len(chronicle.facts("occasion")) == 2
        fake.hour = 8
        await occasions.poll()
        assert [f["kind"] for f in chronicle.facts("occasion")] == ["quiet", "deed", "quiet"]


async def test_the_single_take_call_bundles_priority_order_and_consumes_once(places):
    async with world(places) as (fake, clock, chronicle, occasions):
        await settle(fake, clock, occasions, "ROAD")
        fake.day += 1
        await occasions.poll()
        await settle(fake, clock, occasions, "HOME")
        fake.vars["KAPITEL"] = 2
        clock.t += 30
        await occasions.poll()
        chronicle.record(vanilla_quest())
        chronicle.record({"fact": "found_out", "quest": "LIE"})
        await occasions.poll()
        expected = ["deed", "vanilla", "chapter", "arrived", "quiet"]
        assert kinds(occasions) == expected
        result = await occasions.next()
        assert [result["kind"], *(f["kind"] for f in result["also"])] == expected
        assert occasions.pending == []
        assert len(chronicle.facts("occasion")) == 5


@pytest.mark.parametrize("kind", ["arrived", "quiet"])
async def test_local_occasions_expire_only_after_leaving_more_than_fifty_metres(places, kind):
    async with world(places) as (fake, clock, chronicle, occasions):
        if kind == "arrived":
            await settle(fake, clock, occasions, "HOME")
            origin, destination = "HOME", "ROAD"
        else:
            await settle(fake, clock, occasions, "ROAD")
            fake.day += 1
            await occasions.poll()
            origin, destination = "ROAD", "HOME"
        fake.waypoint_distances[f"{origin}_1"] = 5000
        fake.waypoint_distances[f"{origin}_2"] = 5000
        fake.nearest_waypoint = f"{destination}_1"
        await occasions.poll()
        assert kind in kinds(occasions)
        fake.waypoint_distances[f"{origin}_1"] = 5001
        fake.waypoint_distances[f"{origin}_2"] = 6000
        await occasions.poll()
        assert kind not in kinds(occasions)
        assert any(f["kind"] == kind for f in chronicle.facts("occasion"))  # history stays


async def test_every_kind_expires_after_a_game_day_but_deeds_vanilla_and_chapters_survive_distance(places):
    async with world(places) as (fake, clock, chronicle, occasions):
        await settle(fake, clock, occasions, "HOME")
        fake.vars["KAPITEL"] = 2
        clock.t += 30
        await occasions.poll()
        chronicle.record(vanilla_quest())
        chronicle.record({"fact": "found_out", "quest": "LIE"})
        await occasions.poll()
        fake.nearest_waypoint = "ROAD_1"
        fake.waypoint_distances.update(HOME_1=100000, HOME_2=100000)
        await occasions.poll()
        assert kinds(occasions) == ["deed", "vanilla", "chapter"]
        clock.t += 10
        await occasions.poll()
        fake.day += 1
        await occasions.poll()
        assert kinds(occasions) == ["quiet"]


@pytest.mark.parametrize("expired_by", ["distance", "time"])
async def test_next_refreshes_freshness_before_handoff_without_waiting_for_the_watcher(places, expired_by):
    async with world(places) as (fake, clock, chronicle, occasions):
        await settle(fake, clock, occasions, "HOME")
        if expired_by == "distance":
            fake.nearest_waypoint = "ROAD_1"
            fake.waypoint_distances.update(HOME_1=5001, HOME_2=5001)
        else:
            fake.day += 1
        taking = asyncio.create_task(occasions.next())
        try:
            await asyncio.sleep(0.03)
            assert not taking.done()
            assert occasions.pending == []
            chronicle.record({"fact": "found_out", "quest": "LIE"})
            await occasions.poll()
            assert (await asyncio.wait_for(taking, 1))["kind"] == "deed"
        finally:
            taking.cancel()
            await asyncio.gather(taking, return_exceptions=True)


@pytest.mark.parametrize("busy", ["in_dialog", "in_cutscene"])
async def test_next_waits_while_the_director_is_busy_then_takes_the_fresh_best(places, busy):
    async with world(places) as (fake, clock, chronicle, occasions):
        await settle(fake, clock, occasions, "HOME")
        setattr(fake, busy, True)
        taking = asyncio.create_task(occasions.next())
        try:
            await asyncio.sleep(0.03)
            assert not taking.done()
            chronicle.record({"fact": "found_out", "quest": "LIE"})
            await occasions.poll()
            setattr(fake, busy, False)
            result = await asyncio.wait_for(taking, 1)
            assert result["kind"] == "deed"
            assert [f["kind"] for f in result["also"]] == ["arrived"]
            assert occasions.pending == []
        finally:
            taking.cancel()
            await asyncio.gather(taking, return_exceptions=True)


async def test_next_can_wait_for_the_first_occasion(places):
    async with world(places) as (fake, clock, chronicle, occasions):
        await occasions.poll()
        taking = asyncio.create_task(occasions.next())
        try:
            await asyncio.sleep(0.03)
            assert not taking.done()
            chronicle.record({"fact": "found_out", "quest": "LIE"})
            await occasions.poll()
            assert (await asyncio.wait_for(taking, 1))["kind"] == "deed"
            assert occasions.pending == []
        finally:
            taking.cancel()
            await asyncio.gather(taking, return_exceptions=True)


async def test_only_a_truthful_accepted_script_quest_claim_is_a_deed(places):
    async with world(places) as (fake, clock, chronicle, occasions):
        await occasions.poll()
        for fields in ({"truth": False, "believed": True}, {"truth": True, "believed": False}, {}):
            chronicle.record({"fact": "claim", "quest": "SCRIPT", **fields})
        await occasions.poll()
        assert occasions.pending == []
        claim = {"fact": "claim", "quest": "SCRIPT", "truth": True, "believed": True}
        chronicle.record(claim)
        await occasions.poll()
        assert (await occasions.next())["deed"] == claim


async def test_a_quest_saved_before_the_next_sample_still_resets_quiet(places):
    async with world(places) as (fake, clock, chronicle, occasions):
        await settle(fake, clock, occasions, "ROAD")
        fake.hour = 20
        chronicle.record({"fact": "story_quest", "id": "NEW_LINE"})
        await load(fake, chronicle)  # no poll between making the quest and the save
        await settle(fake, clock, occasions, "ROAD")
        fake.day, fake.hour = 2, 8
        await occasions.poll()
        assert occasions.pending == []
        fake.hour = 20
        await occasions.poll()
        assert kinds(occasions) == ["quiet"]


async def test_next_retries_a_transient_game_error_without_losing_the_occasion(places, monkeypatch):
    async with world(places) as (fake, clock, chronicle, occasions):
        await settle(fake, clock, occasions, "HOME")
        original = chronicle.link.story_state
        calls = 0

        async def interrupted(**kwargs):
            nonlocal calls
            calls += 1
            if calls == 1:
                raise GameError("loading")
            return await original(**kwargs)

        monkeypatch.setattr(chronicle.link, "story_state", interrupted)
        assert (await asyncio.wait_for(occasions.next(), 1))["kind"] == "arrived"
        assert calls == 2 and occasions.pending == []


async def test_next_passes_an_unexpected_handoff_failure_with_the_consumed_occasion(places, monkeypatch):
    async with world(places) as (fake, clock, chronicle, occasions):
        await settle(fake, clock, occasions, "HOME")
        [pending] = occasions.pending

        async def broken(**kwargs):
            raise RuntimeError("handoff broken")

        monkeypatch.setattr(chronicle.link, "story_state", broken)
        with pytest.raises(OccasionFailed) as caught:
            await occasions.next()
        assert caught.value.occasion == pending
        assert isinstance(caught.value.error, RuntimeError)
        assert occasions.pending == []


async def test_next_gathers_for_ten_seconds_after_the_latest_occasion_by_default(places):
    async with world(places, gather=True) as (fake, clock, chronicle, occasions):
        assert occasions.gather_s == 10 and occasions.max_gather_s == 30
        await settle(fake, clock, occasions, "HOME")
        arrival = dict(occasions.pending[0])
        calls = sum(m["type"] in {"story_state", "story_eval"} for m in fake.received)
        taking = asyncio.create_task(occasions.next())
        try:
            await asyncio.sleep(0.03)
            assert not taking.done()
            assert sum(m["type"] in {"story_state", "story_eval"} for m in fake.received) == calls
            clock.t += 5
            chronicle.record({"fact": "found_out", "quest": "LIE"})
            await occasions.poll()
            clock.t += 9.99
            await occasions.poll()
            await asyncio.sleep(0.03)
            assert not taking.done()
            clock.t += 0.01
            await occasions.poll()
            result = await asyncio.wait_for(taking, 1)
            assert result["kind"] == "deed"
            assert result["also"] == [arrival]
            assert occasions.pending == []
            assert all("also" not in f for f in chronicle.facts("occasion"))
        finally:
            taking.cancel()
            await asyncio.gather(taking, return_exceptions=True)


async def test_continuous_occasions_are_handed_off_at_the_thirty_second_limit(places):
    async with world(places, gather=True) as (fake, clock, chronicle, occasions):
        await settle(fake, clock, occasions, "HOME")
        started = clock.t
        taking = asyncio.create_task(occasions.next())
        try:
            for number in range(1, 7):
                clock.t += 5
                chronicle.record({"fact": "found_out", "quest": str(number)})
                await occasions.poll()
                if number < 6:
                    await asyncio.sleep(0.03)
                    assert not taking.done()
            result = await asyncio.wait_for(taking, 1)
            assert clock.t - started == occasions.max_gather_s
            assert result["deed"]["quest"] == "1"
            assert len(result["also"]) == 6
            assert result["also"][-1]["kind"] == "arrived"
            assert occasions.pending == []
        finally:
            taking.cancel()
            await asyncio.gather(taking, return_exceptions=True)


async def test_a_closed_budget_keeps_the_arrival_without_handoff_requests(places):
    async with world(places) as (fake, clock, chronicle, occasions):
        await settle(fake, clock, occasions, "HOME")
        [arrival] = occasions.pending
        budget_open = False
        checked = asyncio.Event()

        async def ready():
            checked.set()
            return budget_open

        calls = sum(m["type"] in {"story_state", "story_eval"} for m in fake.received)
        taking = asyncio.create_task(occasions.next(ready=ready))
        try:
            await asyncio.wait_for(checked.wait(), 1)
            await asyncio.sleep(0.03)
            assert not taking.done()
            assert occasions.pending == [arrival]
            assert sum(m["type"] in {"story_state", "story_eval"} for m in fake.received) == calls
            clock.t += 300
            budget_open = True
            assert await asyncio.wait_for(taking, 1) == arrival
            assert occasions.pending == []
        finally:
            taking.cancel()
            await asyncio.gather(taking, return_exceptions=True)


@pytest.mark.parametrize("expired_by", ["distance", "time"])
async def test_the_watcher_expires_an_arrival_while_the_budget_is_closed(places, expired_by):
    async with world(places) as (fake, clock, chronicle, occasions):
        await settle(fake, clock, occasions, "HOME")
        budget_open = False
        checked = asyncio.Event()

        async def ready():
            checked.set()
            return budget_open

        taking = asyncio.create_task(occasions.next(ready=ready))
        try:
            await asyncio.wait_for(checked.wait(), 1)
            if expired_by == "distance":
                fake.nearest_waypoint = "ROAD_1"
                fake.waypoint_distances.update(HOME_1=5001, HOME_2=5001)
            else:
                fake.day += 1
            await occasions.poll()
            assert "arrived" not in kinds(occasions)
            assert not taking.done()
            chronicle.record({"fact": "found_out", "quest": "FRESH"})
            await occasions.poll()
            budget_open = True
            result = await asyncio.wait_for(taking, 1)
            assert result["deed"]["quest"] == "FRESH"
            assert all(f["kind"] != "arrived" for f in result.get("also", []))
        finally:
            taking.cancel()
            await asyncio.gather(taking, return_exceptions=True)


async def test_load_while_the_budget_check_waits_cannot_take_the_old_branch(places):
    async with world(places) as (fake, clock, chronicle, occasions):
        await settle(fake, clock, occasions, "HOME")
        checking, resume = asyncio.Event(), asyncio.Event()

        async def ready():
            checking.set()
            await resume.wait()
            return True

        taking = asyncio.create_task(occasions.next(ready=ready))
        try:
            await asyncio.wait_for(checking.wait(), 1)
            await load(fake, chronicle)
            resume.set()
            await asyncio.sleep(0.03)
            assert not taking.done()
            assert occasions.pending == []
            chronicle.record({"fact": "found_out", "quest": "NEW_BRANCH"})
            await occasions.poll()
            result = await asyncio.wait_for(taking, 1)
            assert result["deed"]["quest"] == "NEW_BRANCH"
            assert "also" not in result
        finally:
            resume.set()
            taking.cancel()
            await asyncio.gather(taking, return_exceptions=True)


async def test_an_unexpected_handoff_failure_consumes_the_whole_bundle(places, monkeypatch):
    async with world(places) as (fake, clock, chronicle, occasions):
        await settle(fake, clock, occasions, "HOME")
        chronicle.record({"fact": "found_out", "quest": "LIE"})
        await occasions.poll()
        deed, arrival = occasions.pending

        async def broken(**kwargs):
            raise RuntimeError("handoff broken")

        monkeypatch.setattr(chronicle.link, "story_state", broken)
        with pytest.raises(OccasionFailed) as caught:
            await occasions.next()
        assert caught.value.occasion == {**deed, "also": [arrival]}
        assert occasions.pending == []
        assert "also" not in deed and "also" not in arrival
