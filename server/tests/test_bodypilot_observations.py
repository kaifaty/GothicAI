import asyncio
from collections import Counter

import pytest

from gothic_ai import bodypilot
from tests.bodypilot_helpers import (
    eventually,
    fighter,
    player_row,
    running,
)
from tests.test_body_control import events


async def test_memory_only_periodic_rows_advance_policy_without_sending_commands():
    async with running() as rig:
        await rig.active()
        count = len(rig.bridge.commands("body_action"))
        own = fighter(4, owned=True, can_decide=False, action_mask_valid=False)
        rig.bridge.emit(own)
        await eventually(lambda: len(rig.policy.commands("step")) == 2)
        await asyncio.sleep(0.01)
        assert rig.policy.commands("step")[-1]["observation"] == own
        assert len(rig.bridge.commands("body_action")) == count


async def test_rows_sampled_before_action_acceptance_update_memory_until_the_acceptance_step():
    async with running() as rig:
        rig.bridge.action_accepted_events = False
        await rig.active()
        first = rig.bridge.commands("body_action")[0]
        await eventually(lambda: rig.fight._decisions == 1)
        before_acceptance = fighter(4, owned=True, sim_step=7)
        rig.bridge.emit(before_acceptance)
        await asyncio.sleep(0.01)
        assert len(rig.policy.commands("step")) == 1

        rig.bridge.accept_action(first, sim_step=8)
        await eventually(lambda: len(rig.policy.commands("step")) == 2)
        step = rig.policy.commands("step")[-1]
        assert step["observation"] == before_acceptance
        assert step["memory_only"] is True
        assert len(rig.bridge.commands("body_action")) == 1

        # onTick accepts the command before sampling the same simulation step.
        rig.bridge.action_accepted_events = True
        after_acceptance = fighter(5, owned=True, sim_step=8)
        rig.bridge.emit(after_acceptance)
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 2)
        assert not rig.policy.commands("step")[-1].get("memory_only", False)
        assert rig.bridge.commands("body_action")[-1]["observation_seq"] == 5
        assert rig.fight._memory_only_lag_rows == 1
        assert not rig.bridge.commands("body_release")


async def test_action_acceptance_arriving_before_the_receipt_is_retained(monkeypatch):
    async with running() as rig:
        receipt_ready = asyncio.Event()
        release_receipt = asyncio.Event()
        original_action = rig.link.body_action

        async def held_receipt(*args, **kwargs):
            result = await original_action(*args, **kwargs)
            receipt_ready.set()
            await release_receipt.wait()
            return result

        monkeypatch.setattr(rig.link, "body_action", held_receipt)
        await rig.active()
        await asyncio.wait_for(receipt_ready.wait(), 0.1)
        first = rig.bridge.commands("body_action")[0]
        assert rig.fight._accepted_command_id == first["command_id"]
        assert rig.fight._accepted_sim_step == fighter(3)["sim_step"]
        assert rig.fight._decisions == 0  # The receipt has not resumed its coroutine yet.
        release_receipt.set()
        await eventually(lambda: rig.fight._decisions == 1)
        rig.bridge.emit(fighter(4, owned=True))
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 2)
        assert not rig.bridge.commands("body_release")


