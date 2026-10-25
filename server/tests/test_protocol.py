import json

import pytest
from pydantic import ValidationError

from gothic_ai import triggers
from gothic_ai.protocol import (
    BROKEN_TRACE_PROTOCOL,
    CRIME_MEET_PROTOCOL,
    CRIME_PROTOCOL,
    CRIME_REACT_PROTOCOL,
    ERRAND_PROTOCOL,
    FIGHT_REACTION_PROTOCOL,
    NOISE_PROTOCOL,
    NPC_CRIME_PROTOCOL,
    NPC_FAR_PROTOCOL,
    PACE_PROTOCOL,
    PROTOCOL_VERSION,
    TRACE_PROTOCOL,
    ActionCmd,
    ActionResult,
    BeastCmd,
    BeastEvent,
    Beasts,
    Chest,
    CombatHit,
    DebugCmd,
    GraphPatchCmd,
    HeroDeed,
    HoldVerdictCmd,
    HushCmd,
    LineVoice,
    NpcActCmd,
    NpcActResult,
    NpcChestsCmd,
    NpcDeed,
    NpcEvent,
    NpcIdle,
    NpcLook,
    NpcRoster,
    NpcSpec,
    NpcThing,
    NpcViews,
    PerceptionHeld,
    RoutineEntry,
    SayCmd,
    Snapshot,
    StoryEffectCmd,
    StoryNpcCmd,
    StoryProgramCmd,
    StoryStateCmd,
    SvmLine,
    SvmModeCmd,
    ThreatControlCmd,
    TriggerFired,
    parse_game_message,
)
from tests.fake_game import FakeGame


def test_parses_snapshot_from_game():
    msg = parse_game_message(
        '{"type":"snapshot","t":5,"world":{"name":"NEWWORLD.ZEN","day":0,"hour":8,"minute":30},'
        '"npc":{"id":"NONE_100_XARDAS","name":"Ксардас","pos":[1,2,3],"rot":0,"hp":10,"hp_max":10,'
        '"busy":false,"action":null,"waypoint":null,"nearest_waypoint":"WP"},"nearby_waypoints":[]}'
    )
    assert isinstance(msg, Snapshot)
    assert msg.npc.name == "Ксардас"
    assert msg.player is None


def test_fake_game_snapshot_matches_protocol():
    assert isinstance(parse_game_message(_json(FakeGame().snapshot("X"))), Snapshot)


def test_action_result_ok_only_when_done():
    done = parse_game_message('{"type":"action_result","id":"a1","npc":"X","status":"done","distance":12.5}')
    timeout = parse_game_message('{"type":"action_result","id":"a2","npc":"X","status":"timeout"}')
    assert isinstance(done, ActionResult) and done.ok
    assert isinstance(timeout, ActionResult) and not timeout.ok


def test_unknown_message_type_is_rejected():
    with pytest.raises(ValidationError):
        parse_game_message('{"type":"nope"}')


@pytest.mark.parametrize("kind", ["melee", "ranged", "spell"])
def test_v51_combat_hit_reports_actual_health_loss(kind):
    msg = parse_game_message(_json({
        "type": "combat_hit", "npc": "X", "by": "PLAYER", "damage": 7,
        "hp": 23, "hp_max": 100, "down": False, "dead": False, "kind": kind,
    }))
    assert isinstance(msg, CombatHit)
    assert (msg.npc, msg.by, msg.damage, msg.hp, msg.hp_max, msg.kind) == ("X", "PLAYER", 7, 23, 100, kind)


def test_v50_svm_lines_keep_the_native_meaning_and_addressee():
    assert PROTOCOL_VERSION == 51
    msg = parse_game_message(_json({
        "type": "svm_line", "id": "svm-1", "npc": "X", "svm": "$KILLENEMY",
        "text": "Ещё одним монстром меньше.", "target": "WOLF", "overlay": True,
    }))
    assert isinstance(msg, SvmLine)
    assert (msg.id, msg.npc, msg.svm, msg.text, msg.target, msg.overlay) == (
        "svm-1", "X", "$KILLENEMY", "Ещё одним монстром меньше.", "WOLF", True,
    )
    assert json.loads(SvmModeCmd(replace=True).to_json()) == {"type": "svm_mode", "replace": True}


