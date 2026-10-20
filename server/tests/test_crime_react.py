from __future__ import annotations

import asyncio
from types import SimpleNamespace
from typing import Any

import pytest

from gothic_ai import crime_react
from gothic_ai.characters import Card
from gothic_ai.chronicle import Chronicle
from gothic_ai.crime_law import CrimeLaw
from gothic_ai.crime_react import CrimeReact, reaction_for
from gothic_ai.crimes import Crimes
from gothic_ai.game import GameError
from gothic_ai.memory import MemoryStore
from gothic_ai.places import PlaceBook
from gothic_ai.protocol import HoldVerdictCmd, NpcActResult, NpcRoster, PerceptionHeld
from tests.test_crimes import HILDA, LOBART, Link, deed

GUARD = "MIL_305_JERGEL"
OTHER_GUARD = "MIL_306_GUARD"
MATTEO = "VLK_406_MATTEO"
CITIZEN = "VLK_400_CITIZEN"


@pytest.mark.parametrize("kind,role,courage,honesty,guards,expected", [
    ("theft", "owner", 3, 3, False, "fight"),
    ("theft", "owner", 2, 3, False, "demand"),
    ("theft", "owner", 2, 3, True, "call_guards"),
    ("theft", "guard", 1, 1, False, "fight"),
    ("theft", "member", 3, 4, True, "call_guards"),
    ("theft", "member", 3, 4, False, "shout"),
    ("theft", "member", 3, 2, True, "silent"),
    ("theft", "member", 3, 3, True, "shout"),
    ("theft", "stranger", 5, 5, True, "ignore"),
    ("attack", "owner", 5, 5, True, None),
    ("attack", "guard", 1, 1, False, "fight"),
    ("attack", "member", 4, 3, False, "fight"),
    ("attack", "member", 3, 3, False, "shout"),
    ("attack", "member", 2, 3, False, "flee"),
    ("attack", "stranger", 1, 5, True, "ignore"),
    ("murder", "owner", 5, 5, True, None),
    ("murder", "guard", 1, 1, False, "fight"),
    ("murder", "member", 4, 3, False, "fight"),
    ("murder", "member", 3, 3, False, "flee"),
    ("murder", "stranger", 2, 3, False, "flee"),
    ("murder", "stranger", 3, 3, False, "silent"),
    ("sheep", "owner", 1, 1, False, "fight"),
    ("sheep", "guard", 1, 1, False, "fight"),
    ("sheep", "member", 3, 3, False, "shout"),
    ("sheep", "stranger", 5, 5, False, "ignore"),
    ("enter_room", "owner", 5, 5, True, None),
])
def test_reaction_table(kind, role, courage, honesty, guards, expected):
    reaction = reaction_for(kind, role, courage=courage, honesty=honesty, guards=guards)
    assert (reaction.kind if reaction else None) == expected


def test_vanilla_fight_reasons_keep_theft_nonlethal_and_murder_native():
    assert reaction_for("theft", "guard").fight_reason == "AR_THEFT"
    assert reaction_for("murder", "guard").fight_reason == "AR_HUMANMURDEREDHUMAN"
    assert reaction_for("sheep", "guard").fight_reason == "AR_SHEEPKILLER"
    assert reaction_for("attack", "guard").fight_reason == "AR_GUARDSTOPSFIGHT"


@pytest.mark.parametrize("kind", ["theft", "attack", "murder", "sheep", "unknown"])
@pytest.mark.parametrize("courage", [1, 3, 5])
def test_ally_always_consumes_the_perception_without_action(kind, courage):
    reaction = reaction_for(kind, "ally", courage=courage, honesty=5, guards=True)
    assert reaction is not None
    assert (reaction.kind, reaction.svm, reaction.fight_reason) == ("ignore", "", "")


