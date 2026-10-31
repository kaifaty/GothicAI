from __future__ import annotations

import asyncio
from collections.abc import Callable
from types import SimpleNamespace
from typing import Any

import pytest

from gothic_ai.chronicle import Chronicle
from gothic_ai.crimes import Crimes
from gothic_ai.game import GameError
from gothic_ai.memory import MemoryStore
from gothic_ai.npc_crimes import NpcCrimes
from gothic_ai.npc_far import FarWorld
from gothic_ai.protocol import NpcRoster, NpcView, Session, Snapshot
from gothic_ai.social import Social
from tests.test_npc_crimes import NpcLink

THIEF, OWNER, WITNESS = "BAU_FAR_THIEF", "BAU_FAR_OWNER", "BAU_FAR_WITNESS"


def person(npc: str, pos: float, *, guild: int = 12, state: str = "ZS_STAND",
           waypoint: str = "FARM", routine: list[dict[str, Any]] | None = None) -> dict[str, Any]:
    return {"npc": npc, "name": npc, "guild": guild, "routine": routine or [
        {"start": 0, "end": 0, "state": state, "waypoint": waypoint, "pos": [pos, 0, 0]},
    ]}


class FarLink(NpcLink):
    def __init__(self) -> None:
        super().__init__()
        self.runtime = {
            THIEF: {"state": "alive", "pos": [10000, 0, 0]},
            OWNER: {"state": "alive", "pos": [14000, 0, 0]},
            WITNESS: {"state": "alive", "pos": [10200, 0, 0]},
            "PC_HERO": {"state": "alive", "pos": [0, 0, 0]},
        }
        self.views = {npc: NpcView(npc=npc, name=npc, theft_range=500)
                      for npc in (THIEF, OWNER, WITNESS)}
        self.snapshots: dict[str, Snapshot] = {}
        self.queries: list[list[str]] = []
        self.snapshot_queries: list[tuple[str, bool]] = []
        self.values = [10, 20, 50, 5, 100, 200, 3]
        self.after_eval: Callable[[], None] | None = None
        self.after_effect: Callable[[], None] | None = None
        self.effect_error: Exception | None = None

    async def story_state(self, **args: Any) -> dict[str, Any]:
        self.queries.append(args["npcs"])
        return {"npcs": {n: self.runtime[n] for n in args["npcs"] if n in self.runtime}}

    async def snapshot(self, npc: str, *, fresh: bool = False) -> Snapshot:
        self.snapshot_queries.append((npc, fresh))
        if npc not in self.snapshots:
            raise GameError("snapshot unavailable")
        return self.snapshots[npc]

    async def npc_view(self, npcs: list[str]) -> list[NpcView]:
        return [self.views[n] for n in npcs if n in self.views]

    async def story_eval(self, npc: str, exprs: list[Any]) -> list[Any]:
        self.evaluations.append((npc, exprs))
        if self.after_eval is not None:
            self.after_eval()
        return self.values[:len(exprs)]

    async def story_effect(self, npc: str, effects: list[dict[str, Any]], *, check=False) -> None:
        if self.effect_error is not None:
            raise self.effect_error
        self.effects.append((npc, effects, check))
        if self.after_effect is not None:
            self.after_effect()


@pytest.fixture
async def world():
    link, memory = FarLink(), MemoryStore(":memory:")
    chronicle = Chronicle(link, memory)
    crimes = Crimes(link, chronicle)
    deeds = NpcCrimes(link, chronicle, crimes=crimes)
    social = Social([person(THIEF, 10000), person(OWNER, 14000), person(WITNESS, 10200)])
    will = SimpleNamespace(
        link=link, chronicle=chronicle, npc_crimes=deeds, rumors=SimpleNamespace(social=social),
        roster=deeds.roster, _now=8 * 60, _generation=0, _active={}, guard=deeds.guard,
        busy=lambda _: False,
    )
    yield SimpleNamespace(link=link, chronicle=chronicle, memory=memory,
                          deeds=deeds, will=will, far=FarWorld(will))
    await deeds.close()
    await crimes.close()
    if chronicle._background:
        await asyncio.gather(*chronicle._background)
    memory.close()


