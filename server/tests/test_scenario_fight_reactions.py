import pytest

from gothic_ai.scenario_fight_reactions import CombatTrace, combat_shouts


def row(symbol=10, *, t=100, hp=100, attack=False, weapon=0, pos=(1.0, 2.0, 3.0), target=20, events=()):
    return {
        "script_symbol": symbol,
        "entity_id": symbol + 100,
        "t_game_ms": t,
        "self": {
            "hp": hp,
            "attack_anim": attack,
            "weapon_state": weapon,
            "interruptible": True,
            "position_world_cm": pos,
        },
        "target": {"entity_id": target},
        "events": list(events),
    }


def test_only_real_attack_animation_edges_count_as_strikes():
    trace = CombatTrace({10: "bandit"})
    trace.body({"observations": [row(), row(t=150, attack=True), row(t=200, attack=True)]})
    trace.body({"observations": [row(t=250), row(t=300, attack=True)]})
    assert trace.strikes == {"bandit": [150, 300]}


def test_damage_is_hp_loss_after_baseline_with_game_timestamp():
    trace = CombatTrace({10: "bandit"})
    trace.body({"observations": [row(t=100, hp=70), row(t=200, hp=60), row(t=300, hp=60)]})
    trace.body({"observations": [row(t=400, hp=100), row(t=500, hp=95)]})
    assert trace.damage == {"bandit": [(200, 10), (500, 5)]}
    assert len(trace.damage_wall["bandit"]) == 2


def test_unknown_body_cannot_supply_readiness_or_damage():
    trace = CombatTrace({10: "bandit"})
    trace.body({"observations": [row(symbol=99, hp=0, attack=True, weapon=2)]})
    assert not trace.hp
    assert not trace.strikes
    assert not trace.armed


def test_draw_time_is_first_actual_armed_state_and_distance_uses_both_bodies():
    trace = CombatTrace({10: "bandit", 11: "companion"})
    assert trace.distance("bandit", "companion") is None
    trace.body({"observations": [row(t=100), row(t=200, weapon=2), row(t=250, weapon=2)]})
    trace.body({"observations": [row(symbol=11, pos=(1.0, 2.0, 303.0))]})
    assert trace.armed == {"bandit": 200}
    assert trace.distance("bandit", "companion") == 300.0


def test_post_hit_strike_must_target_companion_at_the_time_of_its_animation():
    trace = CombatTrace({10: "bandit", 11: "companion"})
    trace.body({"observations": [row(symbol=11), row(t=150, attack=True, target=111)]})
    trace.body({"observations": [row(t=200), row(t=250, attack=True, target=20)]})
    assert trace.strike_after("bandit", "companion", 200) is None
    trace.body({"observations": [row(t=300), row(t=350, attack=True, target=111)]})
    assert trace.strike_after("bandit", "companion", 200) == 350


def test_counter_isolation_allows_only_the_confirmed_contact_at_hit_time():
    trace = CombatTrace({10: "bandit", 11: "companion"})
    contact = {
        "event_id": 1, "source_entity_id": 111, "source_game_ms": 200,
        "perspective": "self", "code": 3,
    }
    packet = {"observations": [row(symbol=11, t=250, events=[contact])]}
    trace.body(packet)
    trace.body(packet)
    assert trace.counter_interference("companion", 200, 3200) == {"attacks": [], "contacts": []}
    # An attack that began before H can still land after H: the edge alone is insufficient.
    trace.body({"observations": [row(symbol=11, t=350, events=[
        {**contact, "event_id": 2, "source_game_ms": 300},
    ])]})
    assert trace.counter_interference("companion", 200, 3200) == {"attacks": [], "contacts": [300]}


def test_counter_isolation_also_rejects_a_new_attack_without_contact():
    trace = CombatTrace({11: "companion"})
    trace.body({"observations": [row(symbol=11, t=200, attack=True)]})
    trace.body({"observations": [row(symbol=11, t=250)]})
    trace.body({"observations": [row(symbol=11, t=3201, attack=True)]})
    assert trace.counter_interference("companion", 200, 3200) == {"attacks": [200], "contacts": []}


