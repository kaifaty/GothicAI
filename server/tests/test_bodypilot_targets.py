import asyncio
from dataclasses import FrozenInstanceError

import pytest

from gothic_ai import bodypilot
from gothic_ai.protocol import (
    Loading,
    RosterEntry,
)
from tests.bodypilot_helpers import (
    companion_entry,
    eventually,
    fighter,
    player_row,
    running,
    unseen_fighter,
)
from tests.test_body_control import events


@pytest.mark.parametrize("change", [
    {"dead": True}, {"unconscious": True}, {"down": True}, {"hp": 0},
])
async def test_npc_down_or_dead_releases_body_and_ends_episode(change):
    async with running() as rig:
        await rig.active()
        row = fighter(4, owned=True)
        row["self"].update(change)
        rig.bridge.emit(row)
        await rig.ended()


@pytest.mark.parametrize("change,reason", [
    ({"down": True}, "hero_down"), ({"hp": 0}, "hero_down"),
    ({"entity_id": 8}, "target_changed"),
])
async def test_hero_down_dead_or_target_changed_ends_episode(change, reason):
    async with running() as rig:
        await rig.active()
        row = fighter(4, owned=True)
        row["target"].update(change)
        rig.bridge.emit(row)
        await rig.ended()
        assert rig.pilot.last_summary.reason == reason
        assert rig.pilot.last_summary.target_kind == "hero"
        assert rig.pilot.last_summary.target_name is None


@pytest.mark.parametrize("companion", [False, True])
@pytest.mark.parametrize(("ended_activity", "ended_state"), [
    ("routine", "ZS_STAND_ARMSCROSSED"), ("down", "ZS_UNCONSCIOUS"),
])
async def test_down_fight_stays_claimed_until_native_routine_or_down_then_resets(
    companion, ended_activity, ended_state,
):
    async with running(companion=lambda npc: companion and npc == "CAVALORN") as rig:
        target = 3 if companion else 2
        extra = [companion_entry()] if companion else []
        rig.bridge.target_is_hero = not companion
        rig.bridge.target_npc = "CAVALORN" if companion else None
        await rig.active(target_entity=target, extra=extra)
        first_lease, first_client = rig.fight.lease, rig.fight.client
        down = fighter(4, owned=True, target_entity=target)
        down["target"].update(down=True, hp=1)
        rig.bridge.emit(down)
        await rig.ended()
        await eventually(lambda: not rig.pilot.active_count)
        assert rig.pilot.last_summary.reason == ("target_down" if companion else "hero_down")
        assert "NPC" in rig.pilot._claimed_fights
        assert "NPC" not in rig.pilot._lost_fights and "NPC" not in rig.pilot._retarget_pending

        # Neither combat-labelled routines nor temporary distance/absence prove
        # that native post-knockout cleanup has finished.
        rig.clock.advance(bodypilot.REACQUIRE_INTERVAL + 0.1)
        for activity, state, include, distance in (
            ("fight", "ZS_ATTACK", True, 200),
            ("routine", "ZS_ATTACK", True, 200),
            ("fight", "ZS_ATTACK", False, 200),
            ("fight", "ZS_ATTACK", True, 2500),
            ("fight", "ZS_ATTACK", True, 200),
        ):
            entries = [RosterEntry(
                id="NPC", name="Fighter", guild=1, distance=distance, activity=activity, state=state,
            )] if include else []
            rig.bridge.roster(*entries, *extra)
            for kind in ("attack", "threatened", "weapon_drawn", "npc_attack"):
                rig.combat(kind=kind)
            rig.combat(npc="PLAYER", kind="hero_attacked", other="NPC")
            await asyncio.sleep(0.01)
            assert "NPC" in rig.pilot._claimed_fights
            assert not rig.pilot.handles("NPC")
            assert not rig.link.body_leases and not rig.link.body_latest
            assert not rig.pilot._watch_npcs and not rig.pilot._pending_unwatch
            assert len(rig.bridge.commands("body_watch")) == 2
            assert len(rig.bridge.commands("body_acquire")) == len(rig.policy.commands("reset")) == 1

        rig.roster(activity=ended_activity, state=ended_state, extra=extra)
        assert "NPC" not in rig.pilot._claimed_fights
        rig.roster(activity="fight", state="ZS_ATTACK", extra=extra)
        await eventually(lambda: len(rig.bridge.commands("body_watch")) == 3)
        rig.bridge.emit(player_row(5), fighter(6, target_entity=target))
        await eventually(lambda: len(rig.policy.commands("reset")) == 2)
        assert rig.fight.client is not first_client and rig.fight.lease != first_lease
        rig.bridge.emit(fighter(7, owned=True, target_entity=target))
        await eventually(lambda: rig.fight._decisions == 1)
        assert len(rig.bridge.commands("body_acquire")) == 2


