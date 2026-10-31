"""Scenario-local P30 soak observations; no changes to policy or pilot behavior."""

from __future__ import annotations

import json
import math
import os
import re
import time
from collections import Counter, defaultdict
from collections.abc import Callable, Iterator
from contextlib import contextmanager
from dataclasses import asdict
from pathlib import Path
from typing import Any

from ..bodypilot import (
    BODY_ACTIONS,
    TRANSIENT_ACTION_REJECTIONS,
    BodyFight,
    BodyPilot,
    FightSummary,
    PolicyClient,
)
from ..game import GameLink
from ..protocol import (
    ActionResult,
    BodyLease,
    GameMessage,
    NpcActResult,
    Pong,
    Result,
    RoutineInfo,
    Snapshot,
)

WAIT_200 = 10
# Native soak2 control, final 20 minutes; user decision 2026-10-19.
# Source: reports/npc-body/2026-10-19-p30-group-soak/README.md.
GAME_RSS_CONTROL_MIB_PER_LOAD = 182.8
GAME_RSS_TOLERANCE_PERCENT = 10
GAME_RSS_LIMIT_MIB_PER_LOAD = round(
    GAME_RSS_CONTROL_MIB_PER_LOAD * (1 + GAME_RSS_TOLERANCE_PERCENT / 100), 2,
)
FAULT_ENDINGS = frozenset({
    "policy_error", "policy_timeout", "body_action_rejected", "lease_lost",
    "observation_overflow", "pilot_error", "acquire_uncertain", "watch_refused",
    "bridge_error", "symbol_eval_timeout", "symbol_eval_failed",
    "body_watch_timeout", "body_watch_failed", "disconnected", "acceptance_timeout",
})


def _release_delay(summary: FightSummary) -> float | None:
    delays = [value for value in (summary.release_seconds, summary.release_late_seconds)
              if value is not None]
    return max(delays) if delays and min(delays) >= 0 else None


def release_within_deadline(summary: FightSummary, limit: float = 2) -> bool:
    """A late native confirmation is not a fallback, but still has a release deadline."""
    delay = _release_delay(summary)
    return summary.release_confirmed and delay is not None and delay <= limit


def percentile(values: list[float], fraction: float) -> float | None:
    """Use the pilot's nearest-rank definition, including the slowest tail sample."""
    return sorted(values)[math.ceil(len(values) * fraction) - 1] if values else None


def rss_bytes(pid: int) -> int | None:
    """Resident pages in Linux procfs; do not start an extra monitoring process."""
    try:
        return int(Path(f"/proc/{pid}/statm").read_text().split()[1]) * os.sysconf("SC_PAGE_SIZE")
    except (OSError, ValueError, IndexError):
        return None


def rss_window(samples: list[dict[str, Any]], key: str, seconds: float = 1200) -> dict[str, Any]:
    """Compare observed endpoints of the final 20 minutes, or all of a short smoke run."""
    if not samples:
        return {"ok": False, "reason": "no samples", "short_window": True}
    end = samples[-1]["elapsed_seconds"]
    selected = [sample for sample in samples if sample["elapsed_seconds"] >= end - seconds]
    first, last = selected[0], selected[-1]
    before, after = first.get(key), last.get(key)
    before, after = (
        value if isinstance(value, (int, float)) and math.isfinite(value) and value > 0 else None
        for value in (before, after)
    )
    growth = (after / before - 1) * 100 if before is not None and after is not None else None
    observed = last["elapsed_seconds"] - first["elapsed_seconds"]
    return {
        "ok": growth is not None and after * 100 <= before * 105,
        "start_seconds": first["elapsed_seconds"], "end_seconds": last["elapsed_seconds"],
        "observed_seconds": observed, "short_window": end < seconds,
        "start_bytes": before, "end_bytes": after, "growth_percent": growth,
    }


