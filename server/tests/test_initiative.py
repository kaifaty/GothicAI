import asyncio
import json
import random
from collections.abc import AsyncIterator
from contextlib import asynccontextmanager
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any

import pytest

from gothic_ai.bodies import Bodies
from gothic_ai.brain import GameApplier, NpcPlanner, PlannerConfig
from gothic_ai.brain.planner import NpcRecord
from gothic_ai.budget import Budget
from gothic_ai.chronicle import Chronicle
from gothic_ai.concerns import Concerns
from gothic_ai.crimes import Crimes
from gothic_ai.deals import Deals
from gothic_ai.dialogue import DialogueManager
from gothic_ai.gamedata.storygraph import StoryGraph, load
from gothic_ai.initiative import CHECKINFO, TOOLS, ImportantApproach, InitiativeDesk
from gothic_ai.journal import Journal
from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.memory import MemoryStore
from gothic_ai.perception import PerceptionDesk
from gothic_ai.persona import PersonaProvider
from gothic_ai.protocol import NpcActResult
from gothic_ai.storyguard import StoryGuard
from gothic_ai.talk import TalkDesk
from gothic_ai.trace import TraceWriter, read
from tests.fake_game import FakeGame, connected
from tests.fakes import Clock, FakeDecider
from tests.test_dialogue import XARDAS, _until
from tests.test_director import _node
from tests.test_planner import step
from tests.test_talk import person

LOBART, MALETH, VINO, GUARD = "BAU_950_LOBART", "BAU_954_MALETH", "BAU_952_VINO", "MIL_310_STADTWACHE"
WORDS = "Долг за тобой, парень."
# the hero borrowed 25 gold till the end of day 1 (0, as the game counts), and it is day 1: a broken word
KEY = "debt_broken:d1"
OWED = "герой не вернул тебе долг в срок: 25 золотых до конца дня 1"
AGENDA = "make the hero pay back the 25 gold he owes you; his day has passed"
REMEMBERS = "You already spoke to the hero about it today at {}."
RUEBEN = ["==", ["var", "MIS_LOBART_RUEBEN"], ["int", 0]]  # he has not asked the hero about turnips yet
WORKED = [
    "==",
    ["var", "LOBART_RENT_DUE"],
    ["int", 1],
]  # what a permanent important dialog waits for: a rent due


@dataclass
class Scene:
    fake: FakeGame
    store: MemoryStore
    llm: FakeBackend
    dialogue: DialogueManager
    chronicle: Chronicle
    deals: Deals
    concerns: Concerns
    clock: Clock  # the planner's
    decider: FakeDecider
    applier: GameApplier
    planner: NpcPlanner
    talk: TalkDesk
    budget: Budget
    trace: TraceWriter
    desk: InitiativeDesk
    journal: list[dict[str, Any]]


@asynccontextmanager
async def _scene(
    tmp_path: Path,
    llm: FakeBackend | None = None,
    *,
    graph: StoryGraph | None = None,
    budget: Budget | None = None,
    with_crimes: bool = False,
) -> AsyncIterator[Scene]:
    """The parts as cli.py wires them, around the fake game (branch A, day 1, 08:00). The behavioral model
    picks approach_talk; an npc's soft act ends only when the test says so (npc_idle). The budget's clock
    stands: nothing refills."""
    fake, store = FakeGame(branch="A"), MemoryStore(":memory:")
    llm = llm or FakeBackend(WORDS, delay=0)
    journal: list[dict[str, Any]] = []
    trace = TraceWriter(tmp_path)
    async with connected(fake) as link:
        dialogue = DialogueManager(
            link, llm, PersonaProvider(None), memory=store, journal=Journal(journal.append), trace=trace
        )
        chronicle = Chronicle(link, store)
        crimes = Crimes(link, chronicle) if with_crimes else None
        deals = Deals(link, dialogue, chronicle, crimes=crimes)
        dialogue.extensions.append(deals)
        concerns = Concerns(link, chronicle, deals)
        clock, decider, applier = Clock(), FakeDecider("approach_talk", 0.9), GameApplier(link)
        planner = NpcPlanner(
            decider,
            applier,
            config=PlannerConfig(ask_attitude=False),
            clock=clock,
            rng=random.Random(7),
            agenda_of=concerns.agenda,
            trace=trace,
        )
        link.listeners.append(planner.on_message)
        concerns.on_change.append(lambda npc: planner.set_agenda(npc, concerns.agenda(npc)))
        talk = TalkDesk(link, dialogue, clock=clock, planner=planner)  # one clock, as cli.py's monotonic one
        budget = budget if budget is not None else Budget(per_npc_per_min=2, clock=Clock())
        desk = InitiativeDesk(
            link,
            dialogue,
            talk,
            planner,
            concerns,
            StoryGuard(graph),
            budget,
            chronicle,
            bodies=Bodies(link),
            perception=PerceptionDesk(link),
            journal=dialogue.journal,
        )
        yield Scene(
            fake,
            store,
            llm,
            dialogue,
            chronicle,
            deals,
            concerns,
            clock,
            decider,
            applier,
            planner,
            talk,
            budget,
            trace,
            desk,
            journal,
        )


def _owes(s: Scene, npc: str = LOBART, name: str = "Лобарт") -> None:
    """The hero did not pay back the 25 gold he borrowed of the npc: a debt written past the deals, as the e2e
    seeds one, broken on day 1."""
    debt = {"fact": "debt", "id": "d1", "npc": npc, "npc_name": name, "due": 0, "day": 0}
    s.chronicle.record({**debt, "item": "ITMI_GOLD", "count": 25, "name": "Золото"})
    s.deals.reload()
    s.deals.overdue(1)


