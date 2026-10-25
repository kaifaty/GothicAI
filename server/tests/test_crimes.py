from __future__ import annotations

import asyncio
from types import SimpleNamespace
from typing import Any

import pytest

from gothic_ai.characters import Card
from gothic_ai.chronicle import Chronicle
from gothic_ai.commands import Order
from gothic_ai.crime_law import CrimeLaw
from gothic_ai.crime_sides import FIGHT_MEMORY_SECONDS
from gothic_ai.crimes import COMMUNITIES, Crimes, pickpocket_outcome
from gothic_ai.dialogue import DialogueManager
from gothic_ai.executor import Intent
from gothic_ai.fear import afraid
from gothic_ai.gamedata.storygraph import Node, StoryGraph
from gothic_ai.llm.fake import FakeBackend
from gothic_ai.memory import MemoryStore
from gothic_ai.orders import Orders
from gothic_ai.persona import PersonaProvider
from gothic_ai.protocol import HeroDeed, Loading, NpcEvent, NpcViews, Perception, Session
from gothic_ai.questbook import Goal, Progress, Quest, Stage, Talk

LOBART = "BAU_950_LOBART"
HILDA = "BAU_951_HILDA"
LEE = "SLD_800_LEE"


@pytest.mark.parametrize("status", ["pending", "spared", "attacked"])
async def test_surrendered_murder_has_no_fight_excuse_and_costs_more(system, status):
    crimes, chronicle = system.crimes, system.chronicle
    chronicle.record({"fact": "surrender", "npc": LOBART, "until": 5 * 1440, "status": status})
    crimes.sides.note_fight(HILDA, LOBART)
    crime = crimes.record(deed(
        kind="murder", victim=LOBART, victim_guild=8, victim_enemy=True, victim_started_fight=True,
        witnesses=[HILDA], witnesses_metadata=[{"npc": HILDA, "guild": 8}],
    ))
    assert crime["surrendered"] and "lawful_fight" not in crime
    assert crimes.side(HILDA, crime["id"]) is None
    assert crimes.sides.heard_reason(HILDA, crime) is None
    assert "убил того, кто сдался" in crimes.situation(HILDA)
    chronicle.record({"fact": "crime_known", "community": "farm", "crime": crime["id"]})
    assert CrimeLaw(crimes).debt("farm") == 800


@pytest.mark.parametrize("surrender,enabled", [(False, True), (True, False), ("expired", True)])
async def test_ordinary_murder_stays_as_before_mercy(system, surrender, enabled):
    crimes, chronicle = system.crimes, system.chronicle
    crimes.mercy = enabled
    if surrender:
        chronicle.record({"fact": "surrender", "npc": LOBART,
                          "until": 3 * 1440 if surrender == "expired" else 5 * 1440, "status": "spared"})
    crimes.sides.note_fight(HILDA, LOBART)
    crime = crimes.record(deed(
        kind="murder", victim=LOBART, victim_guild=8, victim_enemy=True,
        witnesses=[HILDA], witnesses_metadata=[{"npc": HILDA, "guild": 8}],
    ))
    assert crime["lawful_fight"] and "surrendered" not in crime
    assert crimes.side(HILDA, crime["id"])["why"] == "fought"
    chronicle.record({"fact": "crime_known", "community": "farm", "crime": crime["id"]})
    assert CrimeLaw(crimes).debt("farm") == 550


class Link:
    def __init__(self) -> None:
        self.listeners: list[Any] = []
        self.hello = None
        self.session = Session(type="session", branch="A")
        self.sent: list[Any] = []

    async def send(self, message: Any) -> None:
        self.sent.append(message)

    def receive(self, message: Any) -> None:
        for listener in list(self.listeners):
            listener(message)


@pytest.fixture
async def system():
    link = Link()
    memory = MemoryStore(":memory:")
    chronicle = Chronicle(link, memory)  # type: ignore[arg-type]
    crimes = Crimes(link, chronicle)  # type: ignore[arg-type]
    yield SimpleNamespace(link=link, memory=memory, chronicle=chronicle, crimes=crimes)
    await crimes.close()
    if chronicle._background:
        await asyncio.gather(*chronicle._background)
    memory.close()


def deed(**fields: Any) -> HeroDeed:
    return HeroDeed.model_validate(
        {
            "type": "hero_deed",
            "id": "deed-1",
            "kind": "take_item",
            "item": "ITFO_BREAD",
            "count": 1,
            "hero_guild": 0,
            "position": {"x": 12, "y": 23, "z": 34},
            "waypoint": "FARM1_IN",
            "day": 3,
            "hour": 9,
            "minute": 15,
            **fields,
        }
    )


@pytest.mark.parametrize(
    "ownership",
    [
        {},  # an unowned thing in the field
        {"room_guild": 15},  # GIL_PUBLIC
        {"room_guild": 0},
        {"owner": "PC_HERO", "room_guild": 8},
        {"owner": "PLAYER", "owner_guild": 8},
        {"owner_guild": 8, "hero_guild": 8},
        {"room_owner": "HERO", "room_guild": 8},
        {"room_guild": 8, "hero_guild": 8},
        {"chest": "CHEST", "chest_owner": "PC_HERO", "room_guild": 8},
        {"chest": "CHEST", "chest_guild": 8, "hero_guild": 8},
        {"item_dropped": True, "room_guild": 8},
    ],
)
async def test_own_public_and_dropped_items_are_not_crimes(system, ownership):
    assert system.crimes.record(deed(**ownership)) is None
    assert system.crimes.classified_noncrime("deed-1")
    assert system.chronicle.facts() == []


@pytest.mark.parametrize("monster_source", ["guild", "death"])
async def test_monster_loot_with_a_companion_witness_is_not_a_crime(system, monster_source):
    owner = "YGOBBO_GREEN"
    if monster_source == "death":
        system.chronicle.record({"fact": "died", "npc": owner, "killer": "PLAYER", "monster": True})
    assert system.crimes.record(deed(
        owner=owner,
        owner_npc_guild=37 if monster_source == "guild" else -1,
        witnesses=[HILDA],
        witnesses_metadata=[{"npc": HILDA, "guild": 8, "npctype": 2}],
    )) is None
    assert system.crimes.classified_noncrime("deed-1")
    for kind in ("crime", "crime_seen", "crime_attitude"):
        assert system.chronicle.facts(kind) == []


