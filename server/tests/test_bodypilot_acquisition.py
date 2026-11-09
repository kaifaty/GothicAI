import asyncio
from collections import deque

import pytest

from gothic_ai import bodypilot
from gothic_ai.protocol import (
    NpcRoster,
    RosterEntry,
)
from tests.bodypilot_helpers import (
    companion_entry,
    eventually,
    fighter,
    player_row,
    running,
)
from tests.test_body_control import event, events


async def test_acquire_immediately_resets_before_first_owned_row_and_exact_wire_actions():
    async with running() as rig:
        await rig.acquire()
        acquire = rig.bridge.commands("body_acquire")[0]
        assert acquire["mode"] == "realtime" and acquire["provider"] == "neural-body"
        assert acquire["target_entity_id"] == 2
        await eventually(lambda: rig.policy.commands("reset"))
        assert not rig.policy.commands("step")
        own = fighter(3, owned=True)
        own["snapshot_wall_monotonic_ns"] = 123456789
        own["clock_domain"] = "cxx-steady-clock"
        own["self"]["position_world_cm"] = [101.25, 0.0, 302.5]
        rig.bridge.emit(own)
        await eventually(lambda: rig.bridge.commands("body_action"))
        assert [request["cmd"] for request in rig.policy.requests[:2]] == ["reset", "step"]
        reset, step = rig.policy.requests[:2]
        assert (reset["session_epoch"], reset["world_epoch"], reset["initial_game_ms"]) == (1, 1, 100)
        assert step["observation"] == own
        assert step["now_game_ms"] == 150
        command = rig.bridge.commands("body_action")[0]
        assert {key: command[key] for key in (
            "goal_version", "target_entity_id", "observation_seq", "issued_game_ms", "ttl_ms", "action_id",
        )} == {
            "goal_version": own["goal"]["version"], "target_entity_id": own["target"]["entity_id"],
            "observation_seq": own["observation_seq"], "issued_game_ms": own["t_game_ms"],
            "ttl_ms": 100, "action_id": 1,
        }


async def test_pre_acquire_snapshot_delivered_after_acquire_keeps_lease_and_waits_for_owned_row():
    async with running() as rig:
        await rig.acquire()
        await eventually(lambda: rig.policy.commands("reset"))
        lease = rig.link.body_leases[1]
        # The bridge replies to acquire before flushing observations from the previous tick.
        passive = fighter(3)
        assert passive["control_generation"] == 0
        assert passive["observation_seq"] > lease.acquired_observation_seq
        rig.bridge.emit(passive)
        await eventually(lambda: not rig.fight._rows)
        assert rig.link.body_leases == {1: lease}
        assert rig.fight.lease == lease
        assert not rig.policy.commands("step")
        assert not rig.bridge.commands("body_release")

        own = fighter(4, owned=True)
        rig.bridge.emit(own)
        await eventually(lambda: rig.bridge.commands("body_action"))
        assert [request["cmd"] for request in rig.policy.requests] == ["reset", "step"]
        assert rig.policy.commands("reset")[0]["initial_game_ms"] == fighter()["t_game_ms"]
        assert rig.policy.commands("step")[0]["observation"] == own
        assert rig.bridge.commands("body_action")[0]["observation_seq"] == 4
        assert rig.link.body_leases == {1: lease}
        assert not rig.bridge.commands("body_release")


async def test_shutdown_during_lost_acquire_reply_reconciles_native_lease_before_clearing_watch():
    async with running() as rig:
        rig.bridge.response_delays["body_acquire"] = 0.350
        rig.bridge.acquire_roster_delay = 0.500
        await rig.watch()
        await eventually(lambda: rig.fight._watch_ready)
        rig.bridge.emit(player_row(), fighter())
        await eventually(lambda: rig.bridge.provider_owned)
        # The native actor has a lease before its result or body roster arrives.
        await asyncio.sleep(0.050)
        rig.bridge.emit(fighter(3, owned=True))
        assert not rig.link.body_leases
        await asyncio.wait_for(rig.pilot.aclose(), 1.0)
        await eventually(lambda: rig.bridge.commands("body_release"))
        await eventually(lambda: any(not cmd["npcs"] for cmd in rig.bridge.commands("body_watch")))
        await eventually(lambda: rig.pilot.active_count == 0)
        # Drain the delayed reply and roster; neither may revive the discarded lease.
        await eventually(lambda: not rig.bridge._response_tasks)
        assert not rig.bridge.provider_owned
        assert not rig.link.body_leases
        assert not rig.link.body_acquire_pending(1)
        assert rig.bridge._watched_npc is None
        assert rig.bridge.lease_expirations == 0
        assert len(rig.bridge.commands("body_acquire")) == 1
        assert not rig.bridge.commands("body_heartbeat")
        assert not rig.policy.requests
        release = rig.bridge.commands("body_release")[0]
        assert {key: release[key] for key in (
            "session_epoch", "world_epoch", "entity_id", "control_generation",
        )} == {
            "session_epoch": 1, "world_epoch": 1, "entity_id": 1, "control_generation": 0,
        }
        assert release["acquire_request_id"] == rig.bridge.commands("body_acquire")[0]["acquire_request_id"]
        release_index = rig.bridge.sent.index(release)
        unwatch_index = next(i for i, cmd in enumerate(rig.bridge.sent)
                             if cmd["type"] == "body_watch" and not cmd["npcs"])
        assert release_index < unwatch_index
        assert rig.bridge.commands("body_control_status")


