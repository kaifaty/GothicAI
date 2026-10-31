#!/usr/bin/env python3
"""Bounded, resumable teacher-v3 campaign; execute inside the caller's gq batch.

Selection uses v2 and geometry only. No policy training, queue submission, report
writing or deletion of existing dataset episodes occurs here.
"""
from __future__ import annotations

import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, wait, FIRST_COMPLETED
from contextlib import contextmanager
from copy import copy
import importlib.util
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import threading
import time
from types import SimpleNamespace

_spec = importlib.util.spec_from_file_location("nav_teacher_v3", Path(__file__).with_name("nav_teacher_v3.py"))
v3 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(v3)


_PREPARE_LOCK = threading.Lock()


def setup_fraction_cache(task):
    """Memoize old v2 fractions without changing existing episode metadata."""
    path = Path(task) / "beyond-fractions.json"
    values = v3.read_json(path) if path.is_file() else {}
    original = getattr(v3.beyond_path_fraction, "uncached", v3.beyond_path_fraction)
    lock = threading.Lock()

    def cached(directory, demos=None):
        sample = Path(directory).resolve() / "samples.bin.gz"
        stamp = sample.stat()
        key = f"{sample}|{stamp.st_size}|{stamp.st_mtime_ns}"
        with lock:
            if key not in values:
                values[key] = original(directory, demos)
                v3.write_json(path, values)
            return values[key]

    cached.uncached = original
    v3.beyond_path_fraction = cached


def emit(event, **values):
    print(json.dumps(dict(event=event, **values), ensure_ascii=False), flush=True)


def gate(selected, regions, min_points=30, min_objects=30, min_places=30, per_region=5, object_regions=()):
    counts = v3.qualification_gate(selected, minimum_points=min_points, minimum_objects=min_objects,
                                   minimum_object_places=min_places, required_point_regions=regions,
                                   minimum_points_per_region=per_region)
    objects = Counter(p["region"] for p in selected if p["goal_kind"] == "object")
    missing = sorted(set(object_regions) - objects.keys())
    if missing:
        raise ValueError(f"Qualification gate: missing object regions {missing}")
    counts["object_regions"] = dict(sorted(objects.items()))
    return counts


def point_stop(comparison):
    rate = comparison.get("conditional_success_fraction")
    return rate is not None and rate < .5


class PendingSchedule:
    """One candidate per take, balanced across unmet quotas; finish drains all.

    Inflight episodes balance dispatch, but never count as successful outcomes.
    This deliberately retains every qualified inflight result after a quota fills.
    """
    def __init__(self, pool, qualified, attempted_ids, parallel=3, min_points=30, min_objects=30,
                 min_places=30, per_region=5, regions=(), object_regions=()):
        self.pool = list(pool)
        self.qualified = list(qualified)
        self.attempted = set(attempted_ids)
        self.parallel = parallel
        self.min_points, self.min_objects, self.min_places = min_points, min_objects, min_places
        self.per_region, self.regions, self.object_regions = per_region, tuple(regions), tuple(object_regions)
        self.inflight = set()
        self.running = {}

    def take(self):
        if len(self.inflight) >= self.parallel:
            return None
        points = [p for p in self.qualified if p["goal_kind"] == "point"]
        objects = [p for p in self.qualified if p["goal_kind"] == "object"]
        pc = Counter(p["region"] for p in points)
        oc = Counter(p["region"] for p in objects)
        places = {p["place_id"] for p in objects}
        need_points = len(points) < self.min_points or any(pc[r] < self.per_region for r in self.regions)
        need_objects = (len(objects) < self.min_objects or len(places) < self.min_places
                        or any(not oc[r] for r in self.object_regions))
        projected_points = pc + Counter(p["region"] for p in self.running.values() if p["goal_kind"] == "point")
        projected_places = places | {p["place_id"] for p in self.running.values() if p["goal_kind"] == "object"}
        projected_objects = oc + Counter(p["region"] for p in self.running.values() if p["goal_kind"] == "object")
        candidates = []
        for index, value in enumerate(self.pool):
            if value["id"] in self.attempted or value["id"] in self.inflight:
                continue
            kind = value["goal_kind"]
            if kind == "point" and need_points:
                score = (0 if pc[value["region"]] < self.per_region else 2,
                         projected_points[value["region"]], index)
            elif kind == "object" and need_objects:
                score = (0 if not oc[value["region"]] and value["region"] in self.object_regions else 1,
                         int(value["place_id"] in projected_places), projected_objects[value["region"]], index)
            else:
                continue
            candidates.append((score, value))
        if not candidates:
            return None
        value = min(candidates, key=lambda pair: pair[0])[1]
        self.inflight.add(value["id"])
        self.running[value["id"]] = value
        return value

    def finish(self, value, eligible):
        self.inflight.discard(value["id"])
        self.running.pop(value["id"], None)
        self.attempted.add(value["id"])
        if eligible and not any(p["id"] == value["id"] for p in self.qualified):
            self.qualified.append(value)


def tree_bytes(root):
    root = Path(root)
    total = 0
    if root.exists():
        for path in root.rglob("*"):
            try:
                if path.is_file() and not path.is_symlink():
                    total += path.stat().st_size
            except FileNotFoundError:
                continue
    return total


class StorageBudget:
    """Aggregate dataset + this campaign's new attempts, with concurrent reserves."""
    def __init__(self, task, data, max_bytes=v3.MAX_BYTES):
        self.task, self.data = Path(task).resolve(), Path(data).resolve()
        if not 0 < max_bytes <= v3.MAX_BYTES:
            raise ValueError("Disk budget must be positive and at most 15 GiB")
        self.max_bytes, self.reserved = max_bytes, 0
        self.lock = threading.Lock()
        self.data.mkdir(parents=True, exist_ok=True)

    def used(self):
        attempts = self.task / "v2-attempts"
        # Avoid double accounting if a custom data path contains the task.
        return tree_bytes(self.data) + (0 if attempts.is_relative_to(self.data) else tree_bytes(attempts))

    @contextmanager
    def reserve(self, limit_ms):
        if limit_ms <= 0:
            raise ValueError("Positive decision duration required")
        amount = math.ceil(limit_ms / 100) * v3.ESTIMATED_DECISION_BYTES
        with self.lock:
            used = self.used()
            free = shutil.disk_usage(self.data).free
            if used + self.reserved + amount > self.max_bytes or free < self.reserved + amount + 256 * 1024**2:
                raise ValueError(f"Disk budget: used={used}, reserved={self.reserved}, next={amount}, limit={self.max_bytes}, free={free}")
            self.reserved += amount
        try:
            yield
        finally:
            with self.lock:
                self.reserved -= amount
                used = self.used()
                if used > self.max_bytes:
                    raise ValueError(f"Recording disk budget exceeded: {used} > {self.max_bytes}")


