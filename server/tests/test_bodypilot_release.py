import asyncio
import json
from collections import deque

import pytest
from websockets.exceptions import ConnectionClosedError

from gothic_ai import bodypilot
from gothic_ai.game import GameError
from gothic_ai.protocol import (
    PROTOCOL_VERSION,
    Attached,
    Hello,
    Loading,
    RosterEntry,
)
from tests.bodypilot_helpers import (
    companion_entry,
    eventually,
    fighter,
    multi_entry,
    multi_fighter,
    npc_entity,
    player_row,
    running,
    start_fights,
)
from tests.test_body_control import events


@pytest.mark.parametrize("takeover", ["native_restored", "lease_lost", "attached", "dialog", "unavailable"])
async def test_native_ownership_or_dialogue_takeover_ends_episode(takeover):
    async with running() as rig:
        await rig.active()
        if takeover == "native_restored":
            rig.link._dispatch(events(rig.bridge.control_event(
                reason="native_restored", owner="native",
            )))
        elif takeover == "lease_lost":
            rig.bridge.emit(fighter(4, owned=True, control_generation=8))
        elif takeover == "attached":
            rig.link._dispatch(Attached(type="attached", npc="NPC", name="Fighter"))
        elif takeover == "dialog":
            rig.roster(activity="dialog")
        else:
            rig.pilot.unavailable = lambda _: True
            rig.bridge.emit(fighter(4, owned=True))
        await rig.ended()


async def test_pending_release_keeps_watch_until_native_restoration():
    async with running() as rig:
        await rig.active()
        rig.bridge.release_pending_polls = 2
        rig.link._dispatch(Attached(type="attached", npc="NPC", name="Fighter"))
        await rig.ended()
        commands = rig.bridge.sent
        released_at = next(i for i, command in enumerate(commands) if command["type"] == "body_release")
        unwatched_at = next(i for i, command in enumerate(commands)
                            if command["type"] == "body_watch" and not command["npcs"])
        assert sum(command["type"] == "body_events" for command in commands[released_at:unwatched_at]) == 3


@pytest.mark.parametrize("lost_during_release", [False, True])
async def test_lost_native_restoration_event_reconciles_lease_and_allows_the_next_fight(lost_during_release):
    async with running() as rig:
        await rig.active()
        lease = rig.link.body_leases[1]
        rig.bridge.response_delays["body_events"] = 0.350
        if lost_during_release:
            rig.bridge.release_pending_polls = 1
            rig.link._dispatch(Attached(type="attached", npc="NPC", name="Fighter"))
            await eventually(lambda: not rig.bridge.provider_owned)
        else:
            rig.bridge.restore_native()
            # Polling destructively drains the event before its reply times out.
            await eventually(lambda: not rig.bridge.control_events)
        rig.bridge.emit(fighter(4))
        assert rig.link.body_leases == {1: lease}

        await rig.ended()
        await eventually(lambda: rig.pilot.active_count == 0)
        assert rig.bridge.commands("body_control_status")
        assert rig.bridge._watched_npc is None
        assert len(rig.policy.commands("end_episode")) == 1
        assert len(rig.policy.commands("close")) == 1
        assert len(rig.policy.commands("step")) == 1

        # A later combat can still acquire and run policy decisions.
        rig.bridge.response_delays.clear()
        rig.roster(activity="routine", state="ZS_SIT_BENCH")
        await rig.watch()
        await eventually(lambda: rig.fight._watch_ready)
        rig.bridge.emit(player_row(5), fighter(6))
        await eventually(lambda: len(rig.bridge.commands("body_acquire")) == 2)
        await eventually(lambda: rig.link.body_leases)
        rig.bridge.emit(fighter(7, owned=True))
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 2)
        assert len(rig.policy.commands("reset")) == 2


@pytest.mark.parametrize("status_response", ["missing_actor", "unknown_entity"])
async def test_removed_actor_during_release_clears_watch_and_allows_a_different_fight(
    status_response, monkeypatch,
):
    monkeypatch.setattr(bodypilot, "FINISH_TIMEOUT", 0.8)
    async with running() as rig:
        await rig.active()
        lease = rig.link.body_leases[1]
        # Removal destroys the native actor without a restoration event. The
        # adapter still has the lease, while neither release nor status can find it.
        rig.bridge.provider_owned = False
        rig.bridge._control_scope = None
        rig.bridge._lease_watchdog.cancel()
        rig.bridge.release_rejections.append("unknown_entity")
        if status_response == "unknown_entity":
            rig.bridge.status_error = "unknown_entity"
        assert rig.link.body_leases == {1: lease}
        rig.link._dispatch(Attached(type="attached", npc="NPC", name="Fighter"))
        await eventually(lambda: rig.pilot.active_count == 0, timeout=1.2)
        assert rig.bridge.commands("body_release")
        assert rig.bridge.commands("body_control_status")
        assert not rig.link.body_leases
        assert rig.bridge._watched_npc is None
        assert rig.bridge.commands("body_watch")[-1]["npcs"] == []
        assert len(rig.policy.commands("end_episode")) == 1
        assert len(rig.policy.commands("close")) == 1
        assert not rig.pilot._done.is_set()

        rig.bridge.status_error = None
        rig.bridge.roster(RosterEntry(
            id="OTHER", name="Other Fighter", guild=1, distance=250.0,
            in_view=True, activity="fight", state="ZS_ATTACK",
        ))
        rig.combat("OTHER")
        await eventually(lambda: rig.fight._watch_ready)
        assert rig.bridge.commands("body_watch")[-1]["npcs"] == ["OTHER"]
        rig.bridge.emit(player_row(5), fighter(6, entity=4))
        await eventually(lambda: len(rig.bridge.commands("body_acquire")) == 2)
        await eventually(lambda: rig.link.body_leases)
        rig.bridge.emit(fighter(7, owned=True, entity=4))
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 2)
        assert rig.link.body_leases[4].npc == "OTHER"
        assert len(rig.policy.commands("reset")) == 2


