from __future__ import annotations

import asyncio
from collections import defaultdict
from types import SimpleNamespace
from typing import Any

import pytest

from gothic_ai.chronicle import Chronicle
from gothic_ai.crime_law import CrimeLaw
from gothic_ai.crimes import Crimes
from gothic_ai.game import GameError
from gothic_ai.memory import MemoryStore
from gothic_ai.npc_crimes import NpcCrimes
from gothic_ai.people_knowledge import PeopleKnowledge
from gothic_ai.protocol import Loading, NpcActResult, NpcDeed, NpcIdle, NpcRoster, Session
from gothic_ai.sightings import Sighting
from gothic_ai.social import Rumors, Social
from tests.test_crime_react import card
from tests.test_crimes import HILDA, LOBART, Link

THIEF, VICTIM = "BAU_952_VINO", "BAU_953_MALETH"
GUARD, OTHER_GUARD = "MIL_300_GUARD", "MIL_301_GUARD"
PLOT = "NONE_100_XARDAS"
NAMES = {LOBART: "Лобарт", HILDA: "Хильда", THIEF: "Вино", VICTIM: "Малет"}


@pytest.mark.parametrize("suspect", [THIEF, HILDA])
async def test_missing_chest_suspects_remembered_person_even_when_innocent(system, suspect):
    crime = system.deeds.record(deed(chest="CHEST", chest_owner=LOBART))
    at = 3 * 1440 + 9 * 60 + 15
    sight = Sighting(suspect, NAMES[suspect], at - 2, at + 2, place_ids=["FARM"])
    system.hero.sightings = SimpleNamespace(mind=lambda npc: SimpleNamespace(sightings=[sight]))
    system.hero.places = SimpleNamespace(of=lambda wp: SimpleNamespace(id="FARM"))
    suspected = system.deeds.suspect_chest(LOBART, "CHEST", {"ITFO_BREAD": 1})
    assert [(f["crime"], f["suspect"]) for f in suspected] == [(crime["id"], suspect)]
    assert system.deeds.suspect_chest(LOBART, "CHEST", {"ITFO_BREAD": 1}) == []
    assert system.chronicle.facts("crime_suspected") == []
    assert system.hero.known(LOBART) == []
    assert system.deeds.seen(LOBART) == []


@pytest.mark.parametrize("change", ["hero", "old", "elsewhere", "other_item", "disabled"])
async def test_chest_suspicion_needs_spatial_temporal_memory_and_skips_hero(system, change):
    system.deeds.record(deed(chest="CHEST", chest_owner=LOBART))
    at = 3 * 1440 + 9 * 60 + 15
    sight = Sighting("PLAYER" if change == "hero" else THIEF, "Кто-то", at, at,
                     place_ids=["CITY" if change == "elsewhere" else "FARM"])
    if change == "old":
        sight.start = sight.end = at - 120
    system.hero.sightings = SimpleNamespace(mind=lambda npc: SimpleNamespace(sightings=[sight]))
    system.hero.places = SimpleNamespace(of=lambda wp: SimpleNamespace(id="FARM"))
    system.deeds.enabled = change != "disabled"
    item = "OTHER" if change == "other_item" else "ITFO_BREAD"
    assert system.deeds.suspect_chest(LOBART, "CHEST", {item: 1}) == []
    assert system.chronicle.facts("npc_crime_suspected") == []


class NpcLink(Link):
    def __init__(self) -> None:
        super().__init__()
        self.actions: list[tuple[str, str, dict[str, Any]]] = []
        self.effects: list[tuple[str, list[dict[str, Any]], bool]] = []
        self.evaluations: list[tuple[str, list[Any]]] = []
        self.nearby: list[dict[str, Any]] = []
        self.values = [1, 0, 0, 2, 1, 0, 0]
        self.guilds: dict[str, Any] = {}
        self.accept = True
        self.owned: set[str] = set()
        self.target: dict[str, Any] = {"owner": LOBART, "item": "ITFO_BREAD", "mission": False}

    async def npc_act(self, npc: str, action: str, /, **args: Any) -> str:
        self.actions.append((npc, action, args))
        action_id = f"a{len(self.actions)}"
        self.receive(NpcActResult(
            type="npc_act_result", id=action_id, npc=npc,
            status="queued" if self.accept else "rejected", detail=None if self.accept else "busy",
        ))
        return action_id

    def body_owned(self, npc: str) -> bool:
        return npc in self.owned

    async def story_nearby(self, npc: str, radius: int) -> list[dict[str, Any]]:
        return list(self.nearby)

    async def story_eval(self, npc: str, exprs: list[Any]) -> list[Any]:
        self.evaluations.append((npc, exprs))
        if len(exprs) == 1 and exprs[0][0] == "field":
            return [self.guilds.get(npc, 12)]
        if len(exprs) == 2:
            return [7 if npc == "SLD_FARM" else 2, 0]
        return list(self.values)

    async def story_effect(self, npc: str, effects: list[dict[str, Any]], *, check=False) -> None:
        self.effects.append((npc, effects, check))

    async def story_state(self, **args: Any) -> dict[str, Any]:
        return {"deed_target": self.target}


