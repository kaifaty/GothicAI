import asyncio
import copy
import json
from pathlib import Path

import pytest
from pydantic import ValidationError

from gothic_ai.game import GameLink
from gothic_ai.protocol import (
    BodyObservationPacket,
    BodyObservations,
    BodyWatchCmd,
    Hello,
    Loading,
    Result,
    parse_game_message,
)


def observation(seq=1, generation=1, entity=1):
    """A complete passive wire sample with an unseen target and no privileged target features."""
    gates = {
        "fai_wait_remaining_ms": 0,
        "ani_wait_remaining_ms": 0,
        "ai_wait_remaining_ms": 0,
        "output_wait_remaining_ms": 0,
        "fai_wait": False,
        "ani_wait": False,
        "ai_wait": False,
        "output_wait": False,
    }
    own = {
        "position_world_cm": [100.0, 0.0, 300.0],
        "linear_velocity_cm_s": None,
        "velocity_valid": False,
        "yaw_rad": 0.0,
        "hp": 300,
        "hp_max": 300,
        "hp_fraction": 1.0,
        "weapon_state": 3,
        "weapon_symbol": 20,
        "weapon_length_cm": 100,
        "skill": 60,
        "skill_talent": 1,
        "skill_valid": True,
        "fight_tactic": 1,
        "body_state_raw": 0,
        "body_state_base": 0,
        "body_flags": 0,
        "queued_primitive": 0,
        "choice_id": 0,
        "gates": gates,
    }
    own.update(
        dict.fromkeys(
            [
                "dead",
                "unconscious",
                "down",
                "in_air",
                "swim",
                "dive",
                "attack_anim",
                "prehit",
                "defence_active",
                "jumpback_active",
                "def_window",
                "par_window",
                "interruptible",
            ],
            False,
        )
    )
    target = {"entity_id": 2, "present": True, "visible": False, "current_valid": False}
    target.update(
        dict.fromkeys(
            [
                "position_world_cm",
                "relative_position_cm",
                "linear_velocity_cm_s",
                "relative_velocity_cm_s",
                "yaw_error_rad",
                "fight_distance_cm",
                "attack_range_cm",
                "g_range_cm",
                "closeup_range_cm",
                "in_w_range",
                "in_g_range",
                "in_closeup",
                "focus_30",
                "focus_5",
                "melee_line_clear",
                "hp",
                "hp_max",
                "hp_fraction",
                "weapon_state",
                "body_state_raw",
                "attack_anim",
                "prehit",
                "defence_active",
                "jumpback_active",
                "down",
                "last_known_position_world_cm",
                "last_known_velocity_cm_s",
                "last_seen_game_ms",
                "last_known_age_ms",
            ],
            None,
        )
    )
    return {
        "kind": "body_observation",
        "schema_version": 1,
        "observation_seq": seq,
        "generation": generation,
        "session_epoch": 1,
        "world_epoch": generation,
        "control_generation": 0,
        "entity_id": entity,
        "script_symbol": 50,
        "phase": "post_simulation_post_animation",
        "t_game_ms": seq * 50,
        "dt_game_ms": 50,
        "sim_step": seq * 2,
        "sample_period_ms": 50,
        "periodic": True,
        "event_only": False,
        "state_changed": True,
        "motion_discontinuity": False,
        "can_decide": False,
        "control_allowed": False,
        "native_decision_eligible": False,
        "action_mask_valid": False,
        "mask_kind": "conservative_native_availability",
        "availability_mask": [False] * 12,
        "self": own,
        "target": target,
        "goal": {"kind": "native_combat", "version": 0, "target_id": 2, "move_hint": 0},
        "last_result": {"valid": False, "code": 0, "source_game_ms": 0, "source_sim_step": 0},
        "local_obstacles": {
            "ray_range_cm": 300.0,
            "rays": [{"angle_rad": i * 0.392699, "distance_cm": 300.0, "hit": False} for i in range(16)],
        },
        "events": [],
        "builder_wall_us": 40,
    }


