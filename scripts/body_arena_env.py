#!/usr/bin/env python3
"""Episode adapter for the real Linux headless engine; no Python physics.

The adapter advances game time only through ``arena_step``. Numeric observations
may update a recurrent provider while an animation is locked, but only a fresh,
eligible row can create a new decision. A reset clears all provider bookkeeping.
"""
from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
import sys
import time
from typing import Any

from body_observe_suite import ObserverRunner

ACTIONS = ("continue", "approach", "turn_to_target", "strafe_left", "strafe_right", "jump_back",
           "attack_front", "attack_left", "attack_right", "block", "wait_200", "wait_300")
PROVIDER = "stage5-scripted"
LEASE_KEYS = ("entity_id", "session_epoch", "world_epoch", "control_generation")


def preserve_melee_journals(workdir: Path, episode_dir: Path, seed: int) -> None:
    """Keep this completed duel before the runner's bounded journals rotate."""
    for name in ("melee-blade-shadow", "melee-posture"):
        sources = [path for suffix in (".previous.jsonl", ".jsonl")
                   if (path := workdir / (name + suffix)).is_file()]
        if not sources:
            continue
        destination = episode_dir / (name + ".jsonl")
        seen = set(destination.read_text().splitlines(keepends=True)) if destination.is_file() else set()
        with destination.open("a") as output:
            for path in sources:
                with path.open() as source:
                    for line in source:
                        if line not in seen and json.loads(line).get("arena_seed") == seed:
                            output.write(line)
                            seen.add(line)


class ArenaInfrastructureError(RuntimeError):
    """A failed environment/transport is separate from a game-time truncation."""


class ArenaRunner(ObserverRunner):
    def __init__(self, args):
        if sys.platform != "linux":
            raise ArenaInfrastructureError("native episodes run only on dev-ubuntu, never the Mac")
        super().__init__(args)

    def record(self, direction: str, value: dict[str, Any]) -> None:
        # Full observations and native encoder replies remain on the server so
        # an independent report can reconstruct every command's sampled mask.
        self.ipc.write(json.dumps({"direction": direction, "wall_monotonic": time.monotonic(),
                                  "value": value}, ensure_ascii=False, separators=(",", ":"))+"\n")
        self.ipc.flush()

    def call(self, command: dict[str, Any]) -> dict[str, Any]:
        response = self.request(command, allow_error=True)
        if response.get("ok") is False:
            raise ArenaInfrastructureError(f"runner rejected {command.get('cmd')}: {response.get('error', response.get('reason'))}")
        return response

    def episode_log(self, output: Path) -> None:
        self.ipc.flush()
        self.ipc.close()
        self.ipc = (output / "ipc.jsonl").open("w", encoding="utf-8")


@dataclass(frozen=True)
class EpisodeIdentity:
    session_epoch: int
    world_epoch: int
    generation: int
    episode_id: int


@dataclass(frozen=True)
class EpisodeStep:
    arena: dict[str, Any]
    observations: tuple[dict[str, Any], ...]
    events: tuple[dict[str, Any], ...]
    advanced_substeps: int

    @property
    def done(self) -> bool:
        return bool(self.arena.get("terminated") or self.arena.get("truncated") or self.arena.get("infrastructure"))