async def test_action_acceptance_wait_requires_the_lease_scope_phase_and_command():
    async with running() as rig:
        await rig.acquire()
        wait = asyncio.create_task(rig.fight._wait_action_accepted(9))
        try:
            await asyncio.sleep(0)
            for changes in (
                {"entity_id": 8}, {"session_epoch": 2}, {"world_epoch": 2},
                {"control_generation": 8}, {"phase": "started"}, {"action_id": 0},
            ):
                fields = {"phase": "accepted", "action_id": 1, "command_id": 9, "sim_step": 8}
                fields.update(changes)
                row = rig.bridge.control_event(**fields)
                rig.pilot.on_message(events(
                    row, session_epoch=row["session_epoch"], world_epoch=row["world_epoch"],
                ))
                await asyncio.sleep(0)
                assert not wait.done()
                assert rig.fight._accepted_command_id == rig.fight._accepted_sim_step == 0
            rig.pilot.on_message(events(rig.bridge.control_event(
                phase="accepted", action_id=1, command_id=8, sim_step=7,
            )))
            await asyncio.sleep(0)
            assert not wait.done(), "an earlier command cannot acknowledge this action"
            rig.pilot.on_message(events(rig.bridge.control_event(
                phase="accepted", action_id=1, command_id=9, sim_step=8,
            )))
            await asyncio.wait_for(wait, 0.1)
            assert (rig.fight._accepted_command_id, rig.fight._accepted_sim_step) == (9, 8)
        finally:
            wait.cancel()
            await asyncio.gather(wait, return_exceptions=True)


async def test_continue_needs_no_acceptance_event_and_preserves_the_action_barrier():
    async with running() as rig:
        await rig.active()
        first = rig.bridge.commands("body_action")[0]
        await eventually(lambda: rig.fight._accepted_command_id == first["command_id"])
        barrier = rig.fight._accepted_sim_step
        rig.bridge.action_accepted_events = False
        rig.policy.step_response_actions.extend([0, 0])
        rig.bridge.emit(fighter(4, owned=True, action_mask=[True] + [False] * 11))
        await eventually(lambda: rig.fight._decisions == 2)
        continued = rig.bridge.commands("body_action")[-1]
        assert continued["action_id"] == 0
        rig.bridge.accept_action(continued, sim_step=999)
        assert (rig.fight._accepted_command_id, rig.fight._accepted_sim_step) == (
            first["command_id"], barrier,
        )
        rig.bridge.emit(fighter(5, owned=True, action_mask=[True] + [False] * 11))
        await eventually(lambda: rig.fight._decisions == 3)
        assert len(rig.bridge.commands("body_action")) == 3
        assert not rig.bridge.commands("body_release")


async def test_one_missing_action_acceptance_event_continues_and_is_reported(monkeypatch, caplog):
    monkeypatch.setattr(bodypilot, "BRIDGE_TIMEOUT", 0.025)
    caplog.set_level("INFO", logger="gothic_ai.bodypilot")
    async with running() as rig:
        rig.bridge.action_accepted_events = False
        await rig.active()
        await eventually(lambda: rig.fight._accept_wait_timeouts == 1)
        assert rig.fight._decisions == 1 and rig.link.body_owned("NPC")
        assert not rig.bridge.commands("body_release")
        assert rig.pilot.last_summary is None

        # A lost event leaves the last known barrier intact; the next eligible row
        # can make its ordinary decision without waiting for the missing message.
        rig.bridge.action_accepted_events = True
        rig.bridge.emit(fighter(4, owned=True))
        await eventually(lambda: rig.fight._decisions == 2)
        assert rig.bridge.commands("body_action")[-1]["observation_seq"] == 4
        assert not rig.policy.commands("step")[-1].get("memory_only", False)
        assert not rig.bridge.commands("body_release")
        await rig.pilot.aclose()
        summary = rig.pilot.last_summary
        assert summary.reason == "shutdown"
        assert summary.accept_wait_timeouts == 1
        assert summary.decisions == 2 and summary.action_rejections == ()
        assert all(decision.accepted for decision in summary.last_policy_decisions)
        assert "accept_wait_timeouts=1" in caplog.text
        assert rig.fight._accept_wait_timeouts == 0
        assert not rig.bridge.provider_owned