def packet(*rows, generation=1, **updates):
    value = {
        "kind": "body_observations",
        "schema_version": 1,
        "generation": generation,
        "session_epoch": 1,
        "world_epoch": generation,
        "control_generation": 0,
        "phase": "post_simulation_post_animation",
        "sample_period_ms": 50,
        "queue_capacity": 16,
        "dropped_total": 0,
        "dropped_since_drain": 0,
        "event_dropped_total": 0,
        "observations": list(rows) or [observation(generation=generation)],
    }
    value.update(updates)
    return value


def message(value):
    return parse_game_message(json.dumps({"type": "body_observations", "packet": value}))


def test_body_packet_matches_shared_headless_schema():
    parsed = message(packet())
    assert isinstance(parsed, BodyObservations)
    row = parsed.packet.observations[0]
    assert row.self.position_world_cm == (100.0, 0.0, 300.0)
    assert row.target.present and not row.target.current_valid
    assert not row.can_decide and not row.control_allowed and not row.action_mask_valid
    assert len(row.availability_mask) == 12 and len(row.local_obstacles.rays) == 16


def test_actual_native_encoder_packet_reaches_ordinary_bridge_cache():
    # Two original rows from stage3-v1, stripped only of the runner's response envelope.
    # The ordinary bridge wraps this shared encoder output without changing numeric fields.
    path = Path(__file__).parent / "fixtures/body_observations_native.json"
    native = json.loads(path.read_text())
    parsed = message(native)
    assert parsed.packet.session_epoch == native["session_epoch"] > 1
    link = GameLink(Connection())
    link._dispatch(parsed)
    assert set(link.body_latest) == {row["entity_id"] for row in native["observations"]}
    assert link.body_session_epoch == native["session_epoch"]


def test_raw_body_listener_keeps_original_json_values_after_cache_acceptance():
    row = observation()
    row.update(snapshot_wall_monotonic_ns=123456, clock_domain="linux-clock-monotonic")
    original = packet(row)
    link = GameLink(Connection())
    received = []
    link.raw_body_listeners.append(received.append)
    link._dispatch(message(original), raw_body_packet=original)
    assert received == [original] and received[0] is original
    assert received[0]["observations"][0] is row
    assert isinstance(received[0]["observations"][0]["self"]["position_world_cm"], list)
    assert link.body_latest[1].snapshot_wall_monotonic_ns == 123456
    assert link.body_latest[1].clock_domain == "linux-clock-monotonic"

    link._dispatch(message(original), raw_body_packet=original)
    stale = packet(observation(2, generation=0), generation=0)
    link._dispatch(message(stale), raw_body_packet=stale)
    foreign = packet(observation(2), session_epoch=2)
    foreign["observations"][0]["session_epoch"] = 2
    link._dispatch(message(foreign), raw_body_packet=foreign)
    assert len(received) == 1
    assert link.body_stale_packets == 3


async def test_receive_loop_exposes_original_body_packet_without_model_repacking():
    original = packet()
    following = packet(observation(2))

    class StreamingConnection(Connection):
        async def _empty(self):
            yield json.dumps({"type": "body_observations", "packet": original})
            yield json.dumps({"type": "body_observations", "packet": following})
            yield json.dumps({"type": "loading"})

    link = GameLink(StreamingConnection())
    received = []
    dispatched = []
    ready = asyncio.get_running_loop().create_future()
    armed = asyncio.Event()

    async def observe_first_dispatch():
        armed.set()
        await ready
        latest = link.body_latest.get(1)
        return len(received), list(dispatched), latest.observation_seq if latest else None

    def receive(value):
        received.append(value)
        if len(received) == 1:
            ready.set_result(None)

    def after_dispatch(msg):
        if isinstance(msg, BodyObservations):
            dispatched.append(msg.packet.observations[0].observation_seq)
        elif isinstance(msg, Loading):
            assert not link.body_latest and link.body_generation is None
            dispatched.append("loading")

    link.raw_body_listeners.append(receive)
    link.listeners.append(after_dispatch)
    observer = asyncio.create_task(observe_first_dispatch())
    await armed.wait()
    await link.run()
    assert await observer == (1, [1], 1)
    assert received == [original, following] and received[0] is not received[1]
    assert dispatched == [1, 2, "loading"]
    assert isinstance(received[0]["observations"], list)
    assert isinstance(received[0]["observations"][0]["self"]["position_world_cm"], list)