class EpisodeEnv:
    """Reusable reset/step/action API; all state changes are native commands.

    ``recurrent_token`` is an identity boundary for future GRU hidden state. The
    environment does not allocate a model or infer one on the development Mac.
    """
    def __init__(self, runner: ArenaRunner, scenario: Path, *, watchdog_ms: int = 5000):
        self.runner, self.scenario = runner, scenario
        self.watchdog_ms = watchdog_ms
        self.identity: EpisodeIdentity | None = None
        self.recurrent_token: tuple[int, ...] | None = None
        self.latest: dict[int, dict[str, Any]] = {}
        self.processed_sequences: dict[int, int] = {}
        self.leases: dict[int, dict[str, Any]] = {}
        self.command_ids: dict[int, int] = {}
        self.decisions: list[dict[str, Any]] = []
        self.events: list[dict[str, Any]] = []
        self.arena: dict[str, Any] = {}
        self.initial: dict[str, Any] = {}
        self.reset_wall_s = 0.0
        self.reset_started = 0.0
        self.rollout_started = 0.0
        self.output: Path | None = None
        self.provider = "native"

    def _arena(self, response: dict[str, Any]) -> dict[str, Any]:
        arena = response.get("arena")
        if not isinstance(arena, dict):
            raise ArenaInfrastructureError("native arena status is missing")
        if arena.get("infrastructure") is True or response.get("infrastructure_failure") is True:
            raise ArenaInfrastructureError(f"native arena infrastructure failure: {arena.get('reason', response.get('infrastructure_reason'))}")
        self.arena = arena
        return arena

    def reset(self, *, output: Path, task: str, seed: int, side: int, distance_cm: int,
              timeout_ms: int, provider: str) -> dict[str, Any]:
        if provider not in ("native", "scripted"):
            raise ValueError("unknown provider")
        if task not in ("distance", "attack", "block", "native_duel", "reset_stress"):
            raise ValueError("unknown arena task")
        output.mkdir()
        self.runner.episode_log(output)
        self.output = output
        self.provider = provider
        # Clear memory before contacting the new epoch. A failed reset also
        # cannot reuse a previous episode's hidden state or command sequence.
        self.identity = None
        self.recurrent_token = None
        self.latest.clear()
        self.processed_sequences.clear()
        self.leases.clear()
        self.command_ids.clear()
        self.decisions.clear()
        self.events.clear()
        self.rollout_started = 0.0
        self.reset_started = time.monotonic()
        response = self.runner.call({"cmd": "reset", "scenario": str(self.scenario),
                                     "output_dir": str(output), "mode": "lockstep",
                                     "arena": {"scenario": task, "seed": seed, "side": side,
                                               "distance_cm": distance_cm, "timeout_ms": timeout_ms}})
        arena = self._arena(response)
        if arena.get("ready") is not True or arena.get("started") is not False:
            raise ArenaInfrastructureError("reset did not produce a stable, unstarted arena")
        self.initial = json.loads(json.dumps(arena))
        # These epochs are native values, never inferred from the game clock.
        actors = arena.get("actors")
        if not isinstance(actors, list) or len(actors) != 2:
            raise ArenaInfrastructureError("arena reset requires exactly two native actors")
        self.identity = EpisodeIdentity(arena["session_epoch"], arena["world_epoch"],
                                        arena["runner_generation"], arena["episode_id"])
        self.recurrent_token = (self.identity.session_epoch, self.identity.world_epoch, self.identity.generation)
        self.pull()
        if provider == "scripted":
            entities = [arena["primary_entity_id"]]
            if task != "native_duel":
                entities.append(arena["opponent_entity_id"])
            for entity in entities:
                self.acquire(entity)
        self.reset_wall_s = time.monotonic()-self.reset_started
        self.runner.sample()
        return self.initial

    def status(self) -> dict[str, Any]:
        return self._arena(self.runner.call({"cmd": "arena_status"}))

    def body_status(self, entity: int) -> dict[str, Any]:
        response = self.runner.call({"cmd": "body_control_status"})
        actor = next((row for row in response.get("actors", []) if row.get("entity_id") == entity), None)
        if actor is None:
            raise ArenaInfrastructureError(f"body actor disappeared: {entity}")
        return actor

    def acquire(self, entity: int) -> None:
        actor = self.body_status(entity)
        response = self.runner.call({"cmd": "body_acquire", **{key: actor[key] for key in LEASE_KEYS},
                                     "target_entity_id": actor["target_entity_id"], "provider": PROVIDER,
                                     "body_control": True, "mode": "lockstep", "timeout_ms": self.watchdog_ms})
        if response.get("accepted") is not True:
            raise ArenaInfrastructureError(f"body acquire rejected: {response.get('reason')}")
        self.leases[entity] = {key: response[key] for key in LEASE_KEYS}
        self.command_ids[entity] = 0
        # A new owner invalidates pre-acquire samples; keep them for the raw
        # journal, but they cannot create a decision under the new generation.
        self.latest.pop(entity, None)

    def start(self, substeps: int = 2) -> EpisodeStep:
        self._arena(self.runner.call({"cmd": "arena_start", "provider": self.provider}))
        self.rollout_started = time.monotonic()
        # The post-acquire freshness barrier advances the actual simulation. The realtime
        # driver's first poll sees exactly one tick after arena_start; a lockstep run that
        # must make the same decisions starts with one substep instead of two.
        return self.step(substeps)

    def pull(self) -> tuple[dict[str, Any], ...]:
        response = self.runner.call({"cmd": "body_observations", "max": 256})
        return self.accept_rows(response.get("observations", response.get("rows")))

    def accept_rows(self, rows) -> tuple[dict[str, Any], ...]:
        """Validate drained rows in order and keep each actor's latest one under its lease."""
        if not isinstance(rows, list):
            raise ArenaInfrastructureError("numeric observation packet has no rows")
        for row in rows:
            if self.identity is not None and (row.get("session_epoch") != self.identity.session_epoch or
                                               row.get("world_epoch") != self.identity.world_epoch):
                raise ArenaInfrastructureError("numeric observation belongs to another episode epoch")
            entity, sequence = row["entity_id"], row["observation_seq"]
            if sequence <= self.processed_sequences.get(entity, 0):
                raise ArenaInfrastructureError("repeated or backwards numeric observation")
            self.processed_sequences[entity] = sequence
            lease = self.leases.get(entity)
            terminal = self.arena.get("terminated") or self.arena.get("truncated") or self.arena.get("infrastructure")
            # A safe native handoff can end an episode with generation zero.
            # Preserve that final numeric state for the transition; act() still
            # rejects every decision after the native episode outcome.
            if terminal or lease is None or all(row.get(key) == lease[key] for key in LEASE_KEYS):
                self.latest[entity] = row
        return tuple(rows)

    def step(self, substeps: int = 1) -> EpisodeStep:
        if self.identity is None:
            raise ArenaInfrastructureError("step requires successful reset")
        if self.arena.get("terminated") or self.arena.get("truncated"):
            return EpisodeStep(self.arena, (), (), 0)
        for lease in self.leases.values():
            response = self.runner.call({"cmd": "body_heartbeat", **lease, "provider": PROVIDER})
            if response.get("accepted") is not True:
                raise ArenaInfrastructureError(f"active episode lost ownership: {response.get('reason')}")
        response = self.runner.call({"cmd": "arena_step", "substeps": substeps})
        arena = self._arena(response)
        observations = self.pull()
        event_response = self.runner.call({"cmd": "body_events"})
        events = event_response.get("events")
        if not isinstance(events, list):
            raise ArenaInfrastructureError("body event packet has no events")
        self.events.extend(events)
        return EpisodeStep(arena, observations, tuple(events), response.get("advanced_substeps", 0))

    def eligible(self, entity: int, action: int | None = None) -> bool:
        row = self.latest.get(entity)
        if row is None or entity not in self.leases:
            return False
        if not all(row.get(key) == self.leases[entity][key] for key in LEASE_KEYS):
            return False
        if row.get("can_decide") is not True or row.get("action_mask_valid") is not True:
            return False
        mask = row.get("action_mask")
        return isinstance(mask, list) and len(mask) == 12 and (action is None or mask[action] is True)

    def act(self, entity: int, action: int) -> int:
        if self.arena.get("terminated") or self.arena.get("truncated") or self.arena.get("infrastructure"):
            raise ArenaInfrastructureError("terminal episode cannot accept a new decision")
        if action == 0 or not 0 <= action < len(ACTIONS):
            raise ValueError("new decisions use a non-continue primitive from the fixed vocabulary")
        if not self.eligible(entity, action):
            raise ArenaInfrastructureError(f"provider selected unavailable action {ACTIONS[action]}")
        row = self.latest[entity]
        self.command_ids[entity] += 1
        command_id = self.command_ids[entity]
        request = {"cmd": "body_action", **self.leases[entity], "provider": PROVIDER,
                   "goal_version": row["goal"]["version"], "target_entity_id": row["target"]["entity_id"],
                   "command_id": command_id, "action_id": action, "observation_seq": row["observation_seq"],
                   "issued_game_ms": row["t_game_ms"], "ttl_ms": 1000}
        response = self.runner.call(request)
        if response.get("accepted") is not True:
            raise ArenaInfrastructureError(f"eligible scripted decision rejected: {response.get('reason')}")
        self.decisions.append({**request, "execution_id": response["execution_id"]})
        # Until a new sampled row arrives this actor cannot issue a second
        # command using the row which just anchored the accepted decision.
        self.latest.pop(entity, None)
        return command_id

    def wait_command(self, entity: int, command_id: int, max_substeps: int = 200) -> EpisodeStep:
        for _ in range(max_substeps):
            step = self.step()
            terminal = next((event for event in step.events if event.get("entity_id") == entity and
                             event.get("command_id") == command_id and event.get("phase") in ("completed", "cancelled")), None)
            if terminal is not None:
                if terminal["phase"] != "completed":
                    raise ArenaInfrastructureError(f"healthy scripted command cancelled: {terminal.get('reason')}")
                return step
            if step.done:
                return step
        raise ArenaInfrastructureError("native command did not reach a bounded outcome")

    def finalize(self) -> dict[str, Any]:
        final = self.status()
        self.pull()
        response = self.runner.call({"cmd": "body_events"})
        self.events.extend(response.get("events", []))
        self.runner.sample()
        now = time.monotonic()
        return {"initial": self.initial, "final": final, "recurrent_token": list(self.recurrent_token or ()),
                "timing": {"reset_started_monotonic": self.reset_started, "reset_wall_s": self.reset_wall_s,
                           "rollout_started_monotonic": self.rollout_started, "final_monotonic": now,
                           "rollout_wall_s": now-self.rollout_started if self.rollout_started else 0.0,
                           "total_wall_s": now-self.reset_started},
                "requested_decisions": self.decisions.copy()}


