"""The soak must retain evidence and reject real gate failures in short runs too."""

import asyncio
import json
from collections import deque
from dataclasses import dataclass, replace
from types import SimpleNamespace

import pytest

from gothic_ai.bodypilot import BodyFight, PolicyClient
from gothic_ai.protocol import BodyLease
from gothic_ai.scenarios.neural_load import _policy_resets
from gothic_ai.scenarios.neural_soak import Episode
from gothic_ai.scenarios.neural_soak_metrics import (
    SoakMetrics,
    game_rss_window,
    rss_window,
    scan_server_errors,
)

MIB = 1024**2


def test_rss_uses_final_twenty_minutes_and_enforces_five_percent():
    samples = [
        {"elapsed_seconds": 0, "rss": 10_000},
        {"elapsed_seconds": 600, "rss": 1000},
        {"elapsed_seconds": 1800, "rss": 1050},
    ]
    window = rss_window(samples, "rss")
    assert window["ok"] and not window["short_window"]
    assert window["start_seconds"] == 600
    samples[-1]["rss"] = 1051
    assert not rss_window(samples, "rss")["ok"]


def test_short_or_missing_rss_is_explicit():
    samples = [{"elapsed_seconds": 0, "rss": 100}, {"elapsed_seconds": 180, "rss": 106}]
    assert rss_window(samples, "rss")["short_window"]
    assert not rss_window(samples, "rss")["ok"]
    assert not rss_window(samples, "missing")["ok"]


def _load(completed_at, *, cause="scheduled_600", ok=True):
    return {"cause": cause, "ok": ok,
            "rss_before": {"elapsed_seconds": completed_at - 1},
            "rss_after": {"elapsed_seconds": completed_at}}


def test_game_rss_counts_all_completed_loads_only_between_observed_endpoints():
    samples = [
        {"elapsed_seconds": 0, "game_rss_bytes": 10_000 * MIB},
        {"elapsed_seconds": 605, "game_rss_bytes": 1000 * MIB},
        {"elapsed_seconds": 1800, "game_rss_bytes": 1300 * MIB},
    ]
    loads = [_load(at) for at in (0, 600, 605, 1800, 1800.001)]
    loads += [_load(606, cause="hero_death", ok=False),
              _load(1200, cause="companion_death", ok=False),
              {"rss_before": {"elapsed_seconds": 1500}}]

    window = game_rss_window(samples, loads)

    assert window["start_seconds"] == 605 and window["end_seconds"] == 1800
    assert window["observed_seconds"] == 1195 and not window["short_window"]
    assert window["load_count"] == 3  # All causes and failed checks still contribute RSS.
    assert window["growth_mib_per_load"] == 100 and window["ok"]


@pytest.mark.parametrize(("extra_bytes", "ok"), [(0, True), (1, False)])
def test_game_rss_native_control_tolerance_is_inclusive(extra_bytes, ok):
    # 5027 MiB / 25 loads is exactly 201.08 MiB/load before floating-point division.
    samples = [
        {"elapsed_seconds": 0, "game_rss_bytes": 10_000 * MIB},
        {"elapsed_seconds": 1200, "game_rss_bytes": 15_027 * MIB + extra_bytes},
    ]
    window = game_rss_window(samples, [_load(at) for at in range(1, 26)])
    assert window["ok"] is ok
    assert window["native_control_mib_per_load"] == 182.8
    assert window["tolerance_percent"] == 10
    assert window["limit_mib_per_load"] == 201.08
    assert window["growth_mib_per_load"] >= 201.08
    assert window["growth_percent"] > 5


@pytest.mark.parametrize("missing", [None, 0, -1, float("nan"), float("inf")])
@pytest.mark.parametrize("endpoint", [0, 1])
def test_game_rss_missing_or_invalid_endpoint_is_unassessable(missing, endpoint):
    samples = [{"elapsed_seconds": at, "game_rss_bytes": 100 * MIB} for at in (0, 1200)]
    samples[endpoint]["game_rss_bytes"] = missing
    window = game_rss_window(samples, [_load(600)])
    assert not window["ok"] and not window["short_window"]
    assert window["growth_mib_per_load"] is None
    assert window["reason"] == "missing RSS endpoints"
    json.dumps(window, allow_nan=False)


