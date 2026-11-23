import asyncio
import json
import socket

import pytest

from gothic_ai import bodypilot
from gothic_ai.protocol import (
    PROTOCOL_VERSION,
    Hello,
    parse_game_message,
)
from tests.bodypilot_helpers import (
    eventually,
    fighter,
    player_row,
    running,
)
from tests.test_body_control import events


@pytest.mark.parametrize(
    ("requested", "body_net", "has_url", "selected"),
    [
        ("engine", {"loaded": True}, False, "engine"),
        ("engine", {"loaded": False, "error": "missing weights"}, True, "service"),
        ("engine", {"loaded": False, "error": "missing weights"}, False, None),
        ("engine", None, False, None),
        ("service", {"loaded": True}, True, "service"),
        ("service", {"loaded": True}, False, None),
    ],
)
async def test_policy_route_uses_current_hello_once_per_connection(
    caplog, requested, body_net, has_url, selected,
):
    async with running(body_policy=requested, body_net=body_net, has_url=has_url) as rig:
        assert rig.pilot.policy == selected
        if selected is None:
            rig.roster()
            rig.combat()
            rig.combat()
            assert not rig.pilot.fights and not rig.bridge.commands("body_watch")
        warnings = [record.message for record in caplog.records if "engine unavailable" in record.message]
        unavailable = requested == "engine" and not (body_net and body_net["loaded"])
        assert len(warnings) == int(unavailable)
        if unavailable:
            assert (body_net["error"] if body_net else "body_net missing from hello") in warnings[0]
        rig.link._dispatch(Hello(
            type="hello", protocol=PROTOCOL_VERSION, client="reconnected", codepage="utf-8",
            body_net={"loaded": True},
        ))
        assert rig.pilot.policy == ("engine" if requested == "engine" else selected)


@pytest.mark.parametrize(
    ("end", "restore_first", "final_summary"),
    [("npc_down", False, False), ("policy_fallback:policy_error", False, False), *[
        (f"policy_fallback:{reason}", restore_first, True)
        for reason in ("policy_error", "body_action_rejected")
        for restore_first in (False, True)
    ]],
)
async def test_engine_lease_heartbeats_and_interval_summaries_without_service_or_actions(
    end, restore_first, final_summary, caplog,
):
    caplog.set_level("INFO", logger="gothic_ai.bodypilot")
    async with running(body_net={"loaded": True}, has_url=False) as rig:
        await rig.acquire()
        await eventually(lambda: len(rig.bridge.commands("body_heartbeat")) >= 2)
        lease = rig.fight.lease
        assert rig.bridge.commands("body_acquire")[0]["policy"] == "engine"
        assert rig.fight.client is None and rig.fight._policy_watchdog_task is None
        assert not rig.policy.connections and not rig.bridge.commands("body_action")

        def publish(**updates):
            fields = {
                "type": "body_policy", "session_epoch": lease.session_epoch,
                "world_epoch": lease.world_epoch, "entity_id": lease.entity_id,
                "control_generation": lease.control_generation, "goal_version": lease.goal_version,
                "target_entity_id": lease.target_entity_id, "t_game_ms": 1000, "t_wall_ms": 1000,
                "decisions": 6, "fallbacks": {}, "steps": 7, "p50_us": 50, "p99_us": 300,
                "selected_actions": {"attack_front": 5, "wait_200": 2},
                "accepted_actions": {"attack_front": 4, "wait_200": 2}, "hidden_reset": 1,
                "final": False, "reason": "",
            }
            rig.link._dispatch(parse_game_message(json.dumps({**fields, **updates})))

        publish(entity_id=lease.entity_id + 1)
        publish(control_generation=lease.control_generation - 1)
        publish()
        publish()  # A repeated interval must not count twice.
        transient_refusals = {"command_busy": 1, "stale_observation": 1}
        publish(
            t_wall_ms=2000, decisions=8, steps=10, p50_us=40, p99_us=200,
            fallbacks=transient_refusals, selected_actions={"approach": 8},
            accepted_actions={"approach": 8}, hidden_reset=0,
        )
        assert rig.fight._fallbacks == 0
        refusals = {"masked": 10} if end.endswith("body_action_rejected") else {"policy_error": 1}
        if end.startswith("policy_fallback:"):
            if restore_first:
                rig.bridge.restore_native(release_reason=end)
                rig.link._dispatch(events(rig.bridge.control_events.popleft()))
            if final_summary:
                publish(
                    t_wall_ms=2100, decisions=3, steps=4, final=True,
                    fallbacks=refusals, reason=end,
                    selected_actions={"attack_front": 3, "wait_200": 1},
                    accepted_actions={"attack_front": 2, "wait_200": 1}, hidden_reset=0,
                )
            if not restore_first:
                rig.bridge.restore_native(release_reason=end)
        else:
            down = fighter(4, owned=True)
            down["self"]["down"] = True
            rig.bridge.emit(down)
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        assert summary.policy == "engine" and summary.reason == end
        assert summary.decisions == (17 if final_summary else 14)
        assert summary.engine_steps == (21 if final_summary else 17)
        assert dict(summary.selected_actions) == {
            "approach": 8, "attack_front": 8 if final_summary else 5,
            "wait_200": 3 if final_summary else 2,
        }
        assert dict(summary.accepted_actions) == {
            "approach": 8, "attack_front": 6 if final_summary else 4,
            "wait_200": 3 if final_summary else 2,
        }
        assert summary.policy_memory_resets == 1
        assert summary.engine_max_interval_p50_us == 50 and summary.engine_p99_us == 300
        assert summary.fallbacks == int(end.startswith("policy_fallback:"))
        expected_refusals = transient_refusals | (refusals if summary.fallbacks else {})
        assert summary.policy_fallbacks == tuple(sorted(expected_refusals.items()))
        assert summary.release_confirmed and summary.release_seconds < 2 and summary.watch_removed
        assert not rig.policy.requests and not rig.bridge.commands("body_action")
        assert any(
            f"decisions={summary.decisions}" in record.message and "policy=engine" in record.message
            for record in caplog.records if record.message.startswith("neural-body end ")
        )