@pytest.mark.parametrize("reply_delay", [0.350, 2.0])
async def test_lost_acquire_reply_recovers_the_granted_lease_and_continues(reply_delay, caplog):
    caplog.set_level("INFO", logger="gothic_ai.bodypilot")
    async with running() as rig:
        rig.bridge.response_delays["body_acquire"] = reply_delay
        rig.bridge.acquire_roster_delay = 0.500
        await rig.watch()
        await eventually(lambda: rig.fight._watch_ready)
        rig.bridge.emit(player_row(), fighter())
        await eventually(lambda: rig.bridge.provider_owned)
        await eventually(lambda: rig.policy.commands("reset"))
        await eventually(lambda: rig.fight.client is not None and not rig.fight.client.pending)
        lease = rig.fight.lease
        assert lease is not None and rig.link.body_leases == {1: lease}
        assert not rig.link.body_acquire_pending(1)
        assert (lease.session_epoch, lease.world_epoch, lease.entity_id, lease.control_generation) == (
            1, 1, 1, 7,
        )
        assert len(rig.bridge.commands("body_acquire")) == len(rig.bridge.acquire_grants) == 1
        assert rig.bridge.commands("body_control_status")
        assert not rig.bridge.commands("body_release")
        rig.bridge.emit(fighter(3, owned=True))
        await eventually(lambda: rig.bridge.commands("body_action"))
        assert rig.fight.lease == lease and rig.pilot.last_summary is None
        await rig.pilot.aclose()
        # The receipt can arrive after status recovered the lease or after shutdown.
        # Neither timing may grant again, republish the lease, or leak its native owner.
        await eventually(lambda: not rig.bridge._response_tasks, timeout=2.1)
        summary = rig.pilot.last_summary
        assert summary.acquire_requests == summary.acquire_completed == 1
        assert summary.acquire_uncertain_recovered == 1 and summary.setup_timeouts == 0
        assert summary.decisions == 1 and summary.release_confirmed and summary.watch_removed
        assert not rig.bridge.provider_owned and not rig.link.body_leases
        assert not rig.link.body_acquire_pending(1)
        assert len(rig.bridge.acquire_grants) == len(rig.bridge.commands("body_release")) == 1
        assert rig.bridge.lease_expirations == 0
        assert "acquire_uncertain_recovered=1" in caplog.text


async def test_body_roster_during_uncertain_acquire_keeps_waiting_for_the_granted_lease():
    async with running() as rig:
        rig.bridge.response_delays.update(body_acquire=0.350, body_control_status=0.150)
        rig.bridge.acquire_roster_delay = 0.800
        await rig.watch()
        await eventually(lambda: rig.fight._watch_ready)
        rig.bridge.emit(player_row(), fighter())
        await eventually(lambda: rig.bridge.commands("body_control_status"))
        assert not rig.fight._acquiring and rig.fight._pending_acquire is not None
        assert rig.fight.lease is None and rig.link.body_acquire_pending(1)
        # The native owner is already scripted, but its status response is still
        # in flight. The delayed roster must not classify this actor as unavailable.
        rig.roster(activity="body")
        assert rig.fight._reason is None and rig.pilot.active_count == 1
        assert not rig.bridge.commands("body_release")
        await eventually(lambda: rig.policy.commands("reset"))
        lease = rig.fight.lease
        assert lease is not None and rig.link.body_leases == {1: lease}
        assert not rig.link.body_acquire_pending(1)
        rig.bridge.emit(fighter(3, owned=True))
        await eventually(lambda: rig.bridge.commands("body_action"))
        assert rig.fight.lease == lease and len(rig.bridge.acquire_grants) == 1
        assert len(rig.bridge.commands("body_acquire")) == 1
        assert not rig.bridge.commands("body_release")
        await rig.pilot.aclose()
        summary = rig.pilot.last_summary
        assert summary.acquire_uncertain_recovered == 1 and summary.decisions == 1
        assert summary.release_confirmed and summary.watch_removed
        assert not rig.link.body_acquire_pending(1)
        assert not rig.link.body_leases and not rig.bridge.provider_owned


@pytest.mark.parametrize("restored_roster_activity", ["fight", "body"])
async def test_native_restoration_racing_acquire_status_does_not_resurrect_the_old_lease(
    monkeypatch, restored_roster_activity,
):
    async with running() as rig:
        rig.bridge.response_delays["body_acquire"] = deque([0.350, 0])
        rig.bridge.acquire_roster_delay = 0.800
        original_request = rig.link._request
        restored = asyncio.Event()

        async def restore_before_old_status_returns(command, timeout):
            result = await original_request(command, timeout)
            if command.type == "body_control_status" and not restored.is_set():
                assert result.value["actors"][0]["owner"] == "scripted"
                rig.bridge.provider_owned = False
                rig.bridge._lease_watchdog.cancel()
                # Ownership evidence can precede the next 500 ms roster. A
                # cached body entry must not close recovery before its retry.
                rig.bridge._activity("NPC", restored_roster_activity)
                rig.link._dispatch(events(event(
                    1, **rig.bridge._control_scope, reason="native_restored", owner="native",
                    provider="neural-body", acquire_request_id=rig.bridge._acquire_request_id,
                )))
                restored.set()
            return result

        monkeypatch.setattr(rig.link, "_request", restore_before_old_status_returns)
        await rig.watch()
        await eventually(lambda: rig.fight._watch_ready)
        rig.bridge.emit(player_row(), fighter())
        await eventually(restored.is_set)
        assert not rig.link.body_leases and rig.fight.lease is None
        assert not rig.policy.requests and not rig.bridge.commands("body_release")
        # A subsequent native snapshot resolves the request; only a fresh acquire
        # may start a policy episode after that authoritative restoration.
        await eventually(lambda: rig.fight._pending_acquire is None)
        assert not rig.link.body_acquire_pending(1)
        assert len(rig.bridge.commands("body_control_status")) >= 2
        assert not rig.link.body_leases and not rig.policy.requests
        await asyncio.sleep(0.050)
        assert rig.pilot.active_count == 1 and rig.pilot.last_summary is None
        assert rig.fight._reason is None and rig.fight._acquire_uncertain_recovered == 1
        rig.clock.advance(bodypilot.ACQUIRE_INTERVAL + 0.001)
        rig.bridge.emit(fighter(3))
        await eventually(lambda: rig.policy.commands("reset"))
        rig.bridge.emit(fighter(4, owned=True))
        await eventually(lambda: rig.bridge.commands("body_action"))
        assert len(rig.bridge.commands("body_acquire")) == len(rig.bridge.acquire_grants) == 2
        assert len(rig.policy.commands("reset")) == len(rig.link.body_leases) == 1
        assert not rig.link.body_acquire_pending(1)
        assert not rig.bridge.commands("body_release")
        await rig.pilot.aclose()
        assert rig.pilot.last_summary.release_confirmed and rig.pilot.last_summary.watch_removed
        assert not rig.link.body_acquire_pending(1)
        assert not rig.link.body_leases and not rig.bridge.provider_owned


