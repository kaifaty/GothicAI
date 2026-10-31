import asyncio
import os
from contextlib import asynccontextmanager
from pathlib import Path
from types import SimpleNamespace

import pytest

from gothic_ai.chronicle import Chronicle
from gothic_ai.dialogue import REACT_HINT, DialogueManager
from gothic_ai.executor import Intent
from gothic_ai.gamedata.storygraph import Node, StoryGraph, load
from gothic_ai.llm import FakeBackend, FakeStep, Message, ToolCall, ToolResult
from gothic_ai.memory import MemoryStore
from gothic_ai.orders import Orders
from gothic_ai.parley import AGREEMENT, EXPIRY_CUE, HAIL_CUE, LEASE_MS, NO_REASON, Parley
from gothic_ai.perception import PerceptionDesk
from gothic_ai.persona import PersonaProvider
from gothic_ai.places import PlaceBook
from gothic_ai.protocol import (
    HeroDeed,
    HoldVerdictCmd,
    Loading,
    NpcEvent,
    NpcRoster,
    NpcView,
    NpcViews,
    Perception,
    PerceptionHeld,
    Session,
    WorldLoaded,
)
from gothic_ai.story import StoryPatches, freed
from gothic_ai.talk import TalkDesk
from tests.fake_game import FakeGame, connected

BRAGO, MATE, OTHER = "BDT_1014_BANDIT_L", "BDT_1015_BANDIT_L", "VLK_414_RUPERT"
COMPANION = "PC_THIEF"
FIGHT = ToolCall("set_intent", {"goal": "fight", "target": "PLAYER", "attitude": "hostile"})


@asynccontextmanager
async def world(script=None):
    fake = FakeGame()
    fake.nearby = [{"id": MATE, "name": "Бандит", "distance": 350, "guild": 9}]
    async with connected(fake) as link:
        store = MemoryStore(":memory:")
        dialogue = DialogueManager(
            link, FakeBackend(script=script, delay=0), PersonaProvider(None), memory=store
        )
        dialogue.guild_of = lambda npc: 9 if npc in {BRAGO, MATE} else 3
        chronicle = Chronicle(link, store)
        perception = PerceptionDesk(link)
        await link.attach(BRAGO)
        await link.snapshot(BRAGO, fresh=True)
        dialogue.open(BRAGO, "Браго", warm=False)
        parley = Parley(
            link,
            dialogue,
            chronicle,
            perception=perception,
            companion=lambda npc: npc == COMPANION,
            lead=lambda npc: 10 if npc == BRAGO else 2,
        )
        dialogue.extensions.append(parley)
        perception._store(
            NpcView(
                npc=BRAGO,
                name="Браго",
                hero={
                    "id": "PLAYER",
                    "name": "Герой",
                    "distance": 100,
                    "seen": True,
                },
            )
        )
        yield fake, link, dialogue, chronicle, parley


async def demand(dialogue, parley, seconds=10, npc=BRAGO):
    conv = dialogue.open(npc, "Браго" if npc == BRAGO else "Бандит", warm=False)
    return await parley.handle(
        dialogue,
        conv,
        ToolCall(
            "ultimatum",
            {
                "demand": "Сто золотых или уходи",
                "seconds": seconds,
            },
        ),
        dialogue.link.snapshots.get(npc),
    )


def armed(parley, weapon):
    view = parley.perception.view(BRAGO)
    parley.perception._store(
        view.model_copy(update={"hero": view.hero.model_copy(update={"weapon": weapon})})
    )


async def test_no_reason_rejects_the_bandit_but_not_a_citizen():
    async with world() as (fake, link, dialogue, _, parley):
        conv = dialogue.conversations[BRAGO]
        result = dialogue._set_intent(conv, FIGHT, link.snapshots[BRAGO])
        assert result.status == "rejected" and NO_REASON in result.output
        citizen = dialogue.open(OTHER, "Горожанин", warm=False)
        assert dialogue._set_intent(citizen, FIGHT, None).status == "accepted"
        assert citizen.intent.goal == "fight"
        assert parley.tools(OTHER) == [] and parley.situation(OTHER) is None
        assert await dialogue._attack_hero(citizen, citizen.intent) == ("Ты напал на героя.", True)
        assert any(
            msg.get("npc") == OTHER and msg.get("effects") == [{"do": "attack", "reason": "AR_NONE"}]
            for msg in fake.story
        )


async def test_rejected_fight_can_be_replaced_with_an_ultimatum_in_the_same_turn():
    script = [
        FakeStep("Стоять!", [FIGHT]),
        FakeStep(
            "Сто золотых или уходи.",
            [
                ToolCall(
                    "ultimatum",
                    {"demand": "Сто золотых или уходи", "seconds": 30},
                )
            ],
        ),
    ]
    async with world(script) as (fake, _, dialogue, _, parley):
        await dialogue.say_as_player(BRAGO, "Вечер в хату")
        assert len(dialogue.llm.calls) == 2
        rejected = next(i for i in dialogue.llm.calls[1][1] if isinstance(i, ToolResult))
        assert NO_REASON in rejected.output
        assert parley.ultimatum(BRAGO)["status"] == "active"
        assert not any(op.get("do") == "attack" for msg in fake.story for op in msg.get("effects", []))


@pytest.mark.parametrize("weapon", ["melee", "ranged", "magic"])
async def test_drawn_weapon_is_a_current_reason(weapon):
    async with world() as (_, link, dialogue, _, parley):
        armed(parley, weapon)
        assert (
            dialogue._set_intent(dialogue.conversations[BRAGO], FIGHT, link.snapshots[BRAGO]).status
            == "accepted"
        )
        armed(parley, "none")
        parley._on_message(
            Perception(type="perception", npc=BRAGO, perc="DRAWWEAPON", other="PLAYER", distance=100)
        )
        assert parley.fight_reason(BRAGO) == NO_REASON


@pytest.mark.parametrize(
    ("perc", "culprit", "victim"),
    [
        ("ASSESSDAMAGE", "PLAYER", None),
        ("ASSESSDAMAGE", COMPANION, None),
        ("ASSESSOTHERSDAMAGE", "PLAYER", MATE),
        ("ASSESSOTHERSDAMAGE", COMPANION, MATE),
        ("ASSESSMURDER", "PLAYER", MATE),
        ("ASSESSMURDER", COMPANION, MATE),
    ],
)
async def test_witnessed_attack_or_murder_by_hero_or_companion_is_a_reason(perc, culprit, victim):
    async with world() as (_, link, dialogue, _, parley):
        if perc == "ASSESSMURDER":
            snap = link.snapshots[BRAGO]
            link.snapshots[BRAGO] = snap.model_copy(
                update={
                    "nearby_npcs": [o.model_copy(update={"dead": True}) for o in snap.nearby_npcs],
                }
            )
        parley._on_message(
            Perception(type="perception", npc=BRAGO, perc=perc, other=culprit, victim=victim, distance=100)
        )
        assert (
            dialogue._set_intent(dialogue.conversations[BRAGO], FIGHT, link.snapshots[BRAGO]).status
            == "accepted"
        )
        link.snapshots[BRAGO].nearby_npcs.clear()
        assert parley.fight_reason(BRAGO) is None  # the witness remembers after the victim has left


