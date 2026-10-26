"""A showrunner spends the save branch's budget before asking its model or builders."""

import asyncio
import json
from contextlib import asynccontextmanager
from copy import deepcopy
from dataclasses import dataclass
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest

from gothic_ai.cast import Cast, Person
from gothic_ai.characters import Card
from gothic_ai.crime_law import CrimeLaw
from gothic_ai.crimes import Crimes
from gothic_ai.llm import ToolCall
from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.memory import MemoryStore
from gothic_ai.occasions import Occasions
from gothic_ai.places import PlaceBook
from gothic_ai.protocol import Loading, NpcSpec, RoutineEntry, Session
from gothic_ai.questbook import Goal, Quest, Stage, Talk
from gothic_ai.showrunner import (
    CACHE_KEY,
    MAX_LINES_PER_CHAPTER,
    MAX_OPEN_LINES,
    MAX_PEOPLE_PER_CHAPTER,
    MIN_GAME_MINUTES,
    MIN_REAL_SECONDS,
    PROPOSE,
    PROPOSE_TOOL,
    Proposal,
    Showrunner,
)
from gothic_ai.story import QUEST as QUEST_RECORD
from tests import test_authoring
from tests.fake_game import FakeGame
from tests.fakes import Clock
from tests.test_authoring import BOOK, _author
from tests.test_director import HILDA, LOBART, NAMES, VINO, XARDAS
from tests.test_director import QUEST as SCRIPT_QUEST
from tests.test_memory import close_memory_stores as close_memory_stores
from tests.test_occasions import load

HOME, YARD, FIELD = "FARM1_INHOUSE", "FARM1_ENTRANCE", "FARM1_FIELD"
PREMISE = "Ночью пропала хозяйская пила. Сегодня работа встала, а до дождя надо починить забор."
OWN = "DIR_NPC_1"
DEAD = "BAU_999_DEAD"
graph = test_authoring.graph


def proposal(**change):
    return {
        "kind": "quest",
        "giver": HILDA,
        "place": "",
        "new_people": False,
        "premise": PREMISE,
        "reason": "Герой пришёл как раз к началу работы.",
        **change,
    }


def occasion(**change):
    return {
        "fact": "occasion",
        "kind": "arrived",
        "place": HOME,
        "waypoint": "NW_FARM1_INHOUSE_01",
        "people": [HILDA, LOBART],
        "day": 10,
        "hour": 8,
        "minute": 0,
        **change,
    }


def line(**change):
    return {
        "fact": "line",
        "occasion": occasion(day=1),
        "kind": "quest",
        "giver": HILDA,
        "premise": "Старое поручение хозяйки.",
        "quests": ["DIR_Q1"],
        "characters": [],
        "chapter": 1,
        "day": 1,
        "hour": 8,
        "minute": 0,
        "wall_time": 1000.0,
        **change,
    }


def quest(id="DIR_Q1", giver=HILDA, title="Пропавшая пила"):
    talk = Talk("Нужна помощь?", [{"speaker": "self", "text": "Найди мою пилу."}])
    return Quest(
        id=id,
        giver=giver,
        title=title,
        give=talk,
        stages=[Stage("Надо помочь хозяйке.", [Goal("talk", npc=giver, talk=talk)])],
        done_journal="Я помог хозяйке.",
        names=dict(NAMES),
    )


def spec(npc=OWN, name="Рутгер", point="NW_FARM1_OUT_01"):
    return NpcSpec(
        npc=npc,
        name=name,
        like="VLK_4000_BUERGER",
        at=point,
        routine=[RoutineEntry(start="00:00", end="24:00", state="ZS_STAND_WP", waypoint=point)],
    )


@dataclass
class World:
    fake: FakeGame
    clock: Clock
    showrunner: Showrunner
    author: object
    chains: object
    casting: object
    llm: FakeBackend

    @property
    def chronicle(self):
        return self.showrunner.chronicle

    async def keep(self, *quests):
        await self.author.patches.make([{"op": QUEST_RECORD, "quest": q.to_dict()} for q in quests], [])

    async def finish(self, *ids, state="done"):
        suffix = {"done": "done", "failed": "failed", "obsolete": "obsolete"}[state]
        for id in ids:
            await self.chronicle.record({"fact": f"story_quest_{suffix}", "id": id})


@asynccontextmanager
async def world(story_graph, *proposals, gather_s=0):
    fake, clock = FakeGame(branch="A"), Clock()
    clock.t = 100_000.0
    llm = FakeBackend(
        script=[FakeStep("", [ToolCall(PROPOSE, p, f"line{i}")]) for i, p in enumerate(proposals)],
        delay=0,
    )
    link_cm, author, chronicle = await _author(fake, llm, story_graph, await MemoryStore.open(":memory:"))
    try:
        data = deepcopy(BOOK)
        for place, kind in zip(data["places"], ["house", "yard", "field", "street"], strict=True):
            place["kind"] = kind
        author.places = PlaceBook(data)
        author.book.places = author.places
        fake.day, fake.hour, fake.minute = 10, 8, 0
        fake.nearest_waypoint = "NW_FARM1_INHOUSE_01"
        fake.people = [
            {
                "npc": npc,
                "name": NAMES.get(npc, "Умерший крестьянин"),
                "routine": [
                    {
                        "start": 0,
                        "end": 1440,
                        "state": "ZS_STAND_WP",
                        "waypoint": "NW_FARM1_INHOUSE_01",
                    }
                ],
            }
            for npc in (HILDA, LOBART, VINO, XARDAS, DEAD)
        ]
        for npc in NAMES:
            fake.npc_states[npc]["distance"] = 200 if npc != VINO else 6000
        fake.npc_states[DEAD] = {"state": "dead", "name": "Умерший крестьянин", "distance": 100}
        author.cards[HILDA] = Card(
            HILDA,
            "Хильда",
            "Ты — хозяйка фермы. Не терпишь бездельников.",
            "A practical farmer.",
            3,
            3,
            3,
            3,
            3,
            3,
        )
        cast = Cast(author.link, author.patches, chronicle)
        await cast.prepare()
        casting = SimpleNamespace(cast=cast, character=AsyncMock())
        chains = SimpleNamespace(chain=AsyncMock())
        occasions = Occasions(
            author.link,
            chronicle,
            author.places,
            clock=clock,
            busy_poll_s=0.01,
            gather_s=gather_s,
        )
        runner = Showrunner(occasions, author, chains, casting, llm, clock=clock)
        state = World(fake, clock, runner, author, chains, casting, llm)

        async def make_chain(
            npc, *, new_people=False, premise="", max_people=None, requires=None, follows="",
            quest_count=None, crime="",
        ):
            first = quest(id=await author.book.next_id(), giver=npc)
            quests = [first]
            if quest_count != 1:
                quests.append(quest(id=await author.book.next_id({first.id}), giver=npc, title="След пилы"))
            await state.keep(*quests)
            return quests

        async def make_character(place, *, about="", premise="", **kwargs):
            person = spec(npc=await cast.next_id())
            await author.patches.introduce(person)
            fake.npc_states[person.npc] = {"state": "alive", "name": person.name, "distance": 300}
            return person

        author.quest = AsyncMock()
        chains.chain.side_effect = make_chain
        casting.character.side_effect = make_character
        yield state
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("rule", ["open", "chapter", "game_time", "wall_time"])
async def test_budget_denies_before_the_model_and_records_why(graph, rule):
    async with world(graph, proposal()) as w:
        if rule == "open":
            for i in range(MAX_OPEN_LINES):
                await w.chronicle.record(line(giver=f"GIVER_{i}", quests=[f"DIR_Q{i + 1}"]))
        elif rule == "chapter":
            for i in range(MAX_LINES_PER_CHAPTER):
                await w.chronicle.record(line(quests=[f"DIR_Q{i + 1}"]))
                await w.finish(f"DIR_Q{i + 1}")
        else:
            previous = line(
                day=9 if rule == "game_time" else 8,
                hour=8,
                wall_time=w.clock.t - (MIN_REAL_SECONDS if rule == "game_time" else MIN_REAL_SECONDS - 1),
            )
            await w.chronicle.record(previous)
            await w.finish("DIR_Q1")
            if rule == "game_time":
                w.fake.hour, w.fake.minute = 7, 59
        result = await w.showrunner.decide(occasion())
        assert result["fact"] == "line_skipped" and result["why"]
        assert w.chronicle.facts("line_skipped") == [result]
        assert result["occasion"] == occasion()
        assert w.llm.calls == []
        w.author.quest.assert_not_called()
        w.chains.chain.assert_not_called()
        w.casting.character.assert_not_called()


async def test_giver_and_people_budget_can_skip_when_no_builder_kind_fits(graph):
    async with world(graph, proposal()) as w:
        # Hilda has an unfinished line; all eight newcomers were already introduced by it.
        await w.chronicle.record(line(characters=[f"DIR_NPC_{i}" for i in range(8)]))
        for npc in (LOBART, VINO):
            w.fake.npc_states[npc]["state"] = "dead"
        budget = w.showrunner.budget(1, occasion())
        assert budget.people_left == 0 and budget.busy_givers == {HILDA}
        result = await w.showrunner.decide(occasion())
        assert result["fact"] == "line_skipped" and result["why"]
        assert "нет свободных живых заказчиков" in str(result["why"])
        assert w.chronicle.facts("line_skipped") == [result]
        assert w.llm.calls == []