def test_game_rss_without_samples_or_completed_loads_is_explicit():
    empty = game_rss_window([], [_load(600)])
    assert not empty["ok"] and empty["growth_mib_per_load"] is None
    samples = [{"elapsed_seconds": at, "game_rss_bytes": 100 * MIB} for at in (0, 1200)]
    window = game_rss_window(samples, [_load(0), _load(1201)])
    assert not window["ok"] and not window["short_window"]
    assert window["load_count"] == 0 and window["growth_mib_per_load"] is None
    assert window["reason"] == "no completed save loads in observed RSS window"


@pytest.mark.parametrize("control", [False, True])
@pytest.mark.parametrize(("server_after", "server_ok"), [(1050, True), (1051, False)])
def test_result_uses_per_load_game_growth_and_preserves_server_five_percent(
    tmp_path, monkeypatch, control, server_after, server_ok,
):
    now = [0.0]
    # Reproduce 160.3 MiB/load at about 75% raw RSS growth: raw growth is diagnostic.
    game_before = round(1603 / .75 * MIB)
    game_after = game_before + 1603 * MIB
    monkeypatch.setattr("gothic_ai.scenarios.neural_soak_metrics.rss_bytes",
                        lambda pid: game_after if pid == 123 else server_after)
    (tmp_path / "game.pid").write_text("123")
    (tmp_path / "server.log").write_text("INFO ready\n")
    pilot = None if control else SimpleNamespace(people={}, active_count=0)
    metrics = SoakMetrics(SimpleNamespace(body_leases={}), pilot, tmp_path, clock=lambda: now[0])
    metrics.samples.append({"elapsed_seconds": 600, "game_rss_bytes": game_before,
                            "server_rss_bytes": 1000})
    loads = [_load(600), _load(1800.001)]
    loads += [_load(at, cause="hero_death" if at % 2 else "companion_death", ok=False)
              for at in (601, 700, 800, 900, 1000, 1100, 1200, 1300, 1400, 1800)]
    now[0] = 1800

    checks = metrics.write_result(extra={"loads": loads})

    assert checks["game_rss"].startswith("done")
    assert "160.3" in checks["game_rss"] and "201.08" in checks["game_rss"]
    assert "182.8" in checks["game_rss"] and "tolerance_percent=10" in checks["game_rss"]
    assert "loads=10" in checks["game_rss"] and "final 20 minutes" in checks["game_rss"]
    assert checks["server_rss"].startswith("done" if server_ok else "failed")
    saved = json.loads((tmp_path / "neural_soak_result.json").read_text())
    assert saved["mode"] == ("control" if control else "p30")
    game = saved["metrics"]["game_rss"]
    assert game["ok"] and not game["short_window"]
    assert game["growth_percent"] == pytest.approx(75)
    assert game["growth_mib_per_load"] == 160.3 and game["load_count"] == 10
    assert game["native_control_mib_per_load"] == 182.8
    assert game["limit_mib_per_load"] == 201.08 and game["tolerance_percent"] == 10
    assert game["start_seconds"] == 600 and game["end_seconds"] == 1800
    assert saved["metrics"]["server_rss"]["ok"] is server_ok


@pytest.mark.parametrize("missing", [False, True])
@pytest.mark.parametrize("short", [False, True])
def test_result_defers_only_short_windows_with_valid_rss(tmp_path, monkeypatch, missing, short):
    now = [0.0]
    monkeypatch.setattr("gothic_ai.scenarios.neural_soak_metrics.rss_bytes",
                        lambda pid: None if missing and pid == 123 else 1000)
    (tmp_path / "game.pid").write_text("123")
    (tmp_path / "server.log").write_text("INFO ready\n")
    metrics = SoakMetrics(SimpleNamespace(), None, tmp_path, clock=lambda: now[0])
    metrics.sample(force=True)
    now[0] = 180 if short else 1200

    checks = metrics.write_result()

    assert checks["game_rss"].startswith("done" if short and not missing else "failed")
    assert metrics.data["metrics"]["game_rss"]["growth_mib_per_load"] is None
    assert not metrics.data["metrics"]["game_rss"]["ok"]
    if short and not missing:
        assert "gate deferred to full run" in checks["game_rss"]
    elif not missing:
        assert "no completed save loads" in checks["game_rss"]


