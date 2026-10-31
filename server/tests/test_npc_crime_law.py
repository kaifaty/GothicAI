from __future__ import annotations

import asyncio
import json
from types import SimpleNamespace
from typing import Any

import pytest

from gothic_ai.characters import Card
from gothic_ai.chronicle import Chronicle
from gothic_ai.crime_law import REPORT_DELAYS, CrimeLaw
from gothic_ai.crimes import Crimes
from gothic_ai.game import GameError
from gothic_ai.memory import MemoryStore
from gothic_ai.npc_crime_law import NpcCrimeLaw
from gothic_ai.npc_crimes import NpcCrimes
from gothic_ai.protocol import Loading, NpcIdle, RosterEntry, Session
from tests.test_crimes import HILDA, LOBART
from tests.test_npc_crimes import GUARD, THIEF, NpcLink, deed

NOW = 3 * 1440 + 9 * 60 + 15
ITEM = "ITFO_BREAD"
FARM_GUARD = "SLD_801_GUARD"


def card(npc: str, courage: int) -> Card:
    return Card(npc, npc, "", "", 3, 3, courage, 3, 3, 3)


class LawLink(NpcLink):
    def __init__(self) -> None:
        super().__init__()
        self.inventory = {THIEF: 1, LOBART: 0}

    async def story_eval(self, npc: str, exprs: list[Any]) -> list[Any]:
        if len(exprs) == 1 and exprs[0][:2] == ["call", "NPC_HASITEMS"]:
            self.evaluations.append((npc, exprs))
            return [self.inventory.get(npc, 0)]
        return await super().story_eval(npc, exprs)

    async def story_effect(self, npc: str, effects: list[dict[str, Any]], *, check=False) -> None:
        for effect in effects:
            if effect.get("do") != "give":
                continue
            if check and self.inventory.get(npc, 0) < effect["count"]:
                raise GameError("not enough items")
            self.inventory[npc] = self.inventory.get(npc, 0) - effect["count"]
            target = effect["npc"]
            self.inventory[target] = self.inventory.get(target, 0) + effect["count"]
        await super().story_effect(npc, effects, check=check)


class Will:
    """The law owns guard policy; NpcWill owns the shared encounter and battle rule."""

    def __init__(self, chronicle: Chronicle) -> None:
        self._lock, self._generation, self._now = asyncio.Lock(), 0, NOW
        self.cards = {THIEF: card(THIEF, 3)}
        self.mode: str | None = "far"
        self.meetings: list[tuple[str, str]] = []
        self.fights: list[tuple[str, str]] = []
        self.fight_roles: list[str] = []
        self.outcome = "defender_down"
        self.chronicle = chronicle
        self.guard_names = {GUARD, FARM_GUARD}
        self.refreshes: list[int] = []
        self.far_world = SimpleNamespace(
            social=SimpleNamespace(guilds={}), refresh=self.refresh,
            all_names=lambda: list(self.far_world.social.guilds),
        )

    async def refresh(self, generation: int) -> None:
        self.refreshes.append(generation)

    def _refresh_clock(self) -> None:
        pass

    async def meeting(self, actor: str, target: str, generation: int) -> str | None:
        self.meetings.append((actor, target))
        return self.mode if actor in self.guard_names else None

    async def fight_far(
        self, crime: dict[str, Any], fighter: str, generation: int, *, role: str = "reclaim",
    ) -> dict[str, Any]:
        self.fights.append((crime["id"], fighter))
        self.fight_roles.append(role)
        self.chronicle.record({
            "fact": "npc_crime_reaction", "crime": crime["id"], "npc": fighter,
            "reaction": "fight", "role": role, "act": "far:1", "far": True,
        })
        fact = {
            "fact": "npc_crime_defense", "crime": crime["id"], "culprit": crime["culprit"],
            "fighter": fighter, "outcome": self.outcome, "far": True, "act": "far:1",
        }
        self.chronicle.record(fact)
        return fact