@pytest.mark.parametrize("prior_crime", [None, "attack", "murder"])
async def test_dead_owners_loot_is_lawful_unless_the_hero_murdered_them(system, prior_crime):
    if prior_crime:
        result = system.crimes.record(deed(
            id="violence", kind=prior_crime, victim=LOBART, victim_guild=8,
        ))
        assert result is not None and result["kind"] == prior_crime
    system.chronicle.record({"fact": "died", "npc": LOBART, "killer": "PLAYER", "monster": False})
    result = system.crimes.record(deed(owner=LOBART, owner_npc_guild=8, witnesses=[HILDA]))
    if prior_crime == "murder":
        assert result is not None and result["kind"] == "theft"
        assert system.crimes.facts()[0]["kind"] == "murder"
    else:
        assert result is None
        assert [fact["kind"] for fact in system.crimes.facts()] == ([prior_crime] if prior_crime else [])
    assert system.crimes.classified_noncrime("deed-1") is (prior_crime != "murder")


async def test_monster_death_allows_loot_even_with_an_existing_murder_record(system):
    system.crimes.record(deed(id="murder", kind="murder", victim=LOBART, victim_guild=8))
    system.chronicle.record({"fact": "died", "npc": LOBART, "killer": "PLAYER", "monster": True})
    assert system.crimes.record(deed(owner=LOBART, witnesses=[HILDA])) is None
    assert system.crimes.classified_noncrime("deed-1")
    assert [fact["kind"] for fact in system.crimes.facts()] == ["murder"]


@pytest.mark.parametrize("reverse", [False, True])
@pytest.mark.parametrize("member", ["PLAYER", "order", "intent", "metadata", "people"])
async def test_recent_fight_with_the_hero_or_a_companion_makes_bandit_loot_lawful(system, member, reverse):
    owner = "BDT_1015_BANDIT_L"
    fields = {}
    fighter = "PLAYER" if member == "PLAYER" else HILDA
    if member == "order":
        system.crimes.sides.orders = SimpleNamespace(
            standing={HILDA: SimpleNamespace(kind="follow")}, fights={},
        )
    elif member == "intent":
        system.crimes.dialogue = SimpleNamespace(conversations={
            HILDA: SimpleNamespace(intent=Intent(goal="follow_hero")),
        })
    elif member == "metadata":
        fields["witnesses_metadata"] = [{"npc": HILDA, "guild": 8, "npctype": 2}]
    elif member == "people":
        system.crimes.people[HILDA] = {"guild": 8, "npctype": 2}
    pair = (fighter, owner) if reverse else (owner, fighter)
    system.crimes.sides.note_fight(*pair)
    assert system.crimes.record(deed(owner=owner, owner_npc_guild=9, witnesses=[HILDA], **fields)) is None
    assert system.crimes.classified_noncrime("deed-1")
    assert system.crimes.facts() == []


@pytest.mark.parametrize("fight", ["missing", "expired", "outsider", "other_owner"])
async def test_loot_requires_a_recent_fight_between_the_owner_and_the_hero_group(system, fight):
    owner = "BDT_1015_BANDIT_L"
    if fight == "expired":
        system.crimes.sides.clock = lambda: 0.0
        system.crimes.sides.note_fight(owner, "PLAYER")
        system.crimes.sides.clock = lambda: FIGHT_MEMORY_SECONDS + 1
    elif fight == "outsider":
        system.crimes.sides.note_fight(owner, HILDA)
    elif fight == "other_owner":
        system.crimes.sides.note_fight("OTHER_BANDIT", "PLAYER")
    result = system.crimes.record(deed(owner=owner, owner_npc_guild=9, witnesses=[HILDA]))
    assert result is not None and result["kind"] == "theft"
    assert not system.crimes.classified_noncrime("deed-1")


@pytest.mark.parametrize("source", ["conversation", "order"])
async def test_current_fight_sources_are_read_before_classifying_loot(system, source):
    system.crimes.sides.clock = lambda: 0.0
    system.crimes.sides.note_fight(LOBART, "PLAYER")
    system.crimes.sides.clock = lambda: FIGHT_MEMORY_SECONDS + 1
    if source == "conversation":
        system.crimes.dialogue = SimpleNamespace(conversations={
            LOBART: SimpleNamespace(intent=Intent(goal="fight", target="PC_HERO")),
        })
    else:
        system.crimes.sides.orders = SimpleNamespace(
            standing={HILDA: SimpleNamespace(kind="follow")},
            fights={HILDA: SimpleNamespace(target=LOBART)},
        )
    assert system.crimes.record(deed(owner=LOBART, owner_npc_guild=8)) is None
    assert system.crimes.classified_noncrime("deed-1")


@pytest.mark.parametrize("prior_crime", ["attack", "murder"])
async def test_loot_after_the_heros_criminal_fight_is_still_theft(system, prior_crime):
    result = system.crimes.record(deed(
        id="violence", kind=prior_crime, victim=LOBART, victim_guild=8,
    ))
    assert result is not None and result["kind"] == prior_crime
    system.crimes.sides.note_fight(LOBART, "PLAYER")
    result = system.crimes.record(deed(owner=LOBART, owner_npc_guild=8))
    assert result is not None and result["kind"] == "theft"
    assert not system.crimes.classified_noncrime("deed-1")


@pytest.mark.parametrize("owner,guild,context", [
    ("BDT_1015_BANDIT_L", 9, {}),
    (LOBART, 8, {"victim_enemy": True}),
])
@pytest.mark.parametrize("aftermath", ["dead", "recent_fight"])
async def test_loot_after_a_lawful_human_kill_preserves_murder_without_theft(
    system, owner, guild, context, aftermath,
):
    murder = system.crimes.record(deed(
        id="murder", kind="murder", victim=owner, victim_guild=guild, **context,
    ))
    assert murder is not None and murder["kind"] == "murder" and murder["lawful_fight"] is True
    if aftermath == "dead":
        system.chronicle.record({"fact": "died", "npc": owner, "killer": "PLAYER", "monster": False})
    else:
        system.crimes.sides.note_fight(owner, "PLAYER")
    assert system.crimes.record(deed(
        owner=owner, owner_npc_guild=guild, witnesses=[HILDA],
        witnesses_metadata=[{"npc": HILDA, "guild": 8, "npctype": 2}],
    )) is None
    assert system.crimes.classified_noncrime("deed-1")
    assert system.crimes.facts() == [murder]
    assert system.chronicle.facts("crime_seen") == []
    assert system.chronicle.facts("crime_attitude") == []