@pytest.mark.parametrize("late_count", [1, 3])
async def test_late_step_replies_do_not_act_or_steal_the_next_request_and_timely_reply_resets(late_count):
    async with running() as rig:
        await rig.active()
        rig.policy.step_delays.extend([0.120] * late_count)
        # If an old reply were matched to a new row, this invalid action would
        # terminate the pilot. Only the timely reply authorizes the legal action.
        rig.policy.step_response_actions.extend([11] * late_count + [1])
        for count, seq in enumerate(range(4, 4 + late_count), 1):
            rig.bridge.emit(fighter(seq, owned=True))
            await eventually(lambda count=count: rig.fight._policy_timeouts == count)
            assert rig.fight._policy_timeouts_consecutive == count
            assert rig.link.body_owned("NPC")
            assert len(rig.bridge.commands("body_action")) == 1
            assert not rig.bridge.commands("body_release")
        fresh_seq = 4 + late_count
        rig.bridge.emit(fighter(fresh_seq, owned=True))
        await eventually(lambda: rig.fight._decisions == 2)
        assert rig.fight._policy_timeouts_consecutive == 0
        assert rig.fight._policy_watchdog_task is None
        assert not rig.fight.client.pending
        assert [cmd["observation_seq"] for cmd in rig.bridge.commands("body_action")] == [3, fresh_seq]

        # A later isolated timeout starts a new streak rather than exhausting the
        # earlier one. A memory-only timely reply also resets that streak.
        rig.policy.step_delays.append(0.120)
        rig.policy.step_response_actions.extend([11, 1])
        rig.bridge.emit(fighter(fresh_seq + 1, owned=True))
        await eventually(lambda: rig.fight._policy_timeouts == late_count + 1)
        assert rig.fight._policy_timeouts_consecutive == 1
        rig.bridge.emit(fighter(fresh_seq + 2, owned=True, can_decide=False))
        await eventually(lambda: rig.fight._policy_timeouts_consecutive == 0)
        assert len(rig.bridge.commands("body_action")) == 2
        row = fighter(fresh_seq + 3, owned=True)
        row["self"]["down"] = True
        rig.bridge.emit(row)
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        assert summary.reason == "npc_down" and summary.decisions == 2
        assert summary.policy_timeouts == late_count + 1 and summary.policy_timeouts_consecutive == 0
        assert summary.policy_timeout_wait_p95_ms is not None
        assert summary.policy_response_silence_ms is None
        assert summary.release_confirmed and summary.watch_removed
        assert rig.fight._policy_timeouts == rig.fight._policy_timeouts_consecutive == 0