class ReactLink(Link):
    def __init__(self) -> None:
        super().__init__()
        self.actions: list[tuple[str, str, dict[str, Any]]] = []
        self.effects: list[tuple[str, list[dict[str, Any]], bool]] = []
        self.people: list[dict[str, Any]] = []
        self.nearby: list[dict[str, Any]] = []
        self.accept = True
        self.ack = True
        self.owned: set[str] = set()
        self.eval_values = [3, 0]

    def body_owned(self, npc):
        return npc in self.owned

    async def story_people(self):
        return self.people

    async def story_eval(self, npc, exprs):
        if exprs == [["var", "GIL_MAX"], ["var", "ATT_HOSTILE"]]:
            return [2, 0]
        if exprs and exprs[0][:2] == ["call", "WLD_GETGUILDATTITUDE"]:
            return [2] * len(exprs)
        return self.eval_values

    async def story_nearby(self, npc, radius):
        return self.nearby

    async def story_effect(self, npc, effects, *, check=False):
        self.effects.append((npc, effects, check))

    async def npc_act(self, npc, action, /, **args):
        self.actions.append((npc, action, args))
        action_id = f"a{len(self.actions)}"
        if self.ack:
            self.receive(NpcActResult(
                type="npc_act_result", id=action_id, npc=npc,
                status="queued" if self.accept else "rejected", detail="busy" if not self.accept else None,
            ))
        return action_id


def card(npc, *, courage=3, honesty=3):
    return Card(npc, npc, "", "", 3, honesty, courage, 3, 3, 3)


@pytest.fixture
async def system():
    link = ReactLink()
    memory = MemoryStore(":memory:")
    chronicle = Chronicle(link, memory)  # type: ignore[arg-type]
    crimes = Crimes(link, chronicle)  # type: ignore[arg-type]
    law = CrimeLaw(crimes)
    reactions = CrimeReact(link, crimes, law)  # type: ignore[arg-type]
    crimes.people.update({
        HILDA: {"guild": 12, "npctype": 0, "home_room": "LOBART"},
        LOBART: {"guild": 12, "npctype": 0, "home_room": "LOBART"},
        GUARD: {"guild": 2, "npctype": 0},
        OTHER_GUARD: {"guild": 2, "npctype": 0},
        MATTEO: {"guild": 3, "npctype": 0},
        CITIZEN: {"guild": 3, "npctype": 0},
    })
    yield SimpleNamespace(
        link=link, memory=memory, chronicle=chronicle, crimes=crimes, law=law, react=reactions,
    )
    await reactions.close()
    await crimes.close()
    if chronicle._background:
        await asyncio.gather(*chronicle._background)
    memory.close()


def held(**fields):
    return PerceptionHeld(
        type="perception_held", hold="h1", npc=HILDA, other="PLAYER", kind="theft", deed="deed-1", **fields,
    )


def crime(system, *, npc=HILDA, owner=LOBART, guild=12):
    system.link.receive(deed(
        owner=owner, owner_npc_guild=guild, witnesses=[npc],
        witnesses_metadata=[{"npc": npc, "guild": system.crimes.people.get(npc, {}).get("guild", 3),
                             "npctype": 0}],
    ))
    return system.crimes.crime("deed-1")


async def finish(system):
    # Only holds, not the separate post-combat restitution task, need to finish here.
    for _ in range(30):
        if system.react.history:
            return system.react.history[-1]
        await asyncio.sleep(0.002)
    raise AssertionError("hold was not settled")


def verdicts(system):
    return [message for message in system.link.sent if isinstance(message, HoldVerdictCmd)]


async def test_roles_use_true_ownership_community_and_real_shared_house(system):
    case = crime(system)
    assert system.react.role(LOBART, case) == "owner"
    assert system.react.role(HILDA, case) == "member"
    system.crimes.people[HILDA]["home_room"] = "ANOTHER_HOUSE"
    assert system.react.role(HILDA, case) == "stranger"
    del system.crimes.people[HILDA]["home_room"]
    assert system.react.role(HILDA, case) is None
    city = {**case, "community": "city", "owner": MATTEO, "victim": MATTEO}
    assert system.react.role(GUARD, city) == "guard"
    assert system.react.role("MIL_311_ANDRE", city) == "guard"
    assert system.react.role(CITIZEN, city) == "member"
    system.crimes.people["BANDIT"] = {"guild": 9, "npctype": 5}
    assert system.react.role("BANDIT", {**city, "community": "bl"}) == "stranger"