@pytest.fixture
async def system():
    link, memory = LawLink(), MemoryStore(":memory:")
    chronicle = Chronicle(link, memory)
    hero = Crimes(link, chronicle)
    hero.people = {
        LOBART: {"guild": 3, "npctype": 0}, HILDA: {"guild": 3, "npctype": 0},
        THIEF: {"guild": 12, "npctype": 0}, GUARD: {"guild": 2, "npctype": 0},
        FARM_GUARD: {"guild": 7, "npctype": 0},
    }
    deeds = NpcCrimes(link, chronicle, crimes=hero)
    will = Will(chronicle)
    law = NpcCrimeLaw(link, chronicle, deeds, will=will)
    for npc, guild in ((GUARD, 2), (FARM_GUARD, 7), (THIEF, 12), (LOBART, 3)):
        deeds.roster[npc] = RosterEntry(
            id=npc, name=npc, guild=guild, distance=500, state="ZS_STAND", activity="routine",
        )
    yield SimpleNamespace(link=link, memory=memory, chronicle=chronicle, hero=hero,
                          deeds=deeds, will=will, law=law)
    await law.close()
    await deeds.close()
    await hero.close()
    if chronicle._background:
        await asyncio.gather(*chronicle._background)
    memory.close()


def crime(system, *, kind: str = "theft", witness: str = HILDA, **fields: Any):
    if kind == "attack":
        return system.deeds.record(deed(
            kind="attack", victim=LOBART, victim_guild=3, victim_npctype=0,
            witnesses=[witness] if witness else [], **fields,
        ))
    return system.deeds.record(deed(
        owner=LOBART, owner_npc_guild=3, owner_npctype=0,
        witnesses=[witness] if witness else [], **fields,
    ))


@pytest.mark.parametrize("kind", ["theft", "attack"])
async def test_same_community_witness_reports_at_hero_delay(system, kind):
    event = crime(system, kind=kind)
    due = NOW + REPORT_DELAYS[kind]
    assert system.law.deliver(due - 1) == []
    [known] = system.law.deliver(due)
    assert known == {
        "fact": "npc_crime_known", "crime": event["id"], "community": "city", "via": HILDA, "at": due,
    }
    assert system.law.deliver(due + 1000) == []
    assert system.law.known("city") == [event]


async def test_guard_witness_knows_immediately_without_a_clock_packet(system):
    event = crime(system, witness=GUARD)
    assert system.chronicle.facts("npc_crime_known") == [{
        "fact": "npc_crime_known", "crime": event["id"], "community": "city", "via": GUARD, "at": NOW,
    }]
    assert system.law.deliver(NOW + REPORT_DELAYS["theft"]) == []


@pytest.mark.parametrize("witness,guild,npctype", [(HILDA, 8, 0), (HILDA, 3, 3), (FARM_GUARD, 7, 0)])
async def test_outside_witness_or_guard_does_not_report_to_victims_community(system, witness, guild, npctype):
    system.hero.people[witness] = {"guild": guild, "npctype": npctype}
    crime(system, witness=witness)
    assert system.law.deliver(NOW + 5000) == []
    assert system.law.known("city") == []


async def test_dead_witness_cannot_complete_delayed_report(system):
    crime(system)
    system.chronicle.record({"fact": "died", "npc": HILDA, "at": NOW + 10})
    assert system.law.deliver(NOW + 5000) == []


@pytest.mark.parametrize("mode", ["near", "far"])
async def test_courage_three_surrenders_real_goods_once(system, mode):
    event = crime(system)
    system.will.mode = mode
    await system.law.tick(NOW + REPORT_DELAYS["theft"])
    assert system.link.inventory == {THIEF: 0, LOBART: 1}
    assert system.link.effects == [(THIEF, [{
        "do": "give", "item": ITEM, "count": 1, "to": "npc", "npc": LOBART,
    }], True)]
    [settled] = system.chronicle.facts("npc_crime_settled")
    assert settled == {
        "fact": "npc_crime_settled", "crime": event["id"], "by": GUARD, "how": "surrendered",
        **({"far": True} if mode == "far" else {}),
    }
    assert system.will.fights == []
    assert system.link.actions == ([(GUARD, "say_svm", {"svm": "DIRTYTHIEF", "overlay": True})]
                                   if mode == "near" else [])
    await system.law.tick(NOW + 5000)
    assert len(system.link.effects) == 1
    assert len(system.chronicle.facts("npc_crime_settled")) == 1