@pytest.mark.parametrize("ownership", [
    {"chest": "CHEST_LOBART", "chest_owner": LOBART, "chest_owner_guild": 8},
    {"room_owner": LOBART, "room_owner_guild": 8},
])
async def test_dead_owners_chest_and_room_remain_private_property(system, ownership):
    system.chronicle.record({"fact": "died", "npc": LOBART, "killer": "PLAYER", "monster": False})
    system.crimes.sides.note_fight(LOBART, "PLAYER")
    result = system.crimes.record(deed(**ownership, witnesses=[HILDA]))
    assert result is not None and result["kind"] == "theft" and result["owner"] == LOBART
    assert not system.crimes.classified_noncrime("deed-1")


@pytest.mark.parametrize("cover", [True, False])
async def test_cover_silence_and_old_testimony_follow_switch_but_ordinary_silence_remains(system, cover):
    system.crimes.cover = cover
    system.crimes.record(deed(owner=LOBART, owner_npc_guild=8, witnesses=[HILDA]))
    system.chronicle.record(
        {
            "fact": "silenced",
            "npc": HILDA,
            "crime": "deed-1",
            "cover": "D1",
            "deal": "D1",
        }
    )
    testimony = {
        "fact": "testimony",
        "npc": LOBART,
        "witness": HILDA,
        "cover": "D1",
        "crime_saw": ["deed-1"],
        "crime_told": [],
    }
    system.chronicle.record(testimony)
    assert system.crimes.known(HILDA)[0].silenced is cover
    assert bool(system.crimes.known(LOBART)) is not cover
    system.chronicle.record({"fact": "silenced", "npc": HILDA, "crime": "deed-1", "deal": "D2"})
    assert system.crimes.known(HILDA)[0].silenced
    assert bool(system.crimes.known(LOBART)) is not cover


async def test_no_cover_reconstructs_only_silence_that_preceded_the_testimony(system):
    system.crimes.cover = False
    system.crimes.record(deed(owner=LOBART, owner_npc_guild=8, witnesses=[HILDA]))
    system.chronicle.record({"fact": "silenced", "npc": HILDA, "crime": "deed-1"})
    before = {
        "fact": "testimony", "id": "before", "npc": LOBART, "witness": HILDA,
        "cover": "D1", "crime_saw": ["deed-1"], "crime_told": [],
    }
    system.chronicle.record(before)
    assert system.crimes.testimony_crimes(before) == []
    system.crimes.record(deed(id="deed-2", owner=LOBART, owner_npc_guild=8, witnesses=[HILDA]))
    after = {**before, "id": "after", "crime_saw": ["deed-2"]}
    system.chronicle.record(after)
    system.chronicle.record({"fact": "silenced", "npc": HILDA, "crime": "deed-2"})
    assert system.crimes.testimony_crimes(after) == ["deed-2"]
    assert [k.crime["id"] for k in system.crimes.known(LOBART)] == ["deed-2"]


@pytest.mark.parametrize(
    "ownership,owner,community",
    [
        ({"owner": LOBART, "owner_npc_guild": 8}, LOBART, "farm"),
        ({"owner_guild": 8}, "", "farm"),
        ({"room_owner": LOBART, "room_owner_guild": 8}, LOBART, "farm"),
        ({"room_guild": 8}, "", "farm"),
        ({"chest": "CHEST", "chest_owner": LOBART, "chest_owner_guild": 8}, LOBART, "farm"),
        ({"chest": "CHEST", "chest_guild": 8}, "", "farm"),
        ({"owner": LOBART, "owner_npc_guild": 8, "room_owner": "PC_HERO"}, LOBART, "farm"),
        ({"owner": LOBART, "owner_npc_guild": 8, "room_guild": 15}, LOBART, "farm"),
        ({"owner": LOBART, "owner_npc_guild": 8, "item_dropped": True}, LOBART, "farm"),
        ({"owner": "UNKNOWN_NPC"}, "UNKNOWN_NPC", None),
    ],
)
async def test_foreign_item_room_or_chest_is_theft(system, ownership, owner, community):
    result = system.crimes.record(deed(**ownership, witnesses=[HILDA, HILDA.lower()]))
    assert result is not None
    assert result == {
        "fact": "crime",
        "id": "deed-1",
        "kind": "theft",
        "victim": owner,
        "owner": owner,
        "community": community,
        "item": "ITFO_BREAD",
        "count": 1,
        "place": {"x": 12, "y": 23, "z": 34},
        "waypoint": "FARM1_IN",
        "day": 3,
        "hour": 9,
        "minute": 15,
        "witnesses": [HILDA],
        **({"chest": ownership["chest"]} if ownership.get("chest") else {}),
    }
    assert system.chronicle.facts("crime_seen") == [
        {"fact": "crime_seen", "npc": HILDA, "crime": "deed-1"},
    ]


async def test_opening_foreign_chest_is_raw_until_an_item_was_taken(system):
    result = system.crimes.record(
        deed(
            kind="use_mob",
            item="",
            count=0,
            chest="CHEST_LOBART",
            chest_owner=LOBART,
            chest_owner_guild=8,
        )
    )
    assert result is None
    assert system.chronicle.facts("crime_seen") == []
    result = system.crimes.record(
        deed(
            id="deed-2",
            chest="CHEST_LOBART",
            chest_owner=LOBART,
            chest_owner_guild=8,
        )
    )
    assert result is not None and result["kind"] == "theft"
    assert result["count"] == 1 and result["item"] == "ITFO_BREAD" and result["witnesses"] == []