def resume_metadata(directory, value, version, require_samples=True):
    directory = Path(directory)
    path = directory / "episode.json"
    if not path.is_file():
        return None
    metadata = v3.read_json(path)
    if not v3.same_scenario(metadata, value, version):
        # A process may stop after C++ closed all episode files but before the
        # collector attached its dataset identity. Recover only that exact raw
        # terminal record in its own ID-named directory. Enriched records retain
        # strict identity checks, including the original seed.
        raw_identity = not any(key in metadata for key in ("id", "split", "region", "place_id", "route", "target"))
        matching_raw = (raw_identity and directory.name == value["id"] and v3.complete(directory)
                        and metadata.get("teacher_version") == version and metadata.get("demo_version") == 2
                        and all(metadata.get(key) == value.get(key) for key in ("start", "goal", "goal_kind"))
                        and not metadata.get("drive") and metadata.get("dagger") is None
                        and (value["goal_kind"] in ("point", "object") or metadata.get("seed") == value.get("seed")))
        if not matching_raw:
            raise ValueError(f"Existing recording belongs to another scenario: {directory}")
        metadata.update({key: value[key] for key in v3.SCENARIO_IDENTITY_FIELDS if key in value})
        if not v3.same_scenario(metadata, value, version):
            raise ValueError(f"Recovered recording belongs to another scenario: {directory}")
        metadata["campaign_identity_recovered"] = True
        v3.write_json(path, metadata)
    if require_samples:
        return metadata if v3.complete(directory) else None
    # Rejected new v2 samples are removed only AFTER this marker and the point
    # fraction are durable. Such attempts remain terminal on resume.
    if metadata.get("campaign_attempt_complete"):
        if value["goal_kind"] == "point" and "beyond_path_fraction" not in metadata:
            raise ValueError(f"Completed point attempt lacks cached fraction: {directory}")
        return metadata
    return metadata if v3.complete(directory) else None


def worker_args(options, version, index):
    task = options.task
    return SimpleNamespace(output=task / "workers" / f"v{version}-{index}",
                           runner=task / "build/opengothic/gothic-body-runner", assets=Path(os.environ.get("GOTHIC_DIR", Path.home() / "gothic-ai/game-settings")),
                           save=task / "input/save.sav", cache=task / "cache", links=task / "input/accepted-links.txt",
                           roads=task / "input/places.json", npc="BAU_954_MALETH", ini=None, seed=19102026,
                           physics="cv", watchdog_seconds=900, limit_ms=options.limit_ms,
                           contract=task / "build/opengothic/gothic-navmemoryteacher-contract")


class Worker:
    def __init__(self, args, version, budget):
        self.args, self.version, self.budget = args, version, budget
        self.runner = None

    def ready(self):
        if self.runner is None:
            # Cache construction publishes several files. Serializing initial
            # prepare prevents cold parallel engines from replacing each other's
            # half-built cache; subsequent scenarios still run concurrently.
            with _PREPARE_LOCK:
                if self.runner is None:
                    v3.checked_paths(self.args)
                    self.runner = v3.new_runner(self.args)
                    emit("worker_ready", teacher=self.version, worker=str(self.args.output))
        return self.runner

    def close(self):
        if self.runner is not None:
            self.runner.close()
            self.runner = None

    def scenario(self, name, commands, directory, seed=19102026):
        v3.runner_scenario(self.args, self.ready(), name, commands, directory, seed)

    def trial(self, value, directory):
        resumed = resume_metadata(directory, value, self.version)
        demos = v3.module("nav_demos")
        if resumed is None:
            with self.budget.reserve(self.args.limit_ms):
                started = time.monotonic()
                emit("trial_started", teacher=self.version, id=value["id"], kind=value["goal_kind"], region=value["region"])
                self.scenario(f"v{self.version}-{value['id']}",
                              [f"roads load {self.args.roads}", f"roads on {self.args.npc}",
                               v3.start_command(value, self.version, self.args.npc, self.args.limit_ms), "navdemos wait"],
                              directory, value["seed"])
                if not v3.complete(directory):
                    raise ValueError(f"Incomplete teacher recording: {directory}")
                metadata = v3.read_json(directory / "episode.json")
                if metadata.get("teacher_version") != self.version or metadata.get("demo_version") != 2:
                    raise ValueError(f"Wrong teacher/NAVDEMO version: {directory}")
                metadata.update(value, limit_ms=self.args.limit_ms, collector_wall_s=time.monotonic() - started)
                v3.write_json(directory / "episode.json", metadata)
                demos.summarize(directory, validate=True)
        else:
            metadata = resumed
            demos.summarize(directory, validate=True)
        if self.version == 3:
            contract_path = directory / "contract.json"
            contract = v3.read_json(contract_path) if contract_path.is_file() else {}
            if (contract.get("records") != metadata["decisions"] or contract.get("mismatches") != 0
                    or contract.get("previous_mismatches") != 0):
                v3.replay_contract(self.args.contract, directory)
        emit("trial_complete", teacher=self.version, id=value["id"], kind=value["goal_kind"],
             success=metadata["success"], decisions=metadata["decisions"], resumed=resumed is not None)
        return metadata


def phase_start(name):
    emit("phase_started", phase=name)
    subprocess.run(["df", "-h", str(Path.home())], check=True)


def run_parallel(values, workers, callback, deadline):
    """At most one submitted trial per persistent engine, with clean draining."""
    pending = iter(values)
    with ThreadPoolExecutor(max_workers=len(workers)) as executor:
        running = {}
        exhausted = False
        while running or not exhausted:
            idle = [worker for worker in workers if worker not in running.values()]
            for worker in idle:
                try:
                    value = next(pending)
                except StopIteration:
                    exhausted = True
                    break
                if time.monotonic() >= deadline:
                    raise TimeoutError("Campaign wall budget reached; resume the saved campaign")
                running[executor.submit(callback, worker, value)] = worker
            if running:
                done, _ = wait(running, return_when=FIRST_COMPLETED)
                for future in done:
                    running.pop(future)
                    future.result()


