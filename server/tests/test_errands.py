import asyncio
from types import SimpleNamespace
from typing import Any

import pytest

import gothic_ai.errands
from gothic_ai.bodies import BodyOutcome
from gothic_ai.errands import Errands, checked_items
from gothic_ai.gates import Gates
from gothic_ai.journal import Journal
from gothic_ai.llm import ToolCall
from gothic_ai.meetings import DAY
from gothic_ai.protocol import InventoryItem, RosterEntry
from gothic_ai.social import Rumor, Spot
from tests.test_dialogue import _until
from tests.test_gates import CITY, GatesFake, setup_gate
from tests.test_guide import BOOK

LOBART, ANDRE, PECK, HILDA = "BAU_950_LOBART", "MIL_311_ANDRE", "MIL_332_PECK", "BAU_951_HILDA"
MIL, FARMER = 2, 5
NOW = 2 * DAY + 19 * 60  # day 3 as the npc sees it, 19:00
DAWN = {"do": "tell", "who": "Андре", "item": "", "count": 0, "day": 4, "hour": 7, "minute": 0}
BARRACKS = (60000.0, 0.0, 0.0)  # 600 m from the farm: 300 game minutes of a walk


@pytest.fixture(autouse=True)
def quick(monkeypatch):
    monkeypatch.setattr(gothic_ai.errands, "TICK_S", 0.01)
    monkeypatch.setattr(gothic_ai.errands, "TELL_EVERY_S", 0.01)


def _all_day(state: str, waypoint: str, pos: tuple[float, float, float]) -> Spot:
    return Spot(0, 0, state, waypoint, pos)


class _Social:
    def __init__(self) -> None:
        self.names = {LOBART: "Лобарт", ANDRE: "Андре", PECK: "Пек", HILDA: "Хильда"}
        self.guilds = {LOBART: FARMER, ANDRE: MIL, PECK: MIL, HILDA: FARMER}
        self.ties: dict[str, dict[str, Any]] = {LOBART: {ANDRE: object(), HILDA: object()}}
        self.days = {
            LOBART: _all_day("ZS_STAND_GUARDING", "NW_FARM1_OUT_01", (0.0, 0.0, 0.0)),
            HILDA: _all_day("ZS_COOK_STOVE", "NW_FARM1_INHOUSE_01", (500.0, 0.0, 0.0)),
            ANDRE: _all_day("ZS_STAND_ARMSCROSSED", "NW_CITY_KASERN_01", BARRACKS),
            PECK: _all_day("ZS_STAND_GUARDING", "NW_CITY_KASERN_02", (60500.0, 0.0, 0.0)),
        }

    def spot(self, npc: str, minute: int) -> Spot | None:
        return self.days.get(npc.upper())

    def spots(self):
        return iter(self.days.items())


class _Rumors:
    def __init__(self) -> None:
        self.social = _Social()
        self.dead: set[str] = set()
        lie = Rumor("MIS_LOBART_RUEBEN", "caught", LOBART, "Лобарт", "репа в амбаре", known={LOBART: 0})
        self.rumors = {lie.key: lie}
        self.told: list[tuple[str, str]] = []

    def tell(self, r: Rumor, a: str, b: str) -> str:
        if b in r.known:
            return "no"
        self.told.append((a, b))
        r.known[b] = r.known[a] + 1
        return "told"