async def test_unprovoked_attack_records_the_victims_id_and_real_witnesses(system):
    system.link.receive(deed(kind="attack", victim=LOBART, victim_guild=8, witnesses=[HILDA, LOBART]))
    (result,) = system.crimes.facts()
    assert result["kind"] == "attack" and result["victim"] == LOBART
    assert result["community"] == "farm" and result["witnesses"] == [HILDA, LOBART]


@pytest.mark.parametrize(
    "context",
    [
        {"victim_in_fight": True},  # already ZS_Attack: this blow did not start another assault
        {"victim_started_fight": True},  # defending the hero
        {"victim_enemy": True},  # B_AssessEnemy's hostile target
        {"victim_tough_guy": True},
        {"victim_npctype": 2},  # NPCTYPE_FRIEND
        {"victim_guild": 7},  # GIL_SLD (a mercenary, not GIL_BAU=8)
        {"victim_guild": 6},
        {"victim_guild": 9},
        {"victim_guild": 13},
        {"victim_is_monster": True},
        {"victim_guild": 18},
    ],
)
async def test_combat_retaliation_enemies_and_vanilla_tough_guys_are_not_assaults(system, context):
    fields = {"kind": "attack", "victim": LOBART, "victim_guild": 8, **context}
    assert system.crimes.record(deed(**fields)) is None
    assert system.crimes.facts() == []


async def test_human_kill_is_murder_even_after_assault_or_finishing_blow(system):
    result = system.crimes.record(
        deed(
            kind="murder",
            victim=LOBART,
            victim_guild=8,
            victim_in_fight=True,
            witnesses=[HILDA],
        )
    )
    assert result is not None and result["kind"] == "murder" and result["victim"] == LOBART
    assert result["community"] == "farm"


@pytest.mark.parametrize("context,lawful", [
    ({"victim_enemy": True}, True),
    ({"victim_started_fight": True}, True),
    ({"victim_tough_guy": True}, True),
    ({"victim_guild": 6}, True),
    ({"victim_guild": 7}, True),
    ({"victim_guild": 9}, True),
    ({"victim_guild": 13}, True),
    ({}, False),
    ({"victim_in_fight": True}, False),
    ({"victim_npctype": 2}, False),
])
async def test_human_murder_marks_only_lawful_combat(system, context, lawful):
    fields = {"kind": "murder", "victim": LOBART, "victim_guild": 8, **context}
    result = system.crimes.record(deed(**fields))
    assert result is not None and result["kind"] == "murder"
    if lawful:
        assert result["lawful_fight"] is True
    else:
        assert "lawful_fight" not in result
        system.chronicle.record({"fact": "died", "npc": LOBART, "killer": "PLAYER", "monster": False})
        loot = system.crimes.record(deed(
            id="loot", owner=LOBART, owner_npc_guild=8, witnesses=[HILDA],
        ))
        assert loot is not None and loot["kind"] == "theft"
        assert not system.crimes.classified_noncrime("loot")


async def test_monster_kill_is_not_human_murder(system):
    assert system.crimes.record(deed(kind="murder", victim="WOLF", victim_guild=22)) is None


async def test_foreign_sheep_uses_actual_witness_community_without_inventing_its_owner(system):
    result = system.crimes.record(
        deed(
            kind="murder",
            victim="SHEEP",
            victim_guild=18,
            victim_is_monster=True,
            victim_is_sheep=True,
            witnesses=[HILDA],
            witnesses_metadata=[{"npc": HILDA, "guild": 8, "npctype": 0}],
        )
    )
    assert result is not None and result["kind"] == "sheep" and result["community"] == "farm"
    assert result["victim"] == "SHEEP" and result["owner"] == ""


async def test_unwitnessed_sheep_keeps_unknown_community_and_own_sheep_is_not_a_crime(system):
    result = system.crimes.record(deed(kind="murder", victim="SHEEP", victim_guild=18))
    assert result is not None and result["kind"] == "sheep" and result["community"] is None
    assert (
        system.crimes.record(
            deed(
                id="deed-2",
                kind="murder",
                victim="FOLLOW_SHEEP",
                victim_guild=18,
                victim_tough_guy=True,
            )
        )
        is None
    )


@pytest.mark.parametrize(
    "guild,npctype,expected",
    [
        (1, 0, "city"),
        (2, 1, "city"),
        (3, 0, "city"),
        (1, 3, "oldcamp"),
        (2, 4, "oldcamp"),
        (3, 3, "oldcamp"),
        (7, 0, "farm"),
        (8, 1, "farm"),
        (4, 0, "monastery"),
        (5, 0, "monastery"),
        (9, 5, "bl"),
        (9, 7, "bl"),
        (9, 0, None),
        (0, 0, None),
        (18, 0, None),
    ],
)
async def test_vanilla_communities(system, guild, npctype, expected):
    assert system.crimes.community(guild, npctype) == expected


async def test_community_uses_lifted_helpers_and_lawkeepers_use_the_graph(system):
    # A different DAT can assign different guilds: the lifted expression is authoritative.
    field = ["field", ["param", 0], "GUILD"]
    graph = StoryGraph(
        nodes={
            "DIA_LEE_PETZMASTER": Node(
                id="DIA_LEE_PETZMASTER",
                kind="dialog",
                npc=LEE,
                description="",
                effects=[],
                writes=set(),
            )
        },
        functions={
            "C_NPCBELONGSTOFARM": {
                "params": 1,
                "body": ["==", field, ["int", 10]],
                "lifted": True,
            }
        },
        instances={"LOBART": LOBART},
        vars={},
        quests={},
    )
    system.crimes.graph = graph
    assert system.crimes.community(10) == "farm"
    assert system.crimes.community(8) is None
    assert system.crimes.lawkeepers("farm") == (LEE,)
    assert system.crimes.lawkeepers("bl") == ()
    assert system.crimes.lawkeepers("city") == ()  # no such PETZMASTER in this graph
    result = system.crimes.record(deed(owner="LOBART", owner_npc_guild=10))
    assert result is not None and result["owner"] == LOBART and result["community"] == "farm"
    assert set(COMMUNITIES) == {"city", "farm", "monastery", "oldcamp", "bl"}