async def test_five_consecutive_late_steps_end_and_restore_native_before_any_late_action(caplog):
    caplog.set_level("INFO", logger="gothic_ai.bodypilot")
    async with running() as rig:
        await rig.active()
        rig.policy.step_delays.extend([0.120] * 5)
        rig.policy.step_response_actions.extend([11] * 5)
        for count, seq in enumerate(range(4, 8), 1):
            rig.bridge.emit(fighter(seq, owned=True))
            await eventually(lambda count=count: rig.fight._policy_timeouts_consecutive == count)
            assert not rig.bridge.commands("body_release")
        rig.bridge.emit(fighter(8, owned=True))
        await rig.ended()
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        assert summary.reason == "policy_timeout" and summary.decisions == 1
        assert summary.policy_timeouts == summary.policy_timeouts_consecutive == 5
        assert summary.policy_timeout_wait_p95_ms is not None
        assert summary.release_confirmed and summary.watch_removed
        assert len(rig.bridge.commands("body_action")) == 1
        assert "policy_timeouts=5 policy_timeouts_consecutive=5" in caplog.text


async def test_one_second_without_policy_response_ends_even_without_more_observations():
    async with running() as rig:
        await rig.active()
        rig.policy.step_mode = "timeout"
        rig.bridge.emit(fighter(4, owned=True))
        await eventually(lambda: rig.fight._policy_timeouts == 1)
        assert rig.link.body_owned("NPC") and not rig.bridge.commands("body_release")
        # The fake game sends no more rows, and its injected game clock never
        # advances. The service-silence guard must still use wall time.
        await rig.ended()
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        assert summary.reason == "policy_timeout"
        assert summary.policy_timeouts == summary.policy_timeouts_consecutive == 1
        assert summary.policy_response_silence_ms >= 1000
        assert len(rig.policy.commands("step")) == 2
        assert len(rig.bridge.commands("body_action")) == 1
        assert summary.release_confirmed and summary.watch_removed


@pytest.mark.parametrize("mode", ["fallback", "error", "disconnect"])
async def test_service_failure_releases_to_native_and_does_not_block_loop(mode):
    async with running() as rig:
        await rig.active()
        rig.policy.step_mode = mode
        rig.bridge.emit(fighter(4, owned=True))
        await eventually(lambda: len(rig.policy.commands("step")) == 2)
        # A hung service must not hold the WebSocket receive loop or other server tasks.
        marker = asyncio.Event()
        asyncio.get_running_loop().call_soon(marker.set)
        await asyncio.wait_for(marker.wait(), 0.02)
        await rig.ended(policy=mode != "disconnect")
        assert len(rig.bridge.commands("body_action")) == 1


@pytest.mark.parametrize("slow_operation", ["connect", "reset"])
async def test_three_hundred_ms_initial_policy_setup_keeps_lease_and_starts_combat(
    monkeypatch, slow_operation,
):
    async with running() as rig:
        if slow_operation == "connect":
            open_connection = asyncio.open_connection

            async def slow_connect(*args, **kwargs):
                await asyncio.sleep(0.300)
                return await open_connection(*args, **kwargs)

            monkeypatch.setattr(asyncio, "open_connection", slow_connect)
        else:
            rig.policy.reset_delay = 0.300
        await rig.active()
        assert len(rig.bridge.acquire_grants) == len(rig.link.body_leases) == 1
        assert len(rig.policy.commands("reset")) == len(rig.bridge.commands("body_action")) == 1
        assert not rig.bridge.commands("body_release") and rig.pilot.last_summary is None
        await rig.pilot.aclose()
        summary = rig.pilot.last_summary
        assert summary.setup_timeouts == summary.policy_timeouts == 0
        assert summary.decisions == 1 and summary.release_confirmed and summary.watch_removed
        assert not rig.link.body_leases and not rig.bridge.provider_owned