def test_log_errors_keep_line_references_without_private_endpoints(tmp_path):
    path = tmp_path / "server.log"
    path.write_text("INFO ready\nWARNING protocol mismatch: game 1, server 2\n"
                    "Traceback (most recent call last):\n"
                    "ERROR bridge tcp://private.invalid:123 failed\n")
    errors = scan_server_errors(path)
    assert errors == [{"line": 2, "kind": "protocol"}, {"line": 3, "kind": "traceback"},
                      {"line": 4, "kind": "bridge"}]
    assert "private" not in json.dumps(errors)


@dataclass
class Summary:
    npc: str = "BANDIT"
    decisions: int = 0
    reason: str = "target_lost"
    policy_timeouts: int = 0
    fallbacks: int = 0
    action_rejections: tuple = ()
    fight_seconds: float = 10
    lease: BodyLease | None = None
    release_confirmed: bool = True
    release_seconds: float | None = 0
    release_late_seconds: float | None = None
    end_trigger: str = "target_lost"
    accept_wait_timeouts: int = 0
    policy: str = "service"
    selected_actions: tuple = ()
    accepted_actions: tuple = ()
    policy_fallbacks: tuple = ()
    policy_memory_resets: int = 0
    engine_steps: int = 0
    engine_p99_us: float | None = None


@pytest.mark.parametrize(("accepted", "p99_us", "fallbacks", "reason", "release_s", "failed_gate"), [
    (95, 20_000, (), "target_lost", 2, None),
    (94, 20_000, (), "target_lost", 2, "accepted_commands"),
    (95, 20_001, (), "target_lost", 2, "engine_latency"),
    (95, 20_000, (("policy_error", 1),), "target_lost", 2, "fault_fallbacks"),
    (95, 20_000, (("command_busy", 5),), "target_lost", 2, None),
    (95, 20_000, (), "lease_lost", 2, "fault_fallbacks"),
    (95, 20_000, (), "target_lost", 2.001, "release_clean"),
])
def test_engine_summaries_preserve_soak_gates_and_action_diagnostics(
    tmp_path, monkeypatch, accepted, p99_us, fallbacks, reason, release_s, failed_gate,
):
    now = [0.0]
    monkeypatch.setattr("gothic_ai.scenarios.neural_soak_metrics.rss_bytes", lambda pid: 1000)
    (tmp_path / "game.pid").write_text("123")
    (tmp_path / "server.log").write_text("INFO ready\n")
    pilot = SimpleNamespace(people={}, active_count=0, policy="engine")
    metrics = SoakMetrics(SimpleNamespace(body_leases={}), pilot, tmp_path, clock=lambda: now[0])
    metrics.sample(force=True)
    lease = BodyLease(
        npc="BANDIT", provider="neural-body", mode="realtime", session_epoch=1,
        world_epoch=1, entity_id=1, control_generation=1, goal_version=1,
        target_entity_id=2, acquired_observation_seq=1,
    )
    summary = Summary(
        policy="engine", decisions=accepted, reason=reason, lease=lease, release_seconds=release_s,
        selected_actions=(("continue", 70), ("wait_200", 30)),
        accepted_actions=(("continue", accepted - 30), ("wait_200", 30)),
        policy_memory_resets=1, policy_fallbacks=fallbacks,
        fallbacks=sum(count for reason, count in fallbacks if reason == "policy_error"),
        engine_steps=100, engine_p99_us=p99_us,
    )
    metrics._completed(None, summary, finish_at=0)
    now[0] = 180

    checks = metrics.write_result(eligible=100, captured=95)

    assert {name for name, value in checks.items() if value.startswith("failed")} == (
        {failed_gate} if failed_gate else set()
    )
    assert "service_latency" not in checks
    assert metrics.data["metrics"]["policy_decisions"] == 100
    assert metrics.data["metrics"]["accepted_commands"] == accepted
    record = metrics.fight_records[0]
    assert record["actions"] == {"0": 70, "10": 30}
    assert record["wait_200_fraction"] == .3
    assert record["accepted_fraction"] == accepted / 100
    assert record["accepted_actions"] == {"continue": accepted - 30, "wait_200": 30}
    assert record["accepted_action_fractions"]["continue"] == (accepted - 30) / 70
    assert record["summary"]["policy_memory_resets"] == 1
    assert record["service_calls"] == 0