async def test_unconfirmed_release_hits_finish_deadline_and_closes_episode(monkeypatch, caplog):
    monkeypatch.setattr(bodypilot, "FINISH_TIMEOUT", 0.8)
    caplog.set_level("INFO", logger="gothic_ai.bodypilot")
    async with running() as rig:
        await rig.active()
        lease = rig.link.body_leases[1]
        # Ownership has returned to native, but all evidence is unavailable:
        # release says not_owned, the event queue is empty, and status never replies.
        rig.bridge.provider_owned = False
        rig.bridge._lease_watchdog.cancel()
        rig.bridge.response_delays["body_control_status"] = 2.0
        rig.link._dispatch(Attached(type="attached", npc="NPC", name="Fighter"))
        await eventually(lambda: rig.bridge.commands("body_control_status"))
        assert rig.link.body_leases == {1: lease}
        await eventually(lambda: rig.pilot.active_count == 0, timeout=1.2)
        assert rig.link.body_leases == {1: lease}
        assert rig.bridge._watched_npc == "NPC"
        assert rig.bridge.commands("body_watch")[-1]["npcs"] == ["NPC"]
        assert len(rig.policy.commands("end_episode")) == 1
        assert len(rig.policy.commands("close")) == 1
        assert not rig.pilot._done.is_set()
        assert "reason=finish_deadline" in caplog.text
        assert "unconfirmed=" in caplog.text
        summary = rig.pilot.last_summary
        assert summary.lease == lease and summary.reason == "finish_deadline"
        assert summary.end_trigger == "attached"
        assert not summary.release_confirmed and summary.release_seconds is None
        assert not summary.watch_removed
        rig.bridge.response_delays.clear()
        await eventually(lambda: not rig.pilot._releasing)
        await eventually(lambda: rig.bridge._watched_npc is None)
        assert rig.pilot.last_summary.release_late_seconds >= 0.8


@pytest.mark.parametrize("confirmation", ["event", "status", "timeout"])
async def test_finished_owner_retains_watch_across_new_fight_until_native(monkeypatch, caplog, confirmation):
    monkeypatch.setattr(bodypilot, "FINISH_TIMEOUT", 0.1)
    caplog.set_level("INFO", logger="gothic_ai.bodypilot")
    async with running(multi=True) as rig:
        await start_fights(rig, "NPC_1")
        entity = npc_entity("NPC_1")
        lease = rig.link.body_leases[entity]
        rig.bridge.pending_releases.add(entity)
        rig.pilot.fights["NPC_1"].stop("target_lost")
        await eventually(lambda: rig.pilot.active_count == 0)
        summary = rig.pilot.last_summary
        assert summary.reason == "finish_deadline" and summary.end_trigger == "target_lost"
        assert not summary.release_confirmed and summary.release_late_seconds is None
        assert not summary.watch_removed and rig.pilot.handles("NPC_1")
        assert rig.bridge.watched == ["NPC_1"]
        assert rig.link.body_leases[entity] == lease

        rig.bridge.roster(multi_entry("NPC_1", activity="body"), multi_entry("NPC_2"))
        await eventually(lambda: rig.bridge.watched == ["NPC_1", "NPC_2"])
        await eventually(lambda: rig.pilot.fights["NPC_2"]._watch_ready)
        rig.bridge.emit(multi_fighter("NPC_2"))
        await eventually(lambda: npc_entity("NPC_2") in rig.link.body_leases)
        assert rig.link.body_leases[entity] == lease

        # The engine eventually returns the body; the timeout variant loses all
        # confirmation, so only the watchdog's bounded grace can clear our record.
        rig.bridge.actors[entity]["owned"] = False
        rig.bridge.pending_releases.discard(entity)
        if confirmation == "event":
            rig.link._dispatch(events(rig.bridge.control_event(
                entity_id=entity, reason="native_restored", owner="native",
            )))
        elif confirmation == "timeout":
            rig.clock.advance(bodypilot.LEASE_TIMEOUT_MS / 1000 + bodypilot.RELEASE_ORPHAN_GRACE + 0.01)
        await eventually(lambda: rig.bridge.watched == ["NPC_2"])
        assert not rig.pilot.handles("NPC_1") and entity not in rig.link.body_leases
        assert rig.pilot.handles("NPC_2") and npc_entity("NPC_2") in rig.link.body_leases
        updated = rig.pilot.last_summary
        if confirmation == "timeout":
            assert rig.pilot.release_orphan_timeout == 1
            assert not updated.release_confirmed and updated.release_late_seconds is None
            assert "release_orphan_timeout npc=NPC_1" in caplog.text
        else:
            assert rig.pilot.release_orphan_timeout == 0
            assert updated.release_confirmed and updated.release_late_seconds >= 0.1
            assert updated.reason == "finish_deadline" and updated.end_trigger == "target_lost"
            assert "end_trigger=target_lost release_late_seconds=" in caplog.text
        assert not summary.release_confirmed  # Previously published snapshots stay immutable.