@pytest.mark.parametrize("companion", [False, True])
async def test_down_summary_restores_claim_if_routine_arrives_during_release(companion, monkeypatch):
    async with running(companion=lambda npc: companion and npc == "CAVALORN") as rig:
        target = 3 if companion else 2
        extra = [companion_entry()] if companion else []
        rig.bridge.target_is_hero = not companion
        rig.bridge.target_npc = "CAVALORN" if companion else None
        await rig.active(target_entity=target, extra=extra)
        first_client = rig.fight.client
        releasing, proceed = asyncio.Event(), asyncio.Event()
        original = rig.link.body_release

        async def held_release(*args, **kwargs):
            releasing.set()
            await proceed.wait()
            return await original(*args, **kwargs)

        monkeypatch.setattr(rig.link, "body_release", held_release)
        row = fighter(4, owned=True, target_entity=target)
        row["target"].update(down=True, hp=1)
        rig.bridge.emit(row)
        try:
            await eventually(releasing.is_set)
            rig.roster(activity="routine", state="ZS_TALK", extra=extra)
            assert rig.pilot.people["NPC"].activity == "routine"
            assert rig.pilot.people["NPC"].state == "ZS_TALK"
            assert not rig.pilot.summaries
        finally:
            proceed.set()
        await rig.ended()
        await eventually(lambda: not rig.pilot.active_count)
        assert rig.pilot.last_summary.reason == ("target_down" if companion else "hero_down")
        assert "NPC" in rig.pilot._claimed_fights

        rig.clock.advance(bodypilot.REACQUIRE_INTERVAL + 0.1)
        rig.roster(activity="fight", state="ZS_ATTACK", extra=extra)
        for kind in ("attack", "threatened", "weapon_drawn", "npc_attack"):
            rig.combat(kind=kind)
        rig.combat(npc="PLAYER", kind="hero_attacked", other="NPC")
        await asyncio.sleep(0.04)
        assert not rig.pilot.handles("NPC")
        assert len(rig.bridge.commands("body_watch")) == 2
        assert not rig.pilot._watch_npcs and not rig.link.body_latest

        # Only a routine observed after cleanup rearms the next real fight.
        rig.roster(activity="routine", state="ZS_STAND_ARMSCROSSED", extra=extra)
        rig.roster(activity="fight", state="ZS_ATTACK", extra=extra)
        await eventually(lambda: len(rig.bridge.commands("body_watch")) == 3)
        rig.bridge.emit(player_row(5), fighter(6, target_entity=target))
        await eventually(lambda: len(rig.policy.commands("reset")) == 2)
        assert rig.fight.client is not first_client
        rig.bridge.emit(fighter(7, owned=True, target_entity=target))
        await eventually(lambda: rig.fight._decisions == 1)


@pytest.mark.parametrize("target_state", ["down", "absent"])
async def test_prelease_down_waits_for_lease_and_absent_target_unwatches(target_state):
    async with running() as rig:
        await rig.watch()
        row = fighter(5, t_game_ms=250)
        if target_state == "down":
            row["target"].update(down=True, hp=1)
        else:
            row["self"]["weapon_state"] = 0
            row["target"].update(entity_id=0, present=False, visible=False, current_valid=False)
            for key in row["target"]:
                if key not in {
                    "entity_id", "present", "visible", "current_valid", "last_known_position_world_cm",
                    "last_known_velocity_cm_s", "last_seen_game_ms", "last_known_age_ms",
                }:
                    row["target"][key] = None
        rig.bridge.emit(row)
        await eventually(lambda: rig.fight._last_seq == 5)
        assert rig.pilot.handles("NPC")
        if target_state == "down":
            assert "NPC" not in rig.pilot._claimed_fights
            assert not rig.pilot.summaries
            rig.clock.advance(3)
            rig.roster(activity="fight", state="ZS_ATTACK")
            rig.bridge.emit(fighter(6, t_game_ms=300))
            await eventually(lambda: rig.policy.commands("reset"))
            assert len(rig.bridge.commands("body_acquire")) == 1
            assert not rig.pilot.summaries
            return
        rig.bridge.emit({**row, "observation_seq": 6, "t_game_ms": 1250})
        await eventually(lambda: not rig.pilot.active_count)
        assert rig.pilot.last_summary.reason == "target_lost"
        assert rig.pilot.last_summary.lease is None
        assert rig.pilot.last_summary.decisions == 0
        assert rig.pilot.last_summary.watch_removed
        assert not rig.link.body_leases and not rig.link.body_latest
        assert not rig.pilot._watch_npcs and not rig.pilot._pending_unwatch
        assert len(rig.bridge.commands("body_watch")) == 2
        assert not rig.bridge.commands("body_acquire") and not rig.policy.requests