async def arrive(w):
    occasions = w.showrunner.occasions
    await occasions.poll()
    w.clock.t += occasions.settle_s
    await occasions.poll()
    [arrival] = occasions.pending
    return arrival


async def close_budget(w, rule):
    if rule == "open":
        for i in range(MAX_OPEN_LINES):
            await w.chronicle.record(line(giver=f"GIVER_{i}", quests=[f"DIR_Q{i + 1}"]))
    elif rule == "chapter":
        for i in range(MAX_LINES_PER_CHAPTER):
            await w.chronicle.record(line(quests=[f"DIR_Q{i + 1}"]))
            await w.finish(f"DIR_Q{i + 1}")
    else:
        await w.chronicle.record(
            line(
                day=9 if rule == "game_time" else 8,
                minute=1 if rule == "game_time" else 0,
                wall_time=w.clock.t - MIN_REAL_SECONDS + (1 if rule == "wall_time" else 0),
            )
        )
        await w.finish("DIR_Q1")
    # Completing old test quests is setup, not another new occasion for the hero.
    w.showrunner.occasions._events.clear()


@pytest.mark.parametrize("rule", ["open", "chapter", "game_time", "wall_time"])
async def test_run_keeps_an_arrival_until_its_budget_opens(graph, monkeypatch, rule):
    async with world(graph, proposal(kind="none")) as w:
        arrival = await arrive(w)
        await close_budget(w, rule)
        checked, finished = asyncio.Event(), asyncio.Event()
        original_ready = w.showrunner._ready

        async def ready():
            result = await original_ready()
            checked.set()
            return result

        monkeypatch.setattr(w.showrunner, "_ready", ready)
        w.chronicle.on_fact.append(lambda f: finished.set() if f.get("fact") == "line_none" else None)
        running = asyncio.create_task(w.showrunner.run())
        try:
            await asyncio.wait_for(checked.wait(), 1)
            assert w.showrunner.occasions.pending == [arrival]
            assert w.chronicle.facts("line_skipped") == [] and w.llm.calls == []
            if rule == "open":
                await w.finish(*(f"DIR_Q{i + 1}" for i in range(MAX_OPEN_LINES)))
            elif rule == "chapter":
                w.fake.vars["KAPITEL"] = 2
            elif rule == "game_time":
                w.fake.minute = 1
            w.clock.t += 1 if rule == "wall_time" else w.showrunner.occasions.poll_s
            w.showrunner.occasions._changed.set()
            await asyncio.wait_for(finished.wait(), 1)
            [result] = w.chronicle.facts("line_none")
            assert result["occasion"] == arrival
            assert w.showrunner.occasions.pending == []
            assert len(w.llm.calls) == 1 and w.chronicle.facts("line_skipped") == []
        finally:
            running.cancel()
            await asyncio.gather(running, return_exceptions=True)


@pytest.mark.parametrize("expired_by", ["moving", "game_day"])
async def test_an_arrival_expires_while_the_run_waits_for_budget(graph, monkeypatch, expired_by):
    async with world(graph, proposal(kind="none")) as w:
        arrival = await arrive(w)
        await close_budget(w, "open")
        checked = asyncio.Event()
        original_ready = w.showrunner._ready

        async def ready():
            result = await original_ready()
            checked.set()
            return result

        monkeypatch.setattr(w.showrunner, "_ready", ready)
        running = asyncio.create_task(w.showrunner.run())
        try:
            await asyncio.wait_for(checked.wait(), 1)
            assert arrival in w.showrunner.occasions.pending
            if expired_by == "moving":
                w.fake.nearest_waypoint = "NW_CITY_01"
                w.fake.waypoint_distances["NW_FARM1_INHOUSE_01"] = 90000
            else:
                w.fake.day += 1
            w.clock.t += w.showrunner.occasions.poll_s
            await w.showrunner.occasions.poll()
            assert arrival not in w.showrunner.occasions.pending
            assert w.chronicle.facts("line_skipped") == [] and w.llm.calls == []
        finally:
            running.cancel()
            await asyncio.gather(running, return_exceptions=True)


async def test_budget_readiness_uses_one_state_per_poll_and_counts_real_wait_locally(graph, monkeypatch):
    async with world(graph) as w:
        await close_budget(w, "wall_time")
        state = AsyncMock(wraps=w.showrunner._state)
        monkeypatch.setattr(w.showrunner, "_state", state)
        assert not await w.showrunner._ready()
        for _ in range(25):
            w.clock.t += 0.02
            assert not await w.showrunner._ready()
        assert state.await_count == 1
        w.clock.t += 0.5
        assert await w.showrunner._ready()
        assert state.await_count == 1
        w.clock.t += w.showrunner.occasions.poll_s
        assert await w.showrunner._ready()
        assert state.await_count == 2


@pytest.mark.parametrize("loading", [False, True])
async def test_budget_state_cache_is_reset_by_a_session_or_loading(graph, monkeypatch, loading):
    async with world(graph) as w:
        state = AsyncMock(wraps=w.showrunner._state)
        monkeypatch.setattr(w.showrunner, "_state", state)
        assert await w.showrunner._ready()
        assert state.await_count == 1
        if loading:
            await w.showrunner.link._dispatch(Loading(type="loading"))
        else:
            branch = w.chronicle.branch
            await w.showrunner.link._dispatch(
                Session(
                    type="session",
                    branch="loaded",
                    parent=branch,
                    parent_seq=await w.chronicle.memory.head(branch),
                    reason="load",
                )
            )
        await w.showrunner._ready()
        assert state.await_count == 2


async def test_budget_readiness_waits_for_another_decision_without_reading_the_bridge(graph, monkeypatch):
    async with world(graph) as w:
        state = AsyncMock(wraps=w.showrunner._state)
        monkeypatch.setattr(w.showrunner, "_state", state)
        async with w.showrunner._lock:
            assert not await w.showrunner._ready()
            state.assert_not_awaited()
        assert await w.showrunner._ready()
        state.assert_awaited_once()


async def test_run_returns_its_bundle_when_another_writer_spends_budget_after_handoff(graph, monkeypatch):
    async with world(graph, proposal(kind="none"), gather_s=10) as w:
        occasions = w.showrunner.occasions
        arrival = await arrive(w)
        deed = {"fact": "found_out", "reason": "Герой раскрыл обман с пилой."}
        w.clock.t += 5
        await w.chronicle.record(deed)
        await occasions.poll()
        w.clock.t += occasions.gather_s
        originals = list(occasions.pending)
        assert originals[0]["kind"] == "deed" and originals[1] == arrival
        recorded = w.chronicle.facts("occasion")
        handed_off, resume, returned, finished = (asyncio.Event() for _ in range(4))
        original_next, original_put_back = occasions.next, occasions.put_back
        first = True

        async def next_occasion(**kwargs):
            nonlocal first
            bundle = await original_next(**kwargs)
            if first:
                first = False
                handed_off.set()
                await resume.wait()
            return bundle

        def put_back(bundle):
            original_put_back(bundle)
            returned.set()

        monkeypatch.setattr(occasions, "next", next_occasion)
        monkeypatch.setattr(occasions, "put_back", put_back)
        w.chronicle.on_fact.append(lambda f: finished.set() if f.get("fact") == "line_none" else None)
        running = asyncio.create_task(w.showrunner.run())
        try:
            await asyncio.wait_for(handed_off.wait(), 1)
            assert occasions.pending == []
            # Another writer commits its receipts after handoff's readiness check. Both cooldowns
            # are near their boundary, so reopening them will leave the arrival fresh.
            async with w.showrunner._lock:
                await w.chronicle.record(line(day=9, minute=1, wall_time=w.clock.t - MIN_REAL_SECONDS + 1))
                await w.finish("DIR_Q1")
                resume.set()
            await asyncio.wait_for(returned.wait(), 1)
            assert occasions.pending == originals
            assert w.chronicle.facts("occasion") == recorded
            assert w.chronicle.facts("line_skipped") == [] and w.llm.calls == []
            w.fake.minute = 1
            # Refresh the game minute before a new gather interval could have elapsed.
            w.clock.t += occasions.poll_s
            occasions._changed.set()
            await asyncio.wait_for(finished.wait(), 1)
            [result] = w.chronicle.facts("line_none")
            assert result["occasion"] == {**originals[0], "also": [arrival]}
            assert occasions.pending == [] and len(w.llm.calls) == 1
            assert w.chronicle.facts("line_skipped") == []
            assert w.chronicle.facts("occasion") == recorded
        finally:
            resume.set()
            running.cancel()
            await asyncio.gather(running, return_exceptions=True)


async def test_both_cooldown_boundaries_allow_a_model_call(graph):
    async with world(graph, proposal(kind="none")) as w:
        previous = line(day=9, wall_time=w.clock.t - MIN_REAL_SECONDS)
        await w.chronicle.record(previous)
        await w.finish("DIR_Q1")
        assert MIN_GAME_MINUTES == 24 * 60
        assert w.showrunner.budget(1, occasion()).reasons == []
        result = await w.showrunner.decide(occasion())
        assert result["fact"] == "line_none" and len(w.llm.calls) == 1