class _Link:
    def __init__(self) -> None:
        self.listeners: list[Any] = []
        gold = InventoryItem(item="ITMI_GOLD", name="Золото", count=30)
        letter = InventoryItem(item="ITWR_CANTHARS_KOMPROBRIEF_MIS", name="Письмо", count=1, mission=True)
        beet = InventoryItem(item="ITPL_BEET", name="Репа", count=5)
        world = SimpleNamespace(day=2, hour=19, minute=0)
        self.snapshots: dict[str, Any] = {
            LOBART: SimpleNamespace(npc=SimpleNamespace(pos=(0.0, 0.0, 0.0), inventory=[]), world=world),
            HILDA: SimpleNamespace(npc=SimpleNamespace(pos=(500.0, 0.0, 0.0), inventory=[gold, letter, beet]),
                                   world=world),
        }  # fmt: skip
        self.player: Any = SimpleNamespace(pos=(300.0, 0.0, 0.0))
        self.nearby: dict[str, list[dict[str, Any]]] = {}
        self.effects: list[tuple[str, list[dict[str, Any]], bool]] = []

    async def snapshot(self, npc: str, fresh: bool = False) -> Any:
        snap = self.snapshots[npc]
        return SimpleNamespace(npc=snap.npc, world=snap.world, player=self.player)

    async def story_nearby(self, npc: str, radius: int = 2000) -> list[dict[str, Any]]:
        return self.nearby.get(npc, [])

    async def story_effect(self, npc: str, effects: list[dict[str, Any]], *, check: bool = False) -> None:
        self.effects.append((npc, effects, check))


class _Bodies:
    def __init__(self) -> None:
        self.acts: list[tuple[str, str, dict[str, Any]]] = []
        self.end: asyncio.Future[str] | None = None

    async def act(self, npc: str, action: str, /, **args: Any) -> BodyOutcome:
        self.acts.append((npc, action, args))
        self.end = asyncio.get_running_loop().create_future()
        return BodyOutcome("n1", npc, await self.end)  # type: ignore[arg-type]


class _Chronicle:
    def __init__(self, facts: list[dict[str, Any]] | None = None) -> None:
        self.all = list(facts or [])

    def record(self, fact: dict[str, Any]) -> None:
        self.all.append(fact)

    def facts(self, kind: str | None = None) -> list[dict[str, Any]]:
        return [f for f in self.all if kind is None or f.get("fact") == kind]


class _Guide:
    book = BOOK

    async def free(self, npc: str) -> str | None:
        return "тебе нельзя уходить отсюда (сюжетный: только слова)" if npc.startswith("NONE_") else None


def _desk(facts: list[dict[str, Any]] | None = None, graph: Any = None):
    link, bodies, rumors, chronicle = _Link(), _Bodies(), _Rumors(), _Chronicle(facts)
    talk = SimpleNamespace(people={LOBART: RosterEntry(id=LOBART, name="Лобарт", distance=300)}, partner=None)
    notes: list[str] = []
    dialogue = SimpleNamespace(journal=Journal(), remember=lambda npc, name, text: notes.append(text))
    errands = Errands(
        link,
        dialogue,
        talk,
        _Guide(),
        chronicle,
        bodies,
        rumors=lambda: rumors,
        graph=graph,  # type: ignore[arg-type]
    )
    return errands, link, talk, bodies, rumors, chronicle, notes


async def test_what_an_errand_may_be():
    errands, link, *_ = _desk()
    snap = link.snapshots[LOBART]
    goal, why = await errands.plan(LOBART, "Лобарт", DAWN, snap)
    assert goal is not None and why == ""
    assert (goal.to, goal.start, goal.title) == (ANDRE, 3 * DAY + 7 * 60, "репа в амбаре")
    assert (
        errands.situation(LOBART)
        == "Ты собрался в день 4, 07:00: рассказать о герое (репа в амбаре) — кому: Андре."
    )
    busy = (await errands.plan(LOBART, "Лобарт", {**DAWN, "who": "Пек"}, snap))[1]
    assert busy.startswith("у тебя уже есть поручение")
    other, *_ = _desk()
    assert (await other.plan(LOBART, "Лобарт", {**DAWN, "who": "Ворон"}, snap))[
        1
    ] == "ты не знаешь, кто такой «Ворон»"
    assert (await other.plan(LOBART, "Лобарт", {**DAWN, "day": 3, "hour": 6}, snap))[1].startswith(
        "это время уже"
    )
    assert (await other.plan(HILDA, "Хильда", DAWN, snap))[1] == (
        "Андре уже знает все новости, которые ты можешь рассказать"
    )
    plot = (await other.plan("NONE_100_XARDAS", "Ксардас", DAWN, snap))[1]
    assert plot == "тебе нельзя уходить отсюда (сюжетный: только слова)"