async def test_room_and_chest_owner_are_owners_even_when_item_owner_is_another_npc(system):
    system.link.receive(deed(
        owner=LOBART, owner_npc_guild=12, room_owner=HILDA, room_owner_guild=12,
        chest_owner="CHESTKEEPER", chest_owner_guild=12,
    ))
    case = system.crimes.crime("deed-1")
    assert system.react.role(HILDA, case) == "owner"
    assert system.react.role("CHESTKEEPER", case) == "owner"


async def test_ally_role_precedes_guard_but_not_owner(system):
    case = crime(system, npc=GUARD, owner=MATTEO, guild=3)
    for npc in (GUARD, MATTEO):
        system.chronicle.record({"fact": "crime_side", "npc": npc, "crime": case["id"], "why": "fought"})
    assert system.react.role(GUARD.lower(), case) == "ally"
    assert system.react.role(MATTEO, case) == "owner"


@pytest.mark.parametrize("npc,expected", [
    (HILDA, "ally"), (CITIZEN, "member"), (GUARD, "guard"), (MATTEO, "owner"),
])
async def test_justified_hearsay_changes_only_an_unrelated_listeners_role(system, npc, expected):
    case = system.crimes.record(deed(kind="murder", victim=MATTEO, victim_guild=3))
    system.chronicle.record({
        "fact": "rumor", "to": npc, "from": "ALLY", "crime": case["id"], "kind": "murder", "hops": 1,
        "excuse": "fought", "excuse_by": "ALLY", "excuse_by_name": "Союзник",
    })
    assert system.react.role(npc, case) == expected
    assert system.crimes.does_not_blame(npc, case) is (expected == "ally")
    if expected == "ally":
        assert "Ты не винишь героя" in system.crimes.situation(npc)
        system.chronicle.record({"fact": "crime_seen", "npc": npc, "crime": case["id"]})
        assert system.react.role(npc, case) == "stranger"
        assert not system.crimes.does_not_blame(npc, case)


async def test_unrelated_bandit_does_not_blame_a_justified_killing_in_the_bl_community(system):
    system.crimes.people[HILDA] = {"guild": 9, "npctype": 5}
    case = system.crimes.record(deed(kind="murder", victim=LOBART, victim_guild=9, victim_npctype=5))
    system.chronicle.record({
        "fact": "rumor", "to": HILDA, "from": "ALLY", "crime": case["id"], "kind": "murder",
        "excuse": "hostile", "excuse_by": "ALLY",
    })
    assert system.react.base_role(HILDA, case) == "stranger"
    assert system.react.role(HILDA, case) == "ally"
    assert system.crimes.attitude(HILDA) is None
    assert system.crimes.distrust(HILDA) == []


@pytest.mark.parametrize("unknown", [HILDA, LOBART])
async def test_household_requires_observer_and_owner_home_metadata(system, unknown):
    case = crime(system)
    del system.crimes.people[unknown]["home_room"]
    assert system.react.role(HILDA, case) is None


@pytest.mark.parametrize("observer_home,owner_home", [("", "LOBART"), ("LOBART", ""), ("", "")])
async def test_empty_home_is_uncertain_even_when_the_key_is_present(system, observer_home, owner_home):
    case = crime(system)
    system.crimes.people[HILDA]["home_room"] = observer_home
    system.crimes.people[LOBART]["home_room"] = owner_home
    assert system.react.role(HILDA, case) is None


def sleeping_house(system, npc, waypoint, *, state="ZS_GOTOBED", room=""):
    system.crimes.people[npc].update({
        "home_room": room, "routine": [{"state": state, "waypoint": waypoint}],
    })


