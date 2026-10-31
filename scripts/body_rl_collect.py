#!/usr/bin/env python3
"""Stage 9.2: parallel lockstep rollouts of a frozen policy with sampled actions.

Each worker owns one --fork-reset runner with the training culls and one single-thread
CPU policy runtime. Every periodic row of the policy actor updates memory exactly as in
evaluation; at each decision the action is sampled from the runtime's own logits over
the effective mask (temperature 0 is the evaluation argmax). Per episode the collector
keeps a compact score summary and the model-view rows (features, mask, reset, sampled
label) a self-imitation round trains on. Measurement and data only: no quality credit.

Decisions follow the stage 8 realtime driver: the first one after one tick (--start-substeps 1)
and native command TTL 100 ms. With them a greedy rollout reproduces the realtime driver's
episodes (scripts/body_schedule_equivalence.py); the 9.2 rounds used 2 and 1000 and fitted
a lockstep-only schedule. The fork template plays the bench opening once (--fork-prefix) and every
episode forks right before `bodyarena`. Between decisions the runner advances on its own (arena_advance) to
the tick where the per-tick loop would decide next; --per-tick keeps one round trip per tick.
The world outside the arena is frozen and the actors draw their own script randomness: such a
rollout equals the realtime driver in the live world with OPENGOTHIC_BODY_SCRIPT_RNG=isolated.

Stage 9.3 additions. --opponent gives frozen checkpoints of a self-play pool: a duel against one of
them owns both actors (arena opponent_owned), the opponent's rows go to its own runtime and it
samples at --opponent-temperature; --native-share of the duels keep the native opponent. The engine
starts each owned command in its actor's own tick and the opponent ticks first, so on a shared
decision tick its command can start and close the primary's mask (mask_changed_before_start): the
seat matters. --learner-seat random puts the learner in either one; its summary (reason, health,
score) is then from the learner's side, with the arena's own reason kept as arena_reason.
--wall-share mixes near_wall arenas into one collection. Each learner row also keeps its game time,
both health fractions and, at a decision, the log-probability of the sampled action under the
sampling distribution (PPO's behaviour policy).

Envs take the next episode from one shared queue, so a long duel does not leave the other envs
idle at the end. Every episode samples from its own streams (sampling seed and episode seed):
its rows do not depend on the number of workers or on which env played it.
"""
from __future__ import annotations

import argparse
from collections import Counter
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor
import hashlib
import json
import math
import multiprocessing
import os
from pathlib import Path
import queue
import random
import shutil
import threading
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))

# Training culls: foot IK only for arena actors, no animation far away, the rest of the world frozen,
# and the arena actors' script randomness isolated from it (which makes the frozen world exact).
TRAINING_ENV = {"OPENGOTHIC_FOOTIK_WATCHED_ONLY": "1", "OPENGOTHIC_ANIM_SKIP_FAR": "1",
                "OPENGOTHIC_FREEZE_WORLD": "1", "OPENGOTHIC_BODY_SCRIPT_RNG": "isolated", "DISPLAY": "",
                "WAYLAND_DISPLAY": "", "GOTHIC_AI": "0", "VK_DRIVER_FILES": "/nonexistent/no-vulkan.json",
                "VK_ICD_FILENAMES": "/nonexistent/no-vulkan.json", "OPENGOTHIC_TEXTURES": "",
                "CUDA_VISIBLE_DEVICES": "", "OMP_NUM_THREADS": "1", "MKL_NUM_THREADS": "1"}
HEARTBEAT_EVERY_S = 0.25
# Longest arena_advance; keeps the bounded native queues far from overflow between drains.
ADVANCE_MAX_SUBSTEPS = 30
# Short arena tasks keep their stage-8 distance and timeout; duels vary the distance.
TASK_SETUP = {"distance": (600, 10000), "attack": (150, 5000), "block": (150, 5000)}


def success(summary: dict) -> bool:
    """A duel is won by downing the opponent; a short task only by its own success
    (killing the opponent in the block task is not a blocked hit)."""
    if summary.get("task", "native_duel") == "native_duel":
        return summary["reason"] == "opponent_down"
    return summary["outcome"] == "task_success"


