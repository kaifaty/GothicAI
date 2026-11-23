import asyncio
import json
from collections import deque

import pytest

from gothic_ai import bodypilot
from gothic_ai.game import GameDisconnected
from gothic_ai.protocol import (
    Loading,
    Result,
)
from tests.bodypilot_helpers import (
    companion_entry,
    eventually,
    multi_entry,
    multi_fighter,
    npc_entity,
    running,
    start_fights,
)
from tests.test_body_control import events


async def test_three_fights_have_separate_leases_connections_resets_and_action_streams():
    async with running(multi=True) as rig:
        npcs = ("NPC_1", "NPC_2", "NPC_3")
        assert rig.pilot.active_count == 0
        assert not any(rig.pilot.handles(npc) for npc in npcs)
        rig.bridge.roster(*(multi_entry(npc) for npc in npcs))
        await eventually(lambda: rig.pilot.active_count == 3)
        assert all(rig.pilot.handles(npc.lower()) for npc in npcs)
        assert not rig.link.body_leases and not rig.policy.connections
        await eventually(lambda: all(rig.pilot.fights[npc]._watch_ready for npc in npcs))
        assert set(rig.bridge.watched) == set(npcs)
        rig.bridge.emit(*(multi_fighter(npc) for npc in npcs))
        await eventually(lambda: len(rig.policy.commands("reset")) == 3)
        assert set(rig.link.body_leases) == {npc_entity(npc) for npc in npcs}
        assert len(rig.policy.connections) == 3
        assert len({id(fight.client) for fight in rig.pilot.fights.values()}) == 3
        assert not rig.policy.commands("step")
        owned_rows = {npc: multi_fighter(npc, 3, owned=True) for npc in npcs}
        rig.bridge.emit(*owned_rows.values())
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 3)
        for npc in npcs:
            lease = rig.pilot.fights[npc].lease
            assert lease.npc == npc and lease.entity_id == npc_entity(npc)
            steps = [request for request in rig.policy.commands("step")
                     if request["observation"]["entity_id"] == lease.entity_id]
            assert len(steps) == 1 and steps[0]["observation"] == owned_rows[npc]
            connection = rig.policy.request_connections[id(steps[0])]
            requests = [request for request in rig.policy.requests
                        if rig.policy.request_connections[id(request)] == connection]
            assert [request["cmd"] for request in requests] == ["reset", "step"]
            action = next(command for command in rig.bridge.commands("body_action")
                          if command["entity_id"] == lease.entity_id)
            assert action["control_generation"] == lease.control_generation
            assert action["goal_version"] == lease.goal_version
            assert action["observation_seq"] == owned_rows[npc]["observation_seq"]
            assert action["target_entity_id"] == lease.target_entity_id
        assert rig.pilot.active_count == 3 and all(rig.pilot.handles(npc) for npc in npcs)


async def test_one_npc_down_shrinks_watch_after_release_ack_and_keeps_other_fights_running():
    async with running(multi=True) as rig:
        npcs = ("NPC_1", "NPC_2", "NPC_3")
        await start_fights(rig, *npcs)
        preserved = {npc: rig.pilot.fights[npc].lease for npc in npcs[1:]}
        rig.bridge.release_ack = asyncio.Event()
        down = multi_fighter(npcs[0], 4, owned=True)
        down["self"]["down"] = True
        rig.bridge.emit(down)
        await eventually(lambda: rig.bridge.commands("body_release"))
        assert rig.pilot.handles(npcs[0])  # Cleanup still owns the subscription slot.
        assert set(rig.bridge.watched) == set(npcs)
        assert not rig.policy.commands("end_episode")
        rig.bridge.emit(*(multi_fighter(npc, 4, owned=True) for npc in npcs[1:]))
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 5)
        rig.bridge.release_ack.set()
        await eventually(lambda: rig.pilot.active_count == 2)
        assert not rig.pilot.handles(npcs[0])
        assert all(rig.pilot.handles(npc) for npc in npcs[1:])
        assert set(rig.bridge.watched) == set(npcs[1:])
        assert {npc: rig.pilot.fights[npc].lease for npc in npcs[1:]} == preserved
        assert len(rig.bridge.commands("body_release")) == 1
        assert rig.bridge.commands("body_release")[0]["entity_id"] == npc_entity(npcs[0])
        assert len(rig.policy.commands("end_episode")) == len(rig.policy.commands("close")) == 1
        summary = rig.pilot.last_summary
        assert summary.npc == npcs[0] and summary.reason == "npc_down"
        assert summary.release_confirmed and summary.watch_removed
        assert list(rig.pilot.summaries) == [summary]
        release = rig.bridge.commands("body_release")[0]
        shrink = rig.bridge.commands("body_watch")[-1]
        assert rig.bridge.sent.index(release) < rig.bridge.sent.index(shrink)
        assert len(rig.policy.writers) == 2
        rig.bridge.emit(*(multi_fighter(npc, 5, owned=True) for npc in npcs[1:]))
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 7)
        assert len(rig.policy.commands("reset")) == 3