async def _around(s: Scene, *people: dict) -> None:
    """Who is around the hero (npc_roster), once the server knows it."""
    await s.fake.roster(*people)
    want = {p["id"]: p["distance"] for p in people}
    await _until(lambda: {npc: e.distance for npc, e in s.talk.people.items()} == want)


async def _near(s: Scene, npc: str = LOBART) -> None:
    """The hero is close: the behavioral model is asked what the npc does now."""
    s.planner.on_event(npc, "player_near", s.clock.t, distance_cm=s.planner.npcs[npc].distance_cm)
    await step(s.planner, s.clock, 0.3)  # past the debounce


async def _walks(s: Scene, n: int) -> None:
    """Until the game got the n-th soft act."""
    await _until(lambda: len(s.fake.acts) == n)


async def _idle(s: Scene, reason: str = "done") -> None:
    """The bridge tells the latest soft act is over (npc_idle)."""
    act = s.fake.acts[-1]
    await s.fake.send(type="npc_idle", npc=act["npc"], act=act["id"], reason=reason)


async def _said(s: Scene, *lines: str, npc: str = LOBART) -> None:
    """Until the npc said its lines and its turn is over."""

    def over() -> bool:
        conv = s.dialogue.conversations.get(npc)
        reply = conv.reply if conv is not None else None
        return s.fake.said == list(lines) and reply is not None and reply.done()

    await _until(over)


async def _clock(s: Scene, hour: int, minute: int, day: int = 1) -> None:
    """The game's time: what story_state tells and what the views it pushes once a second carry."""
    s.fake.day, s.fake.hour, s.fake.minute = day, hour, minute
    at = day * 1440 + hour * 60 + minute
    await s.fake.push_views({"npc": LOBART, "name": "Лобарт", "time": at})
    await _until(lambda: s.desk.now == at)


async def _comes_up(s: Scene, *others: dict) -> None:
    """Lobart, 8 m off, walks up to the hero (3 m) and speaks first; `others` are around once it is there."""
    await _around(s, person(LOBART, "Лобарт", 8))
    await _near(s)
    await _walks(s, 1)
    await _around(s, person(LOBART, "Лобарт", 3), *others)
    await _idle(s)
    await _said(s, WORDS)


def _skipped(s: Scene) -> list[str]:
    """Why the desk let walk-ups go (the journal)."""
    return [e["why"] for e in s.journal if e["kind"] == "initiative_skipped"]


async def test_rule_approach_leaves_existing_important_dialog_to_native_perception(tmp_path):
    async with _scene(tmp_path) as s:
        await _around(s, person(LOBART, "Лобарт", 5))
        approach = ImportantApproach(s.desk.link, s.desk.bodies, s.talk, s.desk.guard, s.chronicle)
        told = False
        task = asyncio.create_task(
            approach.approach(LOBART, "crime:C1", valid=lambda: True, told=lambda: told)
        )
        try:
            await _walks(s, 1)
            assert s.fake.acts[-1]["action"] == "go_to_npc"
            # Native B_AssessPlayer can take the NPC into TALK before its soft walk finishes.
            await _around(s, person(LOBART, "Лобарт", 2, activity="dialog", state="ZS_TALK"))
            told = True
            await _idle(s, "gone")
            assert await task
            assert s.chronicle.facts("initiative")[-1]["occasion"] == "crime:C1"
            assert not s.fake.debug_calls and not s.llm.calls and not s.fake.attached
        finally:
            await approach.close()


async def test_rule_approach_clears_only_its_unfinished_walk_when_claim_is_removed(tmp_path):
    async with _scene(tmp_path) as s:
        await _around(s, person(LOBART, "Лобарт", 5))
        approach = ImportantApproach(s.desk.link, s.desk.bodies, s.talk, s.desk.guard, s.chronicle)
        valid = True
        task = asyncio.create_task(
            approach.approach(LOBART, "crime:C1", valid=lambda: valid, told=lambda: False)
        )
        try:
            await _walks(s, 1)
            valid = False
            assert not await task
            assert any(
                m.get("exprs") == [["call", "NPC_CLEARAIQUEUE", ["inst", "SELF"]]]
                for m in s.fake.story
            )
            assert not s.chronicle.facts("initiative") and not s.fake.debug_calls
        finally:
            await approach.close()


async def test_rule_approach_load_does_not_clear_ai_of_new_world(tmp_path):
    async with _scene(tmp_path) as s:
        await _around(s, person(LOBART, "Лобарт", 5))
        approach = ImportantApproach(s.desk.link, s.desk.bodies, s.talk, s.desk.guard, s.chronicle)
        task = asyncio.create_task(
            approach.approach(LOBART, "crime:C1", valid=lambda: True, told=lambda: False)
        )
        try:
            await _walks(s, 1)
            await s.fake.send(type="loading")
            assert not await task
            assert not any(
                m.get("exprs") == [["call", "NPC_CLEARAIQUEUE", ["inst", "SELF"]]]
                for m in s.fake.story
            )
        finally:
            await approach.close()


