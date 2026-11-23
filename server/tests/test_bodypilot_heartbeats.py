import asyncio
from collections import deque
from itertools import pairwise

import pytest

from gothic_ai import bodypilot
from gothic_ai.protocol import (
    Loading,
    Result,
)
from tests.bodypilot_helpers import (
    eventually,
    fighter,
    multi_fighter,
    running,
    start_fights,
)


async def test_policy_backlog_does_not_starve_lease_heartbeats():
    async with running() as rig:
        await rig.active()
        processed = []
        rig.policy.step_delay = 0.075

        def step_received(request):
            processed.append((
                request["observation"]["observation_seq"],
                asyncio.get_running_loop().time(),
            ))

        rig.policy.on_step = step_received
        sequences = list(range(4, 13))
        rig.bridge.emit(*(fighter(seq, owned=True, can_decide=False) for seq in sequences))
        await eventually(lambda: len(processed) == len(sequences), timeout=2.0)
        assert [seq for seq, _ in processed] == sequences
        heartbeats_during_steps = [when for when in rig.bridge.command_times("body_heartbeat")
                                   if processed[0][1] <= when <= processed[-1][1]]
        assert len(heartbeats_during_steps) >= 2
        assert rig.bridge.lease_expirations == 0
        assert rig.link.body_owned("NPC")
        assert not rig.bridge.commands("body_release")


async def test_delayed_heartbeat_and_event_replies_do_not_accumulate_into_lease_expiry():
    async with running() as rig:
        rig.bridge.response_delays.update(body_heartbeat=0.200, body_events=0.240)
        await rig.active()
        assert rig.bridge.commands("body_acquire")[0]["timeout_ms"] == 2000
        try:
            await eventually(lambda: len(rig.bridge.commands("body_heartbeat")) >= 10, timeout=3.5)
        except TimeoutError as error:
            sent = rig.bridge.command_times("body_heartbeat")
            span = sent[-1] - sent[0] if sent else 0.0
            max_gap = max((later - earlier for earlier, later in pairwise(sent)), default=0.0)
            raise AssertionError(
                f"heartbeat timing: count={len(sent)} span_s={span:.3f} max_gap_s={max_gap:.3f}"
            ) from error
        sent = rig.bridge.command_times("body_heartbeat")
        assert sent[-1] - sent[0] >= 2.0
        assert max(later - earlier for earlier, later in pairwise(sent)) < 0.400
        assert len(rig.bridge.commands("body_events")) >= 3
        assert rig.bridge.lease_expirations == 0
        assert rig.link.body_owned("NPC")
        assert not rig.bridge.commands("body_release")
        assert not rig.policy.commands("end_episode")


async def test_frame_hitch_longer_than_heartbeat_timeout_keeps_lease():
    # World chunks built after a teleport held every reply for 0.6-0.7 s on Ryzen (2026-11-01).
    async with running() as rig:
        await rig.active()
        fight, lease = rig.fight, rig.fight.lease
        hitch = bodypilot.HEARTBEAT_TIMEOUT + 0.200
        rig.bridge.response_delays.update(body_heartbeat=deque([hitch]), body_events=deque([hitch]))
        beats = len(rig.bridge.commands("body_heartbeat"))
        await eventually(lambda: fight._heartbeat_timeouts == 1, timeout=2.0)
        await eventually(lambda: len(rig.bridge.commands("body_heartbeat")) >= beats + 4, timeout=2.0)
        assert fight._reason is None and rig.link.body_leases == {lease.entity_id: lease}
        assert rig.bridge.lease_expirations == 0
        assert not rig.bridge.commands("body_release") and not rig.policy.commands("end_episode")


async def test_heartbeats_without_success_for_the_lease_timeout_lose_the_lease():
    async with running() as rig:
        await rig.active()
        fight = rig.fight
        rig.bridge.response_delays["body_heartbeat"] = bodypilot.HEARTBEAT_TIMEOUT + 0.100
        limit = bodypilot.LEASE_TIMEOUT_MS / 1000
        await eventually(lambda: fight._reason == "lease_lost", timeout=limit + 1.5)
        assert fight._heartbeat_timeouts >= limit / bodypilot.HEARTBEAT_TIMEOUT