async def test_max_four_keeps_fifth_native_until_a_fight_frees_its_slot():
    async with running(multi=True, max_fights=4) as rig:
        npcs = tuple(f"NPC_{index}" for index in range(1, 6))
        await start_fights(rig, *npcs[:4])
        rig.bridge.roster(*(multi_entry(npc, activity="body" if index < 4 else "fight")
                            for index, npc in enumerate(npcs)))
        rig.combat(npcs[4])
        await asyncio.sleep(0.02)
        assert rig.pilot.active_count == len(rig.link.body_leases) == len(rig.policy.connections) == 4
        assert not rig.pilot.handles(npcs[4])
        assert npcs[4] not in rig.bridge.watched
        assert len(rig.bridge.commands("body_acquire")) == 4
        down = multi_fighter(npcs[0], 4, owned=True)
        down["self"]["down"] = True
        rig.bridge.emit(down)
        await eventually(lambda: not rig.pilot.handles(npcs[0]))
        rig.clock.advance(bodypilot.REACQUIRE_INTERVAL)
        rig.combat(npcs[4])
        rig.bridge.roster(*(multi_entry(npc, activity="body" if npc in npcs[1:4] else "fight")
                            for npc in npcs))
        await eventually(lambda: rig.pilot.handles(npcs[4]))
        await eventually(lambda: rig.pilot.fights[npcs[4]]._watch_ready)
        rig.bridge.emit(multi_fighter(npcs[4], 3))
        await eventually(lambda: len(rig.bridge.commands("body_acquire")) == 5)
        assert rig.pilot.active_count == len(rig.link.body_leases) == 4
        assert all(rig.pilot.handles(npc) for npc in npcs[1:])
        assert not rig.pilot.handles(npcs[0])


async def test_legacy_watch_growth_refusal_preserves_running_fight_and_retries_after_interval(caplog):
    caplog.set_level("INFO", logger="gothic_ai.bodypilot")
    async with running(multi=True) as rig:
        await start_fights(rig, "NPC_1")
        first = rig.pilot.fights["NPC_1"]
        lease, client = first.lease, first.client
        rig.bridge.legacy_watch = True
        rig.bridge.roster(multi_entry("NPC_1", activity="body"), multi_entry("NPC_2"))
        rig.combat("NPC_2")
        await eventually(lambda: rig.pilot.last_summary is not None)
        assert rig.pilot.last_summary.npc == "NPC_2"
        assert rig.pilot.last_summary.reason == "watch_refused"
        assert "release active body owners before changing body_watch" in caplog.text
        assert rig.pilot.active_count == 1 and rig.pilot.handles("NPC_1")
        assert not rig.pilot.handles("NPC_2")
        assert rig.pilot.fights["NPC_1"] is first and first.lease == lease and first.client is client
        assert rig.bridge.watched == ["NPC_1"]
        assert rig.link.body_leases == {lease.entity_id: lease}
        assert len(rig.policy.connections) == len(rig.policy.commands("reset")) == 1
        assert not rig.bridge.commands("body_release") and not rig.policy.commands("end_episode")
        attempts = len(rig.bridge.commands("body_watch"))
        rig.clock.advance(bodypilot.REACQUIRE_INTERVAL - 0.001)
        rig.combat("NPC_2")
        rig.bridge.emit(multi_fighter("NPC_1", 4, owned=True))
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 2)
        assert len(rig.bridge.commands("body_watch")) == attempts
        rig.bridge.legacy_watch = False
        rig.clock.advance(0.002)
        rig.combat("NPC_2")
        await eventually(lambda: rig.pilot.handles("NPC_2"))
        await eventually(lambda: rig.pilot.fights["NPC_2"]._watch_ready)
        rig.bridge.emit(multi_fighter("NPC_2"))
        await eventually(lambda: len(rig.policy.commands("reset")) == 2)
        assert len(rig.link.body_leases) == 2
        assert first.lease == lease and first.client is client