@pytest.mark.parametrize("rejected", [False, True])
async def test_rule_approach_releases_native_talk_gate_only_after_completed_walk(tmp_path, rejected):
    async with _scene(tmp_path) as s:
        await _around(s, person(LOBART, "Лобарт", 5))
        approach = ImportantApproach(s.desk.link, s.desk.bodies, s.talk, s.desk.guard, s.chronicle)
        accepted: list[str] = []
        entered = False
        exited = False
        told = False

        def receipt(message: Any) -> None:
            if isinstance(message, NpcActResult) and message.status == "queued":
                accepted.append(message.id)

        @asynccontextmanager
        async def release() -> AsyncIterator[None]:
            nonlocal entered, exited, told
            assert accepted
            entered = True
            told = True
            try:
                yield
            finally:
                exited = True

        s.desk.link.listeners.append(receipt)
        if rejected:
            s.fake.busy_npcs.add(LOBART)
        try:
            task = asyncio.create_task(
                approach.approach(
                    LOBART, "crime:C1", valid=lambda: True, told=lambda: told, release=release
                )
            )
            await _walks(s, 1)
            if not rejected:
                await _until(lambda: bool(accepted))
                assert not entered
                await _idle(s)
            result = await task
            assert result is not rejected
            assert entered is not rejected and exited is not rejected
            assert bool(s.chronicle.facts("initiative")) is not rejected
        finally:
            s.desk.link.listeners.remove(receipt)
            await approach.close()


@pytest.mark.parametrize("start,distance", [(8, 2), (8, 5), (3, 3)])
async def test_rule_approach_releases_at_native_dialog_range_without_walk_idle(tmp_path, start, distance):
    async with _scene(tmp_path) as s:
        s.fake.vars["PERC_DIST_DIALOG"] = 500
        await _around(s, person(LOBART, "Лобарт", start))
        approach = ImportantApproach(s.desk.link, s.desk.bodies, s.talk, s.desk.guard, s.chronicle)
        entered, restore = asyncio.Event(), asyncio.Event()
        valid, told = True, False

        @asynccontextmanager
        async def release() -> AsyncIterator[None]:
            entered.set()
            try:
                yield
            finally:
                restore.set()

        task = asyncio.create_task(approach.approach(
            LOBART, "crime:C1", valid=lambda: valid, told=lambda: told, release=release
        ))
        try:
            await _walks(s, 1)
            await asyncio.sleep(0)
            if start > 5:
                assert not entered.is_set()
            # Terrain can prevent the last metres of AI_GotoNpc. No npc_idle arrives.
            await _around(s, person(LOBART, "Лобарт", distance))
            await asyncio.wait_for(entered.wait(), 1)
            await _around(s, person(LOBART, "Лобарт", distance, activity="dialog", state="ZS_TALK"))
            told = True
            assert await task
            assert restore.is_set()
            assert s.chronicle.facts("initiative")[-1]["occasion"] == "crime:C1"
            assert not s.desk.bodies.busy(LOBART)
            assert not any(m.get("exprs") == [["call", "NPC_CLEARAIQUEUE", ["inst", "SELF"]]]
                           for m in s.fake.story)
        finally:
            valid = False
            await asyncio.gather(task, return_exceptions=True)
            await approach.close()


async def test_rule_approach_keeps_gate_released_while_native_talk_is_still_playing(tmp_path, monkeypatch):
    monkeypatch.setattr("gothic_ai.initiative.IMPORTANT_WAIT_S", 0.05)
    async with _scene(tmp_path) as s:
        s.fake.vars["PERC_DIST_DIALOG"] = 500
        await _around(s, person(LOBART, "Лобарт", 3))
        approach = ImportantApproach(s.desk.link, s.desk.bodies, s.talk, s.desk.guard, s.chronicle)
        entered, restored = asyncio.Event(), asyncio.Event()
        told = False

        @asynccontextmanager
        async def release() -> AsyncIterator[None]:
            entered.set()
            try:
                yield
            finally:
                restored.set()

        task = asyncio.create_task(approach.approach(
            LOBART, "crime:C1", valid=lambda: True, told=lambda: told, release=release
        ))
        try:
            await asyncio.wait_for(entered.wait(), 1)
            await _around(s, person(LOBART, "Лобарт", 3, activity="dialog", state="ZS_TALK"))
            await asyncio.sleep(0.4)  # beyond the perception deadline and one WATCH_S poll
            assert not task.done() and not restored.is_set()
            told = True
            assert await task
            assert restored.is_set()
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
            await approach.close()


@pytest.mark.parametrize("cancel", ["crime_resolved", "talk_ended"])
async def test_rule_approach_restores_released_gate_when_cancelled_before_told(tmp_path, cancel):
    async with _scene(tmp_path) as s:
        s.fake.vars["PERC_DIST_DIALOG"] = 500
        await _around(s, person(LOBART, "Лобарт", 3))
        approach = ImportantApproach(s.desk.link, s.desk.bodies, s.talk, s.desk.guard, s.chronicle)
        entered, restore = asyncio.Event(), asyncio.Event()
        valid = True

        @asynccontextmanager
        async def release() -> AsyncIterator[None]:
            entered.set()
            try:
                yield
            finally:
                restore.set()

        task = asyncio.create_task(approach.approach(
            LOBART, "crime:C1", valid=lambda: valid, told=lambda: False, release=release
        ))
        try:
            await asyncio.wait_for(entered.wait(), 1)
            if cancel == "talk_ended":
                await _around(s, person(LOBART, "Лобарт", 3, activity="dialog", state="ZS_TALK"))
                await asyncio.sleep(0.4)  # let one WATCH_S poll observe the started TALK
                await _around(s, person(LOBART, "Лобарт", 3))
            else:
                valid = False
            assert not await asyncio.wait_for(task, 1)
            assert restore.is_set() and not s.chronicle.facts("initiative")
            assert not s.desk.bodies.busy(LOBART)
        finally:
            valid = False
            await asyncio.gather(task, return_exceptions=True)
            await approach.close()


