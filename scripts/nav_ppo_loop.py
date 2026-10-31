#!/usr/bin/env python3
"""Run restartable navigation PPO phases through the shared Ryzen queue.

Only the caller-supplied training and independent validation sets are read.
The final test is deliberately a separate, explicit command. Recordings are
kept for the active iteration only, after its selected update is durable.
"""
from __future__ import annotations

import argparse
from collections import Counter
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import random
import signal
import shutil
import subprocess
import sys
import time


REGIONS = {"market", "harbour", "onar", "lobart", "port_streets", "upper_streets"}
HEADS = ("forward", "strafe", "turn", "walk", "jump", "climb")
GIB = 1024 ** 3


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def write_json(path, value):
    """A killed driver leaves either the previous state or the complete new one."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(path)


def pairs(path):
    return [json.loads(line) for line in Path(path).read_text(encoding="utf-8").splitlines() if line.strip()]


def route_keys(values):
    result = set()
    for pair in values:
        key = pair["start"], pair["goal"]
        if key[0] == key[1] or key in result or key[::-1] in result:
            raise ValueError(f"Repeated route or reverse route: {key}")
        result.add(key)
    return result


def validate_sets(training, validation):
    if len(training) < 1000:
        raise ValueError("PPO needs at least 1000 distinct training pairs")
    if {pair["region"] for pair in training} != REGIONS:
        raise ValueError("Training must cover all six regions")
    if any(not pair.get("detour_complete") or not 0 < pair.get("detour_length_cm", 0) for pair in training):
        raise ValueError("Every training pair needs a complete positive-length Detour path")
    if len(validation) != 60 or Counter(pair["region"] for pair in validation) != Counter({region: 10 for region in REGIONS}):
        raise ValueError("Independent validation needs exactly ten routes in each region")
    if any(not pair.get("final_test") or not pair.get("teacher_success") or pair.get("teacher_version") != 2 for pair in validation):
        raise ValueError("Validation must have frozen v2 teacher qualification for nav_rl drive")
    train_keys, validation_keys = route_keys(training), route_keys(validation)
    if any(key in train_keys or key[::-1] in train_keys for key in validation_keys):
        raise ValueError("Training overlaps independent validation")
    short = [pair for pair in training if pair["detour_length_cm"] <= 6000]
    if {pair["region"] for pair in short} != REGIONS:
        raise ValueError("The <=60m curriculum needs routes in every region")
    return short


def balanced_routes(values, seed):
    """Each consecutive block starts routes in all districts, with fresh order."""
    generator = random.Random(seed)
    groups = {region: [pair for pair in values if pair["region"] == region] for region in sorted(REGIONS)}
    for group in groups.values():
        generator.shuffle(group)
    regions = list(groups)
    generator.shuffle(regions)
    return [groups[region][index] for index in range(max(map(len, groups.values())))
            for region in regions if index < len(groups[region])]


def validation_score(metrics):
    """Prefer both task targets, then arrival, turning, blocking and time."""
    all_metrics = metrics["all"]
    elapsed = all_metrics.get("median_arrival_s")
    feasible = all_metrics["arrived"] >= 42 and all_metrics["action_fraction"]["turn_only"] <= .15
    return (int(feasible), all_metrics["arrived"], -all_metrics["action_fraction"]["turn_only"],
            -all_metrics["blocked_ms"], -(elapsed if elapsed is not None else 1e30))


def record_validation(state, metrics, iteration, policy, checkpoint):
    """Update stop evidence only at independent greedy validation boundaries."""
    state = dict(state)
    rate = metrics["all"]["arrival_fraction"]
    previous_best = state["best_arrival_fraction"]
    state["bad_validations"] = state.get("bad_validations", 0) + 1 if previous_best - rate > .15 + 1e-12 else 0
    target = (state["decisions"] >= 5_000_000 and metrics["all"]["arrived"] >= 50
              and metrics["all"]["action_fraction"]["turn_only"] <= .12)
    state["target_validations"] = state.get("target_validations", 0) + 1 if target else 0
    state["last_validation_decisions"] = state["decisions"]
    state["best_arrival_fraction"] = max(previous_best, rate)
    improved = validation_score(metrics) > tuple(state["best_score"])
    if improved:
        state.update(selected_arrival_fraction=rate, best_score=list(validation_score(metrics)),
                     best_iteration=iteration, best_policy=str(policy),
                     best_checkpoint=str(checkpoint) if checkpoint else None)
    if state["bad_validations"] >= 2:
        state["stop_reason"] = "validation_collapse_two_consecutive_drops_gt_15pp"
    elif state["decisions"] >= 30_000_000 and state["best_arrival_fraction"] - state["baseline_arrival_fraction"] < .05 - 1e-12:
        state["stop_reason"] = "no_5pp_validation_improvement_at_30m_decisions"
    elif state["target_validations"] >= 2:
        state["stop_reason"] = "validation_target_sustained"
    return state, improved


def kl_coefficient(initial, decisions, maximum, floor=.003):
    fraction = min(1., max(0., decisions / maximum))
    return initial * (floor / initial) ** fraction


def training_coefficient(state):
    schedule = state["kl_schedule"]
    return kl_coefficient(schedule["initial"], state["decisions"] - schedule["start_decisions"],
                          schedule["decay_decisions"], schedule["floor"])


def configure_training(state, args):
    """Record an operator change once; resuming never restarts its KL clock."""
    config = dict(actor_lr=args.lr, batch_windows=args.batch, target_kl=args.target_kl,
                  kl_start=args.kl_start, kl_floor=args.kl_floor,
                  kl_decay_decisions=args.kl_decay_decisions)
    if state.get("training_config") == config:
        return state
    previous = state.get("training_config", dict(actor_lr=3e-5, batch_windows=8, target_kl=.03,
                         kl_start=None, kl_floor=.003, kl_decay_decisions=args.max_decisions))
    initial = args.kl_start
    if initial is None:
        initial = training_coefficient(state) if "kl_schedule" in state else kl_coefficient(
            state["initial_kl"] or args.pilot_kl[0], state["decisions"], args.max_decisions)
    result = dict(state, training_config=config, kl_schedule=dict(initial=initial, floor=args.kl_floor,
                  start_decisions=state["decisions"], decay_decisions=args.kl_decay_decisions))
    if state["iteration"]:
        change = dict(after_iteration=state["iteration"], effective_from_iteration=state["iteration"] + 1,
                      decisions=state["decisions"], previous=previous, current=config,
                      kl_schedule=result["kl_schedule"],
                      time=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
        result["parameter_changes"] = [*state.get("parameter_changes", []), change]
    return result


def collection_decision_bound(run_ms, workers, population):
    # Runner simulation ticks are 33ms. A reset can introduce at most one
    # additional initial action per tick, besides the ordinary 100ms decisions.
    return workers * population * (1 + math.ceil(run_ms / 33) + math.ceil(run_ms / 100))


def select_pilot(results, target_kl=.1):
    """Constrain drift and entropy relative to round 2 on this same batch."""
    viable = []
    for candidate in results:
        metrics = candidate["stats"]
        if "reference_entropy_by_head" not in metrics:
            raise KeyError("KL pilot requires current-batch reference_entropy_by_head; rerun cached pilot updates")
        reference_entropy = metrics["reference_entropy_by_head"]
        if (math.isfinite(metrics["kl_reference"]) and metrics["kl_reference"] <= target_kl
                and all(math.isfinite(metrics["entropy_by_head"][head])
                        and math.isfinite(reference_entropy[head]) and reference_entropy[head] >= 0
                        and metrics["entropy_by_head"][head] >= .75 * reference_entropy[head] for head in HEADS)):
            viable.append(candidate)
    if not viable:
        raise ValueError("KL pilot has no candidate below target KL with >=75% entropy in every head")
    return min(viable, key=lambda candidate: candidate["coefficient"])


def update_complete(directory):
    return all((directory / name).is_file() for name in ("last.pt", "policy.navbc", "stats.json"))


def collect_complete(directory, workers, population):
    path = directory / f"throughput-n{population}-r{workers}.json"
    return path.is_file()


def collection_health(result):
    """Count rejected episodes against all episodes, including zero-action ones."""
    workers = result.get("workers", [])
    episodes = result.get("episodes", result.get("collected_episodes", sum(worker.get("episodes", 0) for worker in workers)))
    rejected = result.get("rejected_nonfinite_episodes", sum(worker.get("rejected_nonfinite_episodes", 0) for worker in workers))
    rejected_decisions = result.get("rejected_nonfinite_decisions", sum(worker.get("rejected_nonfinite_decisions", 0) for worker in workers))
    decisions = result.get("collected_decisions", result.get("decisions"))
    if any(not isinstance(value, int) or value < 0 for value in (episodes, rejected, rejected_decisions, decisions)):
        raise ValueError("Collection health counts must be nonnegative integers")
    if episodes == 0 or rejected > episodes or rejected_decisions > decisions:
        raise ValueError("Collection health counts are inconsistent")
    diagnostics = result.get("rejected_nonfinite_diagnostics", result.get("rejected_episode_details", []))
    return dict(episodes=episodes, accepted_episodes=episodes - rejected, decisions=decisions,
                accepted_decisions=decisions - rejected_decisions,
                rejected_nonfinite_episodes=rejected, rejected_nonfinite_decisions=rejected_decisions,
                rejected_nonfinite_fraction=rejected / episodes,
                rejected_nonfinite_diagnostics=diagnostics, rejected_nonfinite_limit=.001,
                # Integer arithmetic preserves the inclusive 0.1% boundary.
                limit_exceeded=rejected * 1000 > episodes)


def updated_collection_health(result, stats):
    """Accept updater quarantine discoveries without losing collected decisions."""
    summary = dict(result)
    for destination, source in (("episodes", "collected_episodes"), ("decisions", "collected_decisions"),
                                ("rejected_nonfinite_episodes", "rejected_nonfinite_episodes"),
                                ("rejected_nonfinite_decisions", "rejected_nonfinite_decisions"),
                                ("rejected_nonfinite_diagnostics", "rejected_episode_details")):
        if source in stats:
            summary[destination] = stats[source]
    health = collection_health(summary)
    original = collection_health(result)
    if (health["decisions"] != original["decisions"] or health["episodes"] != original["episodes"]
            or health["rejected_nonfinite_episodes"] < original["rejected_nonfinite_episodes"]
            or health["rejected_nonfinite_decisions"] < original["rejected_nonfinite_decisions"]
            or stats["decisions"] != health["accepted_decisions"]):
        raise ValueError("PPO accepted/rejected counts differ from population collection")
    return health


class RejectedCollectionError(ValueError):
    def __init__(self, collect, iteration, health):
        super().__init__("Nonfinite observations rejected more than 0.1% of collected episodes")
        self.collect, self.iteration, self.health = collect, iteration, health


class NoViablePilotError(ValueError):
    def __init__(self, collect, iteration, health, selection):
        super().__init__("KL pilot has no viable candidate")
        self.collect, self.iteration, self.health = collect, iteration, health
        self.diagnosis = dict(iteration=iteration, **selection)


def load_pilot_selection(path, collect, iteration, health):
    selection = read_json(path)
    if selection["selected"] is None:
        raise NoViablePilotError(collect, iteration, health, selection)
    return selection["selected"]


def remove_raw(directory):
    """Retain episode outcomes; remove observations, videos and runner scratch."""
    for path in list(directory.rglob("*")):
        if path.is_file() and path.name not in {"episode.json", "episodes.jsonl", "population.json", "measurement.json"}:
            path.unlink()
    for path in sorted(directory.rglob("*"), key=lambda value: len(value.parts), reverse=True):
        if path.is_dir() and not any(path.iterdir()):
            path.rmdir()


def trim_validation(directory):
    # Metrics already include action and movement statistics. Keep those, the
    # per-route outcomes and text trajectories; sensor tensors are never reused.
    for name in ("samples.bin.gz", "samples.jsonl", "bootstrap.bin.gz"):
        for path in directory.rglob(name):
            path.unlink()


class Loop:
    def __init__(self, args):
        self.args = args
        self.work = args.work
        self.script = Path(__file__).resolve().parent
        self.state_path = self.work / "state.json"
        self.env = dict(os.environ, GQ_OWNER="nav-ppo", OMP_NUM_THREADS=str(args.threads),
                        MKL_NUM_THREADS=str(args.threads), OPENBLAS_NUM_THREADS=str(args.threads))
        self.process = None

    def interrupted(self, signum, frame):
        if self.process and self.process.poll() is None:
            self.process.send_signal(signum)
        # Popen.wait may already hold its non-reentrant waitpid lock when the
        # signal arrives. Reap only after unwinding that wait, in queued.finally.
        raise SystemExit(128 + signum)

    def event(self, phase, **values):
        row = dict(time=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), phase=phase, **values)
        print(json.dumps(row, ensure_ascii=False), flush=True)

    def queued(self, name, command, log, *, gpu=False, threads=None):
        args = self.args
        resources = ["--class", "batch", "--threads", str(threads or args.workers + 1),
                     "--mem", "10G" if gpu else "11G", "--max", "2h", "--name", name]
        if gpu:
            resources += ["--gpu-mem", "4G"]
        shell = 'source "$1"; shift; gq_job "$@"'
        command = list(map(str, command))
        # All PPO phase commands use the selected Python environment. -X also
        # survives -I, which ignores the inherited faulthandler environment.
        command[1:1] = ["-X", "faulthandler"]
        queued_command = ["bash", "-c", shell, "nav-ppo-gq", str(self.script / "gq-job.sh"),
                          *resources, "--", str(args.python), "-I", str(self.script / "nav_job.py"),
                          "--max-seconds", "7200", "--", *command]
        log.parent.mkdir(parents=True, exist_ok=True)
        self.event("queued", name=name, log=str(log))
        with log.open("a", encoding="utf-8") as stream:
            self.process = subprocess.Popen(queued_command, env=self.env, cwd=self.work, stdout=stream, stderr=subprocess.STDOUT)
            try:
                result = self.process.wait()
                if result:
                    raise subprocess.CalledProcessError(result, queued_command)
            finally:
                if self.process.poll() is None:
                    self.process.wait()
                self.process = None

    def common(self):
        args = self.args
        result = ["--runner", args.runner, "--assets", args.assets, "--save", args.save,
                  "--cache", args.cache, "--links", args.links, "--roads", args.roads,
                  "--physics", "cv", "--watchdog-seconds", "6900"]
        if args.ini:
            result += ["--ini", args.ini]
        return result

    def evaluate(self, policy, directory):
        metrics = directory / "metrics.json"
        if not metrics.is_file():
            self.queued(f"nav PPO {directory.parent.name} validation",
                        [self.args.python, self.script / "nav_rl.py", "drive", "--weights", policy,
                         "--pairs", self.args.validation, "--output", directory, "--parallel", str(self.args.workers),
                         "--limit-ms", str(self.args.limit_ms), "--seed", str(self.args.seed), "--resume", *self.common()],
                        directory.parent / "launcher-validation.log")
            # Summarization reads NAVDEMO and can take noticeable CPU for 60 drives.
            self.queued(f"nav PPO {directory.parent.name} validation metrics",
                        [self.args.python, self.script / "nav_rl.py", "summarize", "--source", directory, "--output", directory],
                        directory.parent / "launcher-validation-metrics.log", threads=2)
        result = read_json(metrics)
        if result["all"]["n"] != 60:
            raise ValueError("Validation did not measure all 60 frozen routes")
        trim_validation(directory)
        return result

    def update(self, collect, output, previous, iteration, coefficient, critic_only):
        if update_complete(output):
            shutil.rmtree(output / "cache", ignore_errors=True)
            return read_json(output / "stats.json")
        if output.exists():
            shutil.rmtree(output)
        command = [self.args.python, self.script / "nav_ppo.py", "update", "--source", collect, "--output", output,
                   "--reference", self.args.reference, "--iteration", str(iteration), "--kl-coef", str(coefficient),
                   "--device", "cuda", "--threads", str(self.args.threads),
                   "--lr", str(self.args.lr), "--batch", str(self.args.batch)]
        if previous:
            command += ["--resume", previous]
        if critic_only:
            command += ["--critic-only"]
        try:
            self.queued(f"nav PPO iter-{iteration:06d} {'critic' if critic_only else 'update'} kl={coefficient:g}",
                        command, output.parent / f"launcher-{output.name}.log", gpu=True, threads=self.args.threads)
        except subprocess.CalledProcessError:
            # Unexpected nonfinite raw observations may be quarantined by the
            # updater's prepass. Its threshold check runs before optimization.
            rejected = output / "rejected-episodes.json"
            if rejected.is_file():
                health = collection_health(read_json(rejected))
                if health["limit_exceeded"]:
                    raise RejectedCollectionError(collect, iteration, health) from None
            raise
        if not update_complete(output):
            raise ValueError(f"Update did not create all checkpoint outputs: {output}")
        stats = read_json(output / "stats.json")
        if stats.get("peak_cuda_gb", 0) > 4:
            raise ValueError(f"Update exceeded 4 GB GPU budget: {stats['peak_cuda_gb']}")
        shutil.rmtree(output / "cache", ignore_errors=True)
        return stats

    def curves(self):
        rows = [read_json(path) for path in sorted(self.work.glob("iter-*/iteration.json"))]
        temporary = self.work / "curves.jsonl.tmp"
        temporary.write_text("".join(json.dumps(row, ensure_ascii=False, allow_nan=False) + "\n" for row in rows), encoding="utf-8")
        temporary.replace(self.work / "curves.jsonl")

    def restore_best(self, state):
        destination = self.work / "best"
        destination.mkdir(exist_ok=True)
        shutil.copy2(state["best_policy"], destination / "policy.navbc")
        if state.get("best_checkpoint"):
            shutil.copy2(state["best_checkpoint"], destination / "last.pt")
        elif (destination / "last.pt").exists():
            (destination / "last.pt").unlink()
        write_json(destination / "selection.json", {key: state[key] for key in
                   ("best_iteration", "best_score", "best_arrival_fraction", "selected_arrival_fraction", "best_policy", "best_checkpoint")})

    def run(self):
        try:
            self._run()
        except RejectedCollectionError as exc:
            self.stop_iteration(exc.collect, exc.iteration, exc.health,
                                "nonfinite_observation_rate_gt_0_1_percent")
        except NoViablePilotError as exc:
            self.stop_iteration(exc.collect, exc.iteration, exc.health,
                                "kl_pilot_no_viable_candidate", exc.diagnosis)

    def stop_iteration(self, collect, iteration, health, reason, pilot_diagnosis=None):
        # A stopped phase consumes its environment decisions exactly once. Its
        # checkpoint stays at the previous successful update; only an explicit
        # caller reset can allow a later iteration to run.
        state = read_json(self.state_path)
        if state["iteration"] < iteration:
            state["iteration"] = iteration
            state["decisions"] += health["decisions"]
        state.update(stop_reason=reason, collection_health=health)
        row = dict(iteration=iteration, decisions=state["decisions"],
                   iteration_decisions=health["decisions"],
                   critic_only=iteration <= self.args.critic_warmup,
                   collection_health=health, training=None, validation=None,
                   validation_by_region=None, stop_reason=reason)
        if pilot_diagnosis is not None:
            state["pilot_diagnosis"] = pilot_diagnosis
            row["pilot_diagnosis"] = pilot_diagnosis
        directory = collect.parent
        write_json(directory / "collection-health.json", health)
        write_json(directory / "iteration.json", row)
        write_json(self.state_path, state)
        self.curves()
        self.restore_best(state)
        self.event("stopped", reason=reason, decisions=state["decisions"],
                   rejected_nonfinite_episodes=health["rejected_nonfinite_episodes"],
                   rejected_nonfinite_fraction=health["rejected_nonfinite_fraction"])

    def _run(self):
        args = self.args
        self.work.mkdir(parents=True, exist_ok=True)
        # Keep this descriptor open until the process exits, including queue waits.
        self.lock = (self.work / ".loop.lock").open("a")
        try:
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise ValueError("Another navigation PPO driver owns this work directory") from exc
        for signum in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
            signal.signal(signum, self.interrupted)
        if not (Path(os.environ.get("GQ_BIN", str(Path.home() / "gothic-ai/bin/gq")))).is_file():
            raise ValueError("gq is required; navigation PPO never bypasses the shared queue")
        training, validation = pairs(args.training), pairs(args.validation)
        short = validate_sets(training, validation)
        short_file = self.work / "training-short.jsonl"
        text = "".join(json.dumps(pair, ensure_ascii=False) + "\n" for pair in short)
        if short_file.exists() and short_file.read_text(encoding="utf-8") != text:
            raise ValueError("The short curriculum changed during a durable run")
        short_file.write_text(text, encoding="utf-8")
        input_hashes = {name: hashlib.sha256(getattr(args, name).read_bytes()).hexdigest()
                        for name in ("training", "validation", "reference")}
        if self.state_path.is_file():
            state = read_json(self.state_path)
            if "selected_arrival_fraction" not in state or len(state.get("best_score", ())) != 5:
                raise ValueError("Incompatible checkpoint selection state; use a new --work directory")
            if state["training"] != str(args.training) or state["validation"] != str(args.validation) or state["reference"] != str(args.reference):
                raise ValueError("Resume requires the original training, validation and round-2 reference")
            if state["input_hashes"] != input_hashes:
                raise ValueError("Training, validation or round-2 weights changed during the run")
        else:
            # Read only hardware counters. Actual GPU allocation happens in gq.
            subprocess.run(["nvidia-smi"], check=True)
            subprocess.run(["df", "-h", str(self.work)], check=True)
            baseline = self.evaluate(args.reference, self.work / "baseline" / "validation")
            state = dict(iteration=0, decisions=0, baseline_arrival_fraction=baseline["all"]["arrival_fraction"],
                         best_arrival_fraction=baseline["all"]["arrival_fraction"], selected_arrival_fraction=baseline["all"]["arrival_fraction"],
                         best_score=list(validation_score(baseline)),
                         best_iteration=0, best_policy=str(args.reference), best_checkpoint=None,
                         current_policy=str(args.reference), current_checkpoint=None, bad_validations=0,
                         initial_kl=None, baseline_entropy=None, stop_reason=None,
                         training=str(args.training), validation=str(args.validation), reference=str(args.reference),
                         last_validation_decisions=0, input_hashes=input_hashes)
            write_json(self.state_path, state)
            self.restore_best(state)
        if state.get("stop_reason"):
            self.restore_best(state)
            self.event("stopped", reason=state["stop_reason"], decisions=state["decisions"], best=state["best_iteration"])
            return
        configured = configure_training(state, args)
        if configured != state:
            state = configured
            write_json(self.state_path, state)
            self.event("training_configured", iteration=state["iteration"], decisions=state["decisions"],
                       config=state["training_config"], kl_schedule=state["kl_schedule"])
        while state["decisions"] < args.max_decisions:
            bound = collection_decision_bound(args.run_ms, args.workers, args.population)
            if state["decisions"] + bound > args.max_decisions:
                state["stop_reason"] = "maximum_decisions_conservative_collection_bound"
                write_json(self.state_path, state)
                break
            iteration = state["iteration"] + 1
            directory = self.work / f"iter-{iteration:06d}"
            directory.mkdir(exist_ok=True)
            collect = directory / "collect"
            output = directory / "update"
            summary = directory / "collect-summary.json"
            critic_only = iteration <= args.critic_warmup
            if not summary.is_file():
                if shutil.disk_usage(self.work).free < args.min_free_gb * GIB:
                    raise ValueError(f"Less than {args.min_free_gb:g} GiB free before collection; retained checkpoints need cleanup")
                if not collect_complete(collect, args.workers, args.population):
                    if collect.exists():
                        shutil.rmtree(collect)
                    route_file = directory / "training-pairs.jsonl"
                    selected_pairs = short if state["decisions"] < args.curriculum_decisions else training
                    route_file.write_text("".join(json.dumps(pair, ensure_ascii=False) + "\n" for pair in
                                          balanced_routes(selected_pairs, args.seed + iteration * 1009)), encoding="utf-8")
                    self.queued(f"nav PPO iter-{iteration:06d} collect",
                                [args.python, self.script / "nav_rl.py", "population", "--output", collect,
                                 "--pairs", route_file,
                                 "--weights", state["current_policy"], "--population", str(args.population),
                                 "--parallel", str(args.workers), "--record", "--action-mode", "sample", "--hero", "far",
                                 "--episodes", "1000000", "--run-ms", str(args.run_ms), "--limit-ms", str(args.limit_ms),
                                 "--seed", str((args.seed + iteration * 1009) & 0xffffffff), *self.common()],
                                directory / "launcher-collect.log")
                result = read_json(collect / f"throughput-n{args.population}-r{args.workers}.json")
                write_json(summary, result)
            result = read_json(summary)
            health = collection_health(result)
            write_json(directory / "collection-health.json", health)
            if health["limit_exceeded"]:
                raise RejectedCollectionError(collect, iteration, health)
            coefficient = training_coefficient(state)
            if state["initial_kl"] is None and not critic_only:
                selection_file = directory / "pilot-selection.json"
                if not selection_file.is_file():
                    candidates = []
                    # The fixed short pilot brackets useful KL strengths. Only
                    # if all initial candidates fail, try stronger constraints
                    # on the same observations and frozen starting checkpoint.
                    coefficients = args.pilot_kl + [value for value in (30., 100.) if value > max(args.pilot_kl)]
                    for index, value in enumerate(coefficients):
                        if index >= len(args.pilot_kl):
                            try:
                                select_pilot(candidates, args.target_kl)
                                break
                            except ValueError:
                                pass
                        candidate_output = directory / f"pilot-{index}"
                        stats = self.update(collect, candidate_output, state["current_checkpoint"], iteration, value, False)
                        health = updated_collection_health(result, stats)
                        candidates.append(dict(coefficient=value, directory=str(candidate_output), stats=stats))
                    try:
                        selected = select_pilot(candidates, args.target_kl)
                    except ValueError:
                        selected = None
                    write_json(selection_file, dict(selected=selected, candidates=candidates))
                selected = load_pilot_selection(selection_file, collect, iteration, health)
                if not update_complete(output):
                    if output.exists():
                        shutil.rmtree(output)
                    shutil.copytree(selected["directory"], output)
                state["initial_kl"] = selected["coefficient"]
                coefficient = selected["coefficient"]
                state["kl_schedule"].update(initial=coefficient, start_decisions=state["decisions"])
                stats = read_json(output / "stats.json")
            else:
                stats = self.update(collect, output, state["current_checkpoint"], iteration, coefficient, critic_only)
            health = updated_collection_health(result, stats)
            write_json(directory / "collection-health.json", health)
            if health["limit_exceeded"]:
                raise RejectedCollectionError(collect, iteration, health)
            if not state["baseline_entropy"]:
                state["baseline_entropy"] = stats["entropy_by_head"]
            if any(not math.isfinite(stats["entropy_by_head"][head]) for head in HEADS):
                raise ValueError("Non-finite policy entropy")
            entropy_warning = [head for head in HEADS if stats["entropy_by_head"][head] < .5 * state["baseline_entropy"][head]]
            next_state = dict(state, iteration=iteration, decisions=state["decisions"] + result["decisions"],
                              current_policy=str(output / "policy.navbc"), current_checkpoint=str(output / "last.pt"),
                              collection_health=health)
            # A resumed phase can delete raw only after selected update files and
            # its metrics are durable; checkpoint state never depends on cache.
            remove_raw(collect)
            must_evaluate = (iteration % args.eval_every == 0 or next_state["decisions"] >= args.max_decisions
                             or next_state["decisions"] + bound > args.max_decisions
                             or state["decisions"] < 30_000_000 <= next_state["decisions"])
            metrics = None
            if must_evaluate:
                metrics = self.evaluate(next_state["current_policy"], directory / "validation")
                next_state, improved = record_validation(next_state, metrics, iteration,
                                                          next_state["current_policy"], next_state["current_checkpoint"])
                if improved:
                    self.restore_best(next_state)
            if next_state["decisions"] >= args.max_decisions and not next_state["stop_reason"]:
                next_state["stop_reason"] = "maximum_decisions"
            elif next_state["decisions"] + bound > args.max_decisions and not next_state["stop_reason"]:
                next_state["stop_reason"] = "maximum_decisions_conservative_collection_bound"
            row = dict(iteration=iteration, decisions=next_state["decisions"], iteration_decisions=result["decisions"],
                       critic_only=critic_only, curriculum="short" if state["decisions"] < args.curriculum_decisions else "all",
                       kl_coefficient=coefficient, entropy_warning_heads=entropy_warning, training=stats,
                       training_config=state["training_config"],
                       collection_health=health,
                       validation=metrics["all"] if metrics else None, validation_by_region=metrics["by_region"] if metrics else None,
                       stop_reason=next_state["stop_reason"])
            write_json(directory / "iteration.json", row)
            write_json(self.state_path, next_state)
            self.curves()
            state = next_state
            self.event("iteration_complete", iteration=iteration, decisions=state["decisions"],
                       validation_arrival_fraction=metrics["all"]["arrival_fraction"] if metrics else None,
                       kl_reference=stats["kl_reference"], entropy_warning_heads=entropy_warning)
            if state["stop_reason"]:
                break
        self.restore_best(state)
        self.event("stopped", reason=state["stop_reason"], decisions=state["decisions"], best=state["best_iteration"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, default=Path.home() / "gothic-ai/nav-ppo")
    for name in ("runner", "assets", "save", "cache", "links", "roads", "reference", "training", "validation"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--ini", type=Path)
    parser.add_argument("--python", type=Path, default=Path(sys.executable))
    parser.add_argument("--workers", type=int, choices=(1, 4, 8), default=8)
    parser.add_argument("--population", type=int, default=100)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--lr", type=float, default=1e-4)
    parser.add_argument("--batch", type=int, default=32)
    parser.add_argument("--run-ms", type=int, default=40000)
    parser.add_argument("--limit-ms", type=int, default=240000)
    parser.add_argument("--seed", type=int, default=19102026)
    parser.add_argument("--critic-warmup", type=int, default=3)
    parser.add_argument("--eval-every", type=int, default=5)
    parser.add_argument("--curriculum-decisions", type=int, default=5_000_000)
    parser.add_argument("--max-decisions", type=int, default=150_000_000)
    parser.add_argument("--min-free-gb", type=float, default=20)
    parser.add_argument("--pilot-kl", type=float, nargs="+", default=[.1, 1., 10.])
    parser.add_argument("--target-kl", type=float, default=.1)
    parser.add_argument("--kl-start", type=float)
    parser.add_argument("--kl-floor", type=float, default=.01)
    parser.add_argument("--kl-decay-decisions", type=int, default=10_000_000)
    args = parser.parse_args()
    for key, value in vars(args).items():
        if isinstance(value, Path):
            if not value.is_absolute():
                parser.error(f"--{key.replace('_', '-')} must be absolute")
            # A venv interpreter is commonly a symlink to the base Python.
            # Executing that resolved target loses the venv's installed packages.
            setattr(args, key, value if key == "python" else value.resolve())
    if sys.platform != "linux":
        parser.error("Run navigation PPO on Ryzen/Linux")
    if "holdout-cv" in str(args.validation) or "holdout-cv" in str(args.training):
        parser.error("Training loop cannot read the final holdout-cv test")
    if (args.population < 1 or args.threads < 1 or args.run_ms < 100 or args.run_ms > 40000
            or args.limit_ms < args.run_ms or args.critic_warmup < 1 or args.eval_every < 1
            or not 0 < args.max_decisions <= 150_000_000 or args.curriculum_decisions < 0
            or args.min_free_gb < 20 or any(value < .003 or not math.isfinite(value) for value in args.pilot_kl)):
        parser.error("Invalid safety limits, iteration schedule or KL coefficients")
    if (not math.isfinite(args.lr) or args.lr <= 0 or args.batch < 1
            or not math.isfinite(args.target_kl) or args.target_kl <= 0
            or not math.isfinite(args.kl_floor) or args.kl_floor <= 0 or args.kl_decay_decisions < 1
            or args.kl_start is not None and (not math.isfinite(args.kl_start) or args.kl_start < args.kl_floor)):
        parser.error("Invalid training parameters or KL decay schedule")
    try:
        Loop(args).run()
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"nav-ppo-loop: {exc}\n")


if __name__ == "__main__":
    main()