async def test_chronicle_requires_a_witness_and_caught_requires_this_npc():
    async with world() as (_, _, _, chronicle, parley):
        chronicle.record({"fact": "died", "npc": MATE, "killer": "PLAYER"})
        chronicle.record({"fact": "caught", "npc": MATE, "quest": "lie"})
        assert parley.fight_reason(BRAGO) == NO_REASON
        chronicle.record({"fact": "crime", "id": "hit", "kind": "attack", "victim": MATE})
        assert parley.fight_reason(BRAGO) == NO_REASON
        chronicle.record({"fact": "crime_seen", "npc": BRAGO, "crime": "hit"})
        assert parley.fight_reason(BRAGO) is None
    async with world() as (_, _, _, chronicle, parley):
        chronicle.record({"fact": "caught", "npc": BRAGO, "quest": "lie"})
        assert parley.fight_reason(BRAGO) is None


async def test_raw_hero_deed_keeps_bandit_self_defence_even_if_crimes_does_not():
    async with world() as (_, _, _, _, parley):
        deed = HeroDeed(
            type="hero_deed",
            id="hit-bandit",
            kind="attack",
            victim=MATE,
            victim_guild=9,
            position={"x": 0, "y": 0, "z": 0},
            witnesses=[BRAGO],
        )
        parley._on_message(deed)
        assert parley.fight_reason(BRAGO) is None
        assert parley.fight_reason(MATE) == NO_REASON


async def test_picking_up_own_dropped_item_does_not_break_a_truce():
    async with world() as (_, _, _, _, parley):
        await parley.accept_deal(BRAGO, 4)
        parley._on_message(
            HeroDeed(
                type="hero_deed",
                id="own-item",
                kind="take_item",
                owner="PLAYER",
                item_dropped=True,
                item="ITMI_GOLD",
                count=1,
                position={"x": 0, "y": 0, "z": 0},
                witnesses=[BRAGO],
            )
        )
        assert parley.fight_reason(BRAGO) == AGREEMENT
        assert parley.truce(MATE)


@pytest.mark.parametrize("seconds", [9, 61, True, 10.5, "10"])
async def test_ultimatum_seconds_are_checked(seconds):
    async with world() as (_, _, dialogue, _, parley):
        assert (await demand(dialogue, parley, seconds)).status == "rejected"
        assert parley.ultimatum(BRAGO) is None


async def test_ultimatum_expires_once_and_gives_a_turn_and_permission():
    async with world() as (_, _, dialogue, _, parley):
        turns = []
        dialogue.event_turn = lambda npc, cue, **kw: turns.append((npc, cue, kw)) or True
        await demand(dialogue, parley)
        assert parley.fight_reason(BRAGO) == NO_REASON
        parley._on_message(NpcViews(type="npc_views", t=11000, views=[]))
        parley.tick()
        parley.tick()
        assert parley.ultimatum(BRAGO)["status"] == "expired"
        assert parley.fight_reason(BRAGO) is None
        assert len(turns) == 1 and turns[0][1] == "[Срок вышел: герой не подчинился]"
        assert turns[0][2]["relevant"]()
        await parley.accept_deal(BRAGO)
        assert not turns[0][2]["relevant"]()


async def test_the_real_expiry_turn_offers_ultimatum_and_can_attack():
    async with world([FakeStep("Сам напросился.", [FIGHT])]) as (fake, _, dialogue, _, parley):
        await demand(dialogue, parley)
        parley._on_message(NpcViews(type="npc_views", t=11000, views=[]))
        parley.tick()
        conv = dialogue.conversations[BRAGO]
        async with asyncio.timeout(3):
            await conv.reply
            await asyncio.gather(*conv.actions)
        assert len(dialogue.llm.calls) == 1
        _, messages, tools = dialogue.llm.calls[0]
        assert {"ultimatum", "set_intent"} <= {tool.name for tool in tools}
        assert any(EXPIRY_CUE in item.content for item in messages if hasattr(item, "content"))
        assert conv.intent.goal == "fight"
        assert any(
            msg.get("npc") == BRAGO
            and msg.get("effects") == [{"do": "attack", "reason": "AR_SUDDENENEMYINFERNO"}]
            for msg in fake.story
        )


async def test_expiry_waits_for_a_busy_turn_instead_of_losing_the_turn():
    async with world() as (_, _, dialogue, _, parley):
        turns = []
        dialogue.event_turn = lambda npc, cue, **kw: turns.append(cue) or True
        await demand(dialogue, parley)
        conv = dialogue.conversations[BRAGO]
        busy = asyncio.create_task(asyncio.Event().wait())
        conv.reply = busy
        parley._on_message(NpcViews(type="npc_views", t=11000, views=[]))
        parley.tick()
        assert not turns
        busy.cancel()
        await asyncio.gather(busy, return_exceptions=True)
        parley.tick()
        parley.tick()
        assert len(turns) == 1


async def test_a_restored_ultimatum_opens_its_npc_for_the_expiry_turn():
    async with world() as (_, _, dialogue, _, parley):
        await demand(dialogue, parley)
        dialogue.conversations.clear()  # a load opens conversations lazily again
        turns = []
        dialogue.event_turn = lambda npc, cue, **kw: turns.append(cue) or True
        parley._on_message(NpcViews(type="npc_views", t=11000, views=[]))
        parley.tick()
        assert BRAGO in dialogue.conversations and len(turns) == 1


async def test_world_change_clears_old_demands_and_resumes_the_new_world_clock():
    async with world() as (_, link, dialogue, _, parley):
        snap, view = link.snapshots[BRAGO], parley.perception.view(BRAGO)
        turns = []
        dialogue.event_turn = lambda npc, cue, **kw: turns.append(cue) or True
        link._dispatch(NpcRoster(type="npc_roster", t=900000, npcs=[]))
        await demand(dialogue, parley)
        assert parley.ultimatum(BRAGO)["deadline"] == 910000
        link._dispatch(Loading(type="loading"))
        link._dispatch(WorldLoaded(type="world_loaded", world="ADDONWORLD.ZEN"))
        assert not parley._loading and parley._ticks == 0
        assert not parley._roster and not parley._roster_complete and parley._minute is None
        assert parley.ultimatum(BRAGO) is None
        link._dispatch(snap.model_copy(update={"t": 100}))
        link._dispatch(NpcViews(type="npc_views", t=100, views=[view]))
        await demand(dialogue, parley)
        assert parley.ultimatum(BRAGO)["deadline"] == 10100
        link._dispatch(NpcViews(type="npc_views", t=10100, views=[view]))
        parley.tick()
        assert parley.ultimatum(BRAGO)["status"] == "expired"
        assert turns == [EXPIRY_CUE]
        assert parley.fight_reason(BRAGO) is None
        await demand(dialogue, parley)
        link._dispatch(
            NpcViews(
                type="npc_views",
                t=10200,
                views=[view.model_copy(update={"hero": view.hero.model_copy(update={"distance": 1501})})],
            )
        )
        parley.tick()
        assert parley.ultimatum(BRAGO) is None