async def test_observers_chain_and_retain_fights_beyond_the_pilot_deque(tmp_path, monkeypatch):
    now = [0.0]
    history = deque(maxlen=32)
    completions = []

    def completed(fight, summary, finish_at, release_deadline):
        history.append(summary)
        completions.append((finish_at, release_deadline))

    pilot = SimpleNamespace(
        people={}, active_count=0, _completed=completed, summaries=history,
    )
    link = SimpleNamespace(body_leases={}, raw_body_listeners=[])
    monkeypatch.setattr("gothic_ai.scenarios.neural_soak_metrics.rss_bytes", lambda pid: 1000)
    (tmp_path / "game.pid").write_text("123")
    (tmp_path / "server.log").write_text("INFO ready\n")

    async def policy_call(client, command, **fields):
        now[0] += 0.001
        return {"ok": True, "status": "decision", "action_id": 10}

    monkeypatch.setattr(PolicyClient, "call", policy_call)
    client = PolicyClient("tcp://unused.invalid:123")
    metrics = SoakMetrics(link, pilot, tmp_path, clock=lambda: now[0])
    with metrics.observe():
        with _policy_resets() as resets:
            await client.call("reset", seed=1)
            assert resets[client] == [{"seed": 1}]
            for index in range(40):
                client = PolicyClient("tcp://unused.invalid:123")
                lease = BodyLease(
                    npc="BANDIT", provider="neural-body", mode="realtime", session_epoch=1,
                    world_epoch=1, entity_id=1, control_generation=index + 1, goal_version=1,
                    target_entity_id=2, acquired_observation_seq=1,
                )
                scope = lease.model_dump(include={
                    "session_epoch", "world_epoch", "entity_id", "control_generation",
                })
                await client.call("step", observation=scope)
                # _finish clears these fields before it publishes the summary.
                pilot._completed(SimpleNamespace(client=None, lease=None), Summary(
                    decisions=int(index < 37), lease=lease,
                ), now[0], now[0] + 2)
    checks = metrics.write_result(eligible=40, captured=38, release_issues=[])
    assert len(history) == 32 and len(metrics.summaries) == 40
    assert len(completions) == 40
    assert all(deadline - finish_at == pytest.approx(2) for finish_at, deadline in completions)
    assert checks["capture_rate"].startswith("done")
    assert checks["accepted_commands"].startswith("failed")  # 37/40, not 37/(37+0).
    assert metrics.data["metrics"]["policy_decisions"] == 40
    assert metrics.data["metrics"]["accepted_commands"] == 37
    assert all(record["policy_decisions"] == 1 for record in metrics.fight_records)
    assert all(record["service_calls"] == 1 for record in metrics.fight_records)
    assert all(record["wait_200_fraction"] == 1 for record in metrics.fight_records)
    assert PolicyClient.call is policy_call
    assert not link.raw_body_listeners
    lines = [json.loads(line) for line in (tmp_path / "neural_soak.jsonl").read_text().splitlines()]
    assert sum(line["kind"] == "fight_end" for line in lines) == 40
    assert metrics.data["metrics"]["game_rss"]["short_window"]


@pytest.mark.parametrize("reason", [
    "finish_deadline", "bridge_error", "symbol_eval_timeout", "body_watch_failed", "lease_lost",
    "acceptance_timeout",
])
def test_fault_endings_fail_even_without_policy_fallback(tmp_path, reason):
    link = SimpleNamespace(body_leases={})
    pilot = SimpleNamespace(people={}, active_count=0)
    metrics = SoakMetrics(link, pilot, tmp_path)
    metrics.summaries.append(Summary(reason=reason))
    checks = metrics.write_result(eligible=1, captured=1, release_issues=[])
    assert checks["fault_fallbacks"].startswith("failed")
    assert metrics.data["metrics"]["fault_endings"] == {reason: 1}