async def test_loading_finishes_every_fight_and_policy_connection():
    async with running(multi=True) as rig:
        npcs = ("NPC_1", "NPC_2", "NPC_3")
        await start_fights(rig, *npcs)
        rig.link._dispatch(Loading(type="loading"))
        await eventually(lambda: rig.pilot.active_count == 0)
        assert not any(rig.pilot.handles(npc) for npc in npcs)
        assert not rig.link.body_leases and not rig.bridge.watched
        assert {command["entity_id"] for command in rig.bridge.commands("body_release")} == {
            npc_entity(npc) for npc in npcs
        }
        assert len(rig.policy.commands("end_episode")) == len(rig.policy.commands("close")) == 3
        assert len(rig.pilot.summaries) == 3
        assert {summary.npc for summary in rig.pilot.summaries} == set(npcs)
        assert all(summary.reason == "loading" for summary in rig.pilot.summaries)
        assert rig.pilot.last_summary is rig.pilot.summaries[-1]


async def test_observation_overflow_ends_only_the_actor_whose_queue_filled():
    async with running(multi=True) as rig:
        await start_fights(rig, "NPC_1", "NPC_2")
        second = rig.pilot.fights["NPC_2"]
        lease, client = second.lease, second.client
        # Fill one queue synchronously while the event loop cannot drain it.
        for seq in range(4, 134):
            rig.bridge.emit(multi_fighter("NPC_1", seq, owned=True, can_decide=False))
        await eventually(lambda: rig.pilot.active_count == 1)
        assert rig.pilot.last_summary.reason == "observation_overflow"
        assert rig.pilot.last_summary.npc == "NPC_1"
        assert rig.pilot.handles("NPC_2") and not rig.pilot.handles("NPC_1")
        assert second.lease == lease and second.client is client
        assert len(rig.bridge.commands("body_release")) == len(rig.policy.commands("end_episode")) == 1
        # Keep the remaining row fresh relative to the packet's latest game time.
        rig.bridge.emit(multi_fighter("NPC_2", 134, owned=True))
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 3)
        assert rig.bridge.commands("body_action")[-1]["entity_id"] == lease.entity_id
        assert rig.bridge.watched == ["NPC_2"]


async def test_shrink_refusal_is_not_fatal_to_remaining_fights():
    async with running(multi=True) as rig:
        await start_fights(rig, "NPC_1", "NPC_2")
        rig.bridge.refuse_shrink = True
        down = multi_fighter("NPC_1", 4, owned=True)
        down["self"]["down"] = True
        rig.bridge.emit(down)
        await eventually(lambda: rig.pilot.active_count == 1)
        assert rig.pilot.last_summary.reason == "npc_down"
        assert rig.pilot.last_summary.release_confirmed and not rig.pilot.last_summary.watch_removed
        assert rig.pilot.handles("NPC_2") and rig.link.body_owned("NPC_2")
        rig.bridge.emit(multi_fighter("NPC_2", 4, owned=True))
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 3)
        assert len(rig.bridge.commands("body_release")) == 1
        rig.bridge.refuse_shrink = False