async def test_courage_four_fights_far_and_records_terminal_outcome_once(system):
    event = crime(system)
    system.will.cards[THIEF] = card(THIEF, 4)
    await system.law.tick(NOW + REPORT_DELAYS["theft"])
    assert system.link.effects == []
    assert system.will.fights == [(event["id"], GUARD)]
    assert system.will.fight_roles == ["law"]
    [settled] = system.chronicle.facts("npc_crime_settled")
    assert settled["how"] == "defender_down" and settled["far"]
    await system.law.tick(NOW + 5000)
    assert len(system.will.fights) == 1


async def test_courage_four_near_uses_existing_nonlethal_fight_and_return(system):
    event = crime(system)
    system.will.cards[THIEF] = card(THIEF, 4)
    system.will.mode = "near"
    system.link.values = [1, 0, 0, 1, 1, 0, 0]
    await system.law.tick(NOW + REPORT_DELAYS["theft"])
    await asyncio.gather(*tuple(system.deeds._tasks))
    assert system.link.actions == [(GUARD, "fight", {
        "npc": THIEF, "reason": "AR_NONE", "deed": False, "nonlethal": True, "svm": "DIRTYTHIEF",
    })]
    assert system.link.inventory == {THIEF: 0, LOBART: 1}
    [settled] = system.chronicle.facts("npc_crime_settled")
    assert (settled["crime"], settled["by"], settled["how"]) == (event["id"], GUARD, "reclaimed")
    await system.law.tick(NOW + 5000)
    assert len(system.link.actions) == 1


@pytest.mark.parametrize("mode", ["near", "far"])
async def test_attack_receives_one_nonlethal_fight_even_for_courage_one(system, mode):
    event = crime(system, kind="attack")
    system.will.mode = mode
    system.will.cards[THIEF] = card(THIEF, 1)
    await system.law.tick(NOW + REPORT_DELAYS["attack"])
    if mode == "near":
        assert system.link.actions[0][1:] == ("fight", {
            "npc": THIEF, "reason": "AR_NONE", "deed": False, "nonlethal": True, "svm": "HELP",
        })
        assert not system.deeds.settled(event["id"])
        system.link.receive(NpcIdle(
            type="npc_idle", npc=GUARD, act="a1", reason="done", detail="target_down",
        ))
    assert system.deeds.settled(event["id"])
    assert system.link.effects == []
    await system.law.tick(NOW + 5000)
    assert len(system.link.actions) + len(system.will.fights) == 1


async def test_no_item_or_no_meeting_does_not_invent_settlement(system):
    crime(system)
    system.link.inventory[THIEF] = 0
    await system.law.tick(NOW + 5000)
    system.link.inventory[THIEF] = 1
    system.will.mode = None
    await system.law.tick(NOW + 6000)
    assert not system.link.effects and not system.will.fights
    assert system.chronicle.facts("npc_crime_settled") == []


@pytest.mark.parametrize("protection", ["plot", "post", "mission", "owner_post"])
async def test_storyguard_protects_plot_posts_and_mission_items(system, protection):
    event = crime(system)
    if protection == "plot":
        system.deeds.guard.plot = lambda npc: npc == THIEF
    elif protection == "post":
        system.deeds.roster[GUARD] = system.deeds.roster[GUARD].model_copy(
            update={"state": "ZS_GUARD_PASSAGE"},
        )
    elif protection == "owner_post":
        system.deeds.roster[LOBART] = system.deeds.roster[LOBART].model_copy(
            update={"state": "ZS_GUARD_PASSAGE"},
        )
    else:
        # Mission status is part of the rule's protected truth, never a new hero fact.
        system.chronicle.record({**event, "id": "protected", "mission": True})
        system.chronicle.record({"fact": "npc_crime_seen", "crime": "protected", "npc": HILDA})
        system.chronicle.record({
            "fact": "npc_crime_settled", "crime": event["id"], "by": "OTHER", "how": "done",
        })
    await system.law.tick(NOW + 5000)
    assert system.link.effects == [] and system.link.actions == [] and system.will.fights == []