@pytest.mark.parametrize("state", ["open", "given", "done", "failed", "obsolete"])
async def test_a_line_stays_open_until_every_quest_is_finished(graph, state):
    async with world(graph) as w:
        await w.keep(quest(), quest(id="DIR_Q2"))
        await w.finish("DIR_Q1")
        if state != "open":
            suffix = {"given": "given", "done": "done", "failed": "failed", "obsolete": "obsolete"}[state]
            await w.chronicle.record({"fact": f"story_quest_{suffix}", "id": "DIR_Q2"})
        assert w.showrunner.is_open(line(quests=["DIR_Q1", "DIR_Q2"])) is (state in {"open", "given"})
        assert not w.showrunner.is_open(line(quests=[]))


async def test_budget_is_chapter_local_but_open_lines_and_cooldown_cross_chapters(graph):
    async with world(graph) as w:
        await w.chronicle.record(line(chapter=1, characters=[f"DIR_NPC_{i}" for i in range(8)]))
        budget = w.showrunner.budget(2, occasion())
        assert budget.open_left == MAX_OPEN_LINES - 1
        assert budget.lines_left == MAX_LINES_PER_CHAPTER
        assert budget.people_left == MAX_PEOPLE_PER_CHAPTER
        assert budget.busy_givers == {HILDA}
        await w.chronicle.record(line(chapter=2, quests=[], characters=[OWN, "DIR_NPC_2", "DIR_NPC_3"]))
        await w.chronicle.record(line(chapter=2, quests=[], characters=[OWN]))
        assert w.showrunner.budget(2, occasion()).people_left == MAX_PEOPLE_PER_CHAPTER - 3


async def test_brief_has_local_cards_time_quests_deeds_history_and_only_free_living_candidates(graph):
    async with world(graph) as w:
        await w.keep(quest(title="Уже было"))
        await w.keep(quest(id="DIR_Q2", title="Старое поручение строителя"))
        await w.finish("DIR_Q2")
        await w.chronicle.record(line(characters=[OWN]))
        await w.chronicle.record({"fact": "story_quest_given", "id": "DIR_Q1"})
        await w.chronicle.record({"fact": "story_quest_stage", "id": "DIR_Q1", "stage": 2})
        w.fake.vars[SCRIPT_QUEST] = 1
        newcomer = spec().model_copy(update={"routine": []})
        person = Person(
            ru="Ты — пришлый торговец, осторожный и терпеливый.",
            en="A patient travelling merchant.",
            greed=3,
            honesty=3,
            courage=3,
            talk=3,
            pushy=3,
            strangers=3,
            voice="A calm middle aged man with a low voice.",
        )
        await w.author.patches.introduce(newcomer, person.model_dump())
        await w.casting.cast.prepare()
        w.fake.npc_states[OWN] = {"state": "alive", "name": newcomer.name, "distance": 300}
        deed = {"fact": "found_out", "quest": "MIS_LIE", "npc": LOBART, "reason": "Герой солгал."}
        await w.chronicle.record(deed)
        current = occasion(kind="deed", deed=deed)
        brief = await w.showrunner.brief(current)
        assert brief.occasion == current
        assert (brief.chapter, brief.day, brief.hour, brief.minute) == (1, 10, 8, 0)
        assert set(brief.candidates) == {LOBART, OWN}
        assert not {HILDA, XARDAS, VINO, DEAD} & brief.candidates.keys()
        assert {HOME, YARD, FIELD} <= brief.places.keys() and "CITY" not in brief.places
        text = brief.text()
        for expected in (
            "deed",
            HOME,
            "дом",
            "Хильда",
            "Не терпишь бездельников",
            "Убрать поле репы",
            "Уже было",
            "MIS_LIE",
            "Герой солгал",
            "Рутгер",
            "пришлый торговец",
        ):
            assert expected in text
        assert deed in brief.deeds
        assert any("Уже было" in q and "2" in q for q in brief.quests)
        assert any("Уже было" in item for item in brief.history)
        assert any("Старое поручение строителя" in item and HILDA in item for item in brief.history)
        assert any(OWN in item or "Рутгер" in item for item in brief.history)
        assert brief.budget.open_left == 1 and brief.budget.lines_left == 3
        assert brief.budget.people_left == 7


async def test_brief_lists_each_other_occasion_with_details_and_game_age(graph):
    async with world(graph) as w:
        w.fake.minute = 7
        others = [
            occasion(),
            occasion(
                kind="deed",
                place=YARD,
                minute=2,
                deed={"fact": "found_out", "reason": "Герой раскрыл обман с пилой."},
            ),
            occasion(
                kind="vanilla",
                place=FIELD,
                minute=3,
                vanilla={"fact": "vanilla_quest", "title": "Пропала репа", "to": "success"},
            ),
        ]
        brief = await w.showrunner.brief(occasion(kind="chapter", minute=7, also=others))
        [also] = [line for line in brief.text().splitlines() if line.lower().startswith("заодно:")]
        for expected in (
            "arrived",
            "deed",
            "vanilla",
            HOME,
            YARD,
            FIELD,
            "Герой раскрыл обман с пилой.",
            "Пропала репа",
            "success",
            "7",
            "5",
            "4",
        ):
            assert expected in also


async def test_an_arrival_and_a_deed_five_seconds_later_make_one_decision(graph):
    async with world(graph, proposal(kind="none"), gather_s=10) as w:
        arrival = await arrive(w)
        finished = asyncio.Event()
        w.chronicle.on_fact.append(lambda f: finished.set() if f.get("fact") == "line_none" else None)
        running = asyncio.create_task(w.showrunner.run())
        try:
            await asyncio.sleep(0.02)
            assert w.llm.calls == [] and w.showrunner.occasions.pending == [arrival]
            w.clock.t += 5
            deed = {"fact": "died", "npc": "BANDIT", "killer": "PLAYER", "monster": False}
            await w.chronicle.record(deed)
            await w.showrunner.occasions.poll()
            await w.showrunner.occasions.poll()
            assert len(w.showrunner.occasions.pending) == 2 and w.llm.calls == []
            w.clock.t += 5
            w.showrunner.occasions._changed.set()
            await asyncio.sleep(0.02)
            assert w.llm.calls == []
            w.clock.t += 5
            w.showrunner.occasions._changed.set()
            await asyncio.wait_for(finished.wait(), 1)
            [result] = w.chronicle.facts("line_none")
            assert result["occasion"]["kind"] == "deed"
            assert result["occasion"]["deed"] == deed
            assert result["occasion"]["also"] == [arrival]
            assert w.showrunner.occasions.pending == [] and len(w.llm.calls) == 1
            text = "\n".join(getattr(item, "content", "") for item in w.llm.calls[0][1])
            [also] = [line for line in text.splitlines() if line.lower().startswith("заодно:")]
            assert "arrived" in also and HOME in also
        finally:
            running.cancel()
            await asyncio.gather(running, return_exceptions=True)


@pytest.mark.parametrize("outcome", ["line", "line_none", "line_rejected", "line_failed"])
async def test_every_outcome_keeps_and_consumes_the_whole_occasion_bundle(graph, outcome):
    choice = proposal(kind="none") if outcome == "line_none" else proposal()
    if outcome == "line_rejected":
        choice = proposal(giver=XARDAS)
    async with world(graph, choice) as w:
        if outcome == "line_failed":
            w.chains.chain.side_effect = None
            w.chains.chain.return_value = None
        arrival = occasion()
        vanilla = occasion(kind="vanilla", vanilla={"quest": SCRIPT_QUEST, "to": "success"})
        deed = occasion(kind="deed", deed={"fact": "died", "npc": "BANDIT", "killer": "PLAYER"})
        bundle = {**deed, "also": [arrival, vanilla]}
        result = await w.showrunner.decide(bundle)
        assert result["fact"] == outcome and result["occasion"] == bundle
        calls = len(w.llm.calls)
        for source in (deed, arrival, vanilla, {**arrival, "also": [vanilla, deed]}):
            assert await w.showrunner.decide(source) == result
        assert len(w.llm.calls) == calls


@pytest.mark.parametrize("value", [1, 2])
async def test_brief_includes_vanilla_event_giver_known_journal_and_no_future_ending(graph, value):
    async with world(graph) as w:
        w.fake.vars[SCRIPT_QUEST] = value
        graph.nodes["DIA_LOBART_RUEBENRUNNING"].effects.append(
            {"do": "journal_entry", "topic": "TOPIC_RUEBEN", "text": "Будущая тайная развязка."}
        )
        event = {
            "fact": "vanilla_quest",
            "quest": SCRIPT_QUEST,
            "title": "Убрать поле репы",
            "from": "open" if value == 1 else "running",
            "to": "running" if value == 1 else "success",
            "giver": LOBART,
        }
        for i in range(12):
            await w.chronicle.record({**event, "minute": i})
        brief = await w.showrunner.brief(occasion(kind="vanilla", vanilla=event))
        text = brief.text()
        assert len(brief.vanilla) == 10
        assert brief.vanilla == w.chronicle.facts("vanilla_quest")[-10:]
        for known in (SCRIPT_QUEST, "Убрать поле репы", "Лобарт", LOBART, event["to"]):
            assert known in text
        assert "Лобарт хочет, чтобы я собрал репу." in text
        assert "Будущая тайная развязка" not in text
        assert brief.occasion["vanilla"] == event