def nearest_regions(pool, training, positions):
    endpoints = sorted({(pair[field], pair["region"]) for pair in training for field in ("start", "goal")
                        if pair[field] in positions})
    if not endpoints:
        raise ValueError("No training endpoint coordinates for object region assignment")
    for value in pool:
        if value["goal_kind"] != "object":
            continue
        target = value["target_position"]
        name, region = min(endpoints, key=lambda pair: (math.hypot(positions[pair[0]][0]-target[0], positions[pair[0]][2]-target[2]), pair))
        value["region"] = region


def prepare_pool(options, worker, deadline):
    task = options.task
    source = task / "expanded-candidates.jsonl"
    points_file = task / "discovery/points/points-v3.jsonl"
    if not points_file.is_file():
        worker.scenario("export-points-v3", ["navdemos points-v3"], points_file.parent)
    positions = v3.load_start_positions(points_file)
    training = list(v3.rows(task / "input/training-pairs.jsonl"))
    regions = tuple(sorted({p["region"] for p in training}))
    prep = Counter()
    if source.is_file():
        pool = list(v3.rows(source))
    else:
        excluded = v3.exclusion_pairs(task / "input/holdout-cv.jsonl", task / "input/ppo-validation.jsonl")
        pool = v3.build_candidates(v3.read_json(task / "input/places.json")["places"], training, excluded, 19102026,
                                   starts_per_object=192, moving_per_region=3, point_count=2400,
                                   object_places=999, start_positions=positions)
        nearest_regions(pool, training, positions)
        # Expanded object sampling consumes RNG calls. Preserve the previously
        # frozen 18+18 moving scenarios byte-for-byte in identity and routes.
        moving_source = options.data / "scenarios.jsonl"
        if not moving_source.is_file():
            raise ValueError(f"Missing frozen moving selection: {moving_source}")
        moving = [p for p in v3.rows(moving_source) if p["goal_kind"] in ("follow", "approach")]
        moving_counts = Counter(p["goal_kind"] for p in moving)
        if moving_counts != {"follow": 18, "approach": 18}:
            raise ValueError(f"Expected frozen follow18 + approach18, got {dict(moving_counts)}")
        pool = [p for p in pool if p["goal_kind"] in ("point", "object")] + moving
        if any(not v3.isolated(value, excluded) for value in pool):
            raise ValueError("Excluded route in expanded selection")
        prepared = []
        for value in pool:
            if value["start"] not in positions:
                prep["unknown_start"] += 1
                continue
            if value["goal_kind"] == "object":
                if not value["goal"].startswith("cm:") and value["goal"] not in positions:
                    prep["unknown_object_goal"] += 1
                    continue
                if value.get("target_type") in ("house", "cave"):
                    inside, outside = value.get("entrance_in"), value.get("entrance_out")
                    if not inside or not outside or inside not in positions or outside not in positions:
                        prep["missing_entrance_alias"] += 1
                        continue
                    if math.hypot(positions[inside][0]-positions[outside][0], positions[inside][2]-positions[outside][2]) < .001:
                        prep["zero_entrance_normal"] += 1
                        continue
            prepared.append(value)
        pool = prepared
        v3.write_selection(source, pool)
        v3.write_json(task / "preparation-expanded.json", dict(rejections=dict(prep), points_exported=len(positions)))
    moving_source = options.data / "scenarios.jsonl"
    if moving_source.is_file():
        frozen_moving = [p for p in v3.rows(moving_source) if p["goal_kind"] in ("follow", "approach")]
        if [p for p in pool if p["goal_kind"] in ("follow", "approach")] != frozen_moving:
            raise ValueError("Expanded candidate file differs from the frozen moving selection")
    probes_file = task / "expanded-object-probes.jsonl"
    probes = {p.get("label", p.get("id")): p for p in v3.rows(probes_file)} if probes_file.is_file() else {}
    objects = [p for p in pool if p["goal_kind"] == "object" and p["id"] not in probes]
    # Restart only incomplete batches, never append repeated rows to the same
    # runner output. Completed batches are recovered before launching new work.
    for index in range(0, len(objects), 512):
        values = objects[index:index+512]
        label = v3.scenario_id("probe", *(p["id"] for p in values))
        directory = task / "discovery" / label
        output = directory / "object-candidates.jsonl"
        if output.is_file():
            found = list(v3.rows(output))
            ids = {p.get("label", p.get("id")) for p in found}
            if ids == {p["id"] for p in values}:
                probes.update((p.get("label", p.get("id")), p) for p in found)
                continue
        if time.monotonic() >= deadline:
            raise TimeoutError("Campaign wall budget reached during geometry discovery")
        query = task / "probe-queries" / f"{label}.txt"
        query.parent.mkdir(parents=True, exist_ok=True)
        query.write_text("".join(v3.probe_row(p)+"\n" for p in values), encoding="utf-8")
        # Remove only a partial NEW discovery output, never any recorded episode.
        if output.exists():
            output.unlink()
        worker.scenario(label, [f"navdemos probes-v3 {worker.args.npc} {query}"], directory)
        found = list(v3.rows(output))
        if {p.get("label", p.get("id")) for p in found} != {p["id"] for p in values}:
            raise ValueError(f"Probe batch incomplete: {label}")
        probes.update((p.get("label", p.get("id")), p) for p in found)
        with probes_file.open("w", encoding="utf-8") as stream:
            for key in sorted(probes):
                stream.write(json.dumps(probes[key], ensure_ascii=False)+"\n")
        emit("probes_complete", probes=len(probes), object_candidates=sum(p["goal_kind"] == "object" for p in pool))
    return pool, probes, regions


def reference_map(task, data):
    result = v3.references(data / "v2")
    for path in sorted((task / "v2-attempts").rglob("episode.json")):
        metadata = v3.read_json(path)
        if metadata.get("teacher_version") == 2 and metadata.get("campaign_attempt_complete"):
            result.setdefault(metadata["id"], path.parent)
    return result


def qualification(pool, probes, refs):
    return v3.qualified_candidates(pool, probes, refs)


def legacy_probe_stats(task, data):
    probes = {}
    for root in (task, data):
        for path in sorted(Path(root).rglob("object-candidates.jsonl")):
            if path.parent.name != "discovery":
                continue
            for value in v3.rows(path):
                probes[value.get("label", value.get("id"))] = value
    return dict(probes=len(probes), visible_centre=sum(p.get("visible_centre", p.get("visible")) is True for p in probes.values()),
                visible=sum(p.get("visible") is True for p in probes.values()),
                geometry_qualified=sum(v3.object_qualified(p) for p in probes.values()))