async def test_batch_engine_positions_take_priority_over_routines_and_share_fresh_hero(world):
    world.link.runtime[THIEF]["pos"] = [3000, 0, 0]
    position = await world.far.position(THIEF, 0)
    assert (position.pos, position.source, position.state) == ((3000, 0, 0), "engine", "ZS_STAND")
    assert await world.far.far(THIEF, 0)
    assert await world.far.position(OWNER, 0) is not None
    assert len(world.link.queries) == 1
    assert "PC_HERO" in world.link.queries[0]
    assert world.link.snapshot_queries == []
    world.link.runtime[THIEF]["pos"] = [2999, 0, 0]
    await world.far.refresh(0)
    assert not await world.far.far(THIEF, 0)


async def test_missing_engine_position_uses_only_current_routine_entry(world):
    world.will.rumors.social = Social([person(THIEF, 0, routine=[
        {"start": 0, "end": 9 * 60, "state": "ZS_STAND", "waypoint": "FARM", "pos": [10000, 0, 0]},
        {"start": 9 * 60, "end": 0, "state": "ZS_GOTOBED", "waypoint": "BED", "pos": [20000, 0, 0]},
    ])])
    world.link.runtime[THIEF] = {"state": "alive"}
    first = await world.far.position(THIEF, 0)
    assert (first.pos, first.source, first.waypoint) == ((10000, 0, 0), "routine", "FARM")
    assert world.far.social.users("FARM", world.will._now) == {THIEF}
    world.will._now = 9 * 60
    second = await world.far.position(THIEF, 0)
    assert (second.pos, second.state, second.waypoint) == ((20000, 0, 0), "ZS_GOTOBED", "BED")
    assert world.far.social.users("FARM", world.will._now) == set()
    assert not world.far.free(THIEF)


async def test_missing_batch_positions_can_use_fresh_snapshot(world):
    world.link.runtime[THIEF] = {"state": "alive"}
    world.link.snapshots[THIEF] = Snapshot.model_validate({
        "type": "snapshot", "t": 1,
        "world": {"name": "WORLD", "day": 0, "hour": 8, "minute": 0},
        "npc": {"id": THIEF, "name": THIEF, "pos": [12000, 0, 0], "rot": 0,
                "hp": 100, "hp_max": 100, "busy": False},
        "player": {"name": "Герой", "pos": [1000, 0, 0], "distance": 11000},
    })
    position = await world.far.position(THIEF, 0)
    assert (position.pos, position.source) == ((12000, 0, 0), "engine")
    assert world.link.snapshot_queries == [(THIEF, True)]


@pytest.mark.parametrize("status", ["dead", "absent"])
async def test_proven_dead_or_absent_npc_never_appears_by_routine(world, status):
    world.link.runtime[THIEF] = {"state": status}
    assert await world.far.position(THIEF, 0) is None
    assert not world.far.free(THIEF)
    assert not await world.far.far(THIEF, 0)


async def test_unknown_hero_does_not_invent_distant_results(world):
    world.link.runtime.pop("PC_HERO")
    assert not await world.far.far(THIEF, 0)


@pytest.mark.parametrize("blocked", ["busy", "body", "active", "post", "fight"])
async def test_distant_body_still_respects_activity_and_storyguard(world, blocked):
    if blocked == "busy":
        world.will.busy = lambda _: True
    elif blocked == "body":
        world.link.owned.add(THIEF)
    elif blocked == "active":
        world.will._active[THIEF] = "a1"
    else:
        world.link.receive(NpcRoster(type="npc_roster", t=0, npcs=[{
            "id": THIEF, "name": THIEF, "guild": 12, "distance": 10000,
            "state": "ZS_GUARD_PASSAGE" if blocked == "post" else "ZS_STAND",
            "activity": "fight" if blocked == "fight" else "routine",
        }]))
    await world.far.refresh(0)
    assert not world.far.free(THIEF)