@pytest.mark.parametrize(
    "before,after,picked_before,picked_after,caught,expected",
    [
        ([], [{"item": "ITMI_GOLD", "count": 80}], 0, 1, False, "success"),
        (
            [{"item": "ITMI_GOLD", "count": 100}],
            [{"item": "ITMI_GOLD", "count": 180}],
            0,
            1,
            False,
            "success",
        ),
        ([], [{"item": "ITKE_KEY", "count": 1}], 0, 0, False, "success"),
        ([], [{"item": "ITKE_KEY", "count": 1}], 0, 1, True, "success"),  # stale AR_THEFT
        ([], [], 0, 0, True, "failure"),
        ([], [], 0, 0, False, "failure"),
        ([], [], 0, 1, True, "failure"),  # an explicit caught attempt outweighs the pocket flag
        ([], [], 1, 1, False, None),  # the pocket was already marked, no new attempt proved
        ([], [], 0, 1, False, None),
    ],
)
def test_pickpocket_outcome_from_inventory_and_original_script_flag(
    before,
    after,
    picked_before,
    picked_after,
    caught,
    expected,
):
    assert pickpocket_outcome(before, after, picked_before, picked_after, caught) == expected


@pytest.mark.parametrize("caught", [False, True])
async def test_successful_pickpocket_has_no_witnesses_even_with_people_nearby(system, caught):
    result = system.crimes.record(
        deed(
            kind="pickpocket",
            item="",
            count=0,
            victim=LOBART,
            victim_guild=8,
            inventory_before=[{"item": "ITMI_GOLD", "count": 50}],
            inventory_after=[{"item": "ITMI_GOLD", "count": 130}],
            picked_pocket_after=1,
            caught=caught,
            witnesses=[LOBART, HILDA],
        )
    )
    assert result is not None and result["outcome"] == "success" and result["kind"] == "theft"
    assert result["item"] == "ITMI_GOLD" and result["count"] == 80
    assert result["owner"] == LOBART and result["community"] == "farm"
    assert result["witnesses"] == [] and system.chronicle.facts("crime_seen") == []


async def test_failed_pickpocket_adds_the_catching_victim_as_witness(system):
    result = system.crimes.record(
        deed(
            kind="pickpocket",
            victim=LOBART,
            victim_guild=8,
            caught=True,
            witnesses=[HILDA],
        )
    )
    assert result is not None and result["outcome"] == "failure" and result["kind"] == "theft"
    assert result["witnesses"] == [LOBART, HILDA] and result["count"] == 0
    assert [f["npc"] for f in system.chronicle.facts("crime_seen")] == [LOBART, HILDA]


async def test_each_actual_item_gain_is_preserved_for_bespoke_pickpockets(system):
    result = system.crimes.record(
        deed(
            kind="pickpocket",
            victim=LOBART,
            victim_guild=8,
            inventory_after=[{"item": "ITMI_GOLD", "count": 80}, {"item": "ITKE_KEY", "count": 1}],
        )
    )
    assert result is not None and result["items"] == [
        {"item": "ITMI_GOLD", "count": 80},
        {"item": "ITKE_KEY", "count": 1},
    ]
    assert len(system.crimes.facts()) == 1


async def test_save_load_restores_crime_and_seen_at_the_saved_branch_head(system):
    theft = deed(owner=LOBART, owner_npc_guild=8, witnesses=[HILDA])
    system.link.receive(theft)
    saved = system.memory.head("A")
    system.link.receive(deed(id="deed-2", kind="attack", victim=LOBART, victim_guild=8))
    assert len(system.crimes.facts()) == 2

    system.link.receive(Session(type="session", branch="B", parent="A", parent_seq=saved, reason="load"))
    assert [f["id"] for f in system.crimes.facts()] == ["deed-1"]
    assert system.chronicle.facts("crime_seen") == [{"fact": "crime_seen", "npc": HILDA, "crime": "deed-1"}]
    system.link.receive(theft)  # reconnect replay of the same deed
    assert len(system.crimes.facts()) == 1

    system.link.receive(Session(type="session", branch="C", reason="new_game"))
    assert system.crimes.facts() == [] and system.chronicle.facts("crime_seen") == []
    system.link.receive(theft)
    assert len(system.crimes.facts()) == 1  # dedup is scoped to the branch


async def test_no_session_does_not_create_truth_and_close_removes_listener(system):
    system.chronicle.branch = None
    assert system.crimes.record(deed(owner=LOBART)) is None
    assert not system.crimes.classified_noncrime("deed-1")
    await system.crimes.close()
    assert system.crimes._on_message not in system.link.listeners


async def test_duplicate_record_is_not_a_negative_classification(system):
    raw = deed(owner=LOBART, owner_npc_guild=12)
    assert system.crimes.record(raw) is not None
    assert system.crimes.record(raw) is None
    assert not system.crimes.classified_noncrime(raw.id)


@pytest.mark.parametrize("fields", [
    {"item": ""},
    {"count": 0},
    {"kind": "attack", "victim": ""},
    {"kind": "murder", "victim": ""},
    {"kind": "pickpocket", "victim": ""},
    {"kind": "pickpocket", "victim": LOBART, "picked_pocket_before": 1, "picked_pocket_after": 1},
    {"kind": "unknown"},
])
async def test_incomplete_or_unclassified_deed_is_not_a_negative_classification(system, fields):
    classified = []
    system.crimes.on_noncrime.append(classified.append)
    # model_copy also exercises record's defensive branch for an unknown future deed kind.
    raw = deed().model_copy(update=fields)
    assert system.crimes.record(raw) is None
    assert not system.crimes.classified_noncrime(raw.id)
    assert classified == [] and system.crimes.facts() == []


async def test_noncrime_signal_follows_classification_and_is_cleared_on_session(system):
    classified = []
    system.crimes.on_noncrime.append(classified.append)
    system.link.receive(deed())  # nobody owns the item
    assert classified == ["deed-1"]
    assert system.crimes.classified_noncrime("deed-1")
    system.link.receive(Session(type="session", branch="B"))
    assert not system.crimes.classified_noncrime("deed-1")


def murder(system, **fields):
    return system.crimes.record(
        deed(kind="murder", victim=LOBART, victim_guild=8, witnesses=[HILDA], **fields)
    )