async def test_prelease_companion_down_does_not_claim_an_unconfirmed_target():
    async with running(companion=lambda npc: npc == "CAVALORN") as rig:
        rig.bridge.target_is_hero = False
        rig.bridge.target_npc = "CAVALORN"
        await rig.watch(extra=[companion_entry()])
        await eventually(lambda: rig.fight._watch_ready)
        row = fighter(target_entity=3)
        row["target"].update(down=True, hp=1)
        rig.bridge.emit(row)
        await eventually(lambda: rig.fight._last_seq == row["observation_seq"])
        assert rig.pilot.handles("NPC")
        assert "NPC" not in rig.pilot._claimed_fights
        assert not rig.pilot.summaries
        assert not rig.bridge.commands("body_acquire") and not rig.policy.requests

        rig.clock.advance(3)
        rig.roster(activity="fight", state="ZS_ATTACK", extra=[companion_entry()])
        rig.bridge.emit(fighter(3, target_entity=3))
        await eventually(lambda: rig.policy.commands("reset"))
        assert rig.fight.lease.target_entity_id == 3
        assert rig.fight._target_kind == "companion" and rig.fight._target_name == "Кавалорн"
        assert len(rig.bridge.commands("body_acquire")) == len(rig.policy.commands("reset")) == 1
        assert not rig.pilot.summaries


async def test_stale_prelease_down_row_does_not_suppress_current_live_fight():
    async with running() as rig:
        await rig.watch()
        stale = fighter(2, t_game_ms=1)
        stale["target"]["down"] = True
        rig.bridge.emit(player_row(), stale, fighter(3, t_game_ms=200))
        await eventually(lambda: rig.policy.commands("reset"))
        rig.bridge.emit(fighter(4, owned=True, t_game_ms=250))
        await eventually(lambda: rig.bridge.commands("body_action"))
        assert len(rig.bridge.commands("body_acquire")) == 1
        assert len(rig.policy.commands("reset")) == 1
        assert not rig.pilot.summaries


@pytest.mark.parametrize("change", [{"down": True}, {"hp": 0}])
async def test_companion_down_or_dead_ends_with_target_down(change):
    async with running(companion=lambda npc: npc == "CAVALORN") as rig:
        rig.bridge.target_is_hero = False
        rig.bridge.target_npc = "CAVALORN"
        await rig.active(target_entity=3, extra=[companion_entry()])
        row = fighter(4, owned=True, target_entity=3)
        row["target"].update(change)
        rig.bridge.emit(row)
        await rig.ended()
        summary = rig.pilot.last_summary
        assert summary.reason == "target_down"
        assert summary.target_kind == "companion" and summary.target_name == "Кавалорн"
        assert summary.release_confirmed and summary.watch_removed


async def test_companion_to_hero_target_change_reacquires_and_resets_the_policy():
    async with running(companion=lambda npc: npc == "CAVALORN") as rig:
        rig.bridge.target_is_hero = False
        rig.bridge.target_npc = "CAVALORN"
        await rig.active(target_entity=3, extra=[companion_entry()])
        old_lease = rig.fight.lease
        rig.clock.advance(0.5)
        await rig.retarget(4, target_entity=2)
        summary = rig.pilot.last_summary
        assert summary.lease == old_lease
        assert summary.target_kind == "companion" and summary.target_name == "Кавалорн"
        assert summary.release_confirmed and summary.watch_removed

        rig.clock.now = 12.0
        await rig.reacquire(5, target_entity=2, count=2)
        assert rig.pilot.last_summary is summary
        assert [request["cmd"] for request in rig.policy.requests] == [
            "reset", "step", "end_episode", "close", "reset",
        ]
        assert [request["initial_game_ms"] for request in rig.policy.commands("reset")] == [100, 250]
        assert [command["target_entity_id"] for command in rig.bridge.commands("body_acquire")] == [3, 2]
        rig.bridge.emit(fighter(6, owned=True))
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 2)
        assert rig.bridge.commands("body_action")[-1]["target_entity_id"] == 2


@pytest.mark.parametrize("release_reason", ["native_script_takeover", "target_changed_or_lost"])
@pytest.mark.parametrize("old_target,new_target", [(3, 2), (2, 3)])
async def test_polled_native_target_change_reacquires_without_an_owned_target_row(
    release_reason, old_target, new_target,
):
    async with running(companion=lambda npc: npc == "CAVALORN") as rig:
        rig.bridge.target_is_hero = old_target == 2
        rig.bridge.target_npc = "CAVALORN" if old_target == 3 else None
        await rig.active(target_entity=old_target, extra=[companion_entry()])
        rig.clock.advance(0.25)
        # Native setTarget restores ownership before applying the deferred new target.
        # The event poll therefore reaches the pilot before any owned row with that target.
        rig.bridge.target_is_hero = new_target == 2
        rig.bridge.target_npc = "CAVALORN" if new_target == 3 else None
        rig.bridge.restore_native(release_reason=release_reason)
        await eventually(lambda: rig.pilot.last_summary is not None)
        await eventually(lambda: rig.pilot.active_count == 0)
        summary = rig.pilot.last_summary
        assert summary.reason == "target_changed"
        assert summary.native_release_reason == release_reason
        assert summary.target_kind == ("hero" if old_target == 2 else "companion")
        assert summary.release_confirmed and summary.watch_removed
        assert len(rig.policy.commands("step")) == 1

        rig.clock.now = 12.0
        await rig.reacquire(4, target_entity=new_target, count=2)
        assert [command["target_entity_id"] for command in rig.bridge.commands("body_acquire")] == [
            old_target, new_target,
        ]
        assert len(rig.policy.commands("reset")) == 2