async def test_late_action_acceptance_after_wait_timeout_still_sets_the_memory_barrier(monkeypatch):
    monkeypatch.setattr(bodypilot, "BRIDGE_TIMEOUT", 0.025)
    async with running() as rig:
        rig.bridge.action_accepted_events = False
        await rig.active()
        first = rig.bridge.commands("body_action")[0]
        await eventually(lambda: rig.fight._accept_wait_timeouts == 1)
        before_acceptance = fighter(4, owned=True, sim_step=7)
        rig.bridge.emit(before_acceptance)
        rig.bridge.accept_action(first, sim_step=8)
        await eventually(lambda: len(rig.policy.commands("step")) == 2)
        step = rig.policy.commands("step")[-1]
        assert step["observation"] == before_acceptance and step["memory_only"] is True
        assert rig.fight._accepted_sim_step == 8
        assert len(rig.bridge.commands("body_action")) == 1

        rig.bridge.action_accepted_events = True
        rig.bridge.emit(fighter(5, owned=True, sim_step=8))
        await eventually(lambda: rig.fight._decisions == 2)
        assert rig.bridge.commands("body_action")[-1]["observation_seq"] == 5
        assert not rig.policy.commands("step")[-1].get("memory_only", False)
        assert rig.fight._accept_wait_timeouts == 1
        assert not rig.bridge.commands("body_release")


async def test_late_acceptance_before_a_new_receipt_restarts_the_full_silence_window(monkeypatch):
    monkeypatch.setattr(bodypilot, "BRIDGE_TIMEOUT", 0.025)
    async with running() as rig:
        rig.bridge.action_accepted_events = False
        await rig.active()
        first = rig.bridge.commands("body_action")[0]
        await eventually(lambda: rig.fight._accept_wait_timeouts == 1)
        receipt_ready = asyncio.Event()
        release_receipt = asyncio.Event()
        original_action = rig.link.body_action

        async def held_receipt(*args, **kwargs):
            result = await original_action(*args, **kwargs)
            receipt_ready.set()
            await release_receipt.wait()
            return result

        monkeypatch.setattr(rig.link, "body_action", held_receipt)
        rig.bridge.emit(fighter(4, owned=True))
        await asyncio.wait_for(receipt_ready.wait(), 0.1)
        second = rig.bridge.commands("body_action")[-1]
        assert second["command_id"] > first["command_id"]
        rig.bridge.accept_action(first, sim_step=8)
        accepted_at = rig.fight._last_accept_at
        assert accepted_at is not None
        assert rig.bridge.command_times("body_action")[-1] < accepted_at
        assert rig.fight._accept_wait_since is None

        release_receipt.set()
        await eventually(lambda: (
            rig.fight._awaiting_accept_command_id == second["command_id"]
            and rig.fight._accept_wait_since is not None
        ))
        assert rig.fight._accept_wait_since >= accepted_at
        rig.bridge.accept_action(second, sim_step=8)
        await eventually(lambda: rig.fight._accept_wait_since is None)
        assert rig.fight._last_accept_command_id == second["command_id"]
        assert rig.fight._accept_wait_timeouts == 1
        assert not rig.bridge.commands("body_release")


async def test_accepted_receipts_without_acceptance_events_expire_after_the_lease_window(monkeypatch):
    monkeypatch.setattr(bodypilot, "BRIDGE_TIMEOUT", 0.015)
    monkeypatch.setattr(bodypilot, "LEASE_TIMEOUT_MS", 200)
    monkeypatch.setattr(bodypilot, "HEARTBEAT_INTERVAL", 0.010)
    monkeypatch.setattr(bodypilot, "EVENTS_INTERVAL", 0.010)
    async with running() as rig:
        rig.bridge.action_accepted_events = False
        await rig.active()
        await eventually(lambda: rig.fight._accept_wait_timeouts == 1)
        for seq in range(4, 7):
            rig.bridge.emit(fighter(seq, owned=True))
            await eventually(lambda seq=seq: rig.fight._accept_wait_timeouts == seq - 2)
        assert len(rig.bridge.commands("body_action")) == 4
        assert len(rig.bridge.commands("body_heartbeat")) > 1
        assert rig.bridge.lease_expirations == 0
        await rig.ended()
        summary = rig.pilot.last_summary
        assert summary.reason == "acceptance_timeout"
        assert summary.accept_wait_timeouts == summary.decisions == 4
        assert summary.action_rejections == () and summary.policy_timeouts == 0
        assert all(decision.accepted for decision in summary.last_policy_decisions)
        assert not rig.bridge.provider_owned and rig.bridge.lease_expirations == 0