async def test_unconfirmed_release_reserves_one_of_four_watch_slots(monkeypatch):
    monkeypatch.setattr(bodypilot, "FINISH_TIMEOUT", 0.1)
    async with running(multi=True) as rig:
        npcs = ("NPC_1", "NPC_2", "NPC_3", "NPC_4")
        await start_fights(rig, *npcs)
        entity = npc_entity("NPC_1")
        rig.bridge.pending_releases.add(entity)
        rig.pilot.fights["NPC_1"].stop("target_lost")
        await eventually(lambda: rig.pilot.active_count == 3)
        assert "NPC_1" in rig.pilot._releasing

        watch_count = len(rig.bridge.commands("body_watch"))
        rig.bridge.roster(
            *(multi_entry(npc, activity="body") for npc in npcs), multi_entry("NPC_5"),
        )
        rig.combat("NPC_5")
        assert not rig.pilot.handles("NPC_5")
        assert rig.pilot.active_count == 3
        assert set(rig.bridge.watched) == set(npcs)
        assert len(rig.bridge.commands("body_watch")) == watch_count

        rig.bridge.actors[entity]["owned"] = False
        rig.bridge.pending_releases.discard(entity)
        rig.link._dispatch(events(rig.bridge.control_event(
            entity_id=entity, reason="native_restored", owner="native",
        )))
        await eventually(lambda: set(rig.bridge.watched) == set(npcs[1:]))
        rig.bridge.roster(
            *(multi_entry(npc, activity="body") for npc in npcs[1:]), multi_entry("NPC_5"),
        )
        await eventually(lambda: rig.pilot.handles("NPC_5"))
        await eventually(lambda: rig.pilot.fights["NPC_5"]._watch_ready)
        rig.bridge.emit(multi_fighter("NPC_5"))
        await eventually(lambda: npc_entity("NPC_5") in rig.link.body_leases)
        assert set(rig.bridge.watched) == {*npcs[1:], "NPC_5"}
        assert all(len(command["npcs"]) <= 4 for command in rig.bridge.commands("body_watch"))


@pytest.mark.parametrize("end_trigger", ["target_lost", "npc_unavailable", "hero_down", "target_down"])
async def test_native_restored_during_end_episode_confirms_late_release(monkeypatch, end_trigger):
    monkeypatch.setattr(bodypilot, "FINISH_TIMEOUT", 0.1)
    async with running() as rig:
        await rig.active()
        lease = rig.link.body_leases[1]
        rig.bridge.release_pending_polls = 10000
        original_call = rig.fight.client.call
        restored_during_cleanup = []

        async def restore_during_end_episode(command, **fields):
            if command == "end_episode":
                assert rig.link.body_leases == {1: lease}
                rig.bridge.provider_owned = False
                rig.bridge._released_pending = False
                rig.link._dispatch(events(rig.bridge.control_event(
                    entity_id=lease.entity_id, reason="native_restored", owner="native",
                )))
                if end_trigger in {"hero_down", "target_down"}:
                    # A native routine during cleanup can clear the claim before
                    # the final summary reinstates the fallen-target guard.
                    rig.roster(activity="routine", state="ZS_SIT_BENCH")
                    assert "NPC" not in rig.pilot._claimed_fights
                restored_during_cleanup.append(True)
            return await original_call(command, **fields)

        rig.fight.client.call = restore_during_end_episode
        rig.fight.stop(end_trigger)
        await eventually(lambda: rig.pilot.active_count == 0)
        summary = rig.pilot.last_summary
        assert restored_during_cleanup == [True]
        assert summary.reason == "finish_deadline" and summary.end_trigger == end_trigger
        assert summary.release_confirmed and summary.watch_removed
        assert summary.release_late_seconds >= bodypilot.FINISH_TIMEOUT
        assert summary.release_seconds == summary.release_late_seconds
        assert rig.bridge._watched_npc is None
        assert not rig.pilot._releasing and rig.pilot.release_orphan_timeout == 0
        if end_trigger in {"hero_down", "target_down"}:
            assert "NPC" in rig.pilot._claimed_fights and "NPC" in rig.pilot._roster_attempted
            assert "NPC" not in rig.pilot._lost_fights
            rig.roster(activity="fight", state="ZS_ATTACK")
            assert not rig.pilot.handles("NPC")
        else:
            assert "NPC" in rig.pilot._lost_fights


@pytest.mark.parametrize("end_trigger", ["target_lost", "npc_unavailable"])
@pytest.mark.parametrize("range_exit", ["before_native", "after_native"])
async def test_late_release_allows_lost_fighter_to_return_without_routine(
    monkeypatch, end_trigger, range_exit,
):
    monkeypatch.setattr(bodypilot, "FINISH_TIMEOUT", 0.1)
    async with running(multi=True) as rig:
        await start_fights(rig, "NPC_1")
        entity = npc_entity("NPC_1")
        rig.bridge.pending_releases.add(entity)
        rig.pilot.fights["NPC_1"].stop(end_trigger)
        await eventually(lambda: rig.pilot.active_count == 0)
        assert rig.pilot.last_summary.reason == "finish_deadline"
        assert "NPC_1" in rig.pilot._claimed_fights
        distant = multi_entry("NPC_1", activity="body").model_copy(update={"distance": 2100.0})
        if range_exit == "before_native":
            rig.bridge.roster(distant)

        rig.bridge.actors[entity]["owned"] = False
        rig.bridge.pending_releases.discard(entity)
        rig.link._dispatch(events(rig.bridge.control_event(
            entity_id=entity, reason="native_restored", owner="native",
        )))
        if range_exit == "after_native":
            assert "NPC_1" in rig.pilot._claimed_fights and "NPC_1" in rig.pilot._lost_fights
            rig.bridge.roster(distant)
        assert "NPC_1" not in rig.pilot._claimed_fights
        assert "NPC_1" not in rig.pilot._roster_attempted
        await eventually(lambda: not rig.bridge.watched)

        # Return directly to combat: no routine/down roster may be needed to
        # forget a fight whose confirmed native actor left the visible range.
        rig.bridge.roster(multi_entry("NPC_1"))
        await eventually(lambda: rig.pilot.handles("NPC_1"))
        await eventually(lambda: rig.pilot.fights["NPC_1"]._watch_ready)
        rig.bridge.emit(multi_fighter("NPC_1"))
        await eventually(lambda: entity in rig.link.body_leases)
        await eventually(lambda: len(rig.policy.commands("reset")) == 2)
        assert len(rig.bridge.commands("body_acquire")) == 2