@pytest.mark.parametrize("release_reason", ["native_script_takeover", "target_changed_or_lost"])
@pytest.mark.parametrize("new_target", ["CAVALORN", "OTHER"])
async def test_polled_native_release_keeps_same_or_unrelated_target_combat_claimed(
    release_reason, new_target,
):
    async with running(companion=lambda npc: npc == "CAVALORN") as rig:
        rig.bridge.target_is_hero = False
        rig.bridge.target_npc = "CAVALORN"
        await rig.active(target_entity=3, extra=[companion_entry()])
        rig.clock.advance(0.25)
        rig.bridge.target_npc = new_target
        rig.bridge.restore_native(release_reason=release_reason)
        await eventually(lambda: rig.pilot.last_summary is not None)
        await eventually(lambda: rig.pilot.active_count == 0)
        summary = rig.pilot.last_summary
        assert summary.reason == "native_restored"
        assert summary.native_release_reason == release_reason
        assert summary.release_confirmed and summary.watch_removed
        assert "NPC" in rig.pilot._claimed_fights

        rig.clock.advance(3)
        rig.bridge.target_is_hero = True
        rig.bridge.target_npc = None
        rig.roster(extra=[companion_entry()])
        rig.combat()
        rig.bridge.emit(fighter(4))
        await asyncio.sleep(0.060)
        assert len(rig.bridge.commands("body_watch")) == 2
        assert len(rig.bridge.commands("body_acquire")) == 1
        assert len(rig.policy.commands("reset")) == 1
        assert not rig.link.body_leases


@pytest.mark.parametrize("companion", [False, True])
async def test_polled_native_release_keeps_down_reason_from_passive_target_observation(companion):
    async with running(companion=lambda npc: companion and npc == "CAVALORN") as rig:
        rig.bridge.target_is_hero = not companion
        rig.bridge.target_npc = "CAVALORN" if companion else None
        target_entity = 3 if companion else 2
        await rig.active(target_entity=target_entity, extra=[companion_entry()] if companion else [])
        rig.bridge.restore_native(release_reason="target_changed_or_lost")
        row = fighter(4, target_entity=target_entity, periodic=False, event_only=True)
        row["target"]["down"] = True
        rig.bridge.emit(row)
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        assert summary.reason == ("target_down" if companion else "hero_down")
        assert summary.release_confirmed and summary.watch_removed
        assert len(rig.policy.commands("reset")) == 1


@pytest.mark.parametrize("companion", [False, True])
@pytest.mark.parametrize("scope", ["matching", "actor", "target", "generation"])
async def test_native_down_evidence_survives_target_clear_before_event_poll(companion, scope):
    async with running(companion=lambda npc: companion and npc == "CAVALORN") as rig:
        rig.bridge.target_is_hero = not companion
        rig.bridge.target_npc = "CAVALORN" if companion else None
        target_entity = 3 if companion else 2
        await rig.active(target_entity=target_entity, extra=[companion_entry()] if companion else [])
        rig.bridge.restore_native(release_reason="target_changed_or_lost")
        fallen = fighter(
            4, entity=4 if scope == "actor" else 1,
            target_entity=8 if scope == "target" else target_entity,
            control_generation=8 if scope == "generation" else 0,
            control_allowed=False, periodic=False, event_only=True,
        )
        fallen["target"]["down"] = True
        rig.bridge.emit(fallen)
        rig.bridge.emit(unseen_fighter(
            5, present=False, target_entity=0, control_generation=0,
            control_allowed=False, periodic=False, event_only=True,
        ))
        # Both validated snapshots arrive before the pilot can poll native_restored.
        latest = rig.link.body_latest[1]
        assert latest.observation_seq == 5 and latest.target.entity_id == 0
        assert not latest.target.present and not latest.control_allowed
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        expected = ("target_down" if companion else "hero_down") if scope == "matching" else (
            "lease_lost" if scope == "generation" else "native_restored"
        )
        assert summary.reason == expected
        assert summary.target_kind == ("companion" if companion else "hero")
        assert summary.release_confirmed and summary.watch_removed
        assert len(rig.policy.commands("step")) == 1
        assert len(rig.policy.commands("end_episode")) == 1