@pytest.mark.parametrize("state", ["ZS_GOTOBED", "ZS_SLEEP"])
async def test_real_lobart_beds_share_a_house_without_portal_rooms(system, state):
    system.crimes.places = PlaceBook.load()
    assert system.crimes.places is not None
    case = crime(system)
    sleeping_house(system, HILDA, "NW_FARM1_INHOUSE_BED_01", state=state, room="HILDA_BEDROOM")
    sleeping_house(system, LOBART, "NW_FARM1_INHOUSE_BED_02", room="LOBART_BEDROOM")
    assert system.react.role(HILDA, case) == "member"
    assert system.react.home(HILDA) == system.react.home(LOBART) == "place:FARM1_INHOUSE"
    system.crimes.people[HILDA]["home_room"] = ""
    system.crimes.people[LOBART]["home_room"] = ""
    assert system.react.role(HILDA, case) == "member"


async def test_distinct_mapped_houses_are_not_a_household_even_with_equal_guilds(system):
    system.crimes.places = PlaceBook.load()
    case = crime(system)
    sleeping_house(system, HILDA, "NW_FARM1_INSTABLE_BED_02")
    sleeping_house(system, LOBART, "NW_FARM1_INHOUSE_BED_02")
    assert system.react.role(HILDA, case) == "stranger"


@pytest.mark.parametrize("waypoint,state", [
    ("NW_FARM1_OUT_01", "ZS_GOTOBED"),  # a known yard is not evidence of a shared house
    ("UNKNOWN_BED", "ZS_GOTOBED"),
    ("NW_FARM1_INHOUSE_BED_01", "ZS_STAND_GUARDING"),  # working inside is not living together
])
async def test_household_fallback_requires_known_sleep_routine_inside_a_house(system, waypoint, state):
    system.crimes.places = PlaceBook.load()
    case = crime(system)
    sleeping_house(system, HILDA, waypoint, state=state)
    sleeping_house(system, LOBART, "NW_FARM1_INHOUSE_BED_02")
    assert system.react.role(HILDA, case) is None


async def test_mapped_beds_without_the_place_book_stay_uncertain(system):
    case = crime(system)
    sleeping_house(system, HILDA, "NW_FARM1_INHOUSE_BED_01")
    sleeping_house(system, LOBART, "NW_FARM1_INHOUSE_BED_02")
    assert system.react.role(HILDA, case) is None
    assert system.react.home(HILDA) is None


async def test_hold_waits_for_crime_and_keeps_real_seen_before_one_server_verdict(system):
    system.link.receive(held())
    await asyncio.sleep(0)
    assert not verdicts(system)
    crime(system)
    row = await finish(system)
    assert row["role"] == "member" and row["reaction"] == "shout"
    assert system.crimes.known(HILDA)[0].degree == "seen"
    assert system.link.actions == [(HILDA, "say_svm", {"svm": "DIRTYTHIEF", "overlay": True})]
    assert [(v.hold, v.to) for v in verdicts(system)] == [("h1", "server")]
    system.link.receive(held())
    await asyncio.sleep(0)
    assert len(verdicts(system)) == 1
    assert "shout" in system.react.summary("deed-1")


@pytest.mark.parametrize("held_first", [False, True])
async def test_classified_noncrime_releases_immediately_without_waiting_for_deadline(
    system, monkeypatch, held_first,
):
    monkeypatch.setattr(crime_react, "HOLD_SECONDS", 5.0)
    if held_first:
        system.link.receive(held())
        await asyncio.sleep(0)
        assert not verdicts(system)
    system.link.receive(deed())  # Crimes explicitly classifies this unowned item as lawful
    if not held_first:
        system.link.receive(held())
    row = await finish(system)  # finishes within the helper's short window, not five seconds
    assert row["verdict"] == "scripts" and row["reason"] == "noncrime"
    assert len(verdicts(system)) == 1 and system.link.actions == []
    system.link.receive(held())
    await asyncio.sleep(0)
    assert len(verdicts(system)) == 1


async def test_duplicate_deed_still_resolves_to_its_existing_crime(system):
    crime(system)
    assert system.crimes.record(deed(owner=LOBART, owner_npc_guild=12)) is None
    system.link.receive(held())
    row = await finish(system)
    assert row["verdict"] == "server" and row["reaction"] == "shout"