@pytest.fixture
async def system():
    link, memory = NpcLink(), MemoryStore(":memory:")
    chronicle = Chronicle(link, memory)
    hero = Crimes(link, chronicle, names=lambda npc: NAMES.get(npc, npc))
    deeds = NpcCrimes(link, chronicle, crimes=hero)
    yield SimpleNamespace(link=link, memory=memory, chronicle=chronicle, hero=hero, deeds=deeds)
    await deeds.close()
    await hero.close()
    if chronicle._background:
        await asyncio.gather(*chronicle._background)
    memory.close()


def deed(**fields: Any) -> NpcDeed:
    return NpcDeed.model_validate({
        "type": "npc_deed", "id": "npc-1", "culprit": THIEF, "culprit_guild": 12,
        "kind": "take_item", "item": "ITFO_BREAD", "count": 1,
        "position": {"x": 12, "y": 23, "z": 34}, "waypoint": "FARM1_IN",
        "day": 3, "hour": 9, "minute": 15, **fields,
    })


@pytest.mark.parametrize("ownership", [
    {}, {"owner": THIEF}, {"room_owner": THIEF},
    {"chest": "CHEST", "chest_owner": THIEF},
    {"owner_guild": 12}, {"room_guild": 15}, {"item_dropped": True, "room_owner": LOBART},
    {"owner": THIEF, "chest": "CHEST", "chest_owner": LOBART},
])
async def test_own_public_unowned_and_dropped_items_leave_no_fact(system, ownership):
    assert system.deeds.record(deed(**ownership)) is None
    assert system.chronicle.facts() == []


@pytest.mark.parametrize("ownership,owner,community", [
    ({"owner": LOBART, "owner_npc_guild": 8}, LOBART, "farm"),
    ({"room_owner": LOBART, "room_owner_guild": 3}, LOBART, "city"),
    ({"chest": "CHEST_LOBART", "chest_owner": LOBART, "chest_owner_guild": 8}, LOBART, "farm"),
    ({"owner_guild": 8}, "", "farm"),
    ({"owner": LOBART, "owner_npc_guild": 12, "item_dropped": True}, LOBART, None),
])
async def test_foreign_world_chest_room_and_guild_ownership(system, ownership, owner, community):
    fact = system.deeds.record(deed(**ownership, witnesses=[HILDA, HILDA.lower(), THIEF]))
    assert fact is not None
    assert fact["fact"] == "npc_crime" and fact["culprit"] == THIEF and fact["kind"] == "theft"
    assert (fact["victim"], fact["owner"], fact["community"]) == (owner, owner, community)
    assert fact["witnesses"] == [HILDA]
    assert system.deeds.seen(HILDA) == [fact]
    assert system.deeds.seen(THIEF) == []
    assert system.hero.facts() == [] and system.hero.known(HILDA) == []
    assert system.deeds.record(deed(**ownership)) is None
    assert len(system.deeds.facts()) == 1


async def test_owner_can_witness_theft_but_attack_victim_cannot_witness_own_assault(system):
    theft = system.deeds.record(deed(owner=LOBART, witnesses=[LOBART, HILDA, THIEF]))
    assert theft["witnesses"] == [LOBART, HILDA]
    attack = system.deeds.record(deed(
        id="attack", kind="attack", victim=VICTIM, victim_guild=3, victim_npctype=3,
        witnesses=[HILDA, VICTIM, THIEF, "PLAYER"],
    ))
    assert (attack["kind"], attack["community"], attack["witnesses"]) == ("attack", "oldcamp", [HILDA])


