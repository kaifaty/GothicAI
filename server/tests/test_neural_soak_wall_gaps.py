"""Wall-clock pauses remain visible while the game's own clock is stopped."""

from types import SimpleNamespace

import pytest

from gothic_ai.bodypilot import BodyFight, PolicyClient
from gothic_ai.protocol import (
    ActionResult,
    BodyObservations,
    Loading,
    NpcActResult,
    NpcRoster,
    Pong,
    Result,
    RoutineInfo,
    Snapshot,
)
from gothic_ai.scenarios.neural_soak_metrics import SoakMetrics


def packet(game_ms, *, entity_id=1, generation=1):
    return {"generation": generation, "observations": [{
        "session_epoch": 1, "world_epoch": generation, "entity_id": entity_id,
        "control_generation": 0, "periodic": True, "t_game_ms": game_ms,
    }]}


def test_wall_gaps_show_pauses_with_stationary_game_time_and_across_windows(tmp_path):
    now = [100.0]
    metrics = SoakMetrics(SimpleNamespace(), None, tmp_path, clock=lambda: now[0])
    metrics._packet(packet(100))
    metrics._bridge_reply(Pong(type="pong"))
    now[0] = 100.05
    metrics._packet(packet(150))
    now[0] = 100.10
    metrics._bridge_reply(Pong(type="pong"))
    now[0] = 103.50
    metrics._packet(packet(150))  # The game clock did not advance through the pause.
    now[0] = 104.0
    metrics._bridge_reply(Pong(type="pong"))
    first = metrics.sample(force=True)
    assert first["observation_wall_gap_max_ms"] == pytest.approx(3450)
    assert first["bridge_reply_wall_gap_max_ms"] == pytest.approx(3900)
    assert first["observation_wall_count"] == first["bridge_reply_wall_count"] == 3
    assert first["game_interval_max_ms"] == 50

    now[0] = 114.0
    silent = metrics.sample()
    assert silent["observation_wall_count"] == silent["bridge_reply_wall_count"] == 0
    assert silent["observation_wall_gap_max_ms"] == pytest.approx(10_500)
    assert silent["bridge_reply_wall_gap_max_ms"] == pytest.approx(10_000)
    assert silent["observation_wall_age_ms"] == pytest.approx(10_500)

    now[0] = 115.5
    metrics._packet(packet(200))
    now[0] = 116.0
    metrics._bridge_reply(Pong(type="pong"))
    recovered = metrics.sample(force=True)
    assert recovered["observation_wall_gap_max_ms"] == pytest.approx(12_000)
    assert recovered["bridge_reply_wall_gap_max_ms"] == pytest.approx(12_000)
    assert recovered["game_interval_max_ms"] == 50


@pytest.mark.parametrize("message_class", [
    Result, ActionResult, Pong, RoutineInfo, Snapshot, NpcActResult,
])
def test_bridge_reply_counts_receipts_including_rejections(tmp_path, message_class):
    now = [0.0]
    metrics = SoakMetrics(SimpleNamespace(), None, tmp_path, clock=lambda: now[0])
    metrics._bridge_reply(message_class.model_construct())
    now[0] = 2.5
    metrics._bridge_reply(Result(type="result", req="late", ok=False, error="rejected"))
    sample = metrics.sample(force=True)
    assert sample["bridge_reply_wall_count"] == 2
    assert sample["bridge_reply_wall_gap_max_ms"] == 2500


def test_roster_loading_and_body_packets_are_not_bridge_replies(tmp_path):
    metrics = SoakMetrics(SimpleNamespace(), None, tmp_path)
    for message_class in (NpcRoster, Loading, BodyObservations):
        metrics._bridge_reply(message_class.model_construct())
    metrics._packet({"observations": []})
    sample = metrics.sample(force=True)
    assert sample["bridge_reply_wall_gap_max_ms"] is None
    assert sample["observation_wall_gap_max_ms"] is None
    assert sample["bridge_reply_wall_count"] == sample["observation_wall_count"] == 0