@pytest.mark.parametrize("has_outcome", [False, True])
async def test_vanilla_summary_includes_latest_quest_outcome_when_present(graph, has_outcome):
    async with world(graph) as w:
        event = {
            "fact": "vanilla_quest", "quest": SCRIPT_QUEST, "title": "Убрать поле репы",
            "from": "running", "to": "success", "giver": LOBART,
        }
        started = {**event, "from": "open", "to": "running"}
        await w.chronicle.record(started)
        await w.chronicle.record(event)
        expected = dict(event)
        if has_outcome:
            outcome = {
                "fact": "quest_outcome", "quest": SCRIPT_QUEST, "npc": LOBART,
                "how": "amended", "drop": [VINO, "ITPL_BEET", "DIA_LOBART_WORKNOW"],
                "why": ["Вино я отпустил.", "Репа больше не нужна."], "pay": "none",
            }
            w.fake.item_names["ITPL_BEET"] = "Репа"
            graph.nodes["DIA_LOBART_WORKNOW"].description = "Я хочу поработать."
            await w.chronicle.record({**outcome, "how": "done", "drop": [], "pay": "full"})
            await w.chronicle.record(outcome)
            await w.chronicle.record({**outcome, "quest": "MIS_OTHER", "how": "dropped"})
            expected["outcome"] = {
                "how": "amended", "drop": [NAMES[VINO], "Репа", "Я хочу поработать."],
                "why": outcome["why"], "pay": "none",
            }
        brief = await w.showrunner.brief(occasion(kind="vanilla", vanilla=event))
        assert brief.vanilla == [started, expected]
        assert brief.occasion["vanilla"] == expected
        summary = next(s for s in brief.text().splitlines() if s.startswith("Последние ванильные"))
        assert json.loads(summary.split(": ", 1)[1]) == [started, expected]
        assert w.chronicle.facts("vanilla_quest") == [started, event]
        assert "outcome" not in started and "outcome" not in event


def test_proposal_parses_follows_and_all_three_requirements_and_keeps_old_defaults():
    requires = [
        {"kind": "quest", "quest": SCRIPT_QUEST, "state": "success"},
        {"kind": "knows", "node": "DIA_LOBART_WORKNOW"},
        {"kind": "chapter", "chapter": 2},
    ]
    p = Proposal.of(proposal(follows=SCRIPT_QUEST, requires=requires))
    assert (p.follows, p.requires) == (SCRIPT_QUEST, requires)
    assert Proposal.of(proposal()).follows == "" and Proposal.of(proposal()).requires == []
    assert {"follows", "requires"} <= set(PROPOSE_TOOL.parameters["required"])


async def enable_crime(w, **change):
    crimes = Crimes(w.author.link, w.chronicle, places=w.author.places, names=lambda n: NAMES.get(n, n))
    crimes.people[HILDA] = {"guild": 8, "npctype": 0}
    w.showrunner.crimes, w.showrunner.law = crimes, CrimeLaw(crimes)
    crime = {
        "fact": "crime", "id": "A/deed-1", "kind": "theft", "victim": LOBART,
        "owner": LOBART, "community": "farm", "item": "ITFO_BREAD", "count": 2,
        "chest": "LOBART_CHEST", "waypoint": "NW_FARM1_INHOUSE_01",
        "day": 10, "hour": 7, "minute": 45, "witnesses": [HILDA], **change,
    }
    await w.chronicle.record(crime)
    return crime


@pytest.mark.parametrize("status", ["pending", "known", "paid"])
async def test_crime_brief_separates_truth_beliefs_and_read_only_law(graph, status):
    async with world(graph) as w:
        crime = await enable_crime(w)
        cid = crime["id"]
        await w.chronicle.record({"fact": "crime_seen", "npc": HILDA, "crime": cid})
        await w.chronicle.record({"fact": "rumor", "from": HILDA, "to": VINO, "crime": cid, "hops": 2})
        if status in {"known", "paid"}:
            await w.chronicle.record({"fact": "crime_known", "crime": cid, "community": "farm"})
        if status == "paid":
            await w.chronicle.record(
                {"fact": "absolved", "crimes": [cid], "community": "farm", "how": "paid"}
            )
        before = w.chronicle.facts()
        brief = await w.showrunner.brief(occasion(kind="deed", crime=cid, deed=crime))
        case = brief.crimes[cid]
        assert case["truth"] == crime
        assert {k["npc"]: k["degree"] for k in case["knowledge"]} == {
            LOBART: "unknown", HILDA: "seen", VINO: "heard",
        }
        assert next(k for k in case["knowledge"] if k["npc"] == VINO)["source"] == HILDA
        assert case["victim_can_notice_missing"] is True
        assert case["law"]["known"] is (status != "pending")
        assert case["law"]["paid"] is (status == "paid")
        [delivery] = case["law"]["deliveries"]
        assert delivery["status"] == {"pending": "pending", "known": "known", "paid": "absolved"}[status]
        assert delivery["via"] == HILDA
        assert w.chronicle.facts() == before
        text = brief.text()
        assert all(t in text for t in (cid, "ITFO_BREAD", "unknown", "seen", "heard", "07:45"))


@pytest.mark.parametrize("cover", [True, False])
async def test_old_cover_testimony_enters_crime_brief_when_cover_is_disabled(graph, cover):
    async with world(graph) as w:
        crime = await enable_crime(w)
        cid = crime["id"]
        w.showrunner.crimes.cover = cover
        await w.chronicle.record({
            "fact": "testimony", "npc": VINO, "witness": HILDA, "cover": "D1",
            "crime_saw": [cid], "crime_told": [],
        })
        case = w.showrunner._crime_briefs(occasion(), w.chronicle.facts())[cid]
        recipients = [k for k in case["knowledge"] if k["npc"] == VINO]
        assert bool(recipients) is not cover
        if recipients:
            assert (recipients[0]["degree"], recipients[0]["source"]) == ("heard", HILDA)


@pytest.mark.parametrize(
    ("kind", "giver", "degree", "allowed"),
    [
        ("theft", LOBART, "unknown", True),
        ("theft", HILDA, "unknown", False),
        ("theft", HILDA, "seen", True),
        ("theft", HILDA, "heard", True),
        ("theft", HILDA, "suspects", True),
        ("attack", LOBART, "unknown", False),
        ("attack", LOBART, "heard", True),
    ],
)
async def test_crime_giver_canon_uses_personal_knowledge_or_missing_property(
    graph, kind, giver, degree, allowed
):
    async with world(graph) as w:
        crime = await enable_crime(w, kind=kind)
        cid = crime["id"]
        if degree == "seen":
            await w.chronicle.record({"fact": "crime_seen", "npc": giver, "crime": cid})
        elif degree == "heard":
            await w.chronicle.record({"fact": "rumor", "from": VINO, "to": giver, "crime": cid})
        elif degree == "suspects":
            await w.chronicle.record(
                {"fact": "crime_suspected", "npc": giver, "crime": cid, "suspect": "PLAYER"}
            )
        brief = await w.showrunner.brief(occasion(kind="deed", crime=cid, deed=crime))
        errors = w.showrunner.canon(Proposal.of(proposal(giver=giver, crime=cid)), brief)
        assert (not errors) is allowed
        if not allowed:
            assert "не знает о деле" in str(errors)


async def test_a_crime_line_keeps_its_id_through_the_builder_and_load_without_forgiving(graph):
    async with world(graph, proposal(giver=LOBART, crime="A/deed-1")) as w:
        crime = await enable_crime(w, witnesses=[])
        result = await w.showrunner.decide(occasion(kind="deed", crime=crime["id"], deed=crime))
        assert result["fact"] == "line" and result["crime"] == crime["id"]
        assert w.chains.chain.await_args.kwargs["crime"] == crime["id"]
        assert w.chains.chain.await_args.args == (LOBART,)
        assert w.chronicle.facts("forgiven") == []
        await load(w.fake, w.chronicle, branch="restored")
        assert w.showrunner.lines == [result]


async def test_crime_canon_rejects_unknown_ids_and_requires_id_for_a_deed_line(graph):
    async with world(graph) as w:
        crime = await enable_crime(w)
        brief = await w.showrunner.brief(occasion(kind="deed", crime=crime["id"], deed=crime))
        assert "нет такого дела" in str(w.showrunner.canon(Proposal.of(proposal(crime="invented")), brief))
        assert "id в crime" in str(w.showrunner.canon(Proposal.of(proposal()), brief))
        assert "не знает о деле" in str(w.showrunner.canon(
            Proposal.of(proposal(kind="stranger", place=HOME, giver="", crime=crime["id"])), brief
        ))
        assert Proposal.of(proposal()).crime == ""
        assert "crime" in PROPOSE_TOOL.parameters["required"]


async def test_a_crime_can_receive_none_with_a_reason(graph):
    reason = "Сейчас у фермеров нет нового поручения; свидетель сам решит, говорить ли."
    async with world(graph, proposal(kind="none", reason=reason)) as w:
        crime = await enable_crime(w)
        result = await w.showrunner.decide(occasion(kind="deed", crime=crime["id"], deed=crime))
        assert result["fact"] == "line_none" and result["reason"] == reason
        assert len(w.llm.calls) == 1
        w.chains.chain.assert_not_awaited()