async def test_acknowledged_actions_allow_idle_longer_than_the_acceptance_silence_window(monkeypatch):
    monkeypatch.setattr(bodypilot, "LEASE_TIMEOUT_MS", 200)
    monkeypatch.setattr(bodypilot, "HEARTBEAT_INTERVAL", 0.010)
    monkeypatch.setattr(bodypilot, "EVENTS_INTERVAL", 0.010)
    async with running() as rig:
        await rig.active()
        first = rig.bridge.commands("body_action")[0]
        await eventually(lambda: rig.fight._last_accept_command_id == first["command_id"])
        assert rig.fight._accept_wait_since is None
        await asyncio.sleep(0.250)
        assert rig.fight._reason is None and rig.link.body_owned("NPC")
        assert rig.pilot.last_summary is None and rig.bridge.lease_expirations == 0
        assert not rig.fight._accept_wait_expired()
        assert rig.fight._accept_wait_timeouts == 0
        assert len(rig.bridge.commands("body_action")) == 1
        assert not rig.bridge.commands("body_release")
        rig.bridge.emit(fighter(4, owned=True))
        await eventually(lambda: rig.fight._decisions == 2)
        assert rig.bridge.commands("body_action")[-1]["observation_seq"] == 4


async def test_late_acceptance_after_the_silence_deadline_cannot_revive_the_fight(monkeypatch):
    monkeypatch.setattr(bodypilot, "BRIDGE_TIMEOUT", 0.025)
    async with running() as rig:
        rig.bridge.action_accepted_events = False
        await rig.active()
        first = rig.bridge.commands("body_action")[0]
        await eventually(lambda: rig.fight._accept_wait_timeouts == 1)
        # Deliver the late event before the next _tick can observe the expired
        # deadline. Its own guard must preserve the same terminal outcome.
        rig.fight._accept_wait_since = (
            asyncio.get_running_loop().time() - bodypilot.LEASE_TIMEOUT_MS / 1000 - 0.001
        )
        assert rig.fight._accept_wait_expired()
        rig.bridge.accept_action(first, sim_step=8)
        assert rig.fight._reason == "acceptance_timeout"
        assert rig.fight._last_accept_command_id == 0
        assert rig.fight._accept_wait_expired()
        await rig.ended()
        summary = rig.pilot.last_summary
        assert summary.reason == "acceptance_timeout"
        assert summary.accept_wait_timeouts == summary.decisions == 1
        assert summary.action_rejections == ()
        assert len(rig.bridge.commands("body_action")) == 1


async def test_reset_precedes_first_periodic_row_with_the_acquired_identity():
    async with running() as rig:
        await rig.acquire()
        await eventually(lambda: rig.policy.commands("reset"))
        rig.bridge.emit(fighter(3, owned=True, periodic=False, event_only=True))
        await asyncio.sleep(0.01)
        assert not rig.policy.commands("step")
        own = fighter(4, owned=True, can_decide=False)
        rig.bridge.emit(own)
        await eventually(lambda: rig.policy.commands("step"))
        assert rig.policy.commands("reset")[0]["initial_game_ms"] == fighter()["t_game_ms"]
        assert rig.policy.commands("step")[0]["observation"] == own
        assert not rig.bridge.commands("body_action")


