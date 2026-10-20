from __future__ import annotations

import asyncio
from collections import defaultdict
from types import SimpleNamespace
from typing import Any

import pytest

from gothic_ai.chronicle import Chronicle
from gothic_ai.crime_meet import MURDER_REASON, CrimeMeet, reaction_for
from gothic_ai.crime_memory import CrimeMemoryRuntime, memory_value
from gothic_ai.crime_react import CrimeReact
from gothic_ai.crimes import Crimes
from gothic_ai.fear import knows_violence
from gothic_ai.memory import MemoryStore
from gothic_ai.places import PlaceBook
from gothic_ai.protocol import (
    HeroDeed,
    HoldVerdictCmd,
    Loading,
    NpcEvent,
    NpcIdle,
    NpcRoster,
    PerceptionHeld,
    Session,
)
from gothic_ai.social import Rumors
from tests.test_crime_react import GUARD, OTHER_GUARD, ReactLink, card
from tests.test_crimes import HILDA, LOBART

CITIZEN = "VLK_400_CITIZEN"
STRANGER = "BAU_900_STRANGER"
CAVALORN = "BAU_4300_ADDON_CAVALORN"
BRAGO = "BDT_1014_BANDIT_L"
THIRD = "BAU_901_THIRD"
RUMOR_AT = 24 * 60 + 11 * 60


@pytest.mark.parametrize("kind,role,courage,law,expected", [
    ("theft", "owner", 2, False, None),
    ("theft", "owner", 3, False, "demand"),
    ("attack", "owner", 3, False, "demand"),
    ("attack", "owner", 1, False, None),
    ("theft", "guard", 5, True, None),
    ("attack", "guard", 5, True, None),
    ("theft", "member", 5, False, None),
    ("sheep", "owner", 5, False, None),
    ("sheep", "guard", 5, True, None),
    ("murder", "guard", 1, True, "fight"),
    ("murder", "guard", 5, False, None),
    ("murder", "member", 4, False, "fight"),
    ("murder", "member", 3, False, "refuse"),
    ("murder", "member", 2, False, "flee"),
    ("murder", "stranger", 5, False, "refuse"),
    ("murder", "stranger", 2, False, "flee"),
])
def test_role_and_character_table(kind, role, courage, law, expected):
    assert reaction_for(kind, role, courage=courage, law_known=law) == expected


@pytest.mark.parametrize("kind", ["theft", "attack", "murder", "sheep"])
@pytest.mark.parametrize("courage", [1, 3, 5])
def test_ally_has_no_encounter_reaction_even_when_law_knows(kind, courage):
    assert reaction_for(kind, "ally", courage=courage, law_known=True) is None


class MeetLink(ReactLink):
    def __init__(self) -> None:
        super().__init__()
        self.native: dict[str, str] = {}
        self.reason: dict[str, str] = {}
        self.refusal: dict[str, int] = {}
        self.projection: dict[str, int] = {}
        self.visible = True
        self.room_indoor = True
        self.room_point: str | None = None
        self.hero_point: str | None = None
        self.player_portal = 15
        self.same_room = True
        self.room_distance = 400
        self.evaluations: list[tuple[str, list[Any]]] = []
        self.before_act: Any = None
        self.before_metadata: asyncio.Event | None = None

    async def story_effect(self, npc, effects, *, check=False):
        await super().story_effect(npc, effects, check=check)
        for effect in effects:
            if effect["do"] == "crime_memory":
                self.projection[npc] = effect["value"]
            elif effect["do"] == "refuse_talk":
                self.refusal[npc] = effect["seconds"]
            elif effect["do"] == "calm":
                self.native[npc] = "ZS_STAND"

    async def npc_act(self, npc, action, /, **args):
        if action in {"fight", "flee"}:
            self.native[npc] = "ZS_ATTACK" if action == "fight" else "ZS_FLEE"
            self.reason[npc] = args.get("reason", "")
        if self.before_act is not None:
            self.before_act()
        return await super().npc_act(npc, action, **args)

    async def story_eval(self, npc, exprs):
        self.evaluations.append((npc, exprs))
        if exprs == [["var", "AIV_ATTACKREASON"]]:
            return [8]
        if len(exprs) == 2 and exprs[0][0] == "field":
            if self.before_metadata is not None:
                await self.before_metadata.wait()
            return [3, 0]
        if len(exprs) == 3 and exprs[1][0] == "==":
            return [
                int(self.native.get(npc) == "ZS_ATTACK"),
                int(self.reason.get(npc) == exprs[1][2][1]),
                int(self.native.get(npc) == "ZS_CLEARROOM"),
            ]
        values = []
        for expr in exprs:
            call = expr[1]
            if call == "NPC_REFUSETALK":
                values.append(self.refusal.get(npc, 0))
            elif call == "NPC_SETREFUSETALK":
                self.refusal[npc] = expr[3][1]
                values.append(0)
            elif call == "NPC_ISINSTATE":
                values.append(int(self.native.get(npc) == expr[3][1]))
            elif call == "AI_CONTINUEROUTINE":
                self.native[npc] = "ZS_STAND"
                values.append(0)
            elif call == "NPC_CLEARAIQUEUE":
                values.append(0)
            elif call == "NPC_ISINPLAYERSROOM":
                values.append(int(self.same_room))
            elif call == "NPC_CANSEENPC":
                values.append(int(self.visible))
            elif call == "NPC_GETDISTTONPC":
                values.append(self.room_distance)
            elif call == "WLD_GETPLAYERPORTALGUILD":
                values.append(self.player_portal)
            elif call == "NPC_GETNEARESTWP":
                values.append(self.hero_point)
            else:
                raise AssertionError(expr)
        return values

    async def npc_view(self, npcs):
        return [SimpleNamespace(
            npc=npc, hero=SimpleNamespace(seen=self.visible), indoors=self.room_indoor,
            nearest_waypoint=self.room_point,
        ) for npc in npcs]


class Writer:
    def __init__(self) -> None:
        self.calls: list[tuple[str, str]] = []
        self.storylet = SimpleNamespace(stage="open")
        self.gate: asyncio.Event | None = None
        self.started = asyncio.Event()

    async def demand(self, npc, crime):
        self.calls.append((npc, crime))
        self.started.set()
        if self.gate is not None:
            await self.gate.wait()
        return self.storylet


class Approach:
    def __init__(self, link, writer) -> None:
        self.link, self.writer = link, writer
        self.calls: list[tuple[str, str]] = []
        self.started = asyncio.Event()
        self.gate: asyncio.Event | None = None
        self.accept = True
        self.closed = False

    async def approach(self, npc, key, *, valid, told, release):
        assert valid()
        self.calls.append((npc, key))
        async with release():
            assert self.link.projection[npc] == 0
            self.started.set()
            if self.gate is not None:
                await self.gate.wait()
            if self.accept:
                self.writer.storylet.stage = "told"
                assert told()
        return self.accept

    async def close(self):
        self.closed = True