async def test_in_front_of_the_hero_it_walks_there_and_tells_him():
    errands, link, talk, bodies, rumors, chronicle, notes = _desk()
    goal, _ = await errands.plan(LOBART, "Лобарт", DAWN, link.snapshots[LOBART])
    assert goal is not None
    errands.tick(goal.start - 1)
    assert goal.state == "agreed" and bodies.acts == []
    link.player = SimpleNamespace(pos=(59000.0, 0.0, 0.0))  # the hero waits by the barracks
    talk.people.clear()
    errands.tick(goal.start)  # it sets off
    await _until(lambda: bodies.end is not None)
    npc, action, args = bodies.acts[0]
    assert (npc, action, args["waypoint"], args["arrive"]) == (
        LOBART,
        "start_activity",
        "NW_CITY_KASERN_01",
        True,
    )
    assert errands.allow(SimpleNamespace(id=LOBART), "plan_2") == "идёт по поручению"  # type: ignore[arg-type]
    link.nearby[LOBART] = [{"npc": ANDRE, "name": "Андре", "distance": 300, "guild": MIL}]
    assert bodies.end is not None
    bodies.end.set_result("arrived")
    await _until(lambda: goal.state == "done")
    assert rumors.told == [(LOBART, ANDRE)]
    done = chronicle.facts("goal_done")[0]
    assert (done["goal"], done["to"], done["how"]) == (goal.id, ANDRE, "near") and "instead_of" not in done
    assert notes[-1] == "Ты рассказал о герое (репа в амбаре) — кому: Андре"


async def test_the_fork_another_militiaman_hears_it_instead():
    errands, link, _, bodies, rumors, chronicle, _ = _desk()
    goal, _ = await errands.plan(LOBART, "Лобарт", DAWN, link.snapshots[LOBART])
    assert goal is not None
    errands.tick(goal.start)
    await _until(lambda: bodies.end is not None)
    link.nearby[LOBART] = [{"npc": PECK, "name": "Пек", "distance": 400, "guild": MIL}]  # no Andre
    assert bodies.end is not None
    bodies.end.set_result("arrived")
    await _until(lambda: goal.state == "done")
    assert rumors.told == [(LOBART, PECK)]
    assert chronicle.facts("goal_done")[0]["instead_of"] == ANDRE


async def test_nobody_there_it_waits_and_gives_up():
    errands, link, _, bodies, _, chronicle, notes = _desk()
    goal, _ = await errands.plan(LOBART, "Лобарт", DAWN, link.snapshots[LOBART])
    assert goal is not None
    errands.tick(goal.start)
    await _until(lambda: bodies.end is not None)
    assert bodies.end is not None
    bodies.end.set_result("arrived")
    await _until(lambda: goal.state == "there")
    errands.tick(goal.start + 30)  # 30 minutes later, still nobody
    await _until(lambda: goal.state == "failed")
    assert chronicle.facts("goal_failed")[0]["why"] == "не застал на месте: Андре"
    assert notes[-1].startswith("Поручение не вышло: рассказать о герое (репа в амбаре) — кому: Андре")


async def test_far_from_the_hero_nothing_walks_the_fact_comes_when_it_would_be_there():
    errands, link, talk, bodies, rumors, chronicle, _ = _desk()
    goal, _ = await errands.plan(LOBART, "Лобарт", DAWN, link.snapshots[LOBART])
    assert goal is not None
    link.player = SimpleNamespace(pos=(-90000.0, 0.0, 0.0))  # the hero is far from both
    talk.people.clear()
    errands.tick(goal.start)
    await _until(lambda: goal.state == "away")
    assert goal.arrive == goal.start + 300
    errands.tick(goal.start + 299)
    assert rumors.told == []
    errands.tick(goal.start + 300)
    assert rumors.told == [(LOBART, ANDRE)] and bodies.acts == []
    assert chronicle.facts("goal_done")[0]["how"] == "far"