@pytest.mark.parametrize(("reason", "timeouts", "fault"), [
    ("acceptance_timeout", 4, True),
    ("target_lost", 1, False),
])
def test_missing_acceptance_fails_despite_receipts_but_recovered_wait_is_tolerated(
    tmp_path, monkeypatch, reason, timeouts, fault,
):
    monkeypatch.setattr("gothic_ai.scenarios.neural_soak_metrics.rss_bytes", lambda pid: 1000)
    (tmp_path / "game.pid").write_text("123")
    (tmp_path / "server.log").write_text("INFO ready\n")
    metrics = SoakMetrics(
        SimpleNamespace(body_leases={}), SimpleNamespace(people={}, active_count=0), tmp_path,
    )
    lease = BodyLease(
        npc="BANDIT", provider="neural-body", mode="realtime", session_epoch=1,
        world_epoch=1, entity_id=1, control_generation=1, goal_version=1,
        target_entity_id=2, acquired_observation_seq=1,
    )
    metrics.summaries.append(Summary(
        decisions=4, reason=reason, end_trigger=reason, lease=lease,
        release_seconds=0.1, accept_wait_timeouts=timeouts,
    ))
    metrics.actions_by_client[PolicyClient("tcp://unused.invalid:123")][10] = 4
    metrics._latencies.extend([1.0] * 4)

    checks = metrics.write_result(eligible=1, captured=1)

    assert all(value.startswith("done") for name, value in checks.items()
               if name != "fault_fallbacks")
    assert checks["accepted_commands"] == "done (accepted=4/4, minimum=95%)"
    assert checks["fault_fallbacks"].startswith("failed" if fault else "done")
    assert metrics.data["status"] == ("failed" if fault else "done")
    assert metrics.data["metrics"]["fault_endings"] == ({reason: 1} if fault else {})


def test_late_native_confirmation_does_not_hide_acceptance_timeout(tmp_path):
    metrics = SoakMetrics(
        SimpleNamespace(body_leases={}), SimpleNamespace(people={}, active_count=0), tmp_path,
    )
    metrics.summaries.append(Summary(
        reason="finish_deadline", end_trigger="acceptance_timeout",
        release_confirmed=True, release_late_seconds=2.75, accept_wait_timeouts=4,
    ))

    checks = metrics.write_result(eligible=1, captured=1)

    assert checks["fault_fallbacks"].startswith("failed")
    assert metrics.data["metrics"]["fault_endings"] == {"acceptance_timeout": 1}


def test_late_release_updates_retained_snapshots_before_history_eviction(tmp_path):
    lease = BodyLease(
        npc="BANDIT", provider="neural-body", mode="realtime", session_epoch=1,
        world_epoch=1, entity_id=1, control_generation=1, goal_version=1,
        target_entity_id=2, acquired_observation_seq=1,
    )
    pending = Summary(reason="finish_deadline", release_confirmed=False, lease=lease)
    history = deque(maxlen=1)
    pilot = SimpleNamespace(
        people={}, active_count=0, summaries=history, release_orphan_timeout=0,
        _completed=lambda fight, summary, *args: history.append(summary),
    )
    metrics = SoakMetrics(
        SimpleNamespace(body_leases={}, raw_body_listeners=[]), pilot, tmp_path,
    )
    with metrics.observe():
        pilot._completed(None, pending, 1.0, 3.0)
        # PR253 replaces the frozen summary instead of mutating the first snapshot.
        confirmed = replace(pending, release_confirmed=True, release_late_seconds=2.75)
        history[0] = confirmed
        pilot._completed(None, Summary(), 4.0, 6.0)
        assert len(history) == 1 and history[0].lease is None
        assert metrics.summaries[0] is confirmed
        assert metrics.fight_records[0]["summary"]["release_confirmed"]
        assert metrics.fight_records[0]["summary"]["release_late_seconds"] == 2.75

    checks = metrics.write_result(eligible=1, captured=1, release_issues=[])
    assert checks["fault_fallbacks"].startswith("done")
    assert checks["release_clean"].startswith("failed")
    assert metrics.data["metrics"]["release_deadline_issues"][0]["release_late_seconds"] == 2.75
    assert metrics.data["metrics"]["finish_deadline_count"] == 1
    assert metrics.data["metrics"]["finish_deadline_unconfirmed"] == 0
    assert metrics.data["metrics"]["release_late_count"] == 1
    assert metrics.data["metrics"]["release_late_max_seconds"] == 2.75
    updates = [json.loads(line) for line in (tmp_path / "neural_soak.jsonl").read_text().splitlines()
               if json.loads(line)["kind"] == "release_update"]
    assert len(updates) == 1
    assert updates[0]["summary"]["end_trigger"] == "target_lost"
    assert updates[0]["summary"]["release_late_seconds"] == 2.75