@pytest.mark.parametrize(
    "entry,partner",
    [
        (person(LOBART, "Лобарт", 4, state="ZS_SLEEP"), None),
        (person(LOBART, "Лобарт", 4, activity="fight"), None),
        (person(GUARD, "Страж", 8, state="ZS_GUARD_PASSAGE"), None),
        (person(LOBART, "Лобарт", 4), MALETH),
    ],
)
async def test_rule_approach_respects_sleep_busy_posts_and_other_conversation(tmp_path, entry, partner):
    async with _scene(tmp_path) as s:
        await _around(s, entry)
        if partner is not None:
            s.talk.begin(partner, "Малет")
        approach = ImportantApproach(s.desk.link, s.desk.bodies, s.talk, s.desk.guard, s.chronicle)
        try:
            assert not await approach.approach(
                entry["id"], "crime:C1", valid=lambda: True, told=lambda: False
            )
            assert not s.fake.acts and not s.fake.debug_calls
        finally:
            await approach.close()


def _decided(s: Scene) -> list[tuple[str, str, str]]:
    """The behavioral model's answers as the planner traced them: (npc, choice, why it was not applied)."""
    path = s.trace.path()
    rows = read(path) if path.exists() else []
    return [(r["npc"], r["choice"], r["why"]) for r in rows if r["type"] == "brain"]


@pytest.fixture
def graph(tmp_path: Path) -> StoryGraph:
    """Dialogs cut down: Lobart's greeting is important until told (while he has not asked about turnips), his
    PERM is important and permanent: it waits whenever its condition holds (a rent due), told or not; his
    work is not important; Maleth's greeting has a condition the fake world cannot compute; Vino has none."""
    knows = ["call", "NPC_KNOWSINFO", ["inst", "OTHER"], ["sym", "DIA_LOBART_HALLO"]]
    nodes = [
        _node("DIA_LOBART_HALLO", LOBART, [], important=True, condition=RUEBEN),
        _node("DIA_LOBART_PERM", LOBART, [], important=True, permanent=True, condition=WORKED),
        _node("DIA_LOBART_WORK", LOBART, [], condition=None),
        _node("DIA_MALETH_HALLO", MALETH, [], important=True, condition=knows),
    ]
    data = {"version": 1, "nodes": nodes, "functions": {}, "instances": {}, "vars": {}, "quests": []}
    path = tmp_path / "story.json"
    path.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")
    return load(path)


async def test_the_lender_walks_up_and_speaks_first(tmp_path):
    async with _scene(tmp_path) as s:
        _owes(s)
        await _around(s, person(LOBART, "Лобарт", 8))
        assert s.planner.npcs[LOBART].agenda == AGENDA  # its concern is what it wants of the hero
        await _near(s)
        await _walks(s, 1)
        assert s.talk.partner is None and not s.llm.calls  # on its way: nothing said yet
        # Maleth sees it come up, Vino looks elsewhere
        seen = {"id": LOBART, "name": "Лобарт", "distance": 600}
        await s.fake.push_views(
            {"npc": MALETH, "name": "Малет", "time": 1920, "sees": [seen]}, {"npc": VINO, "name": "Вино"}
        )
        await _around(s, person(LOBART, "Лобарт", 3))
        await _idle(s)
        await _said(s, WORDS)

    # the desk walked it, the applier sent no second go_to_npc
    assert [(m["npc"], m["action"], m["args"]) for m in s.fake.acts] == [(LOBART, "go_to_npc", {})]
    assert _decided(s) == [(LOBART, "approach_talk", "initiative desk")] and not s.applier.recent
    assert s.desk.walks == 1 and s.desk.active is None
    # the hero's partner, stopped and turned to him
    assert s.talk.partner.npc == LOBART and s.talk.partner.posture == "listen"
    assert s.talk.held == {LOBART: {"listen"}}
    turned = [(m["npc"], m["action"]) for m in s.fake.received if m["type"] == "action"]
    assert turned == [(LOBART, "turn_to")]
    # the first words: on the occasion, with the conversation's tools, a deal among them
    ((_, request, tools),) = s.llm.calls
    cue = request[-1].content
    assert cue.startswith("[Ты сам подошёл к герою, он тебя не звал.") and f"Повод: {OWED}." in cue
    assert "25 золотых" in cue
    assert "- d1: 25 золотых до конца дня 1 — срок прошёл" in request[-2].content  # repay it by its id
    names = {t.name for t in tools}
    assert names <= set(TOOLS) and "propose_deal" in names
    (turn,) = [r for r in read(s.trace.path()) if r["type"] == "turn"]
    assert turn["trigger"] == {"kind": "initiative", "text": cue}
    # who came up, why, and who saw it
    assert s.chronicle.facts("initiative") == [
        {
            "fact": "initiative",
            "npc": LOBART,
            "name": "Лобарт",
            "occasion": KEY,
            "why": OWED,
            "where": None,
            "seen_by": ["PLAYER", MALETH],
            "day": 1,
            "hour": 8,
            "minute": 0,
        }
    ]
    assert [(e["npc"], e["occasion"]) for e in s.journal if e["kind"] == "initiative"] == [(LOBART, KEY)]
    assert s.budget.allow(LOBART) and not s.budget.allow(LOBART)  # one call: one of its two tokens is gone


