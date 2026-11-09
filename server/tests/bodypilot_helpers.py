"""Exercise the live pilot through the bridge adapter and a torch-free TCP policy."""

import asyncio
import json
from collections import deque
from contextlib import asynccontextmanager

from gothic_ai import bodypilot
from gothic_ai.bodypilot import BodyPilot
from gothic_ai.game import GameError, GameLink
from gothic_ai.protocol import (
    PROTOCOL_VERSION,
    Hello,
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
        self._control_event_seq = 0
        self._observation_steps = {}
        self.action_rejections = deque()
        self.action_accepted_events = True
        self.release_rejections = deque()
        self.status_error = None
        self.acquire_roster_delay = 0
        self.acquire_grants = []
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

    def control_event(self, **updates):
        self._control_event_seq = max(self._control_event_seq, self.link._body_control_last_event) + 1
        return event(self._control_event_seq, **updates)

    def accept_action(self, command, **updates):
        fields = {name: command[name] for name in (
            "entity_id", "session_epoch", "world_epoch", "control_generation", "goal_version",
            "target_entity_id", "command_id", "observation_seq", "action_id", "issued_game_ms", "ttl_ms",
            "provider",
        )}
        fields.update(
            phase="accepted", reason="accepted", t_game_ms=command["issued_game_ms"],
            sim_step=self._observation_steps[command["entity_id"], command["observation_seq"]],
        )
        fields.update(updates)
        self.link._dispatch(events(self.control_event(**fields)))

    def _accept_action(self, command, value):
        if not (
            self.action_accepted_events and command["type"] == "body_action"
            and command["action_id"] != 0 and value["accepted"]
        ):
            return
        # Native events may reach the listener before the RPC coroutine resumes.
        self.accept_action(command)

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
        self.acquire_grants.append(command)
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
        if outcome == "acquired":
            self._grant(command)
            self.control_events.append(self.control_event(
                **self._control_scope, provider=command["provider"],
                acquire_request_id=command["acquire_request_id"], observation_seq=3,
            ))
        else:
            self.control_events.append(self.control_event(
                entity_id=command["entity_id"], provider=command["provider"],
                acquire_request_id=command["acquire_request_id"], reason="acquire_cancelled",
                acquire_cancel_reason=outcome, owner="native", control_generation=0, goal_version=0,
            ))

    def _expire_lease(self):
        self.lease_expirations += 1
        if self.provider_owned:
            self.provider_owned = False
            self.link._dispatch(events(self.control_event(
                **self._control_scope, reason="native_restored", owner="native",
            )))
            self._activity(self._control_npc, "fight")

    def restore_native(self, *, release_reason=""):
        """Restore the native actor, leaving ownership events for the next destructive poll."""
        lease = next(iter(self.link.body_leases.values()))
        self.provider_owned = False
        if self._lease_watchdog is not None:
            self._lease_watchdog.cancel()
        self.control_events.append(self.control_event(
            entity_id=lease.entity_id, reason="native_restored", owner="native",
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
                value = events(self.control_event(
                    entity_id=lease.entity_id, reason="native_restored", owner="native",
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
        self._accept_action(command, value)
        result = Result(type="result", req=command["req"], ok=True, value=value)
        delays = self.response_delays.get(kind, 0)
        if isinstance(delays, deque):
            delay = delays.popleft() if delays else 0
        else:
            delay = delays
        if delay:
            async def reply():
                await asyncio.sleep(delay)
                self.link._dispatch(result)

            # Replies can arrive later while the socket remains writable.
            self._background(reply())
        else:
            self.link._dispatch(result)

    def emit(self, *rows, generation=1):
        self._observation_steps.update({
            (row["entity_id"], row["observation_seq"]): row["sim_step"] for row in rows
        })
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
                        self.link._dispatch(events(self.control_event(
                            entity_id=entity, provider=command["provider"],
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
        self._accept_action(command, value)
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
        self.reset_modes = deque()
        self.reset_delay = 0
        self.reset_delays = deque()
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
                    mode = self.reset_modes.popleft() if self.reset_modes else self.reset_mode
                    delay = self.reset_delays.popleft() if self.reset_delays else self.reset_delay
                    if delay:
                        await asyncio.sleep(delay)
                    if self.reset_waiter is not None:
                        await self.reset_waiter.wait()
                    if mode == "error":
                        result.update(ok=False, reason="reset_error", error="fake reset failure")
                    elif mode == "timeout":
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
    body_policy="engine", body_net=None, has_url=True,
):
    bridge, policy, clock = MultiBridge() if multi else Bridge(), Policy(), Clock()
    if protocol is not None:
        bridge.link._dispatch(Hello(
            type="hello", protocol=protocol, client="fake", codepage="utf-8", body_net=body_net,
        ))
    server = await asyncio.start_server(policy.handle, host="127.0.0.1", port=0)
    host, port = server.sockets[0].getsockname()
    pilot = BodyPilot(
        bridge.link, f"tcp://{host}:{port}" if has_url else None, body_policy=body_policy,
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