async def test_acquired_event_recovers_the_lease_while_status_remains_unavailable():
    async with running() as rig:
        rig.bridge.response_delays.update(body_acquire=1.0, body_control_status=0.350)
        await rig.watch()
        await eventually(lambda: rig.fight._watch_ready)
        rig.bridge.emit(player_row(), fighter())
        await eventually(lambda: rig.bridge.commands("body_control_status"))
        acquire = rig.bridge.commands("body_acquire")[0]
        assert rig.bridge.provider_owned and rig.fight.lease is None
        assert rig.link.body_acquire_pending(1)
        # Another active fight's destructive poll can deliver this request's
        # acquired event while this fight is awaiting a status that times out.
        rig.bridge.control_events.append(event(
            1, **rig.bridge._control_scope, provider=acquire["provider"],
            acquire_request_id=acquire["acquire_request_id"], observation_seq=2,
            watchdog_timeout_ms=bodypilot.LEASE_TIMEOUT_MS,
        ))
        await rig.link.body_events()
        lease = rig.link.body_leases[1]
        assert not rig.link.body_acquire_pending(1)
        await eventually(lambda: rig.policy.commands("reset"), timeout=0.750)
        await eventually(lambda: rig.fight.client is not None and not rig.fight.client.pending)
        await eventually(lambda: rig.bridge.commands("body_heartbeat"))
        assert rig.fight.lease == lease and rig.link.body_leases == {1: lease}
        assert rig.fight._acquire_uncertain_recovered == 1 and rig.pilot.last_summary is None
        acquired_at = rig.bridge.command_times("body_acquire")[0]
        heartbeat_at = rig.bridge.command_times("body_heartbeat")[0]
        assert heartbeat_at - acquired_at < bodypilot.LEASE_TIMEOUT_MS / 1000
        assert len(rig.bridge.commands("body_acquire")) == len(rig.bridge.acquire_grants) == 1
        assert not rig.bridge.commands("body_release")
        rig.bridge.emit(fighter(3, owned=True))
        await eventually(lambda: rig.bridge.commands("body_action"))
        # The late acquire receipt must not replace or duplicate the event's
        # confirmed lease after policy setup and keepalives have already begun.
        await eventually(lambda: not rig.bridge._response_tasks, timeout=1.1)
        assert rig.fight.lease == lease and rig.link.body_leases == {1: lease}
        assert len(rig.policy.commands("reset")) == len(rig.bridge.acquire_grants) == 1
        assert rig.bridge.lease_expirations == 0 and rig.pilot.last_summary is None
        await rig.pilot.aclose()
        summary = rig.pilot.last_summary
        assert summary.acquire_requests == summary.acquire_completed == 1
        assert summary.acquire_uncertain_recovered == 1 and summary.decisions == 1
        assert summary.release_confirmed and summary.watch_removed
        assert not rig.link.body_acquire_pending(1)
        assert not rig.link.body_leases and not rig.bridge.provider_owned
        assert len(rig.bridge.commands("body_release")) == 1
        assert rig.bridge.lease_expirations == 0


async def test_old_native_status_cannot_clear_an_acquire_started_after_its_snapshot(monkeypatch):
    async with running() as rig:
        rig.bridge.response_delays["body_acquire"] = 0.350
        rig.bridge.acquire_roster_delay = 0.800
        await rig.watch()
        await eventually(lambda: rig.fight._watch_ready)
        original_request = rig.link._request
        status_captured, return_status = asyncio.Event(), asyncio.Event()

        async def hold_first_status(command, timeout):
            result = await original_request(command, timeout)
            if command.type == "body_control_status" and not status_captured.is_set():
                assert result.value["actors"] == []
                status_captured.set()
                await return_status.wait()
            return result

        monkeypatch.setattr(rig.link, "_request", hold_first_status)
        old_status = asyncio.create_task(rig.link.body_control_status())
        try:
            await asyncio.wait_for(status_captured.wait(), 1.0)
            rig.bridge.emit(player_row(), fighter())
            await eventually(lambda: rig.bridge.provider_owned)
            assert rig.link.body_acquire_pending(1) and rig.fight._acquiring
        finally:
            return_status.set()
            await old_status
        # This snapshot saw no request. Its late return must leave the newer
        # client-selected request ID available for lost-receipt recovery.
        assert rig.link.body_acquire_pending(1)
        assert rig.fight._pending_acquire is not None and not rig.link.body_leases
        assert not rig.policy.requests and not rig.bridge.commands("body_release")
        await eventually(lambda: rig.policy.commands("reset"))
        assert not rig.link.body_acquire_pending(1)
        assert len(rig.link.body_leases) == len(rig.bridge.acquire_grants) == 1
        rig.bridge.emit(fighter(3, owned=True))
        await eventually(lambda: rig.bridge.commands("body_action"))
        assert len(rig.bridge.commands("body_acquire")) == 1
        await rig.pilot.aclose()
        summary = rig.pilot.last_summary
        assert summary.acquire_uncertain_recovered == 1 and summary.decisions == 1
        assert summary.release_confirmed and summary.watch_removed
        assert not rig.link.body_acquire_pending(1)
        assert not rig.link.body_leases and not rig.bridge.provider_owned


async def test_lost_rejected_acquire_reply_retries_at_the_normal_interval_without_double_grant():
    async with running() as rig:
        rig.bridge.acquire_rejections.append("unsafe_boundary")
        rig.bridge.response_delays["body_acquire"] = deque([0.350, 0])
        await rig.watch()
        await eventually(lambda: rig.fight._watch_ready)
        rig.bridge.emit(player_row(), fighter())
        await eventually(lambda: rig.bridge.commands("body_control_status"))
        await eventually(lambda: not rig.link.body_acquire_pending(1))
        assert not rig.bridge.provider_owned and not rig.link.body_leases
        assert not rig.policy.requests and rig.pilot.last_summary is None
        rig.clock.advance(bodypilot.ACQUIRE_INTERVAL - 0.001)
        rig.bridge.emit(fighter(3))
        await eventually(lambda: not rig.fight._rows)
        assert len(rig.bridge.commands("body_acquire")) == 1
        rig.clock.advance(0.002)
        rig.bridge.emit(fighter(4))
        await eventually(lambda: rig.policy.commands("reset"))
        rig.bridge.emit(fighter(5, owned=True))
        await eventually(lambda: rig.bridge.commands("body_action"))
        first, second = rig.bridge.commands("body_acquire")
        assert first["acquire_request_id"] != second["acquire_request_id"]
        assert len(rig.bridge.acquire_grants) == 1
        assert rig.bridge.acquire_grants[0] is second
        assert not rig.bridge.commands("body_release")
        await eventually(lambda: not rig.bridge._response_tasks)
        assert len(rig.link.body_leases) == 1 and len(rig.policy.commands("reset")) == 1
        assert not rig.link.body_acquire_pending(1)
        await rig.pilot.aclose()
        summary = rig.pilot.last_summary
        assert summary.acquire_requests == 2 and summary.acquire_completed == 1
        assert summary.acquire_uncertain_recovered == 1
        assert summary.release_confirmed and summary.watch_removed
        assert len(rig.bridge.commands("body_release")) == 1
        assert not rig.link.body_leases and not rig.bridge.provider_owned
        assert not rig.link.body_acquire_pending(1)


