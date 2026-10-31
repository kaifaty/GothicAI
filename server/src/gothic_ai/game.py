"""Connection to the game: one OpenGothic client (ai_bridge) at a time."""

from __future__ import annotations

import asyncio
import itertools
import json
import logging
import secrets
from collections import deque
from collections.abc import Awaitable, Callable, Iterable
from dataclasses import dataclass
from http import HTTPStatus
from typing import Any, Literal

from pydantic import ValidationError
from websockets.asyncio.server import ServerConnection, serve
from websockets.exceptions import ConnectionClosed
from websockets.http11 import Request, Response

from .protocol import (
    PROTOCOL_VERSION,
    ActionCmd,
    ActionResult,
    ActionType,
    AttachCmd,
    BeastCmd,
    BeastDo,
    BodyAcquireCmd,
    BodyActionCmd,
    BodyCancelAcquireCmd,
    BodyControlActor,
    BodyControlEvent,
    BodyControlEvents,
    BodyControlEventsCmd,
    BodyControlResult,
    BodyControlStatus,
    BodyControlStatusCmd,
    BodyDisconnectCmd,
    BodyGoalCmd,
    BodyHeartbeatCmd,
    BodyLease,
    BodyMode,
    BodyObservation,
    BodyObservationPacket,
    BodyObservations,
    BodyReleaseCmd,
    BodyWatchCmd,
    Chest,
    DebugCmd,
    DebugCommand,
    DetachCmd,
    Detached,
    EndCard,
    GameMessage,
    GraphPatchCmd,
    Hello,
    Loading,
    NpcActCmd,
    NpcActResult,
    NpcActType,
    NpcChestsCmd,
    NpcRoster,
    NpcSpec,
    NpcView,
    NpcViewCmd,
    PingCmd,
    PlayerPoseCmd,
    PlayerState,
    Pong,
    RaiseDeadCmd,
    Result,
    RoutineCmd,
    RoutineInfo,
    Session,
    Snapshot,
    SnapshotCmd,
    StoryEffectCmd,
    StoryEvalCmd,
    StoryNearbyCmd,
    StoryNpcCmd,
    StoryPeopleCmd,
    StoryProgramCmd,
    StorySpawnCmd,
    StoryStateCmd,
    StoryTagCmd,
    StoryWatchCmd,
    WorldLoaded,
    _Command,
    parse_game_message,
)

log = logging.getLogger(__name__)

# the game's answer to a story effect on an npc whose body P30 owns (aibridge.cpp, story_effect): it lets go
# at the next safe boundary, the effect is sent again
BODY_RELEASING = "body control is releasing"
STORY_EFFECT_RETRY = 0.05

# requests answered with `result`
_Request = (
    AttachCmd
    | PlayerPoseCmd
    | BeastCmd
    | BodyAcquireCmd
    | BodyActionCmd
    | BodyCancelAcquireCmd
    | BodyGoalCmd
    | BodyHeartbeatCmd
    | BodyReleaseCmd
    | BodyControlEventsCmd
    | BodyControlStatusCmd
    | BodyDisconnectCmd
    | BodyWatchCmd
    | DetachCmd
    | DebugCmd
    | GraphPatchCmd
    | StorySpawnCmd
    | StoryNpcCmd
    | StoryEffectCmd
    | RaiseDeadCmd
    | StoryNearbyCmd
    | NpcChestsCmd
    | StoryStateCmd
    | StoryEvalCmd
    | StoryProgramCmd
    | StoryTagCmd
    | StoryPeopleCmd
)

Listener = Callable[[GameMessage], None]
RawBodyListener = Callable[[dict[str, Any]], None]


class GameError(RuntimeError):
    """The game rejected a request."""


class GameDisconnected(GameError):
    """The game connection closed while a request was pending."""


@dataclass
class _PendingBodyAcquire:
    cmd: BodyAcquireCmd
    npc: str
    observation_seq: int
    future: asyncio.Future[BodyControlEvent]
    restored: BodyControlEvent | None = None

    def was_restored(self, generation: int) -> bool:
        return self.restored is not None and self.restored.control_generation == generation

    def has_grant(self) -> bool:
        if not self.future.done() or self.future.cancelled() or self.future.exception() is not None:
            return False
        event = self.future.result()
        return event.reason == "acquired" and not self.was_restored(event.control_generation)