@pytest.mark.parametrize("companion", [False, True])
@pytest.mark.parametrize("evidence", ["matching", "missing", "foreign", "alive"])
async def test_heartbeat_native_handoff_requires_matching_evidence_for_down_reason(companion, evidence):
    async with running(companion=lambda npc: companion and npc == "CAVALORN") as rig:
        rig.bridge.target_is_hero = not companion
        rig.bridge.target_npc = "CAVALORN" if companion else None
        target_entity = 3 if companion else 2
        await rig.active(target_entity=target_entity, extra=[companion_entry()] if companion else [])
        original_call = rig.fight.client.call
        cleared_rows = []

        async def close_after_target_cleared(command, **fields):
            if command == "end_episode":
                # Native combat clears the fallen target while policy cleanup awaits.
                # That later snapshot must not erase the scoped down evidence.
                rig.bridge.emit(unseen_fighter(
                    5, present=False, target_entity=0, control_generation=0,
                    control_allowed=False, periodic=False, event_only=True,
                ))
                cleared_rows.append(rig.link.body_latest[1])
            return await original_call(command, **fields)

        rig.fight.client.call = close_after_target_cleared
        summary = await rig.lose_heartbeat_before_native_event(
            target_entity=target_entity, evidence=evidence, down=evidence != "alive",
        )
        expected = (
            ("target_down" if companion else "hero_down") if evidence == "matching" else "lease_lost"
        )
        assert summary.reason == expected
        assert summary.release_confirmed and summary.watch_removed
        assert summary.native_release_reason == (
            "target_changed_or_lost" if evidence in {"matching", "alive"} else None
        )
        assert len(rig.policy.commands("reset")) == 1
        assert len(rig.policy.commands("end_episode")) == 1
        assert len(cleared_rows) == 1
        latest = cleared_rows[0]
        assert latest.observation_seq == 5
        assert latest.control_generation == 0 and not latest.control_allowed
        assert latest.target.entity_id == 0 and not latest.target.present
        assert 1 not in rig.link.body_latest


async def test_heartbeat_native_target_change_reacquires_and_resets_after_cleanup_event():
    async with running(companion=lambda npc: npc == "CAVALORN") as rig:
        rig.bridge.target_is_hero = False
        rig.bridge.target_npc = "CAVALORN"
        await rig.active(target_entity=3, extra=[companion_entry()])
        rig.bridge.target_is_hero = True
        rig.bridge.target_npc = None
        summary = await rig.lose_heartbeat_before_native_event(target_entity=3)
        assert summary.reason == "target_changed"
        assert summary.native_release_reason == "target_changed_or_lost"
        assert summary.target_kind == "companion"
        assert summary.release_confirmed and summary.watch_removed
        rig.clock.now = 12.0
        await rig.reacquire(5, target_entity=2, count=2)
        assert [command["target_entity_id"] for command in rig.bridge.commands("body_acquire")] == [3, 2]
        assert len(rig.policy.commands("reset")) == 2


@pytest.mark.parametrize("rejection,handoff", [
    (rejection, handoff)
    for rejection in ("not_owned", "release_pending")
    for handoff in ("allowed", "unrelated", "missing", "foreign")
] + [("invalid_command", "allowed")])
async def test_action_rejection_reconciles_only_confirmed_native_target_change(rejection, handoff):
    async with running(companion=lambda npc: npc == "CAVALORN") as rig:
        rig.bridge.target_is_hero = False
        rig.bridge.target_npc = "CAVALORN"
        await rig.active(target_entity=3, extra=[companion_entry()])
        original_action = rig.link.body_action

        async def change_before_action(*args, **kwargs):
            rig.bridge.target_is_hero = handoff != "unrelated"
            rig.bridge.target_npc = "OTHER" if handoff == "unrelated" else None
            rig.bridge.restore_native(release_reason="target_changed_or_lost")
            if handoff == "missing":
                rig.bridge.control_events.clear()
            elif handoff == "foreign":
                rig.bridge.control_events[0]["control_generation"] += 1
            rig.bridge.action_rejections.append(rejection)
            return await original_action(*args, **kwargs)

        rig.link.body_action = change_before_action
        rig.bridge.emit(fighter(4, owned=True, target_entity=3))
        await rig.ended()
        await eventually(lambda: rig.pilot.active_count == 0)
        summary = rig.pilot.last_summary
        recapture = handoff == "allowed" and rejection != "invalid_command"
        assert summary.reason == ("target_changed" if recapture else "body_action_rejected")
        assert dict(summary.action_rejections) == {rejection: 1}
        assert summary.target_kind == "companion"
        assert summary.release_confirmed and summary.watch_removed
        assert summary.native_release_reason == (
            "target_changed_or_lost" if handoff in {"allowed", "unrelated"} else None
        )
        rig.clock.advance(3)
        if recapture:
            await rig.reacquire(5, target_entity=2, count=2)
            assert [command["target_entity_id"] for command in rig.bridge.commands("body_acquire")] == [3, 2]
            assert [request["initial_game_ms"] for request in rig.policy.commands("reset")] == [100, 250]
        else:
            rig.bridge.target_is_hero = True
            rig.bridge.target_npc = None
            rig.roster(extra=[companion_entry()])
            rig.combat()
            rig.bridge.emit(fighter(5))
            await asyncio.sleep(0.060)
            assert len(rig.bridge.commands("body_acquire")) == 1
            assert len(rig.policy.commands("reset")) == 1