def test_hero_deed_retains_ownership_ids_and_engine_witnesses():
    assert PROTOCOL_VERSION >= CRIME_PROTOCOL == 42
    msg = parse_game_message(
        _json(
            {
                "type": "hero_deed",
                "id": "deed-17",
                "kind": "take_item",
                "item": "ITFO_BREAD",
                "count": 2,
                "owner": "BAU_950_LOBART",
                "owner_guild": 7,
                "owner_npc_guild": 7,
                "owner_npctype": 0,
                "room": "LOBART",
                "room_owner": "BAU_950_LOBART",
                "room_guild": 7,
                "position": {"x": 1, "y": 2, "z": 3},
                "waypoint": "NW_FARM1_HOUSE",
                "day": 1,
                "hour": 12,
                "minute": 30,
                "witnesses": ["BAU_951_HILDA"],
            }
        )
    )
    assert isinstance(msg, HeroDeed)
    assert (msg.id, msg.item, msg.count, msg.owner) == ("deed-17", "ITFO_BREAD", 2, "BAU_950_LOBART")
    assert msg.position.model_dump() == {"x": 1, "y": 2, "z": 3}
    assert msg.witnesses == ["BAU_951_HILDA"]
    assert (msg.room_owner, msg.room_guild, msg.waypoint) == ("BAU_950_LOBART", 7, "NW_FARM1_HOUSE")


def test_v44_independent_deeds_switch_and_held_deed_id():
    assert PROTOCOL_VERSION >= CRIME_REACT_PROTOCOL == 44
    assert "deeds" not in json.loads(ThreatControlCmd(hold=True).to_json())
    command = json.loads(ThreatControlCmd(hold=False, deeds=True).to_json())
    assert command == {"type": "threat_control", "hold": False, "deeds": True}
    held = parse_game_message(_json({
        "type": "perception_held", "hold": "h1", "npc": "HILDA", "other": "PLAYER",
        "kind": "theft", "deed": "branch/deed-1",
    }))
    assert isinstance(held, PerceptionHeld)
    assert held.deed == "branch/deed-1"


@pytest.mark.parametrize("hold,deeds,bandits", [(False, False, True), (True, True, False)])
def test_v49_bandit_switch_is_independent_and_opt_in(hold, deeds, bandits):
    assert PROTOCOL_VERSION >= 49
    assert "bandits" not in json.loads(ThreatControlCmd(hold=True).to_json())
    assert json.loads(ThreatControlCmd(bandits=True).to_json()) == {
        "type": "threat_control", "bandits": True,
    }
    assert json.loads(ThreatControlCmd(hold=hold, deeds=deeds, bandits=bandits).to_json()) == {
        "type": "threat_control", "hold": hold, "deeds": deeds, "bandits": bandits,
    }


@pytest.mark.parametrize("kind,other", [
    ("bandit_enemy", "PLAYER"), ("bandit_enemy", "CAVALORN"), ("party_enemy", "BDT_10300"),
])
def test_v49_bandit_sighting_keeps_the_pair_and_distance(kind, other):
    held = parse_game_message(_json({
        "type": "perception_held", "hold": "bandit-1", "npc": "WITNESS", "other": other,
        "other_name": "Target", "kind": kind, "distance": 1200,
    }))
    assert isinstance(held, PerceptionHeld)
    assert (held.npc, held.other, held.other_name, held.kind, held.distance, held.deed) == (
        "WITNESS", other, "Target", kind, 1200, None,
    )


@pytest.mark.parametrize("ms", [1000, 5000, 15000])
def test_v49_parley_verdict_has_a_bounded_lease(ms):
    assert json.loads(HoldVerdictCmd(hold="bandit-1", to="parley", ms=ms).to_json()) == {
        "type": "hold_verdict", "hold": "bandit-1", "to": "parley", "ms": ms,
    }


@pytest.mark.parametrize("ms", [None, 999, 15001, 5000.5, True])
def test_v49_parley_verdict_requires_a_valid_duration(ms):
    with pytest.raises(ValidationError):
        HoldVerdictCmd(hold="bandit-1", to="parley", ms=ms)


@pytest.mark.parametrize("to", ["server", "scripts"])
def test_existing_hold_verdicts_need_no_lease(to):
    assert json.loads(HoldVerdictCmd(hold="h1", to=to).to_json()) == {
        "type": "hold_verdict", "hold": "h1", "to": to,
    }


@pytest.mark.parametrize("kind", ["take_item", "attack"])
def test_v47_npc_deed_keeps_shared_snapshot_and_culprit_separate_from_hero(kind):
    assert PROTOCOL_VERSION >= NPC_CRIME_PROTOCOL == 47
    msg = parse_game_message(_json({
        "type": "npc_deed", "id": "npc-deed-1", "kind": kind, "culprit": "VINO",
        "culprit_guild": 7, "culprit_npctype": 0, "victim": "MALAK", "owner": "LOBART",
        "chest": "CHEST_LOBART", "chest_owner": "LOBART", "room_owner": "LOBART",
        "position": {"x": 1, "y": 2, "z": 3}, "witnesses": ["HILDA"],
    }))
    assert isinstance(msg, NpcDeed) and not isinstance(msg, HeroDeed)
    assert (msg.culprit, msg.culprit_guild, msg.chest_owner, msg.room_owner) == (
        "VINO", 7, "LOBART", "LOBART",
    )
    assert msg.witnesses == ["HILDA"]