async def test_watch_grows_while_first_acquisition_is_pending_without_resetting_it():
    async with running(multi=True) as rig:
        rig.bridge.acquire_ack["NPC_1"] = asyncio.Event()
        rig.bridge.roster(multi_entry("NPC_1"))
        await eventually(lambda: rig.pilot.handles("NPC_1"))
        await eventually(lambda: rig.pilot.fights["NPC_1"]._watch_ready)
        initial = multi_fighter("NPC_1")
        rig.bridge.emit(initial)
        await eventually(lambda: rig.link.body_acquire_pending(npc_entity("NPC_1")))
        first = rig.pilot.fights["NPC_1"]
        rig.bridge.roster(multi_entry("NPC_1"), multi_entry("NPC_2"))
        await eventually(lambda: rig.pilot.handles("NPC_2"))
        await eventually(lambda: rig.pilot.fights["NPC_2"]._watch_ready)
        assert rig.pilot.active_count == 2 and rig.pilot.fights["NPC_1"] is first
        assert rig.pilot.last_summary is None
        assert rig.link.body_acquire_pending(npc_entity("NPC_1"))
        assert rig.link.body_latest[npc_entity("NPC_1")].observation_seq == initial["observation_seq"]
        assert not rig.link.body_leases and not rig.policy.requests
        assert not rig.bridge.commands("body_release")
        assert [command["npcs"] for command in rig.bridge.commands("body_watch")] == [
            ["NPC_1"], ["NPC_1", "NPC_2"],
        ]
        rig.bridge.emit(multi_fighter("NPC_2"))
        await eventually(lambda: len(rig.policy.commands("reset")) == 1)
        second = rig.pilot.fights["NPC_2"]
        second_lease, second_client = second.lease, second.client
        rig.bridge.emit(multi_fighter("NPC_2", 3, owned=True))
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 1)
        assert rig.link.body_acquire_pending(npc_entity("NPC_1"))
        assert rig.bridge.commands("body_action")[0]["entity_id"] == second_lease.entity_id
        rig.bridge.acquire_ack["NPC_1"].set()
        await eventually(lambda: len(rig.policy.commands("reset")) == 2)
        rig.bridge.emit(multi_fighter("NPC_1", 3, owned=True))
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 2)
        assert rig.pilot.active_count == len(rig.link.body_leases) == 2
        assert first.lease.npc == "NPC_1" and first.lease.target_entity_id == 2
        assert second.lease == second_lease and second.client is second_client
        assert len(rig.policy.connections) == 2
        assert len(rig.bridge.commands("body_acquire")) == 2
        assert not rig.bridge.commands("body_release") and not rig.policy.commands("end_episode")


