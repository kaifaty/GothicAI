"""Independent realtime melee leases driven by the remote P30 JSONL policy (no model dependencies)."""

from __future__ import annotations

import asyncio
import contextlib
import itertools
import json
import logging
import math
import time
from collections import Counter, deque
from collections.abc import Callable
from dataclasses import dataclass, replace
from typing import Any
from urllib.parse import urlsplit

from websockets.exceptions import ConnectionClosed

from .game import GameDisconnected, GameError, GameLink
from .protocol import (
    Attached,
    BodyControlEvents,
    BodyLease,
    BodyObservation,
    GameMessage,
    Hello,
    Loading,
    NpcEvent,
    NpcRoster,
    RosterEntry,
)

log = logging.getLogger(__name__)
SERVICE_TIMEOUT = 0.100
POLICY_TIMEOUT_LIMIT = 5
POLICY_RESPONSE_TIMEOUT = 1.0
BRIDGE_TIMEOUT = 0.250
ACQUIRE_INTERVAL = 0.150
REACQUIRE_INTERVAL = 2.0
ACQUIRE_WINDOW = 5.0
AWAIT_BOUNDARY_MS = 1500
MAX_FIGHT_SECONDS = 300.0
HEARTBEAT_INTERVAL = 0.250
HEARTBEAT_TIMEOUT = 0.500
LEASE_TIMEOUT_MS = 2000
EVENTS_INTERVAL = 0.150
FINISH_TIMEOUT = 2.0
RELEASE_ORPHAN_GRACE = 3.0
MAX_JSON_BYTES = 256 * 1024
TRANSIENT_REJECTION_LIMIT = 10
MAX_OBSERVATION_AGE_GAME_MS = 100
TARGET_ABSENCE_TIMEOUT_MS = 1000
WORLD_STOP_REASONS = frozenset({"loading", "session_changed", "world_changed", "disconnected"})
SOFT_BODY_OWNERSHIP_REFUSAL = "NPC already has conversation or soft-action body ownership"
# BodyController::validate and FightAlgo::resolve recheck live state after the
# observation. These refusals retain the lease and need a fresh row, not a retry
# of the same command. Scope, ownership and protocol failures remain terminal.
TRANSIENT_ACTION_REJECTIONS = frozenset({
    "stale_observation", "expired", "expired_wall", "command_busy",
    "target_invalid", "masked", "locked",
})
BRIDGE_ERRORS = (GameError, ConnectionClosed, OSError, TimeoutError)
TARGET_IS_HERO = [
    "&&",
    ["call", "NPC_GETTARGET", ["inst", "SELF"]],
    ["call", "NPC_ISPLAYER", ["inst", "OTHER"]],
]


class PolicyError(RuntimeError):
    """A policy response cannot authorize an action."""


class PolicyClient:
    """Async JSONL RPC. Late replies cannot become the next request's response."""

    def __init__(self, url: str) -> None:
        address = urlsplit(url)
        if address.scheme != "tcp" or not address.hostname or not address.port or address.path:
            raise ValueError("body URL must be tcp://host:port")
        self.host, self.port = address.hostname, address.port
        self.writer: asyncio.StreamWriter | None = None
        self.reader_task: asyncio.Task[None] | None = None
        self.pending: dict[int, asyncio.Future[dict[str, Any]]] = {}
        self.ids = itertools.count(1)
        self.last_response_at: float | None = None
        self.response_received = asyncio.Event()

    async def connect(self) -> None:
        async with asyncio.timeout(SERVICE_TIMEOUT):
            reader, self.writer = await asyncio.open_connection(self.host, self.port, limit=MAX_JSON_BYTES)
        self.reader_task = asyncio.create_task(self._read(reader))

    async def _read(self, reader: asyncio.StreamReader) -> None:
        try:
            while line := await reader.readline():
                if len(line) > MAX_JSON_BYTES or not line.endswith(b"\n"):
                    raise PolicyError("invalid policy response framing")
                response = json.loads(line)
                if not isinstance(response, dict) or type(response.get("id")) is not int:
                    raise PolicyError("invalid policy response identity")
                self.last_response_at = asyncio.get_running_loop().time()
                self.response_received.set()
                future = self.pending.get(response["id"])
                if future is not None and not future.done():
                    future.set_result(response)
            raise PolicyError("policy disconnected")
        except (OSError, ValueError, PolicyError) as error:
            for future in self.pending.values():
                if not future.done():
                    future.set_exception(error)

    async def call(self, command: str, **fields: Any) -> dict[str, Any]:
        request = {"id": next(self.ids), "cmd": command, **fields}
        encoded = (json.dumps(request, separators=(",", ":"), allow_nan=False) + "\n").encode()
        if len(encoded) > MAX_JSON_BYTES:
            raise PolicyError("policy request exceeds bounded JSONL size")
        if self.writer is None or self.writer.is_closing():
            raise PolicyError("policy is not connected")
        future = asyncio.get_running_loop().create_future()
        self.pending[request["id"]] = future
        try:
            async with asyncio.timeout(SERVICE_TIMEOUT):
                self.writer.write(encoded)
                await self.writer.drain()
                response = await future
            if response.get("ok") is not True:
                raise PolicyError(f"policy rejected {command}: {response.get('reason')}")
            return response
        finally:
            self.pending.pop(request["id"], None)

    async def close(self) -> None:
        try:
            if self.writer is not None:
                self.writer.close()
                with contextlib.suppress(OSError):
                    await self.writer.wait_closed()
        finally:
            if self.reader_task is not None:
                self.reader_task.cancel()
                await asyncio.gather(self.reader_task, return_exceptions=True)


def _percentile(values: list[float], fraction: float) -> float | None:
    return sorted(values)[math.ceil(len(values) * fraction) - 1] if values else None


@dataclass(frozen=True)
class PolicyDecisionTrace:
    observation_seq: int
    t_game_ms: int
    action_id: int
    sent: bool
    accepted: bool
    reason: str


@dataclass(frozen=True)
class FightSummary:
    """Immutable fight snapshot, replaced when a late native return is confirmed."""

    npc: str | None
    lease: BodyLease | None
    target_kind: str | None
    target_name: str | None
    reason: str
    decisions: int
    fallbacks: int
    memory_only_lag_rows: int
    observation_too_old: int
    policy_memory_resets: int
    max_batch_rows: int
    max_batch_lag_game_ms: int
    service_p50_ms: float | None
    service_p95_ms: float | None
    packet_to_request_p50_ms: float | None
    packet_to_request_p95_ms: float | None
    packet_to_response_p50_ms: float | None
    packet_to_response_p95_ms: float | None
    response_to_action_p50_ms: float | None
    response_to_action_p95_ms: float | None
    packet_to_action_p50_ms: float | None
    packet_to_action_p95_ms: float | None
    acquire_refusals: tuple[tuple[str, int], ...]
    acquire_requests: int
    acquire_completed: int
    acquire_wait_p50_ms: float | None
    acquire_wait_max_ms: float | None
    action_rejections: tuple[tuple[str, int], ...]
    transient_rejections_consecutive: int
    policy_timeouts: int
    policy_timeouts_consecutive: int
    policy_timeout_wait_p95_ms: float | None
    policy_response_silence_ms: float | None
    ownership_poll_timeouts: int
    ownership_poll_max_gap_ms: float
    target_invisible_seconds: float
    target_invisible_game_ms: int
    target_invisible_last_seconds: float
    target_invisible_last_game_ms: int
    native_release_reason: str | None
    last_policy_decisions: tuple[PolicyDecisionTrace, ...]
    fight_seconds: float | None
    release_confirmed: bool
    release_seconds: float | None
    watch_removed: bool
    end_trigger: str
    release_late_seconds: float | None = None


@dataclass(frozen=True)
class PendingRelease:
    lease: BodyLease
    started_at: float
    deadline: float
    summary: FightSummary