@pytest.mark.parametrize("missing", [
    "visible", "current_valid", "in_w_range", "focus_30", "melee_line_clear",
])
def test_counter_setup_requires_a_fresh_visible_melee_target_even_when_bodies_are_close(missing):
    trace = CombatTrace({10: "bandit", 11: "companion"})
    defender = row(t=250, weapon=2, target=111)
    defender["self"].update(in_air=False, down=False, linear_velocity_cm_s=[0, 0, 0])
    defender["target"].update(
        visible=True, current_valid=True, in_w_range=True, focus_30=True, melee_line_clear=True,
    )
    trace.body({"observations": [row(symbol=11, pos=(181.0, 2.0, 3.0)), defender]})
    assert trace.counter_ready("bandit", "companion", 200)
    assert not trace.counter_ready("bandit", "companion", 250)
    defender["target"][missing] = False
    trace.body({"observations": [defender]})
    assert trace.distance("bandit", "companion") == 180.0
    assert not trace.counter_ready("bandit", "companion", 200)


def test_counter_attacker_must_stop_running_and_drifting_before_the_hit():
    trace = CombatTrace({11: "companion"})
    companion = row(symbol=11, t=250, weapon=2)
    companion["self"].update(
        body_state_base=3, queued_primitive=0, in_air=False, down=False,
        linear_velocity_cm_s=[0, 0, 360], gates={},
    )
    trace.body({"observations": [companion]})
    assert not trace.counter_attacker_ready("companion", 200)
    companion["self"]["body_state_base"] = 0
    trace.body({"observations": [companion]})
    assert not trace.counter_attacker_ready("companion", 200)
    companion["self"]["linear_velocity_cm_s"] = [0, 0, 0]
    trace.body({"observations": [companion]})
    assert trace.counter_attacker_ready("companion", 200)
    assert not trace.counter_attacker_ready("companion", 250)


def test_native_voice_requires_successful_audio_playback(tmp_path):
    game_log = tmp_path / "game.log"
    game_log.write_text(
        "[ai] BANDIT combat shout $WEAPONDOWN audio_ms=0 game_ms=100\n"
        "[ai] OTHER combat shout $WEAPONDOWN audio_ms=500 game_ms=200\n"
        "[ai] BANDIT combat shout $WEAPONDOWN audio_ms=1200 game_ms=300\n"
    )
    assert combat_shouts(game_log, "BANDIT") == [300]
    assert combat_shouts(tmp_path / "absent.log", "BANDIT") == []


def test_native_contacts_deduplicate_keep_victim_and_include_absorbed_bites():
    trace = CombatTrace({10: "wolf", 11: "companion", 12: "PLAYER"})
    trace.body({"observations": [row(symbol=10), row(symbol=11), row(symbol=12)]})
    wolf_contact = {
        "event_id": 7, "source_entity_id": 110, "source_game_ms": 200,
        "perspective": "self", "code": 3,
    }
    trace.body({"observations": [row(t=200, target=111, events=[wolf_contact])]})
    trace.body({"observations": [row(t=250, target=112, events=[wolf_contact])]})
    assert trace.contacts["wolf"] == [(200, 111)]
    assert trace.first_contact("wolf", "companion") == 200
    assert trace.first_contact("wolf", "PLAYER") is None

    wolf_received = {
        "event_id": 8, "source_entity_id": 110, "source_game_ms": 250,
        "perspective": "self", "code": 9,
    }
    trace.body({"observations": [
        row(symbol=11, t=275, target=0),
        row(t=275, hp=95, events=[wolf_received]),
    ]})
    assert trace.first_contact("companion", "wolf") is None
    assert trace.first_received_contact("wolf") == 250

    absorbed_bite = {
        "event_id": 9, "source_entity_id": 112, "source_game_ms": 300,
        "perspective": "self", "code": 10,
    }
    packet = {"observations": [row(symbol=12, t=350, hp=100, events=[absorbed_bite])]}
    trace.body(packet)
    trace.body(packet)
    assert not trace.damage.get("PLAYER")
    assert trace.received_contacts["PLAYER"] == [(300, 10)]
    assert trace.first_received_contact("PLAYER") == 300