async def test_disabled_law_writes_no_new_facts_or_actions(system):
    system.deeds.enabled = False
    system.chronicle.record({
        "fact": "npc_crime", "id": "saved", "kind": "attack", "culprit": THIEF, "victim": LOBART,
        "community": "city", "day": 3, "hour": 9, "minute": 15,
    })
    system.chronicle.record({"fact": "npc_crime_seen", "crime": "saved", "npc": GUARD})
    await system.law.tick(NOW + 5000)
    assert system.chronicle.facts("npc_crime_known") == []
    assert system.chronicle.facts("npc_crime_settled") == []
    assert system.link.actions == [] and system.will.fights == []


async def test_known_and_settled_follow_saved_branch_without_replay(system):
    event = crime(system)
    await system.law.tick(NOW + 5000)
    assert system.deeds.settled(event["id"])
    head = system.memory.head("A")
    system.link.receive(Session(type="session", branch="saved", parent="A", parent_seq=head, reason="load"))
    assert system.law.known("city")[0]["id"] == event["id"]
    assert system.deeds.settled(event["id"])
    await system.law.tick(NOW + 6000)
    assert len(system.link.effects) == 1
    system.link.receive(Session(type="session", branch="before", parent="A", parent_seq=0, reason="load"))
    assert system.law.known("city") == []
    assert not system.deeds.settled(event["id"])


async def test_loaded_fight_is_not_replayed_or_settled_by_reused_action_id(system):
    event = crime(system, kind="attack")
    system.will.mode = "near"
    await system.law.tick(NOW + 5000)
    head = system.memory.head("A")
    system.link.receive(Loading(type="loading"))
    system.link.receive(Session(type="session", branch="saved", parent="A", parent_seq=head, reason="load"))
    system.link.receive(NpcIdle(type="npc_idle", npc=GUARD, act="a1", reason="done", detail="target_down"))
    assert not system.deeds.settled(event["id"])
    system.will.mode = None
    await system.law.tick(NOW + 6000)
    assert not system.deeds.settled(event["id"])
    system.will.mode = "far"
    await system.law.tick(NOW + 7000)
    [settled] = system.chronicle.facts("npc_crime_settled")
    assert (settled["crime"], settled["how"], settled["far"]) == (event["id"], "interrupted", True)
    assert len(system.link.actions) == 1 and system.will.fights == []


async def test_existing_layer_four_guard_fight_is_observed_without_second_fight(system):
    event = crime(system)
    system.chronicle.record({
        "fact": "npc_crime_reaction", "crime": event["id"], "npc": GUARD,
        "reaction": "fight", "role": "guard", "act": "scene-fight",
    })
    await system.law.tick(NOW + 5000)
    assert system.link.actions == [] and system.will.fights == []
    system.chronicle.record({
        "fact": "npc_crime_defense", "crime": event["id"], "fighter": GUARD,
        "act": "scene-fight", "outcome": "defender_down",
    })
    assert system.deeds.settled(event["id"])


async def test_settlement_stops_body_reaction_fight_and_pending_restitution(system):
    event = crime(system, witness=GUARD)
    system.chronicle.record({"fact": "npc_crime_settled", "crime": event["id"], "by": GUARD, "how": "done"})
    await system.deeds._react(event, system.deeds._generation)
    with pytest.raises(GameError, match="уже завершено"):
        await system.deeds._fight(event, GUARD, system.deeds._generation, role="guard")
    await system.deeds._reclaim(event, GUARD, system.deeds._generation, "old-fight")
    assert system.link.actions == [] and system.link.effects == []


async def test_npc_news_and_settlement_never_enter_hero_readers(system):
    event = crime(system)
    await system.law.tick(NOW + 5000)
    hero_law = CrimeLaw(system.hero)
    hero_law.tick(NOW + 6000)
    assert system.hero.facts() == [] and system.hero.known(GUARD) == []
    assert hero_law.known("city") == [] and hero_law.debt("city") == 0
    assert hero_law.deliveries() == []
    assert system.chronicle.facts("crime_known") == []
    assert system.deeds.settled(event["id"])