async def test_lease_heartbeats_run_while_acquire_waits_for_policy_reset(monkeypatch):
    monkeypatch.setattr(bodypilot, "HEARTBEAT_INTERVAL", 0.010)
    monkeypatch.setattr(bodypilot, "SERVICE_TIMEOUT", 0.500)
    async with running() as rig:
        rig.policy.reset_waiter = asyncio.Event()
        await rig.acquire()
        await eventually(lambda: rig.policy.commands("reset"))
        rig.bridge.emit(fighter(3, owned=True))
        await eventually(lambda: len(rig.bridge.commands("body_heartbeat")) >= 3)
        assert not rig.policy.commands("step")
        assert rig.link.body_owned("NPC") and rig.bridge.lease_expirations == 0
        rig.policy.reset_waiter.set()
        await eventually(lambda: rig.bridge.commands("body_action"))
        assert [request["cmd"] for request in rig.policy.requests] == ["reset", "step"]


async def test_foreign_old_event_only_rows_do_not_reach_policy_and_packet_time_is_current():
    async with running() as rig:
        await rig.active()
        old = fighter(3, owned=True)
        rig.bridge.emit(old)  # GameLink rejects the stale packet before listeners.
        foreign = fighter(4, owned=True, entity=4)
        event_only = fighter(5, owned=True, periodic=False, event_only=True)
        rig.bridge.emit(foreign, event_only)
        own = fighter(6, owned=True)
        latest = player_row(7)
        rig.bridge.emit(own, latest)
        await eventually(lambda: len(rig.policy.commands("step")) == 2)
        assert [step["observation"]["observation_seq"] for step in rig.policy.commands("step")] == [3, 6]
        assert rig.policy.commands("step")[-1]["now_game_ms"] == latest["t_game_ms"]


async def test_every_owned_periodic_row_in_a_packet_reaches_policy_as_received():
    async with running() as rig:
        await rig.active()
        first = fighter(4, owned=True, can_decide=False)
        second = fighter(5, owned=True, can_decide=False)
        rig.bridge.emit(first, second)
        await eventually(lambda: len(rig.policy.commands("step")) == 3)
        steps = rig.policy.commands("step")[-2:]
        assert [step["observation"] for step in steps] == [first, second]
        assert all(step["now_game_ms"] == second["t_game_ms"] for step in steps)
        assert len(rig.bridge.commands("body_action")) == 1


async def test_frame_hitch_burst_updates_memory_in_order_and_only_freshest_row_acts():
    async with running() as rig:
        await rig.active()
        rows = [fighter(seq, owned=True) for seq in range(4, 10)]
        assert rows[-1]["t_game_ms"] - rows[0]["t_game_ms"] == 250
        rig.bridge.emit(*rows)
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 2)
        steps = rig.policy.commands("step")[-6:]
        assert [step["observation"] for step in steps] == rows
        assert [step.get("memory_only", False) for step in steps] == [True] * 5 + [False]
        assert all(step["now_game_ms"] == rows[-1]["t_game_ms"] for step in steps)
        assert [command["observation_seq"] for command in rig.bridge.commands("body_action")] == [3, 9]
        assert len(rig.policy.commands("reset")) == 1
        assert rig.link.body_owned("NPC") and not rig.bridge.commands("body_release")
        # The row following a burst belongs to the same uninterrupted episode.
        rig.bridge.emit(fighter(10, owned=True))
        await eventually(lambda: rig.fight._decisions == 3)
        await rig.pilot.aclose()
        summary = rig.pilot.last_summary
        assert summary.memory_only_lag_rows == 5
        assert summary.observation_too_old == summary.policy_memory_resets == 0
        assert summary.max_batch_rows == 6 and summary.max_batch_lag_game_ms == 250