@pytest.mark.parametrize("pending_outcome", ["acquired", "expired"])
async def test_lost_pending_acquire_reply_waits_for_actual_state_before_retrying(pending_outcome):
    async with running() as rig:
        rig.bridge.acquire_pending_results.append(pending_outcome)
        rig.bridge.acquire_pending_delay = 0.450
        rig.bridge.response_delays["body_acquire"] = deque([0.350, 0])
        await rig.watch()
        await eventually(lambda: rig.fight._watch_ready)
        rig.bridge.emit(player_row(), fighter())
        await eventually(lambda: rig.bridge.commands("body_control_status"))
        assert rig.bridge.pending_acquire is not None
        rig.clock.advance(bodypilot.ACQUIRE_INTERVAL + 0.001)
        rig.bridge.emit(fighter(3))
        await asyncio.sleep(0.050)
        assert len(rig.bridge.commands("body_acquire")) == 1
        assert not rig.policy.requests and not rig.bridge.commands("body_release")
        await eventually(lambda: rig.bridge.pending_acquire is None)
        if pending_outcome == "expired":
            await eventually(lambda: not rig.link.body_acquire_pending(1))
            rig.bridge.emit(fighter(4))
        await eventually(lambda: rig.policy.commands("reset"))
        rig.bridge.emit(fighter(5, owned=True))
        await eventually(lambda: rig.bridge.commands("body_action"))
        assert len(rig.bridge.commands("body_acquire")) == (1 if pending_outcome == "acquired" else 2)
        assert len(rig.bridge.acquire_grants) == 1 and len(rig.link.body_leases) == 1
        assert not rig.link.body_acquire_pending(1)
        assert not rig.bridge.commands("body_release")
        await rig.pilot.aclose()
        summary = rig.pilot.last_summary
        assert summary.acquire_uncertain_recovered == 1 and summary.acquire_completed == 1
        assert summary.release_confirmed and summary.watch_removed
        assert not rig.bridge.provider_owned and not rig.link.body_leases
        assert len(rig.bridge.commands("body_release")) == 1
        assert not rig.link.body_acquire_pending(1)


@pytest.mark.parametrize("player_near", [False, True])
async def test_native_attack_roster_starts_episode_without_hero_combat_events(player_near):
    async with running() as rig:
        rig.roster(activity="fight", state="ZS_ATTACK")
        if player_near:
            rig.combat(kind="player_near")
        await eventually(lambda: rig.bridge.commands("body_watch"))
        assert rig.bridge.commands("body_watch")[0]["npcs"] == ["NPC"]
        rig.bridge.emit(player_row(), fighter())
        await eventually(lambda: rig.link.body_leases)
        assert len(rig.bridge.commands("body_acquire")) == 1
        await eventually(lambda: rig.policy.commands("reset"))
        assert not rig.policy.commands("step")
        own = fighter(3, owned=True)
        rig.bridge.emit(own)
        await eventually(lambda: rig.bridge.commands("body_action"))
        assert [request["cmd"] for request in rig.policy.requests[:2]] == ["reset", "step"]
        assert rig.policy.commands("step")[0]["observation"] == own


async def test_conversation_partner_native_attack_acquires_resets_and_keeps_body_lease():
    async with running(talking=True) as rig:
        rig.roster(activity="routine", state="ZS_TALK")
        rig.combat()
        await asyncio.sleep(0.04)
        assert not rig.bridge.commands("body_watch")

        # The talk desk can still name the speaker after story_effect detached
        # him and the native scripts entered combat. The roster is authoritative.
        rig.roster(activity="fight", state="ZS_ATTACK")
        await eventually(lambda: rig.bridge.commands("body_watch"))
        rig.bridge.emit(player_row(), fighter())
        await eventually(lambda: rig.policy.commands("reset"))
        lease = rig.link.body_leases[1]
        rig.bridge.emit(fighter(3, owned=True))
        await eventually(lambda: rig.bridge.commands("body_action"))
        await eventually(lambda: rig.pilot.people["NPC"].activity == "body")
        assert rig.pilot.people["NPC"].state == "ZS_ATTACK"

        rig.bridge.emit(fighter(4, owned=True))
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 2)
        assert rig.link.body_leases == {1: lease}
        assert len(rig.bridge.commands("body_acquire")) == 1
        assert len(rig.policy.commands("reset")) == 1
        assert not rig.bridge.commands("body_release")
        assert not rig.pilot.summaries


@pytest.mark.parametrize(("activity", "state"), [
    ("routine", "ZS_TALK"), ("routine", "ZS_ATTACK"), ("fight", "ZS_WAIT"),
    ("dialog", "ZS_ATTACK"), ("attached", "ZS_ATTACK"),
])
async def test_conversation_partner_without_native_fight_does_not_acquire(activity, state):
    async with running(talking=True) as rig:
        rig.roster(activity=activity, state=state)
        rig.combat()
        rig.bridge.emit(player_row(), fighter())
        await asyncio.sleep(0.04)
        assert not rig.pilot.handles("NPC")
        assert not rig.bridge.commands("body_watch")
        assert not rig.bridge.commands("body_acquire")
        assert not rig.policy.requests


async def test_unavailable_conversation_partner_still_blocks_native_attack_acquisition():
    async with running(talking=True, unavailable=True) as rig:
        rig.roster(activity="fight", state="ZS_ATTACK")
        rig.combat()
        rig.bridge.emit(player_row(), fighter())
        await asyncio.sleep(0.04)
        assert not rig.pilot.handles("NPC")
        assert not rig.bridge.commands("body_watch")
        assert not rig.bridge.commands("body_acquire")
        assert not rig.policy.requests


async def test_fight_activity_without_native_attack_state_does_not_start_watch():
    async with running() as rig:
        rig.roster(activity="fight", state="ZS_WAIT")
        rig.combat(kind="player_near")
        rig.bridge.emit(player_row(), fighter())
        await asyncio.sleep(0.04)
        assert not rig.bridge.commands("body_watch")
        assert not rig.bridge.commands("body_acquire")
        assert not rig.policy.requests


async def test_native_attack_roster_rejects_live_target_outside_hero_and_companions():
    async with running(companion=lambda npc: npc == "CAVALORN") as rig:
        rig.bridge.target_is_hero = False
        rig.bridge.target_npc = "OTHER"
        rig.roster(activity="fight", state="ZS_ATTACK", extra=[companion_entry()])
        await eventually(lambda: rig.bridge.commands("body_watch"))
        own = fighter()
        own["target"]["entity_id"] = 8
        own["goal"]["target_id"] = 8
        rig.bridge.emit(player_row(), own)
        await eventually(lambda: len(rig.bridge.commands("story_eval")) == 2)
        await eventually(lambda: rig.fight._refusals["target_not_allowed"] == 1)
        assert rig.fight._refusals == {"target_not_allowed": 1}
        assert not rig.bridge.commands("body_acquire")
        assert not rig.policy.requests