async def test_a_witness_can_approach_about_a_case_only_once(tmp_path):
    async with _scene(tmp_path, with_crimes=True) as s:
        s.chronicle.record({
            "fact": "crime", "id": "C1", "kind": "theft", "victim": MALETH, "owner": MALETH,
            "item": "ITFO_BREAD", "count": 1, "day": 1, "hour": 8, "minute": 0,
            "community": "farm", "waypoint": "FARM1_IN",
        })
        s.chronicle.record({"fact": "crime_seen", "npc": LOBART, "crime": "C1", "guild": 8, "npctype": 0})
        await _comes_up(s)
        (fact,) = s.chronicle.facts("initiative")
        assert fact["occasion"] == "crime_witness:C1"
        assert "промолчать" in s.llm.calls[0][1][-1].content
        assert "silence" in s.llm.calls[0][1][-1].content
        # The concern remains for the conversation; this case never prompts another approach,
        # including after a day and after reloading the saved initiative facts.
        assert s.concerns.top(LOBART).key == "crime_witness:C1"
        s.talk.on_npc_end(LOBART)
        await _until(lambda: s.planner.npcs[LOBART].activity == "routine")
        s.desk.now += 1440
        s.desk._load()
        assert s.desk.allow(s.planner.npcs[LOBART], "approach_talk") == "уже говорил об этом деле"
        assert s.desk.walks == 1
        original = s.chronicle.facts("crime")[0]
        s.chronicle.record({**original, "id": "C2"})
        s.chronicle.record({"fact": "crime_seen", "npc": LOBART, "crime": "C2", "guild": 8, "npctype": 0})
        assert s.desk.occasion(LOBART).key == "crime_witness:C2"
        assert s.desk.allow(s.planner.npcs[LOBART], "approach_talk") is None


async def test_the_heros_next_words_go_to_it(tmp_path):
    llm = FakeBackend(script=[FakeStep(WORDS), FakeStep("Давно бы так.")], delay=0)
    async with _scene(tmp_path, llm) as s:
        _owes(s)
        # Maleth is in front of the hero and nearer: without a partner his words would go to Maleth
        await _comes_up(s, person(MALETH, "Малет", 2, True))
        await s.fake.send(type="player_text", text="Ну, чего тебе?")
        await _until(lambda: len(s.llm.calls) == 2)

    to, text = s.talk.last
    assert (to.id, to.why, text) == (LOBART, "partner", "Ну, чего тебе?")
    request = s.llm.calls[1][1]
    assert request[-1].content == "Ну, чего тебе?"
    said = [getattr(m, "content", None) for m in request]
    assert WORDS in said and any(c.startswith("[Ты сам подошёл к герою") for c in said if c)  # one talk


async def test_no_budget_no_walk(tmp_path):
    async with _scene(tmp_path, budget=Budget(per_npc_per_min=0, total_per_min=0)) as s:
        _owes(s)
        await _around(s, person(LOBART, "Лобарт", 8))
        await _near(s)
        assert _decided(s) == [(LOBART, "approach_talk", "initiative desk")]
        await _until(lambda: s.desk.active is None)

    assert _skipped(s) == ["budget"]
    assert s.fake.acts == [] and s.desk.walks == 0  # it does not come up only to stand there
    assert not s.llm.calls and s.chronicle.facts("initiative") == []


async def test_pending_important_dialog_keeps_it_away(tmp_path):
    async with _scene(tmp_path) as s:
        _owes(s)
        s.fake.important = {LOBART}  # the scripts have an important dialog for the hero (Npc_CheckInfo)
        await _around(s, person(LOBART, "Лобарт", 8))
        await _near(s)
        await _until(lambda: s.desk.active is None)
        assert _skipped(s) == ["important"] and s.fake.acts == []
        assert [m["exprs"] for m in s.fake.story if m["type"] == "story_eval"] == [[CHECKINFO]]

        # the game has told it (its own talk): next time it comes up itself
        s.fake.important.clear()
        s.clock.t += 121  # approach_talk's cooldown
        await _near(s)
        await _walks(s, 1)
        await _around(s, person(LOBART, "Лобарт", 3))
        await _idle(s)
        await _said(s, WORDS)

    assert [m["action"] for m in s.fake.acts] == ["go_to_npc"]
    assert s.budget.allow(LOBART) and not s.budget.allow(LOBART)  # only the words took a token


async def test_checkinfo_unknown_falls_back_to_the_graph(tmp_path, graph, monkeypatch):
    async with _scene(tmp_path, graph=graph) as s:
        evaluate = s.fake.evaluate

        def unknown(e, npc=None):  # the game could not tell (null): the graph's dialogs tell
            return None if e == CHECKINFO else evaluate(e, npc)

        monkeypatch.setattr(s.fake, "evaluate", unknown)
        assert await s.desk.important(LOBART)
        asked = [m["exprs"] for m in s.fake.story if m["type"] == "story_eval"]
        assert asked == [[CHECKINFO], [RUEBEN, WORKED]]  # not the ordinary one
        s.fake.vars["MIS_LOBART_RUEBEN"] = 1
        assert not await s.desk.important(LOBART)  # its condition no longer holds
        s.fake.vars.clear()
        s.fake.known.add("DIA_LOBART_HALLO")
        assert not await s.desk.important(LOBART)  # told already
        assert s.fake.story[-1]["exprs"] == [WORKED]  # only the permanent one is left to evaluate
        s.fake.vars["LOBART_RENT_DUE"] = 1
        assert await s.desk.important(LOBART)  # the permanent one waits again: its condition holds
        assert await s.desk.important(MALETH)  # a condition the game cannot compute: let it speak first
        assert not await s.desk.important(VINO)  # nothing important to tell
        s.desk.guard = StoryGuard(None)
        assert await s.desk.important(LOBART)  # nothing to tell by: let the game speak


