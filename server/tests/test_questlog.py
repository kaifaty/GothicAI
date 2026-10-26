"""Script quest progress is observed without a model and becomes a showrunner occasion."""

import asyncio
from contextlib import asynccontextmanager
from dataclasses import dataclass
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest

from gothic_ai.chronicle import Chronicle
from gothic_ai.game import GameLink
from gothic_ai.gamedata.storygraph import Node, Quest, StoryGraph, Var
from gothic_ai.llm import ToolCall
from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.memory import MemoryStore
from gothic_ai.occasions import Occasions
from gothic_ai.places import PlaceBook
from gothic_ai.questlog import QuestLog
from gothic_ai.showrunner import PROPOSE, Showrunner
from tests.fake_game import FakeGame, connected
from tests.fakes import Clock
from tests.test_memory import close_memory_stores as close_memory_stores

LOBART, HILDA = "BAU_950_LOBART", "BAU_951_HILDA"
QUEST, SECOND = "MIS_LOBART_RUEBEN", "MIS_HILDA_SOUP"
TITLE = "Убрать поле репы"
OFFER, ACCEPT, FINISH = "DIA_LOBART_WORKNOW", "DIA_LOBART_WORKNOW_OK", "DIA_LOBART_RUEBENRUNNING"
GOSSIP, HILDA_ACCEPT = "DIA_LOBART_GOSSIP", "DIA_HILDA_SOUP"


@pytest.fixture
def graph():
    nodes = {
        OFFER: Node(OFFER, "dialog", LOBART, "Я ищу работу.", [{"do": "choice", "node": ACCEPT}], set()),
        ACCEPT: Node(ACCEPT, "choice", LOBART, "Хорошо.", [], {QUEST}, parents=[OFFER]),
        FINISH: Node(FINISH, "dialog", LOBART, "Вот твоя репа!", [], {QUEST}),
        GOSSIP: Node(GOSSIP, "dialog", LOBART, "Что нового?", [], set(), reads={QUEST}),
        HILDA_ACCEPT: Node(HILDA_ACCEPT, "dialog", HILDA, "Я принесу суп.", [], {SECOND}),
    }
    return StoryGraph(
        nodes=nodes,
        functions={},
        instances={},
        vars={
            QUEST: Var({FINISH, GOSSIP}, {ACCEPT, FINISH}, {"B_KAPITELWECHSEL"}),
            SECOND: Var(set(), {HILDA_ACCEPT}, {"ZS_DEAD"}),
        },
        quests={
            QUEST: Quest(QUEST, {"TOPIC_RUEBEN": TITLE}, {ACCEPT}, {FINISH}, set(), set(), set()),
            SECOND: Quest(SECOND, {"TOPIC_SOUP": "Принести суп"}, {HILDA_ACCEPT}, set(), set(), set(), set()),
        },
    )


async def until(condition):
    async with asyncio.timeout(2):
        while not condition():  # noqa: ASYNC110 - polling the fake game's observable state
            await asyncio.sleep(0.001)


@dataclass
class World:
    fake: FakeGame
    link: GameLink
    chronicle: Chronicle
    questlog: QuestLog

    @property
    def facts(self):
        return self.chronicle.facts("vanilla_quest")

    @property
    def reads(self):
        return [m for m in self.fake.received if m["type"] == "story_state"]

    async def chosen(self, node, *, made=False):
        await self.fake.send(type="dialog_chosen", npc=LOBART, node=node, made=made)
        await self.link.evaluate(["int", 1])  # socket barrier: listeners saw dialog_chosen

    async def load(self, *, branch="loaded", head=None, inherit=True):
        parent = self.chronicle.branch if inherit else None
        if head is None:
            head = await self.chronicle.memory.head(parent) if parent else 0
        await self.fake.send(type="loading")
        await self.fake.send(type="session", branch=branch, parent=parent, parent_seq=head, reason="load")
        await self.link.evaluate(["int", 1])
        await until(lambda: len(self.questlog.states) == len(self.questlog.graph.quests))


@asynccontextmanager
async def world(graph, *, initial=0, poll_s=30.0, settle_s=0.005):
    fake = FakeGame(branch="original")
    fake.vars = {QUEST: initial, SECOND: 0, "KAPITEL": 1}
    async with connected(fake) as link:
        chronicle = Chronicle(link, await MemoryStore.open(":memory:"))
        await chronicle.initialize()
        questlog = QuestLog(link, graph, chronicle, poll_s=poll_s, settle_s=settle_s)
        try:
            await until(lambda: len(questlog.states) == len(graph.quests))
            yield World(fake, link, chronicle, questlog)
        finally:
            await questlog.close()