def save_qualification(task, counters, attempted, preparation=None, original=None, regions=(), object_regions=()):
    value = dict(counters, attempted=len(attempted), required_point_regions=list(regions),
                 geometry_object_regions=list(object_regions),
                 object_regions_without_geometry=sorted(set(regions) - set(object_regions)))
    value["object_rejection_breakdown"] = {
        key: counters.get("rejections", {}).get("object", {}).get(reason, 0)
        for key, reason in (("not_visible", "not_visible"), ("detour_short", "detour_too_short"),
                            ("unsupported", "unsupported_start"), ("incomplete", "no_detour"), ("v2_failed", "v2_failed"))}
    if preparation:
        value["preparation"] = preparation
    if original is not None:
        value["original_probe_baseline"] = original
    v3.write_json(task / "qualification-expanded.json", value)


def baseline(options, pool, probes, regions, workers, deadline):
    refs = reference_map(options.task, options.data)
    qualified, counters = qualification(pool, probes, refs)
    # Moving scenarios never need a privileged v2 baseline for qualification.
    qualified = [p for p in qualified if p["goal_kind"] in ("point", "object")]
    attempted = {p["id"] for p in pool if p["id"] in refs or (p["start"], p["goal"]) in refs}
    geometry_pool = [p for p in pool if p["goal_kind"] == "point"
                     or (p["goal_kind"] == "object" and v3.object_qualified(probes.get(p["id"])))]
    object_regions = tuple(sorted({p["region"] for p in geometry_pool if p["goal_kind"] == "object"}))
    schedule = PendingSchedule(geometry_pool, qualified, attempted, parallel=options.parallel,
                               min_points=options.min_points, min_objects=options.min_objects,
                               min_places=options.min_object_places, per_region=options.per_region,
                               regions=regions, object_regions=object_regions)
    preparation_file = options.task / "preparation-expanded.json"
    preparation = v3.read_json(preparation_file) if preparation_file.is_file() else None
    original = legacy_probe_stats(options.task, options.data)
    save_qualification(options.task, counters, attempted, preparation, original, regions, object_regions)

    def attempt(worker, value):
        directory = options.task / "v2-attempts" / value["goal_kind"] / value["split"] / value["id"]
        metadata = resume_metadata(directory, value, 2, require_samples=False)
        if metadata is None:
            metadata = worker.trial(value, directory)
        if value["goal_kind"] == "point" and "beyond_path_fraction" not in metadata:
            metadata["beyond_path_fraction"] = v3.beyond_path_fraction(directory)
        metadata["campaign_attempt_complete"] = True
        v3.write_json(directory / "episode.json", metadata)
        eligible = metadata.get("success") is True and (value["goal_kind"] != "point" or metadata["beyond_path_fraction"] > .3)
        if eligible:
            destination = v3.episode_directory(options.data, value, 2)
            if destination.exists():
                raise ValueError(f"Refusing to replace baseline episode: {destination}")
            destination.parent.mkdir(parents=True, exist_ok=True)
            with worker.budget.lock:
                shutil.move(str(directory), str(destination))
            directory = destination
        else:
            # Only this campaign's rejected v2 samples are disposable. Every
            # metadata/action/trajectory file and every v3 sample is preserved.
            sample = directory / "samples.bin.gz"
            with worker.budget.lock:
                if sample.is_file():
                    sample.unlink()
        return value, directory, eligible

    with ThreadPoolExecutor(max_workers=options.parallel) as executor:
        running = {}
        while True:
            for worker in [w for w in workers if w not in running.values()]:
                value = schedule.take()
                if value is None:
                    break
                if time.monotonic() >= deadline:
                    raise TimeoutError("Campaign wall budget reached during baseline qualification")
                running[executor.submit(attempt, worker, value)] = worker
            if not running:
                break
            done, _ = wait(running, return_when=FIRST_COMPLETED)
            for future in done:
                running.pop(future)
                value, directory, eligible = future.result()
                refs[value["id"]] = directory
                if eligible:
                    chosen, _ = qualification([value], probes, refs)
                    value = chosen[0]
                schedule.finish(value, eligible)
                # Re-evaluate the original pool with references only, never v3
                # results, so all exclusions and rejection counts remain honest.
                _, counters = qualification(pool, probes, refs)
                save_qualification(options.task, counters, schedule.attempted, preparation, original, regions, object_regions)
                emit("qualification_progress", qualified=counters["qualified"], point_regions=counters["point_regions"],
                     object_places=len(counters["object_places"]), attempted=len(schedule.attempted), inflight=len(running))
    selected, counters = qualification(pool, probes, refs)
    save_qualification(options.task, counters, schedule.attempted, preparation, original, regions, object_regions)
    gate(selected, regions, options.min_points, options.min_objects, options.min_object_places, options.per_region, object_regions)
    v3.write_selection(options.task / "expanded-scenarios.jsonl", selected)
    emit("selection_frozen", scenarios=len(selected), qualified=counters["qualified"], object_places=len(counters["object_places"]))
    return selected


def cohort_directory(data, value, cohort="v3"):
    if cohort not in ("v3", "v3-retry", "v3-visible"):
        raise ValueError(f"Unknown recording cohort: {cohort}")
    return Path(data) / cohort / value["goal_kind"] / value["split"] / value["id"]


def evaluate_selection(selected, data, metrics_fn=None, cohort="v3"):
    metrics_fn = metrics_fn or v3.episode_metrics
    episodes = []
    for value in selected:
        if value["goal_kind"] in ("point", "object"):
            reference = Path(value["teacher_v2_episode"])
            episodes.append(metrics_fn(reference))
        directory = cohort_directory(data, value, cohort)
        if v3.complete(directory) and (directory / "contract.json").is_file():
            if not v3.same_scenario(v3.read_json(directory / "episode.json"), value, 3):
                raise ValueError(f"Existing v3 recording belongs to another scenario: {directory}")
            episodes.append(metrics_fn(directory))
    groups, comparisons = {}, {}
    for kind in v3.KINDS:
        values = [e for e in episodes if e["goal_kind"] == kind]
        groups[kind] = {f"v{version}": v3.group_metrics([e for e in values if e["teacher_version"] == version]) for version in (2, 3)}
        comparisons[kind] = v3.comparison_metrics(values)
    checks = {f"comparison_{kind}": (comparisons[kind]["conditional_success_fraction"] or 0) >= .8 for kind in ("point", "object")}
    follow = groups["follow"]["v3"]
    checks["follow"] = ((follow["follow_fraction"] or 0) >= .8 and follow["follow_first_contacts"] == follow["episodes"] and follow["episodes"] > 0)
    checks["approach"] = (groups["approach"]["v3"]["approach_success_fraction"] or 0) >= .8
    checks.update({f"contract_{kind}": groups[kind]["v3"]["contract_records"] >= 1000 for kind in v3.KINDS})
    expected = Counter(p["goal_kind"] for p in selected)
    checks["all_selected_recorded"] = all(groups[k]["v3"]["episodes"] == expected[k] for k in v3.KINDS)
    return dict(by_kind=groups, comparisons=comparisons, checks=checks, all_passed=all(checks.values()),
                stop_required=point_stop(comparisons["point"]), episodes=episodes,
                selected=len(selected), expected_by_kind=dict(expected))