class BodyPilot:
    """Dispatch independent fights over one bridge and one shared observation watch."""

    def __init__(
        self,
        link: GameLink,
        url: str,
        *,
        companion: Callable[[str], bool] = lambda npc: False,
        unavailable: Callable[[str], bool] = lambda npc: False,
        talking: Callable[[str], bool] = lambda npc: False,
        clock: Callable[[], float] = time.monotonic,
        max_fights: int = 4,
    ) -> None:
        if type(max_fights) is not int or not 1 <= max_fights <= 4:
            raise ValueError("neural body max must be between 1 and 4")
        self.link, self.url = link, url
        self.companion, self.unavailable, self.clock = companion, unavailable, clock
        self.talking = talking
        self.max_fights = max_fights
        self.people: dict[str, RosterEntry] = {}
        self.fights: dict[str, BodyFight] = {}
        self.summaries: deque[FightSummary] = deque(maxlen=32)
        self.watch_busy = 0
        self.release_orphan_timeout = 0
        self._releasing: dict[str, PendingRelease] = {}
        self._last_release_poll = float("-inf")
        self._claimed_fights: set[str] = set()
        self._lost_fights: set[str] = set()
        self._retarget_pending: set[str] = set()
        self._reacquire_after: dict[str, float] = {}
        self._roster_attempted: set[str] = set()
        self._watch_npcs: list[str] = []
        self._pending_unwatch: set[str] = set()
        self._last_watch_attempt = float("-inf")
        self._watch_lock = asyncio.Lock()
        self._wake = asyncio.Event()
        self._done = asyncio.Event()
        self._running = self._closing = self._resetting = False
        link.listeners.append(self.on_message)
        link.raw_body_listeners.append(self.on_body_packet)

    @property
    def last_summary(self) -> FightSummary | None:
        """The last completed fight; late release confirmation publishes a new snapshot."""
        return self.summaries[-1] if self.summaries else None

    @property
    def active_count(self) -> int:
        return len(self.fights)

    def handles(self, npc: str) -> bool:
        """Include observation, pending acquisition and unconfirmed release cleanup."""
        return npc.upper() in self.fights or npc.upper() in self._releasing

    def _allowed(self, npc: str) -> bool:
        entry = self.people.get(npc)
        return bool(
            entry is not None
            and entry.guild <= 16
            and entry.distance <= 2000
            and entry.activity not in {"dialog", "attached", "down"}
            and (entry.activity != "body" or (fight := self.fights.get(npc)) is not None
                 and (fight.lease is not None or fight._acquiring))
            and (not self.talking(npc) or (
                entry.activity in {"fight", "body"} and entry.state.upper() == "ZS_ATTACK"
            ))
            and not self._companion(npc)
            and not self.unavailable(npc)
        )

    def _companion(self, npc: str) -> bool:
        return bool(self.companion(npc) or self.link.party.get(npc, (0, 0))[1])

    async def _live_target(self, actor_npc: str) -> tuple[str, str | None, str] | None:
        """Classify the engine's current target, never the roster's combat snapshot."""
        companions = sorted(
            npc for npc in self.people.keys() | self.link.party.keys()
            if self._companion(npc)
        )
        expressions = [TARGET_IS_HERO, *[
            ["&&", ["call", "NPC_GETTARGET", ["inst", "SELF"]],
             ["==", ["call", "HLP_GETINSTANCEID", ["inst", "OTHER"]],
              ["call", "HLP_GETINSTANCEID", ["inst", npc]]]]
            for npc in companions
        ]]
        matches = await self.link.story_eval(actor_npc, expressions, timeout=BRIDGE_TIMEOUT)
        if len(matches) != len(expressions):
            raise GameError("target identity unavailable")
        if matches[0] == 1:
            return "hero", None, "HERO"
        for npc, matched in zip(companions, matches[1:], strict=True):
            if matched == 1 and self._companion(npc):
                entry = self.people.get(npc)
                return "companion", entry.name if entry else npc, npc
        return None

    def on_message(self, message: GameMessage) -> None:
        if isinstance(message, NpcRoster):
            entries = {entry.id.upper(): entry for entry in message.npcs}
            ended = {
                npc for npc, entry in entries.items()
                if entry.activity == "down"
                or (entry.activity == "routine" and entry.state.upper() not in {"ZS_ATTACK", "ZS_MM_ATTACK"})
            }
            ended.update(npc for npc in self._lost_fights if (
                npc in entries and entries[npc].distance > 2000
                or message.full and npc not in entries
            ))
            self._forget_ended(ended)
            self.people = entries if message.full else {**self.people, **entries}
            for fight in list(self.fights.values()):
                if not self._allowed(fight.npc):
                    fight.stop("npc_unavailable")
            for npc, entry in sorted(entries.items(), key=lambda item: item[1].distance):
                if entry.activity != "fight" or entry.state.upper() != "ZS_ATTACK":
                    continue
                if self.handles(npc):
                    self._roster_attempted.add(npc)
                elif npc not in self._roster_attempted:
                    self._start(npc)
        elif isinstance(message, (Loading, Hello)):
            self._releasing.clear()
            self.people.clear()
            self._claimed_fights.clear()
            self._lost_fights.clear()
            self._retarget_pending.clear()
            self._reacquire_after.clear()
            self._roster_attempted.clear()
            self._stop_all("loading" if isinstance(message, Loading) else "session_changed")
        elif isinstance(message, Attached):
            if fight := self.fights.get(message.npc.upper()):
                fight.stop("attached")
        elif isinstance(message, BodyControlEvents):
            for fight in list(self.fights.values()):
                fight.on_message(message)
            for npc, pending in list(self._releasing.items()):
                if any(
                    (event.entity_id, event.session_epoch, event.world_epoch, event.control_generation)
                    == BodyFight._identity(pending.lease)
                    and event.phase == "ownership" and event.reason == "native_restored"
                    and event.owner == "native"
                    for event in message.events
                ):
                    self._released(npc, pending)
        elif isinstance(message, NpcEvent) and not self._closing:
            npc = None
            if message.other.upper() in {"PLAYER", "HERO"} and message.kind in {
                "attack", "threatened", "weapon_drawn", "npc_attack",
            }:
                npc = message.npc.upper()
            elif message.kind == "hero_attacked":
                npc = message.other.upper()
            if npc is not None:
                self._start(npc)
        self._wake.set()

    def _forget_ended(self, npcs: set[str]) -> None:
        self._claimed_fights.difference_update(npcs)
        self._lost_fights.difference_update(npcs)
        self._retarget_pending.difference_update(npcs)
        self._roster_attempted.difference_update(npcs)
        for npc in npcs:
            self._reacquire_after.pop(npc, None)

    def _start(self, npc: str) -> bool:
        npc = npc.upper()
        if (
            self.handles(npc) or self._closing or self._resetting
            or self.active_count >= self.max_fights
            or len(self.fights.keys() | self._releasing.keys()) >= 4
            or npc in self._claimed_fights
            or self.clock() < self._reacquire_after.get(npc, float("-inf"))
            or not self._allowed(npc)
        ):
            return False
        if (self.fights or self._releasing) and not self._watch_can_change():
            self.watch_busy += 1
            log.info(
                "neural-body watch refused npc=%s reason=watch_busy active_fights=%d protocol=%s count=%d",
                npc, self.active_count, self.link.hello.protocol if self.link.hello else None,
                self.watch_busy,
            )
            self._reacquire_after[npc] = self.clock() + REACQUIRE_INTERVAL
            return False
        self.fights[npc] = BodyFight(self, npc)
        self._retarget_pending.discard(npc)
        entry = self.people[npc]
        if entry.activity == "fight" and entry.state.upper() == "ZS_ATTACK":
            self._roster_attempted.add(npc)
        self._wake.set()
        return True

    def _watch_can_change(self) -> bool:
        return self.link.hello is not None and self.link.hello.protocol >= 45

    async def _watch(self, fight: BodyFight) -> None:
        # A growing/shrinking watch is a replacement list. Serialize its receipts
        # so a concurrent finish cannot overwrite a newly admitted actor.
        async with self._watch_lock:
            npcs = [npc for npc in self._watch_npcs if (
                (npc in self.fights or npc in self._releasing) and npc not in self._pending_unwatch
            )]
            if fight.npc not in npcs:
                npcs.append(fight.npc)
            # A timeout may mean the bridge applied this list without a receipt.
            previous = self._watch_npcs
            self._watch_npcs = npcs
            try:
                await self.link.body_watch(npcs, timeout=BRIDGE_TIMEOUT)
            except GameError:
                self._watch_npcs = previous
                raise
            self._pending_unwatch.difference_update(set(previous) - set(npcs))
            self._pending_unwatch.discard(fight.npc)

    async def _unwatch(self, fight: BodyFight) -> bool:
        self._pending_unwatch.add(fight.npc)
        return await self._flush_unwatch()

    async def _flush_unwatch(self) -> bool:
        async with self._watch_lock:
            pending = self._pending_unwatch.copy()
            npcs = [npc for npc in self._watch_npcs if (
                npc not in pending and (npc in self.fights or npc in self._releasing)
            )]
            self._last_watch_attempt = asyncio.get_running_loop().time()
            await self.link.body_watch(npcs, timeout=BRIDGE_TIMEOUT)
            self._watch_npcs = npcs
            self._pending_unwatch.difference_update(pending)
            return True

    async def _retry_unwatch(self, *, force: bool = False) -> None:
        if self._pending_unwatch and (
            force or asyncio.get_running_loop().time() - self._last_watch_attempt >= ACQUIRE_INTERVAL
        ):
            with contextlib.suppress(*BRIDGE_ERRORS):
                async with asyncio.timeout(BRIDGE_TIMEOUT):
                    await self._flush_unwatch()

    def _released(self, npc: str, pending: PendingRelease, *, confirmed: bool = True) -> None:
        self._releasing.pop(npc, None)
        if self.link.body_leases.get(pending.lease.entity_id) == pending.lease:
            self.link.body_leases.pop(pending.lease.entity_id)
        self._pending_unwatch.add(npc)
        if confirmed:
            late_seconds = time.monotonic() - pending.started_at
            updated = replace(pending.summary, release_confirmed=True, release_late_seconds=late_seconds)
            for index, summary in enumerate(self.summaries):
                if summary is pending.summary:
                    self.summaries[index] = updated
                    break
            self._remember_retarget(updated)
            log.info(
                "neural-body end npc=%s reason=%s end_trigger=%s release_late_seconds=%.3f "
                "release_confirmed=True ownership_poll_timeouts=%d ownership_poll_max_gap_ms=%.1f",
                npc, updated.reason, updated.end_trigger, late_seconds,
                updated.ownership_poll_timeouts, updated.ownership_poll_max_gap_ms,
            )
        self._remember_lost_fight(
            updated if confirmed else pending.summary, orphan_expired=not confirmed,
        )
        self._wake.set()

    async def _poll_releases(self) -> None:
        if not self._releasing:
            return
        # Never infer native ownership merely from a missing local lease: another
        # RPC may have discarded it on a generation mismatch or a refusal.
        for npc, pending in list(self._releasing.items()):
            if self.clock() >= pending.deadline:
                self.release_orphan_timeout += 1
                log.warning(
                    "neural-body release_orphan_timeout npc=%s entity=%d count=%d waited_seconds=%.3f",
                    npc, pending.lease.entity_id, self.release_orphan_timeout,
                    time.monotonic() - pending.started_at,
                )
                self._released(npc, pending, confirmed=False)
        now = asyncio.get_running_loop().time()
        if not self._releasing or now - self._last_release_poll < EVENTS_INTERVAL:
            return
        self._last_release_poll = now
        with contextlib.suppress(*BRIDGE_ERRORS):
            self.on_message(await self.link.body_events(timeout=BRIDGE_TIMEOUT))
        if not self._releasing:
            return
        with contextlib.suppress(*BRIDGE_ERRORS):
            status = await self.link.body_control_status(timeout=BRIDGE_TIMEOUT)
            for npc, pending in list(self._releasing.items()):
                lease = pending.lease
                if (status.session_epoch, status.world_epoch) != (lease.session_epoch, lease.world_epoch):
                    continue
                if any(actor.entity_id == lease.entity_id and actor.owner == "native"
                       for actor in status.actors):
                    self._released(npc, pending)

    def _remember_lost_fight(self, summary: FightSummary, *, orphan_expired: bool = False) -> None:
        reason = summary.end_trigger if summary.reason == "finish_deadline" else summary.reason
        if (
            summary.npc in self._claimed_fights and (summary.release_confirmed or orphan_expired)
            and reason in {"npc_unavailable", "target_lost"}
        ):
            # A distant NPC can return straight to native combat. Its routine
            # while outside the roster is not observable, so range exit ends
            # this lost episode without requiring an intervening routine row.
            # Lease expiry permits the same bookkeeping without fabricating a
            # release confirmation in the summary.
            self._lost_fights.add(summary.npc)
            entry = self.people.get(summary.npc)
            if entry is None or entry.distance > 2000:
                self._forget_ended({summary.npc})

    def _remember_retarget(self, summary: FightSummary) -> None:
        if (
            summary.npc is not None and summary.end_trigger == "target_changed"
            and summary.release_confirmed and not self._closing and not self._resetting
        ):
            # Keep the claim until the dispatcher can check the live target.
            # The completed BodyFight has already discarded its mutable state.
            self._retarget_pending.add(summary.npc)

    async def _resume_retargets(self) -> None:
        for npc in sorted(self._retarget_pending):
            entry = self.people.get(npc)
            if entry is None or entry.activity != "fight" or entry.state.upper() != "ZS_ATTACK":
                continue
            if npc in self._claimed_fights:
                if (
                    self.handles(npc) or not self._allowed(npc)
                    or self.clock() < self._reacquire_after.get(npc, float("-inf"))
                ):
                    continue
                try:
                    target = await self._live_target(npc)
                except BRIDGE_ERRORS:
                    continue
                if self._closing or self._resetting or npc not in self._retarget_pending or self.handles(npc):
                    continue
                if target is None:
                    self._retarget_pending.discard(npc)
                    continue
                self._claimed_fights.discard(npc)
            self._start(npc)

    def _completed(self, fight: BodyFight, summary: FightSummary, finish_at: float,
                   release_deadline: float) -> None:
        self.summaries.append(summary)
        reason = summary.end_trigger if summary.reason == "finish_deadline" else summary.reason
        if (
            summary.npc is not None and summary.lease is not None and not summary.release_confirmed
            and summary.reason not in WORLD_STOP_REASONS
            and not self.link.closed.is_set()
            and (self.link.body_session_epoch, self.link.body_generation)
            == (summary.lease.session_epoch, summary.lease.world_epoch)
        ):
            self._releasing[summary.npc] = PendingRelease(
                summary.lease, finish_at, release_deadline, summary,
            )
        if self.fights.get(summary.npc) is fight:
            del self.fights[summary.npc]
        if (
            summary.npc is not None and reason in {"hero_down", "target_down"}
            and not self._closing and not self._resetting
        ):
            # Native looting/retreat can still be ZS_ATTACK after the target fell.
            # A routine row during release may already have cleared the claim;
            # keep this completed fight closed until a later non-combat roster.
            self._claimed_fights.add(summary.npc)
            self._roster_attempted.add(summary.npc)
            self._lost_fights.discard(summary.npc)
            self._retarget_pending.discard(summary.npc)
        if (
            summary.npc is not None and summary.reason == "acquire_exhausted"
            and summary.lease is None and summary.acquire_completed == 0
            and summary.acquire_requests > 0
            and {reason for reason, _ in summary.acquire_refusals} == {SOFT_BODY_OWNERSHIP_REFUSAL}
        ):
            # Speech or a soft act can outlast this bounded acquisition window.
            # A later native combat roster must retry after that owner is gone;
            # policy failures and every other refusal keep their episode guard.
            self._roster_attempted.discard(summary.npc)
            self._reacquire_after[summary.npc] = self.clock() + REACQUIRE_INTERVAL
        self._remember_lost_fight(summary)
        if not self.fights:
            self._resetting = False
        if summary.reason == "finish_deadline":
            self._remember_retarget(summary)
        self._wake.set()

    def _stop_all(self, reason: str) -> None:
        self._resetting = bool(self.fights)
        for fight in list(self.fights.values()):
            fight.stop(reason)
        self._wake.set()

    def _drain_events(self) -> None:
        # GameLink also polls destructively while awaiting an acquisition's safe
        # boundary. Its queue retains ownership events for every other actor.
        events = []
        while (event := self.link.pop_body_control_event()) is not None:
            events.append(event)
        if events:
            self.on_message(BodyControlEvents(
                type="body_control_events",
                session_epoch=events[0].session_epoch,
                world_epoch=events[0].world_epoch,
                dropped_total=self.link.body_control_dropped_events,
                events=tuple(events),
            ))

    def on_body_packet(self, packet: dict[str, Any]) -> None:
        rows = packet["observations"]
        now_game_ms = max((row["t_game_ms"] for row in rows), default=None)
        for fight in list(self.fights.values()):
            lease = fight.lease
            if lease is not None and (packet["session_epoch"], packet["world_epoch"]) != (
                lease.session_epoch, lease.world_epoch,
            ):
                self._stop_all("world_changed")
                return
        for fight in list(self.fights.values()):
            own_rows = [row for row in rows if (
                (fight._entity is None or row["entity_id"] == fight._entity)
                and row["script_symbol"] == fight._symbol
            )]
            if now_game_ms is not None:
                fight._now_game_ms = now_game_ms
            if own_rows:
                fight.on_body_packet({**packet, "observations": own_rows})

    async def run(self) -> None:
        self._running = True
        try:
            while not self._closing:
                self._wake.clear()
                self._drain_events()
                if self.link.closed.is_set():
                    self._stop_all("disconnected")
                    break
                await self._retry_unwatch()
                await self._poll_releases()
                for fight in list(self.fights.values()):
                    if fight.task is None:
                        fight.task = asyncio.create_task(fight.run())
                await self._resume_retargets()
                try:
                    await asyncio.wait_for(self._wake.wait(), timeout=0.025)
                except TimeoutError:
                    pass
        finally:
            self._closing = True
            fights = list(self.fights.values())
            for fight in fights:
                fight._closing = True
                fight.stop(fight._reason or "shutdown")
            try:
                await asyncio.gather(*[
                    fight.task if fight.task is not None else fight._finish(fight._reason or "shutdown")
                    for fight in fights
                ])
                await self._retry_unwatch(force=True)
            finally:
                self._unregister()
                self._done.set()

    async def aclose(self) -> None:
        self._closing = True
        self._stop_all("shutdown")
        if self._running:
            await self._done.wait()
        else:
            try:
                fights = list(self.fights.values())
                await asyncio.gather(*[fight._finish(fight._reason or "shutdown") for fight in fights])
                await self._retry_unwatch(force=True)
            finally:
                self._unregister()
                self._done.set()

    def _unregister(self) -> None:
        if self.on_message in self.link.listeners:
            self.link.listeners.remove(self.on_message)
        if self.on_body_packet in self.link.raw_body_listeners:
            self.link.raw_body_listeners.remove(self.on_body_packet)


