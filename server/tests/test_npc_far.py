from __future__ import annotations

import asyncio
from collections.abc import Callable
from dataclasses import replace
from types import SimpleNamespace
from typing import Any

import pytest

from gothic_ai.chronicle import Chronicle
from gothic_ai.crimes import Crimes
from gothic_ai.game import GameDisconnected, GameError
from gothic_ai.memory import MemoryStore
from gothic_ai.npc_crimes import NpcCrimes
from gothic_ai.npc_far import FarWorld, _combatant
from gothic_ai.npc_far_fight import TIME_LIMIT_S, Coefficients, Combatant, hit_damage, resolve
from gothic_ai.protocol import NpcRoster, NpcView, Session, Snapshot
from gothic_ai.social import Social
from tests.test_npc_crimes import NpcLink

THIEF, OWNER, WITNESS = "BAU_FAR_THIEF", "BAU_FAR_OWNER", "BAU_FAR_WITNESS"


def fight_stats(*, hp=200, strength=60, protection=30, weapon=30, flags=0,
                hitchance=(0, 60, 10), skills=(1, 2), readied=False) -> list[int]:
    # The item has DAMAGETOTAL, not per-type DAMAGE. Sheathing removes it from NPC.DAMAGE.
    damage = [weapon if readied else 0, *([0] * 7)]
    return [10, hp, hp, strength, *hitchance, *([protection] * 8), *damage,
            1, *skills, int(weapon > 0), flags, int(weapon > 0), weapon, *([0] * 8),
            int(readied and weapon > 0), weapon if readied else 0, *([0] * 8)]


STRONG = fight_stats()
WEAK = fight_stats(hp=40, strength=15, protection=0, weapon=0)
DURABLE = fight_stats(hp=10000, strength=5, protection=100, weapon=0)


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
        self.values = [*STRONG, *WEAK, 3]
        self.after_eval: Callable[[], None] | None = None
        self.after_effect: Callable[[], None] | None = None
        self.effect_error: Exception | None = None
        self.fight_error: Exception | None = None
        self.fight_applies = True
        self.fight_attempts: list[tuple[str, list[dict[str, Any]], bool]] = []
        self.down: set[str] = set()
        self.hp: dict[str, int] = {}
        self.readback_error: Exception | None = None

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
        if len(exprs) == 2 and exprs[1][:2] == ["call", "NPC_ISINSTATE"]:
            if self.readback_error is not None:
                raise self.readback_error
            return [1, 1] if npc in self.down else [40, 0]
        if len(exprs) == 2 and all(expr[:1] == ["field"] for expr in exprs):
            if self.readback_error is not None:
                raise self.readback_error
            return [self.hp[expr[1][1]] for expr in exprs]
        return self.values[:len(exprs)]

    async def story_effect(self, npc: str, effects: list[dict[str, Any]], *, check=False) -> None:
        effect = effects[0]
        if effect["do"] in {"knockout", "wound"}:
            self.fight_attempts.append((npc, effects, check))
            if (isinstance(self.fight_error, GameError)
                    and not isinstance(self.fight_error, GameDisconnected)):
                raise self.fight_error
            if self.fight_applies:
                if effect["do"] == "knockout":
                    self.down.add(npc)
                else:
                    self.hp[npc] = max(1, self.values[1] - effect["loss"])
                    self.hp[effect["by"]] = max(1, self.values[49] - effect["by_loss"])
                self.effects.append((npc, effects, check))
            if self.after_effect is not None:
                self.after_effect()
            if self.fight_error is not None:
                raise self.fight_error
            return
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