def test_sample_and_result_sync_late_release_even_without_another_completion(tmp_path):
    lease = BodyLease(
        npc="BANDIT", provider="neural-body", mode="realtime", session_epoch=1,
        world_epoch=1, entity_id=1, control_generation=1, goal_version=1,
        target_entity_id=2, acquired_observation_seq=1,
    )
    pending = Summary(reason="finish_deadline", release_confirmed=False, lease=lease)
    history = deque([pending])
    pilot = SimpleNamespace(people={}, active_count=0, summaries=history, release_orphan_timeout=0)
    metrics = SoakMetrics(SimpleNamespace(body_leases={}), pilot, tmp_path)
    metrics._completed(None, pending)
    metrics.sample(force=True)
    history[0] = replace(pending, release_confirmed=True, release_late_seconds=4.0)
    metrics.sample()  # Sync runs even if the ten-second sample interval has not elapsed.
    assert metrics.summaries[0] is history[0]
    assert metrics.fight_records[0]["summary"]["release_late_seconds"] == 4.0
    history[0] = replace(history[0], release_late_seconds=4.25)
    metrics.write_result(eligible=1, captured=1, release_issues=[])
    saved = json.loads((tmp_path / "neural_soak_result.json").read_text())
    assert saved["fights"][0]["summary"]["release_late_seconds"] == 4.25
    assert saved["metrics"]["release_late_max_seconds"] == 4.25


def test_orphan_release_timeout_fails_zero_fault_gate_and_is_sampled(tmp_path):
    pilot = SimpleNamespace(people={}, active_count=0, release_orphan_timeout=1)
    metrics = SoakMetrics(SimpleNamespace(body_leases={}), pilot, tmp_path)
    sample = metrics.sample(force=True)
    assert sample["release_orphan_timeout"] == 1
    checks = metrics.write_result(eligible=1, captured=1, release_issues=[])
    assert checks["fault_fallbacks"].startswith("failed")
    assert "release_orphan_timeout=1" in checks["fault_fallbacks"]
    assert metrics.data["metrics"]["release_orphan_timeout"] == 1


def test_extra_check_failure_is_in_the_only_final_result(tmp_path, monkeypatch):
    link = SimpleNamespace(body_leases={})
    pilot = SimpleNamespace(people={}, active_count=0)
    monkeypatch.setattr("gothic_ai.scenarios.neural_soak_metrics.rss_bytes", lambda pid: 1000)
    (tmp_path / "game.pid").write_text("123")
    (tmp_path / "server.log").write_text("INFO ready\n")
    metrics = SoakMetrics(link, pilot, tmp_path)
    metrics.summaries.append(Summary(decisions=1))
    client = PolicyClient("tcp://unused.invalid:123")
    metrics.actions_by_client[client][10] = 1
    metrics._latencies.append(1.0)
    load_check = "failed (old lease survived loading)"

    checks = metrics.write_result(
        eligible=1, captured=1, release_issues=[], extra_checks={"load": load_check},
    )

    assert all(value.startswith("done") for name, value in checks.items() if name != "load")
    assert checks["load"] == load_check
    assert metrics.data["status"] == "failed"
    saved = json.loads((tmp_path / "neural_soak_result.json").read_text())
    assert saved["status"] == "failed" and saved["checks"]["load"] == load_check
    lines = [json.loads(line) for line in (tmp_path / "neural_soak.jsonl").read_text().splitlines()]
    results = [line for line in lines if line["kind"] == "result"]
    assert len(results) == 1
    assert results[0]["status"] == "failed" and results[0]["checks"]["load"] == load_check


def test_game_intervals_ignore_unleased_resubscription_gaps(tmp_path):
    link = SimpleNamespace(body_leases={})
    pilot = SimpleNamespace(people={}, active_count=0)
    metrics = SoakMetrics(link, pilot, tmp_path)

    def row(game_ms, generation):
        return {
            "session_epoch": 1, "world_epoch": 1, "entity_id": 7,
            "control_generation": generation, "periodic": True, "t_game_ms": game_ms,
        }

    metrics._packet({"observations": [row(100, 0)]})
    metrics._packet({"observations": [row(200, 1), row(250, 1)]})
    # The same unleased actor reappears in a later wave. This 90-second
    # subscription gap must not be reported as a stalled leased observation.
    metrics._packet({"observations": [row(90_100, 0)]})
    metrics._packet({"observations": [row(90_200, 2), row(90_250, 2)]})

    sample = metrics.sample(force=True)
    assert sample["game_interval_count"] == 2
    assert sample["game_interval_p95_ms"] == 50
    assert sample["game_interval_max_ms"] == 50