@pytest.mark.parametrize("age_ms, memory_only", [(100, False), (101, True)])
async def test_latest_owned_row_uses_memory_only_after_one_hundred_game_ms(age_ms, memory_only):
    async with running() as rig:
        await rig.active()
        own = fighter(4, owned=True)
        newer = player_row(10)
        newer["t_game_ms"] = own["t_game_ms"] + age_ms
        rig.bridge.emit(own, newer)
        await eventually(lambda: len(rig.policy.commands("step")) == 2)
        if memory_only:
            await asyncio.sleep(0.01)
        else:
            await eventually(lambda: len(rig.bridge.commands("body_action")) == 2)
        step = rig.policy.commands("step")[-1]
        assert step["observation"] == own
        assert step.get("memory_only", False) is memory_only
        assert step["now_game_ms"] == newer["t_game_ms"]
        assert len(rig.bridge.commands("body_action")) == (1 if memory_only else 2)
        assert rig.link.body_owned("NPC") and not rig.bridge.commands("body_release")
        await rig.pilot.aclose()
        assert rig.pilot.last_summary.memory_only_lag_rows == int(memory_only)


async def test_legacy_service_gets_original_step_shape_and_historical_actions_are_suppressed():
    async with running() as rig:
        rig.policy.memory_only_step = False
        await rig.active()
        rows = [fighter(seq, owned=True) for seq in range(4, 10)]
        rig.bridge.emit(*rows)
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 2)
        steps = rig.policy.commands("step")
        assert all(set(step) == {"cmd", "id", "observation", "now_game_ms"}
                   for step in steps)
        assert [step["observation"] for step in steps[-6:]] == rows
        assert [command["observation_seq"] for command in rig.bridge.commands("body_action")] == [3, 9]
        assert rig.link.body_owned("NPC") and not rig.bridge.commands("body_release")
        # Even a lone historical row must not act when an old service ignores its age.
        old = fighter(10, owned=True)
        newer = player_row(12)
        newer["t_game_ms"] = old["t_game_ms"] + 101
        rig.bridge.emit(old, newer)
        await eventually(lambda: len(rig.policy.commands("step")) == 8)
        await asyncio.sleep(0.01)
        assert len(rig.bridge.commands("body_action")) == 2
        assert "memory_only" not in rig.policy.commands("step")[-1]


async def test_old_world_packets_are_excluded_without_rewinding_active_lease():
    async with running() as rig:
        await rig.active()
        old = fighter(4, owned=True, generation=0, world_epoch=0)
        rig.bridge.emit(old, generation=0)
        await asyncio.sleep(0.01)
        assert len(rig.policy.commands("step")) == 1
        assert rig.link.body_leases and not rig.bridge.commands("body_release")


@pytest.mark.parametrize("rejections", [
    ["stale_observation"] * 9,
    ["masked"] * 9,
    ["masked", "stale_observation", "expired", "expired_wall", "target_invalid",
     "command_busy", "locked", "masked", "stale_observation"],
])
async def test_transient_action_rejections_wait_for_a_fresh_row_and_success_resets_streak(rejections):
    async with running() as rig:
        await rig.active()
        rig.bridge.action_rejections.extend(rejections)
        for count, seq in enumerate(range(4, 13), 1):
            rig.bridge.emit(fighter(seq, owned=True))
            await eventually(lambda count=count: rig.fight._transient_rejections_consecutive == count)
            assert rig.link.body_owned("NPC")
            assert not rig.bridge.commands("body_release")
        rig.bridge.emit(fighter(13, owned=True, can_decide=False))
        await eventually(lambda: len(rig.policy.commands("step")) == 11)
        assert rig.fight._transient_rejections_consecutive == 9  # Memory-only is not an accepted action.
        rig.bridge.emit(fighter(14, owned=True))
        await eventually(lambda: rig.fight._decisions == 2)
        assert rig.fight._transient_rejections_consecutive == 0
        # More than ten total refusals are allowed when accepted actions reset the streak.
        rig.bridge.action_rejections.append("masked")
        rig.bridge.emit(fighter(15, owned=True))
        await eventually(lambda: rig.fight._transient_rejections_consecutive == 1)
        assert rig.link.body_owned("NPC") and not rig.bridge.commands("body_release")
        rig.bridge.emit(fighter(16, owned=True))
        await eventually(lambda: rig.fight._decisions == 3)
        assert rig.fight._transient_rejections_consecutive == 0
        rig.bridge.action_rejections.append("stale_observation")
        rig.bridge.emit(fighter(17, owned=True))
        await eventually(lambda: rig.fight._transient_rejections_consecutive == 1)
        assert rig.link.body_owned("NPC") and not rig.bridge.commands("body_release")
        row = fighter(18, owned=True)
        row["self"]["down"] = True
        rig.bridge.emit(row)
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        assert summary.reason == "npc_down" and summary.decisions == 3
        assert dict(summary.action_rejections) == Counter([*rejections, "masked", "stale_observation"])
        assert summary.transient_rejections_consecutive == 1
        assert not rig.fight._action_rejections and rig.fight._transient_rejections_consecutive == 0