@pytest.mark.parametrize("confirmation", ["event", "status"])
async def test_late_release_after_target_change_reacquires_against_live_companion(
    monkeypatch, confirmation,
):
    monkeypatch.setattr(bodypilot, "FINISH_TIMEOUT", 0.1)
    async with running(multi=True, companion=lambda npc: npc == "CAVALORN") as rig:
        await start_fights(rig, "NPC_1")
        first = rig.pilot.fights["NPC_1"]
        first_client = first.client
        entity = npc_entity("NPC_1")
        rig.bridge.pending_releases.add(entity)
        rig.bridge.roster(multi_entry("NPC_1", activity="body"), companion_entry())
        rig.bridge.target_npcs["NPC_1"] = "CAVALORN"
        rig.bridge.emit(multi_fighter("NPC_1", 4, owned=True, target_entity=3))
        await eventually(lambda: rig.pilot.active_count == 0)
        summary = rig.pilot.last_summary
        assert summary.reason == "finish_deadline" and summary.end_trigger == "target_changed"
        assert not summary.release_confirmed and "NPC_1" in rig.pilot._claimed_fights
        assert rig.bridge.watched == ["NPC_1"]

        rig.bridge.actors[entity]["owned"] = False
        rig.bridge.pending_releases.discard(entity)
        if confirmation == "event":
            rig.link._dispatch(events(rig.bridge.control_event(
                entity_id=entity, reason="native_restored", owner="native",
            )))
        await eventually(lambda: rig.pilot.last_summary.release_confirmed)
        await eventually(lambda: "NPC_1" in rig.pilot._retarget_pending)
        assert "NPC_1" in rig.pilot._claimed_fights
        assert rig.pilot.last_summary.end_trigger == "target_changed"
        await eventually(lambda: not rig.bridge.watched)

        rig.clock.advance(bodypilot.REACQUIRE_INTERVAL - 0.001)
        rig.bridge.roster(multi_entry("NPC_1"), companion_entry())
        rig.combat("NPC_1")
        assert not rig.pilot.handles("NPC_1")
        assert len(rig.bridge.commands("body_acquire")) == 1
        rig.clock.advance(0.002)
        rig.bridge.roster(multi_entry("NPC_1"), companion_entry())
        rig.combat("NPC_1")
        await eventually(lambda: rig.pilot.handles("NPC_1"))
        assert "NPC_1" not in rig.pilot._claimed_fights
        await eventually(lambda: rig.pilot.fights["NPC_1"]._watch_ready)
        rig.bridge.emit(multi_fighter("NPC_1", 5, target_entity=3))
        await eventually(lambda: len(rig.policy.commands("reset")) == 2)
        second = rig.pilot.fights["NPC_1"]
        assert second is not first and second.client is not first_client
        assert len(rig.bridge.commands("body_acquire")) == 2
        assert second.lease.target_entity_id == 3
        assert rig.bridge.commands("body_acquire")[-1]["target_entity_id"] == 3
        rig.bridge.emit(multi_fighter("NPC_1", 6, owned=True, target_entity=3))
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 2)
        assert second._decisions == 1


async def test_late_target_change_does_not_reacquire_against_unrelated_native_target(monkeypatch):
    monkeypatch.setattr(bodypilot, "FINISH_TIMEOUT", 0.1)
    async with running(multi=True, companion=lambda npc: npc == "CAVALORN") as rig:
        await start_fights(rig, "NPC_1")
        entity = npc_entity("NPC_1")
        rig.bridge.pending_releases.add(entity)
        rig.bridge.roster(multi_entry("NPC_1", activity="body"), companion_entry())
        rig.bridge.target_npcs["NPC_1"] = "OTHER"
        rig.bridge.emit(multi_fighter("NPC_1", 4, owned=True, target_entity=8))
        await eventually(lambda: rig.pilot.active_count == 0)
        assert rig.pilot.last_summary.end_trigger == "target_changed"
        rig.bridge.actors[entity]["owned"] = False
        rig.bridge.pending_releases.discard(entity)
        rig.link._dispatch(events(rig.bridge.control_event(
            entity_id=entity, reason="native_restored", owner="native",
        )))
        await eventually(lambda: not rig.bridge.watched)

        rig.clock.advance(bodypilot.REACQUIRE_INTERVAL + 0.01)
        queries = len(rig.bridge.commands("story_eval"))
        rig.bridge.roster(multi_entry("NPC_1"), companion_entry())
        rig.combat("NPC_1")
        await eventually(lambda: len(rig.bridge.commands("story_eval")) > queries)
        await asyncio.sleep(0.03)
        assert "NPC_1" in rig.pilot._claimed_fights
        assert not rig.pilot.handles("NPC_1") and not rig.link.body_leases
        assert len(rig.bridge.commands("body_acquire")) == len(rig.policy.commands("reset")) == 1