async def test_foreign_target_down_before_lease_does_not_block_switch_to_hero_without_routine():
    async with running() as rig:
        rig.bridge.target_is_hero = False
        rig.bridge.target_npc = "OTHER"
        await rig.watch(activity="fight", state="ZS_ATTACK")
        await eventually(lambda: rig.fight._watch_ready)
        foreign = fighter(target_entity=8)
        foreign["target"].update(down=True, hp=0)
        rig.bridge.emit(player_row(), foreign)

        # The queued observation still describes entity 8, but NPC_GETTARGET
        # already reports the healthy hero when the pilot processes that row.
        rig.bridge.target_is_hero = True
        rig.bridge.target_npc = None
        await eventually(lambda: rig.fight._last_seq == foreign["observation_seq"])
        await asyncio.sleep(0.02)
        assert rig.pilot.handles("NPC")
        assert rig.fight._reason is None
        assert "NPC" not in rig.pilot._claimed_fights
        assert not rig.pilot.summaries
        assert not rig.bridge.commands("body_acquire") and not rig.policy.requests

        # Native combat can switch directly from an unrelated fallen actor to
        # the hero, with no intervening routine/down roster to clear a claim.
        rig.clock.advance(3)
        rig.roster(activity="fight", state="ZS_ATTACK")
        rig.combat()
        rig.bridge.emit(fighter(3))
        await eventually(lambda: rig.policy.commands("reset"))
        assert rig.pilot.active_count == 1
        assert rig.fight.lease.target_entity_id == 2
        assert rig.fight._target_kind == "hero"
        assert len(rig.bridge.commands("body_watch")) == 1
        assert len(rig.bridge.commands("body_acquire")) == len(rig.policy.commands("reset")) == 1
        rig.bridge.emit(fighter(4, owned=True))
        await eventually(lambda: rig.bridge.commands("body_action"))
        assert not rig.pilot.summaries


@pytest.mark.parametrize("membership", ["orders", "party"])
async def test_live_companion_target_is_acquired_and_named_in_summary(membership):
    async with running(companion=lambda npc: membership == "orders" and npc == "CAVALORN") as rig:
        if membership == "party":
            rig.link.party["CAVALORN"] = (0, 1)
        rig.bridge.target_is_hero = False
        rig.bridge.target_npc = "CAVALORN"
        await rig.active(target_entity=3, extra=[companion_entry()])
        acquire = rig.bridge.commands("body_acquire")[0]
        assert acquire["target_entity_id"] == 3
        assert rig.fight.lease.target_entity_id == 3
        assert len(rig.policy.commands("reset")) == 1
        assert rig.policy.commands("step")[0]["observation"]["target"]["entity_id"] == 3
        assert rig.bridge.commands("body_action")[0]["target_entity_id"] == 3

        row = fighter(4, owned=True, target_entity=3)
        row["self"]["down"] = True
        rig.bridge.emit(row)
        await rig.ended()
        summary = rig.pilot.last_summary
        assert summary.reason == "npc_down"
        assert summary.target_kind == "companion" and summary.target_name == "Кавалорн"
        assert summary.release_confirmed and summary.watch_removed


async def test_companion_membership_is_rechecked_after_live_target_query():
    companions = {"CAVALORN"}
    async with running(companion=companions.__contains__) as rig:
        rig.bridge.target_is_hero = False
        rig.bridge.target_npc = "CAVALORN"
        await rig.watch(extra=[companion_entry()])
        await eventually(lambda: rig.fight._watch_ready)
        rig.bridge.response_delays["story_eval"] = 0.020
        rig.bridge.emit(player_row(), fighter(target_entity=3))
        await eventually(lambda: len(rig.bridge.commands("story_eval")) == 2)
        companions.clear()
        await eventually(lambda: rig.fight._refusals["target_not_allowed"] == 1)
        assert not rig.bridge.commands("body_acquire")
        assert not rig.policy.requests


async def test_runtime_entity_id_is_bound_from_the_watched_npc_symbol():
    async with running() as rig:
        await rig.active(entity=9)
        assert rig.bridge.commands("body_acquire")[0]["entity_id"] == 9
        assert rig.bridge.commands("body_action")[0]["entity_id"] == 9
        assert rig.policy.commands("step")[0]["observation"]["entity_id"] == 9
        assert rig.bridge.commands("body_watch")[0]["npcs"] == ["NPC"]


async def test_native_body_roster_keeps_the_owned_npc_under_its_current_lease():
    async with running() as rig:
        await rig.active()
        await eventually(lambda: rig.pilot.people["NPC"].activity == "body")
        assert rig.pilot.people["NPC"].state == "ZS_ATTACK"
        rig.combat()
        rig.bridge.emit(fighter(4, owned=True))
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 2)
        assert len(rig.bridge.commands("body_acquire")) == 1
        assert rig.link.body_owned("NPC")
        assert not rig.bridge.commands("body_release")


async def test_acquire_rejection_retries_at_150_ms_and_then_starts_an_episode(caplog):
    caplog.set_level("INFO")
    async with running() as rig:
        rig.bridge.acquire_rejections.extend(["unsafe_boundary", "ai_queue_not_empty"])
        await rig.watch()
        rig.bridge.emit(player_row(), fighter())
        await eventually(lambda: len(rig.bridge.commands("body_acquire")) == 1)
        rig.clock.advance(0.149)
        rig.bridge.emit(fighter(3))
        await asyncio.sleep(0.01)
        assert len(rig.bridge.commands("body_acquire")) == 1
        rig.clock.advance(0.002)
        rig.bridge.emit(fighter(4))
        await eventually(lambda: len(rig.bridge.commands("body_acquire")) == 2)
        rig.clock.advance(0.151)
        rig.bridge.emit(fighter(5))
        await eventually(lambda: rig.link.body_leases)
        assert len(rig.bridge.commands("body_acquire")) == 3
        await eventually(lambda: rig.policy.commands("reset"))
        assert not rig.policy.commands("step")
        rig.bridge.emit(fighter(6, owned=True))
        await eventually(lambda: rig.policy.commands("step"))
        await rig.pilot.aclose()
        assert "unsafe_boundary" in caplog.text and "ai_queue_not_empty" in caplog.text
        assert dict(rig.pilot.last_summary.acquire_refusals) == {
            "unsafe_boundary": 1,
            "ai_queue_not_empty": 1,
        }