@pytest.mark.parametrize("wins", [True, False])
async def test_far_fight_knocks_out_loser_before_returning_goods_and_repeats_once(world, wins):
    world.link.values = [*(STRONG if wins else WEAK), *(WEAK if wins else STRONG), 3]
    result = await world.far.fight(crime(), OWNER, 0)
    assert result["far"] and result["returned"] == (2 if wins else 0)
    assert result["outcome"] == ("reclaimed" if wins else "defender_down")
    loser, winner = (THIEF, OWNER) if wins else (OWNER, THIEF)
    expected = resolve(result["act"], _combatant(OWNER, world.link.values[:48]),
                       _combatant(THIEF, world.link.values[48:96]), Coefficients(0.4, 0.5, 1.0))
    assert result["winner"] == winner == expected.winner
    assert (result["reason"], result["model"]) == ("hits_to_down", "hits_v1")
    assert (result["loss"], result["duration_s"], result["hits"], result["p_win"]) == (
        expected.loss, expected.duration_s, expected.hits, round(expected.p_win, 3),
    )
    effects = [(loser, [{"do": "knockout", "by": winner, "loss": expected.loss}], True)]
    if wins:
        effects.append((THIEF, [{"do": "give", "item": "ITFO_BREAD", "count": 2,
                                "to": "npc", "npc": OWNER}], True))
    assert world.link.effects == effects
    assert await world.far.fight(crime(), OWNER, 0) == result
    assert world.link.effects == effects
    assert len(world.link.evaluations) == 1
    assert len(world.chronicle.facts("npc_crime_defense")) == 1
    assert len(world.chronicle.facts("npc_crime_restitution")) == int(wins)
    expressions = world.link.evaluations[0][1]
    assert expressions[:4] == [["field", ["inst", OWNER], "LEVEL"],
                               ["field", ["inst", OWNER], "ATTRIBUTE", 0],
                               ["field", ["inst", OWNER], "ATTRIBUTE", 1],
                               ["field", ["inst", OWNER], "ATTRIBUTE", 4]]
    assert ["call", "NPC_GETTALENTSKILL", ["inst", OWNER], ["int", 2]] in expressions
    calls = [node for expr in expressions for node in _nodes(expr) if node[0] == "call"]
    assert calls and all(isinstance(arg, list) for call in calls for arg in call[2:])
    assert ["field", ["call", "NPC_GETEQUIPPEDMELEEWEAPON", ["inst", OWNER]], "FLAGS"] in expressions
    saved = world.memory.head("A")
    world.link.receive(Session(type="session", branch="loaded", parent="A", parent_seq=saved, reason="load"))
    world.will._generation += 1
    assert await FarWorld(world.will).fight(crime(), OWNER, 1) == result
    assert world.link.effects == effects


def _nodes(expr: Any) -> list[list[Any]]:
    """Every AST node of a story_eval expression: the bridge reads call arguments only as nodes."""
    if not isinstance(expr, list) or not expr or not isinstance(expr[0], str):
        return []
    return [expr, *[node for part in expr[1:] for node in _nodes(part)]]


@pytest.mark.parametrize("kind,goods,outcome,returned", [
    ("theft", 1, "reclaimed", 1), ("theft", 0, "no_goods", 0),
    ("attack", 0, "defeated", 0),
])
async def test_far_fight_missing_goods_partial_return_and_attack(world, kind, goods, outcome, returned):
    world.link.values[-1] = goods
    result = await world.far.fight(crime(kind=kind), OWNER, 0)
    assert (result["outcome"], result["returned"]) == (outcome, returned)
    assert len(world.link.effects) == 1 + int(returned > 0)


async def test_far_timeout_wounds_both_before_facts_without_returning_goods(world):
    world.link.values = [*DURABLE, *DURABLE, 3]
    world.link.after_effect = lambda: world.chronicle.facts() == [] or pytest.fail(
        "Distant fight facts preceded the effect",
    )
    result = await world.far.fight(crime(), OWNER, 0)
    expected = resolve(result["act"], _combatant(OWNER, DURABLE), _combatant(THIEF, DURABLE),
                       Coefficients(0.4, 0.5, 1.0))
    assert (result["outcome"], result["reason"], result["model"], result["winner"]) == (
        "timeout", "deadline", "hits_v1", None,
    )
    assert result["far"] and result["returned"] == 0 and result["duration_s"] == 120
    assert (result["loss"], result["by_loss"], result["hits"], result["p_win"],
            result["p_timeout"]) == (expected.loss, expected.by_loss, expected.hits, 0, 1)
    assert world.link.effects == [(OWNER, [{
        "do": "wound", "by": THIEF, "loss": expected.loss, "by_loss": expected.by_loss,
    }], True)]
    assert world.link.hp == {OWNER: DURABLE[1] - expected.loss,
                             THIEF: DURABLE[1] - expected.by_loss}
    assert not world.link.down and not world.chronicle.facts("npc_crime_restitution")
    assert await world.far.fight(crime(), OWNER, 0) == result
    assert len(world.link.fight_attempts) == 1
    assert len(world.chronicle.facts("npc_crime_defense")) == 1