async def test_not_during_a_dialog(tmp_path):
    async with _scene(tmp_path) as s:
        _owes(s)
        s.fake.in_dialog = True  # the hero is in a dialog of the scripts
        await _around(s, person(LOBART, "Лобарт", 8))
        await _near(s)
        await _until(lambda: s.desk.active is None)

    assert _skipped(s) == ["dialog"] and s.fake.acts == [] and not s.llm.calls
    assert not [m for m in s.fake.story if m["type"] == "story_eval"]  # nothing more is asked of the game
    assert s.budget.allow(LOBART) and s.budget.allow(LOBART)  # nor a token taken


async def test_not_while_the_hero_talks_to_another(tmp_path):
    async with _scene(tmp_path, FakeBackend("Растёт помаленьку.", delay=0)) as s:
        _owes(s)
        await _around(s, person(LOBART, "Лобарт", 8), person(MALETH, "Малет", 3, True))
        await s.fake.send(type="player_text", text="Малет, как урожай?")
        await _said(s, "Растёт помаленьку.", npc=MALETH)
        assert s.talk.partner.npc == MALETH
        await _near(s)  # Lobart overheard him, and wants his money

    assert _decided(s) == [(LOBART, "approach_talk", "veto: герой говорит с другим")]
    assert s.planner.stats.vetoed == 1 and s.desk.active is None
    assert s.fake.acts == [] and s.desk.walks == 0 and len(s.llm.calls) == 1  # Maleth's answer only


async def test_plot_npc_only_close(tmp_path):
    async with _scene(tmp_path) as s:
        _owes(s, XARDAS, "Ксардас")
        await _around(s, person(XARDAS, "Ксардас", 12, True))
        await _near(s, XARDAS)
        assert _decided(s) == [(XARDAS, "approach_talk", "veto: сюжетный: только слова")]
        assert s.planner.stats.vetoed == 1 and s.fake.acts == []

        # the hero is close: it turns to him where it stands and speaks
        s.clock.t += 121  # the veto keeps it from being asked the same at once
        await _around(s, person(XARDAS, "Ксардас", 4, True))
        await _near(s, XARDAS)
        await _walks(s, 1)
        await _idle(s)
        await _said(s, WORDS, npc=XARDAS)

    assert [(m["npc"], m["action"]) for m in s.fake.acts] == [(XARDAS, "turn_to")]  # not a step from its spot
    assert s.talk.partner.npc == XARDAS and s.talk.held == {XARDAS: {"listen"}}
    assert [f["occasion"] for f in s.chronicle.facts("initiative")] == [KEY]


async def test_guard_at_post_does_not_walk(tmp_path):
    post = {"state": "ZS_GUARD_PASSAGE"}  # his dialogs warn and attack by where he stands
    async with _scene(tmp_path) as s:
        _owes(s, GUARD, "Стражник")
        asleep = person(VINO, "Вино", 6, state="ZS_SLEEP")
        await _around(s, person(GUARD, "Стражник", 8, True, **post), asleep)
        await _near(s, GUARD)
        assert _decided(s) == [(GUARD, "approach_talk", "veto: стоит на посту")]
        assert s.desk.allow(s.planner.npcs[VINO], "approach_talk") == "спит"  # nor does one asleep come

        s.clock.t += 121
        await _around(s, person(GUARD, "Стражник", 4, True, **post))
        await _near(s, GUARD)
        await _walks(s, 1)
        await _idle(s)
        await _said(s, WORDS, npc=GUARD)

    assert [(m["npc"], m["action"]) for m in s.fake.acts] == [(GUARD, "turn_to")]
    assert s.talk.partner.npc == GUARD and [f["npc"] for f in s.chronicle.facts("initiative")] == [GUARD]


async def test_it_did_not_get_there(tmp_path):
    async with _scene(tmp_path, budget=Budget(clock=Clock())) as s:
        _owes(s)
        await _around(s, person(LOBART, "Лобарт", 8))
        # struck down or taken over on the way; the bridge gave it up; it stopped short of the hero
        for n, (reason, metres) in enumerate([("gone", 3), ("timeout", 3), ("done", 9)], 1):
            await _near(s)
            await _walks(s, n)
            await _around(s, person(LOBART, "Лобарт", metres))
            await _idle(s, reason)
            await _until(lambda: s.desk.active is None)
            s.clock.t += 121
        s.fake.busy_npcs.add(LOBART)  # the game will not have it walk
        await _near(s)
        await _walks(s, 4)
        await _until(lambda: s.desk.active is None)

    assert _skipped(s) == [
        "missed: gone, 3 м",
        "missed: timeout, 3 м",
        "missed: done, 9 м",
        "missed: rejected, 9 м",
    ]
    assert s.desk.walks == 4 and not s.llm.calls and s.chronicle.facts("initiative") == []
    assert s.talk.partner is None and s.talk.held == {}


async def test_hero_spoke_first_while_it_walked(tmp_path):
    async with _scene(tmp_path, FakeBackend("Ага, вот и ты.", delay=0)) as s:
        _owes(s)
        await _around(s, person(LOBART, "Лобарт", 8))
        await _near(s)
        await _walks(s, 1)
        await _around(s, person(LOBART, "Лобарт", 3))
        # it comes up and the hero speaks first, before the game tells the walk is over (the fake bridge
        # keeps tracking the walk of an npc that is held)
        await s.fake.send(type="player_text", text="Лобарт, чего тебе?")
        await _said(s, "Ага, вот и ты.")
        await _idle(s)
        await _until(lambda: s.desk.active is None)

    history = [m.content for m in s.dialogue.conversations[LOBART].history]
    assert f"[Ты сам шёл к герою: {OWED}]" in history  # it knows why it came
    ((_, request, _),) = s.llm.calls  # his words only: no turn of the desk's
    assert request[-1].content == "Лобарт, чего тебе?"
    assert s.chronicle.facts("initiative") == [] and _skipped(s) == []
    assert s.talk.partner.npc == LOBART