@pytest.mark.parametrize("release_reason, expected_reason", [
    ("target_visibility_lost", "target_lost"),
    ("target_changed_or_lost", "target_changed"),
])
async def test_pending_acquire_event_poll_preserves_another_fights_native_release_reason(
    release_reason, expected_reason,
):
    async with running(multi=True, companion=lambda npc: npc == "CAVALORN") as rig:
        await start_fights(rig, "NPC_1")
        first = rig.pilot.fights["NPC_1"]
        lease = first.lease
        first._last_events_poll = float("inf")
        rig.bridge.acquire_ack["NPC_2"] = asyncio.Event()
        native_events = deque()
        polled_while_pending = []
        target_queries = []
        original_send = rig.bridge.send
        new_target = False

        async def send(raw):
            command = json.loads(raw)
            if command["type"] == "body_events" and native_events:
                polled_while_pending.append(rig.link.body_acquire_pending(npc_entity("NPC_2")))
                rig.bridge.sent.append(command)
                rig.bridge.sent_times.append((command, asyncio.get_running_loop().time()))
                rig.link._dispatch(Result(
                    type="result", req=command["req"], ok=True,
                    value=events(native_events.popleft()).model_dump(mode="json"),
                ))
            elif (
                new_target and command["type"] == "story_eval" and command["npc"] == "NPC_1"
                and bodypilot.TARGET_IS_HERO in command["exprs"]
            ):
                rig.bridge.sent.append(command)
                rig.bridge.sent_times.append((command, asyncio.get_running_loop().time()))
                values = [0 if expr == bodypilot.TARGET_IS_HERO else 1 for expr in command["exprs"]]
                target_queries.append((command["exprs"], values))
                rig.link._dispatch(Result(type="result", req=command["req"], ok=True, value=values))
            else:
                await original_send(raw)

        rig.bridge.send = send
        rig.bridge.roster(
            multi_entry("NPC_1", activity="body"), multi_entry("NPC_2"), companion_entry(),
        )
        await eventually(lambda: rig.pilot.handles("NPC_2"))
        await eventually(lambda: rig.pilot.fights["NPC_2"]._watch_ready)
        rig.bridge.emit(multi_fighter("NPC_2"))
        await eventually(lambda: rig.link.body_acquire_pending(npc_entity("NPC_2")))
        new_target = expected_reason == "target_changed"
        native_events.append(rig.bridge.control_event(
            entity_id=lease.entity_id, control_generation=lease.control_generation,
            target_entity_id=lease.target_entity_id, reason="native_restored", owner="native",
            release_reason=release_reason, t_game_ms=200,
        ))
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        assert polled_while_pending == [True]
        if expected_reason == "target_changed":
            assert target_queries
            for expressions, values in target_queries:
                assert expressions[0] == bodypilot.TARGET_IS_HERO and len(expressions) == 2
                assert values == [0, 1]  # Native target is Cavalorn rather than the hero.
        else:
            assert not target_queries
        assert summary.npc == "NPC_1" and summary.reason == expected_reason
        assert summary.native_release_reason == release_reason and summary.release_confirmed
        assert rig.pilot.handles("NPC_2") and not rig.pilot.handles("NPC_1")
        assert ("NPC_1" in rig.pilot._retarget_pending) == (expected_reason == "target_changed")
        assert len(rig.bridge.commands("body_release")) == 1
        assert rig.link.body_acquire_pending(npc_entity("NPC_2"))
        rig.bridge.acquire_ack["NPC_2"].set()
        await eventually(lambda: len(rig.policy.commands("reset")) == 2)
        rig.bridge.emit(multi_fighter("NPC_2", 3, owned=True))
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 2)
        assert rig.pilot.active_count == 1


async def test_final_fight_clears_stale_watch_entries_after_an_earlier_shrink_refusal():
    async with running(multi=True) as rig:
        await start_fights(rig, "NPC_1", "NPC_2")
        rig.bridge.refuse_shrink = True
        first_down = multi_fighter("NPC_1", 4, owned=True)
        first_down["self"]["down"] = True
        rig.bridge.emit(first_down)
        await eventually(lambda: rig.pilot.active_count == 1)
        assert set(rig.bridge.watched) == {"NPC_1", "NPC_2"}
        assert not rig.pilot.last_summary.watch_removed
        rig.bridge.refuse_shrink = False
        second_down = multi_fighter("NPC_2", 4, owned=True)
        second_down["self"]["down"] = True
        rig.bridge.emit(second_down)
        await eventually(lambda: rig.pilot.active_count == 0)
        assert rig.bridge.watched == []
        assert rig.bridge.commands("body_watch")[-1]["npcs"] == []
        assert not rig.link.body_leases
        assert len(rig.policy.commands("end_episode")) == len(rig.bridge.commands("body_release")) == 2
        assert rig.pilot.last_summary.npc == "NPC_2"
        assert rig.pilot.last_summary.reason == "npc_down" and rig.pilot.last_summary.watch_removed