@pytest.fixture
async def system():
    link = MeetLink()
    memory = MemoryStore(":memory:")
    chronicle = Chronicle(link, memory)  # type: ignore[arg-type]
    crimes = Crimes(link, chronicle)  # type: ignore[arg-type]
    reactions = CrimeReact(link, crimes)  # type: ignore[arg-type]
    projection = CrimeMemoryRuntime(link, crimes)  # type: ignore[arg-type]
    writer = Writer()
    approach = Approach(link, writer)
    clock = SimpleNamespace(now=0.0)
    meets = CrimeMeet(  # type: ignore[arg-type]
        link, crimes, reactions, projection, amends=writer, approach=approach, clock=lambda: clock.now,
    )
    crimes.people.update({
        LOBART: {"guild": 12, "npctype": 0, "home_room": "LOBART"},
        HILDA: {"guild": 12, "npctype": 0, "home_room": "LOBART"},
        GUARD: {"guild": 2, "npctype": 0},
        OTHER_GUARD: {"guild": 2, "npctype": 0},
        CITIZEN: {"guild": 3, "npctype": 0},
        STRANGER: {"guild": 8, "npctype": 0},
    })
    yield SimpleNamespace(
        link=link, memory=memory, chronicle=chronicle, crimes=crimes,
        react=reactions, projection=projection, meet=meets, writer=writer, approach=approach, clock=clock,
    )
    await meets.close()
    await projection.close()
    await reactions.close()
    await crimes.close()
    if chronicle._background:
        await asyncio.gather(*chronicle._background)
    memory.close()


def case(system, *, crime="case-1", kind="theft", npc=LOBART, degree="seen", community=None):
    value = {
        "fact": "crime", "id": crime, "kind": kind, "owner": LOBART, "victim": LOBART,
        "community": community, "day": 3,
    }
    system.chronicle.record(value)
    if degree == "seen":
        system.chronicle.record({"fact": "crime_seen", "npc": npc, "crime": crime})
    elif degree == "heard":
        system.chronicle.record({"fact": "rumor", "to": npc, "from": HILDA, "crime": crime})
    elif degree == "suspects":
        system.chronicle.record({"fact": "crime_suspected", "npc": npc, "crime": crime})
    return value


@pytest.mark.parametrize("cover", [True, False])
async def test_cover_settlement_respects_switch_but_normal_silence_still_settles(system, cover):
    case(system)
    system.crimes.cover = cover
    system.chronicle.record(
        {
            "fact": "silenced",
            "npc": LOBART,
            "crime": "case-1",
            "cover": "D1",
        }
    )
    assert system.meet._settled(LOBART, "case-1") is cover
    system.chronicle.record({"fact": "silenced", "npc": LOBART, "crime": "case-1"})
    assert system.meet._settled(LOBART, "case-1")


def near(system, npc=LOBART, *, event="player_near"):
    system.link.receive(NpcEvent(type="npc_event", npc=npc, kind=event, distance=400))


def roster(system, npc, *, distance=400, state="ZS_STAND", activity="routine"):
    system.link.native[npc] = state
    system.link.receive(NpcRoster(type="npc_roster", t=0, npcs=[{
        "id": npc, "name": npc, "distance": distance, "activity": activity, "state": state,
    }]))


async def finish_demand(system):
    tasks = tuple(system.meet._demands.values())
    assert tasks
    await asyncio.wait_for(asyncio.gather(*tasks), 0.5)


async def test_owner_approaches_for_existing_amends_and_restores_home_memory(system):
    case(system)
    near(system)
    await system.meet.tick()
    await finish_demand(system)
    assert system.writer.calls == [(LOBART, "case-1")]
    assert system.approach.calls == [(LOBART, "crime:case-1")]
    assert system.link.projection[LOBART] == 3
    assert system.chronicle.facts("crime_meet") == [{
        "fact": "crime_meet", "npc": LOBART, "crime": "case-1", "reaction": "demand",
        "event": "player_near", "source": "personal", "role": "owner",
    }]
    assert "demand" in system.meet.summary("case-1")
    await system.meet.tick()
    assert len(system.writer.calls) == 1


async def test_paid_demand_does_not_react_on_next_encounter(system):
    case(system)
    near(system)
    await system.meet.tick()
    await finish_demand(system)
    system.chronicle.record({"fact": "forgiven", "crime": "case-1", "npc": LOBART, "paid": 50})
    await system.meet.tick()
    assert system.link.projection[LOBART] == 0
    roster(system, LOBART, distance=800)
    near(system)
    await system.meet.tick()
    assert len(system.writer.calls) == 1
    assert len(system.chronicle.facts("crime_meet")) == 1


async def test_told_demand_is_not_restarted_at_every_future_meeting(system):
    case(system)
    near(system)
    await system.meet.tick()
    await finish_demand(system)
    roster(system, LOBART, distance=800)
    near(system)
    await system.meet.tick()
    await finish_demand(system)
    assert len(system.approach.calls) == 1
    assert len(system.chronicle.facts("crime_meet")) == 1


async def test_busy_important_approach_reuses_storylet_without_false_demand_fact(system):
    case(system)
    near(system)
    system.approach.accept = False
    await system.meet.tick()
    await finish_demand(system)
    assert not system.chronicle.facts("crime_meet")
    system.approach.accept = True
    await system.meet.tick()
    await finish_demand(system)
    assert len(system.chronicle.facts("crime_meet")) == 1


@pytest.mark.parametrize("degree", ["seen", "heard"])
async def test_coward_owner_keeps_existing_cold_talk_without_approaching(system, degree):
    case(system, degree=degree)
    system.meet.cards = {LOBART: card(LOBART, courage=2)}
    near(system)
    await system.meet.tick()
    assert not system.writer.calls
    assert not system.link.actions
    assert system.crimes.distrust(LOBART)


@pytest.mark.parametrize("degree", ["suspects", None])
async def test_suspicion_or_ownership_alone_cannot_start_demand(system, degree):
    case(system, degree=degree)
    near(system)
    await system.meet.tick()
    assert not system.writer.calls
    assert not system.chronicle.facts("crime_meet")


async def test_law_informs_guard_who_never_saw_the_murder(system):
    case(system, kind="murder", npc=CITIZEN, community="city")
    assert not system.crimes.known(GUARD)
    system.chronicle.record({"fact": "crime_known", "community": "city", "crime": "case-1"})
    near(system, GUARD)
    await system.meet.tick()
    assert system.link.actions == [(GUARD, "fight", {"npc": "PLAYER", "reason": MURDER_REASON})]
    assert system.chronicle.facts("crime_meet")[-1]["source"] == "law"
    assert not system.crimes.known(GUARD)  # no manufactured eyewitness or rumor
    await system.meet.tick()
    assert len(system.link.actions) == 1