async def test_unclassified_deed_without_a_session_still_waits_for_real_crime(system):
    system.chronicle.branch = None
    system.link.receive(deed())
    system.link.receive(held())
    await asyncio.sleep(0)
    assert not verdicts(system)
    system.chronicle.branch = "A"
    crime(system)
    assert (await finish(system))["verdict"] == "server"


async def test_missing_card_uses_three_and_projection_precedes_fight(system):
    seen = []

    async def project(npc):
        seen.append((npc, list(system.link.actions)))

    system.react.project = project
    crime(system, npc=LOBART)
    system.link.receive(held().model_copy(update={"npc": LOBART}))
    row = await finish(system)
    assert row["reaction"] == "fight"
    assert seen == [(LOBART, [])]
    assert system.link.actions == [(LOBART, "fight", {
        "npc": "PLAYER", "reason": "AR_THEFT", "svm": "DIRTYTHIEF",
    })]


async def test_flee_svm_is_part_of_native_state_start_instead_of_a_cleared_queue(system):
    system.react.cards = {HILDA: card(HILDA, courage=2)}
    system.link.receive(deed(
        kind="murder", victim=LOBART, victim_guild=12, witnesses=[HILDA],
        witnesses_metadata=[{"npc": HILDA, "guild": 12, "npctype": 0}],
    ))
    system.link.receive(held().model_copy(update={"kind": "murder"}))
    row = await finish(system)
    assert row["reaction"] == "flee" and row["verdict"] == "server"
    assert system.link.actions == [(HILDA, "flee", {"npc": "PLAYER", "svm": "HELP"})]


async def test_silence_and_ignore_are_valid_server_verdicts_without_body_action(system):
    system.react.cards = {HILDA: card(HILDA, honesty=2)}
    crime(system)
    system.link.receive(held())
    assert (await finish(system))["reaction"] == "silent"
    assert verdicts(system)[0].to == "server"
    assert system.link.actions == []
    assert system.crimes.known(HILDA)[0].degree == "seen"


@pytest.mark.parametrize("busy", [False, True])
async def test_allied_murder_witness_keeps_truth_and_suppresses_vanilla_even_when_busy(system, busy):
    system.link.receive(deed(
        kind="murder", victim=MATTEO, victim_guild=3, witnesses=[GUARD],
        witnesses_metadata=[{"npc": GUARD, "guild": 2, "npctype": 0}],
    ))
    system.chronicle.record({"fact": "crime_side", "npc": GUARD, "crime": "deed-1", "why": "fought"})
    if busy:
        system.link.owned.add(GUARD)
    system.link.receive(held().model_copy(update={"kind": "murder", "npc": GUARD}))
    row = await finish(system)
    assert (row["role"], row["reaction"], row["verdict"]) == ("ally", "ignore", "server")
    assert system.crimes.known(GUARD)[0].degree == "seen"
    assert system.link.actions == []
    assert [(v.hold, v.to) for v in verdicts(system)] == [("h1", "server")]
    assert system.law.tick(3 * 1440 + 9 * 60 + 15) == []


@pytest.mark.parametrize("failure", ["missing_crime", "missing_deed", "busy", "owned", "unknown_role"])
async def test_uncertain_hold_returns_to_scripts_exactly_once(system, monkeypatch, failure):
    monkeypatch.setattr(crime_react, "HOLD_SECONDS", 0.02)
    if failure != "missing_crime":
        crime(system)
    message = held()
    if failure == "missing_deed":
        message = message.model_copy(update={"deed": None})
    elif failure == "busy":
        system.link.receive(NpcRoster(type="npc_roster", t=0, npcs=[{
            "id": HILDA, "name": "Хильда", "guild": 12, "distance": 200, "activity": "dialog",
        }]))
    elif failure == "owned":
        system.link.owned.add(HILDA)
    elif failure == "unknown_role":
        del system.crimes.people[HILDA]["home_room"]
    system.link.receive(message)
    row = await finish(system)
    expected = "busy" if failure == "owned" else failure
    assert row["verdict"] == "scripts" and row["reason"] == expected
    assert len(verdicts(system)) == 1 and system.link.actions == []