@pytest.mark.parametrize("protocol", [None, 43, 44])
@pytest.mark.parametrize("stage", ["observing", "pending", "active"])
async def test_pre_v45_or_unknown_protocol_keeps_new_fight_native_until_watch_is_free(protocol, stage):
    async with running(multi=True, protocol=protocol) as rig:
        rig.bridge.legacy_watch = True
        assert rig.pilot.watch_busy == 0
        if stage == "active":
            await start_fights(rig, "NPC_1")
        else:
            rig.bridge.roster(multi_entry("NPC_1"))
            await eventually(lambda: rig.pilot.handles("NPC_1"))
            await eventually(lambda: rig.pilot.fights["NPC_1"]._watch_ready)
            if stage == "pending":
                rig.bridge.acquire_ack["NPC_1"] = asyncio.Event()
                rig.bridge.emit(multi_fighter("NPC_1"))
                await eventually(lambda: rig.link.body_acquire_pending(npc_entity("NPC_1")))
        first = rig.pilot.fights["NPC_1"]
        first_lease, first_client = first.lease, first.client
        before = {kind: len(rig.bridge.commands(kind)) for kind in (
            "story_eval", "body_watch", "body_acquire", "body_release",
        )}
        connections, resets = len(rig.policy.connections), len(rig.policy.commands("reset"))
        rig.bridge.roster(
            multi_entry("NPC_1", activity="body" if stage == "active" else "fight"),
            multi_entry("NPC_2"),
        )
        assert not rig.pilot.handles("NPC_2")
        assert rig.pilot.active_count == 1
        assert rig.pilot.watch_busy == 1
        assert rig.pilot.last_summary is None and not rig.pilot.summaries
        assert rig.pilot.fights["NPC_1"] is first
        assert first.lease == first_lease and first.client is first_client
        assert all(len(rig.bridge.commands(kind)) == count for kind, count in before.items())
        assert len(rig.policy.connections) == connections
        assert len(rig.policy.commands("reset")) == resets
        assert not rig.policy.commands("end_episode")
        assert rig.bridge.watched == ["NPC_1"]
        if stage == "pending":
            assert rig.link.body_acquire_pending(npc_entity("NPC_1"))
            rig.bridge.acquire_ack["NPC_1"].set()
        elif stage == "observing":
            rig.bridge.emit(multi_fighter("NPC_1"))
        await eventually(lambda: len(rig.policy.commands("reset")) == 1)
        down = multi_fighter("NPC_1", 4, owned=True)
        down["self"]["down"] = True
        rig.bridge.emit(down)
        await eventually(lambda: rig.pilot.active_count == 0)
        assert rig.bridge.watched == [] and not rig.link.body_leases
        assert len(rig.policy.commands("end_episode")) == 1
        summary = rig.pilot.last_summary
        assert summary.npc == "NPC_1" and summary.reason == "npc_down"
        assert list(rig.pilot.summaries) == [summary]
        rig.clock.advance(bodypilot.REACQUIRE_INTERVAL - 0.001)
        rig.combat("NPC_2")
        await asyncio.sleep(0.01)
        assert not rig.pilot.handles("NPC_2") and rig.pilot.active_count == 0
        assert rig.pilot.watch_busy == 1
        rig.clock.advance(0.002)
        rig.combat("NPC_2")
        await eventually(lambda: rig.pilot.handles("NPC_2"))
        await eventually(lambda: rig.pilot.fights["NPC_2"]._watch_ready)
        assert rig.bridge.watched == ["NPC_2"]
        rig.bridge.emit(multi_fighter("NPC_2"))
        await eventually(lambda: len(rig.policy.commands("reset")) == 2)
        rig.bridge.emit(multi_fighter("NPC_2", 3, owned=True))
        actions = 2 if stage == "active" else 1
        await eventually(lambda: len(rig.bridge.commands("body_action")) == actions)
        assert rig.pilot.active_count == 1 and rig.pilot.handles("NPC_2")
        assert rig.link.body_leases[npc_entity("NPC_2")].target_entity_id == 2
        assert rig.bridge.commands("body_action")[-1]["entity_id"] == npc_entity("NPC_2")
        assert rig.bridge.commands("body_action")[-1]["target_entity_id"] == 2
        assert rig.pilot.watch_busy == 1
        assert rig.pilot.last_summary is summary and list(rig.pilot.summaries) == [summary]