async def test_pending_boundary_wait_sends_no_policy_or_actions_until_actual_grant():
    async with running() as rig:
        rig.bridge.acquire_pending_results.append("acquired")
        rig.bridge.acquire_pending_delay = 0.075
        await rig.watch()
        rig.bridge.emit(player_row(), fighter())
        await eventually(lambda: rig.bridge.pending_acquire is not None)
        command = rig.bridge.commands("body_acquire")[0]
        assert command["await_boundary_ms"] == 1500 and command["acquire_request_id"] > 0
        assert not rig.link.body_leases and not rig.policy.requests
        assert not rig.bridge.commands("body_action")
        rig.clock.advance(0.075)
        await eventually(lambda: rig.policy.commands("reset"))
        assert len(rig.bridge.commands("body_acquire")) == 1
        assert rig.fight.lease.acquired_observation_seq == 3
        assert not rig.policy.commands("step")
        rig.bridge.emit(fighter(4, owned=True))
        await eventually(lambda: rig.bridge.commands("body_action"))
        await rig.pilot.aclose()
        summary = rig.pilot.last_summary
        assert summary.acquire_requests == summary.acquire_completed == 1
        assert summary.acquire_wait_p50_ms == pytest.approx(75)
        assert summary.acquire_wait_max_ms == pytest.approx(75)
        assert summary.release_confirmed and summary.watch_removed


async def test_expired_boundary_request_retries_without_ending_or_resetting_episode():
    async with running() as rig:
        rig.bridge.acquire_pending_results.extend(["expired", "acquired"])
        await rig.watch()
        rig.bridge.emit(player_row(), fighter())
        await eventually(lambda: rig.fight._refusals)
        assert dict(rig.fight._refusals) == {"body acquire cancelled: expired": 1}
        assert not rig.policy.requests and rig.fight.npc == "NPC"
        rig.clock.advance(0.151)
        rig.bridge.emit(fighter(4))
        await eventually(lambda: rig.policy.commands("reset"))
        assert len(rig.bridge.commands("body_acquire")) == 2
        assert len(rig.policy.commands("reset")) == 1
        await rig.pilot.aclose()
        summary = rig.pilot.last_summary
        assert summary.acquire_requests == 2 and summary.acquire_completed == 1


async def test_boundary_wait_is_clipped_to_remaining_five_second_acquire_window():
    async with running() as rig:
        rig.bridge.acquire_pending_results.append("expired")
        await rig.watch()
        rig.clock.advance(4.5)
        rig.bridge.emit(player_row(), fighter())
        await eventually(lambda: rig.fight._refusals)
        assert rig.bridge.commands("body_acquire")[0]["await_boundary_ms"] == 500
        rig.clock.advance(0.501)
        rig.bridge.emit(fighter(4))
        await eventually(lambda: rig.pilot.last_summary is not None)
        assert rig.pilot.last_summary.reason == "acquire_exhausted"
        assert rig.pilot.last_summary.acquire_completed == 0
        assert not rig.policy.requests


@pytest.mark.parametrize(("refusals", "retry"), [
    ([bodypilot.SOFT_BODY_OWNERSHIP_REFUSAL], True),
    (["wrong_scope"], False),
    ([bodypilot.SOFT_BODY_OWNERSHIP_REFUSAL, "wrong_scope"], False),
])
async def test_only_soft_ownership_exhaustion_retries_native_combat_after_cooldown(refusals, retry):
    async with running() as rig:
        rig.bridge.request_rejections["body_acquire"] = deque(refusals)
        await rig.watch()
        for index in range(len(refusals)):
            rig.bridge.emit(player_row(1 + index * 2), fighter(2 + index * 2))
            await eventually(lambda index=index: sum(rig.fight._refusals.values()) == index + 1)
            rig.clock.advance(bodypilot.ACQUIRE_INTERVAL + 0.001)

        # The native attack event can arrive while the previous observer still
        # exists, just as speech finishes at the five-second acquisition limit.
        rig.clock.advance(bodypilot.ACQUIRE_WINDOW + 0.1)
        rig.combat(kind="npc_attack")
        await eventually(lambda: not rig.pilot.active_count)
        summary = rig.pilot.last_summary
        assert summary.reason == "acquire_exhausted"
        assert summary.acquire_requests == len(refusals) and summary.acquire_completed == 0
        assert summary.lease is None and summary.watch_removed
        assert not rig.policy.requests and not rig.link.body_leases

        rig.roster(activity="fight", state="ZS_ATTACK")
        await asyncio.sleep(0.04)
        assert len(rig.bridge.commands("body_watch")) == 2
        rig.clock.advance(bodypilot.REACQUIRE_INTERVAL + 0.001)
        rig.roster(activity="fight", state="ZS_ATTACK")
        if not retry:
            rig.combat(kind="player_near")
            await asyncio.sleep(0.04)
            assert len(rig.bridge.commands("body_watch")) == 2
            assert len(rig.bridge.commands("body_acquire")) == len(refusals)
            assert not rig.policy.requests
            return

        # No new attack event or routine transition: the still-active native
        # fight is enough once transient ownership and the backoff have ended.
        await eventually(lambda: len(rig.bridge.commands("body_watch")) == 3)
        rig.bridge.emit(player_row(7), fighter(8))
        await eventually(lambda: rig.policy.commands("reset"))
        rig.bridge.emit(fighter(9, owned=True))
        await eventually(lambda: rig.fight._decisions == 1)
        assert len(rig.bridge.commands("body_acquire")) == len(refusals) + 1
        assert len(rig.policy.commands("reset")) == 1
        assert rig.fight._fallbacks == 0


async def test_shutdown_cancels_boundary_request_before_removing_watch():
    async with running() as rig:
        rig.bridge.acquire_pending_results.append("acquired")
        rig.bridge.acquire_pending_delay = 1.5
        await rig.watch()
        rig.bridge.emit(player_row(), fighter())
        await eventually(lambda: rig.bridge.pending_acquire is not None)
        await rig.pilot.aclose()
        assert rig.bridge.pending_acquire is None and not rig.bridge.provider_owned
        cancel = rig.bridge.commands("body_release")[0]
        assert cancel["control_generation"] == 0
        assert cancel["acquire_request_id"] == rig.bridge.commands("body_acquire")[0]["acquire_request_id"]
        assert rig.bridge.commands("body_watch")[-1]["npcs"] == []
        assert not rig.policy.requests and not rig.link.body_leases