def test_control_tick_intervals_reset_when_passive_watch_changes_generation(tmp_path):
    metrics = SoakMetrics(SimpleNamespace(), None, tmp_path)
    metrics._packet(packet(100))
    metrics._packet(packet(150))
    for game_ms in (200, 250):
        metrics._packet(packet(game_ms, entity_id=2, generation=2))
    metrics._packet(packet(90_100, generation=3))
    metrics._packet(packet(90_150, generation=3))
    sample = metrics.sample(force=True)
    assert sample["game_interval_count"] == 3
    assert sample["game_interval_max_ms"] == 50


def test_control_tick_lag_survives_partial_alternating_actor_packets(tmp_path):
    metrics = SoakMetrics(SimpleNamespace(), None, tmp_path)
    for game_ms, entity_id in ((100, 1), (100, 2), (150, 1), (150, 2), (650, 1), (200, 2)):
        metrics._packet(packet(game_ms, entity_id=entity_id))
    sample = metrics.sample(force=True)
    assert sample["game_interval_count"] == 4
    assert sample["game_interval_max_ms"] == 500


def test_control_observers_restore_listeners_on_exception_without_wrapping_pilot(tmp_path):
    now = [0.0]

    def sentinel(message):
        pass

    link = SimpleNamespace(raw_body_listeners=[], listeners=[sentinel])
    metrics = SoakMetrics(link, None, tmp_path, clock=lambda: now[0])
    original_policy, original_finish = PolicyClient.call, BodyFight._finish
    with pytest.raises(RuntimeError, match="scenario failed"), metrics.observe():
        assert PolicyClient.call is original_policy and BodyFight._finish is original_finish
        assert len(link.raw_body_listeners) == 1
        assert len(link.listeners) == 2
        for listener in link.listeners:
            listener(Pong(type="pong"))
        link.raw_body_listeners[0](packet(100))
        now[0] = 12.0
        raise RuntimeError("scenario failed")
    assert link.listeners == [sentinel] and link.raw_body_listeners == []
    assert metrics.samples[-1]["bridge_reply_wall_gap_max_ms"] == 12_000
    assert metrics.samples[-1]["observation_wall_gap_max_ms"] == 12_000


def test_control_result_has_rss_tick_pauses_and_scenario_checks_without_pilot(tmp_path, monkeypatch):
    now = [0.0]
    monkeypatch.setattr("gothic_ai.scenarios.neural_soak_metrics.rss_bytes", lambda pid: 1000)
    (tmp_path / "game.pid").write_text("123")
    (tmp_path / "server.log").write_text("INFO ready\n")
    link = SimpleNamespace(raw_body_listeners=[], listeners=[])
    metrics = SoakMetrics(link, None, tmp_path, clock=lambda: now[0])
    with metrics.observe():
        metrics._packet(packet(100))
        metrics._bridge_reply(Pong(type="pong"))
        now[0] = 1.0
        metrics._packet(packet(150))
        metrics._bridge_reply(Pong(type="pong"))
        checks = metrics.write_result(
            extra={"loads": [{"cause": "scheduled_600", "ok": True}]},
            extra_checks={"loads": "done (one load)"},
        )
    assert set(checks) == {"game_rss", "server_rss", "server_log", "loads"}
    assert all(value.startswith("done") for value in checks.values())
    assert metrics.data["mode"] == "control" and metrics.data["status"] == "done"
    assert "fights" not in metrics.data
    assert metrics.data["extra"]["loads"][0]["ok"]
    assert metrics.data["metrics"]["observation_wall_gap_max_ms"] == 1000
    assert metrics.data["metrics"]["bridge_reply_wall_gap_max_ms"] == 1000
    forbidden = {"active_leases", "lease_count", "active_count", "body_without_lease",
                 "release_orphan_timeout", "service_calls", "service_p95_ms", "service_p99_ms"}
    assert not forbidden.intersection(metrics.samples[-1])
    assert not forbidden.intersection(metrics.data["metrics"])