def retry_preflight(task, data):
    """The final retry uses exactly the already frozen cohort and baseline."""
    source = Path(task) / "expanded-scenarios.jsonl"
    selected = list(v3.rows(source))
    expected = dict(point=30, object=30, follow=18, approach=18)
    counts = dict(Counter(value["goal_kind"] for value in selected))
    if counts != expected or len({value["id"] for value in selected}) != len(selected):
        raise ValueError(f"Retry requires the exact frozen 30/30/18/18 cohort: {counts}")
    v3.validate_splits(selected)
    original_successes = 0
    for value in selected:
        if value["goal_kind"] in ("point", "object"):
            baseline = v3.episode_directory(data, value, 2)
            if Path(value["teacher_v2_episode"]).resolve() != baseline.resolve() or not v3.complete(baseline):
                raise ValueError(f"Retry baseline is not the preserved DATA/v2 recording: {value['id']}")
            metadata = v3.read_json(baseline / "episode.json")
            if not v3.same_scenario(metadata, value, 2) or metadata.get("success") is not True:
                raise ValueError(f"Retry requires a matching successful v2 baseline: {value['id']}")
        if value["goal_kind"] == "point":
            original = cohort_directory(data, value)
            if not v3.complete(original):
                raise ValueError(f"Original point recording is missing: {value['id']}")
            metadata = v3.read_json(original / "episode.json")
            if not v3.same_scenario(metadata, value, 3):
                raise ValueError(f"Original point identity changed: {value['id']}")
            original_successes += metadata.get("success") is True
    if original_successes != 8:
        raise ValueError(f"Original stop cohort must remain 8/30, found {original_successes}/30")
    return selected


def retry_point_gate(result, selected):
    expected = {value["id"] for value in selected if value["goal_kind"] == "point"}
    measured = [episode for episode in result["episodes"] if episode["goal_kind"] == "point"]
    reference = [episode for episode in measured if episode["teacher_version"] == 2]
    trials = [episode for episode in measured if episode["teacher_version"] == 3]
    if (len(expected) != 30 or len(reference) != 30 or len(trials) != 30
            or {episode["id"] for episode in reference} != expected or {episode["id"] for episode in trials} != expected
            or not all(episode["success"] for episode in reference)):
        raise ValueError("Retry point gate needs all 30 exact point IDs and 30 successful v2 baselines")
    successes = sum(bool(episode["success"]) for episode in trials)
    return dict(completed=30, baseline_successes=30, successes=successes, conditional_success_fraction=successes / 30,
                minimum_successes=15, passed=successes >= 15)


