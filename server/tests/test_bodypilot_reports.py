import asyncio
from dataclasses import FrozenInstanceError

import pytest

from gothic_ai import bodypilot
from tests.bodypilot_helpers import (
    eventually,
    fighter,
    multi_entry,
    multi_fighter,
    running,
    start_fights,
)


async def test_completed_fight_summary_survives_counter_reset_and_the_next_fight():
    async with running() as rig:
        assert rig.pilot.last_summary is None
        await rig.active()
        lease = rig.fight.lease
        for seq in (4, 5):
            rig.bridge.emit(fighter(seq, owned=True))
            await eventually(lambda seq=seq: len(rig.bridge.commands("body_action")) == seq - 2)
        rig.bridge.emit(fighter(6, owned=True, can_decide=False))
        await eventually(lambda: len(rig.fight._latencies) == 4)
        latencies = sorted(rig.fight._latencies)
        rig.clock.advance(0.75)
        row = fighter(7, owned=True)
        row["self"]["down"] = True
        rig.bridge.emit(row)
        await eventually(lambda: rig.pilot.last_summary is not None)

        summary = rig.pilot.last_summary
        assert summary.npc == "NPC" and summary.lease == lease
        assert summary.reason == "npc_down"
        assert summary.decisions == 3 and summary.fallbacks == 0
        assert summary.selected_actions == summary.accepted_actions == (("approach", 3),)
        assert summary.service_p50_ms == latencies[1]
        assert summary.service_p95_ms == latencies[-1]
        assert summary.service_p99_ms == latencies[-1]
        assert summary.acquire_refusals == summary.action_rejections == ()
        assert summary.transient_rejections_consecutive == 0
        assert summary.fight_seconds == pytest.approx(0.75)
        assert summary.release_confirmed
        assert 0 <= summary.release_seconds < bodypilot.FINISH_TIMEOUT
        assert summary.watch_removed
        assert rig.fight.npc is rig.fight.lease is rig.fight.client is None
        assert not rig.link.body_leases and rig.bridge._watched_npc is None
        assert not rig.fight._latencies and rig.fight._decisions == rig.fight._fallbacks == 0
        assert not rig.fight._selected_actions and not rig.fight._accepted_actions
        with pytest.raises(FrozenInstanceError):
            summary.decisions = 100

        rig.roster(activity="routine", state="ZS_SIT_BENCH")
        await rig.watch()
        assert rig.pilot.last_summary is summary
        assert summary.decisions == 3


@pytest.mark.parametrize(("latencies", "p95", "p99"), [
    ([], None, None),
    ([7.5], 7.5, 7.5),
    (list(range(100, 0, -1)), 95, 99),
])
async def test_completed_fight_service_tail_percentiles(latencies, p95, p99):
    async with running() as rig:
        await rig.active()
        rig.fight._latencies[:] = latencies
        row = fighter(4, owned=True)
        row["self"]["down"] = True
        rig.bridge.emit(row)
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        assert summary.service_p95_ms == p95
        assert summary.service_p99_ms == p99
        assert not rig.fight._latencies


async def test_latency_breakdown_ends_at_action_send_before_bridge_ack(monkeypatch, caplog):
    caplog.set_level("INFO", logger="gothic_ai.bodypilot")
    async with running() as rig:
        await rig.acquire()
        await eventually(lambda: rig.policy.commands("reset"))
        action = rig.link.body_action

        async def delayed_ack(*args, **kwargs):
            result = await action(*args, **kwargs)
            rig.clock.advance(0.050)
            return result

        monkeypatch.setattr(rig.link, "body_action", delayed_ack)
        rig.policy.on_step = lambda _: rig.clock.advance(0.010)
        rig.bridge.emit(fighter(3, owned=True))
        rig.clock.advance(0.005)  # Packet waiting in the pilot queue before step.
        await eventually(lambda: rig.fight._decisions == 1)
        row = fighter(4, owned=True)
        row["self"]["down"] = True
        rig.bridge.emit(row)
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        for percentile in ("p50", "p95"):
            assert getattr(summary, f"service_{percentile}_ms") == pytest.approx(10)
            assert getattr(summary, f"packet_to_request_{percentile}_ms") == pytest.approx(5)
            assert getattr(summary, f"packet_to_response_{percentile}_ms") == pytest.approx(15)
            assert getattr(summary, f"response_to_action_{percentile}_ms") == pytest.approx(0)
            assert getattr(summary, f"packet_to_action_{percentile}_ms") == pytest.approx(15)
        assert summary.fight_seconds == pytest.approx(0.065)
        assert "packet_to_action_p95_ms=" in caplog.text
        assert not rig.fight._packet_to_request and not rig.fight._packet_to_response
        assert not rig.fight._response_to_action and not rig.fight._packet_to_action