@pytest.mark.parametrize("fields", [
    {"culprit": "PLAYER"}, {"culprit": ""}, {"count": 0}, {"item": ""},
    {"kind": "attack", "victim": ""}, {"kind": "attack", "victim": THIEF},
    {"kind": "attack", "victim": "PLAYER"},
    {"kind": "attack", "victim": "WOLF", "victim_guild": 30},
])
async def test_malformed_nonphysical_and_non_npc_deeds_are_not_recorded(system, fields):
    assert system.deeds.record(deed(owner=LOBART, **fields)) is None
    assert system.chronicle.facts() == []


@pytest.mark.parametrize("kind,word", [("take_item", "украл"), ("attack", "напал")])
async def test_sources_teach_only_witnesses_then_rumors_about_culprit(system, kind, word):
    system.deeds.record(deed(
        kind=kind, owner=LOBART, victim=VICTIM if kind == "attack" else "", witnesses=[HILDA, LOBART],
    ))
    people = PeopleKnowledge(system.chronicle)
    for witness in (HILDA, LOBART):
        [known] = people.of(witness)
        assert (known.subject, known.kind, known.direct, known.day) == (
            THIEF, "npc_attack" if kind == "attack" else "npc_theft", True, 3,
        )
        assert "Вино" in people.context(witness) and word in people.context(witness)
        assert "ты видел это сам" in people.context(witness)
    assert people.of(THIEF) == [] and people.of("STRANGER") == []
    social = Social([{"npc": npc, "name": name} for npc, name in NAMES.items()])
    social.tie(HILDA, VICTIM).meets = [(0, 1440)]
    rumors = Rumors.__new__(Rumors)
    rumors.social, rumors.chronicle = social, system.chronicle
    rumors.dead, rumors.rumors, rumors.tells = set(), {}, defaultdict(int)
    rumors.dialogue = SimpleNamespace(remember=lambda *args: None)
    rumors.gate, rumors.clock = lambda *args: True, 3 * 1440 + 555
    rumors._load()
    [rumor] = rumors.rumors.values()
    assert rumor.known == {HILDA: 0, LOBART: 0}
    assert rumors.tell(rumor, HILDA, VICTIM) == "told"
    [heard] = people.of(VICTIM)
    assert heard.subject == THIEF and not heard.direct and heard.source == HILDA
    assert word in people.context(VICTIM) and "со слов: Хильда" in people.context(VICTIM)
    before = dict(rumor.known), dict(rumor.since), len(system.chronicle.facts())
    rumors._load()
    [rumor] = rumors.rumors.values()
    assert (rumor.known, rumor.since, len(system.chronicle.facts())) == before


@pytest.mark.parametrize("courage,fights", [(1, False), (2, False), (3, True), (5, True)])
async def test_owner_cries_and_only_brave_owner_fights(system, courage, fights):
    system.deeds.cards = {LOBART: card(LOBART, courage=courage)}
    fact = system.deeds.record(deed(owner=LOBART, witnesses=[LOBART, HILDA]))
    await system.deeds._react(fact, 0)
    actions = system.link.actions
    if not fights:
        assert (LOBART, "say_svm", {"svm": "DIRTYTHIEF", "overlay": True}) in actions
    assert (HILDA, "say_svm", {"svm": "DIRTYTHIEF", "overlay": True}) in actions
    fighting = [(npc, args) for npc, act, args in actions if act == "fight"]
    assert bool(fighting) is fights
    if fights:
        assert fighting == [(LOBART, {
            "npc": THIEF, "reason": "AR_NONE", "deed": False, "nonlethal": True,
            "svm": "DIRTYTHIEF",
        })]


async def test_matching_guard_in_shout_range_fights_and_hears_without_fake_seen(system):
    fact = system.deeds.record(deed(owner=LOBART, owner_npc_guild=3, witnesses=[HILDA]))
    system.link.nearby = [
        {"npc": GUARD, "guild": 2, "distance": 2500},
        {"npc": OTHER_GUARD, "guild": 2, "distance": 2501},
        {"npc": "SLD_FARM", "guild": 7, "distance": 500},
    ]
    system.hero.people[GUARD] = {"guild": 2, "npctype": 0}
    system.hero.people[OTHER_GUARD] = {"guild": 2, "npctype": 0}
    await system.deeds._react(fact, 0)
    fighters = [npc for npc, act, _ in system.link.actions if act == "fight"]
    assert fighters == [GUARD]
    assert system.deeds.seen(GUARD) == []
    [heard] = PeopleKnowledge(system.chronicle).of(GUARD)
    assert heard.subject == THIEF and not heard.direct and heard.source == HILDA
    assert CrimeLaw(system.hero).deliveries() == [] and system.hero.known(GUARD) == []