@pytest.mark.parametrize(
    "value,state", [(0, "open"), (1, "running"), (2, "success"), (3, "failed"), (4, "obsolete")]
)
async def test_first_observation_of_every_quest_is_a_baseline_without_facts(graph, value, state):
    async with world(graph, initial=value) as w:
        assert w.questlog.states == {QUEST: state, SECOND: "open"}
        assert w.facts == w.questlog.recent == []
        assert len(w.reads) == 1 and set(w.reads[0]["vars"]) == {QUEST, SECOND}
        await w.questlog.poll()
        assert w.facts == []


@pytest.mark.parametrize("source", ["quest", "vars", "node"])
async def test_a_quest_writer_dialog_triggers_a_settled_read_and_records_its_cause(graph, source):
    # All three graph indexes are valid sources, including nodes absent from quest.movers().
    if source != "quest":
        graph.quests[QUEST].success.clear()
    if source != "vars":
        graph.vars[QUEST].writers.discard(FINISH)
    if source != "node":
        graph.nodes[FINISH].writes.clear()
    async with world(graph, initial=1) as w:
        w.fake.vars[QUEST] = 2
        w.fake.day, w.fake.hour, w.fake.minute = 3, 14, 17
        await w.chosen(FINISH)
        await until(lambda: len(w.facts) == 1)
        assert w.facts == [
            {
                "fact": "vanilla_quest",
                "quest": QUEST,
                "title": TITLE,
                "from": "running",
                "to": "success",
                "day": 3,
                "hour": 14,
                "minute": 17,
                "node": FINISH,
                "npc": LOBART,
                "giver": LOBART,
            }
        ]


@pytest.mark.parametrize("node,made", [(GOSSIP, False), ("DIA_UNKNOWN", False), (FINISH, True)])
async def test_nonwriters_readers_and_authored_dialogs_do_not_trigger_a_read(graph, node, made):
    async with world(graph) as w:
        count = len(w.reads)
        w.fake.vars[QUEST] = 1
        await w.chosen(node, made=made)
        await asyncio.sleep(0.02)
        assert len(w.reads) == count
        assert w.facts == []


@pytest.mark.parametrize("node", [OFFER, ACCEPT])
async def test_a_choice_writer_and_its_parent_dialog_are_both_observed(graph, node):
    async with world(graph) as w:
        w.fake.vars[QUEST] = 1
        await w.chosen(node)
        await until(lambda: len(w.facts) == 1)
        assert (w.facts[0]["from"], w.facts[0]["to"]) == ("open", "running")
        assert (w.facts[0]["node"], w.facts[0]["npc"], w.facts[0]["giver"]) == (node, LOBART, LOBART)


async def test_poll_catches_outside_scripts_and_maps_every_quest_state(graph):
    async with world(graph) as w:
        previous = "open"
        for value, state in [(1, "running"), (2, "success"), (3, "failed"), (4, "obsolete"), (0, "open")]:
            w.fake.vars[QUEST] = value
            await w.questlog.poll()
            change = w.facts[-1]
            assert (change["from"], change["to"]) == (previous, state)
            assert "node" not in change and "npc" not in change
            previous = state
        assert len(w.facts) == 5
        await w.questlog.poll()
        assert len(w.facts) == 5


async def test_periodic_read_catches_a_script_change_without_a_dialog(graph):
    async with world(graph, poll_s=0.01) as w:
        polling = asyncio.create_task(w.questlog.run())
        try:
            w.fake.vars[SECOND] = 1
            await until(lambda: len(w.facts) == 1)
            assert w.facts[0]["quest"] == SECOND
            assert (w.facts[0]["from"], w.facts[0]["to"]) == ("open", "running")
            assert "node" not in w.facts[0] and "npc" not in w.facts[0]
        finally:
            polling.cancel()
            await asyncio.gather(polling, return_exceptions=True)


