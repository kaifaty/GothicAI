"""Exercise the live pilot through the bridge adapter and a torch-free TCP policy."""

import asyncio
import json
import socket
from collections import Counter, deque
from contextlib import asynccontextmanager
from dataclasses import FrozenInstanceError
from itertools import pairwise

import pytest
from websockets.exceptions import ConnectionClosedError

from gothic_ai import bodypilot
from gothic_ai.bodypilot import BodyPilot
from gothic_ai.game import GameDisconnected, GameError, GameLink
from gothic_ai.protocol import (
    PROTOCOL_VERSION,
    Attached,
    Hello,
    Loading,
    NpcEvent,
    NpcRoster,
    Result,
    RosterEntry,
)
from tests.test_body_control import controlled, event, events, receipt, visibility_diagnostics
from tests.test_body_observations import message, observation, packet


async def eventually(predicate, timeout=1.0):
    async with asyncio.timeout(timeout):
        while not predicate():  # noqa: ASYNC110 - observe a fake bridge or TCP peer without coupling it.
            await asyncio.sleep(0.001)


class Clock:
    def __init__(self):
        self.now = 10.0

    def __call__(self):
        return self.now

    def advance(self, seconds):
        self.now += seconds


class Bridge:
    """Fake native requests while retaining GameLink's lease and message checks."""

    def __init__(self):
        self.sent = []
        self.sent_times = []
        self.link = GameLink(self)
        self.acquire_rejections = deque()
        self.acquire_pending_results = deque()
        self.acquire_pending_delay = 0.020
        self.pending_acquire = None
        self._acquire_request_id = 0
        self._pending_event_seq = 0
        self.action_rejections = deque()
        self.release_rejections = deque()
        self.status_error = None
        self.acquire_roster_delay = 0
        self.target_is_hero = True
        self.target_npc = None
        self.release_pending_polls = 0
        self._released_pending = False
        self.request_rejections = {}
        self.response_delays = {}
        self._response_tasks = set()
        self._roster = {}
        self._watched_npc = None
        self._lease_timeout = 0.0
        self._lease_watchdog = None
        self.lease_expirations = 0
        self.provider_owned = False
        self._control_scope = None
        self._control_npc = None
        self._control_target = 2
        self.control_events = deque()

    def commands(self, kind):
        return [command for command in self.sent if command["type"] == kind]

    def command_times(self, kind):
        return [when for command, when in self.sent_times if command["type"] == kind]

    def roster(self, *entries):
        self._roster = {entry.id.upper(): entry for entry in entries}
        self.link._dispatch(NpcRoster(type="npc_roster", t=1, npcs=entries))

    def _activity(self, npc, activity):
        if npc in self._roster:
            self._roster[npc] = self._roster[npc].model_copy(update={"activity": activity})
            # Validate the same activity value that the native bridge puts on the wire.
            self.link._dispatch(NpcRoster.model_validate({
                "type": "npc_roster", "t": 1,
                "npcs": [entry.model_dump(mode="json") for entry in self._roster.values()],
            }))

    def _background(self, coroutine):
        task = asyncio.create_task(coroutine)
        self._response_tasks.add(task)
        task.add_done_callback(self._response_tasks.discard)

    async def _acquired_roster(self, npc):
        # Native ownership is independent of whether the acquire reply arrives.
        await asyncio.sleep(self.acquire_roster_delay)
        if self.provider_owned:
            self._activity(npc, "body")

    def _renew_lease(self):
        if self._lease_watchdog is not None:
            self._lease_watchdog.cancel()
        self._lease_watchdog = asyncio.get_running_loop().call_later(
            self._lease_timeout, self._expire_lease,
        )

    def _grant(self, command):
        self.provider_owned = True
        self._control_scope = {name: command[name] for name in (
            "entity_id", "session_epoch", "world_epoch",
        )}
        self._control_npc = self._watched_npc
        self._control_target = command["target_entity_id"]
        self._acquire_request_id = command.get("acquire_request_id", 0)
        self._lease_timeout = command["timeout_ms"] / 1000
        self._renew_lease()
        self._background(self._acquired_roster(self._watched_npc))

    async def _resolve_pending(self, command, outcome):
        await asyncio.sleep(self.acquire_pending_delay)
        if self.pending_acquire is not command:
            return
        self.pending_acquire = None
        self._pending_event_seq += 1
        if outcome == "acquired":
            self._grant(command)
            self.control_events.append(event(
                self._pending_event_seq, **self._control_scope, provider=command["provider"],
                acquire_request_id=command["acquire_request_id"], observation_seq=3,
            ))
        else:
            self.control_events.append(event(
                self._pending_event_seq, entity_id=command["entity_id"], provider=command["provider"],
                acquire_request_id=command["acquire_request_id"], reason="acquire_cancelled",
                acquire_cancel_reason=outcome, owner="native", control_generation=0, goal_version=0,
            ))

    def _expire_lease(self):
        self.lease_expirations += 1
        if self.provider_owned:
            self.provider_owned = False
            self.link._dispatch(events(event(
                1, **self._control_scope, reason="native_restored", owner="native",
            )))
            self._activity(self._control_npc, "fight")

    def restore_native(self, *, release_reason=""):
        """Restore the native actor, leaving ownership events for the next destructive poll."""
        lease = next(iter(self.link.body_leases.values()))
        self.provider_owned = False
        if self._lease_watchdog is not None:
            self._lease_watchdog.cancel()
        self.control_events.append(event(
            1, entity_id=lease.entity_id, reason="native_restored", owner="native",
            target_entity_id=lease.target_entity_id, release_reason=release_reason,
        ))
        self._activity(lease.npc, "fight")

    async def close(self):
        if self._lease_watchdog is not None:
            self._lease_watchdog.cancel()
        for task in list(self._response_tasks):
            task.cancel()
        await asyncio.gather(*self._response_tasks, return_exceptions=True)

    async def send(self, raw):
        command = json.loads(raw)
        self.sent.append(command)
        self.sent_times.append((command, asyncio.get_running_loop().time()))
        kind = command["type"]
        if rejections := self.request_rejections.get(kind):
            self.link._dispatch(Result(
                type="result", req=command["req"], ok=False, error=rejections.popleft(),
            ))
            return
        if kind == "body_watch":
            assert not self.link.body_leases, "watch must never change with an active lease"
            if self.provider_owned:
                self.link._dispatch(Result(
                    type="result", req=command["req"], ok=False,
                    error="cannot change watch while a lease is active",
                ))
                return
            self._watched_npc = command["npcs"][0] if command["npcs"] else None
            value = {"generation": 1, "session_epoch": 1, "world_epoch": 1}
        elif kind == "story_eval":
            expressions = command["exprs"]
            value = []
            for expr in expressions:
                if expr[0] == "sym":
                    value.append(50)
                elif expr == bodypilot.TARGET_IS_HERO:
                    value.append(int(self.target_is_hero))
                else:
                    npc = expr[2][2][2][1]
                    assert expr == [
                        "&&", ["call", "NPC_GETTARGET", ["inst", "SELF"]],
                        ["==", ["call", "HLP_GETINSTANCEID", ["inst", "OTHER"]],
                         ["call", "HLP_GETINSTANCEID", ["inst", npc]]],
                    ]
                    value.append(int(not self.target_is_hero and self.target_npc == npc))
        elif kind == "body_events":
            if self.control_events:
                value = events(*self.control_events).model_dump(mode="json")
                self.control_events.clear()
            elif self._released_pending and self.release_pending_polls <= 0:
                lease = next(iter(self.link.body_leases.values()))
                value = events(event(
                    1, entity_id=lease.entity_id, reason="native_restored", owner="native",
                )).model_dump(mode="json")
                self._released_pending = False
                self.provider_owned = False
                self._activity(lease.npc, "fight")
            else:
                value = events().model_dump(mode="json")
                if self._released_pending:
                    self.release_pending_polls -= 1
        elif kind == "body_control_status":
            if self.status_error is not None:
                self.link._dispatch(Result(
                    type="result", req=command["req"], ok=False, error=self.status_error,
                ))
                return
            actors = []
            if self._control_scope is not None:
                row = {
                    **self._control_scope,
                    "control_generation": 7 if self.provider_owned else 0,
                    "acquire_request_id": (
                        self.pending_acquire["acquire_request_id"] if self.pending_acquire
                        else self._acquire_request_id if self.provider_owned else 0
                    ),
                    "acquire_pending": self.pending_acquire is not None,
                    "goal_version": 1 if self.provider_owned else 0,
                    "target_entity_id": self._control_target, "command_id": 0, "execution_id": 0,
                    "owner": "scripted" if self.provider_owned else "native",
                    "release_pending": self._released_pending,
                    "provider": "neural-body" if self.provider_owned else "",
                    "mode": "realtime" if self.provider_owned else "native",
                    "t_game_ms": 200, "observation_seq": 4, "observed_game_ms": 200,
                    "watchdog_timeout_ms": int(self._lease_timeout * 1000) if self.provider_owned else 0,
                    "control_allowed": False, "can_decide": False, "action_mask": [False] * 12,
                }
                actors.append(row)
            value = {
                "type": "body_control_status", "session_epoch": 1, "world_epoch": 1,
                "paused": False, "infrastructure_failure": False, "infrastructure_reason": "",
                "visibility_diagnostics": visibility_diagnostics(),
                "actors": actors,
            }
        else:
            value = receipt(
                entity_id=command.get("entity_id", 1),
                session_epoch=command.get("session_epoch", 1),
                world_epoch=command.get("world_epoch", 1),
                control_generation=command.get("control_generation", 7),
                goal_version=command.get("goal_version", 1),
                target_entity_id=command.get("target_entity_id", 2),
                command_id=command.get("command_id", 0),
                acquire_request_id=command.get("acquire_request_id", 0),
                observation_seq=self.link.body_latest[command.get("entity_id", 1)].observation_seq
                if command.get("entity_id", 1) in self.link.body_latest else 1,
            )
            if kind == "body_acquire" and self.acquire_rejections:
                value.update(accepted=False, reason=self.acquire_rejections.popleft())
            if kind == "body_action" and self.action_rejections:
                value.update(accepted=False, reason=self.action_rejections.popleft())
            if kind == "body_acquire" and value["accepted"]:
                if self.acquire_pending_results:
                    assert command["await_boundary_ms"] > 0
                    self.pending_acquire = command
                    self._control_scope = {name: command[name] for name in (
                        "entity_id", "session_epoch", "world_epoch",
                    )}
                    value.update(reason="acquire_pending", owner="native", control_generation=0)
                    self._background(self._resolve_pending(command, self.acquire_pending_results.popleft()))
                else:
                    self._grant(command)
            elif kind == "body_heartbeat":
                if self.provider_owned:
                    self._renew_lease()
                else:
                    value.update(accepted=False, reason="not_owned", owner="native", control_generation=0)
            if kind == "body_release":
                if self._lease_watchdog is not None:
                    self._lease_watchdog.cancel()
                if command.get("acquire_request_id") and self.pending_acquire is not None:
                    self.pending_acquire = None
                    value.update(owner="native", control_generation=0, reason="acquire_cancelled",
                                 acquire_cancel_reason="released")
                elif self.release_rejections:
                    value.update(accepted=False, reason=self.release_rejections.popleft())
                elif not self.provider_owned:
                    value.update(accepted=False, reason="not_owned", owner="native", control_generation=0)
                elif self.release_pending_polls:
                    self._released_pending = True
                    value.update(owner="scripted", reason="release_pending", release_pending=True)
                else:
                    self.provider_owned = False
                    value.update(owner="native", reason="native_restored")
                    self._activity(self._control_npc, "fight")
        result = Result(type="result", req=command["req"], ok=True, value=value)
        delay = self.response_delays.get(kind, 0)
        if delay:
            async def reply():
                await asyncio.sleep(delay)
                self.link._dispatch(result)

            # Replies can arrive later while the socket remains writable.
            self._background(reply())
        else:
            self.link._dispatch(result)

    def emit(self, *rows, generation=1):
        raw = packet(*rows, generation=generation)
        self.link._dispatch(message(raw), raw_body_packet=raw)