def game_rss_window(
    samples: list[dict[str, Any]], loads: list[dict[str, Any]], seconds: float = 1200,
) -> dict[str, Any]:
    """Compare growth per completed save load with the accepted native control.

    A load's after-RSS sample marks its completion: a load already present in
    the first RSS endpoint is excluded, and one at the last endpoint is included.
    Check failures and load causes do not change this memory denominator.
    """
    window = rss_window(samples, "game_rss_bytes", seconds)
    start, end = window.get("start_seconds"), window.get("end_seconds")
    load_count = 0
    if start is not None and end is not None:
        for load in loads:
            completed_at = (load.get("rss_after") or {}).get("elapsed_seconds")
            if completed_at is not None and start < completed_at <= end:
                load_count += 1
    before, after = window.get("start_bytes"), window.get("end_bytes")
    growth_mib = (after - before) / 1024**2 if window.get("growth_percent") is not None else None
    per_load = growth_mib / load_count if growth_mib is not None and load_count else None
    window.update({
        "ok": per_load is not None and per_load <= GAME_RSS_LIMIT_MIB_PER_LOAD,
        "growth_mib": growth_mib, "growth_mib_per_load": per_load, "load_count": load_count,
        "native_control_mib_per_load": GAME_RSS_CONTROL_MIB_PER_LOAD,
        "tolerance_percent": GAME_RSS_TOLERANCE_PERCENT,
        "limit_mib_per_load": GAME_RSS_LIMIT_MIB_PER_LOAD,
    })
    if growth_mib is None:
        window["reason"] = "missing RSS endpoints"
    elif not load_count:
        window["reason"] = "no completed save loads in observed RSS window"
    return window


def scan_server_errors(path: Path) -> list[dict[str, Any]]:
    """Keep line references, avoiding private endpoint text from exception messages."""
    if not path.is_file():
        return [{"line": None, "kind": "server_log_missing"}]
    patterns = {
        "traceback": re.compile(r"Traceback \(most recent call last\)"),
        "protocol": re.compile(r"protocol mismatch|unparsed message from game"),
        "bridge": re.compile(
            r"body packet changes watched actors|neural-body (?:watch setup|ownership poll) failed"
            r"|game session failed|(?:ERROR|CRITICAL).*?(?:bridge|gothic_ai\.game|body command)",
            re.IGNORECASE,
        ),
    }
    found = []
    for line_number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        for kind, pattern in patterns.items():
            if pattern.search(line):
                found.append({"line": line_number, "kind": kind})
    return found