@pytest.mark.parametrize("first_reply", ["missing", "late"])
async def test_initial_reset_timeout_retries_once_and_keeps_the_same_lease(
    monkeypatch, first_reply, caplog,
):
    monkeypatch.setattr(bodypilot, "SETUP_TIMEOUT", 0.050)
    caplog.set_level("INFO", logger="gothic_ai.bodypilot")
    async with running() as rig:
        if first_reply == "missing":
            rig.policy.reset_modes.extend(["timeout", "normal"])
        else:
            rig.policy.reset_delays.extend([0.075, 0])
        await rig.acquire()
        lease = rig.link.body_leases[1]
        await eventually(lambda: len(rig.policy.commands("reset")) == 2)
        await eventually(lambda: rig.fight.client is not None and not rig.fight.client.pending)
        assert rig.fight.lease == lease and rig.link.body_leases == {1: lease}
        assert not rig.bridge.commands("body_release") and not rig.policy.commands("step")
        rig.bridge.emit(fighter(3, owned=True))
        await eventually(lambda: rig.bridge.commands("body_action"))
        first, second = rig.policy.commands("reset")
        assert first["id"] != second["id"]
        assert {key: value for key, value in first.items() if key != "id"} == {
            key: value for key, value in second.items() if key != "id"
        }
        assert len(rig.policy.connections) == len(rig.bridge.acquire_grants) == 1
        await rig.pilot.aclose()
        summary = rig.pilot.last_summary
        assert summary.setup_timeouts == 1 and summary.policy_timeouts == 0
        assert summary.decisions == 1 and summary.reason == "shutdown"
        assert summary.release_confirmed and summary.watch_removed
        assert "setup_timeouts=1" in caplog.text


@pytest.mark.parametrize("mode", ["error", "timeout"])
async def test_reset_failure_restores_native_before_any_step(monkeypatch, mode, caplog):
    monkeypatch.setattr(bodypilot, "SETUP_TIMEOUT", 0.050)
    caplog.set_level("INFO", logger="gothic_ai.bodypilot")
    async with running() as rig:
        rig.policy.reset_mode = mode
        await rig.watch()
        rig.bridge.emit(player_row(), fighter())
        await rig.ended()
        expected_timeouts = 2 if mode == "timeout" else 0
        assert len(rig.policy.commands("reset")) == (2 if mode == "timeout" else 1)
        assert not rig.policy.commands("step")
        assert not rig.bridge.commands("body_action")
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        assert summary.reason == ("policy_timeout" if mode == "timeout" else "policy_error")
        assert summary.setup_timeouts == expected_timeouts and summary.policy_timeouts == 0
        assert summary.release_confirmed and summary.watch_removed
        assert f"setup_timeouts={expected_timeouts}" in caplog.text


async def test_initial_connect_timeout_counts_setup_failure_and_restores_native(monkeypatch):
    monkeypatch.setattr(bodypilot, "SETUP_TIMEOUT", 0.050)
    async with running() as rig:
        open_connection = asyncio.open_connection

        async def slow_connect(*args, **kwargs):
            await asyncio.sleep(0.075)
            return await open_connection(*args, **kwargs)

        monkeypatch.setattr(asyncio, "open_connection", slow_connect)
        await rig.watch()
        rig.bridge.emit(player_row(), fighter())
        await rig.ended(policy=False)
        assert not rig.policy.requests and not rig.bridge.commands("body_action")
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        assert summary.reason == "policy_timeout"
        assert summary.setup_timeouts == 1 and summary.policy_timeouts == 0
        assert summary.release_confirmed and summary.watch_removed
        assert not rig.bridge.provider_owned and not rig.link.body_leases


async def test_connection_failure_restores_native_before_first_policy_step():
    async with running() as rig:
        # A reserved local TCP port with no listening socket fails without an external service.
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reserved:
            reserved.bind(("127.0.0.1", 0))
            rig.pilot.url = f"tcp://127.0.0.1:{reserved.getsockname()[1]}"
            await rig.watch()
            rig.bridge.emit(player_row(), fighter())
            await rig.ended(policy=False)
        assert not rig.policy.requests and not rig.bridge.commands("body_action")


async def test_policy_fixture_connects_to_its_ipv4_listener_without_hostname_resolution(monkeypatch):
    async def unexpected_resolution(*args, **kwargs):
        raise AssertionError("the loopback TCP fixture must not use DNS")

    monkeypatch.setattr(asyncio.get_running_loop(), "getaddrinfo", unexpected_resolution)
    async with running() as rig:
        await rig.active()
        assert len(rig.policy.connections) == 1
        assert rig.policy.connections[0].get_extra_info("sockname")[0] == "127.0.0.1"
        assert rig.fight.client.host == "127.0.0.1"
        assert rig.pilot.handles("NPC") and rig.link.body_owned("NPC")
        assert rig.bridge.commands("body_action")[0]["target_entity_id"] == 2