class MultiBridge(Bridge):
    """Native ownership keyed by actor, with both growing and legacy watches."""

    def __init__(self):
        super().__init__()
        self.watched = []
        self.actors = {}
        self.target_npcs = {}
        self.legacy_watch = False
        self.refuse_shrink = False
        self.release_ack = None
        self.acquire_ack = {}
        self.event_seq = 0
        self.observation_seq = 0
        self.pending_releases = set()

    def emit(self, *rows, generation=1):
        # The native journal sequences all actors together, including several
        # snapshots in one frame. Their original game timestamps stay intact.
        for row in rows:
            assert row["script_symbol"] in {npc_symbol(npc) for npc in self.watched}, (
                "native packets contain only subscribed actors"
            )
            self.observation_seq += 1
            row["observation_seq"] = self.observation_seq
        super().emit(*rows, generation=generation)

    async def send(self, raw):
        command = json.loads(raw)
        self.sent.append(command)
        self.sent_times.append((command, asyncio.get_running_loop().time()))
        kind = command["type"]
        if kind == "body_watch":
            npcs = command["npcs"]
            assert all(actor["npc"] in npcs for actor in self.actors.values() if actor["owned"]), (
                "body_watch must retain active body owners and pending acquisitions"
            )
            if (self.legacy_watch and self.link.body_leases and npcs != self.watched) or (
                self.refuse_shrink and set(npcs) < set(self.watched)
            ):
                self.link._dispatch(Result(
                    type="result", req=command["req"], ok=False,
                    error="release active body owners before changing body_watch",
                ))
                return
            self.watched = npcs
            value = {"generation": 1, "session_epoch": 1, "world_epoch": 1}
        elif kind == "story_eval":
            target = self.target_npcs.get(command["npc"], "HERO")
            value = [
                npc_symbol(expr[1]) if expr[0] == "sym"
                else int(target == "HERO") if expr == bodypilot.TARGET_IS_HERO
                else int(target == expr[2][2][2][1])
                for expr in command["exprs"]
            ]
        elif kind == "body_events":
            value = events().model_dump(mode="json")
        elif kind == "body_control_status":
            value = {
                "type": "body_control_status", "session_epoch": 1, "world_epoch": 1,
                "paused": False, "infrastructure_failure": False, "infrastructure_reason": "",
                "visibility_diagnostics": visibility_diagnostics(),
                "actors": [{
                    "entity_id": entity, "session_epoch": 1, "world_epoch": 1,
                    "control_generation": 7 if actor["owned"] else 0,
                    "goal_version": 1 if actor["owned"] else 0,
                    "target_entity_id": 2, "command_id": 0, "execution_id": 0,
                    "owner": "scripted" if actor["owned"] else "native",
                    "release_pending": entity in self.pending_releases,
                    "provider": "neural-body" if actor["owned"] else "",
                    "mode": "realtime" if actor["owned"] else "native",
                    "t_game_ms": 200, "observation_seq": 4, "observed_game_ms": 200,
                    "watchdog_timeout_ms": 2000, "control_allowed": False, "can_decide": False,
                    "action_mask": [False] * 12,
                } for entity, actor in self.actors.items()],
            }
        elif kind in {"body_acquire", "body_action", "body_heartbeat", "body_release"}:
            entity = command["entity_id"]
            value = receipt(
                entity_id=entity,
                session_epoch=command["session_epoch"], world_epoch=command["world_epoch"],
                control_generation=command.get("control_generation", 7),
                goal_version=command.get("goal_version", 1),
                target_entity_id=command.get("target_entity_id", 2),
                command_id=command.get("command_id", 0),
                acquire_request_id=command.get("acquire_request_id", 0),
                observation_seq=(
                    self.link.body_latest[entity].observation_seq
                    if entity in self.link.body_latest else 1
                ),
            )
            if kind == "body_acquire":
                npc = next(name for name in self.watched if npc_entity(name) == entity)
                self.actors[entity] = {"npc": npc, "owned": True}
                if npc in self.acquire_ack:
                    self.actors[entity]["owned"] = False
                    value.update(reason="acquire_pending", owner="native", control_generation=0)

                    async def grant_pending():
                        await self.acquire_ack[npc].wait()
                        self.actors[entity]["owned"] = True
                        self._activity(npc, "body")
                        self.event_seq += 1
                        self.link._dispatch(events(event(
                            self.event_seq, entity_id=entity, provider=command["provider"],
                            acquire_request_id=command["acquire_request_id"],
                            observation_seq=self.link.body_latest[entity].observation_seq,
                        )))

                    self._background(grant_pending())
                else:
                    self._activity(npc, "body")
            elif kind == "body_release":
                if self.release_ack is not None:
                    await self.release_ack.wait()
                if entity in self.pending_releases:
                    value.update(reason="release_pending", release_pending=True)
                else:
                    self.actors[entity]["owned"] = False
                    value.update(owner="native", reason="native_restored")
                    self._activity(self.actors[entity]["npc"], "fight")
            else:
                assert self.actors[entity]["owned"], f"{kind} used another fight's released actor"
                assert command["provider"] == "neural-body"
        else:
            raise AssertionError(f"unexpected multi bridge command: {kind}")
        self.link._dispatch(Result(type="result", req=command["req"], ok=True, value=value))


def npc_symbol(npc):
    return 100 + int(npc.removeprefix("NPC_"))


def npc_entity(npc):
    return 10 + int(npc.removeprefix("NPC_"))


def multi_fighter(npc, seq=2, *, owned=False, **changes):
    return fighter(seq, owned=owned, entity=npc_entity(npc), script_symbol=npc_symbol(npc), **changes)


def multi_entry(npc, *, activity="fight"):
    return RosterEntry(
        id=npc, name=f"Fighter {npc}", guild=1, distance=200.0,
        in_view=True, activity=activity, state="ZS_ATTACK",
    )


async def start_fights(rig, *npcs):
    stage = "watch admission"
    try:
        rig.bridge.roster(*(multi_entry(npc) for npc in npcs))
        await eventually(lambda: all(rig.pilot.handles(npc) for npc in npcs))
        stage = "watch acknowledgement"
        await eventually(lambda: all(rig.pilot.fights[npc]._watch_ready for npc in npcs))
        rig.bridge.emit(*(multi_fighter(npc) for npc in npcs))
        stage = "lease acquisition"
        await eventually(lambda: len(rig.link.body_leases) == len(npcs))
        stage = "policy reset request"
        await eventually(lambda: len(rig.policy.commands("reset")) == len(npcs))
        stage = "policy reset response"
        await eventually(lambda: all(
            fight.client is not None and not fight.client.pending for fight in rig.pilot.fights.values()
        ))
        rig.bridge.emit(*(multi_fighter(npc, 3, owned=True) for npc in npcs))
        stage = "first action"
        await eventually(lambda: len(rig.bridge.commands("body_action")) == len(npcs))
    except TimeoutError as error:
        raise AssertionError(f"start_fights timeout at {stage}: {rig.diagnostics()}") from error