def side(system, why):
    assert system.crimes.side(HILDA.lower(), "deed-1") == {
        "fact": "crime_side", "npc": HILDA, "crime": "deed-1", "why": why,
    }
    assert system.crimes.facts()[0]["kind"] == "murder"
    assert system.crimes.facts()[0]["witnesses"] == [HILDA]
    assert [fact["npc"] for fact in system.chronicle.facts("crime_seen")] == [HILDA]


async def test_enemy_witness_fears_only_other_violence_and_understands_its_side(system):
    cards = {
        npc: Card(npc, npc, "", "", greed=3, honesty=3, courage=2, talk=3, pushy=3, strangers=3)
        for npc in (HILDA, LEE)
    }
    system.crimes.sides.note_fight(LOBART, HILDA)
    system.crimes.record(deed(kind="murder", victim=LOBART, victim_guild=8, witnesses=[HILDA, LEE]))
    assert system.crimes.side(HILDA, "deed-1") is not None
    assert not afraid(HILDA, cards, system.crimes, courage=2)
    assert afraid(LEE, cards, system.crimes, courage=2)
    ally_situation = system.crimes.situation(HILDA)
    outsider_situation = system.crimes.situation(LEE)
    assert "Ты видел сам" in ally_situation
    assert " Жертва была твоим врагом: ты не винишь героя." in ally_situation
    assert "Ты видел сам" in outsider_situation
    assert "ты не винишь героя" not in outsider_situation

    system.crimes.record(deed(
        id="other-murder", kind="murder", victim="OTHER_VICTIM", victim_guild=8, witnesses=[HILDA],
    ))
    assert system.crimes.side(HILDA, "other-murder") is None
    assert afraid(HILDA, cards, system.crimes, courage=2)


@pytest.mark.parametrize("attacker,target", [(HILDA, LOBART), (LOBART, HILDA)])
async def test_fight_intent_survives_reset_before_the_execution(system, attacker, target):
    system.crimes.sides.note_intent(attacker, Intent(goal="fight", target=target))
    # The real Brago incident: knocked down at 18:42:15, executed at 18:42:36. At death neither
    # current intent nor the bridge's victim_* combat flags identifies that earlier fight.
    system.crimes.sides.note_intent(attacker, Intent())
    system.crimes.dialogue = SimpleNamespace(conversations={attacker: SimpleNamespace(intent=Intent())})
    assert murder(system) is not None
    side(system, "fought")


@pytest.mark.parametrize("kind", ["npc_attack", "npc_threat"])
async def test_native_attack_or_threat_makes_its_target_a_side(system, kind):
    system.link.receive(NpcEvent(type="npc_event", npc=HILDA, kind=kind, other=LOBART))
    murder(system)
    side(system, "fought")


async def test_controlled_witness_damage_perception_remembers_the_attacker(system):
    system.link.receive(
        Perception(type="perception", npc=HILDA, perc="ASSESSDAMAGE", other=LOBART, distance=20)
    )
    murder(system)
    side(system, "fought")


@pytest.mark.parametrize("actor,target", [(HILDA, LOBART), (LOBART, HILDA)])
async def test_native_combat_target_in_a_view_remembers_both_directions(system, actor, target):
    system.link.receive(NpcViews.model_validate({"type": "npc_views", "t": 1, "views": [{
        "npc": "OBSERVER", "name": "Наблюдатель", "sees": [
            {"id": actor, "name": actor, "distance": 100, "combat_target": target},
        ],
    }]}))
    murder(system)
    side(system, "fought")


@pytest.mark.parametrize("following", ["order", "intent", "friend"])
@pytest.mark.parametrize("signal", ["hero_attacked", "intent", "enemy"])
async def test_companion_sides_with_hero_against_his_attacker(system, following, signal):
    fields = {}
    if following == "order":
        system.crimes.sides.orders = SimpleNamespace(
            standing={HILDA: SimpleNamespace(kind="follow")}, fights={},
        )
    elif following == "intent":
        system.crimes.dialogue = SimpleNamespace(conversations={
            HILDA: SimpleNamespace(intent=Intent(goal="follow_hero")),
        })
    else:
        fields["witnesses_metadata"] = [{"npc": HILDA, "guild": 8, "npctype": 2}]
    if signal == "hero_attacked":
        system.link.receive(NpcEvent(type="npc_event", npc="OBSERVER", kind="hero_attacked", other=LOBART))
    elif signal == "intent":
        system.crimes.sides.note_intent(LOBART, Intent(goal="fight", target="PC_HERO"))
    else:
        fields["victim_enemy"] = True
    murder(system, **fields)
    side(system, "companion")


@pytest.mark.parametrize("kind", ["npc_attack", "npc_threat"])
async def test_attack_on_another_companion_puts_witness_on_the_heros_side(system, kind):
    system.crimes.sides.orders = SimpleNamespace(standing={
        HILDA: SimpleNamespace(kind="follow"), LEE: SimpleNamespace(kind="follow"),
    }, fights={})
    system.link.receive(NpcEvent(type="npc_event", npc=LEE, kind=kind, other=LOBART))
    murder(system)
    side(system, "companion")


async def test_group_attack_survives_the_targets_follow_intent_reset(system):
    system.crimes.dialogue = SimpleNamespace(conversations={
        HILDA: SimpleNamespace(intent=Intent(goal="follow_hero")),
        LEE: SimpleNamespace(intent=Intent(goal="follow_hero")),
    })
    system.crimes.sides.note_fight(LOBART, LEE)
    system.crimes.dialogue.conversations[LEE].intent = Intent()
    murder(system)
    side(system, "companion")


async def test_attack_on_the_group_expires_with_combat_memory(system):
    system.crimes.sides.clock = lambda: 0.0
    system.crimes.sides.note_fight(LOBART, "HERO")
    system.crimes.sides.clock = lambda: 601.0
    murder(system, witnesses_metadata=[{"npc": HILDA, "guild": 8, "npctype": 2}])
    assert system.crimes.side(HILDA, "deed-1") is None
    assert system.crimes.sides.group_threats == {}