@pytest.mark.parametrize(("release_seconds", "late_seconds", "confirmed", "clean"), [
    (2.0, None, True, True),
    (2.75, None, True, False),
    (None, 2.0, True, True),
    (1.0, 2.75, True, False),
    (2.75, 1.0, True, False),
    (None, None, True, False),
    (.5, None, False, False),
])
def test_release_gate_enforces_elapsed_time_without_external_issues(
    tmp_path, release_seconds, late_seconds, confirmed, clean,
):
    lease = BodyLease(
        npc="BANDIT", provider="neural-body", mode="realtime", session_epoch=1,
        world_epoch=1, entity_id=1, control_generation=1, goal_version=1,
        target_entity_id=2, acquired_observation_seq=1,
    )
    metrics = SoakMetrics(
        SimpleNamespace(body_leases={}), SimpleNamespace(people={}, active_count=0), tmp_path,
    )
    metrics.summaries.append(Summary(
        lease=lease, release_confirmed=confirmed, release_seconds=release_seconds,
        release_late_seconds=late_seconds,
    ))
    checks = metrics.write_result(eligible=1, captured=1, release_issues=[])
    assert checks["release_clean"].startswith("done" if clean else "failed")
    assert len(metrics.data["metrics"]["release_deadline_issues"]) == (0 if clean else 1)


@pytest.mark.parametrize(("diagnostics_delay", "release_delay", "terminal_lead", "clean"), [
    (0, 2.75, 0, False),
    (.75, 1.5, 0, False),
    (0, 1.75, .5, False),
    (0, 2.0, 0, True),
])
async def test_escape_deadline_starts_before_cleanup_removes_the_fight(
    tmp_path, monkeypatch, diagnostics_delay, release_delay, terminal_lead, clean,
):
    now = [10.0]
    lease = BodyLease(
        npc="BANDIT", provider="neural-body", mode="realtime", session_epoch=1,
        world_epoch=1, entity_id=1, control_generation=1, goal_version=1,
        target_entity_id=2, acquired_observation_seq=1,
    )
    fight = SimpleNamespace(lease=lease)
    link = SimpleNamespace(body_leases={1: lease}, raw_body_listeners=[])
    history = deque()
    pilot = SimpleNamespace(
        people={}, active_count=1, fights={"BANDIT": fight}, summaries=history,
        _completed=lambda fight, summary, *args: history.append(summary),
    )
    entered, proceed = asyncio.Event(), asyncio.Event()

    async def delayed_finish(fight, reason):
        entered.set()
        await proceed.wait()
        # Native confirmation uses the pilot's own release start, which can
        # follow asynchronous terminal diagnostics. The fight still exists.
        finish_at = 10 + diagnostics_delay
        now[0] = finish_at + release_delay
        link.body_leases.clear()
        pilot.fights.clear()
        pilot.active_count = 0
        pilot._completed(fight, Summary(
            reason=reason, lease=lease, release_seconds=release_delay,
        ), finish_at, finish_at + 10)

    monkeypatch.setattr(BodyFight, "_finish", delayed_finish)
    metrics = SoakMetrics(link, pilot, tmp_path, clock=lambda: now[0])
    episode = Episode(fight, "BANDIT", lease, "hero", 1, now[0], now[0])
    with metrics.observe():
        if terminal_lead:
            # A fallen actor can be observed before the pilot begins finishing.
            metrics.note_fight_end(lease, 10 - terminal_lead)
        task = asyncio.create_task(BodyFight._finish(fight, "target_lost"))
        await entered.wait()
        now[0] = 12
        episode.update_end(
            current=True, finish_started=metrics.finish_started_at(lease), now=now[0],
        )
        assert episode.ended == 10 - terminal_lead and pilot.fights["BANDIT"] is fight
        proceed.set()
        await task
        episode.update_end(
            current=False, finish_started=metrics.finish_started_at(lease), now=now[0],
        )
        assert episode.ended == 10 - terminal_lead  # Cleanup cannot restart the grace period.
        assert metrics.release_elapsed_seconds(history[0]) == pytest.approx(
            terminal_lead + diagnostics_delay + release_delay,
        )
        checks = metrics.write_result(eligible=1, captured=1, release_issues=[])
        assert checks["release_clean"].startswith("done" if clean else "failed")
    assert BodyFight._finish is delayed_finish