def score(summary: dict, timeout_ms: int) -> float:
    """Win/loss, then health margin, then a faster win. Timeouts score on margin only."""
    if summary.get("task", "native_duel") != "native_duel":
        return 1.0 if success(summary) else 0.0
    outcome = {"opponent_down": 1.0, "primary_down": -1.0}.get(summary["reason"], 0.0)
    margin = summary["primary_hp"]/summary["primary_hp_max"]-summary["opponent_hp"]/summary["opponent_hp_max"]
    speed = -0.2*summary["elapsed_game_ms"]/timeout_ms if outcome > 0 else 0.0
    return outcome+0.5*margin+speed


def sample_action(scores: list[float], mask: list[bool], temperature: float, rng: random.Random) -> tuple[int, float]:
    """The action and its log-probability under the sampling distribution (0 for the greedy argmax)."""
    allowed = [index for index in range(1, 12) if mask[index]]
    if temperature <= 0:
        return max(allowed, key=lambda index: scores[index]), 0.0
    top = max(scores[index] for index in allowed)
    weights = [math.exp((scores[index]-top)/temperature) for index in allowed]
    action = rng.choices(allowed, weights=weights)[0]
    return action, math.log(weights[allowed.index(action)]/sum(weights))


def assign(tasks: list[list], *, seed: int, wall_share: float, opponents: list[str], native_share: float,
           learner_seat: str) -> None:
    """Append geometry, opponent and learner seat to each [task, seed, side, distance] from their own
    seeded draws, so the task list itself is the same with or without them. Only duels meet the pool;
    against the native opponent the learner is always the primary."""
    schedule = random.Random(seed)
    for entry in tasks:
        wall, draw, pick, coin = schedule.random(), schedule.random(), schedule.randrange(max(1, len(opponents))), schedule.random()
        entry.append("near_wall" if wall < wall_share else "open")
        entry.append(opponents[pick] if opponents and entry[0] == "native_duel" and draw >= native_share else "native")
        seat = learner_seat if learner_seat != "random" else ("primary" if coin < 0.5 else "opponent")
        entry.append(seat if entry[-1] != "native" else "primary")


def health(section) -> float:
    value = section.get("hp_fraction") if isinstance(section, dict) else None
    return float(value) if type(value) in (int, float) else -1.0


class SharedBackend:
    """One loaded model for several env threads of a process.

    exact: each row steps alone under a lock, bit for bit as the realtime runtime.
    batched: the rows of all envs waiting at that moment step together. GRU outputs then differ
    from one-row steps in the last float bits (measured: under 1e-5 in logits, no changed action).
    """
    def __init__(self, base, *, batched: bool):
        self.base, self.batched = base, batched
        self.model, self.torch = base.model, base.torch
        self.normalizer, self.metadata = base.normalizer, base.metadata
        self.execution_profile, self.last_step_timing = base.execution_profile, {}
        self.condition = threading.Condition()
        self.queue: list[dict] = []
        self.running = False
        self.batches: Counter = Counter()

    def step(self, features, mask, state):
        if not self.batched:
            with self.condition:
                self.batches[1] += 1
                return self.base.step(features, mask, state)
        slot = {"features": features, "mask": mask, "state": state, "result": None}
        with self.condition:
            self.queue.append(slot)
            while slot["result"] is None:
                if self.running:
                    self.condition.wait()
                    continue
                self.running, batch, self.queue = True, self.queue, []
                self.condition.release()
                try:
                    self._compute(batch)
                finally:
                    self.condition.acquire()
                    self.running = False
                    self.condition.notify_all()
        if isinstance(slot["result"], Exception):
            raise slot["result"]
        return slot["result"]

    def _compute(self, batch):
        torch = self.torch
        self.batches[len(batch)] += 1
        try:
            with torch.inference_mode():
                values = torch.tensor([slot["features"] for slot in batch], dtype=torch.float32)
                masks = torch.tensor([slot["mask"] for slot in batch], dtype=torch.bool)
                state = None
                if any(slot["state"] is not None for slot in batch):
                    state = torch.cat([slot["state"] if slot["state"] is not None else self.model.initial_state(values[index:index+1])
                                       for index, slot in enumerate(batch)])
                logits, state = self.model.step(values, masks, state=state)
                rows = logits.detach().cpu().tolist()
            for index, slot in enumerate(batch):
                slot["result"] = (rows[index], state[index:index+1])
        except Exception as error:  # every waiting env fails the same way
            for slot in batch:
                slot["result"] = error