async def test_guard_waits_for_law_and_line_of_sight_then_uses_pending_near_event(system):
    case(system, kind="murder", npc=GUARD, community="city")
    near(system, GUARD)
    await system.meet.tick()
    assert not system.link.actions
    system.chronicle.record({"fact": "crime_known", "community": "city", "crime": "case-1"})
    system.link.visible = False
    await system.meet.tick()
    assert not system.link.actions
    system.link.visible = True
    await system.meet.tick()
    assert system.link.actions[-1][1] == "fight"


async def test_other_community_law_does_not_authorize_guard_attack(system):
    case(system, kind="murder", npc=CITIZEN, community="farm")
    system.chronicle.record({"fact": "crime_known", "community": "farm", "crime": "case-1"})
    near(system, GUARD)
    await system.meet.tick()
    assert not system.link.actions


@pytest.mark.parametrize("courage,expected", [(2, "flee"), (3, "refuse"), (4, "fight")])
async def test_heard_murder_depends_on_courage_and_victims_community(system, courage, expected):
    case(system, kind="murder", npc=CITIZEN, community="city", degree="heard")
    system.meet.cards = {CITIZEN: card(CITIZEN, courage=courage)}
    near(system, CITIZEN)
    await system.meet.tick()
    fact = system.chronicle.facts("crime_meet")[-1]
    assert fact["reaction"] == expected
    if expected == "fight":
        assert system.link.actions[-1][2]["reason"] == MURDER_REASON
    else:
        assert all(action != "fight" for _, action, _ in system.link.actions)
        assert system.link.refusal[CITIZEN] == 2


async def test_brave_outsider_refuses_instead_of_avenging_another_community(system):
    case(system, kind="murder", npc=STRANGER, community="city", degree="heard")
    system.meet.cards = {STRANGER: card(STRANGER, courage=5)}
    near(system, STRANGER)
    await system.meet.tick()
    assert [action for _, action, _ in system.link.actions] == ["say_svm", "turn_away"]
    assert system.link.actions[0][2]["svm"] == "NOTNOW"
    assert system.chronicle.facts("crime_meet")[-1]["reaction"] == "refuse"


@pytest.mark.parametrize("npc", [STRANGER, GUARD])
@pytest.mark.parametrize("courage", [2, 3, 5])
async def test_allied_witness_neither_refuses_flees_nor_fights_after_murder(system, npc, courage):
    crime = case(system, kind="murder", npc=npc, community="city")
    system.chronicle.record({"fact": "crime_side", "npc": npc, "crime": crime["id"], "why": "fought"})
    system.chronicle.record({"fact": "crime_known", "community": "city", "crime": crime["id"]})
    system.meet.cards = {npc: card(npc, courage=courage)}
    near(system, npc)
    await system.meet.tick()
    assert system.react.role(npc, crime) == "ally"
    assert system.crimes.known(npc)[0].degree == "seen"
    assert system.meet.refusal(npc) is None
    assert not system.meet.refuses(npc)
    assert system.link.actions == []
    assert system.link.refusal == {}
    assert system.chronicle.facts("crime_meet") == []


def ally_murder(system, *, victim_guild=9):
    system.crimes.people.update({
        CAVALORN: {"guild": 8, "npctype": 0},
        BRAGO: {"guild": victim_guild, "npctype": 0},
        THIRD: {"guild": 8, "npctype": 0},
    })
    system.crimes.sides.note_fight(BRAGO, CAVALORN)
    crime = system.crimes.record(HeroDeed.model_validate({
        "type": "hero_deed", "id": "brago-murder", "kind": "murder", "victim": BRAGO,
        "victim_guild": victim_guild, "witnesses": [CAVALORN, HILDA],
        "witnesses_metadata": [{"npc": CAVALORN, "guild": 8}, {"npc": HILDA, "guild": 12}],
        "position": {"x": 0, "y": 0, "z": 0}, "day": 1, "hour": 10, "minute": 0,
    }))
    assert crime is not None
    assert system.crimes.side(CAVALORN, crime["id"])["why"] == "fought"
    assert system.crimes.side(HILDA, crime["id"]) is None
    return crime


def crime_rumors(system):
    rumors = Rumors.__new__(Rumors)
    rumors.social = SimpleNamespace(names={
        CAVALORN: "Кавалорн", BRAGO: "Браго", STRANGER: "Крестьянин", THIRD: "Третий", HILDA: "Хильда",
    })
    rumors.dead, rumors.rumors, rumors.tells, rumors.waiting = set(), {}, defaultdict(int), None
    rumors.chronicle, rumors.crimes = system.chronicle, system.crimes
    rumors.dialogue = SimpleNamespace(remember=lambda *args: None)
    rumors._load()
    return rumors


@pytest.mark.parametrize("courage", [2, 3, 5])
async def test_ally_rumor_does_not_make_a_stranger_blame_the_hero(system, courage):
    crime = ally_murder(system)
    rumors = crime_rumors(system)
    [rumor] = rumors.rumors.values()
    fact = rumors._learn(rumor, STRANGER, CAVALORN, RUMOR_AT, "met")
    assert (fact["excuse"], fact["excuse_by"], fact["excuse_by_name"]) == (
        "fought", CAVALORN, "Кавалорн",
    )
    [knowledge] = system.crimes.known(STRANGER)
    assert (knowledge.degree, knowledge.excuse, knowledge.excuse_by) == ("heard", "fought", CAVALORN)
    assert system.crimes.side(STRANGER, crime["id"]) is None
    assert system.react.role(STRANGER, crime) == "ally"
    assert system.meet.refusal(STRANGER) is None
    assert not knows_violence(STRANGER, system.crimes)
    assert memory_value(system.crimes, STRANGER) == (0, 0)
    situation = system.crimes.situation(STRANGER)
    assert "дрался с ним" in situation
    assert "не винишь героя" in situation
    system.meet.cards = {STRANGER: card(STRANGER, courage=courage)}
    near(system, STRANGER)
    await system.meet.tick()
    assert system.link.actions == []
    assert system.chronicle.facts("crime_meet") == []
    assert system.crimes.crime(crime["id"]) == crime