async def test_one_ownership_poll_timeout_keeps_lease_and_recovers_at_regular_interval(caplog):
    caplog.set_level("INFO", logger="gothic_ai.bodypilot")
    async with running() as rig:
        await rig.active()
        fight, lease = rig.fight, rig.fight.lease
        last_success = fight._last_events_success_at
        polls = len(rig.bridge.commands("body_events"))
        rig.bridge.response_delays["body_events"] = bodypilot.BRIDGE_TIMEOUT + 0.100
        fight._last_events_poll = float("-inf")
        fight._wake.set()
        await eventually(lambda: len(rig.bridge.commands("body_events")) == polls + 1)
        # Only this reply is delayed. It still arrives after the RPC timed out,
        # so recovery must come from a fresh poll rather than its stale result.
        rig.bridge.response_delays.clear()
        await eventually(lambda: fight._ownership_poll_timeouts == 1)
        assert fight._last_events_success_at == last_success
        assert rig.pilot.handles("NPC") and rig.link.body_leases == {lease.entity_id: lease}
        assert fight._reason is None and rig.bridge.lease_expirations == 0
        assert not rig.bridge.commands("body_release") and not rig.policy.commands("end_episode")
        await eventually(lambda: fight._last_events_success_at > last_success)
        first_poll, next_poll = rig.bridge.command_times("body_events")[polls:polls + 2]
        assert next_poll - first_poll >= bodypilot.BRIDGE_TIMEOUT + bodypilot.EVENTS_INTERVAL - 0.010
        assert any(first_poll <= when <= next_poll for when in rig.bridge.command_times("body_heartbeat"))
        assert fight._ownership_poll_timeouts == 1
        rig.bridge.emit(fighter(4, owned=True))
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 2)
        await rig.pilot.aclose()
        summary = rig.pilot.last_summary
        assert summary.ownership_poll_timeouts == 1
        assert summary.ownership_poll_max_gap_ms >= (next_poll - first_poll) * 1000
        end = [record.message for record in caplog.records if "neural-body end " in record.message]
        assert len(end) == 1 and "ownership_poll_timeouts=1" in end[0]
        assert f"ownership_poll_max_gap_ms={summary.ownership_poll_max_gap_ms:.1f}" in end[0]


async def test_shared_ownership_poll_timeout_preserves_three_fights(monkeypatch):
    async with running(multi=True) as rig:
        npcs = ("NPC_1", "NPC_2", "NPC_3")
        await start_fights(rig, *npcs)
        fights = {npc: rig.pilot.fights[npc] for npc in npcs}
        leases = dict(rig.link.body_leases)
        successes = {npc: fight._last_events_success_at for npc, fight in fights.items()}
        original_dispatch = rig.link._dispatch
        held = []
        paused = True

        def dispatch(result, **kwargs):
            if paused and isinstance(result, Result) and any(
                command["type"] == "body_events" and command["req"] == result.req
                for command in rig.bridge.sent
            ):
                held.append(result)
                return
            original_dispatch(result, **kwargs)

        monkeypatch.setattr(rig.link, "_dispatch", dispatch)
        for fight in fights.values():
            fight._last_events_poll = float("-inf")
            fight._wake.set()
        await eventually(lambda: len(held) == 3)
        await eventually(lambda: all(fight._ownership_poll_timeouts == 1 for fight in fights.values()))
        assert rig.pilot.active_count == 3 and rig.link.body_leases == leases
        assert all(rig.pilot.fights[npc] is fights[npc] and fights[npc]._reason is None for npc in npcs)
        assert not rig.bridge.commands("body_release") and not rig.policy.commands("end_episode")
        paused = False
        for result in held:
            original_dispatch(result)
        await eventually(lambda: all(
            fight._last_events_success_at > successes[npc] for npc, fight in fights.items()
        ))
        assert rig.pilot.active_count == 3 and rig.link.body_leases == leases
        rig.bridge.emit(*(multi_fighter(npc, 4, owned=True) for npc in npcs))
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 6)
        await rig.pilot.aclose()
        assert len(rig.pilot.summaries) == 3
        assert all(summary.ownership_poll_timeouts == 1 for summary in rig.pilot.summaries)