@pytest.mark.parametrize("moving", [False, True])
async def test_hero_spoke_first_on_its_way(tmp_path, moving):
    """As the bridge has it: held to listen to him, its walk ends as gone; walking, it keeps working and a
    look replaces the walk, whose end never comes. Either way it knows what it came for, at once."""
    async with _scene(tmp_path, FakeBackend("Ага, вот и ты.", delay=0)) as s:
        s.fake.track_acts = True
        _owes(s)
        await _around(s, person(LOBART, "Лобарт", 8))
        await _near(s)
        await _walks(s, 1)
        await _around(s, {**person(LOBART, "Лобарт", 6), "moving": moving})
        await s.fake.send(type="player_text", text="Лобарт, чего тебе?")
        await _said(s, "Ага, вот и ты.")
        await _until(lambda: s.desk.active is None, 2)  # not the walk's 35 s

    assert (LOBART in s.fake.attached) is not moving  # held to listen, or kept working
    assert len(s.llm.calls) == 1 and s.chronicle.facts("initiative") == [] and _skipped(s) == []
    history = [m.content for m in s.dialogue.conversations[LOBART].history]
    assert f"[Ты сам шёл к герою: {OWED}]" in history


async def test_one_per_occasion_per_game_hour_and_the_facts_line(tmp_path):
    async with _scene(tmp_path) as s:
        _owes(s)
        before = s.store.head("A")
        await _comes_up(s)  # day 1, 08:00
        saved = s.store.head("A")
        at_8 = REMEMBERS.format("08:00")
        assert s.desk.facts(LOBART) == (at_8,) and s.desk.facts(MALETH) == ()
        s.talk.on_npc_end(LOBART)  # its goodbye: back to its day
        await _until(lambda: s.planner.npcs[LOBART].activity == "routine")

        # half an hour on it is asked again: it remembers, and the desk holds it back
        await _clock(s, 8, 30)
        s.clock.t += 121  # the cooldown after a talk
        await _near(s)
        assert s.decider.calls[-1]["state"].endswith(f"What you want: {AGENDA} What you remember: {at_8}")
        assert _decided(s)[-1] == (LOBART, "approach_talk", "veto: уже говорил об этом")
        assert s.desk.walks == 1

        # a game hour on: whether to bring it up again is its character's
        await _clock(s, 9, 1)
        s.clock.t += 121
        await _near(s)
        await _walks(s, 2)
        await _idle(s)
        await _said(s, WORDS, WORDS)
        assert [(f["hour"], f["minute"]) for f in s.chronicle.facts("initiative")] == [(8, 0), (9, 1)]
        assert s.desk.facts(LOBART) == (REMEMBERS.format("09:01"),)
        await _clock(s, 7, 0, day=2)
        assert s.desk.facts(LOBART) == ()  # that was yesterday

        # a load of a save made after it first came up: what it spoke of comes back with the facts
        await s.fake.send(type="session", branch="B", parent="A", parent_seq=saved, reason="load")
        await _until(lambda: s.chronicle.branch == "B")
        assert s.desk.spoken == {(LOBART, KEY): 1920} and s.desk.now is None
        await _clock(s, 8, 20)
        assert s.desk.facts(LOBART) == (at_8,)
        # and of one made before: it never did
        await s.fake.send(type="session", branch="C", parent="A", parent_seq=before, reason="load")
        await _until(lambda: s.chronicle.branch == "C")
        assert s.desk.spoken == {}
        await _clock(s, 8, 20)
        assert s.desk.facts(LOBART) == () and s.concerns.agenda(LOBART) == AGENDA


async def test_other_choices_wait_while_it_walks(tmp_path):
    async with _scene(tmp_path) as s:
        s.fake.track_acts = True
        walk_act = s.desk.bodies.act
        s.desk.bodies.act = lambda npc, action, **args: walk_act(npc, action, timeout=1.5, **args)  # not 35 s
        _owes(s)
        await _around(s, person(LOBART, "Лобарт", 8, True))
        await _near(s)
        await _walks(s, 1)
        # another act would replace the walk in the bridge, and its npc_idle would never come
        s.decider.choice = "look_at"
        await _near(s)
        assert _decided(s)[-1] == (LOBART, "look_at", "veto: идёт к герою")
        # a wolf: the body is the threat's
        s.decider.choice = "flee"
        wolf = {"other": "WOLF", "other_name": "Волк", "distance_cm": 600}
        s.planner.on_event(LOBART, "monster_near", s.clock.t, **wolf)
        await step(s.planner, s.clock)
        await _walks(s, 2)
        assert _decided(s)[-1] == (LOBART, "flee", "")
        # the bridge follows the flight now: the walk is never told over, so it ends once the flight is
        # queued (bodies.py), not after its timeout
        await _until(lambda: s.desk.active is None, 1.0)

    assert [(m["action"], m["args"]) for m in s.fake.acts] == [("go_to_npc", {}), ("flee", {"npc": "WOLF"})]
    assert [d.choice for d in s.applier.recent] == ["flee"] and s.planner.stats.vetoed == 1
    assert _skipped(s) == ["missed: gone, 8 м"] and not s.llm.calls