async def test_failed_shrink_is_retried_without_ending_or_resetting_other_fights():
    async with running(multi=True) as rig:
        await start_fights(rig, "NPC_1", "NPC_2", "NPC_3")
        preserved = {
            npc: (rig.pilot.fights[npc], rig.pilot.fights[npc].lease, rig.pilot.fights[npc].client)
            for npc in ("NPC_2", "NPC_3")
        }
        rig.bridge.refuse_shrink = True
        down = multi_fighter("NPC_1", 4, owned=True)
        down["self"]["down"] = True
        rig.bridge.emit(down)
        await eventually(lambda: rig.pilot.active_count == 2)
        summary = rig.pilot.last_summary
        assert summary.reason == "npc_down" and summary.release_confirmed
        assert not summary.watch_removed
        assert set(rig.bridge.watched) == {"NPC_1", "NPC_2", "NPC_3"}
        assert npc_entity("NPC_1") in rig.link.body_latest
        attempts = len(rig.bridge.commands("body_watch"))
        rig.bridge.refuse_shrink = False
        await eventually(lambda: set(rig.bridge.watched) == {"NPC_2", "NPC_3"})
        assert len(rig.bridge.commands("body_watch")) > attempts
        assert rig.pilot.active_count == 2 and not rig.pilot.handles("NPC_1")
        assert npc_entity("NPC_1") not in rig.link.body_latest
        assert rig.pilot.last_summary is summary and not summary.watch_removed
        assert len(rig.bridge.commands("body_release")) == len(rig.policy.commands("end_episode")) == 1
        assert len(rig.policy.commands("reset")) == len(rig.policy.connections) == 3
        for npc, (fight, lease, client) in preserved.items():
            assert rig.pilot.fights[npc] is fight
            assert fight.lease == lease and fight.client is client
            assert rig.link.body_leases[lease.entity_id] == lease
            assert rig.pilot.handles(npc)
        rig.bridge.emit(multi_fighter("NPC_2", 5, owned=True), multi_fighter("NPC_3", 5, owned=True))
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 5)


async def test_target_change_reacquires_after_two_seconds_even_when_shrink_was_refused():
    async with running(multi=True, companion=lambda npc: npc == "CAVALORN") as rig:
        await start_fights(rig, "NPC_1", "NPC_2")
        first = rig.pilot.fights["NPC_1"]
        old_lease, old_client = first.lease, first.client
        second = rig.pilot.fights["NPC_2"]
        second_lease, second_client = second.lease, second.client
        rig.bridge.roster(
            multi_entry("NPC_1", activity="body"), multi_entry("NPC_2", activity="body"),
            companion_entry(),
        )
        rig.bridge.refuse_shrink = True
        rig.bridge.target_npcs["NPC_1"] = "CAVALORN"
        rig.bridge.emit(multi_fighter("NPC_1", 4, owned=True, target_entity=3))
        await eventually(lambda: rig.pilot.active_count == 1)
        summary = rig.pilot.last_summary
        assert summary.npc == "NPC_1" and summary.reason == "target_changed"
        assert summary.lease == old_lease and summary.target_kind == "hero"
        assert summary.release_confirmed and not summary.watch_removed
        assert "NPC_1" in rig.pilot._retarget_pending
        assert set(rig.bridge.watched) == {"NPC_1", "NPC_2"}
        rig.clock.advance(bodypilot.REACQUIRE_INTERVAL - 0.001)
        rig.combat("NPC_1")
        await asyncio.sleep(0.03)
        assert not rig.pilot.handles("NPC_1")
        assert len(rig.bridge.commands("body_acquire")) == len(rig.policy.commands("reset")) == 2
        assert rig.pilot.fights["NPC_2"] is second
        assert second.lease == second_lease and second.client is second_client
        rig.clock.advance(0.002)
        await eventually(lambda: rig.pilot.handles("NPC_1"))
        await eventually(lambda: rig.pilot.fights["NPC_1"]._watch_ready)
        rig.bridge.emit(multi_fighter("NPC_1", 5, target_entity=3))
        await eventually(lambda: len(rig.policy.commands("reset")) == 3)
        replacement = rig.pilot.fights["NPC_1"]
        assert replacement is not first and replacement.client is not old_client
        assert replacement.lease.target_entity_id == 3
        assert rig.bridge.commands("body_acquire")[-1]["target_entity_id"] == 3
        assert "NPC_1" not in rig.pilot._retarget_pending
        rig.bridge.emit(multi_fighter("NPC_1", 6, owned=True, target_entity=3))
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 3)
        assert rig.bridge.commands("body_action")[-1]["entity_id"] == npc_entity("NPC_1")
        assert rig.bridge.commands("body_action")[-1]["target_entity_id"] == 3
        assert rig.pilot.active_count == len(rig.link.body_leases) == 2
        assert second.lease == second_lease and second.client is second_client
        assert len(rig.bridge.commands("body_release")) == len(rig.policy.commands("end_episode")) == 1
        rig.bridge.refuse_shrink = False