async def test_completed_summary_history_keeps_only_last_thirty_two_fights():
    async with running(multi=True) as rig:
        completed = []
        for index in range(34):
            rig.bridge.roster(multi_entry("NPC_1"))
            await eventually(lambda: rig.pilot.handles("NPC_1"))
            await eventually(lambda: rig.pilot.fights["NPC_1"]._watch_ready)
            seq = 2 + index * 2
            rig.bridge.emit(multi_fighter("NPC_1", seq))
            await eventually(lambda index=index: len(rig.policy.commands("reset")) == index + 1)
            down = multi_fighter("NPC_1", seq + 1, owned=True)
            down["self"]["down"] = True
            rig.bridge.emit(down)
            await eventually(lambda: rig.pilot.active_count == 0)
            completed.append(rig.pilot.last_summary)
            assert len(rig.pilot.summaries) == min(index + 1, 32)
            assert completed[-1].reason == "npc_down"
            rig.bridge.roster(multi_entry("NPC_1", activity="routine").model_copy(
                update={"state": "ZS_SIT_BENCH"},
            ))
        assert len(rig.policy.commands("reset")) == len(rig.policy.commands("end_episode")) == 34
        assert list(rig.pilot.summaries) == completed[-32:]
        assert rig.pilot.last_summary is completed[-1]


async def test_forty_watch_busy_refusals_do_not_replace_completed_fight_history():
    async with running(multi=True, protocol=43) as rig:
        rig.bridge.legacy_watch = True
        await start_fights(rig, "NPC_1")
        down = multi_fighter("NPC_1", 4, owned=True)
        down["self"]["down"] = True
        rig.bridge.emit(down)
        await eventually(lambda: rig.pilot.active_count == 0)
        completed = rig.pilot.last_summary
        assert completed.npc == "NPC_1" and completed.reason == "npc_down"
        assert completed.lease is not None and completed.acquire_completed == 1
        assert completed.release_confirmed and completed.watch_removed
        assert list(rig.pilot.summaries) == [completed] and rig.pilot.watch_busy == 0
        rig.bridge.roster(multi_entry("NPC_2"))
        await eventually(lambda: rig.pilot.handles("NPC_2"))
        await eventually(lambda: rig.pilot.fights["NPC_2"]._watch_ready)
        rig.bridge.emit(multi_fighter("NPC_2", 5))
        await eventually(lambda: len(rig.policy.commands("reset")) == 2)
        rig.bridge.emit(multi_fighter("NPC_2", 6, owned=True))
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 2)
        active = rig.pilot.fights["NPC_2"]
        lease, client = active.lease, active.client
        before = {kind: len(rig.bridge.commands(kind)) for kind in (
            "story_eval", "body_watch", "body_acquire", "body_release",
        )}
        for index in range(40):
            candidate = f"NPC_{index + 3}"
            rig.bridge.roster(multi_entry("NPC_2", activity="body"), multi_entry(candidate))
            assert rig.pilot.watch_busy == index + 1
            assert not rig.pilot.handles(candidate) and rig.pilot.active_count == 1
            await asyncio.sleep(0)  # A rejected candidate must not publish a summary in a queued task.
            assert rig.pilot.last_summary is completed
            assert list(rig.pilot.summaries) == [completed]
            assert rig.pilot.fights["NPC_2"] is active
            assert active.lease == lease and active.client is client
            assert all(len(rig.bridge.commands(kind)) == count for kind, count in before.items())
        assert rig.pilot.watch_busy == 40
        assert len(rig.policy.commands("reset")) == len(rig.policy.connections) == 2
        assert len(rig.policy.commands("end_episode")) == 1
        assert rig.bridge.watched == ["NPC_2"] and rig.link.body_leases == {lease.entity_id: lease}