async def test_meeting_requires_current_routine_place_and_both_outside_simulation(world):
    assert await world.far.meeting(THIEF, OWNER, 0)
    world.will.rumors.social = Social([
        person(THIEF, 10000), person(OWNER, 20000, waypoint="OTHER"),
    ])
    assert not await world.far.meeting(THIEF, OWNER, 0)
    world.will.rumors.social = Social([person(THIEF, 10000), person(OWNER, 10000)])
    world.link.runtime[OWNER]["pos"] = [500, 0, 0]
    await world.far.refresh(0)
    assert not await world.far.meeting(THIEF, OWNER, 0)


async def test_trade_tie_alone_is_not_a_physical_meeting(world):
    social = Social([person(THIEF, 10000), person(OWNER, 20000, waypoint="OTHER")])
    social.tie(THIEF, OWNER).deals.append("SHARED_QUEST")
    world.will.rumors.social = social
    assert not await world.far.meeting(THIEF, OWNER, 0)


async def test_witnesses_are_humans_awake_within_their_own_native_theft_range(world):
    close, asleep, beast, out, boundary = "BAU_CLOSE", "BAU_SLEEP", "WOLF", "BAU_OUT", "BAU_BOUNDARY"
    world.will.rumors.social = Social([
        person(THIEF, 10000), person(close, 10100), person(asleep, 10100, state="ZS_GOTOBED"),
        person(beast, 10100, guild=16), person(out, 10501), person(boundary, 10500),
    ])
    for npc, pos in ((close, 10100), (asleep, 10100), (beast, 10100), (out, 10501), (boundary, 10500)):
        world.link.runtime[npc] = {"state": "alive", "pos": [pos, 0, 0]}
        world.link.views[npc] = NpcView(npc=npc, name=npc, theft_range=500, theft_ready=False)
    world.link.views[close] = NpcView(npc=close, name=close, theft_range=99)
    assert await world.far.witnesses(THIEF, 0) == [boundary]
    world.link.views[close] = NpcView(npc=close, name=close, theft_range=100)
    assert await world.far.witnesses(THIEF, 0) == [boundary, close]


async def test_thief_belief_is_independent_from_honest_distant_witnesses(world):
    assert await world.far.alone(THIEF, 0)
    assert await world.far.witnesses(THIEF, 0) == [WITNESS]
    world.link.views[THIEF] = NpcView(npc=THIEF, name=THIEF, theft_range=500,
                                    sees=[{"id": WITNESS, "name": WITNESS, "distance": 200}])
    assert not await world.far.alone(THIEF, 0)


def crime(*, kind: str = "theft") -> dict[str, Any]:
    return {"id": "far-case", "kind": kind, "culprit": THIEF, "owner": OWNER,
            "item": "ITFO_BREAD", "count": 2}


@pytest.mark.parametrize("defender,culprit,wins", [
    ((2, 1, 1), (1, 100, 100), True),
    ((1, 2, 1), (1, 1, 100), True),
    ((1, 1, 2), (1, 1, 1), True),
    ((1, 1, 1), (1, 1, 1), False),
    ((1, 100, 100), (2, 1, 1), False),
])
async def test_far_fight_is_lexicographic_deterministic_and_returns_actual_goods(
    world, defender, culprit, wins,
):
    world.link.values = [*defender, *culprit, 3]
    result = await world.far.fight(crime(), OWNER, 0)
    assert result["far"] and result["returned"] == (2 if wins else 0)
    assert result["outcome"] == ("reclaimed" if wins else "defender_down")
    effects = [(THIEF, [{"do": "give", "item": "ITFO_BREAD", "count": 2,
                         "to": "npc", "npc": OWNER}], True)] if wins else []
    assert world.link.effects == effects
    assert await world.far.fight(crime(), OWNER, 0) == result
    assert world.link.effects == effects
    assert len(world.link.evaluations) == 1
    assert len(world.chronicle.facts("npc_crime_defense")) == 1
    assert len(world.chronicle.facts("npc_crime_restitution")) == int(wins)
    expressions = world.link.evaluations[0][1]
    assert expressions[:3] == [["field", ["inst", OWNER], "LEVEL"],
                               ["field", ["inst", OWNER], "ATTRIBUTE", 4],
                               ["field", ["inst", OWNER], "ATTRIBUTE", 0]]