def test_npc_deed_requires_a_culprit_and_cannot_report_murder():
    deed = {"type": "npc_deed", "id": "n1", "kind": "attack",
            "position": {"x": 0, "y": 0, "z": 0}}
    with pytest.raises(ValidationError):
        parse_game_message(_json(deed))
    with pytest.raises(ValidationError):
        parse_game_message(_json({**deed, "culprit": "VINO", "kind": "murder"}))


@pytest.mark.parametrize("args", [
    {"chest": "CHEST_LOBART", "what": "ITFO_BREAD", "count": 2, "deed": True},
    {"item": "ITFO_BREAD", "what": "ITFO_BREAD", "count": 1, "deed": True},
])
def test_v47_steal_command_preserves_native_target_and_deed_mark(args):
    cmd = NpcActCmd(id="n1", npc="VINO", action="steal", args=args)
    assert json.loads(cmd.to_json())["args"] == args


def test_v47_fight_command_preserves_optional_reason_and_deed_mark():
    cmd = NpcActCmd(id="n1", npc="VINO", action="fight",
                    args={"npc": "MALAK", "reason": "AR_THEFT", "deed": True})
    assert json.loads(cmd.to_json())["args"] == {
        "npc": "MALAK", "reason": "AR_THEFT", "deed": True,
    }


def test_v47_storyguard_reads_the_actual_steal_target_before_commanding_the_body():
    target = {"npc": "VINO", "chest": "CHEST_LOBART", "what": "ITFO_BREAD", "count": 1}
    command = StoryStateCmd(req="s1", deed_target=target)
    assert json.loads(command.to_json())["deed_target"] == target
    assert "deed_target" not in json.loads(StoryStateCmd(req="s2").to_json())


@pytest.mark.parametrize("theft,damage", [(True, False), (False, True), (None, None)])
def test_v47_other_damage_readiness_is_independent_of_theft(theft, damage):
    view = {"npc": "HILDA", "name": "Хильда"}
    if theft is not None:
        view.update(theft_ready=theft, others_damage_ready=damage, others_damage_range=800.0)
    message = parse_game_message(_json({"type": "npc_views", "t": 1, "views": [view]}))
    assert isinstance(message, NpcViews)
    assert message.views[0].theft_ready is theft
    assert message.views[0].others_damage_ready is damage
    assert message.views[0].others_damage_range == (800.0 if theft is not None else None)


@pytest.mark.parametrize("ready,distance", [(True, 1800.0), (False, 2500.0), (None, None)])
def test_v48_theft_range_is_observed_even_without_a_ready_callback(ready, distance):
    view = {"npc": "HILDA", "name": "Хильда"}
    if distance is not None:
        view.update(theft_ready=ready, theft_range=distance)
    message = parse_game_message(_json({"type": "npc_views", "t": 1, "views": [view]}))
    assert isinstance(message, NpcViews)
    assert message.views[0].theft_ready is ready
    assert message.views[0].theft_range == distance


def test_v48_chest_take_names_the_source_and_keeps_self_as_taker():
    assert PROTOCOL_VERSION >= NPC_FAR_PROTOCOL == 48
    take = StoryEffectCmd(
        req="far-theft", npc="BAU_951_HILDA",
        effects=[{"do": "chest_take", "chest": "CHEST_LOBART", "item": "ITMI_GOLD", "count": 2}],
        check=True,
    )
    assert json.loads(take.to_json()) == {
        "type": "story_effect", "req": "far-theft", "npc": "BAU_951_HILDA", "check": True,
        "effects": [{"do": "chest_take", "chest": "CHEST_LOBART", "item": "ITMI_GOLD", "count": 2}],
    }


def test_v48_story_state_preserves_batch_world_positions_and_legacy_absence():
    command = StoryStateCmd(req="far-positions", npcs=["HILDA", "LOBART", "PC_HERO"])
    assert json.loads(command.to_json())["npcs"] == ["HILDA", "LOBART", "PC_HERO"]
    people = {
        "HILDA": {"state": "alive", "name": "Хильда", "pos": [120.5, 10.0, -500.0]},
        "LOBART": {"state": "absent"},
        "PC_HERO": {"state": "alive", "name": "Я", "pos": [30000.0, 20.0, 18000.0]},
    }
    message = parse_game_message(_json({
        "type": "result", "req": command.req, "ok": True, "value": {"npcs": people},
    }))
    assert message.type == "result" and message.value["npcs"] == people
    legacy = parse_game_message(_json({
        "type": "result", "req": "old", "ok": True, "value": {"npcs": {"HILDA": {"state": "alive"}}},
    }))
    assert "pos" not in legacy.value["npcs"]["HILDA"]