async def test_no_crimes_does_not_reintroduce_saved_crimes_in_the_brief(graph):
    async with world(graph) as w:
        crime = await enable_crime(w)
        w.showrunner.crimes, w.showrunner.law = None, None
        brief = await w.showrunner.brief(occasion())
        assert brief.crimes == {} and crime not in brief.deeds
        assert crime["id"] not in brief.text()


async def enable_npc_crime(w, **change):
    await enable_crime(w)
    crime = {
        "fact": "npc_crime", "id": "A/npc-deed-1", "kind": "theft", "culprit": VINO,
        "victim": LOBART, "owner": LOBART, "community": "farm", "item": "ITFO_BREAD",
        "count": 2, "place": HOME, "waypoint": "NW_FARM1_INHOUSE_01",
        "day": 10, "hour": 7, "minute": 45, "witnesses": [HILDA], **change,
    }
    await w.chronicle.record(crime)
    return crime


def npc_rumor(crime, to, **change):
    return {
        "fact": "rumor", "quest": f"npc_crime_seen:{crime['id']}", "kind": "npc_theft",
        "subject": crime["culprit"], "about": crime["victim"], "from": HILDA, "to": to,
        "hops": 1, **change,
    }


async def test_npc_crime_brief_keeps_truth_and_live_property_separate_from_each_person(graph):
    async with world(graph) as w:
        crime = await enable_npc_crime(w, far=True)
        cid = crime["id"]
        for fact in [
            {"fact": "npc_crime_seen", "npc": HILDA, "crime": cid},
            {"fact": "crime_seen", "npc": DEAD, "crime": cid},
            npc_rumor(crime, OWN),
            {"fact": "npc_crime_suspected", "npc": XARDAS, "crime": cid, "suspect": HILDA},
            {"fact": "blamed", "id": "B1", "npc": LOBART, "culprit": HILDA,
             "crime": cid, "because": "hero_words", "truth": False},
            {"fact": "npc_crime_known", "crime": cid, "community": "farm", "via": HILDA},
            {"fact": "npc_crime_settled", "crime": cid, "by": "GUARD", "how": "fight"},
            {"fact": "exposed", "npc": LOBART, "liar": HILDA, "testimony": "T1"},
            {"fact": "betrayed", "npc": DEAD, "deal": "unrelated", "to": "OTHER"},
        ]:
            await w.chronicle.record(fact)
        evaluate = w.fake.evaluate

        def with_inventory(expr, npc):
            if expr[:2] == ["call", "NPC_HASITEMS"]:
                return {VINO: 0, LOBART: 1, "HERO": 2}.get(expr[2][1], 0)
            return evaluate(expr, npc)

        w.fake.evaluate = with_inventory
        before = w.chronicle.facts()
        case = (await w.showrunner.brief(occasion(kind="deed", crime=cid, deed=crime))).npc_crimes[cid]
        assert case["truth"]["culprit"] == VINO and case["truth"]["distance"] == "far"
        assert case["truth"]["item_holders_checked"] is True
        assert case["truth"]["item_holders"] == [
            {"npc": LOBART, "count": 1}, {"npc": "PLAYER", "count": 2},
        ]
        known = {k["npc"]: k for k in case["knowledge"]}
        assert (known[HILDA]["degree"], known[HILDA]["suspect"]) == ("seen", VINO)
        assert (known[OWN]["degree"], known[OWN]["source"]) == ("heard", HILDA)
        assert (known[XARDAS]["degree"], known[XARDAS]["suspect"]) == ("suspects", HILDA)
        assert (known[LOBART]["degree"], known[LOBART]["source"], known[LOBART]["suspect"]) == (
            "blames", "PLAYER", HILDA,
        )
        assert not any("truth" in k for k in case["knowledge"])
        assert DEAD not in known
        assert case["belief_truth"] == [
            {"blamed": "B1", "npc": LOBART, "suspect": HILDA, "truth": False, "refuted": False},
        ]
        assert case["law"] == {"community": "farm", "known": True, "settled": True}
        assert [f["fact"] for f in case["people_context"]] == ["exposed"]
        assert w.chronicle.facts() == before


@pytest.mark.parametrize("via", ["seen", "heard", "testimony", "blames", "suspects", "unknown"])
async def test_npc_crime_canon_uses_own_knowledge_belief_or_the_victims_loss(graph, via):
    async with world(graph) as w:
        crime = await enable_npc_crime(w)
        cid = crime["id"]
        if via == "seen":
            await w.chronicle.record({"fact": "npc_crime_seen", "npc": HILDA, "crime": cid})
        elif via == "heard":
            await w.chronicle.record(npc_rumor(crime, HILDA, **{"from": LOBART}))
        elif via == "testimony":
            await w.chronicle.record({"fact": "testimony", "id": "T1", "npc": HILDA,
                                "witness": LOBART, "about": VINO, "crime_told": [cid]})
        elif via == "blames":
            await w.chronicle.record({"fact": "blamed", "id": "B1", "npc": HILDA,
                                "culprit": VINO, "crime": cid, "because": "hunch", "truth": True})
        elif via == "suspects":
            await w.chronicle.record({"fact": "npc_crime_suspected", "npc": HILDA,
                                "crime": cid, "suspect": VINO})
        brief = await w.showrunner.brief(occasion(kind="deed", crime=cid, deed=crime))
        assert not w.showrunner.canon(Proposal.of(proposal(giver=LOBART, crime=cid)), brief)
        errors = w.showrunner.canon(Proposal.of(proposal(giver=HILDA, crime=cid)), brief)
        assert bool(errors) is (via in {"unknown", "suspects"})
        assert "id в crime" in str(w.showrunner.canon(Proposal.of(proposal()), brief))
        # Even a thief with personal evidence cannot commission a line about his own deed.
        w.fake.npc_states[VINO]["distance"] = 200
        brief = await w.showrunner.brief(occasion(kind="deed", crime=cid, deed=crime))
        assert "виновник" in str(w.showrunner.canon(Proposal.of(proposal(giver=VINO, crime=cid)), brief))


async def test_npc_crime_line_and_its_knowledge_survive_load_without_entering_hero_cases(graph):
    async with world(graph, proposal(giver=LOBART, crime="A/npc-deed-1")) as w:
        crime = await enable_npc_crime(w)
        await w.chronicle.record({"fact": "npc_crime_seen", "npc": HILDA, "crime": crime["id"]})
        current = occasion(kind="deed", crime=crime["id"], deed=crime)
        result = await w.showrunner.decide(current)
        assert result["fact"] == "line" and result["crime"] == crime["id"]
        assert w.chains.chain.await_args.kwargs["crime"] == crime["id"]
        assert crime["id"] not in w.showrunner._crime_briefs(current, w.chronicle.facts())
        before = (await w.showrunner.brief(current)).npc_crimes
        await load(w.fake, w.chronicle, branch="restored")
        assert w.showrunner.lines == [result]
        assert (await w.showrunner.brief(current)).npc_crimes == before


@pytest.mark.parametrize("how", ["unpaid", "no_show"])
async def test_broken_npc_promise_brief_names_promise_failure_and_hearers_without_a_false_giver(graph, how):
    async with world(graph) as w:
        await enable_crime(w)
        await w.showrunner.occasions.poll()
        await w.showrunner.occasions.next()
        promise = {"fact": "deal", "deal": "D1", "npc": HILDA, "name": "Хильда",
                   "npc_later": {"items": {"ITMI_GOLD": 10}, "day": 12}}
        failure = {"fact": "npc_broken", "deal": "D1", "npc": HILDA, "how": how,
                   "witnesses": [LOBART] if how == "unpaid" else [], "day": 12, "hour": 8, "minute": 0}
        await w.chronicle.record(promise)
        await w.chronicle.record(failure)
        await w.showrunner.occasions.poll()
        assert w.showrunner.occasions.pending == []
        assert not w.llm.calls
        brief = await w.showrunner.brief(occasion())
        case = brief.npc_broken["D1"]
        assert case["promise"] == promise and case["failed"]["how"] == how
        assert case["heard"] == failure["witnesses"]
        assert case["claimant"] == "PLAYER" and case["giver"] is None
        assert "герой не может быть заказчиком" in brief.text()
        assert (await w.showrunner.brief(occasion(people=[VINO]))).npc_broken == {}
        await load(w.fake, w.chronicle, branch="restored")
        assert (await w.showrunner.brief(occasion())).npc_broken == brief.npc_broken
        assert not w.llm.calls


@pytest.mark.parametrize("current", [
    occasion(people=[], also=[occasion(people=[HILDA])]),
    occasion(kind="deed", people=[], deed={"fact": "npc_crime", "culprit": HILDA}),
    occasion(kind="vanilla", people=[], vanilla={"giver": HILDA}),
    occasion(people=[LOBART]),
])
async def test_broken_npc_promise_context_follows_people_of_the_current_occasion(graph, current):
    async with world(graph) as w:
        await enable_crime(w)
        # Keep an older relevant breach despite unrelated recent breaches elsewhere.
        await w.chronicle.record({"fact": "npc_broken", "deal": "D1", "npc": HILDA,
                            "how": "unpaid", "witnesses": [LOBART]})
        for i in range(12):
            await w.chronicle.record({"fact": "npc_broken", "deal": f"AWAY_{i}", "npc": VINO,
                                "how": "no_show"})
        brief = await w.showrunner.brief(current)
        assert set(brief.npc_broken) == {"D1"}
        assert not w.llm.calls