@pytest.mark.parametrize("end_trigger,range_exit,exit_when", [
    ("target_lost", "distant", "before_timeout"),
    ("target_lost", "missing_full", "after_timeout"),
    ("npc_unavailable", "missing_full", "before_timeout"),
    ("npc_unavailable", "distant", "after_timeout"),
])
async def test_orphan_timeout_allows_lost_fighter_to_return_without_routine(
    monkeypatch, end_trigger, range_exit, exit_when,
):
    monkeypatch.setattr(bodypilot, "FINISH_TIMEOUT", 0.1)
    async with running(multi=True) as rig:
        await start_fights(rig, "NPC_1")
        first = rig.pilot.fights["NPC_1"]
        first_client = first.client
        entity = npc_entity("NPC_1")
        rig.bridge.pending_releases.add(entity)
        first.stop(end_trigger)
        await eventually(lambda: rig.pilot.active_count == 0)
        assert rig.pilot.last_summary.reason == "finish_deadline"
        assert rig.pilot.last_summary.end_trigger == end_trigger
        assert "NPC_1" in rig.pilot._claimed_fights

        def leave_range():
            entries = [] if range_exit == "missing_full" else [
                multi_entry("NPC_1", activity="body").model_copy(update={"distance": 2100.0}),
            ]
            rig.bridge.roster(*entries)

        if exit_when == "before_timeout":
            leave_range()
        # The engine's lease watchdog fired, but no native event or status was
        # delivered before the local orphan deadline. Advancing before yielding
        # ensures this exercises expiry rather than an ordinary native poll.
        rig.bridge.actors[entity]["owned"] = False
        rig.bridge.pending_releases.discard(entity)
        rig.clock.advance(bodypilot.LEASE_TIMEOUT_MS / 1000 + bodypilot.RELEASE_ORPHAN_GRACE + 0.01)
        await eventually(lambda: rig.pilot.release_orphan_timeout == 1)
        await eventually(lambda: not rig.bridge.watched)
        assert not rig.pilot.last_summary.release_confirmed
        assert entity not in rig.link.body_leases and "NPC_1" not in rig.pilot._releasing
        if exit_when == "after_timeout":
            assert "NPC_1" in rig.pilot._claimed_fights and "NPC_1" in rig.pilot._lost_fights
            leave_range()
        assert "NPC_1" not in rig.pilot._claimed_fights
        assert "NPC_1" not in rig.pilot._roster_attempted

        rig.bridge.roster(multi_entry("NPC_1"))
        rig.combat("NPC_1")
        await eventually(lambda: rig.pilot.handles("NPC_1"))
        await eventually(lambda: rig.pilot.fights["NPC_1"]._watch_ready)
        rig.bridge.emit(multi_fighter("NPC_1", 5))
        await eventually(lambda: len(rig.policy.commands("reset")) == 2)
        second = rig.pilot.fights["NPC_1"]
        assert second is not first and second.client is not first_client
        assert len(rig.bridge.commands("body_acquire")) == 2
        rig.bridge.emit(multi_fighter("NPC_1", 6, owned=True))
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 2)
        assert second._decisions == 1


@pytest.mark.parametrize("end_trigger", ["policy_error", "hero_down", "target_down"])
@pytest.mark.parametrize("confirmation", ["event", "timeout"])
async def test_delayed_release_keeps_closed_fight_claimed_until_native_routine(
    monkeypatch, end_trigger, confirmation,
):
    monkeypatch.setattr(bodypilot, "FINISH_TIMEOUT", 0.1)
    async with running(multi=True) as rig:
        await start_fights(rig, "NPC_1")
        entity = npc_entity("NPC_1")
        rig.bridge.pending_releases.add(entity)
        rig.pilot.fights["NPC_1"].stop(end_trigger)
        await eventually(lambda: rig.pilot.active_count == 0)
        assert rig.pilot.last_summary.reason == "finish_deadline"
        assert rig.pilot.last_summary.end_trigger == end_trigger

        rig.bridge.actors[entity]["owned"] = False
        rig.bridge.pending_releases.discard(entity)
        if confirmation == "event":
            rig.link._dispatch(events(rig.bridge.control_event(
                entity_id=entity, reason="native_restored", owner="native",
            )))
        else:
            rig.clock.advance(bodypilot.LEASE_TIMEOUT_MS / 1000 + bodypilot.RELEASE_ORPHAN_GRACE + 0.01)
        await eventually(lambda: not rig.bridge.watched)
        assert "NPC_1" in rig.pilot._claimed_fights
        assert "NPC_1" not in rig.pilot._lost_fights and "NPC_1" not in rig.pilot._retarget_pending
        rig.clock.advance(bodypilot.REACQUIRE_INTERVAL + 0.01)
        # Distance or full-roster absence cannot reopen post-down cleanup or a
        # policy failure; only the later native routine ends these episodes.
        rig.bridge.roster()
        rig.bridge.roster(multi_entry("NPC_1").model_copy(update={"distance": 2100.0}))
        rig.bridge.roster(multi_entry("NPC_1"))
        rig.combat("NPC_1")
        await asyncio.sleep(0.03)
        assert not rig.pilot.handles("NPC_1")
        assert len(rig.bridge.commands("body_acquire")) == len(rig.policy.commands("reset")) == 1

        rig.bridge.roster(multi_entry("NPC_1", activity="routine").model_copy(
            update={"state": "ZS_SIT_BENCH"},
        ))
        rig.bridge.roster(multi_entry("NPC_1"))
        await eventually(lambda: rig.pilot.handles("NPC_1"))
        await eventually(lambda: rig.pilot.fights["NPC_1"]._watch_ready)
        rig.bridge.emit(multi_fighter("NPC_1", 5))
        await eventually(lambda: len(rig.policy.commands("reset")) == 2)
        assert len(rig.bridge.commands("body_acquire")) == 2