def test_v46_held_room_entry_has_no_deed_and_uses_the_deeds_switch():
    assert PROTOCOL_VERSION >= CRIME_MEET_PROTOCOL == 46
    held = parse_game_message(_json({
        "type": "perception_held", "hold": "room1", "npc": "LOBART", "other": "PLAYER",
        "kind": "enter_room", "distance": 250,
    }))
    assert isinstance(held, PerceptionHeld)
    assert held.kind == "enter_room" and held.deed is None
    assert json.loads(ThreatControlCmd(hold=True, deeds=False).to_json()) == {
        "type": "threat_control", "hold": True, "deeds": False,
    }


def test_v43_native_combat_target_and_armed_npc_threat():
    assert PROTOCOL_VERSION >= FIGHT_REACTION_PROTOCOL == 43
    views = parse_game_message(
        _json(
            {
                "type": "npc_views",
                "t": 1000,
                "views": [
                    {
                        "npc": "CAVALORN",
                        "name": "Кавалорн",
                        "sees": [
                            {
                                "id": "WOLF",
                                "name": "Волк",
                                "kind": "monster",
                                "distance": 2300,
                                "combat_target": "PLAYER",
                                "fighting": True,
                            },
                            {"id": "BRAGO", "name": "Браго", "distance": 300},
                        ],
                    }
                ],
            }
        )
    )
    assert isinstance(views, NpcViews)
    wolf, legacy = views.views[0].sees
    assert (wolf.combat_target, wolf.fighting) == ("PLAYER", True)
    assert (legacy.combat_target, legacy.fighting) == (None, False)
    threat = parse_game_message(
        _json(
            {
                "type": "npc_event",
                "npc": "BRAGO",
                "kind": "npc_threat",
                "other": "CAVALORN",
                "distance": 1100,
                "detail": "you",
            }
        )
    )
    assert isinstance(threat, NpcEvent)
    assert (threat.kind, threat.other) == ("npc_threat", "CAVALORN")


def test_pickpocket_deed_keeps_before_and_after_evidence():
    msg = parse_game_message(
        _json(
            {
                "type": "hero_deed",
                "id": "pocket-1",
                "kind": "pickpocket",
                "victim": "BAU_950_LOBART",
                "position": {"x": 1, "y": 2, "z": 3},
                "inventory_before": [{"item": "ITMI_GOLD", "count": 5}],
                "inventory_after": [{"item": "ITMI_GOLD", "count": 85}],
                "picked_pocket_before": 0,
                "picked_pocket_after": 1,
                "caught": False,
            }
        )
    )
    assert isinstance(msg, HeroDeed)
    assert msg.victim == "BAU_950_LOBART"
    assert msg.inventory_after[0].count - msg.inventory_before[0].count == 80
    assert (msg.picked_pocket_before, msg.picked_pocket_after, msg.caught) == (0, 1, False)


def test_hero_deed_requires_known_action_and_position():
    with pytest.raises(ValidationError):
        parse_game_message(_json({"type": "hero_deed", "id": "x", "kind": "trespass"}))


def test_action_command_omits_empty_fields():
    cmd = ActionCmd(id="a1", npc="X", action="wait", args={"ms": 500})
    assert cmd.to_json() == '{"type":"action","id":"a1","npc":"X","action":"wait","args":{"ms":500}}'


def test_v15_npc_to_npc():
    say = SayCmd(
        npc="BAU_950_LOBART", line="AIB_1", text="Слыхала?", stream=True, rate=24000, to="BAU_951_HILDA"
    )
    assert '"to":"BAU_951_HILDA"' in say.to_json()
    assert '"to"' not in SayCmd(npc="X", line="AIB_2", text="Эй").to_json()  # to the hero, as before
    hush = HushCmd(npc="BAU_950_LOBART", line="AIB_1", said="Слыхала?")
    assert hush.to_json() == '{"type":"hush","npc":"BAU_950_LOBART","said":"Слыхала?","line":"AIB_1"}'
    give = NpcActCmd(id="n1", npc="X", action="give", args={"npc": "Y", "item": "ITFO_BREAD", "count": 1})
    assert '"action":"give"' in give.to_json()
    goto = DebugCmd(
        req="r1", cmd="npc_goto", npc="BAU_951_HILDA", waypoint="NW_FARM1_PATH_01", face="BAU_950_LOBART"
    )
    assert goto.to_json() == (
        '{"type":"debug","req":"r1","cmd":"npc_goto","slot":0,"waypoint":"NW_FARM1_PATH_01",'
        '"npc":"BAU_951_HILDA","face":"BAU_950_LOBART"}'
    )

    failed = parse_game_message(
        '{"type":"npc_idle","npc":"X","act":"n1","reason":"failed","detail":"could not reach the target"}'
    )
    arrived = parse_game_message('{"type":"npc_idle","npc":"X","act":"n2","reason":"arrived"}')
    assert isinstance(failed, NpcIdle) and (failed.reason, failed.detail) == (
        "failed",
        "could not reach the target",
    )
    assert isinstance(arrived, NpcIdle) and (arrived.reason, arrived.detail) == ("arrived", None)