async def test_npc_director_switch_hides_saved_npc_cases_and_allows_none_with_reason(graph):
    reason = "Лобарт занят своим делом; сейчас новая линия не нужна."
    async with world(graph, proposal(kind="none", reason=reason)) as w:
        crime = await enable_npc_crime(w)
        result = await w.showrunner.decide(occasion(kind="deed", crime=crime["id"], deed=crime))
        assert result["fact"] == "line_none" and result["reason"] == reason
        w.chains.chain.assert_not_awaited()
        w.showrunner.occasions.npc_crimes = False
        brief = await w.showrunner.brief(occasion())
        assert brief.npc_crimes == {} and crime["id"] not in brief.text()


async def test_director_cast_card_keeps_dishonesty_and_greed_for_npc_rules(graph):
    from gothic_ai.npc_cheat import decide

    async with world(graph) as w:
        person = Person(
            ru="Ты — жадный лжец, готовый обмануть ради наживы.", en="A greedy liar.",
            greed=5, honesty=1, courage=3, talk=3, pushy=3, strangers=3,
            voice="A rough middle aged man with a low voice.",
        )
        await w.author.patches.introduce(spec(), person.model_dump())
        await w.casting.cast.prepare()
        cards = w.casting.cast.cards()
        assert (cards[OWN].honesty, cards[OWN].greed) == (1, 5)
        intent = await decide(w.chronicle, OWN, "D1", ["contract"], cards=cards)
        assert intent["keeps"] is False


@pytest.mark.parametrize(
    ("bad", "reason"),
    [
        ({"follows": "MIS_INVENTED"}, "не квест ванильного графа"),
        ({"giver": LOBART, "follows": SCRIPT_QUEST}, "выдаёт или закрывает"),
        ({"requires": [{"kind": "quest", "quest": SCRIPT_QUEST, "state": "success"}]}, "недостижимо"),
        ({"requires": [{"kind": "quest", "quest": "MIS_INVENTED", "state": "running"}]}, "не переменная"),
        ({"requires": [{"kind": "knows", "node": "DIA_INVENTED"}]}, "нет ванильного диалога"),
        ({"requires": [{"kind": "chapter", "chapter": 7}]}, "от 1 до 6"),
        ({"requires": {}}, "requires"),
        ({"follows": 12}, "follows"),
    ],
)
async def test_vanilla_canon_rejection_returns_the_reason_before_building(graph, bad, reason):
    async with world(graph, proposal(**bad), proposal()) as w:
        w.fake.vars[SCRIPT_QUEST] = 3
        result = await w.showrunner.decide(occasion(kind="vanilla"))
        assert result["fact"] == "line"
        [rejected] = w.chronicle.facts("line_rejected")
        assert reason in str(rejected["why"])
        correction = "\n".join(getattr(item, "content", "") for item in w.llm.calls[1][1])
        assert all(e in correction for e in rejected["why"])
        w.chains.chain.assert_awaited_once()


async def test_canon_also_excludes_a_different_npc_who_only_closes_the_followed_quest(graph):
    graph.nodes["DIA_LOBART_RUEBENRUNNING"].npc = HILDA
    async with world(graph, proposal(follows=SCRIPT_QUEST), proposal()) as w:
        assert (await w.showrunner.decide(occasion()))["fact"] == "line"
        assert "выдаёт или закрывает" in str(w.chronicle.facts("line_rejected"))


@pytest.mark.parametrize("kind", ["quest", "chain", "stranger"])
async def test_vanilla_fields_reach_every_builder_and_survive_chronicle_reload(graph, kind):
    requires = [{"kind": "quest", "quest": SCRIPT_QUEST, "state": "success"}]
    async with world(graph, proposal(kind=kind, place=HOME, follows=SCRIPT_QUEST, requires=requires)) as w:
        w.fake.vars[SCRIPT_QUEST] = 2
        result = await w.showrunner.decide(occasion(kind="vanilla"))
        assert result["fact"] == "line"
        assert result["follows"] == SCRIPT_QUEST and result["requires"] == requires
        call = w.chains.chain.await_args
        assert call.kwargs["follows"] == SCRIPT_QUEST and call.kwargs["requires"] == requires
        await load(w.fake, w.chronicle, branch="restored")
        assert w.showrunner.lines == [result]


@pytest.mark.parametrize(
    ("bad", "reason"),
    [
        ({"giver": XARDAS}, "сюжет"),
        ({"giver": "UNKNOWN"}, "кандидат"),
        ({"giver": DEAD}, "кандидат"),
        ({"kind": "stranger", "giver": "", "place": "CITY"}, "сводк"),
        ({"new_people": 1}, "new_people"),
        ({"kind": "invented"}, "вид"),
        ({"premise": ""}, "замысел"),
    ],
)
async def test_canon_refusal_returns_reasons_to_one_second_attempt(graph, bad, reason):
    async with world(graph, proposal(**bad), proposal()) as w:
        result = await w.showrunner.decide(occasion())
        assert result["fact"] == "line" and result["giver"] == HILDA
        [rejected] = w.chronicle.facts("line_rejected")
        assert rejected["why"]
        correction = "\n".join(getattr(item, "content", "") for item in w.llm.calls[1][1])
        assert reason.lower() in correction.lower()
        assert len(w.llm.calls) == 2
        assert w.llm.cache_keys == [CACHE_KEY, CACHE_KEY]
        assert [[tool.name for tool in call[2]] for call in w.llm.calls] == [[PROPOSE], [PROPOSE]]
        assert await w.showrunner.decide(occasion()) == result
        assert len(w.llm.calls) == 2
        w.chains.chain.assert_awaited_once()


async def test_occupied_giver_is_rejected_even_with_another_free_candidate(graph):
    async with world(graph, proposal(), proposal(giver=LOBART)) as w:
        await w.chronicle.record(line())
        result = await w.showrunner.decide(occasion())
        assert result["fact"] == "line" and result["giver"] == LOBART
        assert len(w.llm.calls) == 2 and len(w.chronicle.facts("line_rejected")) == 1
        assert w.chains.chain.await_args.args[0] == LOBART


@pytest.mark.parametrize("kind", ["quest", "chain", "stranger"])
async def test_new_people_canon_respects_remaining_slots_but_existing_quest_can_pass(graph, kind):
    async with world(
        graph,
        proposal(
            kind=kind,
            new_people=kind in {"quest", "chain"},
            giver=HILDA if kind in {"quest", "chain"} else "",
            place=HOME,
        ),
        proposal(),
    ) as w:
        await w.chronicle.record(line(characters=[f"DIR_NPC_{i}" for i in range(8)]))
        await w.finish("DIR_Q1")
        result = await w.showrunner.decide(occasion())
        assert result["fact"] == "line" and len(w.llm.calls) == 2
        assert w.chronicle.facts("line_rejected")[0]["why"]
        w.casting.character.assert_not_called()
        w.chains.chain.assert_awaited_once_with(
            HILDA,
            new_people=False,
            premise=PREMISE,
            max_people=0,
            follows="",
            requires=[],
            quest_count=1,
        )
        assert w.showrunner.budget(1, occasion()).people_left == 0


@pytest.mark.parametrize(
    ("attitude", "guild_attitude", "included"),
    [(0, 3, False), (2, 0, False), (2, 2, True), (3, 2, True)],
)
async def test_brief_checks_personal_and_directional_guild_hostility(
    graph, attitude, guild_attitude, included
):
    async with world(graph) as w:
        w.fake.attitudes[HILDA] = attitude
        w.fake.fields[HILDA, "GUILD"] = 8
        w.fake.fields["HERO", "GUILD"] = 7
        w.fake.guild_attitudes[8, 7] = guild_attitude
        w.fake.guild_attitudes[7, 8] = 3  # the reverse direction must not mask hostility towards HERO
        brief = await w.showrunner.brief(occasion())
        assert (HILDA in brief.candidates) is included
        assert LOBART in brief.candidates


async def test_brief_batches_all_candidates_and_keeps_friendly_people_but_not_own_bandits(graph):
    async with world(graph) as w:
        await w.author.patches.introduce(spec().model_copy(update={"guild": "GIL_BDT"}))
        w.fake.npc_states[OWN] = {"state": "alive", "name": "Бандит", "distance": 300}
        w.fake.fields[OWN, "GUILD"] = 9
        w.fake.attitudes.update({HILDA: 2, LOBART: 3, OWN: 3})
        brief = await w.showrunner.brief(occasion())
        assert set(brief.candidates) == {HILDA, LOBART}
        batches = [r for r in w.fake.story if r.get("type") == "story_eval" and "NPC_GETATTITUDE" in str(r)]
        assert len(batches) == 1 and len(batches[0]["exprs"]) == 3
        assert all("WLD_GETGUILDATTITUDE" in str(e) for e in batches[0]["exprs"])