@pytest.mark.parametrize("route,release_reason,evidence", [
    (route, reason, evidence)
    for route in ("status", "release_ack")
    for reason in ("native_script_takeover", "target_changed_or_lost")
    for evidence in ("matching", "missing", "foreign")
] + [
    ("release_ack", "native_script_takeover", "timeout"),
    ("release_ack", "native_script_takeover", "world_changed"),
])
async def test_native_ack_drains_later_target_change_event_before_unwatch(
    route, release_reason, evidence, monkeypatch,
):
    async with running(companion=lambda npc: npc == "CAVALORN") as rig:
        rig.bridge.target_is_hero = False
        rig.bridge.target_npc = "CAVALORN"
        await rig.active(target_entity=3, extra=[companion_entry()])
        lease = rig.fight.lease
        original_status = rig.link.body_control_status
        original_release = rig.link.body_release
        original_events = rig.link.body_events

        def change_native_target():
            rig.bridge.target_is_hero = True
            rig.bridge.target_npc = None
            if route == "status":
                rig.bridge.restore_native(release_reason=release_reason)
                rig.bridge._released_pending = False
                rig.bridge.release_pending_polls = 0
            else:
                # The native acknowledgement will clear GameLink's lease before
                # the pilot consumes this queued ownership event.
                rig.bridge.control_events.append(rig.bridge.control_event(
                    entity_id=lease.entity_id, session_epoch=lease.session_epoch,
                    world_epoch=lease.world_epoch, control_generation=lease.control_generation,
                    target_entity_id=lease.target_entity_id, reason="native_restored", owner="native",
                    release_reason=release_reason,
                ))
            if evidence == "missing":
                rig.bridge.control_events.clear()
            elif evidence == "foreign":
                rig.bridge.control_events[0]["control_generation"] += 1
            elif evidence == "timeout":
                rig.bridge.response_delays["body_events"] = 0.300

        async def native_before_status(*args, **kwargs):
            assert rig.bridge._released_pending and not rig.bridge.control_events
            change_native_target()
            return await original_status(*args, **kwargs)

        async def native_with_release_ack(*args, **kwargs):
            change_native_target()
            result = await original_release(*args, **kwargs)
            assert result.owner == "native" and not result.release_pending
            assert lease.entity_id not in rig.link.body_leases
            return result

        async def change_world_after_final_events(*args, **kwargs):
            result = await original_events(*args, **kwargs)
            rig.link._dispatch(Loading(type="loading"))
            return result

        if route == "status":
            rig.bridge.release_pending_polls = 1
            monkeypatch.setattr(rig.link, "body_control_status", native_before_status)
        else:
            monkeypatch.setattr(rig.link, "body_release", native_with_release_ack)
        if evidence == "world_changed":
            monkeypatch.setattr(rig.link, "body_events", change_world_after_final_events)
        # The rejection starts cleanup while the engine still holds the old target.
        rig.fight._last_events_poll = float("inf")
        first_command = len(rig.bridge.sent)
        rig.bridge.action_rejections.append("release_pending")
        rig.bridge.emit(fighter(4, owned=True, target_entity=3))
        await eventually(lambda: rig.pilot.last_summary is not None, timeout=bodypilot.FINISH_TIMEOUT + 0.5)
        await eventually(lambda: not rig.bridge._response_tasks)
        summary = rig.pilot.last_summary
        recapture = evidence == "matching"
        expected_reason = "loading" if evidence == "world_changed" else (
            "target_changed" if recapture else "body_action_rejected"
        )
        assert summary.reason == expected_reason
        assert summary.native_release_reason == (release_reason if recapture else None)
        assert dict(summary.action_rejections) == {"release_pending": 1}
        assert summary.release_confirmed and summary.watch_removed
        assert not rig.bridge.control_events

        commands = [command["type"] for command in rig.bridge.sent[first_command:]
                    if command["type"] != "story_eval"]
        release_index = commands.index("body_release")
        assert commands[release_index:] == (
            ["body_release", "body_events", "body_control_status", "body_events", "body_watch"]
            if route == "status" else ["body_release", "body_events", "body_watch"]
        )
        rig.clock.advance(3)
        if recapture:
            # New leases must no longer receive the handoff hooks from this fight.
            monkeypatch.setattr(rig.link, "body_release", original_release)
            monkeypatch.setattr(rig.link, "body_control_status", original_status)
            await rig.reacquire(5, target_entity=2, count=2)
            assert [command["target_entity_id"] for command in rig.bridge.commands("body_acquire")] == [3, 2]
            assert [request["initial_game_ms"] for request in rig.policy.commands("reset")] == [100, 250]
        else:
            assert "NPC" not in rig.pilot._retarget_pending
            if evidence != "world_changed":
                assert "NPC" in rig.pilot._claimed_fights
                rig.roster(extra=[companion_entry()])
                rig.combat()
                rig.bridge.emit(fighter(5))
            await asyncio.sleep(0.060)
            assert len(rig.bridge.commands("body_acquire")) == 1
            assert len(rig.policy.commands("reset")) == 1