async def test_witness_guard_fights_even_when_nearby_query_excludes_self(system):
    fact = system.deeds.record(deed(
        kind="attack", victim=VICTIM, victim_guild=3, witnesses=[GUARD],
        witnesses_metadata=[{"npc": GUARD, "guild": 2, "npctype": 0}],
    ))
    await system.deeds._react(fact, 0)
    assert system.link.actions == [
        (GUARD, "fight", {
            "npc": THIEF, "reason": "AR_NONE", "deed": False, "nonlethal": True, "svm": "HELP",
        }),
    ]


async def test_two_witness_guards_each_shout_inside_their_one_native_fight(system):
    fact = system.deeds.record(deed(
        kind="attack", victim=VICTIM, victim_guild=3, witnesses=[GUARD, OTHER_GUARD],
        witnesses_metadata=[{"npc": npc, "guild": 2, "npctype": 0} for npc in (GUARD, OTHER_GUARD)],
    ))
    system.link.nearby = [{"npc": npc, "guild": 2, "distance": 300} for npc in (GUARD, OTHER_GUARD)]
    await system.deeds._react(fact, 0)
    assert [(npc, act, args["svm"]) for npc, act, args in system.link.actions] == [
        (GUARD, "fight", "HELP"), (OTHER_GUARD, "fight", "HELP"),
    ]


async def test_shout_does_not_pull_a_protected_guard_from_its_post(system):
    fact = system.deeds.record(deed(owner=LOBART, owner_npc_guild=3, witnesses=[HILDA]))
    system.link.nearby = [
        {"npc": GUARD, "guild": 2, "npctype": 0, "distance": 300, "state": "ZS_GUARD_PASSAGE"},
        {"npc": OTHER_GUARD, "guild": 2, "npctype": 0, "distance": 500},
    ]
    await system.deeds._react(fact, 0)
    assert [npc for npc, act, _ in system.link.actions if act == "fight"] == [OTHER_GUARD]


@pytest.mark.parametrize("busy", ["fight", "dialog", "sleep", "body"])
async def test_busy_or_sleeping_witness_remembers_but_does_not_act(system, busy):
    fact = system.deeds.record(deed(owner=LOBART, witnesses=[HILDA]))
    if busy == "body":
        system.link.owned.add(HILDA)
    else:
        system.link.receive(NpcRoster(type="npc_roster", t=0, npcs=[{
            "id": HILDA, "name": "Хильда", "distance": 200,
            "activity": "routine" if busy == "sleep" else busy,
            "state": "ZS_SLEEP" if busy == "sleep" else "ZS_STAND",
        }]))
    await system.deeds._react(fact, 0)
    assert system.link.actions == []
    assert PeopleKnowledge(system.chronicle).of(HILDA)


async def test_defeated_thief_returns_only_stolen_amount_to_real_owner(system):
    fact = system.deeds.record(deed(owner=LOBART))
    await system.deeds._reclaim(fact, HILDA, 0)
    assert system.link.effects == [(THIEF, [{
        "do": "give", "item": "ITFO_BREAD", "count": 1, "to": "npc", "npc": LOBART,
    }], True)]
    assert all(expr[2] != ["inst", "OTHER"] for _, exprs in system.link.evaluations for expr in exprs)
    assert system.chronicle.facts("npc_crime_restitution")[0]["culprit"] == THIEF
    assert system.chronicle.facts("crime_restitution") == []


@pytest.mark.parametrize("values", [[1, 1, 0, 1, 1, 0, 0], [1, 0, 1, 1, 1, 0, 0], [1, 0, 0, 0, 1, 0, 0]])
async def test_dead_bodies_and_missing_stolen_item_do_not_restitute(system, values):
    system.link.values = values
    await system.deeds._reclaim(system.deeds.record(deed(owner=LOBART)), HILDA, 0)
    assert system.link.effects == []