@pytest.mark.parametrize("source", ["intent", "executor", "order", "orders_start", "orders_executor"])
async def test_real_intent_and_fight_callbacks_keep_the_conflict_after_the_body_stops(system, source):
    system.link.snapshots = {}

    async def act(npc, action, /, **args):
        return SimpleNamespace(status="done")

    system.link.act = act
    dialogue = DialogueManager(system.link, FakeBackend(), PersonaProvider(None), memory=system.memory)
    system.crimes.dialogue = dialogue
    dialogue.on_intent = system.crimes.sides.note_intent
    dialogue.on_fight = system.crimes.sides.note_fight
    talk = SimpleNamespace(people={
        HILDA: SimpleNamespace(id=HILDA, name="Хильда", activity="routine", earshot=100),
    })
    orders = Orders(system.link, talk, dialogue)
    orders.on_fight = system.crimes.sides.note_fight
    # The callbacks are synchronous, before an action starts. The body itself is irrelevant here.
    orders._spawn = lambda coroutine: coroutine.close()
    conv = dialogue.open(HILDA, "Хильда")
    if source == "intent":
        dialogue.change_intent(HILDA, "Хильда", body=False, goal="fight", target=LOBART)
        dialogue.change_intent(HILDA, "Хильда", body=False, goal="none", target=None)
    elif source == "executor":
        await dialogue._body(conv, [("fight", {"npc": LOBART})])
    elif source == "orders_executor":
        assert await orders._act(HILDA, "fight", npc=LOBART) == "done"
    elif source == "orders_start":
        orders._start(HILDA, LOBART, None)
        orders.fights.clear()
    else:
        conv.intent = Intent(goal="follow_hero")
        assert orders.take(Order("attack", "бей", "Бей его!"), LOBART, None) == [(HILDA, "Хильда")]
        conv.intent = Intent()
    murder(system)
    side(system, "fought")
    if dialogue._background:
        await asyncio.gather(*dialogue._background)


async def test_current_executor_and_order_fights_are_also_evidence(system):
    system.crimes.sides.orders = SimpleNamespace(standing={}, fights={HILDA: SimpleNamespace(target=LOBART)})
    murder(system)
    side(system, "fought")


async def test_current_unreported_fight_intent_is_evidence(system):
    system.crimes.dialogue = SimpleNamespace(conversations={
        LOBART: SimpleNamespace(intent=Intent(goal="fight", target=HILDA)),
    })
    murder(system)
    side(system, "fought")


@pytest.mark.parametrize("hero", ["PLAYER", "HERO", "PC_HERO"])
async def test_attacking_a_victim_does_not_make_it_the_companions_enemy(system, hero):
    system.crimes.sides.orders = SimpleNamespace(standing={HILDA: SimpleNamespace(kind="follow")}, fights={})
    system.crimes.sides.note_fight(hero, LOBART)
    murder(system)
    assert system.crimes.side(HILDA, "deed-1") is None


@pytest.mark.parametrize("unrelated", ["hero_attacked", "different_foe", "halt_order", "expired"])
async def test_outsiders_and_unrelated_or_expired_conflict_still_judge_the_killing(system, unrelated):
    if unrelated == "hero_attacked":
        system.crimes.sides.note_fight(LOBART, "HERO")
    elif unrelated == "different_foe":
        system.crimes.sides.note_fight("SOMEONE_ELSE", HILDA)
    elif unrelated == "halt_order":
        system.crimes.sides.orders = SimpleNamespace(
            standing={HILDA: SimpleNamespace(kind="halt")}, fights={},
        )
    else:
        system.crimes.sides.clock = lambda: 0.0
        system.crimes.sides.note_fight(LOBART, HILDA)
        system.crimes.sides.clock = lambda: 601.0
    murder(system, victim_enemy=True)
    assert system.crimes.side(HILDA, "deed-1") is None
    assert system.crimes.attitude(HILDA) == "wary"


@pytest.mark.parametrize("reset", [Loading(type="loading"), Session(type="session", branch="B")])
async def test_world_reset_clears_short_combat_memory_and_keeps_guild_cache(system, reset):
    system.crimes.sides.note_fight(HILDA, LOBART)
    system.crimes.sides.note_fight(LOBART, "PLAYER")
    system.crimes.sides.hostile_guilds.add((8, 8))
    system.link.receive(reset)
    murder(system)
    assert system.crimes.sides.fights == {}
    assert system.crimes.sides.group_threats == {}
    assert system.crimes.sides.hostile_guilds == {(8, 8)}
    assert system.crimes.side(HILDA, "deed-1") is None


async def test_guild_hostility_is_read_once_per_world_and_uses_witness_to_victim_direction(system):
    calls = []

    async def evaluate(npc, exprs):
        assert npc == "PC_HERO"
        calls.append((npc, exprs))
        if exprs == [["var", "GIL_MAX"], ["var", "ATT_HOSTILE"]]:
            return [3, 0]
        return [0 if (expr[2][1], expr[3][1]) == (1, 2) else 2 for expr in exprs]

    system.link.story_eval = evaluate
    await system.crimes.sides.prepare()
    await system.crimes.sides.prepare()
    assert len(calls) == 2 and len(calls[1][1]) == 9
    result = system.crimes.record(deed(kind="murder", victim=LOBART, victim_guild=2, witnesses=[HILDA],
                                      witnesses_metadata=[{"npc": HILDA, "guild": 1, "npctype": 0}]))
    assert result is not None
    side(system, "hostile")
    system.crimes.record(deed(id="reverse", kind="murder", victim=LEE, victim_guild=1, witnesses=[HILDA],
                             witnesses_metadata=[{"npc": HILDA, "guild": 2, "npctype": 0}]))
    assert system.crimes.side(HILDA, "reverse") is None
    system.link.receive(Loading(type="loading"))
    await system.crimes.sides.prepare()
    assert len(calls) == 4


async def test_old_world_guild_query_cannot_fill_the_new_world_cache(system):
    requested, ready = asyncio.Event(), asyncio.Event()

    async def evaluate(npc, exprs):
        requested.set()
        await ready.wait()
        return [3, 0]

    system.link.story_eval = evaluate
    pending = asyncio.create_task(system.crimes.sides.prepare())
    await requested.wait()
    system.link.receive(Session(type="session", branch="B"))
    ready.set()
    await pending
    assert system.crimes.sides.hostile_guilds == set()
    assert not system.crimes.sides._prepared