async def test_hostile_giver_is_rejected_before_the_builder_and_neutral_retry_is_accepted(graph):
    async with world(graph, proposal(giver=HILDA), proposal(giver=LOBART)) as w:
        w.fake.attitudes[HILDA] = 0
        result = await w.showrunner.decide(occasion())
        assert result["fact"] == "line" and result["giver"] == LOBART
        w.chains.chain.assert_awaited_once_with(
            LOBART,
            new_people=False,
            premise=PREMISE,
            max_people=0,
            follows="",
            requires=[],
            quest_count=1,
        )
        w.author.quest.assert_not_called()
        assert HILDA in str(w.chronicle.facts("line_rejected"))


async def test_two_canon_failures_stop_without_any_builder(graph):
    async with world(graph, proposal(giver=XARDAS)) as w:
        result = await w.showrunner.decide(occasion())
        assert result["fact"] == "line_rejected"
        assert len(w.llm.calls) == 2 and len(w.chronicle.facts("line_rejected")) == 2
        assert w.chronicle.facts("line") == []
        w.author.quest.assert_not_called()
        assert (await w.showrunner.decide(occasion()))["fact"] == "line_rejected"
        assert len(w.llm.calls) == 2


def passed_premise(call):
    return call.kwargs.get("premise", call.args[1] if len(call.args) > 1 else None)


@pytest.mark.parametrize(
    ("kind", "new_people"),
    [("quest", False), ("quest", True), ("chain", False), ("chain", True), ("stranger", False)],
)
async def test_each_kind_passes_the_premise_and_records_the_made_objects(graph, kind, new_people):
    async with world(graph, proposal(kind=kind, new_people=new_people, place=HOME)) as w:
        result = await w.showrunner.decide(occasion())
        assert result["fact"] == "line" and result["kind"] == kind
        assert result["occasion"] == occasion() and result["premise"] == PREMISE
        assert (result["chapter"], result["day"], result["hour"], result["minute"]) == (1, 10, 8, 0)
        assert result["wall_time"] == w.clock.t
        assert set(result["quests"]) == set(await w.author.book.all())
        assert len(result["quests"]) == (2 if kind == "chain" else 1)
        call = w.chains.chain.await_args
        assert call.args[0] == (OWN if kind == "stranger" else HILDA)
        assert call.kwargs.get("new_people", False) is new_people
        assert call.kwargs["max_people"] == (MAX_PEOPLE_PER_CHAPTER if new_people else 0)
        assert call.kwargs.get("quest_count") == (None if kind == "chain" else 1)
        assert passed_premise(call) == PREMISE
        assert result["giver"] == call.args[0]
        w.author.quest.assert_not_called()
        if kind == "stranger":
            call = w.casting.character.await_args
            assert call.args[0] == HOME and call.kwargs["about"] == PREMISE
            assert passed_premise(call) == PREMISE
            assert result["characters"] == [OWN]
        else:
            assert result["characters"] == []
            w.casting.character.assert_not_called()


@pytest.mark.parametrize("kind", ["quest", "chain"])
async def test_new_people_builder_receives_the_remaining_chapter_budget(graph, kind):
    async with world(graph, proposal(kind=kind, new_people=True)) as w:
        await w.chronicle.record(line(characters=[f"DIR_NPC_{i}" for i in range(3)]))
        await w.finish("DIR_Q1")
        result = await w.showrunner.decide(occasion())
        assert result["fact"] == "line"
        assert w.chains.chain.await_args.kwargs["max_people"] == MAX_PEOPLE_PER_CHAPTER - 3
        w.author.quest.assert_not_called()


async def test_a_chain_keeps_the_builders_quest_order_across_two_digit_ids(graph):
    async with world(graph, proposal(kind="chain")) as w:

        async def chain(*args, **kwargs):
            quests = [quest(id="DIR_Q9"), quest(id="DIR_Q10")]
            await w.keep(*quests)
            return quests

        w.chains.chain.side_effect = chain
        result = await w.showrunner.decide(occasion())
        assert result["quests"] == ["DIR_Q9", "DIR_Q10"]


async def test_none_records_the_decision_without_spending_a_line_or_calling_builders(graph):
    async with world(graph, proposal(kind="none", reason="Пусть сначала закончит свои дела.")) as w:
        before = w.showrunner.budget(1, occasion())
        result = await w.showrunner.decide(occasion())
        assert result["fact"] == "line_none" and result["reason"] == "Пусть сначала закончит свои дела."
        assert result["proposal"] == proposal(kind="none", reason="Пусть сначала закончит свои дела.")
        assert w.showrunner.lines == []
        assert w.showrunner.budget(1, occasion()) == before
        assert len(w.llm.calls) == 1
        w.author.quest.assert_not_called()
        w.chains.chain.assert_not_called()
        w.casting.character.assert_not_called()


@pytest.mark.parametrize("kind", ["quest", "chain", "stranger"])
async def test_a_failed_builder_records_a_used_occasion(graph, kind):
    async with world(graph, proposal(kind=kind, place=HOME)) as w:
        if kind in {"quest", "chain"}:
            w.chains.chain.side_effect = None
            w.chains.chain.return_value = None
        elif kind == "stranger":
            w.casting.character.side_effect = None
            w.casting.character.return_value = None
        result = await w.showrunner.decide(occasion())
        assert result["fact"] == "line_failed" and result["occasion"] == occasion()
        assert w.chronicle.facts("line_failed") == [result]
        assert w.showrunner.lines == []
        assert await w.showrunner.decide(occasion()) == result
        assert len(w.llm.calls) == 1


async def test_a_stranger_left_by_a_failed_quest_still_spends_a_character_slot(graph):
    async with world(graph, proposal(kind="stranger", giver="", place=HOME)) as w:
        w.chains.chain.side_effect = None
        w.chains.chain.return_value = None
        result = await w.showrunner.decide(occasion())
        assert result["fact"] == "line_failed" and result["characters"] == [OWN]
        budget = w.showrunner.budget(1, occasion())
        assert budget.people_left == MAX_PEOPLE_PER_CHAPTER - 1
        assert budget.lines_left == MAX_LINES_PER_CHAPTER and budget.open_left == MAX_OPEN_LINES


async def test_partial_bandit_group_counts_every_character_after_a_failed_chain(graph):
    async with world(graph, proposal(kind="chain", new_people=True)) as w:

        async def partial(*args, **kwargs):
            for i in range(3):
                await w.author.patches.introduce(spec(npc=f"DIR_NPC_{i + 1}", name="Бандит"))
            return None

        w.chains.chain.side_effect = partial
        result = await w.showrunner.decide(occasion())
        assert result["fact"] == "line_failed"
        assert set(result["characters"]) == {"DIR_NPC_1", "DIR_NPC_2", "DIR_NPC_3"}
        assert w.showrunner.budget(1, occasion()).people_left == MAX_PEOPLE_PER_CHAPTER - 3


async def test_loading_restores_lines_progress_and_the_budget_from_the_saved_branch(graph):
    async with world(graph, proposal(kind="stranger", giver="", place=HOME)) as w:
        result = await w.showrunner.decide(occasion())
        saved = await w.chronicle.memory.head(w.chronicle.branch)
        before = w.showrunner.budget(1, occasion())
        assert w.showrunner.is_open(result)
        await w.finish(*result["quests"])
        await w.chronicle.record(line(giver=LOBART, quests=[], characters=["DIR_NPC_999"]))
        assert w.showrunner.budget(1, occasion()) != before
        await load(w.fake, w.chronicle, head=saved)
        assert w.showrunner.lines == [result]
        assert w.showrunner.is_open(result)
        assert w.showrunner.budget(1, occasion()) == before
        assert set(await w.author.book.all()) == set(result["quests"])
        assert [person.npc for person in await w.casting.cast.characters()] == [OWN]
        assert await w.showrunner.decide(occasion()) == result
        assert len(w.llm.calls) == 1


async def test_loading_while_the_model_thinks_discards_its_old_branch_decision(graph):
    async with world(graph) as w:
        saved = await w.chronicle.memory.head(w.chronicle.branch)
        started, resume = asyncio.Event(), asyncio.Event()

        async def delayed(*args, **kwargs):
            started.set()
            await resume.wait()
            return proposal()

        w.showrunner.model.propose = delayed
        deciding = asyncio.create_task(w.showrunner.decide(occasion()))
        try:
            await asyncio.wait_for(started.wait(), 1)
            await load(w.fake, w.chronicle, head=saved)
            resume.set()
            assert await asyncio.wait_for(deciding, 1) is None
            assert not any(f["fact"].startswith("line") for f in w.chronicle.facts())
            w.author.quest.assert_not_called()
        finally:
            resume.set()
            deciding.cancel()
            await asyncio.gather(deciding, return_exceptions=True)