def _json(obj) -> str:
    import json

    return json.dumps(obj)


def test_v22_things_lying_about():
    assert PROTOCOL_VERSION >= 22
    seen = parse_game_message(
        _json(
            {
                "type": "npc_views",
                "t": 1000,
                "views": [
                    {
                        "npc": "BAU_4300_ADDON_CAVALORN",
                        "name": "Кавалорн",
                        "items": [
                            {
                                "item": "ITWR_SATURASFIRSTMESSAGE_ADDON_SEALED",
                                "name": "Запечатанное послание",
                                "count": 1,
                                "distance": 120,
                                "hero_cm": 80,
                                "from_hero": "ahead",
                            }
                        ],
                    }
                ],
            }
        )
    )
    assert isinstance(seen, NpcViews)
    thing = seen.views[0].items[0]
    assert (thing.item, thing.hero_cm, thing.from_hero) == (
        "ITWR_SATURASFIRSTMESSAGE_ADDON_SEALED",
        80,
        "ahead",
    )
    assert (
        NpcViews.model_validate({"type": "npc_views", "t": 1, "views": [{"npc": "X", "name": "x"}]})
        .views[0]
        .items
        == []
    )
    assert ActionCmd(id="a1", npc="X", action="take_item", args={"item": "ITPL_BEET"}).action == "take_item"


def test_v21_wolves_hear_the_hero():
    seen = parse_game_message(
        _json(
            {
                "type": "beasts",
                "t": 1000,
                "hero_weapon": "melee",
                "wolves": [
                    {
                        "id": "WOLF#3",
                        "name": "Волк",
                        "distance": 950,
                        "state": "threaten",
                        "hp": 60,
                        "hp_max": 60,
                    },
                    {
                        "id": "YWOLF#4",
                        "name": "Молодой волк",
                        "distance": 400,
                        "state": "calm",
                        "sitting": True,
                        "focus": True,
                        "hit_ms": 120000,
                        "tolerant": True,
                    },
                ],
            }
        )
    )
    assert isinstance(seen, Beasts) and seen.hero_weapon == "melee"
    assert [(w.id, w.state, w.sitting) for w in seen.wolves] == [
        ("WOLF#3", "threaten", False),
        ("YWOLF#4", "calm", True),
    ]
    ended = parse_game_message('{"type":"beast_event","npc":"WOLF#3","kind":"calm_broken","detail":"close"}')
    assert isinstance(ended, BeastEvent) and (ended.kind, ended.detail) == ("calm_broken", "close")
    calm = BeastCmd(req="b1", npc="WOLF#3", do="calm", ms=120000, sit=True)
    assert calm.to_json() == '{"type":"beast","req":"b1","npc":"WOLF#3","do":"calm","ms":120000,"sit":true}'
    assert BeastCmd(req="b2", npc="WOLF#3", do="attack").to_json() == (
        '{"type":"beast","req":"b2","npc":"WOLF#3","do":"attack"}'
    )
    hp = DebugCmd(req="r1", cmd="player_hp", hp=5000)
    assert hp.to_json() == '{"type":"debug","req":"r1","cmd":"player_hp","slot":0,"hp":5000}'


def test_v23_programs_in_the_story_graph():
    assert PROTOCOL_VERSION >= 23
    ask = StoryProgramCmd(req="s1", node="DIA_LOBART_RUEBENRUNNING")
    assert ask.to_json() == (
        '{"type":"story_program","req":"s1","node":"DIA_LOBART_RUEBENRUNNING","part":"information","own":false}'
    )
    with pytest.raises(ValidationError):
        StoryProgramCmd(req="s2", node="X", part="choice")
    steps = [{"do": "say", "speaker": "self", "text": "Спасибо."}, {"do": "return"}]
    patch = GraphPatchCmd(ops=[{"op": "program", "node": "DIA_X", "part": "condition", "steps": steps}])
    assert '"steps":[{"do":"say","speaker":"self","text":"Спасибо."},{"do":"return"}]' in patch.to_json()


def test_v24_choices_in_the_directors_dialogs():
    assert PROTOCOL_VERSION >= 24
    pick = DebugCmd(req="r1", cmd="menu_pick", text="Золото")
    assert pick.to_json() == '{"type":"debug","req":"r1","cmd":"menu_pick","slot":0,"text":"Золото"}'
    assert DebugCmd(req="r2", cmd="choices", npc="X", node="DIR_X").to_json() == (
        '{"type":"debug","req":"r2","cmd":"choices","slot":0,"npc":"X","node":"DIR_X"}'
    )
    for cmd in ("talk", "menu"):
        assert DebugCmd(req="r3", cmd=cmd).cmd == cmd