async def test_shutdown_retries_one_refused_final_unwatch_and_clears_subscription():
    async with running() as rig:
        await rig.active()
        original_send = rig.bridge.send
        empty_watches = []

        async def refuse_first_unwatch(raw):
            command = json.loads(raw)
            if command["type"] == "body_watch" and not command["npcs"]:
                empty_watches.append(command)
                if len(empty_watches) == 1:
                    rig.bridge.sent.append(command)
                    rig.bridge.sent_times.append((command, asyncio.get_running_loop().time()))
                    rig.link._dispatch(Result(
                        type="result", req=command["req"], ok=False, error="temporary watch refusal",
                    ))
                    return
            await original_send(raw)

        rig.bridge.send = refuse_first_unwatch
        await rig.pilot.aclose()
        assert len(empty_watches) == 2
        assert [command["npcs"] for command in rig.bridge.commands("body_watch")] == [["NPC"], [], []]
        assert rig.bridge._watched_npc is None
        assert rig.pilot.active_count == 0 and not rig.pilot.handles("NPC")
        assert not rig.link.body_leases and not rig.link.body_latest
        assert len(rig.bridge.commands("body_release")) == len(rig.policy.commands("end_episode")) == 1
        assert len(rig.policy.commands("close")) == 1
        summary = rig.pilot.last_summary
        assert summary.reason == "shutdown" and summary.release_confirmed and not summary.watch_removed


async def test_disconnection_during_new_acquire_finishes_every_fight_before_socket_closed(monkeypatch):
    async with running(multi=True) as rig:
        await start_fights(rig, "NPC_1")
        original_acquire = rig.link.body_acquire

        async def acquire(npc, *args, **kwargs):
            if npc == "NPC_2":
                raise GameDisconnected("transport failed before closed notification")
            return await original_acquire(npc, *args, **kwargs)

        monkeypatch.setattr(rig.link, "body_acquire", acquire)
        rig.bridge.roster(multi_entry("NPC_1", activity="body"), multi_entry("NPC_2"))
        await eventually(lambda: rig.pilot.handles("NPC_2"))
        await eventually(lambda: rig.pilot.fights["NPC_2"]._watch_ready)
        rig.bridge.emit(multi_fighter("NPC_2"))
        await eventually(lambda: rig.pilot.active_count == 0)
        assert not rig.link.closed.is_set()
        assert not rig.pilot.handles("NPC_1") and not rig.pilot.handles("NPC_2")
        assert not rig.link.body_leases and not rig.bridge.watched
        assert len(rig.pilot.summaries) == 2
        assert {summary.npc for summary in rig.pilot.summaries} == {"NPC_1", "NPC_2"}
        assert all(summary.reason == "disconnected" for summary in rig.pilot.summaries)
        assert len(rig.bridge.commands("body_release")) == len(rig.policy.commands("end_episode")) == 1
        assert len(rig.policy.connections) == 1