async def test_loading_a_save_preserves_its_ultimatum_across_world_loaded():
    async with world() as (_, link, dialogue, chronicle, parley):
        view = parley.perception.view(BRAGO)
        await demand(dialogue, parley)
        threat = parley.ultimatum(BRAGO)
        turns = []
        dialogue.event_turn = lambda npc, cue, **kw: turns.append(cue) or True
        link._dispatch(Loading(type="loading"))
        link._dispatch(
            Session(
                type="session",
                branch="restored-ultimatum",
                parent=chronicle.branch,
                parent_seq=chronicle.memory.head(chronicle.branch),
                reason="load",
            )
        )
        link._dispatch(WorldLoaded(type="world_loaded", world="NEWWORLD.ZEN"))
        assert parley.ultimatum(BRAGO) == threat
        link._dispatch(NpcViews(type="npc_views", t=threat["deadline"], views=[view]))
        parley.tick()
        assert parley.ultimatum(BRAGO)["status"] == "expired"
        assert turns == [EXPIRY_CUE]


async def test_answer_or_nearby_leaders_ultimatum_allows_fight_but_a_subordinates_does_not():
    async with world([FakeStep("Сам напросился.", [FIGHT])]) as (_, _, dialogue, _, parley):
        await demand(dialogue, parley)
        assert parley.fight_reason(BRAGO) == NO_REASON
        await dialogue.say_as_player(BRAGO, "Сколько?")
        assert parley.fight_reason(BRAGO) is None
        assert dialogue.conversations[BRAGO].intent.goal == "fight"
        # The mate has no snapshot yet, but is present in Brago's snapshot.
        snap = parley.link.snapshots[BRAGO]
        parley.link.snapshots[MATE] = snap.model_copy(
            update={
                "npc": snap.npc.model_copy(update={"id": MATE}),
                "nearby_npcs": [snap.nearby_npcs[0].model_copy(update={"id": BRAGO})],
            }
        )
        assert parley.fight_reason(MATE) is None
        await parley.accept_deal(BRAGO)
        await demand(dialogue, parley, npc=MATE)
        parley.hero_said(MATE)
        assert parley.fight_reason(BRAGO) == NO_REASON


@pytest.mark.parametrize("group", [False, True])
async def test_only_words_addressed_to_the_bandit_answer_his_ultimatum(group):
    async with world() as (_, _, dialogue, _, parley):
        await demand(dialogue, parley, seconds=30)
        if group:
            dialogue.told_all(BRAGO, "Браго", "Ребята, денег нет", "Бандит")
        else:
            dialogue.overheard(BRAGO, "Браго", "Кавалорн, пошли", "Кавалорн")
        assert parley.fight_reason(BRAGO) == (None if group else NO_REASON)


async def test_leaving_beyond_fifteen_metres_or_accepting_a_deal_clears_ultimatum():
    async with world() as (_, _, dialogue, _, parley):
        await demand(dialogue, parley)
        parley._on_message(
            NpcRoster(
                type="npc_roster",
                t=2000,
                npcs=[
                    {
                        "id": BRAGO,
                        "name": "Браго",
                        "guild": 9,
                        "distance": 1501,
                    }
                ],
            )
        )
        parley.tick()
        assert parley.ultimatum(BRAGO) is None
        assert parley.fight_reason(BRAGO) == NO_REASON
        await demand(dialogue, parley)
        await parley.accept_deal(BRAGO)
        assert parley.ultimatum(BRAGO) is None


async def test_a_bandit_missing_from_the_roster_keeps_the_gate_and_loses_his_ultimatum():
    async with world() as (_, _, dialogue, _, parley):
        await demand(dialogue, parley)
        parley.hero_said(BRAGO)
        assert parley.fight_reason(BRAGO) is None
        dialogue.guild_of = lambda npc: None
        parley._on_message(NpcRoster(type="npc_roster", t=2000, npcs=[]))
        assert parley.bandit(BRAGO)
        assert parley.fight_reason(BRAGO) == NO_REASON
        assert parley.ultimatum(BRAGO) is None


async def test_a_known_bandit_keeps_the_gate_after_a_session_before_the_roster():
    async with world() as (_, _, dialogue, chronicle, parley):
        assert parley.bandit(BRAGO)
        parley._on_message(Session(type="session", branch=chronicle.branch))
        dialogue.guild_of = lambda npc: 3 if npc == OTHER else None
        assert parley.fight_reason(BRAGO) == NO_REASON
        assert parley.fight_reason(OTHER) is None


async def test_an_unclassified_npc_keeps_the_original_fight_behavior():
    async with world() as (fake, _, dialogue, _, parley):
        dialogue.guild_of = lambda npc: None
        citizen = dialogue.open(OTHER, "Горожанин", warm=False)
        assert parley.fight_reason(OTHER) is None
        assert dialogue._set_intent(citizen, FIGHT, None).status == "accepted"
        assert await dialogue._attack_hero(citizen, citizen.intent) == ("Ты напал на героя.", True)
        assert any(
            msg.get("npc") == OTHER and msg.get("effects") == [{"do": "attack", "reason": "AR_NONE"}]
            for msg in fake.story
        )


@pytest.mark.parametrize("restart", [False, True])
async def test_truce_facts_keep_the_gate_before_the_roster_after_session_or_restart(restart):
    async with world() as (_, link, dialogue, chronicle, parley):
        await parley.accept_deal(BRAGO, 1)
        until = parley.truce(BRAGO)["until"]
        if restart:
            parley = Parley(link, dialogue, chronicle, perception=parley.perception)
        else:
            parley._on_message(Session(type="session", branch=chronicle.branch))
        dialogue.guild_of = lambda npc: 3 if npc == OTHER else None
        assert parley.fight_reason(BRAGO) == AGREEMENT
        assert parley.fight_reason(MATE) == AGREEMENT
        assert parley.fight_reason(OTHER) is None
        parley._minute = until
        assert parley.fight_reason(BRAGO) == NO_REASON
        assert parley.fight_reason(MATE) == NO_REASON