async def test_zero_stays_silent_until_the_quest_changes(graph):
    async with world(graph) as w:
        await w.questlog.poll()
        await w.chosen(ACCEPT)  # selecting a writer whose condition did not take effect changes nothing
        await asyncio.sleep(0.02)
        assert w.facts == []
        w.fake.vars[QUEST] = 1
        await w.questlog.poll()
        assert len(w.facts) == 1
        assert (w.facts[0]["from"], w.facts[0]["to"]) == ("open", "running")


@pytest.mark.parametrize("value,state,extra", [(2, "success", 0), (3, "failed", 1)])
async def test_load_uses_the_latest_inherited_fact_and_records_only_a_missed_change(
    graph, value, state, extra
):
    async with world(graph) as w:
        for progress in (1, 2):
            w.fake.vars[QUEST] = progress
            await w.questlog.poll()
        original = w.facts
        w.fake.vars[QUEST] = value
        await w.load()
        assert w.questlog.states[QUEST] == state
        assert w.facts[:2] == original
        assert len(w.facts) == 2 + extra
        if extra:
            assert (w.facts[-1]["from"], w.facts[-1]["to"]) == ("success", "failed")
            assert "node" not in w.facts[-1] and "npc" not in w.facts[-1]
        await w.questlog.poll()
        assert len(w.facts) == 2 + extra


async def test_only_facts_before_the_saved_head_form_the_loaded_baseline(graph):
    async with world(graph) as w:
        w.fake.vars[QUEST] = 1
        await w.questlog.poll()
        saved = await w.chronicle.memory.head(w.chronicle.branch)
        first = w.facts[0]
        w.fake.vars[QUEST] = 2
        await w.questlog.poll()
        w.fake.vars[QUEST] = 1
        await w.load(head=saved)
        assert w.facts == [first]
        assert w.questlog.states[QUEST] == "running"


async def test_a_new_branch_without_inherited_facts_takes_a_new_baseline(graph):
    async with world(graph) as w:
        w.fake.vars[QUEST] = 1
        await w.questlog.poll()
        w.fake.vars[QUEST] = 2
        await w.load(inherit=False)
        assert w.questlog.states[QUEST] == "success"
        assert w.facts == []


@pytest.mark.parametrize("other_npc,unique", [(LOBART, True), (HILDA, False)])
async def test_giver_is_present_only_when_all_offering_nodes_belong_to_one_npc(graph, other_npc, unique):
    other = "DIA_SECOND_OFFER"
    graph.nodes[other] = Node(other, "dialog", other_npc, "Есть работа.", [], {QUEST})
    graph.quests[QUEST].running.add(other)
    async with world(graph) as w:
        w.fake.vars[QUEST] = 1
        await w.questlog.poll()
        if unique:
            assert w.facts[0]["giver"] == LOBART
        else:
            assert "giver" not in w.facts[0]


async def test_a_read_from_before_loading_cannot_update_the_new_branch(graph, monkeypatch):
    async with world(graph) as w:
        original = w.link.story_state
        received, resume = asyncio.Event(), asyncio.Event()
        delayed = True

        async def delayed_state(**kwargs):
            nonlocal delayed
            state = await original(**kwargs)
            if delayed:
                delayed = False
                received.set()
                await resume.wait()
            return state

        monkeypatch.setattr(w.link, "story_state", delayed_state)
        w.fake.vars[QUEST] = 1
        reading = asyncio.create_task(w.questlog.poll())
        try:
            await asyncio.wait_for(received.wait(), 1)
            w.fake.vars[QUEST] = 2
            loading = asyncio.create_task(w.load())
            await until(lambda: w.chronicle.branch == "loaded")
            resume.set()
            await asyncio.wait_for(reading, 1)
            await asyncio.wait_for(loading, 1)
            assert w.questlog.states[QUEST] == "success"
            assert w.facts == []
            w.fake.vars[QUEST] = 3
            await w.questlog.poll()
            assert (w.facts[0]["from"], w.facts[0]["to"]) == ("success", "failed")
        finally:
            resume.set()
            reading.cancel()
            await asyncio.gather(reading, return_exceptions=True)