@pytest.mark.parametrize("cancel_reply_delay", [0, 0.350])
async def test_uncertain_acquire_recovery_is_bounded_by_finish_deadline(
    monkeypatch, caplog, cancel_reply_delay,
):
    monkeypatch.setattr(bodypilot, "FINISH_TIMEOUT", 0.8)
    caplog.set_level("INFO", logger="gothic_ai.bodypilot")
    async with running() as rig:
        rig.bridge.response_delays.update(body_acquire=2.0, body_control_status=2.0)
        await rig.watch()
        await eventually(lambda: rig.fight._watch_ready)
        rig.bridge.emit(player_row(), fighter())
        await eventually(lambda: rig.bridge.commands("body_control_status"))
        # The native watchdog has restored the actor, but the adapter never
        # received its acquire receipt or an ownership snapshot.
        rig.bridge.provider_owned = False
        rig.bridge._lease_watchdog.cancel()
        rig.clock.advance(bodypilot.ACQUIRE_WINDOW + 0.001)
        await eventually(lambda: rig.pilot.active_count == 0, timeout=1.2)
        assert rig.fight._pending_acquire is None
        assert not rig.link.body_leases
        # The missing cancellation/status evidence still requires this watch;
        # finish must be bounded without abandoning the unresolved request ID.
        assert rig.link.body_acquire_pending(1)
        assert rig.bridge._watched_npc == "NPC"
        assert rig.bridge.commands("body_watch")[-1]["npcs"] == ["NPC"]
        assert rig.pilot.last_summary.end_trigger == "acquire_uncertain"
        assert not rig.pilot.last_summary.watch_removed
        assert not rig.policy.requests
        assert not rig.pilot._done.is_set()
        assert "reason=finish_deadline" in caplog.text
        assert "unconfirmed=" in caplog.text
        polls_before_recovery = len(rig.bridge.commands("body_control_status"))
        rig.bridge.response_delays.clear()
        rig.bridge.response_delays["body_release"] = cancel_reply_delay
        await eventually(lambda: rig.bridge._watched_npc is None)
        assert not rig.link.body_acquire_pending(1)
        assert len(rig.bridge.commands("body_control_status")) > polls_before_recovery
        assert not rig.link.body_leases and not rig.bridge.provider_owned
        assert not rig.pilot._releasing
        assert rig.bridge.commands("body_watch")[-1]["npcs"] == []


async def test_uncertain_acquire_observation_stream_waits_until_the_end_of_the_window():
    async with running() as rig:
        rig.bridge.response_delays["body_acquire"] = 2.0
        rig.bridge.status_error = "status_busy"
        await rig.watch()
        await eventually(lambda: rig.fight._watch_ready)
        rig.bridge.emit(player_row(), fighter())
        await eventually(lambda: rig.bridge.commands("body_control_status"))
        for seq in range(3, 163):
            rig.bridge.emit(fighter(seq, owned=True))
            await asyncio.sleep(0)
        assert rig.fight._reason is None and rig.pilot.last_summary is None
        assert len(rig.fight._rows) <= 1
        assert len(rig.bridge.commands("body_acquire")) == len(rig.bridge.acquire_grants) == 1
        assert not rig.policy.requests and not rig.bridge.commands("body_release")
        rig.bridge.status_error = None
        rig.clock.advance(bodypilot.ACQUIRE_WINDOW + 0.001)
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        assert summary.reason == summary.end_trigger == "acquire_uncertain"
        assert summary.acquire_uncertain_recovered == 0 and summary.watch_removed
        assert not rig.link.body_leases and not rig.link.body_acquire_pending(1)
        assert not rig.bridge.provider_owned


@pytest.mark.parametrize("confirmation", ["event", "status"])
async def test_orphaned_boundary_request_unsubscribes_and_allows_the_next_fight(monkeypatch, confirmation):
    monkeypatch.setattr(bodypilot, "BRIDGE_TIMEOUT", 0.050)
    monkeypatch.setattr(bodypilot, "FINISH_TIMEOUT", 0.150)
    async with running() as rig:
        # The native request expires, but its ACK and cancellation evidence are
        # unavailable until after the pilot's bounded cleanup has finished.
        rig.bridge.acquire_pending_results.append("expired")
        rig.bridge.acquire_pending_delay = 0
        rig.bridge.response_delays["body_acquire"] = 2.0
        rig.bridge.release_rejections.extend(["cancel_busy"] * 100)
        rig.bridge.status_error = "status_busy"
        rig.bridge.request_rejections["body_events"] = deque(["events_busy"] * 100)
        await rig.watch()
        await eventually(lambda: rig.fight._watch_ready)
        rig.bridge.emit(player_row(), fighter())
        await eventually(lambda: rig.bridge.commands("body_control_status"))
        rig.clock.advance(bodypilot.ACQUIRE_WINDOW + 0.001)
        await eventually(lambda: rig.pilot.last_summary is not None)
        assert rig.pilot.last_summary.reason == "finish_deadline"
        summary = rig.pilot.last_summary
        assert not summary.watch_removed
        assert rig.bridge.commands("body_watch")[-1]["npcs"] == ["NPC"]
        assert rig.bridge._watched_npc == "NPC"
        assert rig.link.body_acquire_pending(1)
        assert rig.pilot.handles("NPC")
        assert not rig.bridge.pending_acquire and not rig.bridge.provider_owned
        assert not rig.policy.requests

        rig.bridge.response_delays.clear()
        rig.bridge.release_rejections.clear()
        rig.bridge.request_rejections.pop("body_events")
        if confirmation == "status":
            rig.bridge.control_events.clear()
            rig.bridge.status_error = None
        polls_before_recovery = len(rig.bridge.commands(
            "body_events" if confirmation == "event" else "body_control_status",
        ))
        await eventually(lambda: rig.bridge.commands("body_watch")[-1]["npcs"] == [])
        assert len(rig.bridge.commands(
            "body_events" if confirmation == "event" else "body_control_status",
        )) > polls_before_recovery
        assert not rig.link.body_acquire_pending(1)
        assert rig.bridge._watched_npc is None
        assert not rig.pilot.handles("NPC")
        assert rig.pilot.last_summary is summary and not summary.watch_removed

        rig.bridge.status_error = None
        rig.roster(activity="routine", state="ZS_SIT_BENCH")
        await rig.watch()
        await eventually(lambda: rig.fight._watch_ready)
        rig.bridge.emit(player_row(5), fighter(6))
        await eventually(lambda: rig.link.body_leases)
        rig.bridge.emit(fighter(7, owned=True))
        await eventually(lambda: rig.bridge.commands("body_action"))
        assert len(rig.bridge.commands("body_acquire")) == 2
        assert len(rig.policy.commands("reset")) == 1