@pytest.mark.parametrize("offsets", [(0, 0), (1, 0), (0, 1)])
async def test_unknown_wound_requires_both_expected_hp_and_never_sends_twice(world, offsets):
    world.link.values = [*DURABLE, *DURABLE, 3]
    world.link.fight_error = TimeoutError("lost response")

    def change_hp():
        for npc, offset in zip((OWNER, THIEF), offsets, strict=True):
            world.link.hp[npc] += offset

    world.link.after_effect = change_hp
    if any(offsets):
        with pytest.raises(TimeoutError, match="lost response"):
            await world.far.fight(crime(), OWNER, 0)
        assert world.chronicle.facts() == []
        with pytest.raises(GameError, match="cannot be replayed"):
            await world.far.fight(crime(), OWNER, 0)
        assert world.chronicle.facts() == []
        for npc, offset in zip((OWNER, THIEF), offsets, strict=True):
            world.link.hp[npc] -= offset
    result = await world.far.fight(crime(), OWNER, 0)
    assert result["outcome"] == "timeout"
    assert await world.far.fight(crime(), OWNER, 0) == result
    assert len(world.link.fight_attempts) == 1
    assert len(world.chronicle.facts("npc_crime_defense")) == 1
    assert world.link.evaluations[1] == (OWNER, [
        ["field", ["inst", OWNER], "ATTRIBUTE", 0],
        ["field", ["inst", THIEF], "ATTRIBUTE", 0],
    ])


@pytest.mark.parametrize("during", ["eval", "knockout", "give", "wound"])
async def test_loading_during_far_fight_never_writes_old_world_facts(world, during):
    if during == "wound":
        world.link.values = [*DURABLE, *DURABLE, 3]
    def load() -> None:
        world.will._generation += 1
        world.link.receive(Session(type="session", branch="B"))
    if during == "eval":
        world.link.after_eval = load
    else:
        world.link.after_effect = lambda: load() if world.link.effects[-1][1][0]["do"] == during else None
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
    assert [e[1][0]["do"] for e in world.link.effects] == ["knockout"]
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
    assert [e[1][0]["do"] for e in world.link.effects] == ["knockout", "give"]


async def test_unknown_give_result_keeps_reservation_against_duplicate_transfer(world):
    world.link.effect_error = TimeoutError()
    with pytest.raises(TimeoutError):
        await world.far.fight(crime(), OWNER, 0)
    assert (world.deeds._generation, crime()["id"]) in world.deeds._restitution_started
    world.link.effect_error = None
    result = await world.far.fight(crime(), OWNER, 0)
    assert result["outcome"] == "resolved_elsewhere"
    assert [e[1][0]["do"] for e in world.link.effects] == ["knockout"]


async def test_settled_case_cannot_start_another_far_fight(world):
    world.chronicle.record({"fact": "npc_crime_settled", "crime": crime()["id"],
                            "by": WITNESS, "how": "returned"})
    assert await world.far.fight(crime(), OWNER, 0) is None
    assert world.link.evaluations == []
    assert world.link.effects == []
    assert world.chronicle.facts("npc_crime_defense") == []


@pytest.mark.parametrize("timeout", [False, True])
async def test_fight_rejection_records_no_facts_and_next_tick_sees_near_body(world, timeout):
    if timeout:
        world.link.values = [*DURABLE, *DURABLE, 3]
    before = list(world.chronicle.facts())
    world.link.fight_error = GameError("inside simulation")
    with pytest.raises(GameError, match="inside simulation"):
        await world.far.fight(crime(), OWNER, 0)
    assert world.chronicle.facts() == before
    assert world.link.effects == []
    assert len(world.link.fight_attempts) == 1
    world.link.runtime[THIEF]["pos"] = [100, 0, 0]
    world.will._now += 1
    assert not await world.far.far(THIEF, 0)


@pytest.mark.parametrize("applied", [True, False])
@pytest.mark.parametrize("error", [TimeoutError, ConnectionError, GameDisconnected])
async def test_unknown_knockout_is_read_back_and_never_sent_again(world, applied, error):
    world.link.fight_error = error("lost response")
    world.link.fight_applies = applied
    if not applied:
        with pytest.raises(error, match="lost response"):
            await world.far.fight(crime(), OWNER, 0)
        assert world.chronicle.facts() == []
        with pytest.raises(GameError, match="cannot be replayed"):
            await world.far.fight(crime(), OWNER, 0)
        assert world.chronicle.facts() == []
        assert world.link.evaluations[1] == world.link.evaluations[2]
        world.link.down.add(THIEF)  # The bridge applied the original request after readback.
    result = await world.far.fight(crime(), OWNER, 0)
    assert result["outcome"] == "reclaimed"
    assert await world.far.fight(crime(), OWNER, 0) == result
    assert len(world.link.fight_attempts) == 1
    assert len(world.chronicle.facts("npc_crime_defense")) == 1
    assert len(world.chronicle.facts("npc_crime_restitution")) == 1
    assert world.link.evaluations[1] == (THIEF, [
        ["field", ["inst", THIEF], "ATTRIBUTE", 0],
        ["call", "NPC_ISINSTATE", ["inst", THIEF], ["sym", "ZS_UNCONSCIOUS"]],
    ])