async def test_a_restored_bandit_truce_waits_for_the_game_clock_before_fighting():
    async with world() as (_, link, dialogue, chronicle, parley):
        chronicle.record({"fact": "caught", "npc": BRAGO})
        await parley.accept_deal(BRAGO, 1)
        until = parley.truce(BRAGO)["until"]
        link.snapshots.clear()
        dialogue.guild_of = lambda npc: None
        restored = Parley(link, dialogue, chronicle, perception=parley.perception)
        assert restored.fight_reason(BRAGO) == AGREEMENT
        assert restored.fight_reason(MATE) == AGREEMENT
        restored._minute = until - 1
        assert restored.fight_reason(BRAGO) == AGREEMENT
        assert restored.fight_reason(MATE) == AGREEMENT
        restored._minute = until
        assert restored.fight_reason(BRAGO) is None
        assert restored.fight_reason(MATE) == NO_REASON


async def test_truce_covers_the_band_and_ignores_old_violence_lies_and_ultimatums():
    async with world() as (_, _, dialogue, chronicle, parley):
        parley._on_message(
            Perception(type="perception", npc=BRAGO, perc="ASSESSDAMAGE", other="PLAYER", distance=100)
        )
        chronicle.record({"fact": "caught", "npc": BRAGO, "quest": "lie"})
        await demand(dialogue, parley)
        parley.hero_said(BRAGO)
        await parley.accept_deal(BRAGO, 4)
        assert parley.ultimatum(BRAGO) is None
        assert parley.fight_reason(BRAGO) == AGREEMENT
        assert parley.fight_reason(MATE) == AGREEMENT
        chronicle.record({"fact": "caught", "npc": BRAGO, "quest": "new-lie"})
        assert parley.fight_reason(BRAGO) == AGREEMENT
        assert "уговор с героем до дня" in parley.situation(BRAGO)


async def test_truce_covers_bandits_at_the_same_place_even_beyond_fifteen_metres():
    async with world() as (_, link, _, _, parley):
        link.snapshots[BRAGO].nearby_npcs.clear()
        parley.places = PlaceBook(
            {
                "points": {"CAVE_ENTRANCE": 0, "CAVE_REAR": 0},
                "places": [
                    {
                        "id": "CAVE",
                        "ru": "пещера",
                        "where_ru": "в пещере",
                        "en": "cave",
                        "where_en": "in the cave",
                    }
                ],
            }
        )
        view = parley.perception.view(BRAGO)
        parley.perception._store(view.model_copy(update={"waypoint": "CAVE_ENTRANCE"}))
        parley.perception._store(
            NpcView(
                npc=MATE,
                name="Бандит",
                waypoint="CAVE_REAR",
                hero={
                    "id": "PLAYER",
                    "name": "Герой",
                    "distance": 3000,
                    "seen": True,
                },
            )
        )
        parley.perception._store(NpcView(npc=OTHER, name="Горожанин", waypoint="CAVE_REAR"))
        assert parley.members(BRAGO) == sorted([BRAGO, MATE])
        await parley.accept_deal(BRAGO, 1)
        assert parley.fight_reason(MATE) == AGREEMENT
        assert parley.fight_reason(OTHER) is None


@pytest.mark.parametrize("kind", ["attack", "murder", "theft", "weapon"])
async def test_new_cause_breaks_truce_for_the_whole_band(kind):
    async with world() as (_, _, _, _, parley):
        await parley.accept_deal(BRAGO, 4)
        parley._on_message(
            NpcEvent(
                type="npc_event",
                npc=BRAGO,
                kind={
                    "attack": "attack",
                    "murder": "murder",
                    "theft": "theft",
                    "weapon": "weapon_drawn",
                }[kind],
                other="PLAYER",
                detail="you",
            )
        )
        assert parley.fight_reason(MATE) is None  # no prior gate/lookup for Brago is needed
        assert parley.truce(BRAGO) is None and parley.truce(MATE) is None
        assert parley.fight_reason(BRAGO) is None


async def test_theft_is_a_reason_after_breaking_a_truce_and_does_not_grant_one_before():
    async with world() as (_, _, _, _, parley):
        event = NpcEvent(type="npc_event", npc=BRAGO, kind="theft", other="PLAYER")
        parley._on_message(event)
        assert parley.fight_reason(BRAGO) == NO_REASON
        await parley.accept_deal(BRAGO, 1)
        assert parley.fight_reason(BRAGO) == AGREEMENT
        parley._on_message(event)
        assert parley.fight_reason(BRAGO) is None
        assert parley.fight_reason(BRAGO) is None


@pytest.mark.parametrize("kind", ["weapon_drawn", "theft"])
@pytest.mark.parametrize("renew", [False, True])
async def test_broken_truce_reason_ends_with_its_term_or_a_later_truce(kind, renew):
    async with world() as (_, _, _, _, parley):
        await parley.accept_deal(BRAGO, 4)
        now = parley.now()
        parley._on_message(NpcEvent(type="npc_event", npc=BRAGO, kind=kind, other="PLAYER"))
        assert parley.fight_reason(BRAGO) is None
        assert parley.fight_reason(MATE) is None
        if renew:
            await parley.accept_deal(BRAGO, 1)
            assert parley.fight_reason(BRAGO) == AGREEMENT
            assert parley.fight_reason(MATE) == AGREEMENT
            parley._minute = now + 60
        else:
            parley._minute = now + 3 * 1440
        assert parley.fight_reason(BRAGO) == NO_REASON
        assert parley.fight_reason(MATE) == NO_REASON
        assert "Сначала давишь словами" in parley.situation(BRAGO)


async def test_truce_ends_on_the_game_clock_and_the_save_branch_rolls_it_back():
    async with world() as (_, link, _, chronicle, parley):
        before = chronicle.memory.head(chronicle.branch)
        await parley.accept_deal(BRAGO, 1)
        assert parley.truce(BRAGO)
        parley._minute = 9 * 60
        assert parley.truce(BRAGO) is None
        assert parley.fight_reason(BRAGO) == NO_REASON
        session = Session(type="session", branch="before-truce", parent=chronicle.branch, parent_seq=before)
        chronicle._on_message(session)
        parley._on_message(session)
        assert parley.truce(BRAGO) is None
        assert link.snapshots[BRAGO].world.hour == 8


async def test_a_deferred_attack_rechecks_a_deal_accepted_while_the_npc_spoke():
    async with world() as (fake, _, dialogue, _, parley):
        conv = dialogue.conversations[BRAGO]
        fight = Intent("fight", "PLAYER", "hostile")
        conv.intent = fight
        await parley.accept_deal(BRAGO, 4)
        conv.intent = fight  # recheck the side effect even if a stale intent survives another callback
        note, ok = await dialogue._attack_hero(conv, fight)
        assert not ok and "У вас уговор" in note
        assert conv.intent.goal != "fight"
        assert not any(op.get("do") == "attack" for msg in fake.story for op in msg.get("effects", []))