async def test_who_may_not_come_up(tmp_path):
    async with _scene(tmp_path) as s:
        rec = NpcRecord(MALETH, "Малет", distance_cm=600, in_view=True)
        assert s.desk.allow(rec, "approach_talk") is None
        assert s.desk.allow(replace(rec, activity="dialog"), "approach_talk") == "занят"
        assert (
            s.desk.allow(replace(rec, distance_cm=2500, in_view=False), "approach_talk") == "герой не услышит"
        )
        s.desk.active = LOBART  # one at a time
        assert s.desk.allow(rec, "approach_talk") == "к герою уже идут"
        assert s.desk.allow(rec, "look_at") is None  # only the walker's own choices wait


async def test_without_occasion_it_only_walks_up(tmp_path):
    async with _scene(tmp_path) as s:
        await _around(s, person(LOBART, "Лобарт", 8))
        assert s.desk.occasion(LOBART) is None and s.planner.npcs[LOBART].agenda is None
        await _near(s)
        await _walks(s, 1)
        await _around(s, person(LOBART, "Лобарт", 3))
        await _idle(s)
        await _until(lambda: s.desk.active is None)

    assert [(m["npc"], m["action"]) for m in s.fake.acts] == [(LOBART, "go_to_npc")]  # it came up, as before
    # the game was asked first: a silent walk-up starts its talk too
    assert [m["exprs"] for m in s.fake.story if m["type"] == "story_eval"] == [[CHECKINFO]]
    assert not s.llm.calls and s.chronicle.facts("initiative") == [] and _skipped(s) == []
    assert s.talk.partner is None and s.talk.held == {} and LOBART not in s.fake.attached
    assert s.budget.allow(LOBART) and s.budget.allow(LOBART)  # no words, no budget
    # what its talks left it wanting is an occasion too
    occ = s.desk.occasion(LOBART, "get the hero to find my stolen sheep")
    assert occ is not None and occ.key.startswith("agenda:")
    assert occ.ru == "«get the hero to find my stolen sheep»"


async def test_one_gone_to_bed_does_not_come(tmp_path):
    async with _scene(tmp_path) as s:
        # every sleep of the routines is ZS_GOTOBED, not ZS_SLEEP
        await _around(s, person(VINO, "Вино", 6, True, state="ZS_GOTOBED"), person(MALETH, "Малет", 6, True))
        assert s.desk.allow(s.planner.npcs[VINO], "approach_talk") == "спит"
        assert s.desk.allow(s.planner.npcs[MALETH], "approach_talk") is None


async def _up_while_two_talk(s: Scene) -> None:
    """Lobart walks up to the hero while two others talk next to him (gossip.py's floor)."""
    _owes(s)
    await _around(s, person(LOBART, "Лобарт", 8))
    await _near(s)
    await _walks(s, 1)
    await _around(s, person(LOBART, "Лобарт", 3))
    await _idle(s)


async def test_it_waits_for_two_talking_next_to_the_hero(tmp_path):
    async with _scene(tmp_path) as s:
        talking = True
        s.desk.floor_busy = lambda: talking
        await _up_while_two_talk(s)
        await asyncio.sleep(0.6)  # a few of its looks at the floor
        assert s.desk.active == LOBART and not s.llm.calls and s.talk.partner is None
        talking = False  # their scene is over: now it speaks
        await _said(s, WORDS)

    assert _skipped(s) == [] and s.talk.partner.npc == LOBART
    assert [f["npc"] for f in s.chronicle.facts("initiative")] == [LOBART]
    assert s.budget.allow(LOBART) and not s.budget.allow(LOBART)  # the words took one token, the wait none


async def test_still_talking_it_lets_it_go(tmp_path, monkeypatch):
    monkeypatch.setattr("gothic_ai.initiative.FLOOR_WAIT_S", 0.5)
    async with _scene(tmp_path) as s:
        s.desk.floor_busy = lambda: True
        await _up_while_two_talk(s)
        await _until(lambda: s.desk.active is None)

    assert _skipped(s) == ["рядом болтают"]
    assert not s.llm.calls and s.talk.partner is None and s.talk.held == {}
    assert s.chronicle.facts("initiative") == []
    assert s.budget.allow(LOBART) and s.budget.allow(LOBART)  # no token taken


async def test_the_hero_walks_off_while_it_waits(tmp_path):
    async with _scene(tmp_path) as s:
        talking = True
        s.desk.floor_busy = lambda: talking
        await _up_while_two_talk(s)
        await asyncio.sleep(0.3)  # it came up and waits
        await _around(s, person(LOBART, "Лобарт", 21))  # he left: that also ends their scene (hero_left)
        talking = False
        await _until(lambda: s.desk.active is None)

    assert _skipped(s) == ["missed: waited, 21 м"]
    assert not s.llm.calls and s.talk.partner is None and s.talk.held == {}
    assert s.budget.allow(LOBART) and s.budget.allow(LOBART)  # no token taken


async def test_the_hero_speaks_to_it_while_it_waits(tmp_path):
    async with _scene(tmp_path, FakeBackend("Ага, вот и ты.", delay=0)) as s:
        s.desk.floor_busy = lambda: True
        await _up_while_two_talk(s)
        await s.fake.send(type="player_text", text="Лобарт, чего тебе?")
        await _said(s, "Ага, вот и ты.")
        await _until(lambda: s.desk.active is None)

    history = [m.content for m in s.dialogue.conversations[LOBART].history]
    assert f"[Ты сам шёл к герою: {OWED}]" in history  # it knows why it came
    assert len(s.llm.calls) == 1 and _skipped(s) == [] and s.chronicle.facts("initiative") == []