@pytest.mark.parametrize("timeout", [False, True])
async def test_retry_after_unavailable_fight_readback_only_checks_body(world, timeout):
    if timeout:
        world.link.values = [*DURABLE, *DURABLE, 3]
    world.link.fight_error = TimeoutError()
    world.link.readback_error = ConnectionError()
    with pytest.raises(ConnectionError):
        await world.far.fight(crime(), OWNER, 0)
    assert world.chronicle.facts() == []
    world.link.readback_error = None
    result, repeated = await asyncio.gather(
        world.far.fight(crime(), OWNER, 0), world.far.fight(crime(), OWNER, 0),
    )
    assert result == repeated and result["outcome"] == ("timeout" if timeout else "reclaimed")
    assert len(world.link.fight_attempts) == 1
    assert len(world.chronicle.facts("npc_crime_defense")) == 1


@pytest.mark.parametrize("timeout", [False, True])
async def test_loading_during_fight_readback_drops_old_world_facts(world, timeout):
    if timeout:
        world.link.values = [*DURABLE, *DURABLE, 3]
    world.link.fight_error = TimeoutError()

    def load_during_readback():
        if len(world.link.evaluations) == 2:
            world.will._generation += 1
            world.link.receive(Session(type="session", branch="B"))

    world.link.after_eval = load_during_readback
    assert await world.far.fight(crime(), OWNER, 0) is None
    assert world.chronicle.facts() == []


@pytest.mark.parametrize("flags,expected_chance,expected_tier", [
    (0, 60, 1), (1 << 16, 10, 2), (1 << 17, 10, 2),
])
def test_drawn_equipped_weapon_uses_native_damage_mask_and_selected_talent(
    flags, expected_chance, expected_tier,
):
    sheathed = _combatant(OWNER, fight_stats(flags=flags))
    drawn = _combatant(OWNER, fight_stats(flags=flags, readied=True))
    assert sheathed == drawn
    assert (sheathed.damage, sheathed.damage_type, sheathed.hitchance, sheathed.tier) == (
        (30, *([0] * 7)), 1, expected_chance, expected_tier,
    )
    fists = _combatant(THIEF, fight_stats(weapon=0, hitchance=(7, 60, 100)))
    assert (fists.hitchance, fists.tier, fists.damage) == (7, 0, (0,) * 8)


def test_readied_weapon_contribution_is_replaced_including_per_type_damage():
    values = fight_stats()
    # Native NPC damage = base (3,4) + readied item (2,5) + total10 in type1.
    values[15:17] = [5, 19]
    values[30:32] = [7, 8]  # melee item's per-type damage plus total30 in type0
    values[38:42] = [2, 10, 2, 5]
    actor = _combatant(OWNER, values)
    assert actor.damage[:2] == (40, 12)
    assert actor.damage_type == 1


@pytest.mark.parametrize("contents", [None, "{", "{}"])
def test_far_world_load_requires_valid_calibration(world, tmp_path, monkeypatch, contents):
    path = tmp_path / "npc_far.py"
    calibration = tmp_path / "gamedata" / "far_fight.json"
    calibration.parent.mkdir()
    if contents is not None:
        calibration.write_text(contents)
    monkeypatch.setattr("gothic_ai.npc_far.__file__", str(path))
    with pytest.raises((FileNotFoundError, ValueError)):
        FarWorld(world.will)