async def test_two_fighters_restitute_case_once_and_never_more_than_inventory(system):
    fact = system.deeds.record(deed(owner=LOBART, count=3))
    system.deeds._track_reclaim(fact, HILDA, 0)
    system.deeds._track_reclaim(fact, HILDA, 0)
    system.deeds._track_reclaim(fact, LOBART, 0)
    await asyncio.gather(*tuple(system.deeds._tasks))
    assert len(system.link.effects) == 1 and system.link.effects[0][1][0]["count"] == 2
    assert len(system.chronicle.facts("npc_crime_restitution")) == 1


async def test_no_unconsciousness_does_not_count_as_winning(system):
    fact = system.deeds.record(deed(owner=LOBART))
    system.link.values = [0, 0, 0, 1, 1, 0, 0]
    moments = iter([0.0, 0.0, 181.0])
    system.deeds.clock = lambda: next(moments)
    await system.deeds._reclaim(fact, HILDA, 0)
    assert not system.link.effects


@pytest.mark.parametrize("enabled,with_crimes", [(False, True), (True, False)])
async def test_both_switches_refuse_harm_and_suppress_deeds(system, enabled, with_crimes):
    service = NpcCrimes(
        system.link, system.chronicle, crimes=system.hero if with_crimes else None, enabled=enabled,
    )
    try:
        for action, args in [("steal", {"deed": True}), ("fight", {"deed": True, "npc": VICTIM})]:
            assert "выключены" in await service.check_command(THIEF, action, args)
        assert service.record(deed(owner=LOBART, witnesses=[HILDA])) is None
        assert system.chronicle.facts() == []
        assert await service.check_command(THIEF, "say_svm", {"svm": "HELP"}) is None
    finally:
        await service.close()


async def test_command_guard_checks_actor_target_owner_item_and_explicit_mark(system):
    check = system.deeds.check_command
    assert "deed" in await check(THIEF, "steal", {"chest": "CHEST"})
    assert "сюжетный" in await check(PLOT, "fight", {"npc": VICTIM, "deed": True})
    assert "сюжетный" in await check(THIEF, "fight", {"npc": PLOT, "deed": True})
    system.link.target["owner"] = PLOT
    assert "сюжетный" in await check(THIEF, "steal", {"chest": "CHEST", "deed": True})
    system.link.target.update(owner=LOBART, mission=True)
    assert "вещь для задания" in await check(THIEF, "steal", {"item": "ITFO_BREAD", "deed": True})
    system.link.target.update(mission=False, owner_state="ZS_GUARD_PASSAGE")
    assert "на посту" in await check(THIEF, "steal", {"chest": "CHEST", "deed": True})
    system.link.target["owner_state"] = ""
    args = {"chest": "CHEST", "what": "ITFO_BREAD", "deed": True}
    assert await check(THIEF, "steal", args) is None
    assert args["expected_owner"] == LOBART and args["expected_item"] == "ITFO_BREAD"
    assert await check(THIEF, "fight", {"npc": VICTIM, "deed": True}) is None
    assert await check(PLOT, "fight", {"npc": VICTIM}) is None  # other callers retain their rules


async def test_guard_failure_has_specific_reason_and_no_side_effect(system):
    system.link.target = {"error": "locked chest", "owner": LOBART}
    reason = await system.deeds.check_command(THIEF, "steal", {"chest": "CHEST", "deed": True})
    assert reason == "locked chest"
    assert not system.link.actions and not system.chronicle.facts()


async def test_save_load_restores_deeds_and_knowledge_and_older_save_drops_them(system):
    before = system.memory.head("A")
    first = system.deeds.record(deed(owner=LOBART, witnesses=[HILDA]))
    saved = system.memory.head("A")
    system.deeds.record(deed(id="later", owner=LOBART, witnesses=[HILDA]))
    system.link.receive(Session(type="session", branch="loaded", parent="A", parent_seq=saved, reason="load"))
    assert system.deeds.facts() == [first]
    assert len(PeopleKnowledge(system.chronicle).of(HILDA)) == 1
    assert system.deeds.record(deed(owner=LOBART)) is None
    system.link.receive(Session(type="session", branch="old", parent="A", parent_seq=before, reason="load"))
    assert system.deeds.facts() == [] and PeopleKnowledge(system.chronicle).of(HILDA) == []