@pytest.mark.parametrize("accept,ack,reason", [(False, True, "action_rejected:busy"),
                                            (True, False, "deadline")])
async def test_body_receipt_failure_returns_perception_to_scripts(system, monkeypatch, accept, ack, reason):
    monkeypatch.setattr(crime_react, "HOLD_SECONDS", 0.02)
    system.link.accept, system.link.ack = accept, ack
    crime(system)
    system.link.receive(held())
    row = await finish(system)
    assert row["verdict"] == "scripts" and row["reason"] == reason
    assert len(verdicts(system)) == 1


async def test_other_threats_remain_for_the_planner(system):
    system.link.receive(held().model_copy(update={"deed": None, "other": "WOLF"}))
    await asyncio.sleep(0)
    assert not system.react._tasks and not verdicts(system)


@pytest.mark.parametrize("deed_id", [None, "deed-1"])
async def test_room_entry_remains_for_encounter_rules(system, deed_id):
    crime(system)
    system.link.receive(held().model_copy(update={"kind": "enter_room", "deed": deed_id}))
    await asyncio.sleep(0)
    assert not system.react._tasks and not system.react._holds
    assert not verdicts(system) and not system.link.actions


async def test_called_guards_hear_and_inform_law_and_nearest_free_guard_fights(system):
    system.react.cards = {CITIZEN: card(CITIZEN, honesty=4)}
    system.link.nearby = [
        {"npc": OTHER_GUARD, "guild": 2, "distance": 900},
        {"npc": GUARD, "guild": 2, "distance": 300},
        {"npc": "SLD_800_LEE", "guild": 7, "distance": 100},
    ]
    system.crimes.people["SLD_800_LEE"] = {"guild": 7, "npctype": 0}
    crime(system, npc=CITIZEN, owner=MATTEO, guild=3)
    system.link.receive(held().model_copy(update={"npc": CITIZEN}))
    row = await finish(system)
    assert row["reaction"] == "call_guards" and row["verdict"] == "server"
    assert system.crimes.known(GUARD)[0].degree == "heard"
    assert system.crimes.known(OTHER_GUARD)[0].source == CITIZEN
    assert system.chronicle.facts("crime_known")[0]["how"] == "called"
    assert system.link.actions[-1] == (GUARD, "fight", {"npc": "PLAYER", "reason": "AR_GUARDCALLEDTOTHIEF"})
    assert system.crimes.known("SLD_800_LEE") == []


async def test_guard_witness_fights_and_law_knows_without_model(system):
    crime(system, npc=GUARD, owner=MATTEO, guild=3)
    system.link.receive(held().model_copy(update={"npc": GUARD}))
    row = await finish(system)
    assert row["role"] == "guard" and row["reaction"] == "fight"
    assert system.law.tick(3 * 1440 + 9 * 60 + 15)[0]["how"] == "guard"


async def test_cold_guard_metadata_is_shared_with_called_law_path(system):
    guard = "SLD_900_FARM_GUARD"
    case = crime(system, owner=MATTEO, guild=8)
    system.link.nearby = [{"npc": guard, "guild": 7, "distance": 300}]
    assert guard not in system.crimes.people
    guards = await system.react._guards(HILDA, case)
    assert guards == [guard]
    assert system.crimes.people[guard]["guild"] == 7
    system.law.called(case["id"], HILDA, guards)
    assert system.crimes.known(guard)[0].degree == "heard"
    assert system.chronicle.facts("crime_known")[0]["how"] == "called"


async def test_called_guard_who_sided_with_the_hero_is_not_sent_to_fight(system):
    system.react.cards = {CITIZEN: card(CITIZEN, honesty=4)}
    system.link.nearby = [
        {"npc": GUARD, "guild": 2, "distance": 100},
        {"npc": OTHER_GUARD, "guild": 2, "distance": 500},
    ]
    case = crime(system, npc=CITIZEN, owner=MATTEO, guild=3)
    system.chronicle.record({"fact": "crime_side", "npc": GUARD, "crime": case["id"], "why": "hostile"})
    system.link.receive(held().model_copy(update={"npc": CITIZEN}))
    row = await finish(system)
    assert row["reaction"] == "call_guards"
    assert system.crimes.known(GUARD) == []
    assert system.crimes.known(OTHER_GUARD)[0].degree == "heard"
    assert system.link.actions[-1] == (
        OTHER_GUARD, "fight", {"npc": "PLAYER", "reason": "AR_GUARDCALLEDTOTHIEF"},
    )