@pytest.mark.parametrize("failed_command", [
    "body_release", "body_events", "body_control_status", "body_watch",
])
async def test_websocket_closing_during_cleanup_still_closes_policy_and_pilot(failed_command, monkeypatch):
    async with running() as rig:
        await rig.active()
        client = rig.fight.client
        assert client is not None
        if failed_command in {"body_events", "body_control_status"}:
            rig.bridge.release_pending_polls = 2
        original_send = rig.bridge.send
        failed_sends = []

        async def disconnect_during_send(raw):
            command = json.loads(raw)
            if command["type"] == failed_command:
                failed_sends.append(command)
                # WebSocket send can fail before GameLink.run observes the close.
                asyncio.get_running_loop().call_soon(rig.link.closed.set)
                raise ConnectionClosedError(None, None)
            await original_send(raw)

        monkeypatch.setattr(rig.bridge, "send", disconnect_during_send)
        rig.link._dispatch(Attached(type="attached", npc="NPC", name="Fighter"))
        await eventually(lambda: failed_sends)
        await eventually(lambda: rig.pilot._done.is_set())
        await asyncio.wait_for(rig.pilot.aclose(), 0.1)
        assert rig.link.closed.is_set()
        assert len(rig.policy.commands("end_episode")) == 1
        assert len(rig.policy.commands("close")) == 1
        assert client.writer is not None and client.writer.is_closing()
        assert client.reader_task is not None and client.reader_task.done()
        assert rig.pilot.on_message not in rig.link.listeners
        assert rig.pilot.on_body_packet not in rig.link.raw_body_listeners


@pytest.mark.parametrize("transition", ["loading", "hello", "disconnect"])
async def test_world_or_connection_reset_releases_and_closes_episode(transition, monkeypatch):
    async with running() as rig:
        await rig.active()
        if transition == "loading":
            rig.link._dispatch(Loading(type="loading"))
        elif transition == "hello":
            rig.link._dispatch(Hello(type="hello", protocol=41, client="fake", codepage="utf-8"))
        else:
            attempted = []
            release, watch = rig.link.body_release, rig.link.body_watch

            async def record_release(*args, **kwargs):
                attempted.append("release")
                return await release(*args, **kwargs)

            async def record_watch(*args, **kwargs):
                attempted.append("watch")
                return await watch(*args, **kwargs)

            monkeypatch.setattr(rig.link, "body_release", record_release)
            monkeypatch.setattr(rig.link, "body_watch", record_watch)
            # GameLink.run tears down the cached native state before declaring
            # the WebSocket closed; no command can travel over it afterward.
            rig.link._clear_body_observations()
            rig.link.closed.set()
            await eventually(lambda: rig.pilot._done.is_set())
            assert attempted == ["release", "watch", "watch"]
            assert rig.policy.commands("end_episode") and rig.policy.commands("close")
            assert not rig.link.body_leases and not rig.link.body_latest
            assert not rig.bridge.commands("body_release")
            assert [command["npcs"] for command in rig.bridge.commands("body_watch")] == [["NPC"]]
            return
        await rig.ended()


@pytest.mark.parametrize("transition", ["loading", "hello"])
@pytest.mark.parametrize("rpc", ["body_action", "body_events", "reset", "step"])
async def test_world_reset_reason_survives_late_rpc_failure(transition, rpc):
    async with running() as rig:
        waiter = asyncio.Event()
        if rpc == "reset":
            rig.policy.reset_waiter = waiter
            rig.policy.reset_mode = "error"
            await rig.acquire()
            await eventually(lambda: rig.policy.commands("reset"))
        else:
            await rig.active()
            if rpc == "step":
                rig.policy.step_waiter = waiter
                rig.policy.step_mode = "error"
                before = len(rig.policy.commands("step"))
                rig.bridge.emit(fighter(4, owned=True))
                await eventually(lambda: len(rig.policy.commands("step")) == before + 1)
            else:
                before = len(rig.bridge.commands(rpc))
                rig.bridge.response_delays[rpc] = 0.075
                if rpc == "body_action":
                    rig.bridge.emit(fighter(4, owned=True))
                else:
                    rig.fight._last_events_poll = float("-inf")
                    rig.fight._wake.set()
                await eventually(lambda: len(rig.bridge.commands(rpc)) == before + 1)
        if transition == "loading":
            rig.link._dispatch(Loading(type="loading"))
        else:
            rig.link._dispatch(Hello(
                type="hello", protocol=PROTOCOL_VERSION, client="fake", codepage="utf-8",
            ))
        waiter.set()
        await rig.ended()
        summary = rig.pilot.last_summary
        assert summary.reason == ("loading" if transition == "loading" else "session_changed")
        assert summary.release_confirmed and summary.watch_removed
        assert not rig.link.body_leases and not rig.link.body_latest
        assert len(rig.policy.commands("end_episode")) == len(rig.policy.commands("close")) == 1