async def test_acquire_window_expires_after_five_seconds_and_waits_for_new_combat_event(caplog):
    caplog.set_level("INFO")
    async with running() as rig:
        rig.bridge.acquire_rejections.extend(["unsafe_boundary"] * 4)
        await rig.watch()
        rig.bridge.emit(player_row(), fighter())
        await eventually(lambda: rig.bridge.commands("body_acquire"))
        rig.clock.advance(4.99)
        rig.bridge.emit(fighter(3))
        await eventually(lambda: len(rig.bridge.commands("body_acquire")) == 2)
        rig.clock.advance(0.02)
        rig.bridge.emit(fighter(4))
        await eventually(lambda: any(not cmd["npcs"] for cmd in rig.bridge.commands("body_watch")))
        rig.clock.advance(1.0)
        rig.bridge.emit(fighter(5))
        await asyncio.sleep(0.01)
        assert len(rig.bridge.commands("body_acquire")) == 2
        assert not rig.policy.requests and not rig.bridge.commands("body_release")
        assert "unsafe_boundary" in caplog.text
        rig.combat()
        await eventually(lambda: len(rig.bridge.commands("body_watch")) == 3)


async def test_repeated_native_attack_roster_does_not_restart_exhausted_acquisition():
    async with running() as rig:
        rig.bridge.acquire_rejections.append("unsafe_boundary")
        rig.roster(activity="fight", state="ZS_ATTACK")
        await eventually(lambda: rig.bridge.commands("body_watch"))
        rig.bridge.emit(player_row(), fighter())
        await eventually(lambda: rig.bridge.commands("body_acquire"))
        rig.clock.advance(5.01)
        rig.roster()
        await eventually(lambda: any(not cmd["npcs"] for cmd in rig.bridge.commands("body_watch")))
        await eventually(lambda: rig.pilot.active_count == 0)
        for seq in range(3, 6):
            rig.roster()
            rig.bridge.emit(fighter(seq))
            await asyncio.sleep(0.01)
        assert len(rig.bridge.commands("body_watch")) == 2
        assert len(rig.bridge.commands("body_acquire")) == 1
        assert not rig.policy.requests


async def test_native_attack_roster_starts_after_a_precombat_watch_expires():
    async with running() as rig:
        rig.roster(activity="fight", state="ZS_WAIT")
        rig.combat(kind="weapon_drawn")
        await eventually(lambda: rig.bridge.commands("body_watch"))
        rig.clock.advance(5.01)
        rig.roster(activity="fight", state="ZS_WAIT")
        await eventually(lambda: any(not cmd["npcs"] for cmd in rig.bridge.commands("body_watch")))
        await eventually(lambda: rig.pilot.active_count == 0)
        assert not rig.bridge.commands("body_acquire")
        rig.roster(activity="fight", state="ZS_ATTACK")
        await eventually(lambda: len(rig.bridge.commands("body_watch")) == 3)
        rig.bridge.emit(player_row(), fighter())
        await eventually(lambda: rig.link.body_leases)
        assert len(rig.bridge.commands("body_acquire")) == 1


async def test_native_attack_during_a_precombat_watch_is_not_retried_after_exhaustion():
    async with running() as rig:
        rig.roster(activity="fight", state="ZS_WAIT")
        rig.combat(kind="weapon_drawn")
        await eventually(lambda: rig.bridge.commands("body_watch"))
        rig.clock.advance(2.0)
        rig.roster(activity="fight", state="ZS_ATTACK")
        rig.clock.advance(3.01)
        rig.roster(activity="fight", state="ZS_ATTACK")
        await eventually(lambda: any(not cmd["npcs"] for cmd in rig.bridge.commands("body_watch")))
        await eventually(lambda: rig.pilot.active_count == 0)
        for seq in range(2, 5):
            rig.roster(activity="fight", state="ZS_ATTACK")
            rig.bridge.emit(fighter(seq))
            await asyncio.sleep(0.01)
        assert len(rig.bridge.commands("body_watch")) == 2
        assert not rig.bridge.commands("body_acquire")
        assert not rig.policy.requests


@pytest.mark.parametrize("end", ["fallback", "error", "fight_timeout"])
async def test_native_remainder_of_same_combat_is_not_reacquired(end):
    async with running() as rig:
        await rig.active()
        if end == "fight_timeout":
            rig.clock.advance(300.01)
        else:
            rig.policy.step_mode = end
        rig.bridge.emit(fighter(4, owned=True))
        await rig.ended()
        await eventually(lambda: rig.pilot.active_count == 0)
        # The native bridge emits another attack for every subsequent hero blow.
        for kind in ("attack", "threatened", "weapon_drawn", "npc_attack"):
            rig.combat(kind=kind)
        rig.bridge.emit(fighter(5))
        await asyncio.sleep(0.04)
        assert len(rig.bridge.commands("body_acquire")) == 1
        assert len(rig.bridge.commands("body_watch")) == 2
        assert not rig.link.body_leases


async def test_repeated_native_attack_roster_does_not_reacquire_after_fallback():
    async with running() as rig:
        await rig.active()
        rig.policy.step_mode = "fallback"
        rig.bridge.emit(fighter(4, owned=True))
        await rig.ended()
        await eventually(lambda: rig.pilot.active_count == 0)
        for seq in range(5, 8):
            rig.roster()
            rig.bridge.emit(fighter(seq))
            await asyncio.sleep(0.01)
        assert len(rig.bridge.commands("body_watch")) == 2
        assert len(rig.bridge.commands("body_acquire")) == 1