@pytest.mark.parametrize("hidden_reset", [True, False])
async def test_one_old_observation_fallback_continues_and_resets_before_next_row_if_needed(hidden_reset):
    async with running() as rig:
        await rig.active()
        original_call = rig.fight.client.call
        client_timings = []

        async def call(command, **fields):
            def now_ms():
                return round((asyncio.get_running_loop().time() - rig.policy.started_at) * 1000, 3)

            timing = {"cmd": command, "seq": fields.get("observation", {}).get("observation_seq"),
                      "started_ms": now_ms()}
            client_timings.append(timing)
            try:
                return await original_call(command, **fields)
            except Exception as error:
                timing["error"] = type(error).__name__
                raise
            finally:
                timing["finished_ms"] = now_ms()
                last_response = rig.fight.client.last_response_at
                timing["last_response_ms"] = (
                    round((last_response - rig.policy.started_at) * 1000, 3) if last_response else None
                )

        rig.fight.client.call = call
        rig.policy.step_mode = "fallback"
        rig.policy.step_fallback_reason = "observation_too_old"
        rig.policy.step_hidden_reset = hidden_reset
        rig.bridge.emit(fighter(4, owned=True))
        try:
            await eventually(lambda: rig.fight._transient_rejections_consecutive == 1)
        except TimeoutError as error:
            raise AssertionError(
                f"fallback response missing: client={client_timings}; policy={rig.policy.request_timings}"
            ) from error
        assert rig.link.body_owned("NPC") and not rig.bridge.commands("body_release")
        assert len(rig.bridge.commands("body_action")) == 1
        assert len(rig.policy.commands("reset")) == 1
        rig.policy.step_mode = "normal"
        next_row = fighter(5, owned=True)
        rig.bridge.emit(next_row)
        await eventually(lambda: rig.fight._decisions == 2)
        assert rig.fight._transient_rejections_consecutive == 0
        assert [command["observation_seq"] for command in rig.bridge.commands("body_action")] == [3, 5]
        commands = [request["cmd"] for request in rig.policy.requests]
        assert commands == ["reset", "step", "step"] + (["reset"] if hidden_reset else []) + ["step"]
        if hidden_reset:
            reset = rig.policy.commands("reset")[-1]
            assert (reset["session_epoch"], reset["world_epoch"], reset["initial_game_ms"]) == (
                next_row["session_epoch"], next_row["world_epoch"], next_row["t_game_ms"],
            )
        await rig.pilot.aclose()
        summary = rig.pilot.last_summary
        assert summary.observation_too_old == summary.fallbacks == 1
        assert summary.policy_memory_resets == int(hidden_reset)
        assert summary.transient_rejections_consecutive == 0
        assert summary.memory_only_lag_rows == 0