@pytest.mark.parametrize("mask,damage,protection,critical,expected", [
    (1, (30, 0), (30, 0), True, 60),  # 60 STR + 30 damage - 30 protection
    (1, (30, 0), (30, 0), False, 5),  # (60 - 1) // 10
    (3, (30, 20), (30, 10), True, 130),  # 60 + 70, STR contributes to each type
    (3, (30, 20), (30, 10), False, 11),  # 5 + 6, division precedes the sum
    (1, (30, 0), (90, 0), True, 5),  # protection absorbs all damage, minimum remains
    (1, (30, 0), (100, 0), False, 5),
    (3, (30, 20), (100, 10), False, 6),  # absorbed type adds 0, C++ truncates -1 / 10 to 0
    (3, (30, 20), (-1, 10), True, 70),  # immune first type contributes nothing
    (3, (30, 20), (-1, 10), False, 6),
    (1, (30, 0), (-1, 0), True, 5),  # explicit model minimum after immunity filtering
])
def test_far_hit_damage_matches_manual_g2_calculation(mask, damage, protection, critical, expected):
    attacker = Combatant("A", 100, 60, (*damage, *([0] * 6)), mask, (0,) * 8, 60, 1)
    defender = replace(attacker, name="B", protection=(*protection, *([0] * 6)))
    coefficients = Coefficients(0.4, 0.5, 1.0)
    assert hit_damage(attacker, defender, critical, coefficients) == expected


def test_far_fists_use_supplied_unknown_talent_chance_and_scaled_minimum():
    fists = Combatant("FISTS", 40, 15, (0,) * 8, 1, (0,) * 8, 0, 0)
    defender = replace(fists, name="DEFENDER")
    assert hit_damage(fists, defender, False, Coefficients(0.4, 0.5, 1.0)) == 5
    assert hit_damage(fists, defender, False, Coefficients(0.4, 0.5, 1.5)) == 8
    # With UNKNOWN hitchance zero no critical is drawn: 39 HP requires exactly eight hits.
    result = resolve("far:fists", fists, defender, Coefficients(0.4, 0.5, 1.0))
    assert result.hits[result.winner] == 8
    assert result.loss % 5 == 0


def test_far_fight_mirrors_are_balanced_and_act_is_repeatable():
    fighter = Combatant("FIGHTER", 100, 30, (20, *([0] * 7)), 1, (10,) * 8, 40, 1)
    culprit = replace(fighter, name="CULPRIT")
    coefficients = Coefficients(0.4, 0.5, 1.0)
    act = "far:mirror-case:FIGHTER:reclaim:CULPRIT:"
    result = resolve(act, fighter, culprit, coefficients)
    assert 0.4 <= result.p_win <= 0.6
    assert resolve(act, fighter, culprit, coefficients) == result
    other = resolve("far:other-case:FIGHTER:reclaim:CULPRIT:", fighter, culprit, coefficients)
    assert (other.winner, other.loss, other.duration_s, other.hits) != (
        result.winner, result.loss, result.duration_s, result.hits,
    )
    assert result.duration_s > 0
    assert 0 <= result.loss < fighter.hp


def test_far_fight_armed_veteran_beats_unarmed_weakling():
    fighter = Combatant("VETERAN", 200, 60, (30, *([0] * 7)), 1, (30,) * 8, 60, 1)
    culprit = Combatant("WEAKLING", 40, 15, (0,) * 8, 1, (0,) * 8, 0, 0)
    result = resolve("far:unequal-case:VETERAN:reclaim:WEAKLING:", fighter, culprit,
                     Coefficients(0.4, 0.5, 1.0))
    assert result.winner == fighter.name
    assert result.duration_s < TIME_LIMIT_S
    assert result.p_win >= 0.95
    assert result.p_timeout == 0


def test_far_fight_durable_mirrors_time_out_with_both_losses_and_repeatable_act():
    fighter = Combatant("FIGHTER", 10000, 15, (0,) * 8, 1, (100,) * 8, 0, 0)
    culprit = replace(fighter, name="CULPRIT")
    coefficients = Coefficients(0.4, 0.5, 1.0)
    act = "far:durable-case:FIGHTER:reclaim:CULPRIT:"
    result = resolve(act, fighter, culprit, coefficients)
    assert result.winner is None
    assert result.duration_s == TIME_LIMIT_S == 120
    assert result.loss == result.hits[culprit.name] * 5 > 0
    assert result.by_loss == result.hits[fighter.name] * 5 > 0
    assert result.p_win == 0
    assert result.p_timeout == 1
    assert resolve(act, fighter, culprit, coefficients) == result


@pytest.mark.parametrize("change", [
    {"version": 2}, {"rate_base": float("nan")}, {"damage_scale": 0},
])
def test_far_fight_rejects_invalid_calibration(change):
    data = {"version": 1, "rate_base": 0.4, "rate_per_tier": 0.5,
            "damage_scale": 1.0, "source": "uncalibrated", **change}
    with pytest.raises(ValueError):
        Coefficients.from_mapping(data)