async def test_ally_excuse_keeps_its_first_source_through_retelling_and_save_load(system):
    crime = ally_murder(system)
    rumors = crime_rumors(system)
    [rumor] = rumors.rumors.values()
    rumors._learn(rumor, STRANGER, CAVALORN, RUMOR_AT, "met")
    relayed = rumors._learn(rumor, THIRD, STRANGER, RUMOR_AT + 1, "met")
    assert (relayed["excuse"], relayed["excuse_by"], relayed["excuse_by_name"], relayed["hops"]) == (
        "fought", CAVALORN, "Кавалорн", 2,
    )
    assert system.react.role(THIRD, crime) == "ally"
    assert system.meet.refusal(THIRD) is None
    saved = system.memory.head("A")
    system.chronicle.record({"fact": "crime_seen", "npc": THIRD, "crime": crime["id"]})
    assert system.react.role(THIRD, crime) == "stranger"
    people = system.crimes.people.copy()
    loaded = Session(type="session", branch="B", parent="A", parent_seq=saved, reason="load")
    system.link.session = loaded
    system.link.receive(loaded)
    system.crimes.people.update(people)
    reloaded = Chronicle(system.link, system.memory)
    loaded_crimes = Crimes(system.link, reloaded)
    try:
        [knowledge] = loaded_crimes.known(THIRD)
        assert (knowledge.degree, knowledge.excuse, knowledge.excuse_by) == (
            "heard", "fought", CAVALORN,
        )
        assert reloaded.facts("rumor")[-1] == relayed
        rumors._load()
        [rumor] = rumors.rumors.values()
        again = rumors._learn(rumor, CITIZEN, THIRD, RUMOR_AT + 2, "met")
        assert (again["excuse"], again["excuse_by"], again["hops"]) == ("fought", CAVALORN, 3)
        assert system.react.role(THIRD, crime) == "ally"
        assert system.meet.refusal(THIRD) is None
        assert not knows_violence(THIRD, system.crimes)
        assert memory_value(system.crimes, THIRD) == (0, 0)
    finally:
        await loaded_crimes.close()


@pytest.mark.parametrize("tellers", [(HILDA,), (HILDA, CAVALORN), (CAVALORN, HILDA)])
async def test_plain_rumor_still_blames_but_an_excused_version_wins_in_either_order(system, tellers):
    crime = ally_murder(system)
    rumors = crime_rumors(system)
    [rumor] = rumors.rumors.values()
    for minute, teller in enumerate(tellers):
        rumors._learn(rumor, STRANGER, teller, RUMOR_AT + minute, "met")
    [knowledge] = system.crimes.known(STRANGER)
    if CAVALORN in tellers:
        assert (knowledge.excuse, knowledge.excuse_by) == ("fought", CAVALORN)
        assert system.react.role(STRANGER, crime) == "ally"
        assert system.meet.refusal(STRANGER) is None
        assert not knows_violence(STRANGER, system.crimes)
        assert memory_value(system.crimes, STRANGER) == (0, 0)
    else:
        assert not knowledge.excuse
        refusal = system.meet.refusal(STRANGER)
        assert (refusal.role, refusal.reaction) == ("stranger", "refuse")
        assert knows_violence(STRANGER, system.crimes)
        assert memory_value(system.crimes, STRANGER) == (4, 1)