async def test_far_the_fork_by_the_routines():
    errands, link, talk, bodies, rumors, chronicle, _ = _desk()
    rumors.social.days[ANDRE] = _all_day("ZS_SLEEP", "NW_CITY_KASERN_01", BARRACKS)
    goal, _ = await errands.plan(LOBART, "Лобарт", DAWN, link.snapshots[LOBART])
    assert goal is not None
    link.player = SimpleNamespace(pos=(-90000.0, 0.0, 0.0))
    talk.people.clear()
    errands.tick(goal.start)
    await _until(lambda: goal.state == "away")
    errands.tick(goal.arrive)
    assert rumors.told == [(LOBART, PECK)] and chronicle.facts("goal_done")[0]["instead_of"] == ANDRE


@pytest.mark.parametrize("how", ["near", "far"])
async def test_gate_news_uses_the_existing_plan_and_saved_goal(how):
    errands, link, _, _, rumors, chronicle, _ = _desk()
    delivered = []
    errands.gate_news = lambda npc, to: ("gate:vouch:lothar:1", "Я поручился за героя")

    async def gate_told(key, npc, to):
        delivered.append((key, npc, to))

    errands.gate_told = gate_told
    goal, why = await errands.plan(LOBART, "Лобарт", DAWN, link.snapshots[LOBART])
    assert goal is not None and not why
    assert (goal.rumor, goal.title) == ("gate:vouch:lothar:1", "Я поручился за героя")
    restored, *_ = _desk(chronicle.facts())
    assert restored.goals[goal.id].rumor == goal.rumor
    if how == "near":
        await errands._near(goal, ANDRE)
    else:
        goal.arrive = goal.start + 300
        goal.state = "away"
        errands.tick(goal.arrive)
        errands.tick(goal.arrive + 1)
        await _until(lambda: goal.state == "done")
    assert delivered == [(goal.rumor, LOBART, ANDRE)]
    assert rumors.told == []
    assert chronicle.facts("goal_done")[0]["to"] == ANDRE
    assert chronicle.facts("goal_done")[0]["how"] == how


async def test_delivered_vouch_does_not_hide_vouchers_other_news_to_that_guard():
    guard = "MIL_310_STADTWACHE"
    errands, link, _, _, rumors, chronicle, _ = _desk([
        {"fact": "gate_vouch", "id": "andre-vouch", "npc": ANDRE, "name": "Андре",
         "at": NOW, "expires": NOW + 2 * DAY},
        {"fact": "gate_vouch_heard", "vouch": "andre-vouch", "from": ANDRE, "to": guard},
    ])
    gates = Gates.__new__(Gates)
    gates.chronicle, gates._now = chronicle, NOW
    errands.gate_news = gates.news
    rumors.social.names[guard] = "Стражник"
    rumors.social.guilds[guard] = MIL
    rumors.social.days[guard] = _all_day("ZS_STAND_GUARDING", "GATE_POST", BARRACKS)
    rumors.social.ties[ANDRE] = {guard: object()}
    [rumor] = rumors.rumors.values()
    rumor.known[ANDRE] = 1

    goal, why = await errands.plan(ANDRE, "Андре", {**DAWN, "who": guard}, link.snapshots[LOBART])

    assert goal is not None and not why
    assert (goal.rumor, goal.title) == (rumor.key, rumor.title)