async def test_ownership_poll_silence_expires_after_two_wall_seconds_with_game_time_stopped(monkeypatch):
    # Keep the independent policy-silence watchdog out of this ownership test.
    monkeypatch.setattr(bodypilot, "POLICY_RESPONSE_TIMEOUT", 5.0)
    async with running() as rig:
        await rig.active()
        fight, lease = rig.fight, rig.fight.lease
        last_success = fight._last_events_success_at
        clock = rig.clock()
        steps, actions = len(rig.policy.commands("step")), len(rig.bridge.commands("body_action"))
        rig.bridge.response_delays["body_events"] = 5.0
        fight._last_events_poll = float("-inf")
        fight._wake.set()
        await eventually(lambda: rig.bridge.commands("body_release"), timeout=3.0)
        release_at = rig.bridge.command_times("body_release")[0]
        rig.bridge.response_delays.clear()
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        assert release_at - last_success > 2.0
        assert summary.reason == "lease_lost" and summary.end_trigger == "lease_lost"
        assert summary.ownership_poll_timeouts >= 2
        assert summary.ownership_poll_max_gap_ms > 2000
        assert rig.clock() == clock and summary.fight_seconds == 0
        assert len(rig.policy.commands("step")) == steps
        assert len(rig.bridge.commands("body_action")) == actions
        beats = [when for when in rig.bridge.command_times("body_heartbeat") if when < release_at]
        assert len(beats) >= 7 and rig.bridge.lease_expirations == 0
        assert summary.lease == lease and summary.release_confirmed and summary.watch_removed


async def test_successful_ownership_poll_arriving_after_lease_deadline_cannot_revive_fight(monkeypatch):
    monkeypatch.setattr(bodypilot, "BRIDGE_TIMEOUT", 2.5)
    monkeypatch.setattr(bodypilot, "POLICY_RESPONSE_TIMEOUT", 5.0)
    async with running() as rig:
        await rig.active()
        fight = rig.fight
        polls = len(rig.bridge.commands("body_events"))
        rig.bridge.response_delays["body_events"] = 2.1
        fight._last_events_poll = float("-inf")
        fight._wake.set()
        await eventually(lambda: len(rig.bridge.commands("body_events")) == polls + 1)
        rig.bridge.response_delays.clear()
        await eventually(lambda: rig.bridge.commands("body_release"), timeout=3.0)
        await eventually(lambda: rig.pilot.last_summary is not None, timeout=3.0)
        summary = rig.pilot.last_summary
        assert summary.reason == "lease_lost"
        assert summary.ownership_poll_timeouts == 0 and summary.ownership_poll_max_gap_ms > 2000
        assert rig.bridge.lease_expirations == 0


async def test_ownership_poll_bridge_error_still_ends_fight_immediately(caplog):
    caplog.set_level("WARNING", logger="gothic_ai.bodypilot")
    async with running() as rig:
        await rig.active()
        fight = rig.fight
        polls = len(rig.bridge.commands("body_events"))
        rig.bridge.request_rejections["body_events"] = deque(["fake ownership poll rejected"])
        fight._last_events_poll = float("-inf")
        fight._wake.set()
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        poll_at = rig.bridge.command_times("body_events")[polls]
        assert rig.bridge.command_times("body_release")[0] - poll_at < bodypilot.BRIDGE_TIMEOUT
        assert summary.reason == "lease_lost" and summary.ownership_poll_timeouts == 0
        assert summary.release_confirmed and summary.watch_removed
        failures = [record for record in caplog.records if "ownership poll failed " in record.message]
        assert len(failures) == 1 and failures[0].levelname == "WARNING"
        assert "error_type=GameError" in failures[0].message


async def test_missing_lease_during_ownership_poll_timeout_ends_fight():
    async with running() as rig:
        await rig.active()
        fight, lease = rig.fight, rig.fight.lease
        polls = len(rig.bridge.commands("body_events"))
        rig.bridge.response_delays["body_events"] = bodypilot.BRIDGE_TIMEOUT + 0.100
        fight._last_events_poll = float("-inf")
        fight._wake.set()
        await eventually(lambda: len(rig.bridge.commands("body_events")) == polls + 1)
        rig.bridge.response_delays.clear()
        rig.link.body_leases.pop(lease.entity_id)
        await eventually(lambda: rig.pilot.last_summary is not None)
        assert rig.pilot.last_summary.reason == "lease_lost"
        assert rig.pilot.last_summary.ownership_poll_timeouts == 1
        assert rig.pilot.last_summary.release_confirmed and rig.pilot.last_summary.watch_removed


@pytest.mark.parametrize("transition", ["loading", "world_changed"])
async def test_world_transition_ownership_poll_timeout_is_info_and_preserves_reason(transition, caplog):
    caplog.set_level("INFO", logger="gothic_ai.bodypilot")
    async with running() as rig:
        await rig.active()
        fight = rig.fight
        polls = len(rig.bridge.commands("body_events"))
        rig.bridge.response_delays["body_events"] = bodypilot.BRIDGE_TIMEOUT + 0.100
        fight._last_events_poll = float("-inf")
        fight._wake.set()
        await eventually(lambda: len(rig.bridge.commands("body_events")) == polls + 1)
        rig.bridge.response_delays.clear()
        if transition == "loading":
            rig.link._dispatch(Loading(type="loading"))
        else:
            rig.bridge.emit(fighter(4, owned=True, generation=2, world_epoch=2), generation=2)
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        assert summary.reason == transition and summary.ownership_poll_timeouts == 1
        polls = [record for record in caplog.records if "ownership poll " in record.message]
        assert len(polls) == 1 and polls[0].levelname == "INFO"
        assert "error_type=TimeoutError" in polls[0].message
        assert not any(record.levelname == "WARNING" for record in polls)