@pytest.mark.parametrize("epoch", [0, -1, True, "1791905681806544012", 1.0])
def test_body_process_epoch_requires_a_positive_integer(epoch):
    row = observation()
    row["session_epoch"] = epoch
    with pytest.raises(ValidationError):
        message(packet(row, session_epoch=epoch))


def test_packet_cannot_mix_process_sessions():
    row = observation()
    row["session_epoch"] = 2
    with pytest.raises(ValidationError, match="mixes process sessions"):
        message(packet(row))


def test_previous_process_packet_is_rejected_until_connection_resets():
    link = GameLink(Connection())
    link._dispatch(message(packet()))
    replacement = observation()
    replacement["session_epoch"] = 2
    next_process = message(packet(replacement, session_epoch=2))
    link._dispatch(next_process)
    assert link.body_session_epoch == 1 and link.body_stale_packets == 1
    link._dispatch(Hello(type="hello", protocol=16, client="new-process", codepage="cp1251"))
    link._dispatch(next_process)
    assert link.body_session_epoch == 2 and link.body_latest[1].session_epoch == 2


@pytest.mark.parametrize("bad", [float("nan"), float("inf"), -float("inf"), "1.0", True])
def test_numeric_coordinates_reject_nonfinite_and_coerced_values(bad):
    row = observation()
    row["self"]["position_world_cm"][0] = bad
    with pytest.raises(ValidationError):
        message(packet(row))


@pytest.mark.parametrize(
    "change",
    [
        lambda row: row["availability_mask"].pop(),
        lambda row: row["local_obstacles"]["rays"].pop(),
        lambda row: row["availability_mask"].__setitem__(0, 1),
        lambda row: row.__setitem__("observation_seq", True),
        lambda row: row.__setitem__("schema_version", True),
        lambda row: row.__setitem__("can_decide", 0),
        lambda row: row.__setitem__("state_changed", 0),
        lambda row: row.__setitem__("motion_discontinuity", 1),
        lambda row: row["self"].__setitem__("skill_valid", 1),
        lambda row: row["self"].__setitem__("skill_talent", True),
        lambda row: row.__setitem__("action_mask_valid", True),
        lambda row: row.__setitem__("future_debug_payload", {"hp": 1}),
    ],
)
def test_body_sample_rejects_bad_dimensions_masks_ids_and_control(change):
    row = observation()
    change(row)
    with pytest.raises(ValidationError):
        message(packet(row))


@pytest.mark.parametrize(
    "name,value",
    [
        ("position_world_cm", [4.0, 5.0, 6.0]),
        ("hp", 300),
        ("attack_anim", False),
        ("fight_distance_cm", 200.0),
    ],
)
def test_unseen_target_cannot_leak_current_features(name, value):
    row = observation()
    row["target"][name] = value
    with pytest.raises(ValidationError):
        message(packet(row))


def test_unseen_target_can_keep_last_known_position_and_age():
    row = observation()
    row["target"].update(
        last_known_position_world_cm=[1.0, 2.0, 3.0], last_seen_game_ms=10, last_known_age_ms=40
    )
    assert message(packet(row)).packet.observations[0].target.last_known_age_ms == 40


def test_motion_discontinuity_keeps_elapsed_time_without_inventing_velocity():
    row = observation()
    row["motion_discontinuity"] = True
    parsed = message(packet(row)).packet.observations[0]
    assert parsed.dt_game_ms == 50 and parsed.motion_discontinuity
    assert not parsed.self.velocity_valid and parsed.self.linear_velocity_cm_s is None


def test_packet_rejects_mixed_generations_duplicate_sequences_and_unbounded_rows():
    invalid = [
        packet(observation(generation=2)),
        packet(observation(), observation()),
        packet(*[observation(i + 1) for i in range(17)]),
        packet(*[observation(i + 1, entity=i + 1) for i in range(5)]),
        packet(dropped_total=0, dropped_since_drain=1),
    ]
    for value in invalid:
        with pytest.raises(ValidationError):
            message(value)