async def test_last_guild_matrix_classifies_a_killing_while_new_world_refresh_is_pending(system):
    requested, ready = asyncio.Event(), asyncio.Event()
    refreshing = False

    async def evaluate(npc, exprs):
        if exprs == [["var", "GIL_MAX"], ["var", "ATT_HOSTILE"]]:
            return [3, 0]
        if refreshing:
            requested.set()
            await ready.wait()
            return [2] * len(exprs)
        return [0 if (expr[2][1], expr[3][1]) == (1, 2) else 2 for expr in exprs]

    system.link.story_eval = evaluate
    await system.crimes.sides.prepare()
    assert system.crimes.sides.hostile_guilds == {(1, 2)}
    system.link.receive(Session(type="session", branch="B"))
    refreshing = True
    pending = asyncio.create_task(system.crimes.sides.prepare())
    await requested.wait()
    try:
        assert not pending.done()
        assert system.crimes.sides.hostile_guilds == {(1, 2)}
        system.crimes.record(deed(
            kind="murder", victim=LOBART, victim_guild=2, witnesses=[HILDA],
            witnesses_metadata=[{"npc": HILDA, "guild": 1, "npctype": 0}],
        ))
        side(system, "hostile")
    finally:
        ready.set()
        await pending
    assert system.crimes.sides.hostile_guilds == set()
    assert system.crimes.sides._prepared


@pytest.mark.parametrize("state,stage,giver,expected", [
    ("given", 1, HILDA, "quest"), ("given", 2, HILDA, "quest"),
    ("given", 3, HILDA, "quest"), ("open", 0, HILDA, None), ("done", 3, HILDA, None),
    ("failed", 2, HILDA, None), ("obsolete", 2, HILDA, None), ("given", 1, LEE, None),
])
async def test_only_issued_open_kill_quest_from_this_witness_counts(system, state, stage, giver, expected):
    quest = Quest("DIR_Q1", giver, "Поручение", Talk("Возьмусь", []), [
        Stage("Начало", [Goal(kind="talk", npc=LEE)]),
        Stage("Победить врага", [Goal(kind="kill", npcs=[LOBART])]),
        Stage("Вернуться", [Goal(kind="talk", npc=giver)]),
    ], "Готово")
    system.crimes.sides.book = SimpleNamespace(
        all=lambda: {quest.id: quest}, progress=lambda _: Progress(state, stage),
    )
    murder(system)
    if expected:
        side(system, expected)
    else:
        assert system.crimes.side(HILDA, "deed-1") is None


async def test_sides_precede_all_seen_facts_and_survive_chronicle_reload(system):
    system.crimes.sides.note_fight(LOBART, HILDA)
    observed = []
    system.chronicle.on_fact.append(
        lambda fact: observed.append((fact["fact"], system.crimes.side(HILDA, "deed-1")))
    )
    murder(system)
    assert [kind for kind, _ in observed] == ["crime", "crime_side", "crime_seen"]
    assert observed[-1][1] is not None
    saved = system.memory.head("A")
    system.crimes.record(deed(id="unsaved", kind="murder", victim=LEE, victim_guild=7, witnesses=[HILDA]))
    loaded = Session(type="session", branch="B", parent="A", parent_seq=saved, reason="load")
    system.link.session = loaded
    system.link.receive(loaded)
    reloaded = Chronicle(system.link, system.memory)
    loaded_crimes = Crimes(system.link, reloaded)
    assert [fact["id"] for fact in loaded_crimes.facts()] == ["deed-1"]
    side(SimpleNamespace(crimes=loaded_crimes, chronicle=reloaded), "fought")
    assert loaded_crimes.sides.fights == {}
    assert loaded_crimes.known(HILDA)[0].degree == "seen"
    await loaded_crimes.close()


@pytest.mark.parametrize("why", ["fought", "companion", "hostile", "quest"])
async def test_listener_uses_the_same_side_reasons_without_witness_metadata(system, why):
    system.crimes.people.update({HILDA: {"guild": 1, "npctype": 0}, LOBART: {"guild": 8}})
    if why == "fought":
        system.crimes.sides.note_fight(LOBART, HILDA)
    elif why == "companion":
        system.crimes.people[HILDA]["npctype"] = 2
        system.crimes.sides.note_fight(LOBART, "PLAYER")
    elif why == "hostile":
        system.crimes.sides.hostile_guilds.add((1, 8))
    else:
        quest = Quest("DIR_KILL", HILDA, "Поручение", Talk("Возьмусь", []), [
            Stage("Убить врага", [Goal(kind="kill", npcs=[LOBART])]),
        ], "Готово")
        system.crimes.sides.book = SimpleNamespace(
            all=lambda: {quest.id: quest}, progress=lambda _: Progress("given", 1),
        )
    crime = system.crimes.record(deed(kind="murder", victim=LOBART, victim_guild=8))
    assert crime["victim_guild"] == 8
    first = system.crimes.learn_side(HILDA.lower(), crime)
    assert first == {"fact": "crime_side", "npc": HILDA, "crime": crime["id"], "why": why}
    assert system.crimes.learn_side(HILDA, crime) == first
    assert len(system.chronicle.facts("crime_side")) == 1


@pytest.mark.parametrize("saved,person,expected", [
    (8, 9, "hostile"), (None, 8, "hostile"), (-1, 8, "hostile"), (None, None, None),
])
async def test_listener_hostility_needs_known_victim_guild_and_prefers_saved(system, saved, person, expected):
    system.crimes.people[HILDA] = {"guild": 1, "npctype": 0}
    if person is not None:
        system.crimes.people[LOBART] = {"guild": person}
    system.crimes.sides.hostile_guilds.update({(1, 8), (1, -1)})
    crime = {"id": "old-murder", "kind": "murder", "victim": LOBART}
    if saved is not None:
        crime["victim_guild"] = saved
    side = system.crimes.learn_side(HILDA, crime)
    assert (side["why"] if side else None) == expected