async def test_target_change_reacquisition_waits_two_seconds_for_each_npc():
    async with running(companion=lambda npc: npc == "CAVALORN") as rig:
        rig.bridge.target_is_hero = False
        rig.bridge.target_npc = "CAVALORN"
        await rig.active(target_entity=3, extra=[companion_entry()])
        rig.clock.advance(0.25)
        await rig.retarget(4, target_entity=2)
        rig.clock.now = 11.999
        await asyncio.sleep(0.060)
        assert len(rig.bridge.commands("body_watch")) == 2
        assert len(rig.bridge.commands("body_acquire")) == 1

        rig.clock.now = 12.0
        await rig.reacquire(5, target_entity=2, count=2)
        rig.clock.advance(0.25)
        await rig.retarget(6, target_entity=3, target_npc="CAVALORN")
        rig.clock.now = 13.999
        await asyncio.sleep(0.060)
        assert len(rig.bridge.commands("body_watch")) == 4
        assert len(rig.bridge.commands("body_acquire")) == 2
        assert len(rig.policy.commands("reset")) == 2

        rig.clock.now = 14.0
        await rig.reacquire(7, target_entity=3, count=3)
        assert [command["target_entity_id"] for command in rig.bridge.commands("body_acquire")] == [3, 2, 3]


async def test_change_to_unrelated_npc_blocks_recapture_for_the_same_native_fight():
    async with running(companion=lambda npc: npc == "CAVALORN") as rig:
        rig.bridge.target_is_hero = False
        rig.bridge.target_npc = "CAVALORN"
        await rig.active(target_entity=3, extra=[companion_entry()])
        rig.clock.advance(0.25)
        await rig.retarget(4, target_entity=8, target_npc="OTHER")
        assert "NPC" in rig.pilot._claimed_fights
        rig.clock.advance(3)
        # Returning to an eligible target does not undo the end of this native fight.
        rig.bridge.target_is_hero = True
        rig.bridge.target_npc = None
        for seq in (5, 6):
            rig.roster(extra=[companion_entry()])
            rig.combat()
            rig.bridge.emit(fighter(seq))
            await asyncio.sleep(0.030)
        assert len(rig.bridge.commands("body_watch")) == 2
        assert len(rig.bridge.commands("body_acquire")) == 1
        assert len(rig.policy.commands("reset")) == 1
        assert not rig.link.body_leases


async def test_temporary_target_invisibility_recovers_without_ending_the_fight():
    async with running() as rig:
        await rig.active()
        for seq, game_ms in ((4, 1000), (5, 1250), (6, 1700)):
            rig.clock.advance(0.6)
            rig.bridge.emit(unseen_fighter(seq, t_game_ms=game_ms))
            await eventually(lambda seq=seq: len(rig.policy.commands("step")) == seq - 2)
            assert rig.fight._reason is None
            assert not rig.bridge.commands("body_release")
        rig.bridge.emit(fighter(7, owned=True, t_game_ms=1750))
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 2)
        assert rig.fight._reason is None and rig.fight._target_absent_game_ms is None
        assert len(rig.fight._policy_decisions) == 2  # Locked unseen rows are not policy decisions.
        row = fighter(8, owned=True, t_game_ms=1800)
        row["self"]["down"] = True
        rig.bridge.emit(row)
        await rig.ended()
        assert rig.pilot.last_summary.reason == "npc_down"


@pytest.mark.parametrize("present,target_entity", [(False, 0), (True, 2)])
async def test_target_absence_or_invalidity_ends_after_one_game_second(present, target_entity):
    async with running() as rig:
        await rig.active()
        for seq, game_ms in ((4, 1000), (5, 1999)):
            rig.bridge.emit(unseen_fighter(
                seq, present=present, target_entity=target_entity, t_game_ms=game_ms,
            ))
            await eventually(lambda seq=seq: len(rig.policy.commands("step")) == seq - 2)
            assert rig.fight._reason is None and not rig.bridge.commands("body_release")
        rig.bridge.emit(unseen_fighter(6, present=present, target_entity=target_entity, t_game_ms=2000))
        await rig.ended()
        assert rig.pilot.last_summary.reason == "target_lost"
        assert rig.pilot.last_summary.target_invisible_game_ms == 1000