def test_v25_the_scripts_own_program():
    assert PROTOCOL_VERSION >= 25
    ask = StoryProgramCmd(req="s1", node="DIA_LOBART_BUYCLOTHES", own=True)
    assert ask.to_json() == (
        '{"type":"story_program","req":"s1","node":"DIA_LOBART_BUYCLOTHES","part":"information","own":true}'
    )


def test_v26_the_directors_lines_in_the_npcs_voice():
    assert PROTOCOL_VERSION >= 26
    msg = parse_game_message(
        '{"type":"line_voice","npc":"NONE_100_XARDAS","name":"Ксардас","line":"AIB_G1","text":"Выбирай."}'
    )
    assert isinstance(msg, LineVoice) and (msg.line, msg.text, msg.name) == ("AIB_G1", "Выбирай.", "Ксардас")


def test_v27_whom_the_npc_looks_at():
    assert PROTOCOL_VERSION >= 27
    snap = '{"type":"snapshot","t":5,"world":{"name":"NEWWORLD.ZEN","day":0,"hour":8,"minute":30},'
    npc = '"npc":{"id":"BDT_1014_BANDIT_L","name":"Браго","pos":[1,2,3],"rot":0,"hp":10,"hp_max":10,'
    npc += '"busy":false'
    looks = parse_game_message(f'{snap}{npc},"gaze":"BAU_4300_ADDON_CAVALORN"}}}}')
    nobody = parse_game_message(f'{snap}{npc},"gaze":null}}}}')
    assert isinstance(looks, Snapshot) and looks.npc.gaze == "BAU_4300_ADDON_CAVALORN"
    assert isinstance(nobody, Snapshot) and nobody.npc.gaze is None


def test_v28_npc_chests():
    assert PROTOCOL_VERSION >= 28
    ask = NpcChestsCmd(req="c1", npc="BDT_1014_BANDIT_L")
    assert ask.to_json() == '{"type":"npc_chests","req":"c1","npc":"BDT_1014_BANDIT_L"}'
    chest = Chest.model_validate(
        {
            "id": "MOB_17897_3163_-20770",
            "why": "key",
            "pos": [17897, 3163, -20770],
            "distance": 400,
            "waypoint": "NW_XARDAS_BANDITS_RIGHT",
            "locked": True,
            "key": "ITKE_BANDIT",
            "has_key": True,
            "items": [{"item": "ITRW_BOW_L_01", "name": "Короткий лук", "count": 1}],
            "start": [{"item": "ITMI_GOLD", "name": "Золото", "count": 29}],
        }
    )
    assert chest.why == "key" and chest.items[0].item == "ITRW_BOW_L_01" and chest.start[0].count == 29
    take = DebugCmd(req="r1", cmd="chest_take", chest=chest.id, item="ITMI_GOLD", count=29)
    assert '"cmd":"chest_take"' in take.to_json() and '"count":29' in take.to_json()


def test_v29_the_directors_story_variables():
    assert PROTOCOL_VERSION >= 29
    patch = GraphPatchCmd(ops=[{"op": "vars", "vars": {"DIR_Q1": 0}}])
    assert '"ops":[{"op":"vars","vars":{"DIR_Q1":0}}]' in patch.to_json()
    take = DebugCmd(req="r1", cmd="player_take", item="ITMI_PAN", distance=300)
    assert '"cmd":"player_take"' in take.to_json() and '"distance":300' in take.to_json()


def test_v30_how_far_the_players_voice_goes():
    assert PROTOCOL_VERSION >= 30
    roster = parse_game_message(
        '{"type":"npc_roster","t":1,"npcs":['
        '{"id":"BAU_4300_ADDON_CAVALORN","name":"Кавалорн","distance":140,"heard":140},'
        '{"id":"BDT_1013_BANDIT_L","name":"Бандит","distance":3500,"heard":20000},'
        '{"id":"VLK_LOBART","name":"Лобарт","distance":900}]}'
    )
    assert isinstance(roster, NpcRoster)
    cavalorn, bandit, old = roster.npcs
    assert cavalorn.earshot == 140 and bandit.earshot == 20000  # the rock between: his voice does not reach
    assert old.heard is None and old.earshot == 900  # a bridge before v30: as the crow flies


def test_roster_accepts_a_body_control_provider():
    roster = parse_game_message(
        '{"type":"npc_roster","t":1,"npcs":['
        '{"id":"FIGHTER","name":"Боец","distance":300,"activity":"body"}]}'
    )
    assert isinstance(roster, NpcRoster)
    assert roster.npcs[0].activity == "body"


def test_v31_a_dialog_heard_in_a_conversation():
    assert PROTOCOL_VERSION >= 31
    cmd = StoryEffectCmd(req="e1", npc="BAU_4300_ADDON_CAVALORN", effects=[{"do": "knows", "info": "DIA_X"}])
    assert '"effects":[{"do":"knows","info":"DIA_X"}]' in cmd.to_json()