class Policy:
    """The real JSONL wire protocol, without importing the model package."""

    def __init__(self):
        self.requests = []
        self.connections = []
        self.request_connections = {}
        self.request_timings = []
        self.started_at = asyncio.get_running_loop().time()
        self.writers = set()
        self.tasks = set()
        self.step_mode = "normal"
        self.memory_only_step = True
        self.step_fallback_reason = "fake_fallback"
        self.step_hidden_reset = True
        self.reset_mode = "normal"
        self.reset_waiter = None
        self.step_waiter = None
        self.step_delay = 0
        self.step_delays = deque()
        self.step_response_actions = deque()
        self.on_step = None

    def commands(self, kind):
        return [request for request in self.requests if request["cmd"] == kind]

    async def handle(self, reader, writer):
        self.writers.add(writer)
        self.connections.append(writer)
        self.tasks.add(asyncio.current_task())
        try:
            while line := await reader.readline():
                request = json.loads(line)
                self.requests.append(request)
                self.request_connections[id(request)] = self.connections.index(writer)
                timing = {
                    "connection": self.request_connections[id(request)],
                    "id": request["id"], "cmd": request["cmd"],
                    "seq": request.get("observation", {}).get("observation_seq"),
                    "received_ms": round((asyncio.get_running_loop().time() - self.started_at) * 1000, 3),
                    "step_mode": self.step_mode,
                }
                self.request_timings.append(timing)
                command = request["cmd"]
                result = {"id": request["id"], "schema_version": 1, "ok": True}
                if command == "reset":
                    if self.reset_waiter is not None:
                        await self.reset_waiter.wait()
                    if self.reset_mode == "error":
                        result.update(ok=False, reason="reset_error", error="fake reset failure")
                    elif self.reset_mode == "timeout":
                        continue
                    else:
                        result.update(reason="episode_reset", hidden_reset=True)
                        if self.memory_only_step and request.get("include_capabilities") is True:
                            result["memory_only_step"] = True
                elif command == "step":
                    if self.on_step is not None:
                        self.on_step(request)
                    delay = self.step_delays.popleft() if self.step_delays else self.step_delay
                    if delay:
                        await asyncio.sleep(delay)
                    if self.step_waiter is not None:
                        await self.step_waiter.wait()
                    if self.step_mode == "disconnect":
                        return
                    if self.step_mode == "timeout":
                        continue
                    if self.step_mode == "error":
                        result.update(ok=False, reason="inference_error", error="fake policy failure")
                    elif self.step_mode == "fallback":
                        result.update(
                            status="fallback", action_id=None, reason=self.step_fallback_reason,
                            process={"hidden_reset": self.step_hidden_reset},
                        )
                    else:
                        row = request["observation"]
                        eligible = (
                            row["can_decide"] and row["action_mask_valid"]
                            and not request.get("memory_only", False)
                        )
                        action = self.step_response_actions.popleft() if self.step_response_actions else 1
                        result.update(
                            status="decision" if eligible else "locked",
                            action_id=action if eligible else None,
                            reason="accepted" if eligible else "memory_only",
                        )
                elif command == "end_episode":
                    result.update(accepted=True, reason="episode_ended")
                elif command == "close":
                    result.update(reason="closed")
                else:
                    raise AssertionError(f"unexpected policy command: {command}")
                timing["write_ms"] = round((asyncio.get_running_loop().time() - self.started_at) * 1000, 3)
                writer.write(json.dumps(result).encode() + b"\n")
                await writer.drain()
                timing["replied_ms"] = round((asyncio.get_running_loop().time() - self.started_at) * 1000, 3)
                if command == "close":
                    return
        finally:
            self.writers.discard(writer)
            self.tasks.discard(asyncio.current_task())
            writer.close()
            await writer.wait_closed()

    async def close(self):
        for writer in list(self.writers):
            writer.close()
        for task in list(self.tasks):
            task.cancel()
        await asyncio.gather(*self.tasks, return_exceptions=True)


def fighter(seq=2, *, owned=False, entity=1, target_entity=2, **changes):
    row = controlled(seq) if owned else observation(seq, entity=entity)
    if owned:
        row["action_mask"] = [False, True] + [False] * 10
    row["entity_id"] = entity
    row["self"]["weapon_state"] = 2  # WeaponState::W1H; 3 is two-handed.
    row["target"].update(
        entity_id=target_entity, present=True, visible=True, current_valid=True,
        position_world_cm=[100.0, 0.0, 450.0],
        relative_position_cm=[0.0, 0.0, 150.0],
        hp=300, hp_max=300, hp_fraction=1.0, down=False,
    )
    row["goal"]["target_id"] = target_entity
    row.update(changes)
    return row


def unseen_fighter(seq, *, present=True, target_entity=2, **changes):
    row = fighter(seq, owned=True, can_decide=False, action_mask_valid=False, **changes)
    row["action_mask"] = [False] * 12
    target = row["target"]
    target.update(entity_id=target_entity, present=present, visible=False, current_valid=False)
    allowed = {
        "entity_id", "present", "visible", "current_valid", "last_known_position_world_cm",
        "last_known_velocity_cm_s", "last_seen_game_ms", "last_known_age_ms",
    }
    for key in target:
        if key not in allowed:
            target[key] = None
    return row


def player_row(seq=1):
    row = observation(seq, entity=2)
    row["script_symbol"] = 99
    row["goal"]["kind"] = "native_idle"
    row["goal"]["target_id"] = 0
    row["target"]["entity_id"] = 0
    row["target"]["present"] = False
    return row


def companion_entry():
    return RosterEntry(
        id="CAVALORN", name="Кавалорн", guild=1, distance=150.0,
        in_view=True, activity="fight", state="ZS_ATTACK",
    )


class FightHistory(dict):
    """Keep completed instances available for the existing cleanup assertions."""

    last = None

    def __setitem__(self, key, value):
        self.last = value
        super().__setitem__(key, value)


class Rig:
    def __init__(self, bridge, policy, pilot, clock):
        self.bridge, self.policy, self.pilot, self.clock = bridge, policy, pilot, clock
        self.link = bridge.link
        pilot.fights = FightHistory(pilot.fights)

    @property
    def fight(self):
        return self.pilot.fights.get("NPC") or next(
            iter(self.pilot.fights.values()), self.pilot.fights.last,
        )

    def diagnostics(self):
        """Bounded fake-only state captured before fixture cleanup changes it."""
        now = asyncio.get_running_loop().time()
        fights = list(self.pilot.fights.values())
        if not fights and self.pilot.fights.last is not None:
            fights.append(self.pilot.fights.last)
        summaries = self.pilot.last_summary
        return {
            "clock": self.clock(), "wall_ms": round(now * 1000, 3),
            "elapsed_wall_ms": round((now - self.policy.started_at) * 1000, 3),
            "active_count": self.pilot.active_count,
            "fights": [{
                "npc": fight.npc, "reason": fight._reason,
                "lease": fight.lease.model_dump(mode="json") if fight.lease else None,
                "watch_ready": fight._watch_ready, "last_seq": fight._last_seq,
                "queued_count": len(fight._rows),
                "queued_seqs": [row["observation_seq"] for row, _ in list(fight._rows)[:8]],
                "client_pending": list(fight.client.pending) if fight.client else None,
                "client_last_response_age_ms": (
                    round((now - fight.client.last_response_at) * 1000, 3)
                    if fight.client and fight.client.last_response_at is not None else None
                ),
            } for fight in fights],
            "last_summary": {
                "npc": summaries.npc, "reason": summaries.reason, "decisions": summaries.decisions,
                "policy_timeouts": summaries.policy_timeouts,
            } if summaries else None,
            "policy_requests": [{
                "cmd": request["cmd"], "id": request["id"],
                "seq": request.get("observation", {}).get("observation_seq"),
            } for request in self.policy.requests[-8:]],
            "policy_timings": self.policy.request_timings[-8:],
            "bridge_commands": [{key: value for key, value in command.items() if key in {
                "type", "req", "entity_id", "observation_seq", "npcs",
            }} for command in self.bridge.sent[-8:]],
            "body_latest_seqs": {
                entity: row.observation_seq for entity, row in self.link.body_latest.items()
            },
        }

    def roster(self, *, activity=None, state=None, guild=1, include=True, extra=()):
        if activity is None:
            activity = "body" if self.link.body_owned("NPC") else "fight"
        npcs = [RosterEntry(
            id="NPC", name="Fighter", guild=guild, distance=200.0,
            in_view=True, activity=activity,
            state=state if state is not None else (
                "ZS_ATTACK" if activity in {"body", "fight"} else "ZS_SIT_BENCH"
            ),
        )] if include else []
        self.bridge.roster(*npcs, *extra)

    def combat(self, npc="NPC", kind="attack", other="PLAYER"):
        self.link._dispatch(NpcEvent(type="npc_event", npc=npc, kind=kind, other=other, distance=200.0))

    async def watch(self, **roster):
        self.roster(**roster)
        self.combat()
        await eventually(lambda: self.bridge.commands("body_watch"))

    async def acquire(self, *, entity=1, target_entity=2, extra=()):
        await self.watch(extra=extra)
        self.bridge.emit(player_row(), fighter(entity=entity, target_entity=target_entity))
        await eventually(lambda: self.link.body_leases)

    async def active(self, *, entity=1, target_entity=2, extra=()):
        stage = "lease acquisition"
        try:
            resets = len(self.policy.commands("reset"))
            await self.acquire(entity=entity, target_entity=target_entity, extra=extra)
            stage = "policy reset request"
            await eventually(lambda: len(self.policy.commands("reset")) == resets + 1)
            stage = "policy reset response"
            await eventually(lambda: self.fight.client is not None and not self.fight.client.pending)
            self.bridge.emit(fighter(3, owned=True, entity=entity, target_entity=target_entity))
            stage = "first policy step"
            await eventually(lambda: self.policy.commands("step"))
            stage = "first action"
            await eventually(lambda: self.bridge.commands("body_action"))
        except TimeoutError as error:
            raise AssertionError(f"Rig.active timeout at {stage}: {self.diagnostics()}") from error

    async def ended(self, *, policy=True):
        await eventually(lambda: self.bridge.commands("body_release"))
        await eventually(lambda: any(not cmd["npcs"] for cmd in self.bridge.commands("body_watch")))
        if policy:
            await eventually(lambda: self.policy.commands("end_episode"))
        assert not self.link.body_leases

    async def retarget(self, seq, *, target_entity, target_npc=None):
        releases = len(self.bridge.commands("body_release"))
        episodes = len(self.policy.commands("end_episode"))
        self.bridge.target_is_hero = target_npc is None
        self.bridge.target_npc = target_npc
        self.bridge.emit(fighter(seq, owned=True, target_entity=target_entity))
        await eventually(lambda: len(self.bridge.commands("body_release")) == releases + 1)
        await eventually(lambda: len(self.policy.commands("end_episode")) == episodes + 1)
        await eventually(lambda: self.pilot.active_count == 0)
        assert not self.link.body_leases
        assert self.pilot.last_summary.reason == "target_changed"

    async def reacquire(self, seq, *, target_entity, count):
        await eventually(lambda: len(self.bridge.commands("body_watch")) == 2 * count - 1)
        self.bridge.emit(fighter(seq, target_entity=target_entity))
        await eventually(lambda: len(self.bridge.commands("body_acquire")) == count)
        await eventually(lambda: len(self.policy.commands("reset")) == count)
        assert self.fight.lease.target_entity_id == target_entity

    async def lose_heartbeat_before_native_event(self, *, target_entity, evidence="matching", down=False):
        original_heartbeat = self.link.body_heartbeat
        heartbeat_errors = []

        async def record_heartbeat(*args, **kwargs):
            try:
                return await original_heartbeat(*args, **kwargs)
            except GameError as error:
                heartbeat_errors.append(str(error))
                raise

        self.link.body_heartbeat = record_heartbeat
        # Keep the ordinary event poll out of this handoff. The next heartbeat
        # discovers native ownership; cleanup must then consume its ownership event.
        self.fight._last_events_poll = float("inf")
        first_command = len(self.bridge.sent)
        self.bridge.restore_native(release_reason="target_changed_or_lost")
        if evidence == "missing":
            self.bridge.control_events.clear()
        elif evidence == "foreign":
            self.bridge.control_events[0]["control_generation"] += 1
        row = fighter(4, target_entity=target_entity, periodic=False, event_only=True)
        row["target"]["down"] = down
        self.bridge.emit(row)
        await eventually(lambda: self.pilot.last_summary is not None)
        assert heartbeat_errors == ["body command rejected: not_owned"]
        commands = [command["type"] for command in self.bridge.sent[first_command:]]
        assert (
            commands.index("body_heartbeat") < commands.index("body_release") < commands.index("body_events")
        )
        return self.pilot.last_summary