@pytest.mark.parametrize("native", [False, True])
async def test_truce_immediately_ends_its_own_or_a_band_members_native_fight(native):
    async with world() as (fake, _, dialogue, _, parley):
        target = MATE if native else BRAGO
        if native:
            dialogue.fights_hero = lambda npc: npc == MATE
            assert MATE not in dialogue.conversations
        else:
            conv = dialogue.conversations[BRAGO]
            conv.intent = Intent("fight", "PLAYER", "hostile")
            conv.voice = asyncio.Queue()
            conv.voice.put_nowait(None)  # an unfinished line must not hold up the accepted peace
        await asyncio.wait_for(parley.accept_deal(BRAGO, 4), 1)
        assert any(
            msg["npc"] == target and op.get("do") == "calm"
            for msg in fake.story
            for op in msg.get("effects", [])
        )
        assert dialogue.conversations[target].intent.goal == "none"
        assert parley.fight_reason(BRAGO) == AGREEMENT
        assert parley.fight_reason(MATE) == AGREEMENT


async def test_balance_uses_visible_equipment_health_and_people_not_levels():
    async with world() as (_, link, _, _, parley):
        line = parley.situation(BRAGO)
        assert "он слабее вас" in line and "вас на ногах 2" in line
        assert "Сначала давишь словами" in line
        snap = link.snapshots[BRAGO]
        link.snapshots[BRAGO] = snap.model_copy(
            update={
                "player": snap.player.model_copy(update={"armor": "ITAR_PAL_H"}),
                "nearby_npcs": [],
            }
        )
        assert "он опасен" in parley.situation(BRAGO)
        assert "уров" not in parley.situation(BRAGO).lower()


def held(*, npc=BRAGO, other="PLAYER", kind="bandit_enemy", key="bandit-hold"):
    return PerceptionHeld(
        type="perception_held", hold=key, npc=npc, other=other, kind=kind, distance=100,
    )


def verdicts(monkeypatch, link):
    sent = []
    original = link.send

    async def send(command):
        if isinstance(command, HoldVerdictCmd):
            sent.append(command)
        await original(command)

    monkeypatch.setattr(link, "send", send)
    return sent


async def finish_hails(parley):
    async with asyncio.timeout(2):
        while parley._background:
            await asyncio.gather(*parley._background)


def hail_body(monkeypatch, dialogue, parley):
    approaches, turns = [], []
    parley.bodies = SimpleNamespace(busy=lambda npc: False)
    begun = []
    parley.talk = SimpleNamespace(
        partner=None, begun=begun, begin=lambda npc, name: begun.append(npc) or True,
        people={BRAGO: SimpleNamespace(activity="routine", state="ZS_STAND")},
    )
    monkeypatch.setattr(dialogue, "body_busy", lambda npc: False)

    async def approach(link, bodies, talk, npc, *, valid):
        assert valid()
        approaches.append(npc)
        return True

    monkeypatch.setattr("gothic_ai.initiative.approach", approach)
    monkeypatch.setattr(
        dialogue, "event_turn", lambda npc, cue, **kw: turns.append((npc, cue, kw)) or True,
    )
    return approaches, turns


@pytest.mark.parametrize("other", ["PLAYER", COMPANION])
async def test_held_truce_silently_leases_sightings_of_hero_and_companion(monkeypatch, other):
    async with world() as (_, link, dialogue, _, parley):
        _, turns = hail_body(monkeypatch, dialogue, parley)
        sent = verdicts(monkeypatch, link)
        await parley.accept_deal(BRAGO, 4)
        await parley._held(held(other=other), parley._generation)
        await finish_hails(parley)
        assert [(s.to, s.ms) for s in sent] == [("parley", LEASE_MS)]
        assert not turns and not dialogue.llm.calls


@pytest.mark.parametrize("npc", [BRAGO, "BDT_10300_RANGERBANDIT"])
@pytest.mark.parametrize("other", ["PLAYER", COMPANION])
async def test_cause_attacks_player_immediately_for_overridden_and_native_bandits(monkeypatch, npc, other):
    async with world() as (fake, link, dialogue, chronicle, parley):
        # Brago's native AIV_EnemyOverride suppresses B_AssessEnemy; road bandits use that path normally.
        dialogue.guild_of = lambda who: 9
        chronicle.record({"fact": "caught", "npc": npc, "quest": "lie"})
        sent = verdicts(monkeypatch, link)
        started = asyncio.get_running_loop().time()
        await parley._held(held(npc=npc, other=other), parley._generation)
        elapsed = asyncio.get_running_loop().time() - started
        assert elapsed < 2
        assert [(s.to, s.ms) for s in sent] == [("scripts", None)]
        assert (dialogue.conversations[npc].intent.goal, dialogue.conversations[npc].intent.target) == (
            "fight", "PLAYER",
        )
        assert any(
            msg.get("npc") == npc
            and msg.get("effects") == [{"do": "attack", "reason": "AR_SUDDENENEMYINFERNO"}]
            for msg in fake.story
        )
        assert not dialogue.llm.calls


@pytest.mark.parametrize(("membership", "cause"), [
    ("orders", "weapon"), ("orders", "attack"), ("party", "weapon"),
])
async def test_bandit_needs_a_cause_to_fight_a_companion_and_keeps_the_chosen_target(
    monkeypatch, membership, cause,
):
    async with world() as (fake, link, dialogue, _, parley):
        fake.nearby.append({"id": COMPANION, "name": "Диего", "distance": 200, "guild": 3})
        snap = await link.snapshot(BRAGO, fresh=True)
        orders = Orders(link, TalkDesk(link, dialogue), dialogue)
        companion = dialogue.open(COMPANION, "Диего", warm=False)
        companion.intent = Intent("follow_hero", "PLAYER", "friendly")
        parley.companion = orders.companion if membership == "orders" else lambda npc: False
        if membership == "party":
            link.party[COMPANION] = (1, 1)
        conv = dialogue.conversations[BRAGO]
        attack = ToolCall("set_intent", {"goal": "fight", "target": COMPANION.lower(), "attitude": "hostile"})
        result = dialogue._set_intent(conv, attack, snap)
        assert result.status == "rejected" and NO_REASON in result.output
        assert conv.intent.goal != "fight"
        citizen = dialogue.open(OTHER, "Горожанин", warm=False)
        assert dialogue._set_intent(citizen, attack, snap).status == "accepted"

        sent = verdicts(monkeypatch, link)
        await parley._held(held(other=COMPANION), parley._generation)
        await finish_hails(parley)
        assert [(s.to, s.ms) for s in sent] == [("parley", LEASE_MS)]
        if cause == "weapon":
            armed(parley, "melee")
        else:
            parley._on_message(Perception(
                type="perception", npc=BRAGO, perc="ASSESSDAMAGE", other=COMPANION, distance=100,
            ))
        assert dialogue._set_intent(conv, attack, snap).status == "accepted"
        intent = conv.intent
        await parley._held(held(other=COMPANION, key="chosen-target"), parley._generation)
        assert sent[-1].to == "scripts" and sent[-1].ms is None
        assert conv.intent is intent and intent.target == COMPANION
        assert not any(op.get("do") == "attack" for msg in fake.story for op in msg.get("effects", []))