def run_worker(process: int, options: dict, envs: list[int], tasks) -> dict:
    os.environ.update(TRAINING_ENV)
    import numpy
    from body_arena_env import LEASE_KEYS, ArenaInfrastructureError, preserve_melee_journals
    from body_policy_suite import PolicyArenaRunner, PolicyEpisodeEnv
    from gothic_body.runtime import PolicyRuntime, TorchBackend, actor_identity
    from body_fast_features import pack_features as fast_pack_features

    class LeanRunner(PolicyArenaRunner):
        def record(self, direction, value):
            pass

        def episode_log(self, output):
            pass

    class LeanEnv(PolicyEpisodeEnv):
        """Heartbeats on wall time; per-tick native events are not read."""
        last_heartbeat = 0.0

        def heartbeat(self):
            now = time.monotonic()
            if now-self.last_heartbeat < HEARTBEAT_EVERY_S:
                return
            self.last_heartbeat = now
            for entity, lease in list(self.leases.items()):
                response = self.runner.call({"cmd": "body_heartbeat", **lease, "provider": self.owner_provider})
                if response.get("accepted") is not True and response.get("reason") != "release_pending":
                    self.leases.pop(entity)
                    self.fallbacks.append({"entity_id": entity, "reason": response.get("reason"), "kind": "ownership_loss"})

        def step(self, substeps=1):
            self.heartbeat()
            response = self.runner.call({"cmd": "arena_step", "substeps": substeps})
            self._arena(response)
            self.pull()
            return response.get("advanced_substeps", 0)

        def advance(self, watches, max_substeps):
            """One round trip to the next tick where a watched actor could decide, or terminal."""
            self.heartbeat()
            response = self.runner.call({"cmd": "arena_advance", "max_substeps": max_substeps, "until": watches})
            self._arena(response)
            if response.get("observation_dropped_since_drain"):
                raise ArenaInfrastructureError("arena_advance lost queued observations")
            self.pulled.extend(self.accept_rows(response.get("observations")))
            self.events.extend(response.get("events", []))
            return response.get("advanced_substeps", 0)

    # One model per process, shared by its env threads: row by row (as the realtime runtime) or
    # batched across the envs that wait for inference at the same moment.
    base = TorchBackend(options["checkpoint"], device="cpu", inference_cpu_threads=1)
    backend = SharedBackend(base, batched=options.get("inference", "exact") == "batched")
    checkpoint_sha256 = hashlib.sha256(Path(options["checkpoint"]).read_bytes()).hexdigest()
    # The self-play pool: one frozen model per checkpoint, each row stepped alone.
    pool = {path: (SharedBackend(TorchBackend(path, device="cpu", inference_cpu_threads=1), batched=False),
                   hashlib.sha256(Path(path).read_bytes()).hexdigest()) for path in options.get("opponent") or []}

    class Seat:
        """One owned actor of an episode: its runtime, stream counter and sampling."""
        def __init__(self, entity, runtime, temperature, rng, learner):
            self.entity, self.runtime, self.temperature, self.rng, self.learner = entity, runtime, temperature, rng, learner
            self.stream = 0

    def run_env(worker: int) -> dict:
        base.torch.set_num_threads(1)  # per thread: keep the one-thread kernels of the realtime runtime
        output = Path(options["output"])/f"worker-{worker:02d}"
        output.mkdir(parents=True)
        runner_args = argparse.Namespace(output=output, runner=Path(options["runner"]), assets=Path(options["assets"]),
                                         save=Path(options["save"]), reference=Path(options["reference"]),
                                         scenario=Path(options["scenario"]), compact_replies=True,
                                         native_serial_workers=True,
                                         runner_extra_args=["--fork-reset"]+(["--fork-prefix"] if options.get("fork_prefix") else []))
        runner = LeanRunner(runner_args)
        # Training runtime: the fast packer (float-for-float equal to the gate's) and no provenance digests.
        lean = options.get("lean_runtime", True)
        policy = PolicyRuntime(backend=backend, checkpoint_sha256=checkpoint_sha256, max_actors=4, inference_cpu_threads=1,
                               lean=lean, feature_packer=fast_pack_features if lean else None)
        rivals = {path: PolicyRuntime(backend=model, checkpoint_sha256=digest, max_actors=4, inference_cpu_threads=1,
                                      lean=lean, feature_packer=fast_pack_features if lean else None)
                  for path, (model, digest) in pool.items()}
        summaries = []
        try:
            while True:
                try:
                    task, seed, side, distance, geometry, against, seat_role = tasks.get_nowait()
                except queue.Empty:
                    break
                name = f"ep-{task}-{seed}-{side}-{distance}"
                episode_dir = output/name
                owned = against != "native"
                runner.arena_options = {**({"geometry": geometry} if geometry != "open" else {}),
                                        **({"opponent_owned": True} if owned else {})}  # merged into the reset
                env = LeanEnv(runner, Path(options["scenario"]), owner_provider="stage9-rl")
                env.command_ttl_ms = options.get("command_ttl_ms", 1000)
                started = time.monotonic()
                timeout_ms = TASK_SETUP[task][1] if task in TASK_SETUP else options["timeout_ms"]
                initial = env.reset(output=episode_dir, task=task, seed=seed, side=side, distance_cm=distance,
                                    timeout_ms=timeout_ms, provider="scripted")
                adversary_sent = task != "block"
                primary, opponent = initial["primary_entity_id"], initial["opponent_entity_id"]
                me, rival = (primary, opponent) if seat_role == "primary" else (opponent, primary)
                # The episode's own streams of samples; the opponent's leaves the learner's as without a pool.
                seats = [Seat(me, policy, options["temperature"], random.Random(f"{options['sampling_seed']}:{seed}"), True)]
                if owned:
                    env.acquire(opponent)
                    seats.append(Seat(rival, rivals[against], options["opponent_temperature"],
                                      random.Random(f"{options['sampling_seed']}:{seed}:opponent"), False))
                reset_s = time.monotonic()-started
                if options["keep_journals"] and summaries:
                    previous = summaries[-1]
                    preserve_melee_journals(output / "run", output / previous["name"], previous["seed"])
                for seat in seats:
                    seat.runtime.begin_episode(initial["session_epoch"], initial["world_epoch"], initial_game_ms=initial["game_ms"])
                by_entity = {seat.entity: seat for seat in seats}
                rows, stats = [], Counter()

                def feed(command_enabled):
                    pulled, env.pulled = env.pulled, []
                    for row in pulled:
                        seat = by_entity.get(row.get("entity_id"))
                        if seat is None or row.get("periodic") is not True:
                            continue
                        lease = env.leases.get(seat.entity)
                        if lease is None or any(row.get(key) != lease[key] for key in LEASE_KEYS):
                            continue
                        seat.stream += 1
                        # A per-tick client processes each periodic row on its own tick: now == row time.
                        response = seat.runtime.process({"stream_seq": seat.stream, "observation": row, "source": "periodic_observer",
                                                         "now_game_ms": row["t_game_ms"], "checkpoint_sha256": seat.runtime.checkpoint_sha256,
                                                         "command_enabled": command_enabled,
                                                         "memory_only": not command_enabled or row.get("can_decide") is not True})
                        if response.get("model_processed"):
                            if seat.learner:
                                rows.append({"features": seat.runtime.last_features, "mask": response["effective_action_mask"],
                                             "reset": bool(response.get("hidden_reset")), "label": -1, "seq": row["observation_seq"],
                                             "logp": 0.0, "t": row["t_game_ms"], "hp": health(row.get("self")),
                                             "target_hp": health(row.get("target"))})
                        elif response.get("status") == "fallback" and command_enabled:
                            stats["fallbacks" if seat.learner else "opponent_fallbacks"] += 1
                            env.release_to_native(seat.entity, response.get("reason"))

                def watches():
                    # Exactly the per-tick checks below: an owned actor on a periodic row, and the
                    # block task's held opponent while its single attack is still unsent.
                    out = []
                    for seat in seats:
                        if seat.entity in env.leases:
                            out.append({**{key: env.leases[seat.entity][key] for key in LEASE_KEYS}, "periodic": True})
                    if not adversary_sent and opponent in env.leases:
                        out.append({**{key: env.leases[opponent][key] for key in LEASE_KEYS}, "periodic": False, "actions": [6, 2]})
                    return out

                rollout_started = time.monotonic()
                feed(False)
                env.start(options.get("start_substeps", 2))
                ticks = 0
                while ticks < options["max_substeps"]:
                    terminal = bool(env.arena.get("terminated") or env.arena.get("truncated"))
                    feed(not terminal)
                    if terminal:
                        break
                    if not adversary_sent:
                        # The stage-7 block task: the held opponent throws one real front attack.
                        if env.eligible(opponent, 6):
                            env.act(opponent, 6)
                            adversary_sent = True
                        elif env.eligible(opponent, 2):
                            env.act(opponent, 2)
                    undecided = False
                    for seat in (reversed(seats) if options.get("opponent_first") else seats):
                        row = env.latest.get(seat.entity)
                        if row is None or row.get("periodic") is not True or not env.eligible(seat.entity):
                            continue
                        response = seat.runtime.arbitrate(row, stream_seq=seat.stream)
                        if response.get("status") == "decision" and response.get("action_id") is not None:
                            scores = seat.runtime.states[actor_identity(row)].scores
                            allowed = [bool(a and b) for a, b in zip(response["effective_action_mask"], row["action_mask"])]
                            action, logp = sample_action(scores, allowed, seat.temperature, seat.rng)
                            env.act(seat.entity, action)
                            if not seat.learner:
                                stats["opponent_decisions"] += 1
                                continue
                            stats["decisions"] += 1
                            stats[f"action_{action}"] += 1
                            if rows and rows[-1]["seq"] == row["observation_seq"]:
                                rows[-1]["label"], rows[-1]["logp"] = action, logp
                                if allowed != rows[-1]["mask"]:
                                    stats["sampling_mask_differs"] += 1
                        elif response.get("status") == "fallback":
                            stats["fallbacks" if seat.learner else "opponent_fallbacks"] += 1
                            env.release_to_native(seat.entity, response.get("reason"))
                        else:
                            # The runtime declined an eligible row: re-check it tick by tick, as before.
                            undecided = True
                    if options.get("advance") and not undecided:
                        advanced = env.advance(watches(), min(ADVANCE_MAX_SUBSTEPS, options["max_substeps"]-ticks))
                        stats["advance_calls"] += 1
                    else:
                        advanced = env.step(1)
                    ticks += advanced
                    if advanced == 0 and not (env.arena.get("terminated") or env.arena.get("truncated")):
                        raise ArenaInfrastructureError("rollout made no simulation progress")
                final = env.status()
                for seat in seats:
                    seat.runtime.end_episode(initial["session_epoch"], initial["world_epoch"])
                actors = {actor["entity_id"]: actor for actor in final["actors"]}
                start = {actor["entity_id"]: actor for actor in initial["actors"]}
                summary = {"worker": worker, "name": name, "task": task, "seed": seed, "side": side, "distance_cm": distance,
                           "geometry": geometry, "opponent": against if not owned else (Path(against).stem if Path(against).stem != "best" else Path(against).parent.name),
                           "opponent_sha256": pool[against][1] if owned else None,
                           "learner_seat": seat_role, "arena_reason": final.get("reason"),
                           "reason": final.get("reason") if seat_role == "primary" else
                           {"primary_down": "opponent_down", "opponent_down": "primary_down"}.get(final.get("reason"), final.get("reason")),
                           "outcome": final.get("outcome"), "elapsed_game_ms": final.get("elapsed_game_ms", 0),
                           "primary_hp": actors[me]["hp"], "primary_hp_max": start[me].get("hp_max", start[me]["hp"]),
                           "opponent_hp": actors[rival]["hp"], "opponent_hp_max": start[rival].get("hp_max", start[rival]["hp"]),
                           "rows": len(rows), "ticks": ticks, "reset_s": reset_s, "rollout_s": time.monotonic()-rollout_started,
                           "temperature": options["temperature"], "checkpoint_sha256": policy.checkpoint_sha256, **stats}
                summary["success"] = success(summary)
                summary["score"] = score(summary, timeout_ms)
                if rows:
                    numpy.savez_compressed(output/f"{name}.npz",
                                           features=numpy.asarray([row["features"] for row in rows], dtype=numpy.float32),
                                           masks=numpy.asarray([row["mask"] for row in rows], dtype=bool),
                                           resets=numpy.asarray([row["reset"] for row in rows], dtype=bool),
                                           labels=numpy.asarray([row["label"] for row in rows], dtype=numpy.int16),
                                           logp=numpy.asarray([row["logp"] for row in rows], dtype=numpy.float64),
                                           t_ms=numpy.asarray([row["t"] for row in rows], dtype=numpy.int64),
                                           hp=numpy.asarray([row["hp"] for row in rows], dtype=numpy.float32),
                                           target_hp=numpy.asarray([row["target_hp"] for row in rows], dtype=numpy.float32))
                summaries.append(summary)
                with (output/"episodes.jsonl").open("a") as stream_file:
                    stream_file.write(json.dumps(summary)+"\n")
                if options["keep_journals"]:
                    preserve_melee_journals(output / "run", episode_dir, seed)
                else:
                    shutil.rmtree(episode_dir, ignore_errors=True)
            runner.call({"cmd": "close"})
            if options["keep_journals"] and summaries:
                previous = summaries[-1]
                preserve_melee_journals(output / "run", output / previous["name"], previous["seed"])
        finally:
            runner.terminate()
        return {"worker": worker, "episodes": len(summaries)}

    if len(envs) == 1:
        results = [run_env(envs[0])]
    else:
        with ThreadPoolExecutor(max_workers=len(envs)) as pool:
            results = [future.result() for future in [pool.submit(run_env, index) for index in envs]]
    return {"process": process, "envs": results, "batch_sizes": dict(backend.batches)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runner", "assets", "save", "reference", "scenario", "checkpoint", "output"):
        parser.add_argument("--"+name, type=Path, required=True)
    parser.add_argument("--workers", type=int, default=12)
    parser.add_argument("--episodes", type=int, default=96)
    parser.add_argument("--seed-start", type=int, default=20000)
    parser.add_argument("--temperature", type=float, default=1.0)
    parser.add_argument("--sampling-seed", type=int, default=1)
    parser.add_argument("--timeout-ms", type=int, default=60000)
    parser.add_argument("--max-substeps", type=int, default=2200)
    parser.add_argument("--keep-journals", action="store_true")
    parser.add_argument("--tasks", default="native_duel")
    parser.add_argument("--geometry", choices=("open", "near_wall"), default="open",
                        help="near_wall: the arena against a static wall, as the gate's screening")
    parser.add_argument("--wall-share", type=float, default=None,
                        help="share of near_wall arenas drawn per episode (default: all or none by --geometry)")
    parser.add_argument("--opponent", type=Path, action="append", default=[],
                        help="a frozen checkpoint of the self-play pool; duels against it own both actors")
    parser.add_argument("--native-share", type=float, default=0.5,
                        help="with --opponent: share of duels that keep the native opponent")
    parser.add_argument("--opponent-temperature", type=float, default=1.0)
    parser.add_argument("--learner-seat", choices=("random", "primary", "opponent"), default="random",
                        help="the learner's actor in a duel against the pool (native duels: always primary)")
    parser.add_argument("--opponent-first", action="store_true",
                        help="when both owned actors decide on one tick, the opponent's command goes first")
    parser.add_argument("--start-substeps", type=int, default=1,
                        help="ticks before the first decision; 1 matches the realtime driver (9.2 rounds used 2)")
    parser.add_argument("--envs-per-worker", type=int, default=1,
                        help="env threads per process, sharing one model; envs = workers x this")
    parser.add_argument("--inference", choices=("exact", "batched"), default="exact",
                        help="exact: row by row as the realtime runtime; batched: one step for all waiting envs")
    parser.add_argument("--no-fork-prefix", dest="fork_prefix", action="store_false",
                        help="replay the bench opening in every fork instead of once in the template")
    parser.add_argument("--reference-runtime", dest="lean_runtime", action="store_false",
                        help="gate packer and digest-checked runtime instead of the lean training ones")
    parser.add_argument("--per-tick", dest="advance", action="store_false",
                        help="poll every tick instead of the runner's arena_advance to the next decision")
    parser.add_argument("--command-ttl-ms", type=int, default=100,
                        help="native command TTL; 100 is the stage 8 realtime runtime (9.2 rounds used 1000)")
    args = parser.parse_args()
    # Runners work in their own directory: a relative output would put their journals elsewhere,
    # beyond the cleanup of --keep-journals-less runs.
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    distances = (250, 300, 350, 400)
    kinds = args.tasks.split(",")
    tasks = []
    for index in range(args.episodes):
        task = kinds[index % len(kinds)]
        distance = TASK_SETUP[task][0] if task in TASK_SETUP else distances[(index//2) % len(distances)]
        tasks.append([task, args.seed_start+index, (index//len(kinds)) % 2, distance])
    wall_share = args.wall_share if args.wall_share is not None else float(args.geometry == "near_wall")
    args.opponent = [str(path.resolve()) for path in args.opponent]
    assign(tasks, seed=args.seed_start, wall_share=wall_share, opponents=args.opponent,
           native_share=args.native_share, learner_seat=args.learner_seat)
    options = {key: str(value) if isinstance(value, Path) else value for key, value in vars(args).items()}
    (args.output/"collect.json").write_text(json.dumps({"schema_version": 1, "kind": "body_rl_collect",
                                                        "quality_credit": False, "options": options,
                                                        "training_env": TRAINING_ENV}, indent=2)+"\n")
    started = time.monotonic()
    envs = min(args.workers*args.envs_per_worker, len(tasks))
    groups = [list(range(index, min(index+args.envs_per_worker, envs))) for index in range(0, envs, args.envs_per_worker)]
    batch_sizes: Counter = Counter()
    context = multiprocessing.get_context("spawn")
    with context.Manager() as manager, ProcessPoolExecutor(max_workers=len(groups), mp_context=context) as pool:
        pending = manager.Queue()
        for task in tasks:
            pending.put(task)
        futures = [pool.submit(run_worker, index, options, group, pending) for index, group in enumerate(groups)]
        for future in futures:
            batch_sizes.update({int(size): count for size, count in future.result()["batch_sizes"].items()})
    wall = time.monotonic()-started
    summaries = [json.loads(line) for path in sorted(args.output.glob("worker-*/episodes.jsonl")) for line in path.read_text().splitlines()]
    wins = sum(row["reason"] == "opponent_down" for row in summaries)
    decisions = sum(row.get("decisions", 0) for row in summaries)
    by_task = {}
    for row in summaries:
        entry = by_task.setdefault(row.get("task", "native_duel"), [0, 0])
        entry[0] += int(row.get("success", row["reason"] == "opponent_down"))
        entry[1] += 1
    against = Counter(row.get("opponent", "native") for row in summaries)
    report = {"episodes": len(summaries), "wall_s": wall, "wins": wins, "opponents": dict(sorted(against.items())),
              "wins_by_opponent": {name: sum(row["reason"] == "opponent_down" for row in summaries if row.get("opponent", "native") == name)
                                   for name in sorted(against)},
              "sampling_mask_differs": sum(row.get("sampling_mask_differs", 0) for row in summaries),
              "opponent_fallbacks": sum(row.get("opponent_fallbacks", 0) for row in summaries),
              "success_by_task": {task: f"{done}/{total}" for task, (done, total) in sorted(by_task.items())}, "losses": sum(row["reason"] == "primary_down" for row in summaries),
              "timeouts": sum(row["reason"] not in ("opponent_down", "primary_down") for row in summaries),
              "mean_score": sum(row["score"] for row in summaries)/max(1, len(summaries)),
              "mean_primary_hp": sum(row["primary_hp"] for row in summaries)/max(1, len(summaries)),
              "mean_elapsed_s": sum(row["elapsed_game_ms"] for row in summaries)/1000/max(1, len(summaries)),
              "decisions": decisions, "decisions_per_s": decisions/wall, "episodes_per_min": len(summaries)/wall*60,
              "fallbacks": sum(row.get("fallbacks", 0) for row in summaries),
              "envs": envs, "processes": len(groups), "inference": args.inference,
              "inference_batch_sizes": dict(sorted(batch_sizes.items()))}
    (args.output/"summary.json").write_text(json.dumps(report, indent=2)+"\n")
    print(json.dumps(report), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
