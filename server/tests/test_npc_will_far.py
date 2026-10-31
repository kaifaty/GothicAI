from __future__ import annotations

import asyncio
from collections import defaultdict
from types import SimpleNamespace
from typing import Any

import pytest

from gothic_ai.chronicle import Chronicle
from gothic_ai.crimes import Crimes
from gothic_ai.game import GameError
from gothic_ai.memory import MemoryStore
from gothic_ai.npc_crimes import NpcCrimes
from gothic_ai.npc_will import NpcWill
from gothic_ai.people_knowledge import PeopleKnowledge
from gothic_ai.protocol import Chest, NpcView, NpcViews, Session
from gothic_ai.social import Social
from tests.test_crimes import HILDA, LOBART
from tests.test_npc_crimes import NAMES, THIEF
from tests.test_npc_will import (
    DAY,
    NOW,
    WillLink,
    character,
    commit_theft,
    finish_action,
    greed_scene,
    theft,
)

ITEM = "ITFO_BREAD"
CHEST = "CHEST_LOBART"


class FarLink(WillLink):
    def __init__(self) -> None:
        super().__init__()
        self.engine: dict[str, dict[str, Any]] = {}
        self.state_queries: list[dict[str, Any]] = []
        self.inventory: dict[str, dict[str, int]] = defaultdict(lambda: defaultdict(int))

    async def story_state(self, **args: Any) -> dict[str, Any]:
        self.state_queries.append(args)
        return {
            "deed_target": self.target,
            "npcs": {npc: self.engine[npc] for npc in args.get("npcs", ()) if npc in self.engine},
        }

    async def story_eval(self, npc: str, exprs: list[Any]) -> list[Any]:
        self.evaluations.append((npc, exprs))
        values = []
        for expression in exprs:
            if expression[0] == "field":
                field = expression[2]
                values.append({"GUILD": 12, "NPCTYPE": 0, "LEVEL": 5, "ATTRIBUTE": 100}[field])
            elif expression[:2] == ["call", "NPC_HASITEMS"]:
                values.append(self.inventory[expression[2][1]][expression[3][1]])
            else:
                raise AssertionError(f"unexpected expression: {expression}")
        return values

    async def story_effect(self, npc: str, effects: list[dict[str, Any]], *, check=False) -> None:
        for effect in effects:
            item, count = effect["item"], effect["count"]
            if effect["do"] == "chest_take":
                owner, index, chest = next(
                    (owner, index, chest)
                    for owner, chests in self.chests.items()
                    for index, chest in enumerate(chests)
                    if chest.id == effect["chest"]
                )
                stack = next(i for i in chest.items if i.item == item)
                if stack.count < count:
                    raise GameError("not enough items in chest")
                self.chests[owner][index] = chest.model_copy(update={"items": [
                    entry.model_copy(update={"count": entry.count - count}) if entry.item == item else entry
                    for entry in chest.items
                ]})
                self.inventory[npc][item] += count
            elif effect["do"] == "give":
                if self.inventory[npc][item] < count:
                    raise GameError("not enough items in inventory")
                self.inventory[npc][item] -= count
                self.inventory[effect["npc"]][item] += count
            else:
                raise AssertionError(f"unexpected effect: {effect}")
        await super().story_effect(npc, effects, check=check)


@pytest.fixture
async def system():
    link, memory = FarLink(), MemoryStore(":memory:")
    chronicle = Chronicle(link, memory)
    crimes = Crimes(link, chronicle, names=lambda npc: NAMES.get(npc, npc))
    cards = {npc: character(npc, courage=2) for npc in (LOBART, HILDA, THIEF)}
    deeds = NpcCrimes(link, chronicle, crimes=crimes, cards=cards)
    moments, people = SimpleNamespace(now=100.0), PeopleKnowledge(chronicle)
    will = NpcWill(
        link, chronicle, deeds, cards=cards, people=people, memory=memory,
        clock=lambda: moments.now,
    )
    yield SimpleNamespace(
        link=link, memory=memory, chronicle=chronicle, crimes=crimes, deeds=deeds,
        cards=cards, will=will, people=people, moments=moments,
    )
    await will.close()
    await deeds.close()
    await crimes.close()
    if chronicle._background:
        await asyncio.gather(*chronicle._background)
    memory.close()