@asynccontextmanager
async def running(
    *, companion=False, unavailable=False, talking=False,
    max_fights=4, multi=False, protocol=PROTOCOL_VERSION,
):
    bridge, policy, clock = MultiBridge() if multi else Bridge(), Policy(), Clock()
    if protocol is not None:
        bridge.link._dispatch(Hello(type="hello", protocol=protocol, client="fake", codepage="utf-8"))
    server = await asyncio.start_server(policy.handle, host="127.0.0.1", port=0)
    host, port = server.sockets[0].getsockname()
    pilot = BodyPilot(
        bridge.link, f"tcp://{host}:{port}",
        companion=companion if callable(companion) else lambda _: companion,
        unavailable=lambda _: unavailable, clock=clock, max_fights=max_fights,
        talking=talking if callable(talking) else lambda _: talking,
    )
    task = asyncio.create_task(pilot.run())
    try:
        yield Rig(bridge, policy, pilot, clock)
    finally:
        await pilot.aclose()
        await asyncio.wait_for(task, 1.0)
        server.close()
        await server.wait_closed()
        await policy.close()
        await bridge.close()


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


@pytest.mark.parametrize("shutdown", [False, True])
async def test_lost_acquire_reply_reconciles_native_lease_before_clearing_watch(shutdown):
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
        if shutdown:
            await asyncio.wait_for(rig.pilot.aclose(), 1.0)
        await eventually(lambda: rig.bridge.commands("body_release"))
        await eventually(lambda: any(not cmd["npcs"] for cmd in rig.bridge.commands("body_watch")))
        await eventually(lambda: rig.pilot.active_count == 0)
        # Drain the delayed reply and roster; neither may revive the discarded lease.
        await eventually(lambda: not rig.bridge._response_tasks)
        assert not rig.bridge.provider_owned
        assert not rig.link.body_leases
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

        if not shutdown:
            # Cleanup leaves the same pilot available for a later fight.
            rig.bridge.response_delays.clear()
            rig.bridge.acquire_roster_delay = 0
            rig.roster(activity="routine", state="ZS_SIT_BENCH")
            await rig.watch()
            await eventually(lambda: rig.fight._watch_ready)
            rig.bridge.emit(player_row(4), fighter(5))
            await eventually(lambda: rig.link.body_leases)
            rig.bridge.emit(fighter(6, owned=True))
            await eventually(lambda: rig.bridge.commands("body_action"))
            assert len(rig.bridge.commands("body_acquire")) == 2
            assert len(rig.policy.commands("reset")) == 1


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


async def test_memory_only_periodic_rows_advance_policy_without_sending_commands():
    async with running() as rig:
        await rig.active()
        count = len(rig.bridge.commands("body_action"))
        own = fighter(4, owned=True, can_decide=False, action_mask_valid=False)
        rig.bridge.emit(own)
        await eventually(lambda: len(rig.policy.commands("step")) == 2)
        await asyncio.sleep(0.01)
        assert rig.policy.commands("step")[-1]["observation"] == own
        assert len(rig.bridge.commands("body_action")) == count


async def test_reset_precedes_first_periodic_row_with_the_acquired_identity():
    async with running() as rig:
        await rig.acquire()
        await eventually(lambda: rig.policy.commands("reset"))
        rig.bridge.emit(fighter(3, owned=True, periodic=False, event_only=True))
        await asyncio.sleep(0.01)
        assert not rig.policy.commands("step")
        own = fighter(4, owned=True, can_decide=False)
        rig.bridge.emit(own)
        await eventually(lambda: rig.policy.commands("step"))
        assert rig.policy.commands("reset")[0]["initial_game_ms"] == fighter()["t_game_ms"]
        assert rig.policy.commands("step")[0]["observation"] == own
        assert not rig.bridge.commands("body_action")


async def test_lease_heartbeats_run_while_acquire_waits_for_policy_reset(monkeypatch):
    monkeypatch.setattr(bodypilot, "HEARTBEAT_INTERVAL", 0.010)
    monkeypatch.setattr(bodypilot, "SERVICE_TIMEOUT", 0.500)
    async with running() as rig:
        rig.policy.reset_waiter = asyncio.Event()
        await rig.acquire()
        await eventually(lambda: rig.policy.commands("reset"))
        rig.bridge.emit(fighter(3, owned=True))
        await eventually(lambda: len(rig.bridge.commands("body_heartbeat")) >= 3)
        assert not rig.policy.commands("step")
        assert rig.link.body_owned("NPC") and rig.bridge.lease_expirations == 0
        rig.policy.reset_waiter.set()
        await eventually(lambda: rig.bridge.commands("body_action"))
        assert [request["cmd"] for request in rig.policy.requests] == ["reset", "step"]


async def test_foreign_old_event_only_rows_do_not_reach_policy_and_packet_time_is_current():
    async with running() as rig:
        await rig.active()
        old = fighter(3, owned=True)
        rig.bridge.emit(old)  # GameLink rejects the stale packet before listeners.
        foreign = fighter(4, owned=True, entity=4)
        event_only = fighter(5, owned=True, periodic=False, event_only=True)
        rig.bridge.emit(foreign, event_only)
        own = fighter(6, owned=True)
        latest = player_row(7)
        rig.bridge.emit(own, latest)
        await eventually(lambda: len(rig.policy.commands("step")) == 2)
        assert [step["observation"]["observation_seq"] for step in rig.policy.commands("step")] == [3, 6]
        assert rig.policy.commands("step")[-1]["now_game_ms"] == latest["t_game_ms"]


async def test_every_owned_periodic_row_in_a_packet_reaches_policy_as_received():
    async with running() as rig:
        await rig.active()
        first = fighter(4, owned=True, can_decide=False)
        second = fighter(5, owned=True, can_decide=False)
        rig.bridge.emit(first, second)
        await eventually(lambda: len(rig.policy.commands("step")) == 3)
        steps = rig.policy.commands("step")[-2:]
        assert [step["observation"] for step in steps] == [first, second]
        assert all(step["now_game_ms"] == second["t_game_ms"] for step in steps)
        assert len(rig.bridge.commands("body_action")) == 1


async def test_frame_hitch_burst_updates_memory_in_order_and_only_freshest_row_acts():
    async with running() as rig:
        await rig.active()
        rows = [fighter(seq, owned=True) for seq in range(4, 10)]
        assert rows[-1]["t_game_ms"] - rows[0]["t_game_ms"] == 250
        rig.bridge.emit(*rows)
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 2)
        steps = rig.policy.commands("step")[-6:]
        assert [step["observation"] for step in steps] == rows
        assert [step.get("memory_only", False) for step in steps] == [True] * 5 + [False]
        assert all(step["now_game_ms"] == rows[-1]["t_game_ms"] for step in steps)
        assert [command["observation_seq"] for command in rig.bridge.commands("body_action")] == [3, 9]
        assert len(rig.policy.commands("reset")) == 1
        assert rig.link.body_owned("NPC") and not rig.bridge.commands("body_release")
        # The row following a burst belongs to the same uninterrupted episode.
        rig.bridge.emit(fighter(10, owned=True))
        await eventually(lambda: rig.fight._decisions == 3)
        await rig.pilot.aclose()
        summary = rig.pilot.last_summary
        assert summary.memory_only_lag_rows == 5
        assert summary.observation_too_old == summary.policy_memory_resets == 0
        assert summary.max_batch_rows == 6 and summary.max_batch_lag_game_ms == 250