class SoakMetrics:
    """Use ``observe()`` around the scenario and call ``sample()`` on each polling tick.

    ``PolicyClient.call`` is wrapped by delegation, so nested ``_policy_resets``
    observations retain their behavior. Summary interception runs before the pilot's
    bounded deque can discard a completed fight. The finish observer timestamps the
    end trigger before asynchronous cleanup. All wrappers are removed on exit.
    """

    def __init__(
        self, link: GameLink, pilot: BodyPilot | None, log_dir: Path, *,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.link, self.pilot, self.log_dir, self.clock = link, pilot, Path(log_dir), clock
        self.log_dir.mkdir(parents=True, exist_ok=True)
        self.started = clock()
        self._last_sample = float("-inf")
        self.summaries: list[FightSummary] = []
        self.fight_records: list[dict[str, Any]] = []
        self._summary_indices: dict[tuple[int, ...], int] = {}
        self._finish_started: dict[tuple[int, ...], float] = {}
        self._release_started: dict[tuple[int, ...], float] = {}
        self.samples: list[dict[str, Any]] = []
        self.actions_by_client: dict[PolicyClient, Counter[int]] = defaultdict(Counter)
        self.latencies_by_client: dict[PolicyClient, list[float]] = defaultdict(list)
        self.fallbacks_by_client: dict[PolicyClient, Counter[str]] = defaultdict(Counter)
        self.policy_errors: Counter[str] = Counter()
        self._clients_by_scope: dict[tuple[int, ...], PolicyClient] = {}
        self._service_window: list[float] = []
        self._latencies: list[float] = []
        self._intervals: list[float] = []
        self._previous_game: dict[tuple[int, ...], int] = {}
        self._max_batch_rows = self._max_batch_lag_game_ms = 0
        self._max_batch_rows_all = self._max_batch_lag_all = 0
        self._wall_previous: dict[str, float] = {}
        self._wall_counts: Counter[str] = Counter()
        self._wall_max: dict[str, float] = {}
        self._wall_max_all: dict[str, float] = {}
        self._stream: Any = None
        self.data: dict[str, Any] = {}

    def event(self, kind: str, **data: Any) -> None:
        """Write a wave, load, stall or other scenario event into the same timeline."""
        row = {"kind": kind, "elapsed_seconds": round(self.clock() - self.started, 3), **data}
        encoded = json.dumps(row, ensure_ascii=False, allow_nan=False, default=self._json_default) + "\n"
        if self._stream is None:
            with (self.log_dir / "neural_soak.jsonl").open("a") as stream:
                stream.write(encoded)
        else:
            self._stream.write(encoded)
            self._stream.flush()

    @staticmethod
    def _json_default(value: Any) -> Any:
        if hasattr(value, "model_dump"):
            return value.model_dump(mode="json")
        raise TypeError(f"cannot serialize {type(value).__name__}")

    def _packet(self, packet: dict[str, Any]) -> None:
        rows = packet["observations"]
        if not rows:
            return
        self._wall_received("observation")
        size = len(rows)
        latest = max(row["t_game_ms"] for row in rows)
        lag = latest - min(row["t_game_ms"] for row in rows)
        self._max_batch_rows = max(self._max_batch_rows, size)
        self._max_batch_lag_game_ms = max(self._max_batch_lag_game_ms, lag)
        self._max_batch_rows_all = max(self._max_batch_rows_all, size)
        self._max_batch_lag_all = max(self._max_batch_lag_all, lag)
        for row in rows:
            # Packets can contain only part of the watch. A passive watch change
            # increments world_epoch, so its new scope already resets continuity.
            identity = tuple(row[name] for name in (
                "session_epoch", "world_epoch", "entity_id", "control_generation",
            ))
            previous = self._previous_game.get(identity)
            current = row["t_game_ms"]
            if ((self.pilot is None or row["control_generation"] > 0) and previous is not None
                    and current > previous and row.get("periodic", True)):
                self._intervals.append(current - previous)
            self._previous_game[identity] = max(previous or 0, current)

    def _bridge_reply(self, message: GameMessage) -> None:
        # Observe receipt, not request completion: one late reply still counts,
        # and a timed-out request is never mistaken for an answer. Body packets
        # and unsolicited roster/loading/session events have separate meanings.
        if isinstance(message, (Result, ActionResult, Pong, RoutineInfo, Snapshot, NpcActResult)):
            self._wall_received("bridge_reply")

    def _wall_received(self, channel: str) -> None:
        now = self.clock()
        previous = self._wall_previous.get(channel)
        if previous is not None:
            gap = max(0, now - previous) * 1000
            self._wall_max[channel] = max(self._wall_max.get(channel, 0), gap)
            self._wall_max_all[channel] = max(self._wall_max_all.get(channel, 0), gap)
        self._wall_previous[channel] = now
        self._wall_counts[channel] += 1

    def _wall_sample(self, now: float) -> dict[str, Any]:
        values: dict[str, Any] = {}
        for channel in ("observation", "bridge_reply"):
            previous = self._wall_previous.get(channel)
            age = max(0, now - previous) * 1000 if previous is not None else None
            # Include a still-open gap, so a frozen game is visible before its
            # first packet after recovery. Count/age distinguish silence from
            # a window that actually received a late packet.
            gap = max(self._wall_max.get(channel, 0), age or 0) if previous is not None else None
            if gap is not None:
                self._wall_max_all[channel] = max(self._wall_max_all.get(channel, 0), gap)
            values[f"{channel}_wall_gap_max_ms"] = gap
            values[f"{channel}_wall_age_ms"] = age
            values[f"{channel}_wall_count"] = self._wall_counts[channel]
        self._wall_max.clear()
        self._wall_counts.clear()
        return values

    @staticmethod
    def _lease_scope(summary: FightSummary) -> tuple[int, ...] | None:
        lease = summary.lease
        return (lease.session_epoch, lease.world_epoch, lease.entity_id,
                lease.control_generation) if lease is not None else None

    def finish_started_at(self, lease: BodyLease) -> float | None:
        scope = (lease.session_epoch, lease.world_epoch, lease.entity_id, lease.control_generation)
        return self._finish_started.get(scope)

    def note_fight_end(self, lease: BodyLease, at: float) -> None:
        scope = (lease.session_epoch, lease.world_epoch, lease.entity_id, lease.control_generation)
        self._finish_started[scope] = min(at, self._finish_started.get(scope, at))

    def release_elapsed_seconds(self, summary: FightSummary) -> float | None:
        delay = _release_delay(summary)
        scope = self._lease_scope(summary)
        if delay is None or scope not in self._finish_started:
            return delay
        start = self._finish_started[scope]
        # The pilot can await terminal diagnostics before starting its release
        # budget. The soak's two seconds start at the original finish trigger.
        return delay + max(0, self._release_started.get(scope, start) - start)

    def sync_summaries(self) -> None:
        """Retain late native confirmations before the pilot's bounded history evicts them."""
        for summary in getattr(self.pilot, "summaries", ()):
            scope = self._lease_scope(summary)
            index = self._summary_indices.get(scope) if scope is not None else None
            if index is None or self.summaries[index] == summary:
                continue
            self.summaries[index] = summary
            self.fight_records[index]["summary"] = asdict(summary)
            self.event("release_update", summary=asdict(summary))

    def _completed(self, fight: Any, summary: FightSummary, finish_at: float | None = None) -> None:
        self.summaries.append(summary)
        # BodyFight clears its mutable client/lease before invoking _completed.
        # A scoped immutable lease identifies the policy client from step input.
        scope = self._lease_scope(summary)
        if scope is not None:
            self._summary_indices[scope] = len(self.summaries) - 1
            if finish_at is not None:
                self._finish_started.setdefault(scope, finish_at)
                self._release_started[scope] = finish_at
        client = self._clients_by_scope.get(scope)
        actions = (Counter({BODY_ACTIONS.index(name): count for name, count in summary.selected_actions})
                   if summary.policy == "engine" else self.actions_by_client.get(client, Counter()))
        decisions = sum(actions.values())
        selected = dict(summary.selected_actions)
        accepted = dict(summary.accepted_actions)
        latency = self.latencies_by_client.get(client, [])
        record = {
            "summary": asdict(summary),
            # FightSummary.decisions is incremented only AFTER an accepted bridge
            # receipt. Policy decisions also include rejected/superseded commands.
            "policy_decisions": decisions, "accepted_commands": summary.decisions,
            "accepted_fraction": summary.decisions / decisions if decisions else None,
            "actions": {str(key): value for key, value in sorted(actions.items())},
            "accepted_actions": accepted,
            "accepted_action_fractions": {
                name: accepted.get(name, 0) / count if count else None for name, count in selected.items()
            },
            "wait_200_fraction": actions[WAIT_200] / decisions if decisions else None,
            "service_calls": len(latency), "observed_service_p95_ms": percentile(latency, 0.95),
            "observed_service_p99_ms": percentile(latency, 0.99),
            "policy_fallback_reasons": (dict(summary.policy_fallbacks) if summary.policy == "engine"
                                        else dict(self.fallbacks_by_client.get(client, {}))),
        }
        self.fight_records.append(record)
        self.event("fight_end", **record)

    @contextmanager
    def observe(self) -> Iterator[SoakMetrics]:
        original_call = PolicyClient.call
        original_finish = BodyFight._finish
        original_completed = self.pilot._completed if self.pilot is not None else None
        had_completed = self.pilot is not None and "_completed" in vars(self.pilot)
        self._stream = (self.log_dir / "neural_soak.jsonl").open("a")

        async def observed(client: PolicyClient, command: str, **fields: Any) -> dict[str, Any]:
            if command != "step":
                return await original_call(client, command, **fields)
            raw = fields.get("observation")
            if isinstance(raw, dict):
                scope = tuple(raw[name] for name in (
                    "session_epoch", "world_epoch", "entity_id", "control_generation",
                ))
                self._clients_by_scope[scope] = client
            start = self.clock()
            try:
                result = await original_call(client, command, **fields)
            except Exception as error:
                self.policy_errors[type(error).__name__] += 1
                raise
            else:
                action = result.get("action_id")
                if result.get("status") == "decision" and type(action) is int:
                    self.actions_by_client[client][action] += 1
                if result.get("status") == "fallback":
                    self.fallbacks_by_client[client][str(result.get("reason"))] += 1
                return result
            finally:
                elapsed = (self.clock() - start) * 1000
                self.latencies_by_client[client].append(elapsed)
                self._latencies.append(elapsed)
                self._service_window.append(elapsed)

        async def finishing(fight: BodyFight, *args: Any, **kwargs: Any) -> None:
            lease = fight.lease
            if lease is not None:
                self.note_fight_end(lease, self.clock())
            await original_finish(fight, *args, **kwargs)

        def completed(fight: Any, summary: FightSummary, *args: Any, **kwargs: Any) -> None:
            self.sync_summaries()
            self._completed(fight, summary, args[0] if args else kwargs.get("finish_at"))
            original_completed(fight, summary, *args, **kwargs)

        if self.pilot is not None:
            PolicyClient.call = observed
            BodyFight._finish = finishing
            self.pilot._completed = completed
        self.link.raw_body_listeners.append(self._packet)
        listeners = getattr(self.link, "listeners", None)
        if listeners is not None:
            listeners.append(self._bridge_reply)
        try:
            self.sample(force=True)
            yield self
        finally:
            self.sample(force=True)
            self.link.raw_body_listeners.remove(self._packet)
            if listeners is not None:
                listeners.remove(self._bridge_reply)
            if self.pilot is not None:
                PolicyClient.call = original_call
                BodyFight._finish = original_finish
                if had_completed:
                    self.pilot._completed = original_completed
                else:
                    del self.pilot._completed
            self._stream.close()
            self._stream = None

    def sample(self, *, force: bool = False) -> dict[str, Any] | None:
        self.sync_summaries()
        now = self.clock()
        if not force and now - self._last_sample < 10:
            return None
        self._last_sample = now
        try:
            game_pid = int((self.log_dir / "game.pid").read_text().strip())
        except (OSError, ValueError):
            game_pid = None
        sample = {
            "elapsed_seconds": round(now - self.started, 3),
            "game_rss_bytes": rss_bytes(game_pid) if game_pid else None,
            "server_rss_bytes": rss_bytes(os.getpid()),
            "game_interval_count": len(self._intervals),
            "game_interval_p95_ms": percentile(self._intervals, 0.95),
            "game_interval_max_ms": max(self._intervals, default=None),
            "max_batch_rows": self._max_batch_rows,
            "max_batch_lag_game_ms": self._max_batch_lag_game_ms,
            **self._wall_sample(now),
        }
        if self.pilot is not None:
            leased = {lease.npc.upper() for lease in self.link.body_leases.values()}
            orphans = sorted(npc for npc, entry in self.pilot.people.items() if (
                entry.activity == "body" and npc.upper() not in leased
            ))
            sample.update({
                "active_leases": [lease.model_dump(mode="json")
                                  for lease in self.link.body_leases.values()],
                "lease_count": len(self.link.body_leases), "active_count": self.pilot.active_count,
                "body_without_lease": orphans,
                "release_orphan_timeout": getattr(self.pilot, "release_orphan_timeout", 0),
                "service_calls": len(self._service_window),
                "service_p95_ms": percentile(self._service_window, 0.95),
                "service_p99_ms": percentile(self._service_window, 0.99),
            })
        self.samples.append(sample)
        self.event("sample", **sample)
        self._service_window.clear()
        self._intervals.clear()
        self._max_batch_rows = self._max_batch_lag_game_ms = 0
        return sample

    def write_result(
        self, *, eligible: int = 0, captured: int = 0, release_issues: list[Any] | None = None,
        extra: dict[str, Any] | None = None,
        extra_checks: dict[str, str] | None = None,
    ) -> dict[str, str]:
        self.sample(force=True)
        release_issues = release_issues or []
        accepted = sum(summary.decisions for summary in self.summaries)
        engine_summaries = [summary for summary in self.summaries if summary.policy == "engine"]
        decisions = (sum(sum(actions.values()) for actions in self.actions_by_client.values())
                     + sum(count for summary in engine_summaries for _, count in summary.selected_actions))
        fault_ends: Counter[str] = Counter()
        for summary in self.summaries:
            # Cleanup may replace the outcome with finish_deadline. A later
            # native confirmation must not hide the fault that ended the fight.
            reason = (summary.end_trigger if summary.end_trigger in FAULT_ENDINGS
                      else summary.reason)
            if (
                reason in FAULT_ENDINGS
                or reason == "finish_deadline" and not (
                    summary.release_confirmed and summary.release_late_seconds is not None
                )
                or reason.startswith("policy_fallback:")
                and reason != "policy_fallback:observation_too_old"
            ):
                fault_ends[reason] += 1
        finish_deadlines = [summary for summary in self.summaries
                            if summary.reason == "finish_deadline"]
        late_releases = [summary.release_late_seconds for summary in self.summaries
                         if summary.release_confirmed and summary.release_late_seconds is not None]
        orphan_timeouts = getattr(self.pilot, "release_orphan_timeout", 0)
        release_deadline_issues = []
        for summary in self.summaries:
            if summary.lease is None:
                continue
            elapsed = self.release_elapsed_seconds(summary)
            if not release_within_deadline(summary) or elapsed is None or elapsed > 2:
                release_deadline_issues.append({
                    "npc": summary.npc, "lease": summary.lease.model_dump(mode="json"),
                    "release_confirmed": summary.release_confirmed,
                    "release_seconds": summary.release_seconds,
                    "release_late_seconds": summary.release_late_seconds,
                    "release_elapsed_seconds": elapsed,
                })
        timeouts = sum(summary.policy_timeouts for summary in self.summaries)
        faulty_fallbacks: Counter[str] = Counter()
        engine_fallbacks = [dict((reason, count) for reason, count in summary.policy_fallbacks
                                if reason not in TRANSIENT_ACTION_REJECTIONS)
                            for summary in engine_summaries]
        for fallbacks in [*self.fallbacks_by_client.values(), *engine_fallbacks]:
            faulty_fallbacks.update({reason: count for reason, count in fallbacks.items()
                                    if reason != "observation_too_old"})
        game_rss = game_rss_window(self.samples, (extra or {}).get("loads", []))
        server_rss = rss_window(self.samples, "server_rss_bytes")
        p95, p99 = percentile(self._latencies, 0.95), percentile(self._latencies, 0.99)
        log_errors = scan_server_errors(self.log_dir / "server.log")
        checks: dict[str, str] = {}

        def check(name: str, ok: bool, detail: str) -> None:
            checks[name] = f"{'done' if ok else 'failed'} ({detail})"

        capture_fraction = captured / eligible if eligible else None
        accept_fraction = accepted / decisions if decisions else None
        if self.pilot is not None:
            check("fault_fallbacks", not (
                fault_ends or timeouts or faulty_fallbacks or self.policy_errors or orphan_timeouts
            ),
                  f"ends={dict(fault_ends)}, timeouts={timeouts}, fallbacks={dict(faulty_fallbacks)}, "
                  f"policy_errors={dict(self.policy_errors)}, release_orphan_timeout={orphan_timeouts}")
            check("capture_rate", capture_fraction is not None and capture_fraction >= 0.95,
                  f"captured={captured}/{eligible}, minimum=95%")
            check("accepted_commands", accept_fraction is not None and accept_fraction >= 0.95,
                  f"accepted={accepted}/{decisions}, minimum=95%")
            check("release_clean", not release_issues and not release_deadline_issues,
                  f"issues={len(release_issues)}, deadline_issues={len(release_deadline_issues)}, limit=2s")
        short = game_rss["short_window"] and game_rss.get("growth_percent") is not None
        check("game_rss", game_rss["ok"] or short,
              f"growth_percent={game_rss.get('growth_percent')}, "
              f"growth_mib_per_load={game_rss['growth_mib_per_load']}, "
              f"native_control_mib_per_load={game_rss['native_control_mib_per_load']}, "
              f"limit_mib_per_load={game_rss['limit_mib_per_load']}, "
              f"tolerance_percent={game_rss['tolerance_percent']}, loads={game_rss['load_count']}, "
              f"assessment={'20-minute gate deferred to full run' if short else 'final 20 minutes'}, "
              f"window_start_seconds={game_rss.get('start_seconds')}, "
              f"window_end_seconds={game_rss.get('end_seconds')}, "
              f"window_seconds={game_rss.get('observed_seconds')}, "
              f"short_window={game_rss['short_window']}, reason={game_rss.get('reason')}")
        short = server_rss["short_window"] and server_rss.get("growth_percent") is not None
        check("server_rss", server_rss["ok"] or short,
              f"growth_percent={server_rss.get('growth_percent')}, limit=5%, "
              f"assessment={'20-minute gate deferred to full run' if short else 'final 20 minutes'}, "
              f"window_seconds={server_rss.get('observed_seconds')}, "
              f"short_window={server_rss['short_window']}")
        if self.pilot is not None:
            if getattr(self.pilot, "policy", "service") == "engine":
                engine_p99 = max((summary.engine_p99_us for summary in engine_summaries
                                  if summary.engine_steps and summary.engine_p99_us is not None),
                                 default=None)
                check("engine_latency", engine_p99 is not None and engine_p99 <= 20_000,
                      f"p99_us={engine_p99}, limit=20000us, "
                      f"steps={sum(summary.engine_steps for summary in engine_summaries)}")
            else:
                check("service_latency", p95 is not None and p99 is not None and p95 <= 20 and p99 <= 20,
                      f"p95_ms={p95}, p99_ms={p99}, limit=20ms, calls={len(self._latencies)}")
        check("server_log", not log_errors, f"errors={log_errors}, log=server.log")
        checks.update(extra_checks or {})
        self.data = {
            "status": "done" if all(value.startswith("done") for value in checks.values()) else "failed",
            "elapsed_seconds": round(self.clock() - self.started, 3),
            "checks": checks,
            "metrics": {
                "eligible": eligible, "captured": captured, "capture_fraction": capture_fraction,
                "policy_decisions": decisions, "accepted_commands": accepted,
                "accepted_fraction": accept_fraction,
                "action_rejections": dict(sum((Counter(dict(summary.action_rejections))
                                                for summary in self.summaries), Counter())),
                "fault_endings": dict(fault_ends), "policy_timeouts": timeouts,
                "faulty_policy_fallbacks": dict(faulty_fallbacks), "policy_errors": dict(self.policy_errors),
                "fallbacks": sum(summary.fallbacks for summary in self.summaries),
                "release_orphan_timeout": orphan_timeouts,
                "finish_deadline_count": len(finish_deadlines),
                "finish_deadline_unconfirmed": sum(not (
                    summary.release_confirmed and summary.release_late_seconds is not None
                ) for summary in finish_deadlines),
                "release_late_count": len(late_releases),
                "release_late_max_seconds": max(late_releases, default=None),
                "release_issues": release_issues, "release_deadline_issues": release_deadline_issues,
                "game_rss": game_rss, "server_rss": server_rss,
                "service_calls": len(self._latencies), "service_p95_ms": p95, "service_p99_ms": p99,
                "max_batch_rows": self._max_batch_rows_all,
                "max_batch_lag_game_ms": self._max_batch_lag_all,
                "server_log_errors": log_errors,
                "end_reasons": dict(Counter(summary.reason for summary in self.summaries)),
                "long_fights": [record for record in self.fight_records if (
                    (record["summary"]["fight_seconds"] or 0) > 60
                    and record["summary"]["reason"] not in {"hero_down", "target_down"}
                )],
            },
            "fights": self.fight_records, "extra": extra or {},
        }
        common_metrics = {
            "game_rss": game_rss, "server_rss": server_rss,
            "max_batch_rows": self._max_batch_rows_all,
            "max_batch_lag_game_ms": self._max_batch_lag_all,
            "observation_wall_gap_max_ms": self._wall_max_all.get("observation"),
            "bridge_reply_wall_gap_max_ms": self._wall_max_all.get("bridge_reply"),
            "server_log_errors": log_errors,
        }
        if self.pilot is None:
            self.data["mode"] = "control"
            self.data["metrics"] = common_metrics
            del self.data["fights"]
        else:
            self.data["mode"] = "p30"
            self.data["metrics"].update(common_metrics)
        (self.log_dir / "neural_soak_result.json").write_text(
            json.dumps(self.data, ensure_ascii=False, indent=2, allow_nan=False,
                       default=self._json_default) + "\n"
        )
        self.event("result", status=self.data["status"], checks=checks)
        return checks