@pytest.mark.parametrize("kind,goods,outcome,returned", [
    ("theft", 1, "reclaimed", 1), ("theft", 0, "no_goods", 0),
    ("attack", 0, "defeated", 0),
])
async def test_far_fight_missing_goods_partial_return_and_attack(world, kind, goods, outcome, returned):
    world.link.values[-1] = goods
    result = await world.far.fight(crime(kind=kind), OWNER, 0)
    assert (result["outcome"], result["returned"]) == (outcome, returned)
    assert len(world.link.effects) == int(returned > 0)


@pytest.mark.parametrize("during", ["eval", "effect"])
async def test_loading_during_far_fight_never_writes_old_world_facts(world, during):
    def load() -> None:
        world.will._generation += 1
        world.link.receive(Session(type="session", branch="B"))
    if during == "eval":
        world.link.after_eval = load
    else:
        world.link.after_effect = load
    assert await world.far.fight(crime(), OWNER, 0) is None
    assert world.chronicle.facts("npc_crime_defense") == []
    assert world.chronicle.facts("npc_crime_restitution") == []


async def test_load_discards_positions_from_previous_generation(world):
    assert (await world.far.position(THIEF, 0)).pos == (10000, 0, 0)
    world.will._generation += 1
    world.link.runtime[THIEF]["pos"] = [500, 0, 0]
    assert await world.far.position(THIEF, 0) is None
    assert (await world.far.position(THIEF, 1)).pos == (500, 0, 0)
    assert not await world.far.far(THIEF, 1)


async def test_native_restitution_in_progress_blocks_duplicate_far_transfer(world):
    world.deeds._restitution_started.add((world.deeds._generation, crime()["id"]))
    result = await world.far.fight(crime(), OWNER, 0)
    assert result["outcome"] == "resolved_elsewhere"
    assert world.link.effects == []
    assert world.chronicle.facts("npc_crime_restitution") == []


async def test_explicit_give_rejection_releases_reservation_for_retry(world):
    world.link.effect_error = GameError("not enough items")
    with pytest.raises(GameError, match="not enough items"):
        await world.far.fight(crime(), OWNER, 0)
    assert (world.deeds._generation, crime()["id"]) not in world.deeds._restitution_started
    assert world.chronicle.facts("npc_crime_defense") == []
    assert world.chronicle.facts("npc_crime_restitution") == []
    world.link.effect_error = None
    result = await world.far.fight(crime(), OWNER, 0)
    assert result["outcome"] == "reclaimed"
    assert len(world.link.effects) == 1


async def test_unknown_give_result_keeps_reservation_against_duplicate_transfer(world):
    world.link.effect_error = TimeoutError()
    with pytest.raises(TimeoutError):
        await world.far.fight(crime(), OWNER, 0)
    assert (world.deeds._generation, crime()["id"]) in world.deeds._restitution_started
    world.link.effect_error = None
    result = await world.far.fight(crime(), OWNER, 0)
    assert result["outcome"] == "resolved_elsewhere"
    assert world.link.effects == []


async def test_settled_case_cannot_start_another_far_fight(world):
    world.chronicle.record({"fact": "npc_crime_settled", "crime": crime()["id"],
                            "by": WITNESS, "how": "returned"})
    assert await world.far.fight(crime(), OWNER, 0) is None
    assert world.link.evaluations == []
    assert world.link.effects == []
    assert world.chronicle.facts("npc_crime_defense") == []