@pytest.mark.parametrize("end", ["npc_unavailable", "target_lost"])
@pytest.mark.parametrize("range_exit", ["missing_full", "distant"])
async def test_lost_fight_reacquires_with_fresh_reset_when_distant_npc_returns_attacking(end, range_exit):
    async with running() as rig:
        await rig.active()
        first_lease, first_client = rig.fight.lease, rig.fight.client

        def leave_range():
            if range_exit == "missing_full":
                rig.roster(include=False)
            else:
                rig.bridge.roster(RosterEntry(
                    id="NPC", name="Fighter", guild=1, distance=2500,
                    activity="body" if rig.link.body_owned("NPC") else "fight", state="ZS_ATTACK",
                ))

        if end == "npc_unavailable":
            # The full roster can lose the NPC before release has completed.
            leave_range()
        else:
            rig.link._dispatch(events(rig.bridge.control_event(
                reason="native_restored", release_reason="target_visibility_lost", owner="native",
            )))
        await rig.ended()
        await eventually(lambda: not rig.pilot.active_count)
        assert rig.pilot.last_summary.reason == end
        assert rig.pilot.last_summary.release_confirmed
        if end == "target_lost":
            # A partial roster omitting him is not evidence that he left range.
            rig.link._dispatch(NpcRoster(type="npc_roster", t=2, npcs=[], full=False))
            assert "NPC" in rig.pilot._claimed_fights
            # Here range exit arrives only after the old fight was cleaned up.
            leave_range()
        assert "NPC" not in rig.pilot._claimed_fights
        assert "NPC" not in rig.pilot._roster_attempted

        rig.clock.advance(bodypilot.REACQUIRE_INTERVAL + 0.1)
        # No routine row or new attack event is delivered while the hero returns.
        rig.roster(activity="fight", state="ZS_ATTACK")
        await eventually(lambda: len(rig.bridge.commands("body_watch")) == 3)
        rig.bridge.emit(player_row(4), fighter(5))
        await eventually(lambda: len(rig.policy.commands("reset")) == 2)
        second = rig.fight
        assert second.lease != first_lease and second.client is not first_client
        rig.bridge.emit(fighter(6, owned=True))
        await eventually(lambda: second._decisions == 1)
        assert len(rig.bridge.commands("body_acquire")) == 2
        assert [request["initial_game_ms"] for request in rig.policy.commands("reset")] == [100, 250]
        assert len(rig.policy.connections) == 2
        assert len(rig.pilot.summaries) == 1


@pytest.mark.parametrize("activity,state", [("routine", "ZS_SIT_BENCH"), ("down", "ZS_UNCONSCIOUS")])
async def test_new_native_attack_roster_rearms_after_the_previous_fight_ends(activity, state):
    async with running() as rig:
        await rig.active()
        rig.policy.step_mode = "fallback"
        rig.bridge.emit(fighter(4, owned=True))
        await rig.ended()
        await eventually(lambda: rig.pilot.active_count == 0)
        rig.roster(activity=activity, state=state)
        rig.roster(activity="fight", state="ZS_ATTACK")
        await eventually(lambda: len(rig.bridge.commands("body_watch")) == 3)
        rig.bridge.emit(fighter(5))
        await eventually(lambda: len(rig.bridge.commands("body_acquire")) == 2)
        assert rig.link.body_leases


async def test_fallback_npc_can_be_acquired_in_a_new_combat_after_returning_to_routine():
    async with running() as rig:
        await rig.active()
        rig.policy.step_mode = "fallback"
        rig.bridge.emit(fighter(4, owned=True))
        await rig.ended()
        await eventually(lambda: rig.pilot.active_count == 0)
        seq = 4
        for temporary_roster in (
            {"include": False},
            {"activity": "dialog"},
            {"activity": "attached"},
            {"activity": "routine", "state": "ZS_ATTACK"},
        ):
            # Absence, a takeover, and a transient activity label do not prove
            # that the native fight ended, even if another attack follows.
            rig.roster(**temporary_roster)
            rig.roster()
            rig.combat()
            seq += 1
            rig.bridge.emit(fighter(seq))
            await asyncio.sleep(0.01)
            assert len(rig.bridge.commands("body_acquire")) == 1
            assert len(rig.bridge.commands("body_watch")) == 2
        rig.roster(activity="routine", state="ZS_SIT_BENCH")
        rig.roster()
        rig.combat()
        await eventually(lambda: len(rig.bridge.commands("body_watch")) == 3)
        rig.bridge.emit(fighter(seq + 1))
        await eventually(lambda: len(rig.bridge.commands("body_acquire")) == 2)
        assert rig.link.body_leases


async def test_fallback_for_one_npc_does_not_block_a_different_npc():
    async with running() as rig:
        await rig.active()
        rig.policy.step_mode = "fallback"
        rig.bridge.emit(fighter(4, owned=True))
        await rig.ended()
        await eventually(lambda: rig.pilot.active_count == 0)
        rig.roster(extra=[RosterEntry(
            id="OTHER", name="Other Fighter", guild=1, distance=250.0,
            in_view=True, activity="fight", state="ZS_ATTACK",
        )])
        rig.combat("OTHER")
        await eventually(lambda: len(rig.bridge.commands("body_watch")) == 3)
        rig.bridge.emit(fighter(5, entity=4))
        await eventually(lambda: len(rig.bridge.commands("body_acquire")) == 2)
        assert rig.bridge.commands("body_watch")[-1]["npcs"] == ["OTHER"]
        assert rig.bridge.commands("body_acquire")[-1]["entity_id"] == 4


@pytest.mark.parametrize("denied", ["monster", "unknown", "companion", "twohand", "dialog", "attached"])
async def test_disallowed_npc_is_never_acquired(denied):
    async with running(companion=denied == "companion") as rig:
        rig.roster(
            guild=17 if denied == "monster" else 1,
            activity=denied if denied in {"dialog", "attached"} else "fight",
            include=denied != "unknown",
        )
        rig.combat()
        await asyncio.sleep(0.01)
        row = fighter()
        if denied == "twohand":
            row["self"]["weapon_state"] = 3
        rig.bridge.emit(player_row(), row)
        await asyncio.sleep(0.01)
        assert not rig.bridge.commands("body_acquire")
        assert not rig.policy.requests


@pytest.mark.parametrize("wrong_target", ["hidden", "other_npc"])
async def test_acquisition_needs_a_visible_hero_target(wrong_target):
    async with running() as rig:
        await rig.watch()
        own = fighter()
        if wrong_target == "hidden":
            own["target"]["visible"] = False
            own["target"]["current_valid"] = False
            for key in own["target"]:
                if key not in {"entity_id", "present", "visible", "current_valid"}:
                    own["target"][key] = None
        else:
            rig.bridge.target_is_hero = False
            own["target"]["entity_id"] = 8
            own["goal"]["target_id"] = 8
        rig.bridge.emit(player_row(), own)
        await asyncio.sleep(0.02)
        assert not rig.bridge.commands("body_acquire")


async def test_max_one_fight_does_not_change_watch_or_acquire_another_npc():
    async with running(max_fights=1) as rig:
        await rig.active()
        rig.roster(extra=[RosterEntry(
            id="OTHER", name="Other Fighter", guild=1, distance=250.0,
            in_view=True, activity="fight", state="ZS_ATTACK",
        )])
        rig.combat("OTHER")
        rig.bridge.emit(fighter(4, owned=True, entity=4))
        await asyncio.sleep(0.02)
        assert len(rig.bridge.commands("body_acquire")) == 1
        assert len(rig.bridge.commands("body_watch")) == 1
        assert len(rig.policy.commands("step")) == 1