class ScriptedProvider:
    """Small deterministic baseline consuming only numeric, visible features."""
    def __init__(self):
        self.token: tuple[int, ...] | None = None
        self.attack_index = 0
        self.processed: dict[int, int] = {}

    def reset(self, token: tuple[int, ...]) -> None:
        self.token, self.attack_index = token, 0
        self.processed.clear()

    def choose(self, row: dict[str, Any], task: str) -> int | None:
        entity, sequence = row["entity_id"], row["observation_seq"]
        if sequence <= self.processed.get(entity, 0):
            return None
        self.processed[entity] = sequence
        if row.get("can_decide") is not True or row.get("action_mask_valid") is not True:
            return None
        mask = row["action_mask"]
        target = row.get("target", {})
        if target.get("current_valid") is not True:
            return None
        if task == "distance":
            choices = (2, 1, 11)
        elif task == "block":
            # Waiting would lock the defender before the attack arrives. Leave
            # the native body idle until an observed attack warrants a block.
            choices = (9, 2) if target.get("attack_anim") or target.get("prehit") else (2,)
        elif task == "attack":
            choices = (2, 6, 7, 8, 1, 11)
        else:
            attack = (6, 7, 8)[self.attack_index % 3]
            choices = ((9,) if target.get("attack_anim") or target.get("prehit") else ()) + (2, attack, 6, 1, 11)
        for action in choices:
            if mask[action] is True:
                if action in (6, 7, 8):
                    self.attack_index += 1
                return action
        return None