@pytest.mark.parametrize("age_ms, memory_only", [(100, False), (101, True)])
async def test_latest_owned_row_uses_memory_only_after_one_hundred_game_ms(age_ms, memory_only):
    async with running() as rig:
        await rig.active()
        own = fighter(4, owned=True)
        newer = player_row(10)
        newer["t_game_ms"] = own["t_game_ms"] + age_ms
        rig.bridge.emit(own, newer)
        await eventually(lambda: len(rig.policy.commands("step")) == 2)
        if memory_only:
            await asyncio.sleep(0.01)
        else:
            await eventually(lambda: len(rig.bridge.commands("body_action")) == 2)
        step = rig.policy.commands("step")[-1]
        assert step["observation"] == own
        assert step.get("memory_only", False) is memory_only
        assert step["now_game_ms"] == newer["t_game_ms"]
        assert len(rig.bridge.commands("body_action")) == (1 if memory_only else 2)
        assert rig.link.body_owned("NPC") and not rig.bridge.commands("body_release")
        await rig.pilot.aclose()
        assert rig.pilot.last_summary.memory_only_lag_rows == int(memory_only)


async def test_legacy_service_gets_original_step_shape_and_historical_actions_are_suppressed():
    async with running() as rig:
        rig.policy.memory_only_step = False
        await rig.active()
        rows = [fighter(seq, owned=True) for seq in range(4, 10)]
        rig.bridge.emit(*rows)
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 2)
        steps = rig.policy.commands("step")
        assert all(set(step) == {"cmd", "id", "observation", "now_game_ms"}
                   for step in steps)
        assert [step["observation"] for step in steps[-6:]] == rows
        assert [command["observation_seq"] for command in rig.bridge.commands("body_action")] == [3, 9]
        assert rig.link.body_owned("NPC") and not rig.bridge.commands("body_release")
        # Even a lone historical row must not act when an old service ignores its age.
        old = fighter(10, owned=True)
        newer = player_row(12)
        newer["t_game_ms"] = old["t_game_ms"] + 101
        rig.bridge.emit(old, newer)
        await eventually(lambda: len(rig.policy.commands("step")) == 8)
        await asyncio.sleep(0.01)
        assert len(rig.bridge.commands("body_action")) == 2
        assert "memory_only" not in rig.policy.commands("step")[-1]


async def test_policy_backlog_does_not_starve_lease_heartbeats():
    async with running() as rig:
        await rig.active()
        processed = []
        rig.policy.step_delay = 0.075

        def step_received(request):
            processed.append((
                request["observation"]["observation_seq"],
                asyncio.get_running_loop().time(),
            ))

        rig.policy.on_step = step_received
        sequences = list(range(4, 13))
        rig.bridge.emit(*(fighter(seq, owned=True, can_decide=False) for seq in sequences))
        await eventually(lambda: len(processed) == len(sequences), timeout=2.0)
        assert [seq for seq, _ in processed] == sequences
        heartbeats_during_steps = [when for when in rig.bridge.command_times("body_heartbeat")
                                   if processed[0][1] <= when <= processed[-1][1]]
        assert len(heartbeats_during_steps) >= 2
        assert rig.bridge.lease_expirations == 0
        assert rig.link.body_owned("NPC")
        assert not rig.bridge.commands("body_release")


async def test_delayed_heartbeat_and_event_replies_do_not_accumulate_into_lease_expiry():
    async with running() as rig:
        rig.bridge.response_delays.update(body_heartbeat=0.200, body_events=0.240)
        await rig.active()
        assert rig.bridge.commands("body_acquire")[0]["timeout_ms"] == 2000
        try:
            await eventually(lambda: len(rig.bridge.commands("body_heartbeat")) >= 10, timeout=3.5)
        except TimeoutError as error:
            sent = rig.bridge.command_times("body_heartbeat")
            span = sent[-1] - sent[0] if sent else 0.0
            max_gap = max((later - earlier for earlier, later in pairwise(sent)), default=0.0)
            raise AssertionError(
                f"heartbeat timing: count={len(sent)} span_s={span:.3f} max_gap_s={max_gap:.3f}"
            ) from error
        sent = rig.bridge.command_times("body_heartbeat")
        assert sent[-1] - sent[0] >= 2.0
        assert max(later - earlier for earlier, later in pairwise(sent)) < 0.400
        assert len(rig.bridge.commands("body_events")) >= 3
        assert rig.bridge.lease_expirations == 0
        assert rig.link.body_owned("NPC")
        assert not rig.bridge.commands("body_release")
        assert not rig.policy.commands("end_episode")


async def test_one_ownership_poll_timeout_keeps_lease_and_recovers_at_regular_interval(caplog):
    caplog.set_level("INFO", logger="gothic_ai.bodypilot")
    async with running() as rig:
        await rig.active()
        fight, lease = rig.fight, rig.fight.lease
        last_success = fight._last_events_success_at
        polls = len(rig.bridge.commands("body_events"))
        rig.bridge.response_delays["body_events"] = bodypilot.BRIDGE_TIMEOUT + 0.100
        fight._last_events_poll = float("-inf")
        fight._wake.set()
        await eventually(lambda: len(rig.bridge.commands("body_events")) == polls + 1)
        # Only this reply is delayed. It still arrives after the RPC timed out,
        # so recovery must come from a fresh poll rather than its stale result.
        rig.bridge.response_delays.clear()
        await eventually(lambda: fight._ownership_poll_timeouts == 1)
        assert fight._last_events_success_at == last_success
        assert rig.pilot.handles("NPC") and rig.link.body_leases == {lease.entity_id: lease}
        assert fight._reason is None and rig.bridge.lease_expirations == 0
        assert not rig.bridge.commands("body_release") and not rig.policy.commands("end_episode")
        await eventually(lambda: fight._last_events_success_at > last_success)
        first_poll, next_poll = rig.bridge.command_times("body_events")[polls:polls + 2]
        assert next_poll - first_poll >= bodypilot.BRIDGE_TIMEOUT + bodypilot.EVENTS_INTERVAL - 0.010
        assert any(first_poll <= when <= next_poll for when in rig.bridge.command_times("body_heartbeat"))
        assert fight._ownership_poll_timeouts == 1
        rig.bridge.emit(fighter(4, owned=True))
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 2)
        await rig.pilot.aclose()
        summary = rig.pilot.last_summary
        assert summary.ownership_poll_timeouts == 1
        assert summary.ownership_poll_max_gap_ms >= (next_poll - first_poll) * 1000
        end = [record.message for record in caplog.records if "neural-body end " in record.message]
        assert len(end) == 1 and "ownership_poll_timeouts=1" in end[0]
        assert f"ownership_poll_max_gap_ms={summary.ownership_poll_max_gap_ms:.1f}" in end[0]


async def test_shared_ownership_poll_timeout_preserves_three_fights(monkeypatch):
    async with running(multi=True) as rig:
        npcs = ("NPC_1", "NPC_2", "NPC_3")
        await start_fights(rig, *npcs)
        fights = {npc: rig.pilot.fights[npc] for npc in npcs}
        leases = dict(rig.link.body_leases)
        successes = {npc: fight._last_events_success_at for npc, fight in fights.items()}
        original_dispatch = rig.link._dispatch
        held = []
        paused = True

        def dispatch(result, **kwargs):
            if paused and isinstance(result, Result) and any(
                command["type"] == "body_events" and command["req"] == result.req
                for command in rig.bridge.sent
            ):
                held.append(result)
                return
            original_dispatch(result, **kwargs)

        monkeypatch.setattr(rig.link, "_dispatch", dispatch)
        for fight in fights.values():
            fight._last_events_poll = float("-inf")
            fight._wake.set()
        await eventually(lambda: len(held) == 3)
        await eventually(lambda: all(fight._ownership_poll_timeouts == 1 for fight in fights.values()))
        assert rig.pilot.active_count == 3 and rig.link.body_leases == leases
        assert all(rig.pilot.fights[npc] is fights[npc] and fights[npc]._reason is None for npc in npcs)
        assert not rig.bridge.commands("body_release") and not rig.policy.commands("end_episode")
        paused = False
        for result in held:
            original_dispatch(result)
        await eventually(lambda: all(
            fight._last_events_success_at > successes[npc] for npc, fight in fights.items()
        ))
        assert rig.pilot.active_count == 3 and rig.link.body_leases == leases
        rig.bridge.emit(*(multi_fighter(npc, 4, owned=True) for npc in npcs))
        await eventually(lambda: len(rig.bridge.commands("body_action")) == 6)
        await rig.pilot.aclose()
        assert len(rig.pilot.summaries) == 3
        assert all(summary.ownership_poll_timeouts == 1 for summary in rig.pilot.summaries)


async def test_ownership_poll_silence_expires_after_two_wall_seconds_with_game_time_stopped(monkeypatch):
    # Keep the independent policy-silence watchdog out of this ownership test.
    monkeypatch.setattr(bodypilot, "POLICY_RESPONSE_TIMEOUT", 5.0)
    async with running() as rig:
        await rig.active()
        fight, lease = rig.fight, rig.fight.lease
        last_success = fight._last_events_success_at
        clock = rig.clock()
        steps, actions = len(rig.policy.commands("step")), len(rig.bridge.commands("body_action"))
        rig.bridge.response_delays["body_events"] = 5.0
        fight._last_events_poll = float("-inf")
        fight._wake.set()
        await eventually(lambda: rig.bridge.commands("body_release"), timeout=3.0)
        release_at = rig.bridge.command_times("body_release")[0]
        rig.bridge.response_delays.clear()
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        assert release_at - last_success > 2.0
        assert summary.reason == "lease_lost" and summary.end_trigger == "lease_lost"
        assert summary.ownership_poll_timeouts >= 2
        assert summary.ownership_poll_max_gap_ms > 2000
        assert rig.clock() == clock and summary.fight_seconds == 0
        assert len(rig.policy.commands("step")) == steps
        assert len(rig.bridge.commands("body_action")) == actions
        beats = [when for when in rig.bridge.command_times("body_heartbeat") if when < release_at]
        assert len(beats) >= 7 and rig.bridge.lease_expirations == 0
        assert summary.lease == lease and summary.release_confirmed and summary.watch_removed