async def test_a_periodic_poll_waits_for_dialog_effects_and_preserves_the_dialog_cause(graph, monkeypatch):
    async with world(graph, initial=1, poll_s=0.005, settle_s=0.08) as w:
        queried = asyncio.Event()
        original = w.link.story_state

        async def observed_state(**kwargs):
            queried.set()
            return await original(**kwargs)

        monkeypatch.setattr(w.link, "story_state", observed_state)
        await w.chosen(FINISH)
        polling = asyncio.create_task(w.questlog.run())
        try:
            await asyncio.sleep(0.02)
            assert not queried.is_set()
            w.fake.vars[QUEST] = 2
            await until(lambda: len(w.facts) == 1)
            assert w.facts[0]["node"] == FINISH and w.facts[0]["npc"] == LOBART
            assert (w.facts[0]["from"], w.facts[0]["to"]) == ("running", "success")
        finally:
            polling.cancel()
            await asyncio.gather(polling, return_exceptions=True)


def places():
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
                for i, (name, kind) in enumerate([("ROAD", "road"), ("HOME", "house")])
            ],
            "points": {"ROAD_1": 0, "HOME_1": 1},
        }
    )


def people(fake):
    fake.people = [
        {
            "npc": npc,
            "name": name,
            "routine": [{"start": 0, "end": 1440, "state": "ZS_STAND", "waypoint": "HOME_1"}],
        }
        for npc, name in [(LOBART, "Лобарт"), (HILDA, "Хильда")]
    ]
    fake.npc_states = {
        LOBART: {"state": "alive", "name": "Лобарт", "distance": 200},
        HILDA: {"state": "alive", "name": "Хильда", "distance": 6000},
    }


async def test_quest_changes_become_vanilla_occasions_between_deeds_and_chapters(graph):
    async with world(graph) as w:
        clock = Clock()
        people(w.fake)
        w.fake.nearest_waypoint = "ROAD_1"
        occasions = Occasions(w.link, w.chronicle, places(), clock=clock, settle_s=0, gather_s=0)
        await occasions.poll()
        w.fake.day += 1
        await occasions.poll()  # quiet
        w.fake.nearest_waypoint = "HOME_1"
        w.fake.vars["KAPITEL"] = 2
        clock.t += 30
        w.fake.vars[QUEST] = 1
        await w.chosen(ACCEPT)
        await until(lambda: len(w.facts) == 1)
        await w.chronicle.record({"fact": "found_out", "quest": "MIS_LIE"})
        await occasions.poll()  # arrived, chapter, the quest change, deed
        expected = ["deed", "vanilla", "chapter", "arrived", "quiet"]
        assert [f["kind"] for f in occasions.pending] == expected
        vanilla = occasions.pending[1]
        assert vanilla["vanilla"] == w.facts[0]
        assert (vanilla["place"], vanilla["waypoint"], vanilla["people"]) == ("HOME", "HOME_1", [LOBART])
        current = await occasions.next()
        assert [current["kind"], *(f["kind"] for f in current["also"])] == expected
        assert occasions.pending == []


async def test_showrunner_decide_accepts_a_real_vanilla_occasion_without_special_logic(graph):
    async with world(graph) as w:
        people(w.fake)
        w.fake.nearest_waypoint = "HOME_1"
        occasions = Occasions(w.link, w.chronicle, places(), settle_s=0, gather_s=0)
        await occasions.poll()
        await occasions.next()  # first arrival
        w.fake.vars[QUEST] = 1
        await w.questlog.poll()
        await occasions.poll()
        current = await occasions.next()
        assert current["kind"] == "vanilla" and current["vanilla"] == w.facts[0]
        author = SimpleNamespace(
            book=SimpleNamespace(all=AsyncMock(return_value={})),
            rules=SimpleNamespace(plot_npc=lambda npc: False),
            graph=graph,
            cards={},
            old={},
            quest=AsyncMock(),
        )
        chains = SimpleNamespace(chain=AsyncMock())
        casting = SimpleNamespace(
            cast=SimpleNamespace(characters=AsyncMock(side_effect=lambda: []), cards=lambda: {})
        )
        llm = FakeBackend(
            script=[FakeStep("", [ToolCall(PROPOSE, {"kind": "none", "reason": "Пока тишина."}, "p1")])],
            delay=0,
        )
        showrunner = Showrunner(occasions, author, chains, casting, llm)
        result = await showrunner.decide(current)
        assert result["fact"] == "line_none" and result["occasion"] == current
        assert len(llm.calls) == 1
        prompt = "\n".join(getattr(item, "content", "") for item in llm.calls[0][1])
        assert '"kind": "vanilla"' in prompt and QUEST in prompt and TITLE in prompt
        author.quest.assert_not_called()
        chains.chain.assert_not_called()