async def test_far_guard_is_discovered_from_social_guilds_without_local_roster(system):
    event = crime(system)
    remote_guard = "MIL_099_DISTANT_GUARD"
    system.will.far_world.social.guilds[remote_guard] = 2
    system.will.guard_names = {remote_guard}
    await system.law.tick(NOW + REPORT_DELAYS["theft"])
    [settled] = system.chronicle.facts("npc_crime_settled")
    assert (settled["crime"], settled["by"], settled["far"]) == (event["id"], remote_guard, True)
    assert system.will.refreshes == [system.will._generation]
    assert system.will._now == NOW + REPORT_DELAYS["theft"]


@pytest.mark.parametrize("mode", ["near", "far"])
async def test_returned_goods_do_not_confiscate_spare_items_of_the_same_type(system, mode):
    event = crime(system)
    system.will.mode = mode
    system.link.inventory = {THIEF: 4, LOBART: 1}
    system.chronicle.record({
        "fact": "npc_crime_restitution", "crime": event["id"], "npc": LOBART, "culprit": THIEF,
        "fighter": LOBART, "item": ITEM, "count": 1, "how": "defeated_thief",
    })
    await system.law.tick(NOW + REPORT_DELAYS["theft"])
    assert system.link.inventory == {THIEF: 4, LOBART: 1}
    assert system.link.effects == [] and system.link.actions == [] and system.will.fights == []
    [settled] = system.chronicle.facts("npc_crime_settled")
    assert (settled["crime"], settled["how"]) == (event["id"], "already_returned")


@pytest.mark.parametrize("mode", ["near", "far"])
async def test_body_defense_reservation_prevents_law_taking_another_copy(system, mode):
    event = crime(system)
    system.will.mode = mode
    system.link.inventory = {THIEF: 4, LOBART: 0}
    system.deeds._restitution_started.add((system.deeds._generation, event["id"]))
    await system.law.tick(NOW + REPORT_DELAYS["theft"])
    assert system.link.inventory == {THIEF: 4, LOBART: 0}
    assert system.link.effects == [] and system.link.actions == [] and system.will.fights == []
    assert not system.deeds.settled(event["id"])


async def test_surrender_claims_the_same_reservation_before_sending_transfer(system):
    event = crime(system)
    original_effect = system.link.story_effect

    async def effect(npc, effects, *, check=False):
        assert (system.deeds._generation, event["id"]) in system.deeds._restitution_started
        await original_effect(npc, effects, check=check)

    system.link.story_effect = effect
    await system.law.tick(NOW + REPORT_DELAYS["theft"])
    assert system.link.inventory == {THIEF: 0, LOBART: 1}
    assert system.deeds.settled(event["id"])


@pytest.mark.parametrize("far", [False, True])
async def test_saved_defense_without_law_settlement_is_projected_after_load(system, far):
    event = crime(system, kind="attack")
    system.chronicle.record({
        "fact": "npc_crime_reaction", "crime": event["id"], "npc": GUARD,
        "reaction": "fight", "role": "guard", "act": "saved-fight",
    })
    # An older process can save its completed body defense before this law subsystem is enabled.
    system.memory.add_fact("A", json.dumps({
        "fact": "npc_crime_defense", "crime": event["id"], "fighter": GUARD,
        "act": "saved-fight", "outcome": "defender_down", **({"far": True} if far else {}),
    }))
    head = system.memory.head("A")
    system.link.receive(Session(type="session", branch="saved", parent="A", parent_seq=head, reason="load"))
    await system.law.tick(NOW + REPORT_DELAYS["attack"])
    [settled] = system.chronicle.facts("npc_crime_settled")
    assert settled == {
        "fact": "npc_crime_settled", "crime": event["id"], "by": GUARD, "how": "defender_down",
        **({"far": True} if far else {}),
    }
    assert system.link.actions == [] and system.will.fights == []