async def test_loading_cancels_reclaim_without_writing_to_loaded_branch(system):
    fact = system.deeds.record(deed(owner=LOBART))
    entered, blocked = asyncio.Event(), asyncio.Event()

    async def evaluate(npc, exprs):
        entered.set()
        await blocked.wait()
        return [1, 0, 0, 1, 1, 0, 0]

    system.link.story_eval = evaluate
    system.deeds._track_reclaim(fact, HILDA, 0)
    await entered.wait()
    tasks = tuple(system.deeds._tasks)
    system.link.receive(Loading(type="loading"))
    await asyncio.gather(*tasks, return_exceptions=True)
    assert all(task.cancelled() for task in tasks)
    assert not system.link.effects and not system.chronicle.facts("npc_crime_restitution")


async def test_nonlethal_action_done_can_prove_knockout_after_native_fight_already_ended(system):
    fact = system.deeds.record(deed(owner=LOBART))
    system.link.values = [1, 0, 0, 2, 0, 0, 0]
    system.link.receive(NpcIdle(type="npc_idle", npc=LOBART, act="fight-1", reason="done"))
    await system.deeds._reclaim(fact, LOBART, 0, "fight-1")
    assert len(system.link.effects) == 1


@pytest.mark.parametrize("reason", ["interrupted", "gone", "timeout", "failed", "done"])
async def test_unrelated_later_defeat_after_finished_action_does_not_restitute(system, reason):
    fact = system.deeds.record(deed(owner=LOBART))
    system.link.values = [0 if reason == "done" else 1, 0, 0, 2, 0, 0, 0]
    system.link.receive(NpcIdle(type="npc_idle", npc=LOBART, act="fight-1", reason=reason))
    await system.deeds._reclaim(fact, LOBART, 0, "fight-1")
    assert not system.link.effects


@pytest.mark.parametrize("guild", [16, 18, 27, 31])
async def test_nonhuman_bridge_witness_cannot_learn_speak_or_start_defense(system, guild):
    sheep = "SHEEP_WITNESS"
    fact = system.deeds.record(deed(
        owner=LOBART, witnesses=[sheep, HILDA],
        witnesses_metadata=[{"npc": sheep, "guild": guild}, {"npc": HILDA, "guild": 12}],
    ))
    assert fact["witnesses"] == [HILDA]
    assert system.deeds.seen(sheep) == []
    assert PeopleKnowledge(system.chronicle).of(sheep) == []
    await system.deeds._react(fact, 0)
    assert all(npc != sheep for npc, _, _ in system.link.actions)
    assert system.chronicle.facts("npc_crime_reaction")[0]["npc"] != sheep


async def test_cached_nonhuman_is_filtered_even_when_old_snapshot_lacks_metadata(system):
    sheep = "SHEEP_WITNESS"
    system.hero.people[sheep] = {"guild": 18}
    fact = system.deeds.record(deed(owner=LOBART, witnesses=[sheep, HILDA]))
    assert fact["witnesses"] == [HILDA]


@pytest.mark.parametrize("guild", [None, 18, 27])
async def test_old_witness_without_guild_needs_live_human_proof_before_svm(system, guild):
    # Legacy bridge observations remain evidence, but do not authorize a voice without live identity.
    fact = system.deeds.record(deed(owner=LOBART, witnesses=[HILDA]))
    system.link.guilds[HILDA] = guild
    assert fact["witnesses"] == [HILDA]
    await system.deeds._react(fact, 0)
    assert system.link.actions == []
    assert system.link.evaluations[0] == (HILDA, [["field", ["inst", "SELF"], "GUILD"]])


async def test_unknown_human_witness_is_confirmed_once_before_svm(system):
    fact = system.deeds.record(deed(owner=LOBART, witnesses=[HILDA]))
    await system.deeds._react(fact, 0)
    assert system.hero.people[HILDA]["guild"] == 12
    assert system.link.actions == [(HILDA, "say_svm", {"svm": "DIRTYTHIEF", "overlay": True})]
    identities = [exprs for _, exprs in system.link.evaluations if len(exprs) == 1]
    assert identities == [[["field", ["inst", "SELF"], "GUILD"]]]


async def test_direct_svm_and_fight_svm_refuse_nonhuman_speakers(system):
    system.hero.people[HILDA] = {"guild": 18}
    for action, args in [("say_svm", {"svm": "HELP"}), ("fight", {"npc": THIEF, "svm": "HELP"})]:
        with pytest.raises(GameError, match="not a human speaker"):
            await system.deeds._act(HILDA, action, **args)
    assert system.link.actions == []