def test_v32_where_the_hero_faces():
    assert PROTOCOL_VERSION >= 32
    snap = '{"type":"snapshot","t":5,"world":{"name":"NEWWORLD.ZEN","day":0,"hour":8,"minute":30},'
    snap += '"npc":{"id":"BAU_4300_ADDON_CAVALORN","name":"Кавалорн","pos":[1,2,3],"rot":0,"hp":10,'
    snap += '"hp_max":10,"busy":false},"player":{"name":"Я","pos":[0,0,0],"distance":300'
    new = parse_game_message(f'{snap},"ahead":[0,1],"right":[-1,0]}}}}')
    old = parse_game_message(f"{snap}}}}}")
    assert isinstance(new, Snapshot) and new.player is not None
    assert new.player.ahead == (0, 1) and new.player.right == (-1, 0)
    assert isinstance(old, Snapshot) and old.player is not None and old.player.ahead is None


def test_v33_an_npc_of_the_directors():
    assert PROTOCOL_VERSION >= 33
    own = DebugCmd(req="r1", cmd="own_npc", npc="DIR_NPC_1", like="BAU_955_BAUER", waypoint="NW_FARM1_OUT_01")
    assert '"cmd":"own_npc"' in own.to_json() and '"like":"BAU_955_BAUER"' in own.to_json()


def test_v34_a_character_of_the_directors():
    assert PROTOCOL_VERSION >= 34
    spec = NpcSpec(
        npc="DIR_NPC_1",
        like="VLK_4000_BUERGER",
        at="NW_FARM1_PATH_01",
        name="Гарет",
        look=NpcLook(gender="MALE", head="Hum_Head_Pony", face="FACE_N_NORMAL05", skin="BODYTEX_N"),
        items=[NpcThing(item="ITMI_GOLD", count=40)],
        routine=[
            RoutineEntry(
                start="06:00", end="06:00", state="ZS_STAND_ARMSCROSSED", waypoint="NW_FARM1_PATH_01"
            )
        ],
    )
    json = StoryNpcCmd(req="n1", **spec.model_dump()).to_json()
    assert '"type":"story_npc"' in json and '"name":"Гарет"' in json and '"armor"' not in json
    assert '"routine":[{"start":"06:00","end":"06:00","state":"ZS_STAND_ARMSCROSSED"' in json


def test_v35_the_directors_triggers():
    assert PROTOCOL_VERSION >= 35
    op = triggers.trigger("DIR_Q1_T1", triggers.STATE, [], when=triggers.hero_has("ITMI_SAW"), repeat=True)
    patch = GraphPatchCmd(ops=[op]).to_json()
    assert '"op":"trigger"' in patch and '"on":"state"' in patch and '"repeat":true' in patch
    fired = parse_game_message(
        '{"type":"trigger_fired","id":"DIR_Q1_T1","count":2,"npc":"PC_HERO","other":"PC_HERO"}'
    )
    assert isinstance(fired, TriggerFired) and fired.count == 2 and fired.error is None
    assert '"cmd":"triggers"' in DebugCmd(req="r1", cmd="triggers").to_json()


def test_v36_physics_noise_and_investigation():
    assert PROTOCOL_VERSION >= NOISE_PROTOCOL == 36
    noise = parse_game_message(
        '{"type":"npc_event","npc":"X","kind":"noise","at":[100,20,300],'
        '"place":"NW_CITY_TAVERN_01","loudness":0.54,"source":"table","distance":502}'
    )
    assert isinstance(noise, NpcEvent)
    assert noise.at == (100, 20, 300) and noise.place == "NW_CITY_TAVERN_01"
    assert noise.loudness == 0.54 and noise.source == "table" and noise.distance == 502
    assert noise.other == "" and noise.other_name is None
    seen = parse_game_message('{"type":"npc_event","npc":"X","kind":"noise","other":"PLAYER","at":[1,2,3]}')
    assert isinstance(seen, NpcEvent) and seen.other == "PLAYER"
    legacy = parse_game_message('{"type":"npc_event","npc":"X","kind":"player_near"}')
    assert isinstance(legacy, NpcEvent) and legacy.other == "PLAYER" and legacy.at is None
    cmd = NpcActCmd(id="n1", npc="X", action="investigate", args={"at": [100, 20, 300], "ms": 5000})
    assert '"action":"investigate"' in cmd.to_json() and '"at":[100,20,300]' in cmd.to_json()
    assert '"cmd":"physics_noise"' in DebugCmd(req="r1", cmd="physics_noise", text="status").to_json()