def routine(pos: tuple[int, int, int], *, waypoint: str = "FARM1_IN", state: str = "ZS_STAND",
            start: int = 0, end: int = 0) -> dict[str, Any]:
    return {"start": start, "end": end, "state": state, "waypoint": waypoint, "pos": list(pos)}


def far_scene(system, *, now: int = NOW, witness: bool = False, owner_x: int = 20000,
              owner_state: str = "ZS_STAND", greedy: bool = True) -> None:
    if greedy:
        system.cards[THIEF] = character(THIEF, honesty=1, greed=5)
    positions = {THIEF: (10000, 0, 0), LOBART: (owner_x, 0, 0)}
    if witness:
        positions[HILDA] = (11800, 0, 0)
    system.link.people = [
        {"npc": npc, "name": NAMES[npc], "guild": 12, "routine": [routine(
            pos, waypoint="FARM1_IN" if npc == THIEF else f"FARM1_{npc}",
            state=owner_state if npc == LOBART else "ZS_STAND",
        )]}
        for npc, pos in positions.items()
    ]
    system.will.rumors = SimpleNamespace(social=Social(system.link.people), clock=now)
    system.link.snapshots.clear()
    system.link.engine = {
        npc: {"state": "alive", "pos": list(pos)} for npc, pos in positions.items()
    }
    system.link.engine["PC_HERO"] = {"state": "alive", "pos": [0, 0, 0]}
    system.link.chests[LOBART] = [Chest(
        id=CHEST, why="owner", pos=(10100, 0, 0), distance=abs(owner_x - 10100),
        waypoint="FARM1_IN", items=[{"item": ITEM, "name": "Хлеб", "count": 7}],
    )]
    views = [NpcView(
        npc=npc, name=NAMES[npc], time=now, hp=100, hp_max=100, state="ZS_STAND",
        theft_range=1000 if npc == THIEF else 2500, theft_ready=False,
        hero={"id": "PLAYER", "name": "Герой", "distance": pos[0], "seen": False},
        sees=[],
    ) for npc, pos in positions.items()]
    system.link.views = {view.npc: view for view in views}
    system.link.receive(NpcViews(type="npc_views", t=now, views=views))


def update_social(system) -> None:
    system.will.rumors.social = Social(system.link.people)


def near_scene(system, *, now: int) -> None:
    greed_scene(system, now=now)
    system.link.engine = {
        npc: {"state": "alive", "pos": list(snapshot.npc.pos)}
        for npc, snapshot in system.link.snapshots.items()
    }
    system.link.engine["PC_HERO"] = {"state": "alive", "pos": [0, 0, 0]}


async def test_far_greed_transfers_before_truth_and_uses_separate_witness_rule(system):
    far_scene(system, witness=True)
    await system.will.tick(NOW)
    assert system.link.effects == [(THIEF, [{
        "do": "chest_take", "chest": CHEST, "item": ITEM, "count": 1,
    }], True)]
    assert system.link.inventory[THIEF][ITEM] == 1
    assert system.link.chests[LOBART][0].items[0].count == 6
    [crime] = system.deeds.facts()
    assert (crime["culprit"], crime["owner"], crime["chest"], crime["far"]) == (THIEF, LOBART, CHEST, True)
    assert crime["witnesses"] == [HILDA]
    assert system.deeds.seen(HILDA) == [crime]
    assert not system.deeds.seen(LOBART)
    [decision] = system.chronicle.facts("npc_will")
    assert (decision["kind"], decision["act"], decision["far"]) == ("greed", "steal", True)
    [attempt] = system.will.attempts()
    assert attempt["outcome"] == "committed" and attempt["crime"] == crime["id"]
    assert system.chronicle.facts("npc_will_outcome")[0]["far"] is True
    assert not system.link.actions
    assert system.crimes.facts() == [] and system.crimes.known(HILDA) == []


async def test_far_owner_can_be_a_witness_from_outside_owner_near_gate(system):
    far_scene(system, owner_x=11601)
    await system.will.tick(NOW)
    [crime] = system.deeds.facts()
    assert crime["witnesses"] == [LOBART]
    assert system.deeds.seen(LOBART) == [crime]


async def test_sleeping_far_owner_does_not_prevent_theft_or_become_a_witness(system):
    far_scene(system, owner_x=11601, owner_state="ZS_SLEEP")
    await system.will.tick(NOW)
    [crime] = system.deeds.facts()
    assert crime["witnesses"] == []
    assert system.link.inventory[THIEF][ITEM] == 1