class GameLink:
    """A connected game. Requests are matched to replies by `req`/`id`."""

    def __init__(self, conn: ServerConnection) -> None:
        self._conn = conn
        self._ids = itertools.count(1)
        self._requests: dict[str, asyncio.Future[Result]] = {}
        self._actions: dict[str, asyncio.Future[ActionResult]] = {}
        self._pongs: dict[str, asyncio.Future[Pong]] = {}
        self._snapshot_waiters: dict[str, list[asyncio.Future[Snapshot]]] = {}
        self._routines: dict[str, asyncio.Future[RoutineInfo]] = {}
        self._world_ready = asyncio.Event()
        self.hello: Hello | None = None
        self.world: str | None = None
        self.session: Session | None = None
        self.snapshots: dict[str, Snapshot] = {}
        self.player: PlayerState | None = None  # the hero, as the latest snapshot of any npc had him
        # the last `party` effect sent to each npc, by anyone: its number and value (orders.py unmarks only
        # its own mark, not a companion dialog's)
        self.party: dict[str, tuple[int, int]] = {}
        self._party_writes = itertools.count(1)
        self.body_latest: dict[int, BodyObservation] = {}
        self._body_packets: deque[BodyObservationPacket] = deque(maxlen=4)
        self.body_generation: int | None = None
        self.body_session_epoch: int | None = None
        self.body_buffer_dropped_packets = 0
        self.body_sequence_gaps = 0
        self.body_stale_packets = 0
        self._body_last_seq = 0
        self.body_leases: dict[int, BodyLease] = {}
        self._roster_bodies: set[str] = set()
        self._body_command_ids = itertools.count(1)
        self._body_acquisitions: dict[int, _PendingBodyAcquire] = {}
        self._body_acquire_waiters: dict[int, _PendingBodyAcquire] = {}
        self._body_control_events: deque[BodyControlEvent] = deque(maxlen=512)
        self._body_control_last_event = 0
        self.body_control_dropped_events = 0
        self.listeners: list[Listener] = []
        self.npc_crime_check: Callable[[str, NpcActType, dict[str, Any]], Awaitable[str | None]] | None = None
        # Policy consumers need the exact JSON values from the numeric channel (including
        # optional encoder metadata and list types), after the ordinary schema/staleness checks.
        self.raw_body_listeners: list[RawBodyListener] = []
        self.closed = asyncio.Event()

    # --- receiving --------------------------------------------------------------

    async def run(self) -> None:
        try:
            async for raw in self._conn:
                try:
                    msg = parse_game_message(raw)
                except ValidationError as err:
                    log.warning("unparsed message from game: %s (%s)", raw, err.errors()[0]["msg"])
                    continue
                raw_body_packet = None
                if isinstance(msg, BodyObservations) and self.raw_body_listeners:
                    raw_body_packet = json.loads(raw)["packet"]
                self._dispatch(msg, raw_body_packet=raw_body_packet)
        except ConnectionClosed:
            pass
        finally:
            self._clear_body_observations()
            self._roster_bodies.clear()
            self.closed.set()
            self._fail_pending(GameDisconnected("game disconnected"))

    def _dispatch(self, msg: GameMessage, *, raw_body_packet: dict[str, Any] | None = None) -> None:
        match msg:
            case Hello():
                self._clear_body_observations()
                self._roster_bodies.clear()
                self.hello = msg
                if msg.protocol != PROTOCOL_VERSION:
                    log.warning("protocol mismatch: game %s, server %s", msg.protocol, PROTOCOL_VERSION)
            case WorldLoaded():
                self.world = msg.world
                self._world_ready.set()
            case Session():
                self.session = msg
            case Loading():
                self._clear_body_observations()
                self._roster_bodies.clear()
                self.world = None
                self._world_ready.clear()
                self.snapshots.clear()
                self.player = None
                self.party.clear()
            case Result():
                _resolve(self._requests.pop(msg.req, None), msg)
            case ActionResult():
                _resolve(self._actions.pop(msg.id, None), msg)
            case Pong():
                _resolve(self._pongs.pop(msg.req, None), msg)
            case RoutineInfo():
                _resolve(self._routines.pop(msg.req, None), msg)
            case Snapshot():
                self.snapshots[msg.npc.id] = msg
                if msg.player is not None:
                    self.player = msg.player
                for fut in self._snapshot_waiters.pop(msg.npc.id, []):
                    _resolve(fut, msg)
            case NpcRoster():
                if msg.full:
                    self._roster_bodies.clear()
                else:
                    self._roster_bodies.difference_update(e.id.upper() for e in msg.npcs)
                self._roster_bodies.update(e.id.upper() for e in msg.npcs if e.activity == "body")
            case BodyObservations():
                if not self._receive_body_observations(msg.packet):
                    return
                if raw_body_packet is not None:
                    for listener in self.raw_body_listeners:
                        listener(raw_body_packet)
            case BodyControlEvents():
                if not self._receive_body_control_events(msg):
                    return
            case Detached():
                self.snapshots.pop(msg.npc, None)
            case NpcActResult(status="rejected"):
                log.debug("npc_act %s for %s rejected: %s", msg.id, msg.npc, msg.detail)
        for listener in self.listeners:
            listener(msg)

    def _clear_body_observations(self) -> None:
        for pending in (*self._body_acquisitions.values(), *self._body_acquire_waiters.values()):
            if not pending.future.done():
                pending.future.set_exception(GameError("body acquisition belongs to an invalidated world"))
                pending.future.exception()
        self._body_acquisitions.clear()
        self._body_acquire_waiters.clear()
        self.body_latest.clear()
        self.body_leases.clear()
        self._body_control_events.clear()
        self._body_control_last_event = 0
        self.body_control_dropped_events = 0
        self._body_packets.clear()
        self.body_generation = None
        self.body_session_epoch = None
        self._body_last_seq = 0
        self.body_buffer_dropped_packets = 0
        self.body_sequence_gaps = 0
        self.body_stale_packets = 0

    def _receive_body_observations(self, packet: BodyObservationPacket) -> bool:
        """Bound both history and latest actor state, rejecting duplicate/older generations."""
        if self.body_session_epoch is not None and packet.session_epoch != self.body_session_epoch:
            self.body_stale_packets += 1
            return False
        if self.body_generation is not None and packet.generation < self.body_generation:
            self.body_stale_packets += 1
            return False
        if packet.generation != self.body_generation:
            self._clear_body_observations()
            self.body_generation = packet.generation
        self.body_session_epoch = packet.session_epoch
        if not packet.observations:
            return False
        if packet.observations[0].observation_seq <= self._body_last_seq:
            self.body_stale_packets += 1
            return False
        actors = set(self.body_latest) | {row.entity_id for row in packet.observations}
        if len(actors) > 4:
            self.body_stale_packets += 1
            log.warning("body packet changes watched actors without a generation reset")
            return False
        for row in packet.observations:
            self.body_sequence_gaps += max(0, row.observation_seq - self._body_last_seq - 1)
            self._body_last_seq = row.observation_seq
            self.body_latest[row.entity_id] = row
            lease = self.body_leases.get(row.entity_id)
            if lease is not None and (
                row.observation_seq > lease.acquired_observation_seq and
                # Buffered pre-acquire snapshots can arrive after the receipt.
                row.control_generation != 0 and
                lease.control_generation != row.control_generation
            ):
                self.body_leases.pop(row.entity_id, None)
        if len(self._body_packets) == self._body_packets.maxlen:
            self.body_buffer_dropped_packets += 1
        self._body_packets.append(packet)
        return True

    def _receive_body_control_events(self, packet: BodyControlEvents) -> bool:
        if packet.session_epoch != self.body_session_epoch or packet.world_epoch != self.body_generation:
            return False
        if packet.events and packet.events[0].event_seq <= self._body_control_last_event:
            return False
        for event in packet.events:
            self._body_control_last_event = event.event_seq
            lease = self.body_leases.get(event.entity_id)
            if lease is not None and event.control_generation == lease.control_generation and (
                event.phase == "ownership" and event.reason == "native_restored"
            ):
                self.body_leases.pop(event.entity_id, None)
            pending = self._body_acquire_waiters.get(event.entity_id) or self._body_acquisitions.get(
                event.entity_id
            )
            if pending is not None and (
                event.phase == "ownership"
                and event.acquire_request_id == pending.cmd.acquire_request_id
                and (event.provider, event.mode, event.target_entity_id) == (
                    pending.cmd.provider, pending.cmd.mode, pending.cmd.target_entity_id,
                )
            ):
                # A grant and its restoration can arrive before the acquire task
                # resumes, including in one packet. Retain the restoration even
                # after the grant future is done, until publication is resolved.
                if event.reason == "native_restored":
                    pending.restored = event
                elif event.reason in {"acquired", "acquire_cancelled", "expired"}:
                    self._forget_body_acquire(pending)
                    if (
                        event.reason == "acquired"
                        and self._body_acquire_waiters.get(event.entity_id) is not pending
                    ):
                        # A late grant still owns the body even if its caller
                        # has already timed out; retain it for release/status.
                        self._body_lease(pending.npc, pending.cmd.provider, pending.cmd.mode,
                                         event, event.observation_seq)
                    if not pending.future.done():
                        pending.future.set_result(event)
            if len(self._body_control_events) == self._body_control_events.maxlen:
                self.body_control_dropped_events += 1
            self._body_control_events.append(event)
        return True

    def pop_body_control_event(self) -> BodyControlEvent | None:
        return self._body_control_events.popleft() if self._body_control_events else None

    def pop_body_observations(self) -> BodyObservationPacket | None:
        """Consume one queued packet; the latest state remains available in body_latest.

        Four packets are retained. A slow consumer sees the local overflow counter and each
        engine packet's drop counters and observation sequences; no policy hidden state exists here.
        """
        return self._body_packets.popleft() if self._body_packets else None

    def _fail_pending(self, err: Exception) -> None:
        pending: list[asyncio.Future[Any]] = [
            *self._requests.values(),
            *self._actions.values(),
            *self._pongs.values(),
            *self._routines.values(),
            *(f for waiters in self._snapshot_waiters.values() for f in waiters),
        ]
        for fut in pending:
            if not fut.done():
                fut.set_exception(err)
        self._requests.clear()
        self._actions.clear()
        self._pongs.clear()
        self._routines.clear()
        self._snapshot_waiters.clear()

    # --- requests ---------------------------------------------------------------

    def _next_id(self, prefix: str) -> str:
        return f"{prefix}{next(self._ids)}"

    async def send(self, cmd: _Command) -> None:
        if self.closed.is_set():
            raise GameDisconnected("game disconnected")
        if isinstance(cmd, NpcActCmd) and self.npc_crime_check is not None:
            reason = await self.npc_crime_check(cmd.npc, cmd.action, cmd.args)
            if reason is not None:
                raise GameError(reason)
        await self._conn.send(cmd.to_json())

    async def wait_world(self, timeout: float | None = None) -> str:
        await asyncio.wait_for(self._world_ready.wait(), timeout)
        assert self.world is not None
        return self.world

    async def _request(self, cmd: _Request, timeout: float) -> Result:
        assert cmd.req is not None
        fut: asyncio.Future[Result] = asyncio.get_running_loop().create_future()
        self._requests[cmd.req] = fut
        try:
            await self.send(cmd)
            res = await asyncio.wait_for(fut, timeout)
        finally:
            self._requests.pop(cmd.req, None)
        if not res.ok:
            raise GameError(res.error or "request failed")
        return res

    async def player_pose(self, pose: Literal["kneel", "none"], timeout: float = 5.0) -> None:
        await self._request(PlayerPoseCmd(req=self._next_id("p"), pose=pose), timeout)

    async def attach(self, npc: str, timeout: float = 5.0) -> None:
        self._require_native_body(npc)
        await self._request(AttachCmd(req=self._next_id("r"), npc=npc), timeout)

    async def detach(self, npc: str, timeout: float = 5.0) -> None:
        self._require_native_body(npc)
        await self._request(DetachCmd(req=self._next_id("r"), npc=npc), timeout)

    async def body_watch(self, npcs: list[str], rate_hz: int = 20, timeout: float = 5.0) -> dict[str, Any]:
        """Passively watch up to four existing NPC symbols at 10 or 20 Hz; [] disables it."""
        cmd = BodyWatchCmd(req=self._next_id("b"), npcs=[npc.upper() for npc in npcs], rate_hz=rate_hz)
        watched = {"PLAYER" if npc == "HERO" else npc for npc in cmd.npcs}
        pending_symbols = {
            "PLAYER" if pending.npc == "HERO" else pending.npc
            for pending in (*self._body_acquisitions.values(), *self._body_acquire_waiters.values())
        }
        removed = pending_symbols - watched
        if removed:
            raise GameError(
                f"cannot remove {', '.join(sorted(removed))} from watch while a body acquisition is pending"
            )
        previous_generation = self.body_generation
        result = await self._request(cmd, timeout)
        stats = result.value if isinstance(result.value, dict) else {}
        generation = stats.get("generation")
        # The receive loop may already have processed the first packet after resolving result.
        # Preserve it if it belongs to the acknowledged generation.
        if type(generation) is int and generation != self.body_generation:
            self._clear_body_observations()
            self.body_generation = generation
        elif type(generation) is int and generation == previous_generation:
            # v45 can change the watch without changing the world generation. Idle
            # actors from the previous subscription must not fill the four-actor
            # cache; current idle rows will return with the next packet. Retain
            # owned actors and grants whose acquire caller has not published yet.
            retained = (
                self.body_leases.keys() | self._body_acquisitions.keys() | self._body_acquire_waiters.keys()
            )
            for entity_id in self.body_latest.keys() - retained:
                self.body_latest.pop(entity_id)
        return stats

    def body_owned(self, npc: str) -> bool:
        """A provider owns the body, including ownership first announced by the roster."""
        npc = npc.upper()
        return npc in self._roster_bodies or any(lease.npc == npc for lease in self.body_leases.values())

    def _require_native_body(self, npc: str) -> None:
        if self.body_owned(npc):
            raise GameError("NPC body is owned by the combat provider")

    async def _body_request(self, cmd: _Request, timeout: float) -> BodyControlResult:
        result = BodyControlResult.model_validate((await self._request(cmd, timeout)).value)
        if not result.accepted:
            raise GameError(f"body command rejected: {result.reason}")
        if isinstance(cmd, (
            BodyAcquireCmd, BodyActionCmd, BodyGoalCmd, BodyHeartbeatCmd, BodyReleaseCmd,
            BodyDisconnectCmd, BodyCancelAcquireCmd,
        )):
            if (result.entity_id, result.session_epoch, result.world_epoch) != (
                cmd.entity_id, cmd.session_epoch, cmd.world_epoch,
            ):
                raise GameError("body response belongs to a different actor or world")
            if self.body_session_epoch != result.session_epoch or self.body_generation != result.world_epoch:
                raise GameError("body response belongs to an invalidated world")
            if isinstance(cmd, BodyCancelAcquireCmd):
                if result.acquire_request_id != cmd.acquire_request_id:
                    raise GameError("body response belongs to a different acquisition request")
            elif not isinstance(cmd, BodyAcquireCmd) and result.control_generation != cmd.control_generation:
                raise GameError("body response belongs to a different control generation")
        return result

    async def body_acquire(
        self, npc: str, observation: BodyObservation, provider: str, *,
        mode: BodyMode = "realtime", timeout_ms: int = 500, timeout: float = 5.0,
        await_boundary_ms: int = 0, cancel_event: asyncio.Event | None = None,
        retain_on_timeout: bool = False,
        policy: Literal["engine"] | None = None,
    ) -> BodyLease:
        """Claim a watched actor using its real process/world/target IDs, without changing native state."""
        if mode != "realtime":
            raise GameError("ordinary game body control requires realtime mode")
        cmd = BodyAcquireCmd(
            req=self._next_id("bc"), entity_id=observation.entity_id,
            session_epoch=observation.session_epoch, world_epoch=observation.world_epoch,
            provider=provider, mode=mode, timeout_ms=timeout_ms,
            target_entity_id=observation.target.entity_id,
            await_boundary_ms=await_boundary_ms,
            acquire_request_id=(secrets.randbits(63) or 1) if await_boundary_ms else 0,
            policy=policy,
        )
        if (
            observation.entity_id in self._body_acquisitions
            or observation.entity_id in self._body_acquire_waiters
        ):
            raise GameError("body acquisition already pending for this actor")
        pending = None
        if await_boundary_ms:
            pending = _PendingBodyAcquire(
                cmd, npc.upper(), observation.observation_seq,
                asyncio.get_running_loop().create_future(),
            )
            # Ownership can be pushed before the immediate receipt is received.
            self._body_acquisitions[observation.entity_id] = pending
            # Keep the publication context after a terminal event removes the
            # queued request, so a following native_restored still invalidates it.
            self._body_acquire_waiters[observation.entity_id] = pending
        completed = False
        retained = False
        try:
            try:
                result = await self._body_request(cmd, timeout)
            except GameError as error:
                if str(error).startswith("body command rejected: "):
                    completed = True  # An explicit rejection never queued a request.
                raise
            if cancel_event is not None and cancel_event.is_set():
                raise GameError("body acquisition cancelled by client")
            acquired_seq = result.observation_seq or observation.observation_seq
            if await_boundary_ms and result.acquire_request_id != cmd.acquire_request_id:
                raise GameError("body response belongs to a different acquisition request")
            if await_boundary_ms and result.reason != "acquire_pending" and result.observation_seq == 0:
                raise GameError("body acquisition response lacks its acquisition observation sequence")
            if result.reason == "acquire_pending":
                if pending is None or (
                    result.owner != "native" or result.control_generation != 0
                    or result.acquire_request_id != cmd.acquire_request_id
                ):
                    raise GameError("invalid pending body acquisition receipt")
                event = await self._await_body_acquire(
                    pending, await_boundary_ms / 1000 + timeout, timeout, cancel_event,
                )
                if event.reason in {"acquire_cancelled", "expired"}:
                    completed = True
                    raise GameError(f"body acquire cancelled: {event.acquire_cancel_reason or event.reason}")
                result = BodyControlResult(
                    type="body_control_result", accepted=True, duplicate=False, reason="acquired",
                    **event.model_dump(include={
                        "session_epoch", "world_epoch", "entity_id", "control_generation", "goal_version",
                        "target_entity_id", "command_id", "execution_id", "owner", "release_pending",
                        "acquire_request_id",
                    }),
                )
                acquired_seq = event.observation_seq
            if pending is not None and pending.was_restored(result.control_generation):
                completed = True  # Authoritatively native; no cancellation is needed.
                assert pending.restored is not None
                raise GameError(f"body acquisition already released: {pending.restored.release_reason}")
            lease = self._body_lease(npc, provider, mode, result, acquired_seq)
            completed = True
            return lease
        except TimeoutError:
            # The pilot resolves a lost receipt with status inside its acquire
            # window. Cancelling here could release an already granted lease.
            retained = retain_on_timeout
            raise
        finally:
            if pending is not None:
                try:
                    if retained:
                        if pending.has_grant() and (self.body_session_epoch, self.body_generation) == (
                            cmd.session_epoch, cmd.world_epoch,
                        ):
                            grant = pending.future.result()
                            self._body_lease(npc, provider, mode, grant, grant.observation_seq)
                    elif completed:
                        self._forget_body_acquire(pending)
                    elif self._body_acquisitions.get(observation.entity_id) is pending or (
                        self._body_acquire_waiters.get(observation.entity_id) is pending
                        and pending.has_grant()
                    ):
                        # A missing receipt does not prove the engine declined the request.
                        # The client-selected ID can also release a grant racing this cancel.
                        try:
                            await self.body_cancel_acquire(observation.entity_id, timeout=timeout)
                        except (GameError, ConnectionClosed, OSError, TimeoutError):
                            # Keep an unresolved request or its confirmed grant
                            # available for authoritative cleanup/retry.
                            if pending.has_grant() and (self.body_session_epoch, self.body_generation) == (
                                cmd.session_epoch, cmd.world_epoch,
                            ):
                                grant = pending.future.result()
                                self._body_lease(npc, provider, mode, grant, grant.observation_seq)
                finally:
                    if self._body_acquire_waiters.get(observation.entity_id) is pending:
                        self._body_acquire_waiters.pop(observation.entity_id)
                    if pending.future.done() and not pending.future.cancelled():
                        pending.future.exception()

    async def _await_body_acquire(
        self, pending: _PendingBodyAcquire, wait: float, timeout: float,
        cancel_event: asyncio.Event | None,
    ) -> BodyControlEvent:
        async with asyncio.timeout(wait):
            while not pending.future.done():
                if cancel_event is not None and cancel_event.is_set():
                    raise GameError("body acquisition cancelled by client")
                # Existing bridge event drains publish ownership events; this polls
                # events, never retries acquire to hunt for the animation boundary.
                try:
                    await self.body_events(timeout=timeout)
                except (GameError, TimeoutError):
                    if self.closed.is_set():
                        raise GameDisconnected("game disconnected") from None
                if not pending.future.done():
                    await asyncio.wait({pending.future}, timeout=0.150)
            return pending.future.result()

    def _body_lease(
        self, npc: str, provider: str, mode: BodyMode,
        result: BodyControlResult | BodyControlEvent | BodyControlActor, acquired_seq: int,
    ) -> BodyLease:
        lease = BodyLease(
            npc=npc.upper(), provider=provider, mode=mode, session_epoch=result.session_epoch,
            world_epoch=result.world_epoch, entity_id=result.entity_id,
            control_generation=result.control_generation, goal_version=result.goal_version,
            target_entity_id=result.target_entity_id,
            acquired_observation_seq=acquired_seq,
        )
        # A loading/disconnect notification may already have invalidated this response's world.
        if self.body_session_epoch != lease.session_epoch or self.body_generation != lease.world_epoch:
            raise GameError("body acquisition belongs to an invalidated world")
        if len(self.body_leases) >= 4 and lease.entity_id not in self.body_leases:
            raise GameError("body lease cache is full")
        self.body_leases[lease.entity_id] = lease
        return lease

    async def body_cancel_acquire(self, entity_id: int, *, timeout: float = 5.0) -> None:
        """Cancel a queued request or release the lease it just obtained, by request identity."""
        pending = self._body_acquisitions.get(entity_id) or self._body_acquire_waiters.get(entity_id)
        if pending is None:
            return
        cmd = pending.cmd
        if self.closed.is_set() or (self.body_session_epoch, self.body_generation) != (
            cmd.session_epoch, cmd.world_epoch,
        ):
            self._forget_body_acquire(pending)
            return
        try:
            result = await self._body_request(BodyCancelAcquireCmd(
                req=self._next_id("br"), entity_id=entity_id, session_epoch=cmd.session_epoch,
                world_epoch=cmd.world_epoch, provider=cmd.provider,
                acquire_request_id=cmd.acquire_request_id,
            ), timeout)
        except (GameError, TimeoutError):
            # A cancellation acknowledgement may itself be lost. Native ownership
            # with no pending ID is authoritative; native ownership alone is not.
            try:
                status = await self.body_control_status(timeout=timeout)
            except GameError as error:
                if "unknown_entity" in str(error) and (
                    self.body_session_epoch, self.body_generation,
                ) == (cmd.session_epoch, cmd.world_epoch):
                    self._forget_body_acquire(pending)
                    return
                raise
            if (status.session_epoch, status.world_epoch) != (cmd.session_epoch, cmd.world_epoch):
                raise GameError("body cancellation belongs to an invalidated world") from None
            actor = next((row for row in status.actors if row.entity_id == entity_id), None)
            if actor is None or (actor.owner == "native" and actor.acquire_request_id == 0):
                self._forget_body_acquire(pending)
                return
            raise
        if result.owner == "scripted" and not pending.was_restored(result.control_generation):
            self._body_lease(
                pending.npc, cmd.provider, cmd.mode, result,
                result.observation_seq or pending.observation_seq,
            )
        self._forget_body_acquire(pending)

    def _forget_body_acquire(self, pending: _PendingBodyAcquire) -> None:
        if self._body_acquisitions.get(pending.cmd.entity_id) is pending:
            self._body_acquisitions.pop(pending.cmd.entity_id)

    def body_acquire_pending(self, entity_id: int) -> bool:
        return entity_id in self._body_acquisitions

    async def body_goal(
        self, lease: BodyLease, goal_version: int, target_entity_id: int, *, timeout: float = 5.0,
    ) -> BodyLease:
        cmd = BodyGoalCmd(
            req=self._next_id("bg"), **self._body_scope(lease), goal_version=goal_version,
            target_entity_id=target_entity_id,
        )
        result = await self._body_request(cmd, timeout)
        updated = lease.model_copy(
            update={"goal_version": result.goal_version, "target_entity_id": target_entity_id}
        )
        if self.body_leases.get(lease.entity_id) == lease:
            self.body_leases[lease.entity_id] = updated
        return updated

    @staticmethod
    def _body_scope(lease: BodyLease) -> dict[str, Any]:
        return lease.model_dump(
            include={"entity_id", "session_epoch", "world_epoch", "provider", "control_generation"}
        )

    async def body_action(
        self, lease: BodyLease, observation: BodyObservation, action_id: int, *,
        command_id: int | None = None, ttl_ms: int = 200, timeout: float = 5.0,
    ) -> BodyControlResult:
        if (observation.entity_id, observation.session_epoch, observation.world_epoch) != (
            lease.entity_id, lease.session_epoch, lease.world_epoch,
        ):
            raise GameError("body observation belongs to a different actor or world")
        if observation.control_generation != lease.control_generation or not observation.action_mask_valid:
            raise GameError("body observation does not grant this control generation")
        if (
            observation.goal.version != lease.goal_version or
            observation.goal.target_id != lease.target_entity_id
        ):
            raise GameError("body observation belongs to an older goal or target")
        cmd = BodyActionCmd(
            req=self._next_id("ba"), **self._body_scope(lease),
            command_id=next(self._body_command_ids) if command_id is None else command_id,
            goal_version=lease.goal_version, target_entity_id=lease.target_entity_id,
            observation_seq=observation.observation_seq, issued_game_ms=observation.t_game_ms,
            action_id=action_id, ttl_ms=ttl_ms,
        )
        return await self._body_request(cmd, timeout)

    async def body_heartbeat(self, lease: BodyLease, *, timeout: float = 5.0) -> BodyControlResult:
        return await self._body_request(
            BodyHeartbeatCmd(req=self._next_id("bh"), **self._body_scope(lease)), timeout,
        )

    async def body_release(self, lease: BodyLease, *, timeout: float = 5.0) -> BodyControlResult:
        result = await self._body_request(
            BodyReleaseCmd(req=self._next_id("br"), **self._body_scope(lease)), timeout,
        )
        if result.owner == "native" and not result.release_pending:
            self.body_leases.pop(lease.entity_id, None)
        return result

    async def body_control_status(self, *, timeout: float = 5.0) -> BodyControlStatus:
        leases = self.body_leases.copy()
        acquisitions = self._body_acquisitions.copy()
        result = await self._request(BodyControlStatusCmd(req=self._next_id("bs")), timeout)
        status = BodyControlStatus.model_validate(result.value)
        if status.session_epoch == self.body_session_epoch and status.world_epoch == self.body_generation:
            for actor in status.actors:
                lease = leases.get(actor.entity_id)
                if (
                    lease is not None and self.body_leases.get(actor.entity_id) == lease
                    and actor.control_generation != lease.control_generation
                ):
                    self.body_leases.pop(actor.entity_id, None)
            actors = {actor.entity_id: actor for actor in status.actors}
            for entity_id, pending in acquisitions.items():
                # Another fight can acquire while this snapshot is in flight.
                if self._body_acquisitions.get(entity_id) is not pending:
                    continue
                actor = actors.get(entity_id)
                if actor is None or (
                    actor.owner == "native" and actor.acquire_request_id == 0 and not actor.acquire_pending
                ):
                    self._forget_body_acquire(pending)
                    if not pending.future.done():
                        pending.future.set_exception(GameError("body acquisition is no longer pending"))
                        # An orphaned request no longer has a task awaiting it.
                        pending.future.exception()
                elif (
                    entity_id not in self._body_acquire_waiters
                    and actor.owner == "scripted" and not actor.release_pending
                    and not pending.was_restored(actor.control_generation)
                    and (actor.acquire_request_id, actor.provider, actor.mode, actor.target_entity_id) == (
                        pending.cmd.acquire_request_id, pending.cmd.provider,
                        pending.cmd.mode, pending.cmd.target_entity_id,
                    )
                ):
                    # A status snapshot can recover an orphan even when both
                    # its receipt and ownership event were lost. Forget its ID
                    # before a late event can publish the same grant again.
                    self._body_lease(pending.npc, pending.cmd.provider, pending.cmd.mode,
                                     actor, pending.observation_seq)
                    self._forget_body_acquire(pending)
        return status

    async def body_events(self, *, timeout: float = 5.0) -> BodyControlEvents:
        result = await self._request(BodyControlEventsCmd(req=self._next_id("be")), timeout)
        events = BodyControlEvents.model_validate(result.value)
        self._receive_body_control_events(events)
        return events

    async def body_disconnect(self, lease: BodyLease, *, timeout: float = 5.0) -> BodyControlResult:
        return await self._body_request(
            BodyDisconnectCmd(req=self._next_id("bd"), **self._body_scope(lease)), timeout,
        )

    async def routine(self, npc: str, timeout: float = 5.0) -> RoutineInfo:
        """The npc's daily routine, its routine variants and current script state (v6)."""
        cmd = RoutineCmd(req=self._next_id("q"), npc=npc)
        fut: asyncio.Future[RoutineInfo] = asyncio.get_running_loop().create_future()
        self._routines[cmd.req] = fut
        try:
            await self.send(cmd)
            info = await asyncio.wait_for(fut, timeout)
        finally:
            self._routines.pop(cmd.req, None)
        if info.error:
            raise GameError(info.error)
        return info

    async def debug(self, cmd: DebugCommand, slot: int = 0, timeout: float = 10.0, **args: Any) -> None:
        """A test helper (game in -devmode): saving/loading `save_slot_<slot>.sav` (returns once it has
        started), `spawn` (instance=, near=, distance=), `player_weapon` (state="melee"|"none"),
        `teleport` (near=, distance=), `kill` (instance=, near=), `npc_goto` (npc=, waypoint= or to= and
        distance=, face=: the npc stays free)."""
        await self._request(DebugCmd(req=self._next_id("r"), cmd=cmd, slot=slot, **args), timeout)

    async def patch_graph(
        self, ops: list[dict[str, Any]], replace: bool = False, timeout: float = 5.0
    ) -> None:
        """Change the story graph in the game (see GraphPatchCmd); GameError lists the rejected operations."""
        await self._request(GraphPatchCmd(req=self._next_id("g"), replace=replace, ops=ops), timeout)

    async def story_spawn(
        self,
        instance: str,
        tag: str,
        count: int = 1,
        near: str | None = None,
        distance: int = 600,
        waypoint: str | None = None,
    ) -> int:
        """Creatures under a tag for the director (near an npc, or at a waypoint); how many came."""
        cmd = StorySpawnCmd(
            req=self._next_id("s"),
            instance=instance.upper(),
            tag=tag,
            count=count,
            near=near,
            distance=distance,
            waypoint=waypoint,
        )
        return int((await self._request(cmd, 5.0)).value or 0)

    async def story_npc(self, spec: NpcSpec, timeout: float = 10.0) -> dict[str, Any]:
        """A character of the director's, made once (see StoryNpcCmd): {npc, state, index}."""
        cmd = StoryNpcCmd(req=self._next_id("n"), **spec.model_dump())
        return dict((await self._request(cmd, timeout)).value or {})

    async def story_watch(self, npcs: list[str], owner: str = "") -> None:
        """Npc's whose distance to every tag's living creatures the game reports (story_world); each owner
        keeps its own list."""
        await self.send(StoryWatchCmd(npcs=[n.upper() for n in npcs], owner=owner))

    async def story_effect(
        self, npc: str, effects: list[dict[str, Any]], timeout: float = 5.0, *, check: bool = False
    ) -> None:
        """Effects on an npc through the scripts (see StoryEffectCmd); `check`: all or nothing, the gives
        only if each giver has what it gives."""
        if any(e.get("do") in ("gate_pass", "gate_checklog") for e in effects):
            from .gates import play_gate_effects

            await play_gate_effects(self, npc.upper(), effects)
            return
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        while True:
            cmd = StoryEffectCmd(
                req=self._next_id("s"), npc=npc.upper(), effects=effects, check=check or None,
            )
            try:
                await self._request(cmd, max(deadline - loop.time(), 0.1))
                break
            except GameError as err:
                # P30 owns the npc's body: the game asked it to let go and ran none of the effects
                if not str(err).startswith(BODY_RELEASING) or loop.time() + STORY_EFFECT_RETRY >= deadline:
                    raise
            await asyncio.sleep(STORY_EFFECT_RETRY)
        for e in effects:
            if e.get("do") == "party":
                self.party[npc.upper()] = (next(self._party_writes), 1 if e.get("value") else 0)

    async def beast(
        self, npc: str, do: BeastDo, *, ms: int | None = None, sit: bool | None = None, timeout: float = 5.0
    ) -> None:
        """The server's word on a wolf (see BeastCmd): listen, calm, flee, attack, resume."""
        await self._request(BeastCmd(req=self._next_id("b"), npc=npc, do=do, ms=ms, sit=sit), timeout)

    async def raise_dead(
        self, npc: str, card: EndCard, spell: str | None = None, timeout: float = 5.0
    ) -> None:
        """The necromancer's answer: `npc` kills the hero, raises him as a skeleton, the game ends on
        `card`."""
        cmd = RaiseDeadCmd(req=self._next_id("s"), npc=npc.upper(), spell=spell, card=card)
        await self._request(cmd, timeout)

    async def waypoints(self, timeout: float = 10.0) -> list[dict[str, Any]]:
        """Every waypoint and freepoint of the world: name, position, freepoint or not (game in -devmode)."""
        res = await self._request(DebugCmd(req=self._next_id("r"), cmd="waypoints"), timeout)
        return list(res.value or [])

    async def npc_view(self, npcs: list[str], timeout: float = 3.0) -> list[NpcView]:
        """What these npc's perceive now (see NpcViewCmd)."""
        res = await self._request(NpcViewCmd(req=self._next_id("v"), npcs=[n.upper() for n in npcs]), timeout)
        return [NpcView.model_validate(v) for v in res.value or []]

    async def story_nearby(self, npc: str, radius: int = 2000) -> list[dict[str, Any]]:
        """People within `radius` cm of an npc."""
        res = await self._request(StoryNearbyCmd(req=self._next_id("s"), npc=npc.upper(), radius=radius), 5.0)
        return list(res.value or [])

    async def npc_chests(self, npc: str) -> list[Chest]:
        """The npc's chests and what lies in them (v28), nearest first."""
        res = await self._request(NpcChestsCmd(req=self._next_id("c"), npc=npc.upper()), 5.0)
        return [Chest.model_validate(c) for c in res.value or []]

    async def story_state(
        self,
        vars: Iterable[str] = (),
        npcs: Iterable[str] = (),
        knows: Iterable[str] = (),
        symbols: Iterable[str] = (),
        items: Iterable[str] = (),
        near: str | None = None,
        timeout: float = 5.0,
        deed_target: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        """What the director's rules read (see StoryStateCmd); with `near`, how far each npc is from it."""
        cmd = StoryStateCmd(
            req=self._next_id("s"),
            vars=list(vars),
            npcs=[n.upper() for n in npcs],
            near=near.upper() if near else None,
            knows=list(knows),
            symbols=list(symbols),
            items=list(items),
            deed_target=deed_target,
        )
        return dict((await self._request(cmd, timeout)).value or {})

    async def story_people(self, timeout: float = 10.0) -> list[dict[str, Any]]:
        """Every person of the world and its day (see StoryPeopleCmd)."""
        return list((await self._request(StoryPeopleCmd(req=self._next_id("s")), timeout)).value or [])

    async def own_npc(
        self,
        npc: str,
        like: str,
        waypoint: str | None = None,
        distance: int | None = None,
        timeout: float = 10.0,
    ) -> dict[str, Any]:
        """An npc of the director's (game in -devmode, v33): a script symbol of ours made like the script's
        npc `like`, put at a waypoint or `distance` cm in front of the player; {npc, index: its symbol,
        name}."""
        cmd = DebugCmd(
            req=self._next_id("r"),
            cmd="own_npc",
            npc=npc.upper(),
            like=like.upper(),
            waypoint=waypoint,
            distance=distance,
        )
        return dict((await self._request(cmd, timeout)).value or {})

    async def triggers(self, timeout: float = 5.0) -> list[dict[str, Any]]:
        """The director's triggers in the game (game in -devmode, v35): [{id, on, npc, node, repeat, count,
        holds}] — `holds`: a state trigger's condition now (None: not computed)."""
        res = await self._request(DebugCmd(req=self._next_id("r"), cmd="triggers"), timeout)
        return list(res.value or [])

    async def story_eval(self, npc: str, exprs: list[Any], timeout: float = 5.0) -> list[Any]:
        """Story graph expressions on the live VM, self = `npc` (see StoryEvalCmd)."""
        cmd = StoryEvalCmd(req=self._next_id("s"), npc=npc.upper(), exprs=exprs)
        return list((await self._request(cmd, timeout)).value or [])

    async def story_program(
        self,
        node: str,
        part: Literal["information", "condition"] = "information",
        own: bool = False,
        timeout: float = 5.0,
    ) -> dict[str, Any]:
        """The program a dialog's function runs now, or the scripts' own (StoryProgramCmd): {node, fn,
        returns, patched, steps}."""
        cmd = StoryProgramCmd(req=self._next_id("s"), node=node.upper(), part=part, own=own)
        return dict((await self._request(cmd, timeout)).value or {})

    async def story_tag(self, tag: str, npcs: list[str], timeout: float = 5.0) -> int:
        """Npc's of the world watched under a tag (see StoryTagCmd); how many were found."""
        cmd = StoryTagCmd(req=self._next_id("s"), tag=tag, npcs=[n.upper() for n in npcs])
        return int((await self._request(cmd, timeout)).value or 0)

    async def offered_dialogs(self, npc: str, timeout: float = 5.0) -> list[str]:
        """The dialogs the npc offers the player now (game in -devmode)."""
        res = await self._request(DebugCmd(req=self._next_id("r"), cmd="dialogs", npc=npc.upper()), timeout)
        return list(res.value or [])

    async def choose_dialog(self, npc: str, node: str, timeout: float = 5.0) -> None:
        """Play a dialog the npc offers as if the player chose it (game in -devmode)."""
        await self._request(
            DebugCmd(req=self._next_id("r"), cmd="choose", npc=npc.upper(), node=node.upper()), timeout
        )

    async def dialog_choices(self, npc: str, node: str, timeout: float = 5.0) -> list[dict[str, str]]:
        """The choices dialog `node` offers after its lines, as the dialog window asks (game in -devmode)."""
        cmd = DebugCmd(req=self._next_id("r"), cmd="choices", npc=npc.upper(), node=node.upper())
        return list((await self._request(cmd, timeout)).value or [])

    async def start_talk(self, npc: str, timeout: float = 5.0) -> None:
        """The hero talks to the npc as with the action key: the dialog window opens (game in -devmode)."""
        await self._request(DebugCmd(req=self._next_id("r"), cmd="talk", npc=npc.upper()), timeout)

    async def dialog_menu(self, timeout: float = 5.0) -> list[str]:
        """What the dialog window offers now, none while a line plays (game in -devmode)."""
        return list((await self._request(DebugCmd(req=self._next_id("r"), cmd="menu"), timeout)).value or [])

    async def pick_in_menu(self, text: str, timeout: float = 5.0) -> None:
        """Pick the dialog window's entry `text` as the player does (game in -devmode)."""
        await self._request(DebugCmd(req=self._next_id("r"), cmd="menu_pick", text=text), timeout)

    async def deal_window(self, timeout: float = 5.0) -> dict[str, Any] | None:
        """What the deal window shows (game in -devmode): {deal, version, npc, npc_gives, hero_gives,
        missing, sent}, or None."""
        res = await self._request(DebugCmd(req=self._next_id("r"), cmd="deal"), timeout)
        return dict(res.value) if res.value else None

    async def press_deal(self, timeout: float = 5.0) -> str:
        """Y as the player presses it on the deal window (game in -devmode): "sent", or why not ("none",
        "waiting", "missing", "changed", "blocked")."""
        res = await self._request(DebugCmd(req=self._next_id("r"), cmd="deal_accept"), timeout)
        return str(res.value or "")

    async def decline_deal(self, timeout: float = 5.0) -> str:
        """N as the player presses it on the deal window (game in -devmode): "declined", "none", "blocked"."""
        res = await self._request(DebugCmd(req=self._next_id("r"), cmd="deal_decline"), timeout)
        return str(res.value or "")

    async def check_conditions(self, timeout: float = 30.0) -> dict[str, Any]:
        """Script conditions of every npc in the world against their lifted form (game in -devmode)."""
        return dict((await self._request(DebugCmd(req=self._next_id("r"), cmd="check"), timeout)).value or {})

    async def evaluate(self, expr: list[Any], npc: str | None = None, timeout: float = 5.0) -> Any:
        """A story graph expression computed in the game, self = `npc` (game in -devmode)."""
        cmd = DebugCmd(req=self._next_id("r"), cmd="eval", npc=npc.upper() if npc else None, expr=expr)
        return (await self._request(cmd, timeout)).value

    async def act(
        self, npc: str, action: ActionType, /, *, timeout: float = 90.0, **args: Any
    ) -> ActionResult:
        """Run one action and wait for its result. The game enforces its own per-action deadline.

        `args` go to the game as-is, e.g. act(npc, "turn_to", npc="PLAYER").
        """
        self._require_native_body(npc)
        cmd = ActionCmd(id=self._next_id("a"), npc=npc, action=action, args=args)
        fut: asyncio.Future[ActionResult] = asyncio.get_running_loop().create_future()
        self._actions[cmd.id] = fut
        try:
            await self.send(cmd)
            return await asyncio.wait_for(fut, timeout)
        finally:
            self._actions.pop(cmd.id, None)

    async def npc_act(self, npc: str, action: NpcActType, /, **args: Any) -> str:
        """Queue a soft action for an npc that follows its routine; returns the id. The game answers
        `npc_act_result` at once and `npc_idle` when the npc is done."""
        self._require_native_body(npc)
        cmd = NpcActCmd(id=self._next_id("n"), npc=npc, action=action, args=args)
        await self.send(cmd)
        return cmd.id

    async def snapshot(self, npc: str, *, fresh: bool = False, timeout: float = 5.0) -> Snapshot:
        npc = npc.upper()
        if not fresh and npc in self.snapshots:
            return self.snapshots[npc]
        fut: asyncio.Future[Snapshot] = asyncio.get_running_loop().create_future()
        self._snapshot_waiters.setdefault(npc, []).append(fut)
        await self.send(SnapshotCmd(npc=npc))
        return await asyncio.wait_for(fut, timeout)

    async def ping(self, timeout: float = 5.0) -> float:
        loop = asyncio.get_running_loop()
        cmd = PingCmd(req=self._next_id("p"))
        fut: asyncio.Future[Pong] = loop.create_future()
        self._pongs[cmd.req] = fut
        started = loop.time()
        try:
            await self.send(cmd)
            await asyncio.wait_for(fut, timeout)
        finally:
            self._pongs.pop(cmd.req, None)
        return loop.time() - started


def _resolve[T](fut: asyncio.Future[T] | None, value: T) -> None:
    if fut is not None and not fut.done():
        fut.set_result(value)


OnLink = Callable[[GameLink], Awaitable[None]]


class GameServer:
    """WebSocket server the game connects to. A new connection replaces the previous one.

    `GET /health` on the same port answers {"ok":true,"service":"gothic-ai","game":<connected>}:
    the launcher waits for it before starting the game."""

    def __init__(self, host: str = "127.0.0.1", port: int = 47817, on_link: OnLink | None = None) -> None:
        self.host = host
        self.port = port
        self.on_link = on_link
        self.link: GameLink | None = None
        self.started = asyncio.Event()
        self._link_changed = asyncio.Condition()

    async def wait_link(self) -> GameLink:
        async with self._link_changed:
            await self._link_changed.wait_for(lambda: self.link is not None and not self.link.closed.is_set())
            assert self.link is not None
            return self.link

    async def _handler(self, conn: ServerConnection) -> None:
        link = GameLink(conn)
        log.info("game connected from %s", conn.remote_address)
        async with self._link_changed:
            self.link = link
            self._link_changed.notify_all()

        session = asyncio.create_task(self.on_link(link)) if self.on_link else None
        if session is not None:
            session.add_done_callback(_log_session_error)
        try:
            await link.run()
        finally:
            log.info("game disconnected")
            if session is not None and not session.done():
                session.cancel()
                await asyncio.gather(session, return_exceptions=True)
            async with self._link_changed:
                if self.link is link:
                    self.link = None
                self._link_changed.notify_all()

    def _health(self, conn: ServerConnection, request: Request) -> Response | None:
        if request.path != "/health":
            return None
        game = self.link is not None and not self.link.closed.is_set()
        resp = conn.respond(HTTPStatus.OK, json.dumps({"ok": True, "service": "gothic-ai", "game": game}))
        del resp.headers["Content-Type"]
        resp.headers["Content-Type"] = "application/json"
        return resp

    async def serve_forever(self) -> None:
        async with serve(self._handler, self.host, self.port, process_request=self._health) as server:
            self.port = server.sockets[0].getsockname()[1]  # actual port when 0 was requested
            self.started.set()
            log.info("waiting for the game on ws://%s:%d", self.host, self.port)
            await server.serve_forever()


def _log_session_error(task: asyncio.Task[None]) -> None:
    if not task.cancelled() and task.exception() is not None:
        log.error("game session failed", exc_info=task.exception())