@pytest.mark.parametrize("npc,degree,react,expected", [
    (HILDA, "seen", True, False),
    (CITIZEN, "heard", True, False),
    (GUARD, "heard", True, False),
    (STRANGER, "heard", True, True),
    (STRANGER, "heard", False, False),
])
async def test_justified_version_only_upgrades_a_known_strangers_heard_murder(
    system, monkeypatch, npc, degree, react, expected,
):
    crime = ally_murder(system, victim_guild=3)
    rumors = crime_rumors(system)
    [rumor] = rumors.rumors.values()
    if degree == "heard":
        rumors._learn(rumor, npc, HILDA, RUMOR_AT, "met")
    assert system.crimes.known(npc)[0].degree == degree
    assert CAVALORN in rumor.known and npc in rumor.known
    if not react:
        system.crimes.react = None
    monkeypatch.setattr("gothic_ai.social.P_MEET", 1.0)
    rumors.clock = RUMOR_AT + 1
    rumors.social.ties = {CAVALORN: {npc: SimpleNamespace()}}
    staged = []
    rumors.stage = lambda r, teller, listener: staged.append((teller, listener))
    facts, tells, known = system.chronicle.facts("rumor"), dict(rumors.tells), dict(rumor.known)
    assert rumors._passes(rumor, CAVALORN, npc, rumors.clock, True) is expected
    assert rumors.offer_status(rumor, CAVALORN, npc) == ("told" if expected else "no")
    if expected:
        assert staged == [(CAVALORN, npc)]
        assert len(system.chronicle.facts("rumor")) == len(facts) + 1
        [knowledge] = system.crimes.known(npc)
        assert (knowledge.excuse, knowledge.excuse_by) == ("fought", CAVALORN)
        assert system.react.role(npc, crime) == "ally"
        assert rumors.tells[rumor.key, CAVALORN, RUMOR_AT // (24 * 60)] == 1
    else:
        assert staged == []
        assert system.chronicle.facts("rumor") == facts
        assert dict(rumors.tells) == tells
        assert rumor.known == known
        assert not system.crimes.known(npc)[0].excuse


@pytest.mark.parametrize("courage", [2, 3])
async def test_later_excused_version_stops_existing_flight_or_refusal(system, courage):
    crime = ally_murder(system)
    rumors = crime_rumors(system)
    [rumor] = rumors.rumors.values()
    rumors._learn(rumor, STRANGER, HILDA, RUMOR_AT, "met")
    system.meet.cards = {STRANGER: card(STRANGER, courage=courage)}
    near(system, STRANGER)
    await system.meet.tick()
    assert STRANGER in system.meet._active
    assert system.link.refusal[STRANGER] == 2
    assert system.link.projection[STRANGER] == 3
    actions = len(system.link.actions)
    rumors._learn(rumor, STRANGER, CAVALORN, RUMOR_AT + 1, "met")
    await system.meet.tick()
    assert system.react.role(STRANGER, crime) == "ally"
    assert system.meet.refusal(STRANGER) is None
    assert STRANGER not in system.meet._active
    assert system.link.refusal[STRANGER] == 0
    assert system.link.native.get(STRANGER, "ZS_STAND") == "ZS_STAND"
    assert system.link.projection[STRANGER] == 0
    assert len(system.link.actions) == actions
    assert len(system.chronicle.facts("crime_meet")) == 1


@pytest.mark.parametrize("npc,role,courage,reaction", [
    (CITIZEN, "member", 2, "flee"),
    (CITIZEN, "member", 3, "refuse"),
    (CITIZEN, "member", 4, "fight"),
    (GUARD, "guard", 2, "fight"),
    (GUARD, "guard", 5, "fight"),
])
async def test_excused_rumor_keeps_victims_community_and_guard_consequences(
    system, npc, role, courage, reaction,
):
    crime = ally_murder(system, victim_guild=3)
    rumors = crime_rumors(system)
    [rumor] = rumors.rumors.values()
    fact = rumors._learn(rumor, npc, CAVALORN, RUMOR_AT, "met")
    assert fact["excuse"] == "fought"
    assert system.react.role(npc, crime) == role
    assert knows_violence(npc, system.crimes)
    assert memory_value(system.crimes, npc) == (4, 1)
    assert system.crimes.attitude(npc) == "hostile"
    delivered = system.react.law.tick(RUMOR_AT)
    assert bool(delivered) is (role == "guard")
    if role == "guard":
        assert delivered[0]["crime"] == crime["id"]
        assert delivered[0]["via"] == GUARD
    system.meet.cards = {npc: card(npc, courage=courage)}
    near(system, npc)
    await system.meet.tick()
    [meeting] = system.chronicle.facts("crime_meet")
    assert (meeting["role"], meeting["reaction"]) == (role, reaction)
    assert "не винишь героя" not in system.crimes.situation(npc)


@pytest.mark.parametrize("npc", [STRANGER, GUARD])
@pytest.mark.parametrize("why", ["hostile", "quest", "fought", "companion"])
async def test_listener_gets_own_side_before_normal_rumor_and_does_not_blame(system, npc, why):
    crime = ally_murder(system, victim_guild=3)
    if why == "hostile":
        system.crimes.sides.hostile_guilds.add((system.crimes.people[npc]["guild"], 3))
    elif why == "quest":
        quest = SimpleNamespace(id="kill-brago", giver=npc, stages=[SimpleNamespace(goals=[
            SimpleNamespace(kind="kill", npcs=[BRAGO]),
        ])])
        system.crimes.sides.book = SimpleNamespace(
            all=lambda: {quest.id: quest}, progress=lambda _: SimpleNamespace(state="given"),
        )
    elif why == "fought":
        system.crimes.sides.note_fight(BRAGO, npc)
    else:
        system.crimes.people[npc]["npctype"] = 2
        system.crimes.sides.note_fight(BRAGO, "PLAYER")
    rumors = crime_rumors(system)
    [rumor] = rumors.rumors.values()
    observed = []

    def on_fact(fact):
        observed.append((fact["fact"], system.crimes.side(npc, crime["id"])))
        if fact["fact"] == "rumor":
            assert system.crimes.attitude(npc) is None
            assert system.react.law.tick(RUMOR_AT) == []

    system.chronicle.on_fact.append(on_fact)
    fact = rumors._learn(rumor, npc, HILDA, RUMOR_AT, "met")
    assert "excuse" not in fact
    assert [kind for kind, _ in observed] == ["crime_side", "rumor"]
    assert all(side is not None and side["why"] == why for _, side in observed)
    assert system.react.role(npc, crime) == "ally"
    assert system.meet.refusal(npc) is None
    assert system.crimes.attitude(npc) is None
    assert not knows_violence(npc, system.crimes)
    assert memory_value(system.crimes, npc) == (0, 0)
    assert system.react.law.deliveries() == []
    assert system.chronicle.facts("crime_known") == []
    near(system, npc)
    await system.meet.tick()
    assert system.link.actions == []
    assert system.chronicle.facts("crime_meet") == []


@pytest.mark.parametrize("kind", ["theft", "attack", "sheep"])
async def test_guard_never_starts_meeting_fight_for_lesser_known_cases(system, kind):
    case(system, kind=kind, npc=GUARD, community="city")
    system.chronicle.record({"fact": "crime_known", "community": "city", "crime": "case-1"})
    near(system, GUARD)
    await system.meet.tick()
    assert not system.link.actions


@pytest.mark.parametrize("settlement", ["silenced", "forgiven", "paid", "died"])
@pytest.mark.parametrize("courage", [2, 3, 4])
async def test_every_personal_settlement_clears_active_reaction_in_same_poll(system, settlement, courage):
    case(system, kind="murder", npc=CITIZEN, community="city", degree="heard")
    system.meet.cards = {CITIZEN: card(CITIZEN, courage=courage)}
    near(system, CITIZEN)
    await system.meet.tick()
    fact = {"fact": settlement, "npc": CITIZEN, "crime": "case-1"}
    if settlement == "paid":
        fact.update(fact="forgiven", paid=50)
    system.chronicle.record(fact)
    await system.meet.tick()
    assert CITIZEN not in system.meet._active
    assert system.link.projection[CITIZEN] == 0
    if settlement != "died":
        assert system.link.native.get(CITIZEN, "ZS_STAND") == "ZS_STAND"
        assert system.link.refusal.get(CITIZEN, 0) == 0
    assert len(system.chronicle.facts("crime_meet")) == 1


async def test_absolution_stops_law_guard_without_dropping_citizens_personal_claim(system):
    case(system, kind="murder", npc=CITIZEN, community="city", degree="heard")
    system.chronicle.record({"fact": "crime_known", "community": "city", "crime": "case-1"})
    near(system, GUARD)
    near(system, CITIZEN)
    await system.meet.tick()
    system.chronicle.record({"fact": "absolved", "community": "city", "crimes": ["case-1"]})
    await system.meet.tick()
    assert GUARD not in system.meet._active
    assert system.link.native[GUARD] == "ZS_STAND"
    assert CITIZEN in system.meet._active
    assert system.link.refusal[CITIZEN] == 2


@pytest.mark.parametrize("settlement", ["silenced", "forgiven"])
async def test_guard_personal_agreement_does_not_erase_law_or_bind_other_guards(system, settlement):
    case(system, kind="murder", degree=None, community="city")
    system.chronicle.record({"fact": "crime_known", "community": "city", "crime": "case-1"})
    system.chronicle.record({"fact": settlement, "npc": GUARD, "crime": "case-1"})
    near(system, GUARD)
    near(system, OTHER_GUARD)
    await system.meet.tick()
    assert [npc for npc, action, _ in system.link.actions if action == "fight"] == [OTHER_GUARD]
    assert len(system.meet.law.unpaid("city")) == 1


async def test_settlement_never_calms_a_later_unrelated_fight(system):
    case(system, kind="murder", npc=CITIZEN, community="city", degree="heard")
    system.meet.cards = {CITIZEN: card(CITIZEN, courage=4)}
    near(system, CITIZEN)
    await system.meet.tick()
    system.link.reason[CITIZEN] = "AR_NONE"
    system.chronicle.record({"fact": "forgiven", "npc": CITIZEN, "crime": "case-1"})
    await system.meet.tick()
    assert not any(e["do"] == "calm" for _, effects, _ in system.link.effects for e in effects)
    assert system.link.native[CITIZEN] == "ZS_ATTACK"


async def test_preexisting_native_refusal_is_not_cleared_by_our_settlement(system):
    case(system, kind="murder", npc=CITIZEN, community="city", degree="heard")
    system.link.refusal[CITIZEN] = 30
    near(system, CITIZEN)
    await system.meet.tick()
    system.chronicle.record({"fact": "forgiven", "npc": CITIZEN, "crime": "case-1"})
    await system.meet.tick()
    assert system.link.refusal[CITIZEN] == 30


@pytest.mark.parametrize("during", ["writer", "walk"])
async def test_payment_cancels_pending_work_and_never_restores_stale_memory(system, during):
    case(system)
    near(system)
    gate = asyncio.Event()
    if during == "writer":
        system.writer.gate = gate
        started = system.writer.started
    else:
        system.approach.gate = gate
        started = system.approach.started
    await system.meet.tick()
    await asyncio.wait_for(started.wait(), 0.5)
    system.chronicle.record({"fact": "forgiven", "crime": "case-1", "npc": LOBART, "paid": 50})
    await system.meet.tick()
    assert not system.meet._active
    assert system.link.projection[LOBART] == 0
    assert not system.chronicle.facts("crime_meet")


async def test_payment_during_action_receipt_rolls_back_the_just_started_native_fight(system):
    case(system, kind="murder", npc=CITIZEN, community="city", degree="heard")
    system.meet.cards = {CITIZEN: card(CITIZEN, courage=4)}
    near(system, CITIZEN)
    system.link.before_act = lambda: system.chronicle.record({
        "fact": "forgiven", "crime": "case-1", "npc": CITIZEN,
    })
    await system.meet.tick()
    assert system.link.native[CITIZEN] == "ZS_STAND"
    assert system.link.projection[CITIZEN] == 0
    assert not system.chronicle.facts("crime_meet")


async def test_loading_cancels_approach_and_rebuilds_only_new_branch_knowledge(system):
    case(system)
    near(system)
    system.approach.gate = asyncio.Event()
    await system.meet.tick()
    await asyncio.wait_for(system.approach.started.wait(), 0.5)
    old = tuple(system.meet._demands.values())
    system.link.receive(Loading(type="loading"))
    system.link.receive(Session(type="session", branch="B"))
    await asyncio.gather(*old, return_exceptions=True)
    assert system.projection._suspended == {}
    assert not system.meet._active
    near(system)
    await system.meet.tick()
    assert not system.chronicle.facts("crime_meet")
    assert len(system.writer.calls) == 1
    case(system, crime="branch-b", kind="murder", npc=CITIZEN, community="city", degree="heard")
    near(system, CITIZEN)
    await system.meet.tick()
    assert system.chronicle.facts("crime_meet")[-1]["crime"] == "branch-b"


async def test_metadata_await_revalidates_the_current_branch_before_acting(system):
    case(system, kind="murder", npc=CITIZEN, community="city", degree="heard")
    system.crimes.people.pop(CITIZEN)
    system.link.before_metadata = asyncio.Event()
    near(system, CITIZEN)
    task = asyncio.create_task(system.meet.tick())
    await asyncio.sleep(0)
    system.link.receive(Session(type="session", branch="B"))
    system.link.before_metadata.set()
    await task
    assert not system.link.actions
    assert not system.chronicle.facts("crime_meet")


async def test_native_known_theft_home_warning_is_observed_without_new_fight_mechanism(system):
    case(system, npc=HILDA)
    roster(system, HILDA, state="ZS_CLEARROOM")
    near(system, HILDA, event="enter_room")
    await system.meet.tick()
    assert system.link.projection[HILDA] == 3
    assert not system.link.actions
    assert system.chronicle.facts("crime_meet")[-1] == {
        "fact": "crime_meet", "npc": HILDA, "crime": "case-1", "reaction": "room",
        "event": "enter_room", "source": "personal", "role": "member",
    }
    system.chronicle.record({"fact": "silenced", "npc": HILDA, "crime": "case-1"})
    await system.meet.tick()
    assert system.link.projection[HILDA] == 0
    assert system.link.native[HILDA] == "ZS_STAND"


async def test_suspicion_cannot_be_reported_as_a_native_home_reaction(system):
    case(system, npc=HILDA, degree="suspects")
    roster(system, HILDA, state="ZS_CLEARROOM")
    near(system, HILDA, event="enter_room")
    await system.meet.tick()
    assert not system.chronicle.facts("crime_meet")


async def test_busy_body_waits_without_interfering_with_other_combat_provider(system):
    case(system, kind="murder", npc=CITIZEN, community="city", degree="heard")
    system.meet.cards = {CITIZEN: card(CITIZEN, courage=4)}
    system.link.owned.add(CITIZEN)
    near(system, CITIZEN)
    await system.meet.tick()
    assert not system.link.actions
    system.link.owned.clear()
    await system.meet.tick()
    assert system.link.actions[-1][1] == "fight"


async def test_close_removes_the_important_approach_listener(system):
    await system.meet.close()
    assert system.approach.closed


@pytest.mark.parametrize("settlement", ["silenced", "forgiven", "paid", "died", "loading"])
async def test_conversational_refusal_reads_live_knowledge_without_waiting_for_poll(system, settlement):
    case(system, kind="murder", npc=CITIZEN, community="city", degree="heard")
    assert system.meet.refuses(CITIZEN.lower())
    if settlement == "loading":
        system.link.receive(Loading(type="loading"))
    else:
        system.chronicle.record({
            "fact": "forgiven" if settlement == "paid" else settlement,
            "npc": CITIZEN, "crime": "case-1", **({"paid": 50} if settlement == "paid" else {}),
        })
    assert not system.meet.refuses(CITIZEN)


async def test_refusal_does_not_turn_a_suspicion_into_a_conversation_ban(system):
    case(system, kind="murder", npc=CITIZEN, community="city", degree="suspects")
    assert not system.meet.refuses(CITIZEN)
    assert system.meet.refusal(CITIZEN) is None


@pytest.mark.parametrize("courage,reaction", [(3, "refuse"), (2, "flee")])
async def test_refusal_returns_the_murder_case_role_and_reaction(system, courage, reaction):
    crime = case(system, kind="murder", npc=STRANGER, community="city")
    system.meet.cards = {STRANGER: card(STRANGER, courage=courage)}
    reason = system.meet.refusal(STRANGER.lower())
    assert reason is not None
    assert reason.crime == crime
    assert reason.crime["kind"] == "murder"
    assert reason.role == "stranger"
    assert reason.reaction == reaction
    assert system.meet.refuses(STRANGER)
    system.chronicle.record({"fact": "forgiven", "crime": crime["id"], "npc": STRANGER})
    assert system.meet.refusal(STRANGER) is None
    assert not system.meet.refuses(STRANGER)


@pytest.mark.parametrize("settlement", ["silenced", "forgiven"])
async def test_guard_agreement_ends_only_its_active_enforcement_same_poll(system, settlement):
    case(system, kind="murder", degree=None, community="city")
    system.chronicle.record({"fact": "crime_known", "community": "city", "crime": "case-1"})
    near(system, GUARD)
    near(system, OTHER_GUARD)
    await system.meet.tick()
    system.chronicle.record({"fact": settlement, "npc": GUARD, "crime": "case-1"})
    await system.meet.tick()
    assert system.link.native[GUARD] == "ZS_STAND"
    assert system.link.native[OTHER_GUARD] == "ZS_ATTACK"
    assert len(system.meet.law.unpaid("city")) == 1


async def test_home_settlement_stops_its_tracked_nonlethal_fight(system):
    case(system, npc=HILDA)
    roster(system, HILDA, state="ZS_CLEARROOM")
    near(system, HILDA, event="enter_room")
    await system.meet.tick()
    system.link.native[HILDA] = "ZS_ATTACK"
    system.link.reason[HILDA] = "AR_CLEARROOM"
    system.chronicle.record({"fact": "forgiven", "npc": HILDA, "crime": "case-1"})
    await system.meet.tick()
    assert system.link.projection[HILDA] == 0
    assert system.link.native[HILDA] == "ZS_STAND"


async def test_fact_wake_and_poll_do_not_cancel_a_writer_rollback_twice(system, monkeypatch):
    case(system)
    near(system)
    writing, rolling_back, finish_rollback = (asyncio.Event() for _ in range(3))
    cancellations = []

    async def demand(npc, crime):
        writing.set()
        try:
            await asyncio.Event().wait()
        except asyncio.CancelledError:
            rolling_back.set()
            await finish_rollback.wait()
            cancellations.append(asyncio.current_task().cancelling())
            raise

    monkeypatch.setattr(system.writer, "demand", demand)
    await system.meet.tick()
    await writing.wait()
    system.chronicle.record({"fact": "forgiven", "npc": LOBART, "crime": "case-1"})
    await rolling_back.wait()
    poll = asyncio.create_task(system.meet.tick())
    await asyncio.sleep(0)
    finish_rollback.set()
    await poll
    assert cancellations == [1]
    assert LOBART not in system.meet._active


def public_home(system, *, npc=LOBART, degree="seen"):
    system.crimes.places = PlaceBook.load()
    assert system.crimes.places is not None
    system.crimes.people[LOBART]["routine"] = [{
        "state": "ZS_GOTOBED", "waypoint": "NW_FARM1_INHOUSE_BED_02",
    }]
    system.crimes.people[HILDA]["routine"] = [{
        "state": "ZS_GOTOBED", "waypoint": "NW_FARM1_INHOUSE_BED_01",
    }]
    system.link.room_point = "NW_FARM1_INHOUSE_01"
    system.link.hero_point = "NW_FARM1_INHOUSE_02"
    assert system.crimes.places.of(system.link.room_point).id == "FARM1_INHOUSE"
    assert system.crimes.places.of(system.link.hero_point).id == "FARM1_INHOUSE"
    case(system, npc=npc, degree=degree)


def held_room(system, *, npc=LOBART, hold="room-1"):
    system.link.receive(PerceptionHeld(
        type="perception_held", hold=hold, npc=npc, kind="enter_room", other="PLAYER", distance=400,
    ))


async def finish_room(system, *, voiced=True):
    tasks = tuple(system.meet._room_tasks)
    assert tasks
    await asyncio.wait_for(asyncio.gather(*tasks), 0.7)
    verdict = [message for message in system.link.sent if isinstance(message, HoldVerdictCmd)][-1].to
    if voiced:
        for npc, active in tuple(system.meet._active.items()):
            if active.room_warning_id is not None and active.room_warned_at is None:
                system.link.receive(NpcIdle(
                    type="npc_idle", npc=npc, act=active.room_warning_id, reason="done",
                ))
        await system.meet.tick()
    return verdict


@pytest.mark.parametrize("npc", [LOBART, HILDA])
async def test_known_public_home_warns_only_its_owner_or_household_with_native_voice(system, npc):
    public_home(system, npc=npc)
    held_room(system, npc=npc)
    assert await finish_room(system) == "server"
    assert system.link.actions == [(npc, "say_svm", {"svm": "GETOUTOFHERE"})]
    fact = system.chronicle.facts("crime_meet")[-1]
    assert fact["reaction"] == "room" and fact["event"] == "enter_room"
    assert fact["svm"] == "GETOUTOFHERE" and fact["outcome"] == "warned"
    assert system.link.player_portal == 15  # room ownership was never changed to make vanilla work


async def test_public_home_grace_then_native_nonlethal_fight_is_idempotent(system):
    public_home(system)
    held_room(system)
    assert await finish_room(system) == "server"
    system.clock.now = 4.99
    await system.meet.tick()
    assert len(system.link.actions) == 1
    held_room(system, hold="room-2")
    assert await finish_room(system) == "server"
    assert len(system.link.actions) == 1  # a repeated native poll cannot restart the warning's grace
    system.clock.now = 5.0
    await system.meet.tick()
    assert system.link.actions[-1] == (LOBART, "fight", {"npc": "PLAYER", "reason": "AR_CLEARROOM"})
    assert system.chronicle.facts("crime_meet")[-1]["outcome"] == "fight"
    system.clock.now = 100
    await system.meet.tick()
    assert len(system.link.actions) == 2


async def test_public_home_grace_starts_only_after_queued_warning_actually_finishes(system):
    public_home(system)
    held_room(system)
    assert await finish_room(system, voiced=False) == "server"
    assert not system.chronicle.facts("crime_meet")
    system.clock.now = 100
    await system.meet.tick()
    assert len(system.link.actions) == 1
    active = system.meet._active[LOBART]
    system.link.receive(NpcIdle(type="npc_idle", npc=LOBART, act=active.room_warning_id, reason="done"))
    await system.meet.tick()
    assert active.room_warned_at == 100
    assert system.chronicle.facts("crime_meet")[-1]["outcome"] == "warned"
    system.clock.now = 104.99
    await system.meet.tick()
    assert len(system.link.actions) == 1
    system.clock.now = 105
    await system.meet.tick()
    assert system.link.actions[-1] == (LOBART, "fight", {"npc": "PLAYER", "reason": "AR_CLEARROOM"})


@pytest.mark.parametrize("reason", ["failed", "timeout", "interrupted", "gone"])
async def test_public_home_unsuccessful_warning_never_escalates_or_clears_later_native_work(system, reason):
    public_home(system)
    held_room(system)
    assert await finish_room(system, voiced=False) == "server"
    active = system.meet._active[LOBART]
    system.link.native[LOBART] = "ZS_OTHER"
    system.link.receive(NpcIdle(type="npc_idle", npc=LOBART, act=active.room_warning_id, reason=reason))
    await system.meet.tick()
    assert LOBART not in system.meet._active
    assert system.link.native[LOBART] == "ZS_OTHER"
    system.clock.now = 100
    await system.meet.tick()
    assert len(system.link.actions) == 1
    assert not any(
        expr[1] == "NPC_CLEARAIQUEUE" for _, exprs in system.link.evaluations for expr in exprs
    )


async def test_public_home_replaced_warning_never_escalates_or_clears_the_new_action(system):
    public_home(system)
    held_room(system)
    assert await finish_room(system, voiced=False) == "server"
    await system.link.npc_act(LOBART, "say_svm", svm="SMALLTALK")
    system.link.native[LOBART] = "ZS_OTHER"
    system.clock.now = 100
    await system.meet.tick()
    assert LOBART not in system.meet._active
    assert system.link.native[LOBART] == "ZS_OTHER"
    assert len(system.link.actions) == 2
    assert not any(
        expr[1] == "NPC_CLEARAIQUEUE" for _, exprs in system.link.evaluations for expr in exprs
    )


async def test_public_home_pending_own_warning_is_cancelled_when_knowledge_is_settled(system):
    public_home(system)
    held_room(system)
    assert await finish_room(system, voiced=False) == "server"
    system.chronicle.record({"fact": "forgiven", "npc": LOBART, "crime": "case-1"})
    await system.meet.tick()
    assert LOBART not in system.meet._active
    assert any(expr[1] == "NPC_CLEARAIQUEUE" for _, exprs in system.link.evaluations for expr in exprs)
    assert len(system.link.actions) == 1


@pytest.mark.parametrize("left", ["room", "range"])
async def test_public_home_same_nearest_waypoint_cannot_start_fight_after_leaving_actual_room(system, left):
    public_home(system)
    held_room(system)
    assert await finish_room(system) == "server"
    if left == "room":
        system.link.same_room = False
    else:
        system.link.room_distance = 1001
    system.clock.now = 100
    await system.meet.tick()
    assert LOBART not in system.meet._active
    assert len(system.link.actions) == 1
    assert system.chronicle.facts("crime_meet")[-1]["outcome"] == "left"


async def test_leaving_public_home_during_grace_stops_its_warning_and_allows_new_entry(system):
    public_home(system)
    held_room(system)
    assert await finish_room(system) == "server"
    system.link.player_portal = 0
    system.link.hero_point = "NW_FARM1_OUT_01"
    await system.meet.tick()
    system.clock.now = 100
    await system.meet.tick()
    assert all(action != "fight" for _, action, _ in system.link.actions)
    assert LOBART not in system.meet._active
    assert system.chronicle.facts("crime_meet")[-1]["outcome"] == "left"
    system.link.player_portal = 15
    system.link.hero_point = "NW_FARM1_INHOUSE_02"
    held_room(system, hold="room-2")
    assert await finish_room(system) == "server"
    assert len(system.link.actions) == 2


@pytest.mark.parametrize("settlement", ["silenced", "forgiven", "paid", "died"])
@pytest.mark.parametrize("fighting", [False, True])
async def test_public_home_personal_resolution_stops_warning_or_its_own_fight(system, settlement, fighting):
    public_home(system)
    held_room(system)
    assert await finish_room(system) == "server"
    if fighting:
        system.clock.now = 5.0
        await system.meet.tick()
        assert system.link.reason[LOBART] == "AR_CLEARROOM"
    system.chronicle.record({
        "fact": "forgiven" if settlement == "paid" else settlement,
        "npc": LOBART, "crime": "case-1", **({"paid": 50} if settlement == "paid" else {}),
    })
    await system.meet.tick()
    assert LOBART not in system.meet._active
    assert system.link.projection[LOBART] == 0
    if settlement != "died":
        assert system.link.native.get(LOBART, "ZS_STAND") == "ZS_STAND"
    actions = len(system.link.actions)
    system.clock.now = 100
    await system.meet.tick()
    assert len(system.link.actions) == actions


@pytest.mark.parametrize("degree", [None, "suspects"])
async def test_public_home_ownership_or_suspicion_cannot_own_a_room_hold(system, degree):
    public_home(system, degree=degree)
    held_room(system)
    assert await finish_room(system) == "scripts"
    assert not system.link.actions
    assert not system.chronicle.facts("crime_meet")


@pytest.mark.parametrize("missing", ["indoors", "seen", "same_room", "distance", "actual_house", "home"])
async def test_public_home_requires_actual_own_house_and_present_identifiable_hero(system, missing):
    public_home(system)
    if missing == "indoors":
        system.link.room_indoor = False
    elif missing == "seen":
        system.link.visible = False
    elif missing == "same_room":
        system.link.same_room = False
    elif missing == "distance":
        system.link.room_distance = 1001
    elif missing == "actual_house":
        system.link.room_point = "NW_FARM1_OUT_01"
    else:
        system.crimes.people[LOBART].pop("routine")
    held_room(system)
    assert await finish_room(system) == "scripts"
    assert not system.link.actions


async def test_private_home_retains_its_existing_native_warning_and_observer(system):
    public_home(system)
    system.link.player_portal = 12
    held_room(system)
    assert await finish_room(system) == "scripts"
    assert not system.link.actions
    roster(system, LOBART, state="ZS_CLEARROOM")
    await system.meet.tick()
    fact = system.chronicle.facts("crime_meet")[-1]
    assert fact["reaction"] == "room" and "outcome" not in fact


async def test_public_home_does_not_interrupt_another_combat_provider(system):
    public_home(system)
    system.link.owned.add(LOBART)
    held_room(system)
    assert await finish_room(system) == "scripts"
    assert not system.link.actions


async def test_public_home_hold_deadline_falls_back_without_starting_a_late_warning(system, monkeypatch):
    public_home(system)
    waiting = asyncio.Event()

    async def unavailable(npcs):
        await waiting.wait()
        raise AssertionError("the timed out read must have been cancelled")

    monkeypatch.setattr(system.link, "npc_view", unavailable)
    monkeypatch.setattr("gothic_ai.crime_meet.ROOM_HOLD_SECONDS", 0.01)
    held_room(system)
    assert await finish_room(system) == "scripts"
    assert not system.link.actions


async def test_loading_cancels_public_home_grace_without_replaying_into_new_branch(system):
    public_home(system)
    held_room(system)
    assert await finish_room(system) == "server"
    system.link.receive(Loading(type="loading"))
    system.link.receive(Session(type="session", branch="B"))
    system.clock.now = 100
    await system.meet.tick()
    assert not system.meet._active
    assert not system.chronicle.facts("crime_meet")
    assert len(system.link.actions) == 1