async def test_far_greed_uses_the_current_routine_instead_of_any_daily_spot(system):
    far_scene(system)
    system.link.people[0]["routine"] = [
        routine((10000, 0, 0), start=8 * 60, end=9 * 60),
        routine((30000, 0, 0), waypoint="FARM1_FIELD", start=9 * 60, end=8 * 60),
    ]
    update_social(system)
    await system.will.tick(NOW)
    assert not system.link.effects and not system.chronicle.facts("npc_motive")


@pytest.mark.parametrize("actor_x,owner_x", [(14001, 20000), (10000, 11600)])
async def test_far_greed_rechecks_real_actor_and_owner_positions(system, actor_x, owner_x):
    far_scene(system)
    system.link.engine[THIEF]["pos"] = [actor_x, 0, 0]
    system.link.engine[LOBART]["pos"] = [owner_x, 0, 0]
    # This cached chest distance still claims the owner is away.
    assert system.link.chests[LOBART][0].distance > 1500
    await system.will.tick(NOW)
    assert not system.link.effects and not system.deeds.facts()


async def test_far_greed_uses_current_routine_when_engine_has_no_position(system):
    far_scene(system)
    for npc in (THIEF, LOBART):
        system.link.engine[npc].pop("pos")
    await system.will.tick(NOW)
    assert system.link.inventory[THIEF][ITEM] == 1
    assert system.deeds.facts()[0]["far"] is True
    assert (THIEF, True) in system.link.snapshot_queries


@pytest.mark.parametrize("observer", [HILDA, "PLAYER", "SHEEP"])
async def test_far_greed_waits_when_the_thief_currently_perceives_someone(system, observer):
    far_scene(system)
    view = system.link.views[THIEF]
    if observer == "PLAYER":
        view = view.model_copy(update={"hero": view.hero.model_copy(update={"seen": True})})
    else:
        view = NpcView.model_validate({**view.model_dump(), "sees": [{
            "id": observer, "name": observer, "distance": 300,
            "kind": "monster" if observer == "SHEEP" else "human",
        }]})
    system.link.views[THIEF] = view
    await system.will.tick(NOW)
    assert not system.link.effects and not system.deeds.facts()
    assert len(system.will.pending()) == 1


async def test_far_greed_checks_new_candidates_only_once_per_game_hour(system):
    far_scene(system)
    system.link.chests.clear()
    await system.will.tick(NOW)
    far_scene(system, now=NOW + 1)
    await system.will.tick(NOW + 1)
    assert not system.link.effects
    system.will.rumors.clock = NOW + 60
    await system.will.tick(NOW + 60)
    assert system.link.inventory[THIEF][ITEM] == 1


@pytest.mark.parametrize("first", ["near", "far"])
async def test_greed_cooldown_is_shared_between_nearby_actions_and_distant_outcomes(system, first):
    if first == "near":
        near_scene(system, now=NOW)
        await system.will.tick(NOW)
        commit_theft(system)
        finish_action(system)
        far_scene(system, now=NOW + DAY)
    else:
        far_scene(system)
        await system.will.tick(NOW)
        near_scene(system, now=NOW + DAY)
    await system.will.tick(NOW + DAY)
    assert len(system.chronicle.facts("npc_will")) == 1
    assert not system.will._cool(THIEF, system.cards[THIEF])
    system.moments.now += 60
    system.will.rumors.clock = NOW + 2 * DAY
    await system.will.tick(NOW + 2 * DAY)
    assert len(system.chronicle.facts("npc_will")) == 2
    assert len(system.link.actions) == len(system.link.effects) == 1


@pytest.mark.parametrize("enabled,hero_crimes", [(False, True), (True, False)])
async def test_both_crime_switches_disable_distant_motives_and_effects(system, enabled, hero_crimes):
    far_scene(system)
    system.deeds.enabled = enabled and hero_crimes
    await system.will.tick(NOW)
    assert not system.link.effects and not system.deeds.facts()
    assert not system.chronicle.facts("npc_motive") and not system.chronicle.facts("npc_will")