class BodyFight:
    """One lease, policy TCP episode and all of its independent combat state."""

    def __init__(self, pilot: BodyPilot, npc: str) -> None:
        self.pilot, self.link, self.clock = pilot, pilot.link, pilot.clock
        self.npc: str | None = npc
        self._retargeting = npc in pilot._retarget_pending
        self._claimed_fights = pilot._claimed_fights
        self._retarget_pending = pilot._retarget_pending
        self._reacquire_after = pilot._reacquire_after
        self.task: asyncio.Task[None] | None = None
        self.lease: BodyLease | None = None
        self.client: PolicyClient | None = None
        self._rows: deque[tuple[dict[str, Any], float]] = deque()
        self._wake = asyncio.Event()
        self._closing = False
        self._reason: str | None = None
        self._watched = False
        self._watch_ready = False
        self._attempt_at = self.clock()
        self._last_acquire = float("-inf")
        self._fight_at = 0.0
        self._initial_game_ms: int | None = None
        self._now_game_ms = 0
        self._last_seq = 0
        self._heartbeat_task: asyncio.Task[None] | None = None
        self._policy_watchdog_task: asyncio.Task[None] | None = None
        self._policy_watchdog_at: float | None = None
        self._last_events_poll = 0.0
        self._last_events_success_at: float | None = None
        self._ownership_poll_timeouts = 0
        self._ownership_poll_max_gap_ms = 0.0
        self._acquiring = False
        self._acquire_cancel = asyncio.Event()
        self._pending_acquire: BodyObservation | None = None
        self._symbol: int | None = None
        self._entity: int | None = None
        self._hero: int | None = None
        self._target_kind: str | None = None
        self._target_name: str | None = None
        self._target_npc: str | None = None
        self._observed_down: str | None = None
        self._refusals: Counter[str] = Counter()
        self._acquire_requests = 0
        self._acquire_completed = 0
        self._acquire_waits: list[float] = []
        self._action_rejections: Counter[str] = Counter()
        self._transient_rejections_consecutive = 0
        self._policy_timeouts = 0
        self._policy_timeouts_consecutive = 0
        self._policy_timeout_waits: list[float] = []
        self._target_absent_game_ms: int | None = None
        self._invisible_at: float | None = None
        self._invisible_game_ms: int | None = None
        self._invisible_seconds_max = 0.0
        self._invisible_game_ms_max = 0
        self._invisible_last_seconds = 0.0
        self._invisible_last_game_ms = 0
        self._native_release_reason: str | None = None
        self._policy_decisions: deque[PolicyDecisionTrace] = deque(maxlen=10)
        self._latencies: list[float] = []
        self._packet_to_request: list[float] = []
        self._packet_to_response: list[float] = []
        self._response_to_action: list[float] = []
        self._packet_to_action: list[float] = []
        self._decisions = 0
        self._fallbacks = 0
        self._memory_only_step = False
        self._policy_needs_reset = False
        self._memory_only_lag_rows = 0
        self._observation_too_old = 0
        self._policy_memory_resets = 0
        self._max_batch_rows = 0
        self._max_batch_lag_game_ms = 0

    @property
    def url(self) -> str:
        return self.pilot.url

    @property
    def people(self) -> dict[str, RosterEntry]:
        return self.pilot.people

    def _allowed(self, npc: str) -> bool:
        return self.pilot._allowed(npc)

    def _companion(self, npc: str) -> bool:
        return self.pilot._companion(npc)

    async def _live_target(self) -> tuple[str, str | None, str] | None:
        assert self.npc is not None
        return await self.pilot._live_target(self.npc)

    def stop(self, reason: str) -> None:
        self._set_reason(reason)
        self._acquire_cancel.set()
        self._wake.set()

    def _set_reason(self, reason: str) -> None:
        # A late RPC failure is a consequence of a world reset, not a new
        # outcome. Retain the first reset reason through subsequent callbacks.
        if self._reason not in WORLD_STOP_REASONS or reason == "disconnected":
            self._reason = reason

    def on_message(self, message: GameMessage) -> None:
        if isinstance(message, BodyControlEvents) and self.lease is not None:
            for event in message.events:
                if (
                    (event.entity_id, event.session_epoch, event.world_epoch, event.control_generation)
                    == self._identity(self.lease)
                    and event.phase == "ownership"
                    and event.reason == "native_restored"
                ):
                    self._capture_invisibility(self.clock(), event.t_game_ms)
                    self._invisible_at = self._invisible_game_ms = None
                    self._native_release_reason = event.release_reason
                    self._set_reason(
                        "target_lost" if event.release_reason == "target_visibility_lost"
                        else "native_restored"
                    )
        if self._reason is not None:
            self._acquire_cancel.set()
        self._wake.set()

    @staticmethod
    def _identity(lease: BodyLease) -> tuple[int, int, int, int]:
        return lease.entity_id, lease.session_epoch, lease.world_epoch, lease.control_generation

    def on_body_packet(self, packet: dict[str, Any]) -> None:
        received_at = self.clock()
        rows = packet["observations"]
        for row in rows:
            self._remember_down(row)
        if self.npc is None or not self._watch_ready:
            return
        if self.lease is not None and (packet["session_epoch"], packet["world_epoch"]) != (
            self.lease.session_epoch,
            self.lease.world_epoch,
        ):
            self.pilot._stop_all("world_changed")
            return
        for row in rows:
            if len(self._rows) >= 128:
                self._set_reason("observation_overflow")
                break
            self._rows.append((row, received_at))
        if self.lease is not None:
            pending = [row for row, _ in self._rows if self._leased_periodic(row)]
            if pending:
                self._max_batch_rows = max(self._max_batch_rows, len(pending))
                self._max_batch_lag_game_ms = max(
                    self._max_batch_lag_game_ms,
                    self._now_game_ms - min(row["t_game_ms"] for row in pending),
                )
        self._wake.set()

    async def run(self) -> None:
        try:
            while self.npc is not None:
                self._wake.clear()
                if self.link.closed.is_set():
                    self.pilot._stop_all("disconnected")
                if self._reason == "disconnected":
                    self.pilot._stop_all("disconnected")
                await self._tick()
                if self.npc is None:
                    break
                try:
                    await asyncio.wait_for(self._wake.wait(), timeout=0.025)
                except TimeoutError:
                    pass
        except asyncio.CancelledError:
            self._set_reason(self._reason or "shutdown")
            raise
        except Exception:
            self._set_reason("pilot_error")
            log.exception("neural-body pilot failed npc=%s", self.npc)
        finally:
            await self._finish(self._reason or "shutdown")

    async def _heartbeats(self, lease: BodyLease) -> None:
        """Renew from send time, independently of policy calls and ownership polling."""
        loop = asyncio.get_running_loop()
        deadline = loop.time()
        while self.lease == lease and not self._closing and self._reason is None:
            await asyncio.sleep(max(0.0, deadline - loop.time()))
            if self.lease != lease or self._closing or self._reason is not None:
                return
            if self.link.closed.is_set() or self.link.body_leases.get(lease.entity_id) != lease:
                self._set_reason("lease_lost")
                self._wake.set()
                return
            sent_at = loop.time()
            try:
                await self.link.body_heartbeat(lease, timeout=HEARTBEAT_TIMEOUT)
            except Exception:
                # Never let a detached keepalive task fail silently and leave the
                # main loop accepting policy decisions for an unrenewed lease.
                self._set_reason(self._reason or "lease_lost")
                self._wake.set()
                return
            deadline = sent_at + HEARTBEAT_INTERVAL

    def _policy_response_age(self) -> float | None:
        if self._policy_watchdog_at is None or self.client is None:
            return None
        last_response = self.client.last_response_at
        since = max(self._policy_watchdog_at, last_response or self._policy_watchdog_at)
        return asyncio.get_running_loop().time() - since

    async def _policy_watchdog(self) -> None:
        """A missing observation cannot hide an unresponsive policy connection."""
        assert self.client is not None
        client = self.client
        while (
            self.client is client and self._policy_watchdog_at is not None
            and not self._closing and self._reason is None
        ):
            client.response_received.clear()
            age = self._policy_response_age()
            assert age is not None
            remaining = POLICY_RESPONSE_TIMEOUT - age
            if remaining <= 0:
                self._set_reason("policy_timeout")
                self._wake.set()
                return
            try:
                await asyncio.wait_for(client.response_received.wait(), timeout=remaining)
            except TimeoutError:
                pass

    def _ownership_poll_gap_ms(self) -> float:
        if self._last_events_success_at is None:
            return 0.0
        gap_ms = (asyncio.get_running_loop().time() - self._last_events_success_at) * 1000
        self._ownership_poll_max_gap_ms = max(self._ownership_poll_max_gap_ms, gap_ms)
        return gap_ms

    async def _tick(self) -> None:
        self.pilot._drain_events()
        if self._reason:
            await self._finish(self._reason)
            return
        assert self.npc is not None
        now = self.clock()
        if not self._allowed(self.npc):
            await self._finish("npc_unavailable")
            return
        if self.lease is None and now - self._attempt_at >= ACQUIRE_WINDOW:
            await self._finish("acquire_exhausted")
            return
        if not self._watched:
            started = time.monotonic()
            try:
                symbol = await self.link.story_eval(self.npc, [["sym", self.npc]], timeout=BRIDGE_TIMEOUT)
                if len(symbol) != 1 or type(symbol[0]) is not int or symbol[0] < 0:
                    raise GameError("NPC script symbol unavailable")
                self._symbol = symbol[0]
            except BRIDGE_ERRORS as error:
                log.warning(
                    "neural-body watch setup failed stage=symbol_eval rpc=story_eval npc=%s "
                    "error_type=%s error=%r elapsed_ms=%.1f",
                    self.npc, type(error).__name__, error, (time.monotonic() - started) * 1000,
                )
                await self._finish(
                    "symbol_eval_timeout" if isinstance(error, TimeoutError) else "symbol_eval_failed"
                )
                return
            # A timed-out request may already have enabled observations on
            # the bridge. Track it before awaiting the reply so cleanup
            # also unsubscribes when that acknowledgement never arrives.
            self._watched = True
            started = time.monotonic()
            try:
                await self.pilot._watch(self)
            except BRIDGE_ERRORS as error:
                log.warning(
                    "neural-body watch setup failed stage=body_watch rpc=body_watch npc=%s "
                    "error_type=%s error=%r elapsed_ms=%.1f",
                    self.npc, type(error).__name__, error, (time.monotonic() - started) * 1000,
                )
                refused = isinstance(error, GameError) and any(
                    other is not self for other in self.pilot.fights.values()
                )
                if refused:
                    self._watched = False
                    self.pilot._reacquire_after[self.npc] = self.clock() + REACQUIRE_INTERVAL
                    self.pilot._roster_attempted.discard(self.npc)
                await self._finish(
                    "watch_refused" if refused else (
                        "body_watch_timeout" if isinstance(error, TimeoutError) else "body_watch_failed"
                    )
                )
                return
            self._watch_ready = True
        if self.lease is not None:
            if (
                self.link.body_leases.get(self.lease.entity_id) != self.lease
                or self._ownership_poll_gap_ms() > LEASE_TIMEOUT_MS
            ):
                await self._finish("lease_lost")
                return
            if now - self._fight_at >= MAX_FIGHT_SECONDS or (
                self._initial_game_ms is not None and self._now_game_ms - self._initial_game_ms >= 300_000
            ):
                await self._finish("fight_timeout")
                return
            loop = asyncio.get_running_loop()
            if loop.time() - self._last_events_poll >= EVENTS_INTERVAL:
                poll_timed_out = False
                try:
                    self._last_events_poll = loop.time()
                    self.pilot.on_message(await self.link.body_events(timeout=BRIDGE_TIMEOUT))
                except TimeoutError as error:
                    poll_timed_out = True
                    self._ownership_poll_timeouts += 1
                    # Wait the usual interval after a timeout instead of immediately
                    # retrying an RPC that already took longer than EVENTS_INTERVAL.
                    self._last_events_poll = loop.time()
                    poll_log = log.info if self._reason in {"loading", "world_changed"} else log.warning
                    poll_log(
                        "neural-body ownership poll timeout npc=%s rpc=body_events "
                        "error_type=%s error=%r ownership_poll_timeouts=%d ownership_poll_gap_ms=%.1f",
                        self.npc, type(error).__name__, error, self._ownership_poll_timeouts,
                        self._ownership_poll_gap_ms(),
                    )
                except BRIDGE_ERRORS as error:
                    log.warning(
                        "neural-body ownership poll failed npc=%s rpc=body_events error_type=%s error=%r",
                        self.npc, type(error).__name__, error,
                    )
                    await self._finish("lease_lost")
                    return
                if (
                    self._reason or self.link.body_leases.get(self.lease.entity_id) != self.lease
                    or self._ownership_poll_gap_ms() > LEASE_TIMEOUT_MS
                ):
                    await self._finish(self._reason or "lease_lost")
                    return
                if poll_timed_out:
                    return
                self._last_events_success_at = loop.time()
        while self._rows and self.npc is not None:
            row, received_at = self._rows.popleft()
            await self._row(row, received_at)
            if self._reason:
                await self._finish(self._reason)
                break
            if (
                self.lease is not None
                and asyncio.get_running_loop().time() - self._last_events_poll >= EVENTS_INTERVAL
            ):
                # Keep ownership polling responsive under a continuous row stream.
                self._wake.set()
                break

    def _is_leased_target(self, row: dict[str, Any]) -> bool:
        return self.lease is None or row["target"]["entity_id"] == self.lease.target_entity_id

    def _leased_periodic(self, row: dict[str, Any]) -> bool:
        return bool(
            self.lease is not None and row["periodic"] and row["script_symbol"] == self._symbol
            and (row["entity_id"], row["session_epoch"], row["world_epoch"], row["control_generation"])
            == self._identity(self.lease)
            and row["observation_seq"] > self._last_seq
        )

    def _lagging(self, row: dict[str, Any]) -> bool:
        return (
            self._now_game_ms - row["t_game_ms"] > MAX_OBSERVATION_AGE_GAME_MS
            or any(
                self._leased_periodic(pending) and pending["observation_seq"] > row["observation_seq"]
                for pending, _ in self._rows
            )
        )

    async def _reset_policy(self, initial_game_ms: int) -> None:
        assert self.client is not None and self.lease is not None
        result = await self.client.call(
            "reset", session_epoch=self.lease.session_epoch, world_epoch=self.lease.world_epoch,
            initial_game_ms=initial_game_ms, include_capabilities=True,
        )
        self._memory_only_step = result.get("memory_only_step") is True
        self._policy_needs_reset = False

    def _capture_invisibility(self, now: float, game_ms: int) -> None:
        if self._invisible_at is not None:
            assert self._invisible_game_ms is not None
            self._invisible_last_seconds = max(0.0, now - self._invisible_at)
            self._invisible_last_game_ms = max(0, game_ms - self._invisible_game_ms)
            self._invisible_seconds_max = max(self._invisible_seconds_max, self._invisible_last_seconds)
            self._invisible_game_ms_max = max(
                self._invisible_game_ms_max, self._invisible_last_game_ms,
            )

    def _terminal(self, row: dict[str, Any]) -> str | None:
        own, target = row["self"], row["target"]
        now = self.clock()
        if not target["visible"]:
            if self._invisible_at is None:
                self._invisible_at, self._invisible_game_ms = now, row["t_game_ms"]
            self._capture_invisibility(now, row["t_game_ms"])
        else:
            self._capture_invisibility(now, row["t_game_ms"])
            self._invisible_at = self._invisible_game_ms = None
        if own["dead"] or own["unconscious"] or own["down"] or own["hp"] <= 0:
            return "npc_down"
        if target["entity_id"] and not self._is_leased_target(row):
            return "target_changed"
        if not target["present"] or not target["current_valid"]:
            if self._target_absent_game_ms is None:
                self._target_absent_game_ms = row["t_game_ms"]
            if row["t_game_ms"] - self._target_absent_game_ms >= TARGET_ABSENCE_TIMEOUT_MS:
                return "target_lost"
        else:
            self._target_absent_game_ms = None
        if target["current_valid"] and (target["down"] or target["hp"] is None or target["hp"] <= 0):
            return "target_down" if self._target_kind == "companion" else "hero_down"
        if own["weapon_state"] != 2:
            return "weapon_changed"
        return None

    async def _row(self, raw: dict[str, Any], received_at: float) -> None:
        if raw["script_symbol"] != self._symbol or not raw["periodic"]:
            return
        if self._entity is not None and raw["entity_id"] != self._entity:
            return
        if (raw["session_epoch"], raw["world_epoch"]) != (
            self.link.body_session_epoch,
            self.link.body_generation,
        ):
            return
        if raw["observation_seq"] <= self._last_seq:
            return
        if self.lease is not None and (
            raw["entity_id"],
            raw["session_epoch"],
            raw["world_epoch"],
            raw["control_generation"],
        ) != self._identity(self.lease):
            return
        self._last_seq = raw["observation_seq"]
        if self.lease is not None:
            terminal = self._terminal(raw)
            if terminal:
                if terminal in {"hero_down", "target_down"} and not self._is_leased_target(raw):
                    return
                self._set_reason(terminal)
                return
        else:
            if self.clock() - received_at > 0.100 or self._now_game_ms - raw["t_game_ms"] > 100:
                return
            terminal = self._terminal(raw)
            if terminal in {"npc_down", "target_lost"}:
                self._set_reason(terminal)
                return
            # Before acquisition there is no confirmed numeric target identity.
            # A later live-target query can describe a new, healthy target, so
            # an old down row must not close or claim this observing episode.
            if terminal or not all(
                raw["target"][key] for key in ("present", "visible", "current_valid")
            ):
                return
        observation = self.link.body_latest.get(raw["entity_id"])
        if observation is None or observation.observation_seq != raw["observation_seq"]:
            # The exact typed row is only needed at the bridge API boundary.
            observation = BodyObservation.model_validate(raw)
        if self.lease is None:
            if self.clock() - self._last_acquire < ACQUIRE_INTERVAL:
                return
            self._last_acquire = self.clock()
            try:
                # NPC_GETTARGET sets OTHER to the actual current target; story_eval
                # restores SELF/OTHER afterward. Acquire independently checks that
                # the observation's numeric target still matches this live target.
                target = await self._live_target()
                if target is None:
                    self._refusals["target_not_allowed"] += 1
                    if self._retargeting:
                        self._claimed_fights.add(self.npc)
                        self._set_reason("target_not_allowed")
                    return
                if self._reason or self._closing or not self._allowed(self.npc):
                    return
                if self.clock() - received_at > 0.100 or self.clock() - self._attempt_at >= ACQUIRE_WINDOW:
                    return
                self._acquiring = True
                self._pending_acquire = observation
                requested_at = self.clock()
                self._acquire_requests += 1
                try:
                    self.lease = await self.link.body_acquire(
                        self.npc,
                        observation,
                        "neural-body",
                        mode="realtime",
                        timeout_ms=LEASE_TIMEOUT_MS,
                        await_boundary_ms=min(
                            AWAIT_BOUNDARY_MS,
                            max(0, int((ACQUIRE_WINDOW - (requested_at - self._attempt_at)) * 1000)),
                        ),
                        timeout=BRIDGE_TIMEOUT,
                        cancel_event=self._acquire_cancel,
                    )
                except (TimeoutError, OSError, ConnectionClosed):
                    # The bridge may own the body even though no receipt arrived.
                    self._set_reason(self._reason or "acquire_uncertain")
                    raise
                except GameError:
                    recovered = self.link.body_leases.get(observation.entity_id)
                    if recovered is not None and recovered.provider == "neural-body":
                        self.lease = recovered
                        self._claimed_fights.add(self.npc)
                    if self.link.body_acquire_pending(observation.entity_id):
                        self._set_reason(self._reason or "acquire_uncertain")
                    else:
                        self._pending_acquire = None
                    raise
                else:
                    self._pending_acquire = None
                finally:
                    self._acquiring = False
            except (GameDisconnected, ConnectionClosed):
                self.pilot._stop_all("disconnected")
            except BRIDGE_ERRORS as error:
                self._refusals[str(error).removeprefix("body command rejected: ")] += 1
            else:
                self._acquire_completed += 1
                self._acquire_waits.append((self.clock() - requested_at) * 1000)
                self._claimed_fights.add(self.npc)
                self._reacquire_after[self.npc] = self.clock() + REACQUIRE_INTERVAL
                self._entity = self.lease.entity_id
                self._hero = self.lease.target_entity_id
                self._target_kind, self._target_name, self._target_npc = target
                self._observed_down = None
                self._fight_at = self.clock()
                self._last_events_poll = float("-inf")
                # Acquisition starts the initial grace period before the first poll.
                self._last_events_success_at = asyncio.get_running_loop().time()
                self._heartbeat_task = asyncio.create_task(self._heartbeats(self.lease))
                log.info(
                    "neural-body start npc=%s target_kind=%s target_name=%s lease=%s acquire_refusals=%s",
                    self.npc,
                    self._target_kind,
                    self._target_name,
                    self._identity(self.lease),
                    dict(self._refusals),
                )
                # Establish the episode while waiting for the first leased row.
                # The independent heartbeat already protects the lease during TCP
                # connection and reset, so neither delays that row's first step.
                self._initial_game_ms = observation.t_game_ms
                if self._closing or self._reason:
                    return
                try:
                    self.client = PolicyClient(self.url)
                    await self.client.connect()
                    await self._reset_policy(self._initial_game_ms)
                except TimeoutError:
                    self._set_reason("policy_timeout")
                except (OSError, ValueError, PolicyError):
                    self._set_reason("policy_error")
            return
        if raw["observation_seq"] <= self.lease.acquired_observation_seq:
            return
        try:
            assert self.client is not None
            if self._policy_needs_reset:
                try:
                    await self._reset_policy(raw["t_game_ms"])
                except TimeoutError:
                    self._set_reason("policy_timeout")
                    return
            memory_only = self._lagging(raw)
            if memory_only:
                self._memory_only_lag_rows += 1
            started = self.clock()
            if self._policy_watchdog_at is None:
                self._policy_watchdog_at = asyncio.get_running_loop().time()
                self._policy_watchdog_task = asyncio.create_task(self._policy_watchdog())
            self._packet_to_request.append((started - received_at) * 1000)
            try:
                fields = {"memory_only": True} if memory_only and self._memory_only_step else {}
                result = await self.client.call(
                    "step", observation=raw, now_game_ms=self._now_game_ms, **fields,
                )
            except TimeoutError:
                self._policy_timeouts += 1
                self._policy_timeouts_consecutive += 1
                self._policy_timeout_waits.append((self.clock() - started) * 1000)
                if self._policy_timeouts_consecutive >= POLICY_TIMEOUT_LIMIT:
                    self._set_reason("policy_timeout")
                return
            finally:
                responded_at = self.clock()
                self._latencies.append((responded_at - started) * 1000)
            self._policy_timeouts_consecutive = 0
            self._policy_watchdog_at = None
            assert self._policy_watchdog_task is not None
            self._policy_watchdog_task.cancel()
            self._policy_watchdog_task = None
            self._packet_to_response.append((responded_at - received_at) * 1000)
            if result.get("status") == "fallback":
                self._fallbacks += 1
                if result.get("reason") == "observation_too_old":
                    self._observation_too_old += 1
                    if result.get("process", {}).get("hidden_reset") is True:
                        self._policy_memory_resets += 1
                        self._policy_needs_reset = True
                    self._transient_rejections_consecutive += 1
                    if self._transient_rejections_consecutive < TRANSIENT_REJECTION_LIMIT:
                        return
                self._set_reason(f"policy_fallback:{result.get('reason')}")
                return
            if self._closing or self._reason or not self._allowed(self.npc):
                self._set_reason(self._reason or "npc_unavailable")
                return
            if self.link.body_leases.get(self.lease.entity_id) != self.lease:
                self._set_reason("lease_lost")
                return
            if (
                not memory_only and not self._lagging(raw)
                and result.get("status") == "decision" and raw["can_decide"] and raw["action_mask_valid"]
            ):
                action = result.get("action_id")
                if type(action) is not int or not 0 <= action <= 11 or not raw["action_mask"][action]:
                    raise PolicyError("invalid policy action")
                self._policy_decisions.append(PolicyDecisionTrace(
                    observation_seq=observation.observation_seq,
                    t_game_ms=observation.t_game_ms,
                    action_id=action,
                    sent=False,
                    accepted=False,
                    reason="observation_superseded",
                ))
                latest = self.link.body_latest.get(self.lease.entity_id)
                if latest is None or latest.observation_seq != observation.observation_seq:
                    return
                # Record the send boundary before awaiting its acknowledgement;
                # bridge reply latency is not server decision-processing time.
                sent_at = self.clock()
                self._response_to_action.append((sent_at - responded_at) * 1000)
                self._packet_to_action.append((sent_at - received_at) * 1000)
                try:
                    await self.link.body_action(
                        self.lease, observation, action, ttl_ms=100, timeout=BRIDGE_TIMEOUT
                    )
                except BRIDGE_ERRORS as error:
                    reason = str(error).removeprefix("body command rejected: ") or type(error).__name__
                    self._policy_decisions[-1] = PolicyDecisionTrace(
                        observation.observation_seq, observation.t_game_ms, action, True, False, reason,
                    )
                    self._action_rejections[reason] += 1
                    if isinstance(error, GameError) and reason in TRANSIENT_ACTION_REJECTIONS:
                        self._transient_rejections_consecutive += 1
                        if self._transient_rejections_consecutive >= TRANSIENT_REJECTION_LIMIT:
                            self._set_reason("body_action_rejected")
                        return
                    raise
                self._policy_decisions[-1] = PolicyDecisionTrace(
                    observation.observation_seq, observation.t_game_ms, action, True, True, "accepted",
                )
                self._transient_rejections_consecutive = 0
                self._decisions += 1
        except (OSError, ValueError, PolicyError):
            self._set_reason("policy_error")
        except BRIDGE_ERRORS:
            self._set_reason("body_action_rejected")

    async def _recover_acquire(self) -> bool:
        """Resolve an uncertain acquire before releasing or changing the watch."""
        pending = self._pending_acquire
        assert pending is not None and self.npc is not None
        scope = pending.session_epoch, pending.world_epoch
        while not self.link.closed.is_set():
            if (self.link.body_session_epoch, self.link.body_generation) != scope:
                self._pending_acquire = None
                return True
            try:
                # Native status alone cannot prove a queued request was cancelled.
                # Resolve its request identity before changing the observation watch.
                await self.link.body_cancel_acquire(pending.entity_id, timeout=BRIDGE_TIMEOUT)
                status = await self.link.body_control_status(timeout=BRIDGE_TIMEOUT)
            except BRIDGE_ERRORS as error:
                if "unknown_entity" in str(error):
                    self._pending_acquire = None
                    return True
            else:
                if self.link.closed.is_set() or (
                    self.link.body_session_epoch, self.link.body_generation
                ) != scope:
                    self._pending_acquire = None
                    return True
                if (status.session_epoch, status.world_epoch) == scope:
                    actor = next((row for row in status.actors if row.entity_id == pending.entity_id), None)
                    if actor is None or actor.owner == "native":
                        self._pending_acquire = None
                        return True
                    if actor.provider == "neural-body" and actor.mode == "realtime":
                        self.lease = BodyLease(
                            npc=self.npc, provider=actor.provider, mode=actor.mode,
                            session_epoch=actor.session_epoch, world_epoch=actor.world_epoch,
                            entity_id=actor.entity_id, control_generation=actor.control_generation,
                            goal_version=actor.goal_version, target_entity_id=actor.target_entity_id,
                            acquired_observation_seq=pending.observation_seq,
                        )
                        self.link.body_leases[self.lease.entity_id] = self.lease
                        self._claimed_fights.add(self.npc)
                        self._pending_acquire = None
                        return True
            await asyncio.sleep(0.025)
        self._pending_acquire = None
        return True

    async def _finish_status(self, lease: BodyLease) -> bool:
        try:
            status = await self.link.body_control_status(timeout=BRIDGE_TIMEOUT)
        except GameError as error:
            if "unknown_entity" not in str(error):
                raise
        else:
            if (status.session_epoch, status.world_epoch) != (lease.session_epoch, lease.world_epoch):
                return False
            if any(actor.entity_id == lease.entity_id for actor in status.actors):
                return self.link.body_leases.get(lease.entity_id) != lease
        # Removed actors cannot send native_restored or appear in a status row.
        if self.link.body_leases.get(lease.entity_id) == lease:
            self.link.body_leases.pop(lease.entity_id)
        return True

    def _remember_down(self, row: dict[str, Any]) -> None:
        lease = self.lease
        if lease is None or (
            row["entity_id"], row["session_epoch"], row["world_epoch"]
        ) != (lease.entity_id, lease.session_epoch, lease.world_epoch) or (
            row["control_generation"] not in {0, lease.control_generation}
            or row["observation_seq"] <= lease.acquired_observation_seq
        ):
            return
        # Native/event-only rows do not drive policy steps, but a later empty
        # target must not erase their evidence of this lease's fallen actor.
        own, target = row["self"], row["target"]
        if own["dead"] or own["unconscious"] or own["down"] or own["hp"] <= 0:
            self._observed_down = "npc_down"
        elif (
            self._observed_down is None and target["entity_id"] == lease.target_entity_id
            and target["current_valid"]
            and (target["down"] or target["hp"] is None or target["hp"] <= 0)
        ):
            self._observed_down = "target_down" if self._target_kind == "companion" else "hero_down"

    def _latest_terminal(self, lease: BodyLease) -> str | None:
        if self.lease == lease and self._observed_down is not None:
            return self._observed_down
        latest = self.link.body_latest.get(lease.entity_id)
        if latest is not None and latest.observation_seq > lease.acquired_observation_seq and (
            latest.session_epoch, latest.world_epoch, latest.control_generation
        ) in {
            (lease.session_epoch, lease.world_epoch, 0),
            (lease.session_epoch, lease.world_epoch, lease.control_generation),
        }:
            terminal = self._terminal(latest.model_dump(mode="json"))
            if terminal in {"target_changed", "hero_down", "target_down", "npc_down"}:
                return terminal
        return None

    async def _finish(self, reason: str) -> None:
        self.pilot._drain_events()
        if self._reason in WORLD_STOP_REASONS:
            reason = self._reason
        if reason in {"lease_lost", "native_restored"} and self._reason == "target_lost":
            reason = "target_lost"
        npc, client = self.npc, self.client
        if npc is None and self.lease is None and client is None:
            return
        self._ownership_poll_gap_ms()
        native_reasons = {"native_restored", "lease_lost"}
        if (
            reason == "body_action_rejected" and self._policy_decisions
            and self._policy_decisions[-1].reason in {"not_owned", "release_pending"}
        ):
            native_reasons.add(reason)
        observed_terminal = (
            self._latest_terminal(self.lease)
            if reason in native_reasons and self.lease is not None else None
        )
        if reason == "native_restored" and observed_terminal is not None:
            reason = observed_terminal
        # Native setTarget defers its mutation until after native_restored, so
        # no owned observation of the new target is guaranteed to reach us.
        if (
            reason == "native_restored"
            and self._native_release_reason in {"native_script_takeover", "target_changed_or_lost"}
            and npc is not None and self._allowed(npc) and not self._closing
        ):
            with contextlib.suppress(*BRIDGE_ERRORS):
                target = await self._live_target()
                if target is not None and target[2] != self._target_npc:
                    reason = "target_changed"
        finish_at = time.monotonic()
        release_deadline = self.clock() + LEASE_TIMEOUT_MS / 1000 + RELEASE_ORPHAN_GRACE
        end_trigger = reason
        self._capture_invisibility(self.clock(), self._now_game_ms)
        fight_seconds = self.clock() - self._fight_at if self.lease is not None else None
        policy_response_age = self._policy_response_age()
        release_confirmed = watch_removed = False
        release_seconds = None
        self._watch_ready = False
        unconfirmed = "acquire_status" if self._pending_acquire is not None else "body_release"
        lease = self.lease
        heartbeat, self._heartbeat_task = self._heartbeat_task, None
        policy_watchdog, self._policy_watchdog_task = self._policy_watchdog_task, None
        try:
            # One wall-clock budget covers every route into ownership cleanup.
            async with asyncio.timeout(FINISH_TIMEOUT):
                if heartbeat is not None:
                    heartbeat.cancel()
                    await asyncio.gather(heartbeat, return_exceptions=True)
                if policy_watchdog is not None:
                    policy_watchdog.cancel()
                    await asyncio.gather(policy_watchdog, return_exceptions=True)
                if self._pending_acquire is not None:
                    await self._recover_acquire()
                lease = self.lease
                if lease is not None:
                    try:
                        await self.link.body_release(lease, timeout=BRIDGE_TIMEOUT)
                    except BRIDGE_ERRORS as error:
                        unconfirmed = f"body_release:{error}"
                while lease is not None and self.link.body_leases.get(lease.entity_id) == lease:
                    if self.link.closed.is_set():
                        break
                    try:
                        self.pilot.on_message(await self.link.body_events(timeout=BRIDGE_TIMEOUT))
                    except BRIDGE_ERRORS:
                        pass
                    if self.link.body_leases.get(lease.entity_id) != lease:
                        break
                    try:
                        if await self._finish_status(lease):
                            break
                        unconfirmed = "native_restored"
                    except BRIDGE_ERRORS as error:
                        unconfirmed = f"body_control_status:{error}"
                    await asyncio.sleep(0.025)
                if (
                    reason in native_reasons and lease is not None
                    and self._native_release_reason is None and not self.link.closed.is_set()
                    and self.link.body_leases.get(lease.entity_id) != lease
                    and (self.link.body_session_epoch, self.link.body_generation)
                    == (lease.session_epoch, lease.world_epoch)
                ):
                    # A status/release acknowledgement can confirm native ownership
                    # after the last event poll. Drain its reason before unwatch discards it.
                    with contextlib.suppress(*BRIDGE_ERRORS):
                        events = await self.link.body_events(timeout=BRIDGE_TIMEOUT)
                        if (self.link.body_session_epoch, self.link.body_generation) == (
                            lease.session_epoch, lease.world_epoch,
                        ):
                            self.pilot.on_message(events)
                if (
                    reason in native_reasons and lease is not None
                    and self._native_release_reason is not None and not self.link.closed.is_set()
                    and self.link.body_leases.get(lease.entity_id) != lease
                ):
                    # A heartbeat or action can discover native ownership before
                    # the destructive event poll receives its matching restoration.
                    observed_terminal = observed_terminal or self._latest_terminal(lease)
                    if observed_terminal is not None:
                        reason = observed_terminal
                    elif (
                        reason != "native_restored"
                        and self._native_release_reason in {
                            "native_script_takeover", "target_changed_or_lost"
                        }
                        and npc is not None and self._allowed(npc) and not self._closing
                    ):
                        with contextlib.suppress(*BRIDGE_ERRORS):
                            target = await self._live_target()
                            if target is not None and target[2] != self._target_npc:
                                reason = "target_changed"
        except TimeoutError:
            end_trigger = reason
            reason = "finish_deadline"
            # Make a final authoritative check; an unconfirmed lease stays watched.
            lease = self.lease
            if lease is not None:
                with contextlib.suppress(*BRIDGE_ERRORS):
                    async with asyncio.timeout(BRIDGE_TIMEOUT):
                        await self._finish_status(lease)
        finally:
            if reason != "finish_deadline" and self._reason in WORLD_STOP_REASONS:
                reason = self._reason
            release_confirmed = bool(
                lease is not None
                and reason != "disconnected"
                and not self.link.closed.is_set()
                and self.link.body_leases.get(lease.entity_id) != lease
            )
            if release_confirmed:
                release_seconds = time.monotonic() - finish_at
            self._pending_acquire = None
            if client is not None:
                try:
                    if lease is not None:
                        with contextlib.suppress(OSError, ValueError, PolicyError, TimeoutError):
                            await client.call(
                                "end_episode",
                                session_epoch=lease.session_epoch,
                                world_epoch=lease.world_epoch,
                            )
                    with contextlib.suppress(OSError, ValueError, PolicyError, TimeoutError):
                        await client.call("close")
                finally:
                    with contextlib.suppress(OSError, TimeoutError):
                        async with asyncio.timeout(SERVICE_TIMEOUT):
                            await client.close()
            if (
                reason in native_reasons and release_confirmed
                and self._native_release_reason is not None
            ):
                observed_terminal = observed_terminal or self._latest_terminal(lease)
                if observed_terminal is not None:
                    reason = observed_terminal
            # Another fight's poll can deliver native_restored while the policy
            # episode is closing. Recheck before unwatch and orphan registration.
            if not release_confirmed and (
                lease is not None and reason != "disconnected"
                and not self.link.closed.is_set()
                and self.link.body_leases.get(lease.entity_id) != lease
            ):
                release_confirmed = True
                release_seconds = time.monotonic() - finish_at
            if self._watched and (
                release_confirmed or (lease is None and self._pending_acquire is None)
                or not self.link.body_leases
            ):
                with contextlib.suppress(*BRIDGE_ERRORS):
                    async with asyncio.timeout(BRIDGE_TIMEOUT):
                        watch_removed = await self.pilot._unwatch(self)
        summary = FightSummary(
            npc=npc,
            lease=lease,
            target_kind=self._target_kind,
            target_name=self._target_name,
            reason=reason,
            decisions=self._decisions,
            fallbacks=self._fallbacks,
            memory_only_lag_rows=self._memory_only_lag_rows,
            observation_too_old=self._observation_too_old,
            policy_memory_resets=self._policy_memory_resets,
            max_batch_rows=self._max_batch_rows,
            max_batch_lag_game_ms=self._max_batch_lag_game_ms,
            service_p50_ms=_percentile(self._latencies, 0.50),
            service_p95_ms=_percentile(self._latencies, 0.95),
            packet_to_request_p50_ms=_percentile(self._packet_to_request, 0.50),
            packet_to_request_p95_ms=_percentile(self._packet_to_request, 0.95),
            packet_to_response_p50_ms=_percentile(self._packet_to_response, 0.50),
            packet_to_response_p95_ms=_percentile(self._packet_to_response, 0.95),
            response_to_action_p50_ms=_percentile(self._response_to_action, 0.50),
            response_to_action_p95_ms=_percentile(self._response_to_action, 0.95),
            packet_to_action_p50_ms=_percentile(self._packet_to_action, 0.50),
            packet_to_action_p95_ms=_percentile(self._packet_to_action, 0.95),
            acquire_refusals=tuple(sorted(self._refusals.items())),
            acquire_requests=self._acquire_requests,
            acquire_completed=self._acquire_completed,
            acquire_wait_p50_ms=_percentile(self._acquire_waits, 0.50),
            acquire_wait_max_ms=max(self._acquire_waits, default=None),
            action_rejections=tuple(sorted(self._action_rejections.items())),
            transient_rejections_consecutive=self._transient_rejections_consecutive,
            policy_timeouts=self._policy_timeouts,
            policy_timeouts_consecutive=self._policy_timeouts_consecutive,
            policy_timeout_wait_p95_ms=_percentile(self._policy_timeout_waits, 0.95),
            policy_response_silence_ms=(
                policy_response_age * 1000 if policy_response_age is not None else None
            ),
            ownership_poll_timeouts=self._ownership_poll_timeouts,
            ownership_poll_max_gap_ms=self._ownership_poll_max_gap_ms,
            target_invisible_seconds=self._invisible_seconds_max,
            target_invisible_game_ms=self._invisible_game_ms_max,
            target_invisible_last_seconds=self._invisible_last_seconds,
            target_invisible_last_game_ms=self._invisible_last_game_ms,
            native_release_reason=self._native_release_reason,
            last_policy_decisions=tuple(self._policy_decisions),
            fight_seconds=fight_seconds,
            release_confirmed=release_confirmed,
            release_seconds=release_seconds,
            watch_removed=watch_removed,
            end_trigger=end_trigger if reason == "finish_deadline" else reason,
            release_late_seconds=release_seconds if reason == "finish_deadline" else None,
        )
        log.info(
            "neural-body end npc=%s target_kind=%s target_name=%s reason=%s end_trigger=%s "
            "release_late_seconds=%s "
            "decisions=%d fallback=%d memory_only_lag_rows=%d observation_too_old=%d "
            "policy_memory_resets=%d max_batch_rows=%d max_batch_lag_game_ms=%d service_p50_ms=%s "
            "service_p95_ms=%s packet_to_request_p50_ms=%s packet_to_request_p95_ms=%s "
            "packet_to_response_p50_ms=%s packet_to_response_p95_ms=%s "
            "response_to_action_p50_ms=%s response_to_action_p95_ms=%s "
            "packet_to_action_p50_ms=%s packet_to_action_p95_ms=%s "
            "acquire_refusals=%s acquire_requests=%d acquire_completed=%d "
            "acquire_wait_p50_ms=%s acquire_wait_max_ms=%s "
            "action_rejections=%s transient_rejections_consecutive=%d "
            "policy_timeouts=%d policy_timeouts_consecutive=%d policy_timeout_wait_p95_ms=%s "
            "policy_response_silence_ms=%s ownership_poll_timeouts=%d "
            "ownership_poll_max_gap_ms=%.1f unconfirmed=%s",
            npc,
            summary.target_kind,
            summary.target_name,
            reason,
            summary.end_trigger,
            summary.release_late_seconds,
            self._decisions,
            self._fallbacks,
            summary.memory_only_lag_rows,
            summary.observation_too_old,
            summary.policy_memory_resets,
            summary.max_batch_rows,
            summary.max_batch_lag_game_ms,
            summary.service_p50_ms,
            summary.service_p95_ms,
            summary.packet_to_request_p50_ms,
            summary.packet_to_request_p95_ms,
            summary.packet_to_response_p50_ms,
            summary.packet_to_response_p95_ms,
            summary.response_to_action_p50_ms,
            summary.response_to_action_p95_ms,
            summary.packet_to_action_p50_ms,
            summary.packet_to_action_p95_ms,
            dict(summary.acquire_refusals),
            summary.acquire_requests,
            summary.acquire_completed,
            summary.acquire_wait_p50_ms,
            summary.acquire_wait_max_ms,
            dict(summary.action_rejections),
            summary.transient_rejections_consecutive,
            summary.policy_timeouts,
            summary.policy_timeouts_consecutive,
            summary.policy_timeout_wait_p95_ms,
            summary.policy_response_silence_ms,
            summary.ownership_poll_timeouts,
            summary.ownership_poll_max_gap_ms,
            unconfirmed if reason == "finish_deadline" else None,
        )
        log.info(
            "neural-body target diagnostics invisible_max_seconds=%s invisible_max_game_ms=%d "
            "invisible_last_seconds=%s invisible_last_game_ms=%d "
            "native_release_reason=%s release_confirmed=%s last_policy_decisions=%s",
            summary.target_invisible_seconds,
            summary.target_invisible_game_ms,
            summary.target_invisible_last_seconds,
            summary.target_invisible_last_game_ms,
            summary.native_release_reason,
            summary.release_confirmed,
            summary.last_policy_decisions,
        )
        if (
            reason == "target_changed" and release_confirmed
            and not self._closing and self._reason in {None, "target_changed", "native_restored"}
            and npc is not None
            and self._allowed(npc)
        ):
            with contextlib.suppress(*BRIDGE_ERRORS):
                if await self._live_target() is not None:
                    self._claimed_fights.discard(npc)
                    self._retarget_pending.add(npc)
        self.npc = self.lease = self.client = None
        self._watched = self._watch_ready = False
        self._reason = None
        self._rows.clear()
        self._last_seq = self._now_game_ms = 0
        self._last_acquire = float("-inf")
        self._initial_game_ms = None
        self._symbol = self._entity = self._hero = None
        self._target_kind = self._target_name = None
        self._target_npc = None
        self._observed_down = None
        self._retargeting = False
        self._refusals.clear()
        self._acquire_requests = self._acquire_completed = 0
        self._acquire_waits.clear()
        self._action_rejections.clear()
        self._transient_rejections_consecutive = 0
        self._policy_timeouts = self._policy_timeouts_consecutive = 0
        self._policy_timeout_waits.clear()
        self._last_events_success_at = None
        self._ownership_poll_timeouts = 0
        self._ownership_poll_max_gap_ms = 0.0
        self._target_absent_game_ms = self._invisible_at = self._invisible_game_ms = None
        self._invisible_seconds_max = 0.0
        self._invisible_game_ms_max = 0
        self._invisible_last_seconds = 0.0
        self._invisible_last_game_ms = 0
        self._native_release_reason = None
        self._policy_decisions.clear()
        self._policy_watchdog_at = None
        self._latencies.clear()
        self._packet_to_request.clear()
        self._packet_to_response.clear()
        self._response_to_action.clear()
        self._packet_to_action.clear()
        self._decisions = self._fallbacks = 0
        self._memory_only_step = self._policy_needs_reset = False
        self._memory_only_lag_rows = self._observation_too_old = self._policy_memory_resets = 0
        self._max_batch_rows = self._max_batch_lag_game_ms = 0
        self.pilot._completed(self, summary, finish_at, release_deadline)