async def test_prepare_uses_real_house_metadata_and_preserves_npctype(system):
    system.link.people = [{"npc": HILDA, "guild": 12, "home_room": "NW_HOUSE_LOBART"}]
    await system.react.prepare()
    assert system.crimes.people[HILDA] == {
        "npc": HILDA, "guild": 12, "npctype": 0, "home_room": "NW_HOUSE_LOBART",
    }


async def test_failed_guild_read_keeps_people_ready_and_retries_only_the_matrix(system):
    now = 0.0
    system.react.clock = lambda: now
    system.link.people = [{"npc": HILDA, "guild": 12, "home_room": "LOBART"}]
    people_calls, matrix_calls = 0, 0
    read_people, evaluate = system.link.story_people, system.link.story_eval

    async def people():
        nonlocal people_calls
        people_calls += 1
        return await read_people()

    async def guilds(npc, exprs):
        nonlocal matrix_calls
        if exprs and exprs[0][:2] == ["call", "WLD_GETGUILDATTITUDE"]:
            matrix_calls += 1
            if matrix_calls == 1:
                raise GameError("matrix unavailable")
        return await evaluate(npc, exprs)

    system.link.story_people, system.link.story_eval = people, guilds
    await system.react.prepare()
    assert system.react._prepared
    assert system.crimes.people[HILDA]["home_room"] == "LOBART"
    assert not system.crimes.sides._prepared
    assert (people_calls, matrix_calls) == (1, 1)

    now = 4.9
    await system.react.prepare()
    assert (people_calls, matrix_calls) == (1, 1)
    now = 5.0
    await system.react.prepare()
    assert system.crimes.sides._prepared
    assert (people_calls, matrix_calls) == (1, 2)


async def test_identity_read_preserves_home_loaded_while_vm_reply_is_pending(system):
    system.crimes.people[HILDA] = {"guild": 12}
    reply = asyncio.get_running_loop().create_future()
    evaluate_world = system.link.story_eval

    async def evaluate(npc, expressions):
        if npc == "PC_HERO":
            return await evaluate_world(npc, expressions)
        return await reply

    system.link.story_eval = evaluate
    pending = asyncio.create_task(system.react._person(HILDA))
    await asyncio.sleep(0)
    system.link.people = [{"npc": HILDA, "guild": 12, "home_room": "LOBART"}]
    await system.react.prepare()
    reply.set_result([12, 0])
    await pending
    assert system.crimes.people[HILDA]["home_room"] == "LOBART"
    assert system.crimes.people[HILDA]["npctype"] == 0


async def test_restitution_only_moves_existing_goods_after_proven_unconsciousness(system):
    case = crime(system)
    system.link.eval_values = [1, 0, 0, 1, 0, 1]
    await system.react._reclaim(case, LOBART, 0)
    assert system.link.effects == [(LOBART, [{
        "do": "give", "item": "ITFO_BREAD", "count": 1, "to": "self",
    }], True)]
    assert system.chronicle.facts("crime_restitution")[0]["how"] == "defeated_thief"


async def test_second_fighter_returns_goods_when_first_fighter_leaves_the_fight(system):
    case = crime(system, npc=MATTEO, owner=MATTEO, guild=3)
    owner_left = asyncio.Event()
    owner_reads = 0

    async def evaluate(npc, expressions):
        nonlocal owner_reads
        if npc == MATTEO:
            owner_reads += 1
            if owner_reads == 1:
                return [0, 0, 0, 2, 1, 0]
            owner_left.set()
            return [0, 0, 0, 2, 0, 0]
        await owner_left.wait()
        return [1, 0, 0, 2, 1, 0]

    system.link.story_eval = evaluate
    system.react._track_reclaim(case, MATTEO, 0)
    system.react._track_reclaim(case, GUARD, 0)
    await asyncio.wait_for(asyncio.gather(*tuple(system.react._tasks)), 2.0)
    assert owner_reads == 2
    assert system.link.effects == [(MATTEO, [{
        "do": "give", "item": "ITFO_BREAD", "count": 1, "to": "self",
    }], True)]
    assert system.chronicle.facts("crime_restitution")[0]["fighter"] == GUARD