async def test_no_cause_leases_before_a_slow_approach_and_hails_the_leader_once(monkeypatch):
    async with world() as (_, link, dialogue, chronicle, parley):
        approaches, turns = hail_body(monkeypatch, dialogue, parley)
        sent = verdicts(monkeypatch, link)
        arrived = asyncio.Event()
        approaching = asyncio.Event()

        async def approach(link, bodies, talk, npc, *, valid):
            assert valid() and sent[0].to == "parley"
            approaches.append(npc)
            approaching.set()
            await arrived.wait()
            return True

        monkeypatch.setattr("gothic_ai.initiative.approach", approach)
        parley._on_message(held())
        async with asyncio.timeout(1):
            await approaching.wait()
        assert sent[0].to == "parley" and not turns
        await parley._held(held(key="again"), parley._generation)
        arrived.set()
        await finish_hails(parley)
        await parley._held(held(npc=MATE, key="mate"), parley._generation)
        await finish_hails(parley)
        assert approaches == [BRAGO]
        assert parley.talk.begun == [BRAGO]
        assert len(turns) == 1 and turns[0][:2] == (BRAGO, HAIL_CUE)
        assert turns[0][2]["tools"] == {"ultimatum", "propose_deal", "set_intent", "recall"}
        assert turns[0][2]["relevant"]()
        assert parley._encounters()[0]["hailed"]
        assert parley._encounters()[0]["members"] == sorted([BRAGO, MATE])
        assert len(chronicle.facts("parley_encounter")) == 3


@pytest.mark.parametrize("arrived", [False, True])
async def test_hail_renewals_do_not_repeat_an_attempt_or_snapshot(monkeypatch, arrived):
    async with world() as (_, link, dialogue, chronicle, parley):
        approaches, turns = hail_body(monkeypatch, dialogue, parley)
        snapshots = []
        original = link.snapshot

        async def snapshot(npc, **kw):
            snapshots.append(npc)
            return await original(npc, **kw)

        async def approach(link, bodies, talk, npc, *, valid):
            assert valid()
            approaches.append(npc)
            return arrived

        monkeypatch.setattr(link, "snapshot", snapshot)
        monkeypatch.setattr("gothic_ai.initiative.approach", approach)
        for i in range(5):
            parley._ticks += LEASE_MS + 1
            await parley._held(held(npc=MATE if i % 2 else BRAGO, key=str(i)), parley._generation)
            await finish_hails(parley)
        assert approaches == [BRAGO] and len(snapshots) == 1
        assert len(turns) == int(arrived)
        assert parley._encounters()[0]["hail_attempted"]
        assert len(chronicle.facts("parley_encounter")) == 2 + int(arrived)


@pytest.mark.parametrize("busy", ["dialog", "cutscene", "missing", "partner", "body"])
async def test_native_busy_preflight_does_not_consume_the_hail_attempt(monkeypatch, busy):
    async with world() as (fake, _, dialogue, _, parley):
        approaches, turns = hail_body(monkeypatch, dialogue, parley)
        if busy == "dialog":
            fake.in_dialog = True
        elif busy == "cutscene":
            fake.in_cutscene = True
        elif busy == "missing":
            parley.talk.people.clear()
        elif busy == "partner":
            parley.talk.partner = SimpleNamespace(npc=OTHER)
        else:
            parley.bodies.busy = lambda npc: True
        await parley._held(held(), parley._generation)
        await finish_hails(parley)
        assert not approaches and not turns
        assert not parley._encounters()[0].get("hail_attempted")
        fake.in_dialog, fake.in_cutscene = False, False
        parley.talk.people[BRAGO] = SimpleNamespace(activity="routine", state="ZS_STAND")
        parley.talk.partner = None
        parley.bodies.busy = lambda npc: False
        await parley._held(held(key="eligible"), parley._generation)
        await finish_hails(parley)
        assert approaches == [BRAGO] and len(turns) == 1
        assert parley._encounters()[0]["hail_attempted"]


async def test_hail_preserves_members_added_during_approach(monkeypatch):
    async with world() as (_, link, dialogue, _, parley):
        _, turns = hail_body(monkeypatch, dialogue, parley)
        approaching, arrived = asyncio.Event(), asyncio.Event()

        async def approach(link, bodies, talk, npc, *, valid):
            approaching.set()
            await arrived.wait()
            return True

        monkeypatch.setattr("gothic_ai.initiative.approach", approach)
        await parley._held(held(), parley._generation)
        async with asyncio.timeout(1):
            await approaching.wait()
        third = "BDT_1013_BANDIT"
        dialogue.guild_of = lambda npc: 9
        snap = link.snapshots[BRAGO]
        snap.nearby_npcs.append(snap.nearby_npcs[0].model_copy(update={"id": third}))
        widened = parley._encounter(BRAGO)
        assert third in widened["members"]
        arrived.set()
        await finish_hails(parley)
        assert parley._encounters()[0]["members"] == sorted([BRAGO, MATE, third])
        await parley._held(held(npc=third, key="third"), parley._generation)
        await finish_hails(parley)
        assert len(turns) == 1


@pytest.mark.parametrize("busy", ["sleep", "fight", "reply", "partner"])
async def test_hail_waits_when_the_leader_is_busy_or_hero_talks_to_the_band(monkeypatch, busy):
    async with world() as (_, link, dialogue, _, parley):
        approaches, turns = hail_body(monkeypatch, dialogue, parley)
        row = {
            "id": BRAGO, "name": "Браго", "guild": 9, "distance": 100,
            "state": "ZS_SLEEP" if busy == "sleep" else "ZS_STAND", "activity": "routine",
        }
        if busy == "fight":
            row["activity"] = "fight"
        elif busy == "reply":
            dialogue.conversations[BRAGO].reply = asyncio.create_task(asyncio.Event().wait())
        elif busy == "partner":
            parley.talk.partner = SimpleNamespace(npc=MATE)
        parley._on_message(NpcRoster(type="npc_roster", t=1000, npcs=[row]))
        await parley._held(held(), parley._generation)
        await finish_hails(parley)
        assert not approaches and not turns
        reply = dialogue.conversations[BRAGO].reply
        if reply is not None:
            reply.cancel()
            await asyncio.gather(reply, return_exceptions=True)
        parley.talk.partner = None
        parley._on_message(NpcRoster(type="npc_roster", t=2000, npcs=[{
            **row, "state": "ZS_STAND", "activity": "routine",
        }]))
        await parley._held(held(key="ready"), parley._generation)
        await finish_hails(parley)
        assert approaches == [BRAGO] and len(turns) == 1