@pytest.mark.parametrize("kind,knowledge", [("suspect", "npc_suspected"), ("reclaim", "npc_accusation")])
async def test_far_meeting_accuses_once_without_svm_and_keeps_belief_separate(system, kind, knowledge):
    far_scene(system, greedy=False, owner_x=11000)
    case = theft(system, witnesses=[LOBART] if kind == "reclaim" else [])
    if kind == "suspect":
        system.chronicle.record({
            "fact": "npc_crime_suspected", "npc": LOBART, "crime": case["id"], "suspect": THIEF,
        })
    await system.will.tick(NOW)
    await system.will.tick(NOW + 60)
    [accusation] = system.chronicle.facts("npc_accused")
    assert (accusation["npc"], accusation["target"], accusation["kind"], accusation["far"]) == (
        LOBART, THIEF, kind, True,
    )
    assert accusation["witnesses"] == [LOBART]
    [decision] = system.chronicle.facts("npc_will")
    assert (decision["act"], decision["far"]) == ("accuse", True)
    assert (knowledge, THIEF) in {(k.kind, k.subject) for k in system.people.of(LOBART)}
    assert system.memory.load("A", LOBART).entries
    assert not system.link.actions and not system.link.effects
    assert system.crimes.facts() == []
    assert bool(system.deeds.seen(LOBART)) is (kind == "reclaim")


async def test_distant_accusation_waits_for_a_current_routine_meeting(system):
    far_scene(system, greedy=False)
    theft(system, witnesses=[LOBART])
    await system.will.tick(NOW)
    assert len(system.will.pending()) == 1 and not system.chronicle.facts("npc_accused")
    system.link.people[1]["routine"] = [routine((11000, 0, 0))]
    update_social(system)
    await system.will.tick(NOW + 60)
    assert len(system.chronicle.facts("npc_accused")) == 1


async def test_settled_case_stops_already_pending_reclaim_and_suspicion(system):
    far_scene(system, greedy=False)
    case = theft(system, witnesses=[LOBART])
    system.chronicle.record({
        "fact": "npc_crime_suspected", "npc": LOBART, "crime": case["id"], "suspect": THIEF,
    })
    await system.will.tick(NOW)
    assert {m["kind"] for m in system.will.pending()} == {"reclaim", "suspect"}
    system.chronicle.record({
        "fact": "npc_crime_settled", "crime": case["id"], "by": "MIL_300_GUARD", "how": "returned",
    })
    system.link.people[1]["routine"] = [routine((11000, 0, 0))]
    update_social(system)
    await system.will.tick(NOW + 60)
    assert system.will.pending() == []
    assert not system.chronicle.facts("npc_will") and not system.chronicle.facts("npc_accused")
    assert not system.link.actions and not system.link.effects


async def test_loading_completed_far_theft_restores_cooldown_without_repeating_transfer(system):
    far_scene(system)
    before = system.memory.head("A")
    await system.will.tick(NOW)
    saved = system.memory.head("A")
    system.link.receive(Session(type="session", branch="loaded", parent="A", parent_seq=saved, reason="load"))
    far_scene(system, now=NOW + DAY)
    await system.will.tick(NOW + DAY)
    assert len(system.link.effects) == len(system.deeds.facts()) == 1
    assert len(system.chronicle.facts("npc_motive")) == len(system.chronicle.facts("npc_will")) == 1
    system.link.receive(Session(type="session", branch="old", parent="A", parent_seq=before, reason="load"))
    far_scene(system, now=NOW + DAY)
    await system.will.tick(NOW + DAY)
    assert len(system.link.effects) == 2
    assert len(system.deeds.facts()) == len(system.chronicle.facts("npc_will")) == 1


async def test_loading_waiting_far_motive_resumes_once_instead_of_discovering_a_duplicate(system):
    far_scene(system)
    view = system.link.views[THIEF]
    system.link.views[THIEF] = view.model_copy(update={"hero": view.hero.model_copy(update={"seen": True})})
    await system.will.tick(NOW)
    assert len(system.will.pending()) == 1 and not system.link.effects
    saved = system.memory.head("A")
    system.link.receive(Session(type="session", branch="loaded", parent="A", parent_seq=saved, reason="load"))
    far_scene(system, now=NOW + 60)
    await system.will.tick(NOW + 60)
    await system.will.tick(NOW + 120)
    assert len(system.link.effects) == len(system.deeds.facts()) == 1
    assert len(system.chronicle.facts("npc_motive")) == len(system.chronicle.facts("npc_will")) == 1