async def test_vouch_and_plan_in_one_reply_deliver_the_vouch_before_after_speech(monkeypatch):
    lothar = "PAL_203_LOTHAR"
    async with setup_gate(GatesFake()) as system:
        link, dialogue, chronicle, gates, _, _ = system
        rumors = _Rumors()
        rumors.social.names.update({lothar: "Лотар", CITY: "Стражник"})
        rumors.social.guilds.update({lothar: 1, CITY: MIL})
        rumors.social.days[CITY] = _all_day("ZS_STAND_GUARDING", "GATE_POST", BARRACKS)
        errands = Errands(
            link, dialogue, SimpleNamespace(people={}, partner=None), _Guide(), chronicle, _Bodies(),
            rumors=lambda: rumors,
        )
        errands.gate_news, errands.gate_told = gates.news, gates.told
        errands.now = DAY + 8 * 60
        jobs = []
        monkeypatch.setattr(dialogue, "after_speech", lambda conv, job: jobs.append(job))
        conv = dialogue.open(lothar, "Лотар")

        outcome = await gates.handle(
            dialogue, conv, ToolCall("vouch_for_hero", {"note": "Я отвечаю за него."}, "vouch"), None,
        )
        goal, why = await errands.plan(lothar, "Лотар", {"do": "tell", "who": CITY}, None)

        assert outcome is not None and outcome.status == "accepted"
        assert goal is not None and not why
        [vouch] = chronicle.facts("gate_vouch")
        assert goal.rumor == f"gate:vouch:{vouch['id']}"
        assert not jobs
        assert not chronicle.facts("gate_vouch_heard")

        await errands._near(goal, CITY)

        [heard] = chronicle.facts("gate_vouch_heard")
        assert (heard["vouch"], heard["from"], heard["to"]) == (vouch["id"], lothar, CITY)
        assert goal.state == "done" and not rumors.told


async def test_gate_news_near_waits_for_its_exact_addressee_instead_of_another_guard():
    errands, link, _, bodies, rumors, chronicle, _ = _desk()
    delivered = []
    errands.gate_news = lambda npc, to: ("gate:claim:guard:1", "Герой сослался на тебя")

    async def gate_told(key, npc, to):
        delivered.append((key, npc, to))

    errands.gate_told = gate_told
    goal, _ = await errands.plan(LOBART, "Лобарт", DAWN, link.snapshots[LOBART])
    assert goal is not None
    errands.tick(goal.start)
    await _until(lambda: bodies.end is not None)
    link.nearby[LOBART] = [{"npc": PECK, "name": "Пек", "distance": 400, "guild": MIL}]
    assert bodies.end is not None
    bodies.end.set_result("arrived")
    await _until(lambda: goal.state == "there")
    errands.tick(goal.start + 30)
    await _until(lambda: goal.state == "failed")
    assert not delivered and not rumors.told and not chronicle.facts("goal_done")


async def test_gate_news_far_cannot_be_delivered_to_a_substitute_guard():
    errands, link, talk, _, rumors, chronicle, _ = _desk()
    delivered = []
    errands.gate_news = lambda npc, to: ("gate:vouch:lothar:1", "Я поручился за героя")

    async def gate_told(key, npc, to):
        delivered.append((key, npc, to))

    errands.gate_told = gate_told
    rumors.social.days[ANDRE] = _all_day("ZS_SLEEP", "NW_CITY_KASERN_01", BARRACKS)
    goal, _ = await errands.plan(LOBART, "Лобарт", DAWN, link.snapshots[LOBART])
    assert goal is not None
    link.player = SimpleNamespace(pos=(-90000.0, 0.0, 0.0))
    talk.people.clear()
    errands.tick(goal.start)
    await _until(lambda: goal.state == "away")
    errands.tick(goal.arrive)
    assert goal.state == "failed"
    assert not delivered and not rumors.told and not chronicle.facts("goal_done")