def test_events_cannot_come_from_future_and_are_bounded():
    row = observation()
    event = {
        "event_id": 1,
        "source_entity_id": 1,
        "source_game_ms": 51,
        "source_sim_step": 2,
        "perspective": "self",
        "code": 1,
    }
    row["events"] = [event]
    with pytest.raises(ValidationError):
        message(packet(row))
    event["source_game_ms"] = 50
    row["events"] = [copy.deepcopy(event) for _ in range(33)]
    with pytest.raises(ValidationError):
        message(packet(row))


class Connection:
    def __init__(self):
        self.sent = []
        self.link = None
        self.reply_generation = 3
        self.send_sample_before_resume = False

    async def send(self, raw):
        cmd = json.loads(raw)
        self.sent.append(cmd)
        if self.link is not None:
            self.link._dispatch(
                Result(type="result", req=cmd["req"], ok=True, value={"generation": self.reply_generation})
            )
            if self.send_sample_before_resume:
                self.link._dispatch(message(packet(generation=self.reply_generation)))

    def __aiter__(self):
        return self._empty()

    async def _empty(self):
        if False:
            yield ""


def test_transport_bounds_history_and_rejects_duplicate_or_old_generation():
    link = GameLink(Connection())
    received = []
    link.listeners.append(received.append)
    for seq in range(1, 7):
        link._dispatch(message(packet(observation(seq))))
    assert len(link.body_latest) == 1 and link.body_latest[1].observation_seq == 6
    assert link.body_buffer_dropped_packets == 2
    assert [link.pop_body_observations().observations[0].observation_seq for _ in range(4)] == [3, 4, 5, 6]
    assert link.pop_body_observations() is None
    link._dispatch(message(packet(observation(6))))
    link._dispatch(message(packet(generation=0)))
    assert link.body_stale_packets == 2 and len(received) == 6
    link._dispatch(message(packet(observation(9))))
    assert link.body_sequence_gaps == 2
    link._dispatch(message(packet(generation=2)))
    assert link.body_generation == 2 and link.body_latest[1].observation_seq == 1
    assert link.body_buffer_dropped_packets == 0


def test_transport_does_not_grow_latest_actor_set_without_generation_reset():
    link = GameLink(Connection())
    for entity in range(1, 6):
        link._dispatch(message(packet(observation(entity, entity=entity))))
    assert len(link.body_latest) == 4 and link.body_stale_packets == 1


@pytest.mark.parametrize(
    "reset", [Loading(type="loading"), Hello(type="hello", protocol=16, client="test", codepage="cp1251")]
)
def test_loading_and_hello_invalidate_cached_body_state(reset):
    link = GameLink(Connection())
    link._dispatch(message(packet()))
    link._dispatch(reset)
    assert not link.body_latest and link.body_generation is None
    assert link.pop_body_observations() is None


async def test_disconnect_invalidates_body_state():
    link = GameLink(Connection())
    link._dispatch(message(packet()))
    await link.run()
    assert link.closed.is_set() and not link.body_latest and link.pop_body_observations() is None


async def test_watch_only_sends_passive_command_and_preserves_first_new_packet():
    conn = Connection()
    link = GameLink(conn)
    conn.link = link
    conn.send_sample_before_resume = True
    stats = await link.body_watch(["none_100_xardas"], 20)
    assert stats == {"generation": 3} and link.body_latest[1].generation == 3
    assert conn.sent == [{"type": "body_watch", "req": "b1", "npcs": ["NONE_100_XARDAS"], "rate_hz": 20}]
    conn.send_sample_before_resume = False
    conn.reply_generation = 4
    await link.body_watch([])
    assert not link.body_latest and link.body_generation == 4


@pytest.mark.parametrize(
    "npcs,rate",
    [(["X"] * 5, 20), (["x", "X"], 20), (["PLAYER", "HERO"], 20), (["X"], 30), (["X"], True), (["X"], 20.0)],
)
def test_watch_rejects_invalid_subscription(npcs, rate):
    with pytest.raises(ValidationError):
        BodyWatchCmd(req="r1", npcs=npcs, rate_hz=rate)


def test_body_parser_does_not_change_existing_message_compatibility():
    parsed = parse_game_message('{"type":"pong","req":"p1","future_field":true}')
    assert parsed.req == "p1"
    assert BodyObservationPacket.model_validate(packet()).generation == 1