async def test_ten_old_observation_fallbacks_end_fight_and_report_memory_resets(caplog):
    caplog.set_level("INFO", logger="gothic_ai.bodypilot")
    async with running() as rig:
        await rig.active()
        rig.policy.step_mode = "fallback"
        rig.policy.step_fallback_reason = "observation_too_old"
        for count, seq in enumerate(range(4, 13), 1):
            rig.bridge.emit(fighter(seq, owned=True))
            await eventually(lambda count=count: rig.fight._transient_rejections_consecutive == count)
            assert rig.link.body_owned("NPC") and not rig.bridge.commands("body_release")
        rig.bridge.emit(fighter(13, owned=True))
        await rig.ended()
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        assert summary.reason == "policy_fallback:observation_too_old"
        assert summary.decisions == 1 and len(rig.bridge.commands("body_action")) == 1
        assert summary.observation_too_old == summary.policy_memory_resets == summary.fallbacks == 10
        assert summary.transient_rejections_consecutive == 10
        assert len(rig.policy.commands("reset")) == 10
        assert summary.release_confirmed and summary.watch_removed
        assert "observation_too_old=10" in caplog.text and "policy_memory_resets=10" in caplog.text


@pytest.mark.parametrize("last_failure", ["observation_too_old", "masked"])
async def test_old_observation_and_bridge_rejections_share_consecutive_limit(last_failure):
    async with running() as rig:
        await rig.active()
        rig.policy.step_fallback_reason = "observation_too_old"
        failures = ["observation_too_old", "masked"] * 4 + ["masked", last_failure]
        for count, (seq, failure) in enumerate(zip(range(4, 14), failures, strict=True), 1):
            rig.policy.step_mode = "fallback" if failure == "observation_too_old" else "normal"
            if failure == "masked":
                rig.bridge.action_rejections.append("masked")
            rig.bridge.emit(fighter(seq, owned=True))
            if count < 10:
                await eventually(lambda count=count: rig.fight._transient_rejections_consecutive == count)
                assert rig.link.body_owned("NPC") and not rig.bridge.commands("body_release")
        await rig.ended()
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        expected_old = failures.count("observation_too_old")
        assert summary.transient_rejections_consecutive == 10
        assert summary.observation_too_old == summary.policy_memory_resets == expected_old
        assert dict(summary.action_rejections) == {"masked": failures.count("masked")}
        assert summary.reason == (
            "policy_fallback:observation_too_old" if last_failure == "observation_too_old"
            else "body_action_rejected"
        )
        assert summary.decisions == 1 and summary.release_confirmed and summary.watch_removed


@pytest.mark.parametrize("rejections", [
    ["stale_observation"] * 10,
    ["masked"] * 10,
    ["masked", "stale_observation"] * 5,
])
async def test_ten_consecutive_transient_action_rejections_restore_native_and_are_reported(
    caplog, rejections,
):
    caplog.set_level("INFO", logger="gothic_ai.bodypilot")
    async with running() as rig:
        await rig.active()
        rig.bridge.action_rejections.extend(rejections)
        for count, seq in enumerate(range(4, 13), 1):
            rig.bridge.emit(fighter(seq, owned=True))
            await eventually(lambda count=count: rig.fight._transient_rejections_consecutive == count)
            assert not rig.bridge.commands("body_release")
        rig.bridge.emit(fighter(13, owned=True))
        await rig.ended()
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        assert summary.reason == "body_action_rejected" and summary.decisions == 1
        assert dict(summary.action_rejections) == Counter(rejections)
        assert summary.transient_rejections_consecutive == 10
        assert summary.release_confirmed and summary.watch_removed
        assert f"action_rejections={dict(sorted(Counter(rejections).items()))}" in caplog.text
        assert "transient_rejections_consecutive=10" in caplog.text


@pytest.mark.parametrize("reason", [
    "not_owned", "stale_generation", "owner_mismatch", "invalid_command", "weapon_mode", "paused_or_failed",
])
async def test_nontransient_action_rejection_still_ends_immediately(reason):
    async with running() as rig:
        await rig.active()
        rig.bridge.action_rejections.append(reason)
        rig.bridge.emit(fighter(4, owned=True))
        await rig.ended()
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        assert summary.reason == "body_action_rejected" and summary.decisions == 1
        assert dict(summary.action_rejections) == {reason: 1}
        assert summary.transient_rejections_consecutive == 0