async def test_two_fighters_observing_same_defeat_return_only_the_stolen_amount(system):
    case = crime(system, npc=MATTEO, owner=MATTEO, guild=3)
    observed = set()
    both_observed, transfer_started, finish_transfer = (asyncio.Event() for _ in range(3))

    async def evaluate(npc, expressions):
        observed.add(npc)
        if len(observed) == 2:
            both_observed.set()
        await both_observed.wait()
        return [1, 0, 0, 2, 1, 0]  # the hero has a second, unrelated copy of the item

    async def transfer(npc, effects, *, check=False):
        system.link.effects.append((npc, effects, check))
        transfer_started.set()
        await finish_transfer.wait()

    system.link.story_eval, system.link.story_effect = evaluate, transfer
    system.react._track_reclaim(case, MATTEO, 0)
    system.react._track_reclaim(case, MATTEO, 0)  # repeat delivery must not duplicate this fighter
    system.react._track_reclaim(case, GUARD, 0)
    tasks = tuple(system.react._tasks)
    assert len(tasks) == 2
    await asyncio.wait_for(transfer_started.wait(), 1.0)
    await asyncio.sleep(0)
    finish_transfer.set()
    await asyncio.wait_for(asyncio.gather(*tasks), 1.0)
    assert len(system.link.effects) == 1
    assert system.link.effects[0][1][0]["count"] == 1
    assert len(system.chronicle.facts("crime_restitution")) == 1


async def test_session_change_cancels_all_fighters_waiting_to_return_goods(system):
    case = crime(system, npc=MATTEO, owner=MATTEO, guild=3)
    observed = set()
    both_observed = asyncio.Event()
    pending_reply = asyncio.Event()

    async def evaluate(npc, expressions):
        observed.add(npc)
        if len(observed) == 2:
            both_observed.set()
        await pending_reply.wait()
        return [1, 0, 0, 2, 1, 0]

    system.link.story_eval = evaluate
    system.react._track_reclaim(case, MATTEO, 0)
    system.react._track_reclaim(case, GUARD, 0)
    tasks = tuple(system.react._tasks)
    await asyncio.wait_for(both_observed.wait(), 1.0)
    system.link.receive(system.link.session.model_copy(update={"branch": "B"}))
    await asyncio.gather(*tasks, return_exceptions=True)
    assert all(task.cancelled() for task in tasks)
    assert not system.react._reclaiming and not system.react._restitution_started
    assert not system.link.effects and not system.chronicle.facts("crime_restitution")


@pytest.mark.parametrize("values", [[1, 1, 0, 1, 1, 0], [1, 0, 1, 1, 1, 0], [1, 0, 0, 0, 1, 0]])
async def test_dead_hero_dead_witness_or_missing_goods_never_restitute(system, values):
    system.link.eval_values = values
    await system.react._reclaim(crime(system), LOBART, 0)
    assert not system.link.effects and not system.chronicle.facts("crime_restitution")


async def test_low_hp_or_unrelated_state_does_not_count_as_a_defeated_thief(system):
    system.link.eval_values = [0, 0, 0, 1, 1, 0]
    moments = iter([0.0, 0.0, 181.0])
    system.react.clock = lambda: next(moments)
    await system.react._reclaim(crime(system), LOBART, 0)
    assert not system.link.effects


async def test_world_change_cancels_pending_hold_without_writing_into_loaded_branch(system):
    system.link.receive(held())
    await asyncio.sleep(0)
    system.link.receive(system.link.session.model_copy(update={"branch": "B"}))
    await asyncio.gather(*tuple(system.react._tasks), return_exceptions=True)
    assert not system.react.history and not verdicts(system)