def binary_digest(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def retry_settings(options):
    return dict(selection_sha256=binary_digest(options.task / "expanded-scenarios.jsonl"),
                runner_sha256=binary_digest(options.task / "build/opengothic/gothic-body-runner"),
                contract_sha256=binary_digest(options.task / "build/opengothic/gothic-navmemoryteacher-contract"),
                diagnostics_sha256=binary_digest(options.task / "build/opengothic/gothic-navmemoryteacher-diagnostics"),
                limit_ms=options.limit_ms, cohort="v3-retry", mechanics_attempt=2)


def retry_diagnostic_file(binary, source, indices, output, counterfactual=False):
    """Strict new replay and explicitly counterfactual old observations stay separate."""
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    index_path = output.with_suffix(".indices.txt")
    content = "".join(f"{index}\n" for index in sorted(set(indices)))
    if index_path.exists() and index_path.read_text(encoding="utf-8") != content:
        raise ValueError(f"Retry diagnostic index selection changed: {index_path}")
    index_path.write_text(content, encoding="utf-8")
    if not output.is_file():
        command = [str(binary), str(source), "--indices", str(index_path)]
        if counterfactual:
            command.append("--allow-mismatch")
        temporary = output.with_suffix(".partial")
        with temporary.open("w", encoding="utf-8") as stream:
            process = subprocess.run(command, stdout=stream, stderr=subprocess.PIPE, text=True, check=False)
        if process.returncode:
            raise ValueError(f"Retry diagnostics failed: {source}: {process.stderr[-2000:]}")
        temporary.replace(output)
    summaries, found = [], []
    for row in v3.rows(output):
        if row.get("type") == "record":
            found.append(row["index"])
        elif row.get("type") == "summary":
            summaries.append(row)
    if len(summaries) != 1 or sorted(found) != sorted(set(indices)):
        raise ValueError(f"Incomplete retry diagnostics: {output}")
    summary = summaries[0]
    required = ("steering_start_found_false", "steering_start_evaluable_records", "steering_escape_required_records")
    if (any(not isinstance(summary.get(key), int) or summary[key] < 0 for key in required)
            or summary["steering_start_evaluable_records"] != summary.get("records")
            or summary["steering_start_found_false"] > summary["records"]
            or bool(summary.get("counterfactual", False)) != counterfactual):
        raise ValueError(f"Invalid retry all-decision start counters: {output}: {summary}")
    if not counterfactual and (summary.get("mismatches") != 0 or summary.get("previous_mismatches") != 0):
        raise ValueError(f"Strict retry recording does not replay: {output}")
    selected = []
    for row in v3.rows(output):
        if row.get("type") != "record":
            continue
        for target_kind in ("steering", "goal"):
            production = [value for value in row["diagnostics"] if value.get("target_kind") == target_kind
                          and value.get("name") == f"production-{target_kind}"]
            if len(production) != 1:
                raise ValueError(f"Missing retry 15-cm {target_kind} diagnostic: {output}, index={row['index']}")
            selected.append(dict(index=row["index"], target_kind=target_kind, start_found=production[0]["start_found"],
                                 actual_target_reached=production[0]["actual_target_reached"]))
    summary = dict(summary, selected_frames=len(found), selected_production=selected, output=str(output))
    return summary


def retry_diagnostics(options, selected, deadline):
    binary = options.task / "build/opengothic/gothic-navmemoryteacher-diagnostics"
    jobs = []
    for value in selected:
        if value["goal_kind"] != "point":
            continue
        directory = cohort_directory(options.data, value, "v3-retry")
        metadata = v3.read_json(directory / "episode.json")
        indices = list(range(metadata["decisions"] - 1, max(-1, metadata["decisions"] - 102), -10))
        jobs.append(dict(id=value["id"], cohort="new", source=directory / "samples.bin.gz", indices=indices,
                         decisions=metadata["decisions"], counterfactual=False))
    original = options.task / "failures-v3-first/extraction.json"
    if not original.is_file():
        raise ValueError("Paired first-take diagnostic indices are missing")
    extraction = v3.read_json(original)
    for case in extraction["cases"]:
        source = Path(case["source_samples"])
        # Extraction case.json omits split: bind to the frozen point ID instead.
        value = next((value for value in selected if value["id"] == case["id"] and value["goal_kind"] == "point"), None)
        if value is None or source.resolve() != (cohort_directory(options.data, value) / "samples.bin.gz").resolve():
            raise ValueError(f"Counterfactual source is not an original frozen point: {case['id']}")
        jobs.append(dict(id=case["id"], cohort="original-counterfactual", source=source,
                         indices=case["diagnostic_indices"], decisions=case["decisions"], counterfactual=True))
    if len(extraction["cases"]) != 22:
        raise ValueError(f"Paired diagnostic cohort must include all 22 original failures, found {len(extraction['cases'])}")

    def diagnose(job):
        if time.monotonic() >= deadline:
            raise TimeoutError("Retry diagnostics exceeded the job wall budget; resume this same attempt")
        result = retry_diagnostic_file(binary, job["source"], job["indices"],
                                       options.task / "retry-diagnostics" / job["cohort"] / f"{job['id']}.jsonl",
                                       job["counterfactual"])
        if result["records"] != job["decisions"]:
            raise ValueError(f"Retry diagnostic decision count differs: {job['id']}")
        return dict(job, source=str(job["source"]), summary=result)

    with ThreadPoolExecutor(max_workers=options.parallel) as executor:
        outcomes = list(executor.map(diagnose, jobs))
    groups = {}
    for cohort in ("new", "original-counterfactual"):
        values = [value for value in outcomes if value["cohort"] == cohort]
        summaries = [value["summary"] for value in values]
        counts = {key: sum(value[key] for value in summaries) for key in (
            "records", "steering_start_found_false", "steering_start_evaluable_records", "steering_escape_required_records", "selected_frames")}
        counts["episodes"] = len(values)
        counts["start_not_found_fraction"] = counts["steering_start_found_false"] / counts["steering_start_evaluable_records"] if counts["steering_start_evaluable_records"] else None
        counts["selected_start_not_found"] = {kind: sum(not frame["start_found"] for summary in summaries
            for frame in summary["selected_production"] if frame["target_kind"] == kind) for kind in ("steering", "goal")}
        groups[cohort] = counts
    old_frames = old_start_false = 0
    for case in extraction["cases"]:
        path = options.task / "failures-v3-first" / case["id"] / "diagnostics.jsonl"
        found = []
        for row in v3.rows(path):
            if row.get("type") != "record":
                continue
            baseline = [value for value in row["diagnostics"] if value.get("name") == "r30-u4"
                        and value.get("target_kind", "steering") == "steering"]
            if len(baseline) != 1:
                raise ValueError(f"Missing original production diagnostic: {path}, index={row['index']}")
            found.append(row["index"])
            old_start_false += not baseline[0]["start_found"]
        if sorted(found) != sorted(case["diagnostic_indices"]):
            raise ValueError(f"Original diagnostic frame indices changed: {path}")
        old_frames += len(found)
    paired = groups["original-counterfactual"]
    if old_frames != paired["selected_frames"] or old_frames != 350:
        raise ValueError(f"Paired old/new diagnostic cohort must preserve the same 350 frames: {old_frames}/{paired['selected_frames']}")
    explicit = dict(new_all_decisions=dict(count=groups["new"]["steering_start_found_false"],
                    denominator=groups["new"]["steering_start_evaluable_records"], episodes=groups["new"]["episodes"]),
                    new_tail_frames=dict(count=groups["new"]["selected_start_not_found"]["steering"],
                        denominator=groups["new"]["selected_frames"], goal_count=groups["new"]["selected_start_not_found"]["goal"]),
                    original_same_350_frames=dict(old_count=old_start_false,
                        new_count=paired["selected_start_not_found"]["steering"], denominator=old_frames,
                        goal_new_count=paired["selected_start_not_found"]["goal"]))
    return dict(start_not_found=explicit, by_cohort=groups, episodes=outcomes,
                interpretation="New all-decision counters are strict replays. Old inputs use the revised planner counterfactually; expected old-key mismatches are not a new-recording contract failure.")


def retry(options):
    selected = retry_preflight(options.task, options.data)
    settings = retry_settings(options)
    output = options.task / "retry-metrics.json"
    if output.is_file():
        previous = v3.read_json(output)
        if previous.get("retry_settings") != settings:
            raise ValueError("The final mechanics retry already belongs to another binary or frozen cohort")
    else:
        v3.write_json(output, dict(retry_settings=settings, status="recording_points"))
    deadline = time.monotonic() + options.budget_seconds
    budget = StorageBudget(options.task, options.data)
    workers = []
    for index in range(options.parallel):
        args = worker_args(options, 3, index)
        args.output = options.task / "workers" / f"v3-retry-{index}"
        workers.append(Worker(args, 3, budget))
    metric_cache = {}

    def measured(directory):
        directory = Path(directory)
        if directory not in metric_cache:
            metric_cache[directory] = v3.episode_metrics(directory)
        return metric_cache[directory]

    diagnostics = None
    try:
        for kind in v3.KINDS:
            phase_start(f"v3-retry-{kind}")
            values = [value for value in selected if value["goal_kind"] == kind]
            emit("retry_started", kind=kind, scenarios=len(values), mechanics_attempt=2)
            run_parallel(values, workers, lambda worker, value: worker.trial(value, cohort_directory(options.data, value, "v3-retry")), deadline)
            result = evaluate_selection(selected, options.data, metrics_fn=measured, cohort="v3-retry")
            result.update(retry_settings=settings, status=f"recorded_{kind}", recording_cohort="v3-retry")
            if kind == "point":
                result["retry_point_gate"] = retry_point_gate(result, selected)
                result["stop_required"] = not result["retry_point_gate"]["passed"]
                v3.write_json(output, result)
                diagnostics = retry_diagnostics(options, selected, deadline)
                result["start_diagnostics"] = diagnostics
                v3.write_json(output, result)
                if result["stop_required"]:
                    emit("retry_stop_required", **result["retry_point_gate"])
                    return result
            else:
                result["retry_point_gate"] = retry_point_gate(result, selected)
                result["start_diagnostics"] = diagnostics
                v3.write_json(output, result)
        result["status"] = "complete"
        v3.write_json(output, result)
    finally:
        for worker in workers:
            worker.close()
    emit("retry_complete", point_gate=result["retry_point_gate"], all_passed=result["all_passed"])
    return result


def visible_preflight(task, data):
    """Independent visible-target authorization does not reopen point mechanics."""
    selected = list(v3.rows(Path(task) / "expanded-scenarios.jsonl"))
    counts = dict(Counter(value["goal_kind"] for value in selected))
    if counts != dict(point=30, object=30, follow=18, approach=18) or len({value["id"] for value in selected}) != 96:
        raise ValueError(f"Visible stage requires the exact frozen 30/30/18/18 cohort: {counts}")
    v3.validate_splits(selected)
    point_successes = 0
    for value in selected:
        if value["goal_kind"] == "object":
            baseline = v3.episode_directory(data, value, 2)
            if Path(value["teacher_v2_episode"]).resolve() != baseline.resolve() or not v3.complete(baseline):
                raise ValueError(f"Visible object baseline is not preserved DATA/v2: {value['id']}")
            metadata = v3.read_json(baseline / "episode.json")
            if not v3.same_scenario(metadata, value, 2) or metadata.get("success") is not True:
                raise ValueError(f"Visible stage requires a matching successful object v2 baseline: {value['id']}")
        elif value["goal_kind"] == "point":
            directory = cohort_directory(data, value, "v3-retry")
            if not v3.complete(directory) or not (directory / "contract.json").is_file():
                raise ValueError(f"Closed point retry recording is missing: {value['id']}")
            metadata = v3.read_json(directory / "episode.json")
            if not v3.same_scenario(metadata, value, 3):
                raise ValueError(f"Closed point retry identity changed: {value['id']}")
            point_successes += metadata.get("success") is True
    if point_successes != 10:
        raise ValueError(f"Closed retry cohort must remain 10/30, found {point_successes}/30")
    return selected, dict(successes=10, attempted=30, stop_required=True, mechanics_closed=True)


def visible_settings(options):
    settings = dict(selection_sha256=binary_digest(options.task / "expanded-scenarios.jsonl"),
                    runner_sha256=binary_digest(options.task / "build/opengothic/gothic-body-runner"),
                    contract_sha256=binary_digest(options.task / "build/opengothic/gothic-navmemoryteacher-contract"),
                    limit_ms=options.limit_ms, cohort="v3-visible")
    previous = v3.read_json(options.task / "retry-metrics.json").get("retry_settings", {})
    if any(settings[key] != previous.get(key) for key in ("selection_sha256", "runner_sha256", "contract_sha256")):
        raise ValueError("Visible targets must use the unchanged frozen selection and revised v3 binaries")
    return settings


def strict_example_contract(binary, directory, decisions):
    """Replay read-only: never rewrite the preserved successful point evidence."""
    result = subprocess.run([str(binary), str(Path(directory) / "samples.bin.gz")],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False)
    if result.returncode:
        raise ValueError(f"Successful point example does not replay: {directory}: {result.stderr[-2000:]}")
    contract = json.loads(result.stdout)
    if contract.get("records") != decisions or contract.get("mismatches") != 0 or contract.get("previous_mismatches") != 0:
        raise ValueError(f"Successful point example contract is incomplete: {directory}: {contract}")
    return contract


def point_examples(selected, data, contract_binary, metrics_fn=None):
    metrics_fn = metrics_fn or v3.episode_metrics
    root = Path(data) / "examples/point"
    successful = []
    for value in selected:
        if value["goal_kind"] != "point":
            continue
        directory = cohort_directory(data, value, "v3-retry")
        metadata = v3.read_json(directory / "episode.json")
        if metadata.get("success") is True:
            successful.append((value, directory))
    if len(successful) != 10:
        raise ValueError(f"Point examples must contain exactly 10 successful revised episodes, found {len(successful)}")
    expected = {root / value["split"] / value["id"] for value, _ in successful}
    for existing in root.glob("*/*"):
        if existing not in expected or existing.is_symlink() or not existing.is_dir():
            raise ValueError(f"Unexpected entry in successful point examples: {existing}")
    metrics, destinations = [], []
    names = ("episode.json", "samples.bin.gz", "actions.txt", "trajectory.txt", "contract.json", "teacher-paths.jsonl")
    for value, directory in successful:
        metadata = v3.read_json(directory / "episode.json")
        if not v3.same_scenario(metadata, value, 3) or not v3.complete(directory):
            raise ValueError(f"Successful point example identity is invalid: {directory}")
        measured = metrics_fn(directory)  # NAVDEMO check and saved strict contract.
        if measured.get("success") is not True or measured.get("contract_records") != measured.get("decisions"):
            raise ValueError(f"Successful point example validation is incomplete: {directory}")
        strict_example_contract(contract_binary, directory, measured["decisions"])
        destination = root / value["split"] / value["id"]
        destination.mkdir(parents=True, exist_ok=True)
        if any(path.name not in names for path in destination.iterdir()):
            raise ValueError(f"Unexpected files in point example: {destination}")
        for name in names:
            source = (directory / name).resolve()
            target = destination / name
            if not source.is_file():
                raise ValueError(f"Successful point example lacks {name}: {directory}")
            if target.exists() or target.is_symlink():
                if not target.is_symlink() or target.resolve() != source:
                    raise ValueError(f"Point example link points outside its successful revised recording: {target}")
            else:
                target.symlink_to(source)
        metrics.append(measured)
        destinations.append(str(destination))
    return dict(v3.group_metrics(metrics), paths=destinations, source_cohort="v3-retry", examples_root=str(root),
                selection="Only the 10 successful revised points; all failed raw recordings remain preserved as diagnostic evidence")


def evaluate_visible_selection(selected, data, metrics_fn=None):
    if any(value["goal_kind"] == "point" for value in selected):
        raise ValueError("Visible evaluation must not include point scenarios")
    result = evaluate_selection(selected, data, metrics_fn, cohort="v3-visible")
    result["checks"].pop("comparison_point")
    result["checks"].pop("contract_point")
    result["all_passed"] = all(result["checks"].values())
    result["stop_required"] = False
    return result


def visible(options):
    frozen, point_stop_state = visible_preflight(options.task, options.data)
    settings = visible_settings(options)
    output = options.task / "visible-metrics.json"
    if output.is_file() and v3.read_json(output).get("visible_settings") != settings:
        raise ValueError("Visible target recordings belong to another binary or frozen cohort")
    deadline = time.monotonic() + options.budget_seconds
    budget = StorageBudget(options.task, options.data)
    examples = point_examples(frozen, options.data, options.task / "build/opengothic/gothic-navmemoryteacher-contract")
    selected = [value for value in frozen if value["goal_kind"] != "point"]
    shared = dict(visible_settings=settings, recording_cohort="v3-visible", point_stop=point_stop_state,
                  independent_visible_target_authorization=True, point_examples=examples)
    v3.write_json(output, dict(shared, status="recording_visible_targets"))
    workers = []
    for index in range(options.parallel):
        args = worker_args(options, 3, index)
        args.output = options.task / "workers" / f"v3-visible-{index}"
        workers.append(Worker(args, 3, budget))
    metric_cache = {}

    def measured(directory):
        directory = Path(directory)
        if directory not in metric_cache:
            metric_cache[directory] = v3.episode_metrics(directory)
        return metric_cache[directory]

    try:
        for kind in ("object", "follow", "approach"):
            phase_start(f"v3-visible-{kind}")
            values = [value for value in selected if value["goal_kind"] == kind]
            emit("visible_started", kind=kind, scenarios=len(values), point_rollouts=0)
            run_parallel(values, workers, lambda worker, value: worker.trial(value, cohort_directory(options.data, value, "v3-visible")), deadline)
            result = evaluate_visible_selection(selected, options.data, metrics_fn=measured)
            result.update(shared, status=f"recorded_{kind}")
            v3.write_json(output, result)
        result["status"] = "complete"
        v3.write_json(output, result)
    finally:
        for worker in workers:
            worker.close()
    emit("visible_complete", **{key: value for key, value in result.items() if key not in ("episodes", "point_examples")})
    return result


def campaign(options):
    if options.stage == "retry":
        return retry(options)
    if options.stage == "visible":
        return visible(options)
    deadline = time.monotonic() + options.budget_seconds
    budget = StorageBudget(options.task, options.data)
    selection = options.task / "expanded-scenarios.jsonl"
    setup_fraction_cache(options.task)
    if options.stage == "evaluate" and not selection.is_file():
        raise ValueError("--stage evaluate requires the frozen expanded-scenarios.jsonl")
    if options.stage == "analyze":
        selected = list(v3.rows(selection))
        result = evaluate_selection(selected, options.data)
        v3.write_json(options.task / "campaign-metrics.json", result)
        emit("evaluation_complete", **{k: v for k, v in result.items() if k != "episodes"})
        return result
    workers = [Worker(worker_args(options, 2, i), 2, budget) for i in range(options.parallel)]
    try:
        phase_start("qualification")
        if selection.is_file():
            selected = list(v3.rows(selection))
            regions = sorted({p["region"] for p in v3.rows(options.task / "input/training-pairs.jsonl")})
            stats_path = options.task / "qualification-expanded.json"
            required_objects = v3.read_json(stats_path).get("geometry_object_regions", []) if stats_path.is_file() else []
            gate(selected, regions, options.min_points, options.min_objects, options.min_object_places, options.per_region, required_objects)
            v3.validate_splits(selected)
        else:
            pool, probes, regions = prepare_pool(options, workers[0], deadline)
            emit("ready", candidates=len(pool), geometry_objects=sum(v3.object_qualified(p) for p in probes.values()), regions=regions)
            selected = baseline(options, pool, probes, regions, workers, deadline)
    finally:
        for worker in workers:
            worker.close()
    workers = [Worker(worker_args(options, 3, i), 3, budget) for i in range(options.parallel)]
    try:
        for kind in v3.KINDS:
            phase_start(f"v3-{kind}")
            values = [p for p in selected if p["goal_kind"] == kind]
            emit("evaluate_started", kind=kind, scenarios=len(values), parallel=options.parallel)
            run_parallel(values, workers, lambda worker, value: worker.trial(value, v3.episode_directory(options.data, value, 3)), deadline)
            result = evaluate_selection(selected, options.data)
            v3.write_json(options.task / "campaign-metrics.json", result)
            if kind == "point" and result["stop_required"]:
                emit("stop_required", **result["comparisons"]["point"])
                return result
    finally:
        for worker in workers:
            worker.close()
    emit("evaluation_complete", **{k: v for k, v in result.items() if k != "episodes"})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--task", type=Path, required=True)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--parallel", type=int, default=3)
    parser.add_argument("--min-points", type=int, default=30)
    parser.add_argument("--min-objects", type=int, default=30)
    parser.add_argument("--min-object-places", type=int, default=30)
    parser.add_argument("--per-region", type=int, default=5)
    parser.add_argument("--stage", choices=("campaign", "evaluate", "analyze", "retry", "visible"), default="campaign")
    parser.add_argument("--limit-ms", type=int, default=240000)
    parser.add_argument("--budget-seconds", type=float, default=6900)
    options = parser.parse_args()
    if not options.task.is_absolute() or not options.data.is_absolute():
        parser.error("--task and --data must be absolute")
    if not 1 <= options.parallel <= 3 or min(options.min_points, options.min_objects, options.min_object_places, options.per_region, options.limit_ms, options.budget_seconds) <= 0:
        parser.error("Positive bounds and --parallel between 1 and 3 are required")
    options.task, options.data = options.task.resolve(), options.data.resolve()
    try:
        result = campaign(options)
    except Exception as exc:
        error_file = f"{options.stage}-error.json" if options.stage in ("retry", "visible") else "campaign-error.json"
        v3.write_json(options.task / error_file, dict(error=type(exc).__name__, detail=str(exc)))
        emit("campaign_failed", error=type(exc).__name__, detail=str(exc))
        raise
    return 1 if result["stop_required"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