async def test_defender_knockout_ends_tracker_and_leaves_stolen_goods_with_thief(system):
    fact = system.deeds.record(deed(owner=LOBART))
    system.link.values = [0, 0, 0, 2, 0, 0, 1]
    system.deeds._track_reclaim(fact, LOBART, 0, "fight-1")
    await asyncio.gather(*tuple(system.deeds._tasks))
    assert not system.deeds._reclaiming and not system.link.effects
    assert not system.chronicle.facts("npc_crime_restitution")
    [result] = system.chronicle.facts("npc_crime_defense")
    assert result == {
        "fact": "npc_crime_defense", "crime": fact["id"], "culprit": THIEF, "fighter": LOBART,
        "act": "fight-1", "outcome": "defender_down", "reason": "native_unconsciousness",
        "item": "ITFO_BREAD", "count": 1, "returned": 0,
    }


async def test_native_knockout_receipt_can_arrive_before_script_unconscious_state(system):
    fact = system.deeds.record(deed(owner=LOBART))
    system.link.values = [0, 0, 0, 2, 0, 0, 0]
    system.link.receive(NpcIdle(
        type="npc_idle", npc=LOBART, act="fight-1", reason="done", detail="target_down",
    ))
    await system.deeds._reclaim(fact, LOBART, 0, "fight-1")
    assert len(system.link.effects) == 1
    [result] = system.chronicle.facts("npc_crime_defense")
    assert result["outcome"] == "reclaimed" and result["returned"] == 1


async def test_native_defender_down_receipt_ends_tracker_before_script_state_catches_up(system):
    fact = system.deeds.record(deed(owner=LOBART))
    system.link.values = [0, 0, 0, 2, 0, 0, 0]
    system.link.receive(NpcIdle(
        type="npc_idle", npc=LOBART, act="fight-1", reason="gone", detail="defender_down",
    ))
    await system.deeds._reclaim(fact, LOBART, 0, "fight-1")
    assert not system.link.effects
    assert system.chronicle.facts("npc_crime_defense")[0]["outcome"] == "defender_down"


async def test_native_timeout_is_terminal_and_does_not_reclaim_or_restart_fight(system):
    fact = system.deeds.record(deed(owner=LOBART))
    system.link.values = [0, 0, 0, 2, 1, 0, 0]
    system.link.receive(NpcIdle(type="npc_idle", npc=LOBART, act="fight-1", reason="timeout"))
    await system.deeds._reclaim(fact, LOBART, 0, "fight-1")
    assert not system.link.effects and not system.link.actions
    assert system.chronicle.facts("npc_crime_defense")[0]["outcome"] == "timeout"


async def test_server_waits_for_native_timeout_receipt_reserve_then_finishes_once(system):
    fact = system.deeds.record(deed(owner=LOBART))
    moments = iter([0.0, 121.0, 181.0])
    system.deeds.clock = lambda: next(moments)
    system.link.values = [0, 0, 0, 2, 1, 0, 0]
    await system.deeds._reclaim(fact, LOBART, 0, "fight-1")
    assert len(system.link.evaluations) == 1 and not system.link.effects
    [result] = system.chronicle.facts("npc_crime_defense")
    assert result["outcome"] == "timeout" and result["returned"] == 0


@pytest.mark.parametrize("error_type", [GameError, TimeoutError])
async def test_reclaim_failure_warns_with_actual_error_and_tracked_fight(system, caplog, error_type):
    fact = system.deeds.record(deed(owner=LOBART))

    async def fail_transfer(npc, effects, *, check=False):
        raise error_type("missing: hero ITFO_BREAD 1 (has 0)")

    system.link.story_effect = fail_transfer
    with caplog.at_level("WARNING", logger="gothic_ai.npc_crimes"):
        await system.deeds._reclaim(fact, LOBART, 0, "fight-1")
    assert f"{error_type.__name__}: missing: hero ITFO_BREAD 1 (has 0)" in caplog.text
    assert fact["id"] in caplog.text and LOBART in caplog.text and "fight-1" in caplog.text
    assert system.chronicle.facts("npc_crime_defense")[0]["outcome"] == "unavailable"
    assert not system.chronicle.facts("npc_crime_restitution")