def test_v36_investigation_receipt_identifies_action_without_breaking_old_bridge():
    receipt = parse_game_message(
        '{"type":"npc_act_result","id":"n1","npc":"X","status":"queued","action":"investigate"}'
    )
    assert isinstance(receipt, NpcActResult) and receipt.action == "investigate" and receipt.id == "n1"
    legacy = parse_game_message('{"type":"npc_act_result","id":"n2","npc":"X","status":"queued"}')
    assert isinstance(legacy, NpcActResult) and legacy.action is None


def test_v37_physical_traces_and_noise_clock():
    assert PROTOCOL_VERSION >= TRACE_PROTOCOL == 37
    msg = parse_game_message(
        _json(
            {
                "type": "npc_views",
                "t": 1000,
                "views": [
                    {
                        "npc": "X",
                        "name": "Корагон",
                        "time": 725,
                        "traces": [
                            {
                                "id": "NEWWORLD.ZEN:vob:42",
                                "kind": "tilted",
                                "name": "стол",
                                "waypoint": "NW_CITY_TAVERN_01",
                                "distance": 210,
                                "hero_cm": 300,
                                "from_hero": "left",
                                "age": 5,
                            }
                        ],
                    }
                ],
            }
        )
    )
    assert isinstance(msg, NpcViews)
    trace = msg.views[0].traces[0]
    assert (trace.kind, trace.name, trace.waypoint, trace.age) == (
        "tilted",
        "стол",
        "NW_CITY_TAVERN_01",
        5,
    )
    assert trace.hero_cm == 300 and trace.from_hero == "left"
    assert set(trace.model_dump()) == {
        "id",
        "kind",
        "name",
        "waypoint",
        "distance",
        "hero_cm",
        "from_hero",
        "age",
    }
    legacy = parse_game_message('{"type":"npc_views","t":1,"views":[{"npc":"X","name":"x"}]}')
    assert isinstance(legacy, NpcViews) and legacy.views[0].traces == []
    noise = parse_game_message('{"type":"npc_event","npc":"X","kind":"noise","time":725}')
    assert isinstance(noise, NpcEvent) and noise.time == 725 and noise.other == ""


def test_v38_a_guide_keeps_the_heros_pace():
    assert PROTOCOL_VERSION >= PACE_PROTOCOL == 38  # v36, v37 are the physics session's
    go = ActionCmd(id="a1", npc="BAU_954_MALETH", action="go_to_waypoint", args={"pace": "hero"})
    assert '"pace":"hero"' in go.to_json()


def test_v39_errands_mission_items_and_a_give_to_an_npc():
    assert PROTOCOL_VERSION >= ERRAND_PROTOCOL == 39
    snap = parse_game_message(_json({
        "type": "snapshot", "req": "r1", "t": 1,
        "npc": {"id": "X", "name": "Хильда", "pos": [0, 0, 0], "rot": 0, "hp": 1, "hp_max": 1, "busy": False,
                "inventory": [{"item": "ITWR_CANTHARS_KOMPROBRIEF_MIS", "name": "Письмо", "count": 1,
                               "mission": True}, {"item": "ITMI_GOLD", "name": "Золото", "count": 25}]},
        "world": {"name": "NEWWORLD.ZEN", "day": 1, "hour": 7, "minute": 0},
    }))  # fmt: skip
    assert isinstance(snap, Snapshot)
    assert [i.mission for i in snap.npc.inventory] == [True, False]
    effect = {"do": "give", "item": "ITMI_GOLD", "count": 25, "to": "npc", "npc": "BAU_950_LOBART"}
    give = StoryEffectCmd(req="e1", npc="BAU_951_HILDA", effects=[effect])
    assert '"to":"npc","npc":"BAU_950_LOBART"' in give.to_json()


@pytest.mark.parametrize("age", [0, 9, None])
def test_v40_broken_trace_has_age_and_no_culprit(age):
    assert PROTOCOL_VERSION >= BROKEN_TRACE_PROTOCOL == 40
    command = DebugCmd(req="stand", cmd="destruction_traces", text="sample")
    assert '"cmd":"destruction_traces"' in command.to_json()
    msg = parse_game_message(_json({
        "type": "npc_views", "t": 10,
        "views": [{"npc": "BAU_950_LOBART", "name": "Лобарт", "time": 725, "traces": [{
            "id": "NEWWORLD.ZEN:vob:42", "kind": "broken", "name": "сломанный забор",
            "waypoint": "NW_FARM1_OUT_02", "distance": 210, "age": age,
        }]}],
    }))
    assert isinstance(msg, NpcViews)
    trace = msg.views[0].traces[0]
    assert (trace.kind, trace.name, trace.age) == ("broken", "сломанный забор", age)
    assert trace.waypoint == "NW_FARM1_OUT_02" and trace.distance == 210
    assert set(trace.model_dump()) == {
        "id", "kind", "name", "waypoint", "distance", "hero_cm", "from_hero", "age",
    }