async def test_successful_ownership_poll_arriving_after_lease_deadline_cannot_revive_fight(monkeypatch):
    monkeypatch.setattr(bodypilot, "BRIDGE_TIMEOUT", 2.5)
    monkeypatch.setattr(bodypilot, "POLICY_RESPONSE_TIMEOUT", 5.0)
    async with running() as rig:
        await rig.active()
        fight = rig.fight
        polls = len(rig.bridge.commands("body_events"))
        rig.bridge.response_delays["body_events"] = 2.1
        fight._last_events_poll = float("-inf")
        fight._wake.set()
        await eventually(lambda: len(rig.bridge.commands("body_events")) == polls + 1)
        rig.bridge.response_delays.clear()
        await eventually(lambda: rig.bridge.commands("body_release"), timeout=3.0)
        await eventually(lambda: rig.pilot.last_summary is not None, timeout=3.0)
        summary = rig.pilot.last_summary
        assert summary.reason == "lease_lost"
        assert summary.ownership_poll_timeouts == 0 and summary.ownership_poll_max_gap_ms > 2000
        assert rig.bridge.lease_expirations == 0


async def test_ownership_poll_bridge_error_still_ends_fight_immediately(caplog):
    caplog.set_level("WARNING", logger="gothic_ai.bodypilot")
    async with running() as rig:
        await rig.active()
        fight = rig.fight
        polls = len(rig.bridge.commands("body_events"))
        rig.bridge.request_rejections["body_events"] = deque(["fake ownership poll rejected"])
        fight._last_events_poll = float("-inf")
        fight._wake.set()
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        poll_at = rig.bridge.command_times("body_events")[polls]
        assert rig.bridge.command_times("body_release")[0] - poll_at < bodypilot.BRIDGE_TIMEOUT
        assert summary.reason == "lease_lost" and summary.ownership_poll_timeouts == 0
        assert summary.release_confirmed and summary.watch_removed
        failures = [record for record in caplog.records if "ownership poll failed " in record.message]
        assert len(failures) == 1 and failures[0].levelname == "WARNING"
        assert "error_type=GameError" in failures[0].message


async def test_missing_lease_during_ownership_poll_timeout_ends_fight():
    async with running() as rig:
        await rig.active()
        fight, lease = rig.fight, rig.fight.lease
        polls = len(rig.bridge.commands("body_events"))
        rig.bridge.response_delays["body_events"] = bodypilot.BRIDGE_TIMEOUT + 0.100
        fight._last_events_poll = float("-inf")
        fight._wake.set()
        await eventually(lambda: len(rig.bridge.commands("body_events")) == polls + 1)
        rig.bridge.response_delays.clear()
        rig.link.body_leases.pop(lease.entity_id)
        await eventually(lambda: rig.pilot.last_summary is not None)
        assert rig.pilot.last_summary.reason == "lease_lost"
        assert rig.pilot.last_summary.ownership_poll_timeouts == 1
        assert rig.pilot.last_summary.release_confirmed and rig.pilot.last_summary.watch_removed


@pytest.mark.parametrize("transition", ["loading", "world_changed"])
async def test_world_transition_ownership_poll_timeout_is_info_and_preserves_reason(transition, caplog):
    caplog.set_level("INFO", logger="gothic_ai.bodypilot")
    async with running() as rig:
        await rig.active()
        fight = rig.fight
        polls = len(rig.bridge.commands("body_events"))
        rig.bridge.response_delays["body_events"] = bodypilot.BRIDGE_TIMEOUT + 0.100
        fight._last_events_poll = float("-inf")
        fight._wake.set()
        await eventually(lambda: len(rig.bridge.commands("body_events")) == polls + 1)
        rig.bridge.response_delays.clear()
        if transition == "loading":
            rig.link._dispatch(Loading(type="loading"))
        else:
            rig.bridge.emit(fighter(4, owned=True, generation=2, world_epoch=2), generation=2)
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        assert summary.reason == transition and summary.ownership_poll_timeouts == 1
        polls = [record for record in caplog.records if "ownership poll " in record.message]
        assert len(polls) == 1 and polls[0].levelname == "INFO"
        assert "error_type=TimeoutError" in polls[0].message
        assert not any(record.levelname == "WARNING" for record in polls)


async def test_heartbeat_and_event_requests_overlap_with_acknowledgement_gates():
    async with running() as rig:
        original_dispatch = rig.link._dispatch
        original_heartbeat = rig.link.body_heartbeat
        held_heartbeat = None
        held_event = None
        heartbeat_receipts = []
        paired_polls = []

        def dispatch(result, **kwargs):
            nonlocal held_heartbeat, held_event
            if isinstance(result, Result):
                command = next(command for command in reversed(rig.bridge.sent)
                               if command.get("req") == result.req)
                if command["type"] == "body_heartbeat":
                    assert held_heartbeat is None
                    # A main loop waiting for heartbeat before polling events
                    # would deadlock here. Its independent keepalive must overlap.
                    held_heartbeat = result
                    return
                if command["type"] == "body_events" and held_heartbeat is not None:
                    assert held_event is None
                    held_event = result
                    paired_polls.append(asyncio.get_running_loop().time())
                    reply, held_heartbeat = held_heartbeat, None
                    original_dispatch(reply)
                    return
            original_dispatch(result, **kwargs)

        async def heartbeat(*args, **kwargs):
            nonlocal held_event
            result = await original_heartbeat(*args, **kwargs)
            heartbeat_receipts.append(asyncio.get_running_loop().time())
            # The event response waits for an acknowledged heartbeat, giving
            # deterministic overlap without scheduling near the RPC deadline.
            if held_event is not None:
                reply, held_event = held_event, None
                original_dispatch(reply)
            return result

        rig.link._dispatch = dispatch
        rig.link.body_heartbeat = heartbeat
        try:
            await rig.active()
            assert rig.bridge.commands("body_acquire")[0]["timeout_ms"] == 2000
            await eventually(lambda: len(heartbeat_receipts) >= 10, timeout=3.5)
            sent = rig.bridge.command_times("body_heartbeat")
            assert len(sent) >= 10 and sent[-1] - sent[0] >= 2.0
            assert max(later - earlier for earlier, later in pairwise(sent)) < 0.400
            assert len(rig.bridge.commands("body_events")) >= 3
            assert len(paired_polls) == len(heartbeat_receipts) >= 10
            assert all(ack >= poll for poll, ack in zip(paired_polls, heartbeat_receipts, strict=True))
            assert rig.bridge.lease_expirations == 0
            assert rig.link.body_owned("NPC")
            assert not rig.bridge.commands("body_release")
            assert not rig.policy.commands("end_episode")
        finally:
            rig.link._dispatch = original_dispatch
            rig.link.body_heartbeat = original_heartbeat


async def test_old_world_packets_are_excluded_without_rewinding_active_lease():
    async with running() as rig:
        await rig.active()
        old = fighter(4, owned=True, generation=0, world_epoch=0)
        rig.bridge.emit(old, generation=0)
        await asyncio.sleep(0.01)
        assert len(rig.policy.commands("step")) == 1
        assert rig.link.body_leases and not rig.bridge.commands("body_release")


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
        assert summary.service_p50_ms == latencies[1]
        assert summary.service_p95_ms == latencies[-1]
        assert summary.acquire_refusals == summary.action_rejections == ()
        assert summary.transient_rejections_consecutive == 0
        assert summary.fight_seconds == pytest.approx(0.75)
        assert summary.release_confirmed
        assert 0 <= summary.release_seconds < bodypilot.FINISH_TIMEOUT
        assert summary.watch_removed
        assert rig.fight.npc is rig.fight.lease is rig.fight.client is None
        assert not rig.link.body_leases and rig.bridge._watched_npc is None
        assert not rig.fight._latencies and rig.fight._decisions == rig.fight._fallbacks == 0
        with pytest.raises(FrozenInstanceError):
            summary.decisions = 100

        rig.roster(activity="routine", state="ZS_SIT_BENCH")
        await rig.watch()
        assert rig.pilot.last_summary is summary
        assert summary.decisions == 3


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


@pytest.mark.parametrize("rejections", [
    ["stale_observation"] * 9,
    ["masked"] * 9,
    ["masked", "stale_observation", "expired", "expired_wall", "target_invalid",
     "command_busy", "locked", "masked", "stale_observation"],
])
async def test_transient_action_rejections_wait_for_a_fresh_row_and_success_resets_streak(rejections):
    async with running() as rig:
        await rig.active()
        rig.bridge.action_rejections.extend(rejections)
        for count, seq in enumerate(range(4, 13), 1):
            rig.bridge.emit(fighter(seq, owned=True))
            await eventually(lambda count=count: rig.fight._transient_rejections_consecutive == count)
            assert rig.link.body_owned("NPC")
            assert not rig.bridge.commands("body_release")
        rig.bridge.emit(fighter(13, owned=True, can_decide=False))
        await eventually(lambda: len(rig.policy.commands("step")) == 11)
        assert rig.fight._transient_rejections_consecutive == 9  # Memory-only is not an accepted action.
        rig.bridge.emit(fighter(14, owned=True))
        await eventually(lambda: rig.fight._decisions == 2)
        assert rig.fight._transient_rejections_consecutive == 0
        # More than ten total refusals are allowed when accepted actions reset the streak.
        rig.bridge.action_rejections.append("masked")
        rig.bridge.emit(fighter(15, owned=True))
        await eventually(lambda: rig.fight._transient_rejections_consecutive == 1)
        assert rig.link.body_owned("NPC") and not rig.bridge.commands("body_release")
        rig.bridge.emit(fighter(16, owned=True))
        await eventually(lambda: rig.fight._decisions == 3)
        assert rig.fight._transient_rejections_consecutive == 0
        rig.bridge.action_rejections.append("stale_observation")
        rig.bridge.emit(fighter(17, owned=True))
        await eventually(lambda: rig.fight._transient_rejections_consecutive == 1)
        assert rig.link.body_owned("NPC") and not rig.bridge.commands("body_release")
        row = fighter(18, owned=True)
        row["self"]["down"] = True
        rig.bridge.emit(row)
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        assert summary.reason == "npc_down" and summary.decisions == 3
        assert dict(summary.action_rejections) == Counter([*rejections, "masked", "stale_observation"])
        assert summary.transient_rejections_consecutive == 1
        assert not rig.fight._action_rejections and rig.fight._transient_rejections_consecutive == 0