async def test_target_validity_recovery_restarts_the_absence_grace():
    async with running() as rig:
        await rig.active()
        for seq, game_ms, visible in ((4, 1000, False), (5, 1700, True),
                                      (6, 1900, False), (7, 2899, False)):
            row = (fighter(seq, owned=True, t_game_ms=game_ms) if visible
                   else unseen_fighter(seq, t_game_ms=game_ms))
            rig.bridge.emit(row)
            await eventually(lambda seq=seq: len(rig.policy.commands("step")) == seq - 2)
            assert rig.fight._reason is None and not rig.bridge.commands("body_release")
        rig.bridge.emit(unseen_fighter(8, t_game_ms=2900))
        await rig.ended()
        assert rig.pilot.last_summary.reason == "target_lost"


@pytest.mark.parametrize("change,reason", [("target", "target_changed"), ("weapon", "weapon_changed")])
async def test_target_change_and_weapon_change_remain_immediate_while_unseen(change, reason):
    async with running() as rig:
        await rig.active()
        row = unseen_fighter(4, target_entity=8 if change == "target" else 2)
        if change == "weapon":
            row["self"]["weapon_state"] = 0
        rig.bridge.emit(row)
        await rig.ended()
        assert rig.pilot.last_summary.reason == reason


@pytest.mark.parametrize("scope", [
    {"entity_id": 8}, {"session_epoch": 2}, {"world_epoch": 2}, {"control_generation": 8},
])
async def test_native_visibility_restoration_respects_lease_identity(scope):
    async with running() as rig:
        await rig.active()
        rig.pilot.on_message(events(rig.bridge.control_event(
            reason="native_restored", release_reason="target_visibility_lost", owner="native", **scope,
        ), **{key: value for key, value in scope.items() if key in {"session_epoch", "world_epoch"}}))
        assert rig.fight._reason is None
        assert rig.fight._native_release_reason is None
        rig.link._dispatch(events(rig.bridge.control_event(
            reason="native_restored", release_reason="target_visibility_lost", owner="native",
        )))
        await rig.ended()
        assert rig.pilot.last_summary.reason == "target_lost"
        assert rig.pilot.last_summary.native_release_reason == "target_visibility_lost"


async def test_visibility_restoration_diagnostics_keep_last_ten_actual_policy_decisions(caplog):
    caplog.set_level("INFO", logger="gothic_ai.bodypilot")
    async with running() as rig:
        await rig.active()
        rig.bridge.emit(unseen_fighter(4, t_game_ms=1000))
        await eventually(lambda: len(rig.policy.commands("step")) == 2)
        rig.clock.advance(0.8)
        rig.bridge.emit(fighter(5, owned=True, t_game_ms=1800))
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 2)
        for seq in range(6, 18):
            if seq == 8:
                rig.bridge.action_rejections.append("masked")
            rig.bridge.emit(fighter(seq, owned=True, t_game_ms=1800 + seq))
            await eventually(lambda seq=seq: len(rig.bridge.commands("body_action")) == seq - 3)
        rig.bridge.emit(unseen_fighter(18, t_game_ms=2000))
        await eventually(lambda: len(rig.policy.commands("step")) == 16)
        rig.clock.advance(0.24)
        rig.bridge.emit(unseen_fighter(19, t_game_ms=2200))
        await eventually(lambda: len(rig.policy.commands("step")) == 17)
        rig.clock.advance(0.35)
        rig.link._dispatch(events(rig.bridge.control_event(
            reason="native_restored", release_reason="target_visibility_lost", owner="native",
            t_game_ms=2400,
        )))
        await rig.ended()
        summary = rig.pilot.last_summary
        assert summary.reason == "target_lost" and summary.release_confirmed
        assert summary.target_invisible_seconds == pytest.approx(0.8)
        assert summary.target_invisible_game_ms == 800
        assert summary.target_invisible_last_seconds == pytest.approx(0.59)
        assert summary.target_invisible_last_game_ms == 400
        decisions = summary.last_policy_decisions
        assert tuple(row.observation_seq for row in decisions) == tuple(range(8, 18))
        assert all(row.action_id == 1 and row.sent for row in decisions)
        assert not decisions[0].accepted and decisions[0].reason == "masked"
        assert all(row.accepted and row.reason == "accepted" for row in decisions[1:])
        assert not rig.fight._policy_decisions
        with pytest.raises(FrozenInstanceError):
            decisions[0].accepted = True
        assert "invisible_max_game_ms=800" in caplog.text
        assert "invisible_last_game_ms=400" in caplog.text
        assert "native_release_reason=target_visibility_lost release_confirmed=True" in caplog.text