async def test_encounter_boundary_oscillation_does_not_grow_the_chronicle():
    async with world() as (_, _, _, chronicle, parley):
        original = parley._encounter(BRAGO)["id"]
        for i in range(100):
            parley._on_message(NpcRoster(type="npc_roster", t=1000 + i * 250, npcs=[
                {"id": npc, "name": "Бандит", "guild": 9, "distance": 2990 if i % 2 else 3010}
                for npc in (BRAGO, MATE)
            ]))
            parley.tick()
        assert parley._encounters()[0]["id"] == original
        assert len(chronicle.facts("parley_encounter")) == 1


async def test_leaving_all_members_for_sixty_continuous_seconds_starts_a_new_encounter(monkeypatch):
    async with world() as (_, link, dialogue, _, parley):
        _, turns = hail_body(monkeypatch, dialogue, parley)
        await parley._held(held(), parley._generation)
        await finish_hails(parley)
        original = parley._encounters()[0]["id"]

        def roster(t, brago=3100, mate=3100):
            parley._on_message(NpcRoster(type="npc_roster", t=t, npcs=[
                {"id": npc, "name": "Бандит", "guild": 9, "distance": distance}
                for npc, distance in ((BRAGO, brago), (MATE, mate))
            ]))
            parley.tick()

        roster(2000)
        roster(61000, mate=3000)  # one member is still within 30 m: restart the timer
        roster(62000)
        roster(121999)
        assert parley._encounters()[0]["id"] == original
        roster(122000)
        assert not parley._encounters()
        roster(123000, brago=100, mate=200)
        await parley._held(held(key="new-meeting"), parley._generation)
        await finish_hails(parley)
        assert len(turns) == 2 and parley._encounters()[0]["id"] != original


async def test_deal_keeps_the_encounter_band_after_its_leader_steps_away():
    async with world() as (_, link, _, chronicle, parley):
        encounter = parley._encounter(BRAGO)
        assert encounter["members"] == sorted([BRAGO, MATE])
        # The greeting moved Brago: the farthest member is now beyond his snapshot's 15 m radius.
        link.snapshots[BRAGO].nearby_npcs.clear()
        assert parley.members(BRAGO) == sorted([BRAGO, MATE])
        assert parley.members(MATE) == sorted([BRAGO, MATE])
        await parley.accept_deal(BRAGO, 4)
        assert chronicle.facts("truce")[-1]["members"] == sorted([BRAGO, MATE])
        assert parley.truce(MATE) is not None
        parley.chronicle.record({**encounter, "status": "closed", "why": "left"})
        assert parley.members(BRAGO) == [BRAGO]


async def test_encounter_hail_rolls_back_with_the_save_branch_and_world_change_ends_it(monkeypatch):
    async with world() as (_, link, dialogue, chronicle, parley):
        _, turns = hail_body(monkeypatch, dialogue, parley)
        before = chronicle.memory.head(chronicle.branch)
        await parley._held(held(), parley._generation)
        await finish_hails(parley)
        parent, head = chronicle.branch, chronicle.memory.head(chronicle.branch)
        parley._on_message(Loading(type="loading"))
        link._dispatch(Session(type="session", branch="after-hail", parent=parent, parent_seq=head))
        link._dispatch(WorldLoaded(type="world_loaded", world="NEWWORLD.ZEN"))
        await parley._held(held(key="after-load"), parley._generation)
        await finish_hails(parley)
        assert len(turns) == 1 and parley._encounters()[0]["hailed"]
        link._dispatch(Session(type="session", branch="before-hail", parent=parent, parent_seq=before))
        assert not parley._encounters()
        await parley._held(held(key="replayed"), parley._generation)
        await finish_hails(parley)
        assert len(turns) == 2
        parley._on_message(Loading(type="loading"))
        parley._on_message(WorldLoaded(type="world_loaded", world="ADDONWORLD.ZEN"))
        assert not parley._encounters() and not turns[-1][2]["relevant"]()


@pytest.mark.parametrize("number", [1040, 1049, 1060, 1068])
async def test_dexter_camp_sightings_return_to_the_scripts(monkeypatch, number):
    async with world() as (_, link, dialogue, _, parley):
        npc = f"BDT_{number}_BANDIT"
        dialogue.guild_of = lambda who: 9
        sent = verdicts(monkeypatch, link)
        await parley._held(held(npc=npc), parley._generation)
        assert sent[0].to == "scripts" and parley.fight_reason(npc) is None
        assert npc not in dialogue.conversations


async def test_important_dialog_stays_native_until_story_patches_disable_it(monkeypatch):
    async with world() as (_, link, dialogue, chronicle, parley):
        node = Node("DIA_BANDIT_WARN", "dialog", BRAGO, "", [], set(), important=True)
        parley.graph = StoryGraph({node.id: node}, {}, {}, {}, {})
        parley.patches = StoryPatches(link, chronicle.memory)
        sent = verdicts(monkeypatch, link)
        await parley._held(held(), parley._generation)
        assert sent[-1].to == "scripts" and parley.fight_reason(BRAGO) is None
        await parley.patches.apply({"op": "disable", "node": node.id})
        await parley._held(held(key="freed"), parley._generation)
        await finish_hails(parley)
        assert sent[-1].to == "parley" and parley.fight_reason(BRAGO) == NO_REASON
        await parley.patches.apply({"op": "enable", "node": node.id})
        await parley._held(held(key="native-again"), parley._generation)
        assert sent[-1].to == "scripts"


@pytest.fixture(scope="module")
def real_graph():
    data = Path(os.environ.get("GOTHIC_AI_DATA", Path.home() / ".local/share/gothic-ai"))
    path = Path(os.environ.get("GOTHIC_STORY_GRAPH", data / "story.json"))
    if not path.exists():
        pytest.skip("needs actual storygraph export (GOTHIC_STORY_GRAPH)")
    return load(path)


@pytest.mark.parametrize("npc", ["SLD_840_ALVARES", "SLD_841_ENGARDO"])
async def test_freed_akil_farm_bandits_become_negotiable(monkeypatch, real_graph, npc):
    async with world() as (_, link, dialogue, chronicle, parley):
        dialogue.guild_of = lambda who: 9
        parley.graph = real_graph
        assert any(n.important for n in parley.graph.dialogs_of(npc))
        sent = verdicts(monkeypatch, link)
        await parley._held(held(npc=npc), parley._generation)
        assert sent[-1].to == "scripts" and not parley.negotiable(npc)
        parley.patches = StoryPatches(
            link, chronicle.memory, static=freed(parley.graph.nodes.__contains__),
        )
        await parley._held(held(npc=npc, key="freed"), parley._generation)
        await finish_hails(parley)
        assert sent[-1].to == "parley" and parley.negotiable(npc)