@pytest.mark.parametrize("hidden_reset", [True, False])
async def test_one_old_observation_fallback_continues_and_resets_before_next_row_if_needed(hidden_reset):
    async with running() as rig:
        await rig.active()
        original_call = rig.fight.client.call
        client_timings = []

        async def call(command, **fields):
            def now_ms():
                return round((asyncio.get_running_loop().time() - rig.policy.started_at) * 1000, 3)

            timing = {"cmd": command, "seq": fields.get("observation", {}).get("observation_seq"),
                      "started_ms": now_ms()}
            client_timings.append(timing)
            try:
                return await original_call(command, **fields)
            except Exception as error:
                timing["error"] = type(error).__name__
                raise
            finally:
                timing["finished_ms"] = now_ms()
                last_response = rig.fight.client.last_response_at
                timing["last_response_ms"] = (
                    round((last_response - rig.policy.started_at) * 1000, 3) if last_response else None
                )

        rig.fight.client.call = call
        rig.policy.step_mode = "fallback"
        rig.policy.step_fallback_reason = "observation_too_old"
        rig.policy.step_hidden_reset = hidden_reset
        rig.bridge.emit(fighter(4, owned=True))
        try:
            await eventually(lambda: rig.fight._transient_rejections_consecutive == 1)
        except TimeoutError as error:
            raise AssertionError(
                f"fallback response missing: client={client_timings}; policy={rig.policy.request_timings}"
            ) from error
        assert rig.link.body_owned("NPC") and not rig.bridge.commands("body_release")
        assert len(rig.bridge.commands("body_action")) == 1
        assert len(rig.policy.commands("reset")) == 1
        rig.policy.step_mode = "normal"
        next_row = fighter(5, owned=True)
        rig.bridge.emit(next_row)
        await eventually(lambda: rig.fight._decisions == 2)
        assert rig.fight._transient_rejections_consecutive == 0
        assert [command["observation_seq"] for command in rig.bridge.commands("body_action")] == [3, 5]
        commands = [request["cmd"] for request in rig.policy.requests]
        assert commands == ["reset", "step", "step"] + (["reset"] if hidden_reset else []) + ["step"]
        if hidden_reset:
            reset = rig.policy.commands("reset")[-1]
            assert (reset["session_epoch"], reset["world_epoch"], reset["initial_game_ms"]) == (
                next_row["session_epoch"], next_row["world_epoch"], next_row["t_game_ms"],
            )
        await rig.pilot.aclose()
        summary = rig.pilot.last_summary
        assert summary.observation_too_old == summary.fallbacks == 1
        assert summary.policy_memory_resets == int(hidden_reset)
        assert summary.transient_rejections_consecutive == 0
        assert summary.memory_only_lag_rows == 0


async def test_ten_old_observation_fallbacks_end_fight_and_report_memory_resets(caplog):
    caplog.set_level("INFO", logger="gothic_ai.bodypilot")
    async with running() as rig:
        await rig.active()
        rig.policy.step_mode = "fallback"
        rig.policy.step_fallback_reason = "observation_too_old"
        for count, seq in enumerate(range(4, 13), 1):
            rig.bridge.emit(fighter(seq, owned=True))
            await eventually(lambda count=count: rig.fight._transient_rejections_consecutive == count)
            assert rig.link.body_owned("NPC") and not rig.bridge.commands("body_release")
        rig.bridge.emit(fighter(13, owned=True))
        await rig.ended()
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        assert summary.reason == "policy_fallback:observation_too_old"
        assert summary.decisions == 1 and len(rig.bridge.commands("body_action")) == 1
        assert summary.observation_too_old == summary.policy_memory_resets == summary.fallbacks == 10
        assert summary.transient_rejections_consecutive == 10
        assert len(rig.policy.commands("reset")) == 10
        assert summary.release_confirmed and summary.watch_removed
        assert "observation_too_old=10" in caplog.text and "policy_memory_resets=10" in caplog.text


@pytest.mark.parametrize("last_failure", ["observation_too_old", "masked"])
async def test_old_observation_and_bridge_rejections_share_consecutive_limit(last_failure):
    async with running() as rig:
        await rig.active()
        rig.policy.step_fallback_reason = "observation_too_old"
        failures = ["observation_too_old", "masked"] * 4 + ["masked", last_failure]
        for count, (seq, failure) in enumerate(zip(range(4, 14), failures, strict=True), 1):
            rig.policy.step_mode = "fallback" if failure == "observation_too_old" else "normal"
            if failure == "masked":
                rig.bridge.action_rejections.append("masked")
            rig.bridge.emit(fighter(seq, owned=True))
            if count < 10:
                await eventually(lambda count=count: rig.fight._transient_rejections_consecutive == count)
                assert rig.link.body_owned("NPC") and not rig.bridge.commands("body_release")
        await rig.ended()
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        expected_old = failures.count("observation_too_old")
        assert summary.transient_rejections_consecutive == 10
        assert summary.observation_too_old == summary.policy_memory_resets == expected_old
        assert dict(summary.action_rejections) == {"masked": failures.count("masked")}
        assert summary.reason == (
            "policy_fallback:observation_too_old" if last_failure == "observation_too_old"
            else "body_action_rejected"
        )
        assert summary.decisions == 1 and summary.release_confirmed and summary.watch_removed


@pytest.mark.parametrize("rejections", [
    ["stale_observation"] * 10,
    ["masked"] * 10,
    ["masked", "stale_observation"] * 5,
])
async def test_ten_consecutive_transient_action_rejections_restore_native_and_are_reported(
    caplog, rejections,
):
    caplog.set_level("INFO", logger="gothic_ai.bodypilot")
    async with running() as rig:
        await rig.active()
        rig.bridge.action_rejections.extend(rejections)
        for count, seq in enumerate(range(4, 13), 1):
            rig.bridge.emit(fighter(seq, owned=True))
            await eventually(lambda count=count: rig.fight._transient_rejections_consecutive == count)
            assert not rig.bridge.commands("body_release")
        rig.bridge.emit(fighter(13, owned=True))
        await rig.ended()
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        assert summary.reason == "body_action_rejected" and summary.decisions == 1
        assert dict(summary.action_rejections) == Counter(rejections)
        assert summary.transient_rejections_consecutive == 10
        assert summary.release_confirmed and summary.watch_removed
        assert f"action_rejections={dict(sorted(Counter(rejections).items()))}" in caplog.text
        assert "transient_rejections_consecutive=10" in caplog.text