async def test_heartbeat_and_event_requests_overlap_with_acknowledgement_gates(monkeypatch):
    async with running() as rig:
        original_dispatch = rig.link._dispatch
        original_heartbeat = rig.link.body_heartbeat
        held_heartbeat = None
        held_event = None
        heartbeat_receipts = []
        paired_polls = []

        def dispatch(result, **kwargs):
            nonlocal held_heartbeat, held_event
            if isinstance(result, Result):
                command = next(command for command in reversed(rig.bridge.sent)
                               if command.get("req") == result.req)
                if command["type"] == "body_heartbeat":
                    assert held_heartbeat is None
                    # A main loop waiting for heartbeat before polling events
                    # would deadlock here. Its independent keepalive must overlap.
                    held_heartbeat = result
                    return
                if command["type"] == "body_events" and held_heartbeat is not None:
                    assert held_event is None
                    held_event = result
                    paired_polls.append(asyncio.get_running_loop().time())
                    reply, held_heartbeat = held_heartbeat, None
                    original_dispatch(reply)
                    return
            original_dispatch(result, **kwargs)

        async def heartbeat(*args, **kwargs):
            nonlocal held_event
            result = await original_heartbeat(*args, **kwargs)
            heartbeat_receipts.append(asyncio.get_running_loop().time())
            # The event response waits for an acknowledged heartbeat, giving
            # deterministic overlap without scheduling near the RPC deadline.
            if held_event is not None:
                reply, held_event = held_event, None
                original_dispatch(reply)
            return result

        rig.link._dispatch = dispatch
        rig.link.body_heartbeat = heartbeat
        try:
            await rig.active()
            await eventually(lambda: (
                heartbeat_receipts and held_heartbeat is None and held_event is None
                and rig.fight._last_events_success_at >= paired_polls[-1]
            ))
            first_heartbeat = len(rig.bridge.commands("body_heartbeat"))
            first_receipt, first_poll = len(heartbeat_receipts), len(paired_polls)
            loop = asyncio.get_running_loop()
            now = loop.time()

            async def advance_clock():
                nonlocal now
                while True:
                    # Drain fake bridge callbacks before advancing timer deadlines.
                    # Host scheduling pauses must not become heartbeat cadence gaps.
                    for _ in range(10):
                        await asyncio.sleep(0)
                    now += 0.005

            with monkeypatch.context() as clock:
                clock.setattr(loop, "time", lambda: now)
                clock_task = asyncio.create_task(advance_clock())
                try:
                    assert rig.bridge.commands("body_acquire")[0]["timeout_ms"] == 2000
                    await eventually(lambda: len(heartbeat_receipts) >= first_receipt + 10, timeout=3.5)
                    sent = rig.bridge.command_times("body_heartbeat")[first_heartbeat:]
                    assert len(sent) >= 10 and sent[-1] - sent[0] >= 2.0
                    assert max(later - earlier for earlier, later in pairwise(sent)) < 0.400
                    assert len(rig.bridge.commands("body_events")) >= 3
                    assert len(paired_polls[first_poll:]) == len(heartbeat_receipts[first_receipt:]) >= 10
                    assert all(ack >= poll for poll, ack in zip(
                        paired_polls, heartbeat_receipts, strict=True,
                    ))
                    assert rig.bridge.lease_expirations == 0
                    assert rig.link.body_owned("NPC")
                    assert not rig.bridge.commands("body_release")
                    assert not rig.policy.commands("end_episode")
                finally:
                    clock_task.cancel()
                    with pytest.raises(asyncio.CancelledError):
                        await clock_task
        finally:
            rig.link._dispatch = original_dispatch
            rig.link.body_heartbeat = original_heartbeat


async def test_heartbeat_keeps_active_lease_alive_and_close_restores_native(caplog):
    caplog.set_level("INFO")
    async with running() as rig:
        await rig.active()
        await eventually(lambda: rig.bridge.commands("body_heartbeat"))
        assert rig.link.body_leases
        await rig.pilot.aclose()
        await rig.ended()
        assert "p50" in caplog.text and "p95" in caplog.text