async def test_hilda_takes_lobart_the_gold_and_never_a_mission_item():
    graph = SimpleNamespace(
        nodes={"DIA_LOBART_RUEBEN": SimpleNamespace(
            npc=LOBART, effects=[], condition_effects=[],
            condition=["call", "NPC_HASITEMS", ["inst", "SELF"], ["sym", "ITPL_BEET"]],
        )},
        npc=lambda name: name,
    )  # fmt: skip
    errands, link, talk, bodies, _, chronicle, _ = _desk(graph=graph)
    snap = link.snapshots[HILDA]
    give = {"do": "give", "who": "Лобарт", "item": "ITMI_GOLD", "count": 25, "day": 0, "hour": 0, "minute": 0}
    letter = (
        await errands.plan(
            HILDA, "Хильда", {**give, "item": "ITWR_CANTHARS_KOMPROBRIEF_MIS", "count": 1}, snap
        )
    )[1]
    assert letter == "Письмо — вещь для задания: такие не отдают"
    beet = (await errands.plan(HILDA, "Хильда", {**give, "item": "ITPL_BEET", "count": 5}, snap))[1]
    assert beet == "Репа по сюжету должна быть у Лобарт: её не отдают"
    assert (await errands.plan(HILDA, "Хильда", {**give, "count": 40}, snap))[
        1
    ] == "у тебя только 30 шт.: Золото"
    goal, why = await errands.plan(HILDA, "Хильда", give, snap)
    assert goal is not None and why == ""
    talk.people[HILDA] = RosterEntry(id=HILDA, name="Хильда", distance=400)
    link.nearby[HILDA] = [{"npc": LOBART, "name": "Лобарт", "distance": 500, "guild": FARMER}]
    errands.tick(NOW)  # now: Lobart is right by her, no walk first
    await _until(lambda: bodies.end is not None)
    assert bodies.acts[0] == (
        HILDA,
        "give",
        {"npc": LOBART, "item": "ITMI_GOLD", "count": 25, "timeout": 120.0},
    )
    assert bodies.end is not None
    bodies.end.set_result("done")
    await _until(lambda: goal.state == "done")
    gave = chronicle.facts("gave")[0]
    assert (gave["from"], gave["to"], gave["item"], gave["count"]) == (HILDA, LOBART, "ITMI_GOLD", 25)
    assert "PLAYER" in gave["seen_by"]


async def test_a_give_far_from_the_hero_is_the_bridge_s_effect():
    errands, link, talk, _, _, chronicle, _ = _desk()
    give = {"do": "give", "who": "Андре", "item": "ITMI_GOLD", "count": 10, "day": 0, "hour": 0, "minute": 0}
    goal, _ = await errands.plan(HILDA, "Хильда", give, link.snapshots[HILDA])
    assert goal is not None
    link.player = SimpleNamespace(pos=(-90000.0, 0.0, 0.0))
    talk.people.clear()
    errands.tick(NOW)
    await _until(lambda: goal.state == "away")
    errands.tick(goal.arrive)
    await _until(lambda: goal.state == "done")
    effect = {"do": "give", "item": "ITMI_GOLD", "count": 10, "to": "npc", "npc": ANDRE}
    assert link.effects == [(HILDA, [effect], True)]
    assert chronicle.facts("gave")[0]["seen_by"] == []


async def test_a_load_brings_the_goals_back():
    errands, link, *_ = _desk()
    goal, _ = await errands.plan(LOBART, "Лобарт", DAWN, link.snapshots[LOBART])
    assert goal is not None
    again, *_ = _desk(errands.chronicle.facts())  # type: ignore[attr-defined]
    back = again.goals[goal.id]
    assert (back.state, back.to, back.start, back.rumor) == ("agreed", ANDRE, goal.start, goal.rumor)
    done, *_ = _desk([*errands.chronicle.facts(), {"fact": "goal_done", "goal": goal.id}])  # type: ignore[attr-defined]
    assert done.goals[goal.id].state == "done"


def test_the_graph_s_checked_things_but_gold():
    graph = SimpleNamespace(
        nodes={"A": SimpleNamespace(
            npc="VLK_469_HALVOR",
            effects=[{"when": ["call", "NPC_HASITEMS", ["inst", "SELF"], ["sym", "ITMI_GOLD"]]}],
            condition_effects=[],
            condition=["&&", ["call", "NPC_HASITEMS", ["inst", "SARAH"], ["sym", "ITWR_LETTER"]],
                       ["call", "NPC_HASITEMS", ["inst", "OTHER"], ["sym", "ITMI_RING"]]],
        )},
        npc=lambda name: {"SARAH": "VLK_470_SARAH"}.get(name, name),
    )  # fmt: skip
    assert checked_items(graph) == {"ITWR_LETTER": {"VLK_470_SARAH"}}  # type: ignore[arg-type]