async def test_party_enemy_leases_a_peaceful_gang_and_releases_a_provoked_gang(monkeypatch):
    async with world() as (_, link, _, _, parley):
        sent = verdicts(monkeypatch, link)
        signal = held(npc=COMPANION, other=BRAGO, kind="party_enemy")
        await parley._held(signal, parley._generation)
        assert sent[-1].to == "parley"
        await parley.accept_deal(BRAGO, 4)
        await parley._held(signal, parley._generation)
        assert sent[-1].to == "parley"
        armed(parley, "melee")
        await parley._held(signal, parley._generation)
        assert sent[-1].to == "scripts"


@pytest.mark.parametrize("pending", ["reply", "offer"])
async def test_answered_ultimatum_does_not_interrupt_a_slow_reply_or_deal_window(monkeypatch, pending):
    async with world() as (fake, link, dialogue, _, parley):
        sent = verdicts(monkeypatch, link)
        await demand(dialogue, parley)
        parley.hero_said(BRAGO)
        assert parley.fight_reason(BRAGO) is None
        reply = asyncio.create_task(asyncio.Event().wait())
        if pending == "reply":
            dialogue.conversations[BRAGO].reply = reply
        else:
            parley.offer_pending = lambda npc: npc == BRAGO
        await parley._held(held(), parley._generation)
        assert sent[-1].to == "parley" and not fake.story
        armed(parley, "melee")
        await parley._held(held(key="weapon"), parley._generation)
        assert sent[-1].to == "scripts"
        assert any(op.get("do") == "attack" for msg in fake.story for op in msg.get("effects", []))
        reply.cancel()
        await asyncio.gather(reply, return_exceptions=True)


@pytest.mark.parametrize("kind", ["bandit_enemy", "party_enemy"])
async def test_answered_ultimatum_keeps_silence_without_a_talk_partner_until_expiry(monkeypatch, kind):
    async with world() as (fake, link, dialogue, _, parley):
        monkeypatch.setattr(dialogue, "event_turn", lambda *a, **kw: None)
        sent = verdicts(monkeypatch, link)
        await demand(dialogue, parley)
        parley.hero_said(BRAGO)
        assert parley.talk is None and parley.fight_reason(BRAGO) is None
        signal = held() if kind == "bandit_enemy" else held(npc=COMPANION, other=BRAGO, kind=kind)
        parley._ticks = parley.ultimatum(BRAGO)["deadline"] - 1
        await parley._held(signal, parley._generation)
        assert sent[-1].to == "parley" and not fake.story
        parley._ticks += 1
        await parley._held(signal, parley._generation)
        assert parley.ultimatum(BRAGO)["status"] == "expired"
        assert sent[-1].to == "scripts"
        if kind == "bandit_enemy":
            assert any(op.get("do") == "attack" for msg in fake.story for op in msg.get("effects", []))


async def test_companion_model_waits_for_negotiations_but_a_hero_order_does_not(monkeypatch):
    async with world() as (fake, link, dialogue, _, parley):
        fake.nearby = [{"id": BRAGO, "name": "Браго", "distance": 100, "guild": 9}]
        await link.attach(COMPANION)
        snap = await link.snapshot(COMPANION, fresh=True)
        talk = TalkDesk(link, dialogue)
        orders = Orders(link, talk, dialogue)
        orders.parley_quiet = parley.quiet
        assert dialogue.fight_reason == parley.fight_reason
        conv = dialogue.open(COMPANION, "Диего", warm=False)
        conv.intent = Intent("follow_hero", "PLAYER", "friendly")
        starts = []
        monkeypatch.setattr(orders, "_start", lambda npc, target, standing: starts.append((npc, target)))
        monkeypatch.setattr(dialogue, "event_turn", lambda *a, **kw: None)
        attack = ToolCall("set_intent", {"goal": "fight", "target": BRAGO, "attitude": "friendly"})

        await demand(dialogue, parley)
        parley.hero_said(BRAGO)
        assert parley.fight_reason(BRAGO) is None and parley.quiet(BRAGO)
        result = dialogue._set_intent(conv, attack, snap)
        assert result.status == "rejected" and "Нужен приказ героя" in result.output
        assert conv.intent.goal == "follow_hero" and not starts
        assert orders.fight_intent(COMPANION, BRAGO)
        assert starts == [(COMPANION, BRAGO)]

        # A new, unanswered demand expires: the model follows the usual fight rules again.
        await demand(dialogue, parley)
        parley._ticks = parley.ultimatum(BRAGO)["deadline"]
        parley.tick()
        assert not parley.ultimatum(BRAGO).get("answered") and not parley.quiet(BRAGO)
        assert dialogue._set_intent(conv, attack, snap).status == "accepted"
        assert starts == [(COMPANION, BRAGO), (COMPANION, BRAGO)]


async def test_expired_ultimatum_attacks_even_with_a_lingering_talk_partner(monkeypatch):
    async with world() as (fake, link, dialogue, _, parley):
        parley.talk = SimpleNamespace(partner=SimpleNamespace(npc=BRAGO))
        monkeypatch.setattr(dialogue, "event_turn", lambda *a, **kw: None)
        await demand(dialogue, parley)
        parley._ticks = 11000
        sent = verdicts(monkeypatch, link)
        await parley._held(held(), parley._generation)
        assert parley.ultimatum(BRAGO)["status"] == "expired"
        assert sent[-1].to == "scripts"
        assert any(op.get("do") == "attack" for msg in fake.story for op in msg.get("effects", []))


async def test_quick_summaries_include_the_same_balance_without_waiting_for_extensions(monkeypatch):
    async with world() as (_, link, dialogue, _, parley):
        conv = dialogue.conversations[BRAGO]
        conv.intent = Intent(attitude="wary")
        quick = FakeBackend("Твоё невежество граничит с безумием.", delay=0)
        dialogue.quick = quick

        async def no_preparation(npc):
            raise AssertionError("the quick summary must not prepare extensions")

        parley.prepare = no_preparation
        brief, _, _ = await dialogue._situation(conv, None, brief=True)
        balance = parley.balance(BRAGO)
        assert balance and balance in brief and balance in parley.situation(BRAGO)
        assert conv.intent.describe() in brief
        assert balance in dialogue._brief(conv)
        request = [
            *conv.history, Message("developer", brief + "\n" + REACT_HINT), Message("user", "Я убью вас!")
        ]
        await dialogue._first_reaction(conv, "Я убью вас!", request)
        assert balance in quick.calls[0][1][-2].content
        assert quick.temperatures == [0.6]
        assert parley.balance(OTHER) is None

        dialogue.guild_of = lambda npc: None
        parley._guilds.clear()

        def no_facts(*args):
            raise AssertionError("the quick summary must not read the database")

        monkeypatch.setattr(parley.chronicle, "facts", no_facts)
        assert parley.balance(BRAGO) == balance
        assert parley.balance(OTHER) is None
        parley._on_message(Session(type="session", branch="another"))
        assert parley.balance(BRAGO) is None