async def test_loading_reason_survives_late_heartbeat_failure_while_policy_is_in_flight(monkeypatch):
    monkeypatch.setattr(bodypilot, "HEARTBEAT_INTERVAL", 0.010)
    async with running() as rig:
        await rig.active()
        waiter = asyncio.Event()
        rig.policy.step_waiter = waiter
        rig.policy.step_mode = "error"
        rig.bridge.response_delays["body_heartbeat"] = 0.050
        original = rig.link.body_heartbeat
        failures = []

        async def heartbeat(*args, **kwargs):
            try:
                return await original(*args, **kwargs)
            except GameError as error:
                failures.append(str(error))
                raise

        monkeypatch.setattr(rig.link, "body_heartbeat", heartbeat)
        steps = len(rig.policy.commands("step"))
        beats = len(rig.bridge.commands("body_heartbeat"))
        try:
            rig.bridge.emit(fighter(4, owned=True))
            await eventually(lambda: len(rig.policy.commands("step")) == steps + 1)
            await eventually(lambda: len(rig.bridge.commands("body_heartbeat")) == beats + 1)
            rig.link._dispatch(Loading(type="loading"))
            await eventually(lambda: failures)
        finally:
            waiter.set()
        await rig.ended()
        assert failures == ["body response belongs to an invalidated world"]
        assert rig.pilot.last_summary.reason == "loading"
        assert rig.pilot.last_summary.release_confirmed and rig.pilot.last_summary.watch_removed
        assert not rig.link.body_leases and not rig.link.body_latest


@pytest.mark.parametrize("rpc,stage", [("story_eval", "symbol_eval"), ("body_watch", "body_watch")])
@pytest.mark.parametrize("failure", ["timeout", "rejected"])
async def test_watch_setup_failure_identifies_rpc_and_failure_type(rpc, stage, failure, caplog):
    caplog.set_level("WARNING", logger="gothic_ai.bodypilot")
    async with running() as rig:
        if failure == "timeout":
            rig.bridge.response_delays[rpc] = 0.300
        else:
            rig.bridge.request_rejections[rpc] = deque([f"fake {rpc} rejected"])
        rig.roster(activity="fight", state="ZS_ATTACK")
        await eventually(lambda: rig.bridge.commands(rpc))
        # The first reply remains delayed, while an unsubscribe may acknowledge promptly.
        rig.bridge.response_delays.clear()
        await eventually(lambda: rig.pilot.last_summary is not None)
        await eventually(lambda: not rig.bridge._response_tasks)
        summary = rig.pilot.last_summary
        suffix = "timeout" if failure == "timeout" else "failed"
        assert summary.reason == f"{stage}_{suffix}"
        assert summary.lease is None and summary.acquire_requests == 0
        assert not summary.release_confirmed
        assert summary.watch_removed == (rpc == "body_watch")
        assert not rig.bridge.commands("body_acquire") and not rig.policy.requests
        watches = [command["npcs"] for command in rig.bridge.commands("body_watch")]
        assert watches == ([["NPC"], []] if rpc == "body_watch" else [])
        assert rig.bridge._watched_npc is None
        assert not rig.fight._watched and not rig.fight._watch_ready

        records = [record for record in caplog.records if "watch setup failed stage=" in record.message]
        assert len(records) == 1
        diagnostic = records[0].message
        assert f"stage={stage} rpc={rpc} npc=NPC" in diagnostic
        assert f"error_type={'TimeoutError' if failure == 'timeout' else 'GameError'}" in diagnostic
        assert "error=" in diagnostic and "elapsed_ms=" in diagnostic
        assert records[0].args[-1] >= 0
        if failure == "rejected":
            assert f"fake {rpc} rejected" in diagnostic


async def test_watch_timeout_clears_applied_subscription_and_ignores_late_replies():
    async with running() as rig:
        rig.bridge.response_delays["body_watch"] = 0.300
        rig.roster(activity="fight", state="ZS_ATTACK")
        await eventually(lambda: rig.bridge._watched_npc == "NPC")
        # The bridge applies the watch immediately, before its delayed acknowledgement.
        rig.bridge.emit(fighter())
        await eventually(lambda: any(not cmd["npcs"] for cmd in rig.bridge.commands("body_watch")))
        await asyncio.wait_for(rig.pilot.aclose(), 1.0)
        # Both acknowledgements arrive after their request deadlines. Neither
        # may revive a watch or allow the queued observation to acquire a body.
        await eventually(lambda: not rig.bridge._response_tasks)
        assert [cmd["npcs"] for cmd in rig.bridge.commands("body_watch")] == [["NPC"], [], []]
        assert rig.bridge._watched_npc is None
        assert rig.pilot.active_count == 0
        assert not rig.fight._watched and not rig.fight._watch_ready
        assert not rig.bridge.commands("body_acquire")
        assert not rig.bridge.commands("body_action")
        assert not rig.policy.requests
        summary = rig.pilot.last_summary
        assert summary.reason == "body_watch_timeout" and summary.lease is None
        assert summary.fight_seconds is None
        assert not summary.release_confirmed and summary.release_seconds is None
        assert not summary.watch_removed


async def test_five_minute_combat_limit_ends_episode():
    async with running() as rig:
        await rig.active()
        rig.clock.advance(300.01)
        rig.bridge.emit(fighter(4, owned=True, t_game_ms=300_151))
        await rig.ended()


async def test_no_decision_is_sent_after_takeover_during_slow_policy_reply():
    async with running() as rig:
        await rig.active()
        rig.policy.step_waiter = asyncio.Event()
        rig.bridge.emit(fighter(4, owned=True))
        await eventually(lambda: len(rig.policy.commands("step")) == 2)
        rig.link._dispatch(Attached(type="attached", npc="NPC", name="Fighter"))
        rig.policy.step_waiter.set()
        await rig.ended()
        assert len(rig.bridge.commands("body_action")) == 1