@pytest.mark.parametrize("reason", [
    "not_owned", "stale_generation", "owner_mismatch", "invalid_command", "weapon_mode", "paused_or_failed",
])
async def test_nontransient_action_rejection_still_ends_immediately(reason):
    async with running() as rig:
        await rig.active()
        rig.bridge.action_rejections.append(reason)
        rig.bridge.emit(fighter(4, owned=True))
        await rig.ended()
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        assert summary.reason == "body_action_rejected" and summary.decisions == 1
        assert dict(summary.action_rejections) == {reason: 1}
        assert summary.transient_rejections_consecutive == 0


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
                rig.bridge.control_events.append(event(
                    1, entity_id=lease.entity_id, session_epoch=lease.session_epoch,
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
        rig.pilot.on_message(events(event(
            1, reason="native_restored", release_reason="target_visibility_lost", owner="native", **scope,
        ), **{key: value for key, value in scope.items() if key in {"session_epoch", "world_epoch"}}))
        assert rig.fight._reason is None
        assert rig.fight._native_release_reason is None
        rig.link._dispatch(events(event(
            2, reason="native_restored", release_reason="target_visibility_lost", owner="native",
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
        rig.link._dispatch(events(event(
            1, reason="native_restored", release_reason="target_visibility_lost", owner="native",
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


@pytest.mark.parametrize("takeover", ["native_restored", "lease_lost", "attached", "dialog", "unavailable"])
async def test_native_ownership_or_dialogue_takeover_ends_episode(takeover):
    async with running() as rig:
        await rig.active()
        if takeover == "native_restored":
            rig.link._dispatch(events(event(1, reason="native_restored", owner="native")))
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
            rig.link._dispatch(events(event(
                1, entity_id=entity, reason="native_restored", owner="native",
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
        rig.link._dispatch(events(event(
            1, entity_id=entity, reason="native_restored", owner="native",
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
                rig.link._dispatch(events(event(
                    1, entity_id=lease.entity_id, reason="native_restored", owner="native",
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
        rig.link._dispatch(events(event(
            1, entity_id=entity, reason="native_restored", owner="native",
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
            rig.link._dispatch(events(event(
                1, entity_id=entity, reason="native_restored", owner="native",
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
        rig.link._dispatch(events(event(
            1, entity_id=entity, reason="native_restored", owner="native",
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
            rig.link._dispatch(events(event(
                1, entity_id=entity, reason="native_restored", owner="native",
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


async def test_uncertain_acquire_recovery_is_bounded_by_finish_deadline(monkeypatch, caplog):
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
        await eventually(lambda: rig.pilot.active_count == 0, timeout=1.2)
        assert rig.fight._pending_acquire is None
        assert not rig.link.body_leases
        assert rig.bridge._watched_npc is None
        assert rig.bridge.commands("body_watch")[-1]["npcs"] == []
        assert not rig.policy.requests
        assert not rig.pilot._done.is_set()
        assert "reason=finish_deadline" in caplog.text
        assert "unconfirmed=" in caplog.text


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
        await rig.watch()
        await eventually(lambda: rig.fight._watch_ready)
        rig.bridge.emit(player_row(), fighter())
        await eventually(lambda: rig.pilot.last_summary is not None)
        assert rig.pilot.last_summary.reason == "finish_deadline"
        summary = rig.pilot.last_summary
        assert not summary.watch_removed
        assert rig.bridge.commands("body_watch")[-1]["npcs"] == ["NPC"]
        assert rig.bridge._watched_npc == "NPC"
        assert rig.link.body_acquire_pending(1)
        assert not rig.bridge.pending_acquire and not rig.bridge.provider_owned
        assert not rig.policy.requests

        rig.bridge.status_error = None
        rig.bridge.response_delays.clear()
        rig.bridge.release_rejections.clear()
        if confirmation == "event":
            await rig.link.body_events()
        else:
            status = await rig.link.body_control_status()
            assert status.actors[0].owner == "native" and status.actors[0].acquire_request_id == 0
        assert not rig.link.body_acquire_pending(1)
        await eventually(lambda: rig.bridge.commands("body_watch")[-1]["npcs"] == [])
        assert rig.bridge._watched_npc is None
        assert rig.pilot.last_summary is summary and not summary.watch_removed

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


@pytest.mark.parametrize("late_count", [1, 3])
async def test_late_step_replies_do_not_act_or_steal_the_next_request_and_timely_reply_resets(late_count):
    async with running() as rig:
        await rig.active()
        rig.policy.step_delays.extend([0.120] * late_count)
        # If an old reply were matched to a new row, this invalid action would
        # terminate the pilot. Only the timely reply authorizes the legal action.
        rig.policy.step_response_actions.extend([11] * late_count + [1])
        for count, seq in enumerate(range(4, 4 + late_count), 1):
            rig.bridge.emit(fighter(seq, owned=True))
            await eventually(lambda count=count: rig.fight._policy_timeouts == count)
            assert rig.fight._policy_timeouts_consecutive == count
            assert rig.link.body_owned("NPC")
            assert len(rig.bridge.commands("body_action")) == 1
            assert not rig.bridge.commands("body_release")
        fresh_seq = 4 + late_count
        rig.bridge.emit(fighter(fresh_seq, owned=True))
        await eventually(lambda: rig.fight._decisions == 2)
        assert rig.fight._policy_timeouts_consecutive == 0
        assert rig.fight._policy_watchdog_task is None
        assert not rig.fight.client.pending
        assert [cmd["observation_seq"] for cmd in rig.bridge.commands("body_action")] == [3, fresh_seq]

        # A later isolated timeout starts a new streak rather than exhausting the
        # earlier one. A memory-only timely reply also resets that streak.
        rig.policy.step_delays.append(0.120)
        rig.policy.step_response_actions.extend([11, 1])
        rig.bridge.emit(fighter(fresh_seq + 1, owned=True))
        await eventually(lambda: rig.fight._policy_timeouts == late_count + 1)
        assert rig.fight._policy_timeouts_consecutive == 1
        rig.bridge.emit(fighter(fresh_seq + 2, owned=True, can_decide=False))
        await eventually(lambda: rig.fight._policy_timeouts_consecutive == 0)
        assert len(rig.bridge.commands("body_action")) == 2
        row = fighter(fresh_seq + 3, owned=True)
        row["self"]["down"] = True
        rig.bridge.emit(row)
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        assert summary.reason == "npc_down" and summary.decisions == 2
        assert summary.policy_timeouts == late_count + 1 and summary.policy_timeouts_consecutive == 0
        assert summary.policy_timeout_wait_p95_ms is not None
        assert summary.policy_response_silence_ms is None
        assert summary.release_confirmed and summary.watch_removed
        assert rig.fight._policy_timeouts == rig.fight._policy_timeouts_consecutive == 0


async def test_five_consecutive_late_steps_end_and_restore_native_before_any_late_action(caplog):
    caplog.set_level("INFO", logger="gothic_ai.bodypilot")
    async with running() as rig:
        await rig.active()
        rig.policy.step_delays.extend([0.120] * 5)
        rig.policy.step_response_actions.extend([11] * 5)
        for count, seq in enumerate(range(4, 8), 1):
            rig.bridge.emit(fighter(seq, owned=True))
            await eventually(lambda count=count: rig.fight._policy_timeouts_consecutive == count)
            assert not rig.bridge.commands("body_release")
        rig.bridge.emit(fighter(8, owned=True))
        await rig.ended()
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        assert summary.reason == "policy_timeout" and summary.decisions == 1
        assert summary.policy_timeouts == summary.policy_timeouts_consecutive == 5
        assert summary.policy_timeout_wait_p95_ms is not None
        assert summary.release_confirmed and summary.watch_removed
        assert len(rig.bridge.commands("body_action")) == 1
        assert "policy_timeouts=5 policy_timeouts_consecutive=5" in caplog.text


async def test_one_second_without_policy_response_ends_even_without_more_observations():
    async with running() as rig:
        await rig.active()
        rig.policy.step_mode = "timeout"
        rig.bridge.emit(fighter(4, owned=True))
        await eventually(lambda: rig.fight._policy_timeouts == 1)
        assert rig.link.body_owned("NPC") and not rig.bridge.commands("body_release")
        # The fake game sends no more rows, and its injected game clock never
        # advances. The service-silence guard must still use wall time.
        await rig.ended()
        await eventually(lambda: rig.pilot.last_summary is not None)
        summary = rig.pilot.last_summary
        assert summary.reason == "policy_timeout"
        assert summary.policy_timeouts == summary.policy_timeouts_consecutive == 1
        assert summary.policy_response_silence_ms >= 1000
        assert len(rig.policy.commands("step")) == 2
        assert len(rig.bridge.commands("body_action")) == 1
        assert summary.release_confirmed and summary.watch_removed


@pytest.mark.parametrize("mode", ["fallback", "error", "disconnect"])
async def test_service_failure_releases_to_native_and_does_not_block_loop(mode):
    async with running() as rig:
        await rig.active()
        rig.policy.step_mode = mode
        rig.bridge.emit(fighter(4, owned=True))
        await eventually(lambda: len(rig.policy.commands("step")) == 2)
        # A hung service must not hold the WebSocket receive loop or other server tasks.
        marker = asyncio.Event()
        asyncio.get_running_loop().call_soon(marker.set)
        await asyncio.wait_for(marker.wait(), 0.02)
        await rig.ended(policy=mode != "disconnect")
        assert len(rig.bridge.commands("body_action")) == 1


@pytest.mark.parametrize("mode", ["error", "timeout"])
async def test_reset_failure_restores_native_before_any_step(mode):
    async with running() as rig:
        rig.policy.reset_mode = mode
        await rig.watch()
        rig.bridge.emit(player_row(), fighter())
        await rig.ended()
        assert len(rig.policy.commands("reset")) == 1
        assert not rig.policy.commands("step")
        assert not rig.bridge.commands("body_action")


async def test_connection_failure_restores_native_before_first_policy_step():
    async with running() as rig:
        # A reserved local TCP port with no listening socket fails without an external service.
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reserved:
            reserved.bind(("127.0.0.1", 0))
            rig.pilot.url = f"tcp://127.0.0.1:{reserved.getsockname()[1]}"
            await rig.watch()
            rig.bridge.emit(player_row(), fighter())
            await rig.ended(policy=False)
        assert not rig.policy.requests and not rig.bridge.commands("body_action")


async def test_five_minute_combat_limit_ends_episode():
    async with running() as rig:
        await rig.active()
        rig.clock.advance(300.01)
        rig.bridge.emit(fighter(4, owned=True, t_game_ms=300_151))
        await rig.ended()


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
            rig.link._dispatch(events(event(
                1, reason="native_restored", release_reason="target_visibility_lost", owner="native",
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


async def test_heartbeat_keeps_active_lease_alive_and_close_restores_native(caplog):
    caplog.set_level("INFO")
    async with running() as rig:
        await rig.active()
        await eventually(lambda: rig.bridge.commands("body_heartbeat"))
        assert rig.link.body_leases
        await rig.pilot.aclose()
        await rig.ended()
        assert "p50" in caplog.text and "p95" in caplog.text


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
        rig.bridge.event_seq += 1
        native_events.append(event(
            rig.bridge.event_seq, entity_id=lease.entity_id, control_generation=lease.control_generation,
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


async def test_policy_fixture_connects_to_its_ipv4_listener_without_hostname_resolution(monkeypatch):
    async def unexpected_resolution(*args, **kwargs):
        raise AssertionError("the loopback TCP fixture must not use DNS")

    monkeypatch.setattr(asyncio.get_running_loop(), "getaddrinfo", unexpected_resolution)
    async with running() as rig:
        await rig.active()
        assert len(rig.policy.connections) == 1
        assert rig.policy.connections[0].get_extra_info("sockname")[0] == "127.0.0.1"
        assert rig.fight.client.host == "127.0.0.1"
        assert rig.pilot.handles("NPC") and rig.link.body_owned("NPC")
        assert rig.bridge.commands("body_action")[0]["target_entity_id"] == 2


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