@pytest.mark.parametrize("built", ["people", "quests", "both"])
async def test_loading_records_partial_build_synchronously_on_the_old_branch_and_spends_its_budget(
    graph, built
):
    async with world(graph, proposal(kind="chain", new_people=True)) as w:
        # Pre-existing objects are not receipts of the interrupted attempt.
        await w.author.patches.introduce(spec(npc="DIR_NPC_99"))
        await w.keep(quest(id="DIR_OLD"))
        old_branch = w.chronicle.branch
        saved = await w.chronicle.memory.head(old_branch)
        started, resume = asyncio.Event(), asyncio.Event()

        async def partial(*args, **kwargs):
            if built in {"people", "both"}:
                await w.author.patches.introduce(spec())
            if built in {"quests", "both"}:
                await w.keep(quest())
            started.set()
            await resume.wait()

        w.chains.chain.side_effect = partial
        deciding = asyncio.create_task(w.showrunner.decide(occasion()))
        try:
            await asyncio.wait_for(started.wait(), 1)
            link = w.showrunner.link
            await link._dispatch(Loading(type="loading"))
            # Loading's receipt is durable before the following Session switches branches.
            [failed] = w.chronicle.facts("line_failed")
            assert failed["occasion"] == occasion() and "загрузк" in failed["why"][0]
            assert failed["characters"] == ([OWN] if built in {"people", "both"} else [])
            assert failed["quests"] == (["DIR_Q1"] if built in {"quests", "both"} else [])
            budget = w.showrunner.budget(1, occasion())
            assert budget.people_left == MAX_PEOPLE_PER_CHAPTER - len(failed["characters"])
            assert budget.lines_left == MAX_LINES_PER_CHAPTER - bool(failed["quests"])
            assert budget.open_left == MAX_OPEN_LINES - bool(failed["quests"])
            assert budget.game_wait == (MIN_GAME_MINUTES if failed["quests"] else 0)
            assert budget.real_wait == (MIN_REAL_SECONDS if failed["quests"] else 0)
            await link._dispatch(
                Session(type="session", branch="loaded", parent=old_branch, parent_seq=saved, reason="load")
            )
            assert w.chronicle.branch == "loaded" and w.chronicle.facts("line_failed") == []
            assert await asyncio.wait_for(deciding, 1) is None
            assert w.chronicle.facts("line_failed") == []
            assert w.showrunner.budget(1, occasion()).people_left == MAX_PEOPLE_PER_CHAPTER
            await load(w.fake, w.chronicle, branch=old_branch)
            assert w.chronicle.facts("line_failed") == [failed]
            assert w.showrunner.budget(1, occasion()) == budget
        finally:
            resume.set()
            deciding.cancel()
            await asyncio.gather(deciding, return_exceptions=True)


@pytest.mark.parametrize("stage", ["state", "model", "builder"])
async def test_run_logs_unexpected_failure_records_the_occasion_and_continues(
    graph, monkeypatch, caplog, stage
):
    async with world(graph, proposal(kind="chain", new_people=True), proposal(kind="none")) as w:
        first, second = occasion(), occasion(kind="chapter", chapter=2)
        monkeypatch.setattr(
            w.showrunner.occasions,
            "next",
            AsyncMock(side_effect=[first, second, asyncio.CancelledError()]),
        )
        if stage == "state":
            state = await w.showrunner._state()
            monkeypatch.setattr(
                w.showrunner, "_state", AsyncMock(side_effect=[RuntimeError("broken"), state])
            )
        elif stage == "model":
            monkeypatch.setattr(
                w.showrunner.model,
                "propose",
                AsyncMock(side_effect=[RuntimeError("broken"), proposal(kind="none")]),
            )
        else:

            async def broken_builder(*args, **kwargs):
                await w.author.patches.introduce(spec())
                await w.keep(quest())
                raise RuntimeError("broken")

            w.chains.chain.side_effect = broken_builder
        with pytest.raises(asyncio.CancelledError):
            await w.showrunner.run()
        [failed] = w.chronicle.facts("line_failed")
        assert failed["occasion"] == first and "RuntimeError: broken" in failed["why"][0]
        assert any(record.exc_info and "showrunner" in record.message for record in caplog.records)
        if stage == "builder":
            assert failed["characters"] == [OWN] and failed["quests"] == ["DIR_Q1"]
            assert w.showrunner.budget(1, occasion()).people_left == MAX_PEOPLE_PER_CHAPTER - 1
            assert w.showrunner.occasions.pending == [second]
            assert w.chronicle.facts("line_skipped") == []
        else:
            assert any(f.get("occasion") == second for f in w.chronicle.facts())


async def test_run_marks_an_unexpected_handoff_failure_and_takes_the_next_occasion(
    graph, monkeypatch, caplog
):
    async with world(graph, proposal(kind="none")) as w:
        first = occasion(kind="deed", deed={"fact": "found_out", "reason": "Герой раскрыл обман."})
        companion, second = occasion(), occasion(kind="chapter", chapter=2)
        w.showrunner.occasions.pending[:] = [first, companion]
        original = w.showrunner.link.story_state

        async def broken_once(**kwargs):
            monkeypatch.setattr(w.showrunner.link, "story_state", original)
            raise RuntimeError("handoff broken")

        monkeypatch.setattr(w.showrunner.link, "story_state", broken_once)
        failed_once, finished = asyncio.Event(), asyncio.Event()

        def outcome(fact):
            if fact.get("fact") == "line_failed":
                failed_once.set()
            elif fact.get("fact") == "line_none":
                finished.set()

        w.chronicle.on_fact.append(outcome)
        running = asyncio.create_task(w.showrunner.run())
        try:
            await asyncio.wait_for(failed_once.wait(), 1)
            [failed] = w.chronicle.facts("line_failed")
            assert failed["occasion"] == {**first, "also": [companion]}
            assert "RuntimeError: handoff broken" in failed["why"][0]
            assert w.showrunner.occasions.pending == []
            w.clock.t += w.showrunner.occasions.poll_s
            w.showrunner.occasions.pending.append(second)
            w.showrunner.occasions._changed.set()
            await asyncio.wait_for(finished.wait(), 1)
            assert w.chronicle.facts("line_none")[0]["occasion"] == second
            assert w.showrunner.occasions.pending == []
            assert any(record.exc_info for record in caplog.records)
        finally:
            running.cancel()
            with pytest.raises(asyncio.CancelledError):
                await running


@pytest.mark.parametrize("loading", [False, True])
async def test_server_shutdown_cancellation_propagates_even_if_loading_cancelled_the_builder(graph, loading):
    async with world(graph, proposal()) as w:
        started = asyncio.Event()

        async def building(*args, **kwargs):
            started.set()
            await asyncio.Event().wait()

        w.chains.chain.side_effect = building
        w.showrunner.occasions.next = AsyncMock(return_value=occasion())
        running = asyncio.create_task(w.showrunner.run())
        try:
            await asyncio.wait_for(started.wait(), 1)
            if loading:
                await w.showrunner.link._dispatch(Loading(type="loading"))
            running.cancel()
            with pytest.raises(asyncio.CancelledError):
                await asyncio.wait_for(running, 1)
            assert len(w.chronicle.facts("line_failed")) == int(loading)
        finally:
            running.cancel()
            await asyncio.gather(running, return_exceptions=True)


async def test_run_does_not_write_a_late_old_builder_failure_into_the_loaded_branch(
    graph, monkeypatch, caplog
):
    async with world(graph, proposal()) as w:
        started, resume = asyncio.Event(), asyncio.Event()
        old_branch, saved = w.chronicle.branch, await w.chronicle.memory.head(w.chronicle.branch)
        original_next = w.showrunner.occasions.next
        first = True

        async def next_occasion(**kwargs):
            nonlocal first
            if first:
                first = False
                return occasion()
            return await original_next(**kwargs)

        async def late_failure(*args, **kwargs):
            await w.keep(quest())
            started.set()
            # Loading and Session may each cancel this deliberately uncooperative builder.
            while not resume.is_set():
                try:
                    await resume.wait()
                except asyncio.CancelledError:
                    pass
            raise RuntimeError("late old failure")

        monkeypatch.setattr(w.showrunner.occasions, "next", next_occasion)
        w.chains.chain.side_effect = late_failure
        running = asyncio.create_task(w.showrunner.run())
        try:
            await asyncio.wait_for(started.wait(), 1)
            await w.showrunner.link._dispatch(Loading(type="loading"))
            [failed] = w.chronicle.facts("line_failed")
            assert failed["quests"] == ["DIR_Q1"]
            await w.showrunner.link._dispatch(
                Session(type="session", branch="loaded", parent=old_branch, parent_seq=saved, reason="load")
            )
            resume.set()
            # This event follows the cancelled child and the loop's exception handler.
            for _ in range(20):
                if any("late old failure" in str(record.exc_info) for record in caplog.records):
                    break
                await asyncio.sleep(0.01)
            assert any(record.exc_info for record in caplog.records)
            assert w.chronicle.facts("line_failed") == []
            await load(w.fake, w.chronicle, branch=old_branch)
            assert w.chronicle.facts("line_failed") == [failed]
        finally:
            resume.set()
            running.cancel()
            await asyncio.gather(running, return_exceptions=True)


async def test_pitch_uses_the_heros_place_and_budget_without_consuming_the_queue(graph):
    async with world(graph, proposal(kind="none")) as w:
        queued = occasion(kind="chapter")
        w.showrunner.occasions.pending.append(queued)
        result = await w.showrunner.pitch()
        assert result["fact"] == "line_none"
        assert result["occasion"]["place"] == HOME
        assert w.showrunner.occasions.pending == [queued]
        await w.chronicle.record(line(day=10, wall_time=w.clock.t))
        w.clock.t += 1
        result = await w.showrunner.pitch()
        assert result["fact"] == "line_skipped"
        assert result["why"] and w.chronicle.facts("line_skipped") == [result]
        assert w.showrunner.occasions.pending == [queued]
        assert len(w.llm.calls) == 1
