#!/usr/bin/env python3
"""Bounded memory-v3 training demonstrations; run inside a caller-owned gq batch.

Only train splits are recorded. Every completed attempt remains on disk; failures
are retained with training_eligible=false. Resuming never retries a failed or
partial episode. This command does not train, submit jobs, or write reports.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from functools import lru_cache
import importlib.util
import math
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import threading
import time
from types import SimpleNamespace


def module(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + ".py"))
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


campaign = module("nav_teacher_v3_campaign")
v3 = campaign.v3
jumps = module("nav_jump_record")
demos = module("nav_demos")
SEED = 19102026
MEMORY_VERSION = 3
V5_MAX_BYTES = 30_000_000_000
V5_MIN_FREE_BYTES = 20 * 1024**3
FAMILIES = ("point", "object", "approach", "follow", "jump")
FOLLOW_BELOW_GATE = "follow_below_gate"
FOLLOW_BELOW_GATE_MIN_FRACTION = .7
MOVING_SPLITS = dict(harbour="train", market="train", port_streets="train", upper_streets="train",
                     onar="validation", lobart="test")
INPUTS = ("training-pairs.jsonl", "ppo-validation.jsonl", "holdout-cv.jsonl", "places.json",
          "accepted-links.txt", "object-candidates.jsonl", "object-probes.jsonl", "moving-scenarios.jsonl",
          "jump-routes.jsonl", "jump-links.txt", "save.sav")
LIVE_SUITE = Path(__file__).with_name("nav-live") / "scenarios.json"


def atomic_json(path, value):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + ".tmp")
    v3.write_json(temporary, value)
    temporary.replace(path)


class CollectionState:
    """Keep sample hashes and status outside immutable recording directories."""
    def __init__(self, path, settings, previous=None, archive_root=None):
        self.path, self.lock = Path(path), threading.Lock()
        self.archive_root = Path(archive_root) / "recordings" if archive_root is not None else None
        self.data = dict(previous if previous is not None else
                         v3.read_json(self.path) if self.path.is_file() else {})
        self.data["settings"] = settings
        integrity = self.data.setdefault("integrity", dict(schema_version=1, episodes={}))
        if (integrity.get("schema_version") != 1 or not isinstance(integrity.get("episodes"), dict)):
            raise ValueError("Unsupported recording integrity cache")
        if not isinstance(self.data.setdefault("rejected_candidates", {}), dict):
            raise ValueError("Unsupported rejected candidate registry")
        summary = self.path.with_name("collection-metrics.json")
        self.saved = {self.key(value["episode"]): value for value in
                      (v3.read_json(summary).get("episodes", []) if summary.is_file() else [])}

    def key(self, directory):
        directory = Path(directory)
        for root in (self.path.parent, self.archive_root):
            if root is not None and directory.is_relative_to(root):
                return str(directory.relative_to(root))
        raise ValueError(f"Recording is outside the collection roots: {directory}")

    def saved_metrics(self, directory):
        with self.lock:
            return self.saved.get(self.key(directory))

    def verify_samples(self, directory, metadata, *, allow_unpinned=False):
        digest = campaign.binary_digest(Path(directory) / "samples.bin.gz")
        with self.lock:
            pin = self.data["integrity"]["episodes"].get(self.key(directory), {})
            saved = self.saved.get(self.key(directory), {})
            summaries = (metadata, metadata.get("collection_metrics", {}), saved, pin)
            recorded = [value["samples_sha256"] for value in summaries if value.get("samples_sha256")]
        if not recorded and not allow_unpinned:
            raise ValueError(f"Completed recording lacks its recorded samples hash: {directory}")
        if any(value != digest for value in recorded):
            raise ValueError(f"Recorded samples hash differs: {directory}")
        return digest

    def pin_samples(self, directory, digest):
        with self.lock:
            episodes = self.data["integrity"]["episodes"]
            prior = episodes.get(self.key(directory), {})
            if prior.get("samples_sha256", digest) != digest:
                raise ValueError(f"Recorded samples hash differs: {directory}")
            episodes[self.key(directory)] = dict(samples_sha256=digest)
            atomic_json(self.path, self.data)

    def remember_metrics(self, directory, measured):
        with self.lock:
            self.saved[self.key(directory)] = measured

    def rejections(self):
        with self.lock:
            return [dict(value) for _, value in sorted(self.data["rejected_candidates"].items())]

    def rejected_candidate(self, directory):
        with self.lock:
            value = self.data["rejected_candidates"].get(self.key(directory))
            return dict(value) if value is not None else None

    def reject_candidate(self, directory, value, reason):
        """Persist a native setup refusal outside its untouched log directory.

        A caller recovering an already failed phase may use the same method
        after verifying its saved runner refusal. Live workers additionally
        require a surviving process and a clean close before calling this.
        """
        if setup_rejection_reason(RuntimeError(reason), value) is None:
            raise ValueError(f"Not a recognized candidate setup refusal: {reason}")
        require_no_recording_output(directory)
        result = dict(candidate_rejected=True, id=value["id"], family=family(value),
            reason=reason, scenario=dict(value), training_eligible=False, recorded=False,
            directory=str(directory), teacher_version=version(value), navigation_memory_version=MEMORY_VERSION)
        base = self.data["settings"].get("base_recording_identity", self.data["settings"])
        result["navigation_memory_version"] = base.get("navigation_memory_version", MEMORY_VERSION)
        with self.lock:
            rejected = self.data["rejected_candidates"]
            key = self.key(directory)
            if key in rejected and rejected[key] != result:
                raise ValueError(f"Rejected candidate changed: {directory}")
            rejected[key] = result
            atomic_json(self.path, self.data)
        return result

    def update(self, **values):
        with self.lock:
            self.data.update(values)
            atomic_json(self.path, self.data)


def recorded_parameters(metadata):
    fields = (*v3.SCENARIO_IDENTITY_FIELDS, "index", "site_id", "transition_id", "type",
              "dataset_family", "follow_below_gate", "minimum_follow_fraction", "command_seed",
              "limit_ms", "teacher_version", "navigation_memory_version", "demo_version",
              "npc", "local", "residency", "knownroads_selection", "water_qualification")
    return {key: metadata[key] for key in fields if key in metadata}


def balanced(values, key, seed=SEED):
    groups = defaultdict(list)
    for value in sorted(values, key=lambda value: value["id"]):
        groups[value[key]].append(value)
    rng = random.Random(seed)
    for group in groups.values():
        rng.shuffle(group)
    result = []
    while any(groups.values()):
        for name in sorted(groups):
            if groups[name]:
                result.append(groups[name].pop())
    return result


def select_points(training, excluded, count=400):
    regions = sorted({value["region"] for value in training})
    if len(regions) != 6:
        raise ValueError(f"Road collection requires the original six regions: {regions}")
    pool = {}
    for pair in training:
        value = dict(goal_kind="point", split="train", region=pair["region"], start=pair["start"],
                     goal=pair["goal"], seed=SEED, id=v3.scenario_id("point", pair["start"], pair["goal"]),
                     source_pair_index=pair.get("index"))
        if v3.isolated(value, excluded):
            prior = pool.setdefault(value["id"], value)
            if prior["region"] != value["region"]:
                raise ValueError("One training pair belongs to multiple regions")
    selected = balanced(pool.values(), "region")[:count]
    counts = Counter(value["region"] for value in selected)
    if len(selected) != count or set(counts) != set(regions) or max(counts.values()) - min(counts.values()) > 1:
        raise ValueError(f"Not enough isolated, balanced road pairs: {dict(counts)}")
    return selected


def point_schedule(values, done, limit=400, rejected=None):
    """Reduce future road work without changing the frozen candidate pool."""
    rejected = rejected or set()
    available = [value for value in values if ("point", value["id"]) not in rejected]
    if limit == 400:
        return available
    if not isinstance(limit, int) or limit <= 0 or limit >= 400 or limit % 6:
        raise ValueError("A reduced point limit must be a positive multiple of six below 400")
    regions = sorted({value["region"] for value in values})
    if len(regions) != 6:
        raise ValueError("Reduced road collection requires the original six regions")
    quota = limit // 6
    recorded = [value for value in available if ("point", value["id"]) in done]
    counts = Counter(value["region"] for value in recorded)
    pool = Counter(value["region"] for value in values)
    if any(counts[region] > quota or pool[region] < quota for region in regions):
        raise ValueError(f"Point limit cannot discard completed candidates or exceed a region pool: {dict(counts)}")
    selected = {value["id"] for value in recorded}
    for value in available:
        if value["id"] not in selected and counts[value["region"]] < quota:
            selected.add(value["id"])
            counts[value["region"]] += 1
    return [value for value in values if value["id"] in selected]


def select_objects(pool, probes, excluded):
    v3.validate_splits(pool)
    by_id = {}
    for probe in probes:
        identifier = probe.get("label", probe.get("id"))
        if identifier in by_id and by_id[identifier] != probe:
            raise ValueError(f"Conflicting object probes: {identifier}")
        by_id[identifier] = probe
    selected = []
    for original in pool:
        if original.get("goal_kind") != "object" or original.get("split") != "train":
            continue
        probe = by_id.get(original["id"])
        if not v3.object_qualified(probe) or v3.object_visibility(probe) == "not_visible":
            continue
        if (probe.get("start"), probe.get("goal")) != (original["start"], original["goal"]):
            raise ValueError(f"Object probe route differs: {original['id']}")
        if v3.isolated(original, excluded):
            value = dict(original, qualification=probe, visibility=v3.object_visibility(probe))
            selected.append(value)
    if len({value["id"] for value in selected}) != len(selected):
        raise ValueError("Duplicate object candidate IDs")
    if len(selected) < 100:
        raise ValueError(f"Only {len(selected)} qualified train objects; need at least 100")
    return balanced(selected, "place_id")


def select_moving(places, training, excluded, frozen, per_region=100):
    old = [value for value in frozen if value.get("goal_kind") in ("follow", "approach")]
    v3.validate_splits(old)
    for value in old:
        if MOVING_SPLITS.get(value["region"]) != value["split"]:
            raise ValueError(f"Frozen moving split changed: {value['id']}")
    generated = v3.build_candidates(places, training, excluded, SEED, point_count=0, object_places=0,
                                    moving_per_region=per_region)
    pool = {value["id"]: value for value in old}
    for value in generated:
        if value["goal_kind"] not in ("follow", "approach"):
            continue
        value["split"] = MOVING_SPLITS[value["region"]]
        if value["id"] in pool:
            # Older IDs describe the route, excluding count-dependent seed and
            # target choice. A larger pool can generate that same route with
            # another seed/creature; keep the exact frozen trial instead.
            core = ("id", "goal_kind", "split", "region", "start", "goal", "route")
            if any(value.get(key) != pool[value["id"]].get(key) for key in core):
                raise ValueError(f"Generated route changed frozen moving membership: {value['id']}")
        else:
            pool[value["id"]] = value
    result = list(pool.values())
    v3.validate_splits(result)
    for value in result:
        if not v3.isolated(value, excluded):
            raise ValueError(f"Moving route overlaps an excluded pair: {value['id']}")
    # Keep complete old split membership as selection evidence, never as recordings.
    return sorted(result, key=lambda value: (value["split"], value["goal_kind"], value["id"]))


def follow_enabled(task, requested):
    if requested == "no":
        return False
    result = v3.read_json(Path(task) / "validation/attempt-2/metrics.json")
    group = result.get("by_kind", {}).get("follow", {}).get("v3", {})
    current_runner = campaign.binary_digest(Path(task) / "build/opengothic/gothic-body-runner")
    passed = (result.get("complete") is True and result.get("collection", {}).get("follow") is True
            and result.get("settings", {}).get("runner_sha256") == current_runner
            and group.get("episodes") == 10
            and group.get("contact_episodes") == 10 and group.get("observed_episodes") == 10
            and group.get("full_horizon_episodes") == 10 and group.get("nonterminal_episodes") == 10
            and (group.get("follow_fraction") or 0) >= .8)
    if requested == "yes" and not passed:
        raise ValueError("Follow recordings require the completed ten-case >=80% validation gate")
    return passed


def family(value):
    return value.get("dataset_family", value["goal_kind"])


def version(value):
    return 2 if family(value) == "jump" else 3


@lru_cache(maxsize=1)
def live_exclusions(path):
    suite = v3.read_json(path)
    if suite.get("training_eligible") is not False:
        raise ValueError("The live navigation suite must be excluded from training")
    identifiers, pairs = set(), set()
    for scenario in suite["scenarios"]:
        variants = [dict(scenario, **variant) for variant in scenario.get("variants", [])]
        for value in variants:
            value["id"] = scenario["id"] + "-" + value["id"]
        for value in [scenario, *variants]:
            if value.get("training_eligible") is not False:
                raise ValueError("A live navigation scenario is eligible for training")
            identifiers.add(value["id"])
            pairs.update(v3.legs(value))
    return identifiers, pairs


def directory_for(task, value):
    if value.get("split") != "train":
        raise ValueError("Validation and test recordings are forbidden")
    live_ids, live_pairs = live_exclusions(LIVE_SUITE)
    if value["id"] in live_ids or not v3.isolated(value, live_pairs):
        raise ValueError("Live navigation suite recordings are forbidden")
    identifier = value["id"]
    if Path(identifier).name != identifier or identifier in ("", ".", ".."):
        raise ValueError("Unsafe recording scenario ID")
    return Path(task) / "recordings" / family(value) / "train" / identifier


def recording_roots(options):
    roots = [options.task / "recordings"]
    if getattr(options, "archive_root", None) is not None:
        roots.append(Path(options.archive_root) / "recordings")
    return roots


def resolve_directory(options, value):
    """Resume archived attempts directly; active episodes always start locally."""
    local = directory_for(options.task, value)
    candidates = [root / local.relative_to(options.task / "recordings")
                  for root in recording_roots(options)]
    existing = [path for path in candidates if path.exists()]
    if len(existing) > 1:
        raise ValueError(f"Recording exists in both active and archived storage: {value['id']}")
    return existing[0] if existing else local


def v5(options):
    return getattr(options, "corpus_version", 4) == 5


def prepare_residency(options):
    """Export native knownroads membership once, inside the caller's gq job."""
    directory = options.task / "knownroads"
    snapshots = directory / "roads-snapshots.jsonl"
    if snapshots.is_file():
        return
    if directory.exists():
        raise ValueError("Incomplete native knownroads export retained")
    actors = [row for row in v3.rows(options.task / "discovery/roads-events.jsonl")
              if row.get("kind") == "roads_actors"]
    if len(actors) != 1:
        raise ValueError("Expected one native knownroads actor export")
    npcs = sorted({actor["instance"] for actor in actors[0]["actors"]
                   if actor["instance"] != "PC_HERO" and any(
                       routine["node"] is not None for routine in actor["routines"])})
    if not npcs:
        raise ValueError("Native knownroads export has no routine actors")
    args = runner_args(options, "knownroads")
    worker = campaign.Worker(args, 3, None)
    try:
        worker.scenario("demos-v5-knownroads", [f"roads load {args.roads}", *[
            command for index, npc in enumerate(npcs)
            for command in (f"roads on {npc}", f"roads snapshot actor-{index} {npc}")]], directory)
    finally:
        worker.close()
    exported = list(v3.rows(snapshots))
    if len(exported) != len(npcs) or {row["actor"] for row in exported} != set(npcs):
        raise ValueError("Native knownroads membership export is incomplete")


def residency_inputs(task):
    graph = v3.read_json(task / "discovery/roads-graph.json")
    positions = v3.load_start_positions(task / "discovery/points-v3.jsonl")
    snapshots = list(v3.rows(task / "knownroads/roads-snapshots.jsonl"))
    if not snapshots or any(row.get("kind") != "roads_snapshot" or row.get("stranger") is not False
                            for row in snapshots):
        raise ValueError("Expected ordinary native knownroads memberships")
    return graph["nodes"], positions, snapshots


def resident_scenario(value, inputs):
    nodes, positions, snapshots = inputs
    start = ([float(part) for part in value["start"][3:].split(",")]
             if value["start"].startswith("cm:") else positions[value["start"]])
    node = min(range(len(nodes)), key=lambda index: math.hypot(
        nodes[index][1] - start[0], nodes[index][3] - start[2]))
    candidates, known_candidates = [], []
    for row in snapshots:
        if row["actor"] == value.get("target"):
            continue
        routine_nodes = [routine["node"] for routine in row["routines"] if routine["node"] is not None]
        if routine_nodes:
            distance = min(math.hypot(nodes[index][1] - start[0], nodes[index][3] - start[2])
                           for index in routine_nodes)
            candidates.append((distance, row["actor"], row["spawn_cm"]))
            if node in row["known_node_ids"]:
                known_candidates.append(candidates[-1])
    if not candidates:
        raise ValueError(f"No native knownroads actor has routine membership: {value['id']}")
    distance, npc, spawn = min(known_candidates or candidates)
    # Scenario IDs are frozen; both groups therefore remain stable across phases.
    import hashlib
    local = (bool(known_candidates)
             and int(hashlib.sha256(value["id"].encode()).hexdigest()[:8], 16) % 2 == 0)
    selection = dict(source="native known_node_ids at nearest start node", nearest_start_node=node,
                     nearest_routine_to_start_cm=distance, spawn_cm=spawn)
    return dict(value, npc=npc, local=local, residency="local" if local else "stranger",
                knownroads_selection=selection)


def prepare_v5_selections(task):
    """Keep v4 scenario IDs, seeds and membership; add only the approved families."""
    names = (*FAMILIES, "water")
    selected = {name: list(v3.rows(task / "recording-input" / f"{name}.jsonl")) for name in names}
    if (len(selected["point"]) != 400 or len(selected["jump"]) != 168
            or len(selected["object"]) < 100 or len(selected["approach"]) < 60
            or len(selected["follow"]) < 60 or len(selected["water"]) < 40):
        raise ValueError("Frozen v5 candidate pools do not meet the approved family targets")
    for name, values in selected.items():
        if (len({value["id"] for value in values}) != len(values)
                or any(value.get("split") != "train" or family(value) != name for value in values)):
            raise ValueError(f"Invalid frozen train family: {name}")
    follow = [dict(value, dataset_family=FOLLOW_BELOW_GATE, follow_below_gate=True,
                   minimum_follow_fraction=FOLLOW_BELOW_GATE_MIN_FRACTION) for value in selected["follow"]]
    freeze_selection(task / "recording-input" / f"{FOLLOW_BELOW_GATE}.jsonl", follow)
    selected[FOLLOW_BELOW_GATE] = follow
    return selected


def setup_rejection_reason(error, value):
    """Recognize only native candidate validation before a moving episode starts."""
    if not isinstance(error, RuntimeError) or value.get("goal_kind") not in ("follow", "approach"):
        return None
    text = str(error)
    prefix = "Runner rejected advance: navdemos moving "
    if text in (prefix + "route needs distinct connected points", prefix + "route lacks waynet point"):
        return text
    waypoint = prefix + "missing route waypoint "
    if text.startswith(waypoint) and text[len(waypoint):] and not any(
            char.isspace() for char in text[len(waypoint):]):
        return text
    return None


def require_no_recording_output(directory):
    """A refusal may retain diagnostics, but never any episode recording output."""
    protected = {"episode.json", "actions.txt", "trajectory.txt", "target-trajectory.txt"}
    if any(path.name in protected or path.name.startswith("samples.bin")
           for path in Path(directory).rglob("*")):
        raise ValueError(f"Setup refusal has recording output; retaining incomplete attempt: {directory}")


def freeze_selection(path, values):
    """Verify frozen selections without even rewriting an identical old file."""
    path = Path(path)
    if path.exists():
        if list(v3.rows(path)) != values:
            raise ValueError(f"Refusing to replace scenario selection: {path}")
    else:
        v3.write_selection(path, values)


def prepare_selections(task, follow_below_gate=False):
    source = Path(task) / "input"
    excluded = v3.exclusion_pairs(source / "holdout-cv.jsonl", source / "ppo-validation.jsonl")
    training = list(v3.rows(source / "training-pairs.jsonl"))
    points = select_points(training, excluded)
    objects = select_objects(list(v3.rows(source / "object-candidates.jsonl")),
                             list(v3.rows(source / "object-probes.jsonl")), excluded)
    moving = select_moving(v3.read_json(source / "places.json")["places"], training, excluded,
                           list(v3.rows(source / "moving-scenarios.jsonl")))
    routes = jumps.load_routes(source / "jump-routes.jsonl", source / "jump-links.txt")
    if len(routes) != 267 or sum(route["split"] == "train" for route in routes) != 168:
        raise ValueError("Jump source must preserve the original 267 routes, including 168 train routes")
    jump_values = [dict(route, id=f"jump-{route['index']:04d}", goal_kind="point", dataset_family="jump", seed=19)
                   for route in jumps.collection_order(routes) if route["split"] == "train"]
    selections = dict(point=points, object=objects,
        approach=balanced([value for value in moving if value["goal_kind"] == "approach" and value["split"] == "train"], "region"),
        follow=balanced([value for value in moving if value["goal_kind"] == "follow" and value["split"] == "train"], "region"),
        jump=jump_values)
    for name, values in selections.items():
        if any(value["split"] != "train" for value in values):
            raise ValueError("Held-out scenario entered a recording selection")
        freeze_selection(Path(task) / "recording-input" / f"{name}.jsonl", values)
    freeze_selection(Path(task) / "recording-input/moving-all-splits.jsonl", moving)
    if follow_below_gate:
        # The additive family uses the frozen training selection verbatim. It
        # never changes the old family or expands any held-out membership.
        source = list(v3.rows(Path(task) / "recording-input/follow.jsonl"))
        selections[FOLLOW_BELOW_GATE] = [dict(value, dataset_family=FOLLOW_BELOW_GATE,
            follow_below_gate=True, minimum_follow_fraction=FOLLOW_BELOW_GATE_MIN_FRACTION)
            for value in source]
        freeze_selection(Path(task) / "recording-input" / f"{FOLLOW_BELOW_GATE}.jsonl",
                         selections[FOLLOW_BELOW_GATE])
    return selections


class StorageLimit(ValueError):
    """No new attempt was started because its reservation would exceed space."""


class StorageBudget:
    """Aggregate active and archived recordings, including failed attempts."""
    def __init__(self, root, max_bytes=v3.MAX_BYTES, *, archive_root=None, min_free_bytes=256 * 1024**2):
        self.root, self.max_bytes = Path(root), max_bytes
        self.archive_root = Path(archive_root) if archive_root is not None else None
        self.min_free_bytes = min_free_bytes
        self.root.mkdir(parents=True, exist_ok=True)
        if not 0 < max_bytes <= (V5_MAX_BYTES if self.archive_root is not None else v3.MAX_BYTES):
            raise ValueError("Recording budget exceeds the corpus storage ceiling")
        self.reserved, self.lock = 0, threading.Lock()

    def used(self):
        return campaign.tree_bytes(self.root) + (campaign.tree_bytes(self.archive_root)
            if self.archive_root is not None and not self.archive_root.is_relative_to(self.root) else 0)

    @staticmethod
    def estimate(limit_ms, jump=False):
        amount = math.ceil(limit_ms / 100) * v3.ESTIMATED_DECISION_BYTES
        return max(amount, jumps.RESERVE_BYTES) if jump else amount

    def require_room(self, amount):
        used, free = self.used(), shutil.disk_usage(self.root).free
        if used + self.reserved + amount > self.max_bytes or free < self.reserved + amount + self.min_free_bytes:
            raise StorageLimit(f"Disk budget: used={used}, reserved={self.reserved}, next={amount}, limit={self.max_bytes}, free={free}")

    def can_reserve_batch(self, limit_ms, count, jump=False):
        with self.lock:
            try:
                self.require_room(self.estimate(limit_ms, jump) * count)
            except StorageLimit:
                return False
            return True

    @contextmanager
    def reserve(self, limit_ms, jump=False):
        amount = self.estimate(limit_ms, jump)
        with self.lock:
            self.require_room(amount)
            self.reserved += amount
        try:
            yield
        finally:
            with self.lock:
                self.reserved -= amount
                if self.used() > self.max_bytes:
                    raise ValueError("Aggregate recording budget exceeded its storage ceiling")


def runner_args(options, index, jump=False):
    task = options.task
    return SimpleNamespace(output=task / "recording-workers" / str(index) / ("jumps" if jump else "v3"),
        runner=task / "build/opengothic/gothic-body-runner", assets=Path(os.environ.get("GOTHIC_DIR", Path.home() / "gothic-ai/game-settings")),
        save=task / "input/save.sav", cache=task / "recording-cache" / ("jumps" if jump else "v3"),
        links=task / "input" / ("jump-links.txt" if jump else "accepted-links.txt"),
        roads=task / "input/places.json", npc="BAU_954_MALETH", ini=None, seed=SEED,
        physics="cv", watchdog_seconds=900, limit_ms=options.limit_ms,
        contract=task / "build/opengothic/gothic-navmemoryteacher-contract", corpus_version=5 if v5(options) else 4,
        familiar=task / "input/familiar.bin")


def commands(args, value):
    directory_for(Path("/unused"), value)
    npc = value.get("npc", args.npc)
    episode_args = SimpleNamespace(**dict(vars(args), npc=npc))
    start = (jumps.scenario_commands(episode_args, value)[-2] if family(value) == "jump"
             else v3.start_command(value, 3, npc, args.limit_ms))
    initial = [f"roads load {args.roads}"]
    if getattr(args, "corpus_version", 4) == 5:
        # Use the fresh save's native NPC origin, as in prepare_residency.
        initial += [f"roads familiar-load {args.familiar}",
            f"roads {'on' if value['local'] else 'stranger'} {npc}",
            f"roads snapshot recording {npc}", f"roads familiar-stats {npc} 300",
            "navdemos perception-monitor on"]
    else:
        initial.append(f"roads on {npc}")
    return [*initial, "navdemos record on", "navdemos perception-mode gaze",
            "navdemos oracle off", start, "navdemos wait"]


def validate_native(metadata, value, identity):
    memory_version = identity.get("navigation_memory_version", MEMORY_VERSION)
    demo_version = identity.get("demo_version", 2)
    if (metadata.get("teacher_version") != version(value) or metadata.get("demo_version") != demo_version
            or metadata.get("navigation_memory_version") != memory_version
            or metadata.get("perception_mode") != "gaze" or metadata.get("physics_mode") != "cv"
            or metadata.get("oracle") is not False or metadata.get("record") is not True
            or metadata.get("demonstration_recorded") is not True
            or metadata.get("drive", False) or metadata.get("replay", False)
            or metadata.get("limit_ms") != identity["limit_ms"]
            or metadata.get("start") != value["start"] or metadata.get("goal_kind") != value["goal_kind"]):
        raise ValueError(f"Recording identity, memory or mode differs: {value['id']}")
    if memory_version == 4 and (metadata.get("journey_memory") is not True
            or metadata.get("familiar_map") is not value["local"] or metadata.get("perception_monitor") is not True):
        raise ValueError(f"Recording lacks journey memory, native familiarity or physical monitoring: {value['id']}")
    if metadata.get("observation_source") != ("detour_teacher_v2" if family(value) == "jump" else "learner_memory"):
        raise ValueError("Wrong teacher observation source")
    if value["goal_kind"] not in ("follow", "approach") and metadata.get("goal") != value["goal"]:
        raise ValueError("Raw recording goal differs")
    # Point/object native metadata uses zero for the moving-target seed. The
    # global scenario seed is witnessed by the command file, not this field.
    if value["goal_kind"] in ("follow", "approach") and metadata.get("seed") != value["seed"]:
        raise ValueError(f"Recording seed differs: {value['id']}")
    if value["goal_kind"] == "approach" and (metadata.get("target_immortal") is not True
            or metadata.get("target_nonhostile") is not True or metadata.get("target_final_dead") is not False
            or metadata.get("target_final_down") is not False):
        raise ValueError("Approach fixture lacks immortal, nonhostile, surviving target evidence")
    if "id" in metadata and (metadata.get("recording_identity") != identity
            or any((metadata.get("command_seed") if key == "seed" and value["goal_kind"] in ("point", "object")
                    else metadata.get(key)) != value.get(key) for key in v3.SCENARIO_IDENTITY_FIELDS)
            or (family(value) == "jump" and any(metadata.get(key) != value.get(key) for key in
                ("index", "site_id", "transition_id", "type", "dataset_family")))
            or (family(value) == FOLLOW_BELOW_GATE and (any(metadata.get(key) != value.get(key) for key in
                ("dataset_family", "follow_below_gate", "minimum_follow_fraction"))
                or metadata.get("teacher_goal_success") != metadata.get("success")))):
        raise ValueError(f"Completed recording belongs to another frozen scenario: {value['id']}")
    if "id" in metadata and memory_version == 4 and any(metadata.get(key) != value[key]
            for key in ("npc", "local", "residency", "knownroads_selection")):
        raise ValueError(f"Completed recording belongs to another resident: {value['id']}")
    if "id" in metadata and memory_version == 4 and (metadata.get("teacher_familiar_hint") is not False
            or metadata.get("teacher_variant") != ("v2" if family(value) == "jump" else "v3b")):
        raise ValueError(f"Completed recording has another teacher variant or familiar hint: {value['id']}")


def eligibility(metadata, moving=None):
    if metadata.get("dataset_family") == "jump":
        return metadata.get("transition_success") is True
    if metadata.get("dataset_family") == "water":
        return (metadata.get("success") is True and moving is not None
                and moving.get("deep_water_frames") == 0 and moving.get("swimming_frames") == 0)
    if metadata.get("goal_kind") == "follow":
        moving = moving or {}
        if metadata.get("dataset_family") == FOLLOW_BELOW_GATE:
            return (metadata.get("follow_below_gate") is True
                    and metadata.get("minimum_follow_fraction") == FOLLOW_BELOW_GATE_MIN_FRACTION
                    and metadata.get("failure") not in ("actor_down", "target_down", "collection_limit")
                    and all(metadata.get(key) is False for key in
                        ("final_dead", "final_down", "target_final_dead", "target_final_down"))
                    and metadata.get("elapsed_ms", 0) >= metadata["limit_ms"]
                    and moving.get("first_contact_ms") is not None and moving.get("follow_observed_ms", 0) > 0
                    and (moving.get("follow_fraction") or 0) >= FOLLOW_BELOW_GATE_MIN_FRACTION)
        return (metadata.get("success") is True and not metadata.get("final_dead", False)
                and not metadata.get("final_down", False) and metadata.get("elapsed_ms", 0) >= metadata["limit_ms"]
                and moving.get("first_contact_ms") is not None and moving.get("follow_observed_ms", 0) > 0
                and (moving.get("follow_fraction") or 0) >= .8)
    return metadata.get("success") is True


def native_residency(directory, value):
    snapshots = [row for row in v3.rows(Path(directory) / "roads-snapshots.jsonl")
                 if row.get("tag") == "recording"]
    stats = [row for row in v3.rows(Path(directory) / "roads-events.jsonl")
             if row.get("kind") == "familiar_stats"]
    if len(snapshots) != 1 or len(stats) != 1:
        raise ValueError(f"Missing native knownroads/familiarity evidence: {directory}")
    known, familiar = snapshots[0], stats[0]
    stranger = not value["local"]
    if (known.get("actor") != value["npc"] or familiar.get("actor") != value["npc"]
            or known.get("stranger") is not stranger or familiar.get("stranger") is not stranger):
        raise ValueError(f"Native knownroads resident differs: {directory}")
    if value["local"]:
        if (value["knownroads_selection"]["nearest_start_node"] not in known.get("known_node_ids", [])
                or familiar.get("known_tiles", 0) <= 0):
            raise ValueError(f"Local actor lacks native start knowledge or familiar tiles: {directory}")
    # Strangers know major roads, but have no familiar terrain tiles.
    elif any(familiar.get(key) != 0
            for key in ("known_tiles", "known_2m_tiles_xz", "eligible_2m_tiles", "familiar_source_poses")):
        raise ValueError(f"Stranger unexpectedly knows terrain: {directory}")
    return dict(knownroads=known, familiar=familiar)


def water_metrics(directory, metadata):
    frames = list(v3.rows(Path(directory) / "perception-safety-frames.jsonl"))
    if (len(frames) < 2 or frames[0].get("elapsed_ms") != 0
            or any(right["elapsed_ms"] <= left["elapsed_ms"] for left, right in zip(frames, frames[1:]))
            or frames[-1].get("elapsed_ms") != metadata["elapsed_ms"]):
        raise ValueError(f"Missing or incomplete physical water evidence: {directory}")
    for frame in frames:
        if (type(frame.get("swimming")) is not bool or type(frame.get("cv_active")) is not bool
                or not isinstance(frame.get("water_depth_cm"), (int, float))
                or not math.isfinite(frame["water_depth_cm"]) or frame["water_depth_cm"] < 0
                or not isinstance(frame.get("deep_water_threshold_cm"), (int, float))
                or not math.isfinite(frame["deep_water_threshold_cm"]) or frame["deep_water_threshold_cm"] <= 0):
            raise ValueError(f"Invalid physical water evidence: {directory}")
    if not any(frame["cv_active"] for frame in frames):
        raise ValueError(f"Physical monitor lacks CV frames: {directory}")
    wet = [frame["water_depth_cm"] > 10 for frame in frames]
    deep = [frame["water_depth_cm"] + 1 >= frame["deep_water_threshold_cm"] for frame in frames]
    def entries(values):
        return sum(value and not previous for previous, value in zip([False, *values[:-1]], values))
    return dict(physical_frames=len(frames), entered_water_frames=sum(wet), deep_water_frames=sum(deep),
                swimming_frames=sum(frame["swimming"] for frame in frames), water_entries=entries(wet),
                deep_water_entries=entries(deep), max_water_depth_cm=max(frame["water_depth_cm"] for frame in frames))


def measured_episode(directory, metadata, checked):
    result = {key: metadata.get(key) for key in ("id", "goal_kind", "split", "region", "place_id", "success",
        "failure", "elapsed_ms", "decisions", "teacher_version", "navigation_memory_version", "transition_success",
        "travel_cm", "detour_length_cm", "journey_memory", "familiar_map", "demo_version")}
    result.update(family=metadata.get("dataset_family", metadata["goal_kind"]), episode=str(directory),
        key_counts=checked["key_counts"],
        payload_bytes=sum(path.stat().st_size for path in Path(directory).rglob("*")
                          if path.is_file() and path.name != "episode.json"), validated=True)
    if metadata["goal_kind"] in ("follow", "approach"):
        import numpy as np
        trace = np.loadtxt(Path(directory) / "target-trajectory.txt", ndmin=2)
        if trace.shape != (metadata["decisions"] + 1, 6):
            raise ValueError(f"Moving target trace is incomplete: {directory}")
        result.update(v3.follow_metrics(trace[:, 0], trace[:, 4], metadata["elapsed_ms"]))
    if metadata.get("navigation_memory_version") == 4:
        result.update({key: metadata[key] for key in ("npc", "local", "residency", "knownroads_selection")})
        result.update(teacher_variant="v2" if result["family"] == "jump" else "v3b",
                      teacher_familiar_hint=False,
                      **native_residency(directory, metadata), **water_metrics(directory, metadata))
    result["training_eligible"] = eligibility(metadata, result)
    if result["family"] == FOLLOW_BELOW_GATE:
        result.update(follow_below_gate=True, minimum_follow_fraction=FOLLOW_BELOW_GATE_MIN_FRACTION,
                      teacher_goal_success=metadata.get("teacher_goal_success", metadata["success"]))
    return result


def replay_contract(binary, directory, decisions, *, write=False):
    """Replay each time, but keep already checked recording metadata immutable."""
    process = subprocess.run([str(binary), str(Path(directory) / "samples.bin.gz")],
                             capture_output=True, text=True, check=False)
    if process.returncode:
        raise ValueError(f"Teacher contract failed: {directory}: {process.stderr[-2000:]}")
    import json
    result = json.loads(process.stdout)
    if result.get("records") != decisions or result.get("mismatches") != 0 or result.get("previous_mismatches") != 0:
        raise ValueError(f"Teacher contract mismatch: {directory}: {result}")
    path = Path(directory) / "contract.json"
    if path.exists():
        saved = v3.read_json(path)
        if (saved.get("records") != decisions or saved.get("mismatches") != 0
                or saved.get("previous_mismatches") != 0):
            raise ValueError(f"Saved contract changed: {directory}")
    elif write:
        atomic_json(path, result)
    return result


class Worker(campaign.Worker):
    def __init__(self, options, index, budget, identity, state=None):
        self.options, self.index, self.identity = options, index, identity
        self.state = state or CollectionState(options.task / "recordings/collection-state.json", identity,
                                             archive_root=getattr(options, "archive_root", None))
        self.residency = residency_inputs(options.task) if v5(options) else None
        self.jump = False
        super().__init__(runner_args(options, index), 3, budget)

    def resume(self, value, directory):
        """Completed attempts own their parameters and finalized measurements."""
        if not v3.complete(directory):
            raise ValueError(f"Incomplete recording retained: {directory}")
        metadata = v3.read_json(directory / "episode.json")
        raw_v5 = v5(self.options) and "id" not in metadata
        if v5(self.options):
            validate_native(metadata, value, resumed_recording_identity(self.identity, value, metadata))
        if raw_v5:
            actual_seed = metadata.get("seed")
            metadata.update(value, recording_identity=recording_identity(self.identity, value),
                            command_seed=value["seed"], teacher_familiar_hint=False,
                            teacher_variant="v2" if family(value) == "jump" else "v3b",
                            **native_residency(directory, value))
            metadata["seed"] = actual_seed
            if family(value) == FOLLOW_BELOW_GATE:
                metadata["teacher_goal_success"] = metadata["success"]
        parameters = recorded_parameters(metadata)
        saved = metadata.get("collection_metrics") or self.state.saved_metrics(directory)
        digest = self.state.verify_samples(directory, metadata, allow_unpinned=saved is None)
        checked = demos.summarize(directory, validate=True)
        if v5(self.options) and (checked.get("navigation_memory_versions") != [4]
                                or checked.get("format_versions") != [3]):
            raise ValueError("Only memory-v4 NAVDEMO-v3 recordings are allowed")
        if saved is not None:
            measured = dict(saved)
        else:
            # A native attempt completed just before collection was interrupted.
            # Fill only missing collection labels; keep its recorded seed and
            # parameters. Finalization is saved outside the recording directory.
            for key, item in value.items():
                metadata.setdefault(key, item)
            metadata.setdefault("command_seed", value["seed"])
            if family(value) == "jump":
                metadata.update(jumps.assess_episode(directory, metadata))
            else:
                replay_contract(self.args.contract, directory, metadata["decisions"])
            measured = measured_episode(directory, metadata, checked)
        if v5(self.options):
            measured = measured_episode(directory, dict(value, **metadata), checked)
        measured.update(episode=str(directory), seed=metadata.get("seed"), scenario=parameters,
                        samples_sha256=digest)
        if raw_v5:
            metadata.update(training_eligible=measured["training_eligible"], collection_metrics=measured,
                            samples_sha256=digest)
            atomic_json(directory / "episode.json", metadata)
        self.state.pin_samples(directory, digest)
        self.state.remember_metrics(directory, measured)
        campaign.emit("recording_complete", family=family(value), id=value["id"], resumed=True,
            success=measured["success"], training_eligible=measured["training_eligible"], decisions=measured["decisions"])
        return measured

    def trial(self, value):
        if self.residency is not None:
            value = resident_scenario(value, self.residency)
        directory = resolve_directory(self.options, value)
        rejected = self.state.rejected_candidate(directory)
        if rejected is not None:
            require_no_recording_output(directory)
            return rejected
        if (directory / "episode.json").is_file():
            return self.resume(value, directory)
        identity = recording_identity(self.identity, value)
        jump = family(value) == "jump"
        if jump != self.jump:
            self.close()
            self.jump, self.args = jump, runner_args(self.options, self.index, jump)
        prefix = "demos-v5" if v5(self.options) else "demos-v4"
        scenario_name = (f"{prefix}-{FOLLOW_BELOW_GATE}-{value['id']}" if family(value) == FOLLOW_BELOW_GATE
                         else f"{prefix}-{value['id']}")
        expected = [f"seed {value['seed']}", "cheat god", "set time 10 0", "navdemos physics cv",
                    *commands(self.args, value), "label done"]
        started = time.monotonic()
        if directory.exists():
            raise ValueError(f"Partial recording retained; refusing to overwrite: {directory}")
        if getattr(self.options, "verify_existing_only", False):
            raise ValueError(f"Verification cannot start a new recording: {directory}")
        with self.budget.reserve(self.args.limit_ms, jump=jump):
            campaign.emit("recording_started", family=family(value), id=value["id"])
            try:
                self.scenario(scenario_name, commands(self.args, value), directory, value["seed"])
            except RuntimeError as error:
                reason = setup_rejection_reason(error, value)
                process = getattr(self.runner, "process", None)
                if reason is None or process is None or process.poll() is not None:
                    raise
                try:
                    require_no_recording_output(directory)
                except ValueError:
                    raise error from None
                # A rejected advance leaves its scenario active. Replace only
                # this worker's engine; close failures remain fatal.
                self.close()
                rejected = self.state.reject_candidate(directory, value, reason)
                campaign.emit("candidate_rejected", family=family(value), id=value["id"], reason=reason)
                return rejected
        if not v3.complete(directory):
            raise ValueError(f"Incomplete recording retained: {directory}")
        witness = self.args.output / "scenarios" / f"{scenario_name}.bench"
        if not witness.is_file() or witness.read_text(encoding="utf-8").splitlines() != expected:
            raise ValueError(f"Missing or changed seeded command witness: {value['id']}")
        metadata = v3.read_json(directory / "episode.json")
        validate_native(metadata, value, identity)
        metadata["runner_goal"] = metadata.get("goal")
        actual_seed = metadata.get("seed")
        metadata.update(value, recording_identity=identity, collector_wall_s=time.monotonic() - started,
                        command_seed=value["seed"])
        if v5(self.options):
            metadata.update(teacher_variant="v2" if jump else "v3b", teacher_familiar_hint=False,
                            **native_residency(directory, value))
        metadata["seed"] = actual_seed
        if family(value) == FOLLOW_BELOW_GATE:
            metadata["teacher_goal_success"] = metadata["success"]
        digest = self.state.verify_samples(directory, metadata, allow_unpinned=True)
        if jump:
            metadata.update(jumps.assess_episode(directory, value))
        checked = demos.summarize(directory, validate=True)
        if (checked.get("navigation_memory_versions") != [4 if v5(self.options) else 3]
                or checked.get("format_versions") != [3 if v5(self.options) else 2]):
            raise ValueError("Recording memory and NAVDEMO versions differ from this corpus")
        if not jump:
            replay_contract(self.args.contract, directory, metadata["decisions"], write=True)
        measured = measured_episode(directory, metadata, checked)
        measured.update(seed=metadata.get("seed"), scenario=recorded_parameters(metadata), samples_sha256=digest)
        metadata.update(training_eligible=measured["training_eligible"], collection_metrics=measured,
                        samples_sha256=digest)
        atomic_json(directory / "episode.json", metadata)
        self.state.pin_samples(directory, digest)
        self.state.remember_metrics(directory, measured)
        campaign.emit("recording_complete", family=family(value), id=value["id"], resumed=False,
            success=measured["success"], training_eligible=measured["training_eligible"], decisions=measured["decisions"])
        return measured


def settings(options, include_follow):
    task = options.task
    validation = task / "validation/attempt-2/metrics.json"
    result = dict(schema_version=1, navigation_memory_version=4 if v5(options) else 3,
        demo_version=3 if v5(options) else 2, perception_mode="gaze", physics="cv",
        validation_attempt_2_sha256=campaign.binary_digest(validation) if validation.is_file() else None,
        input_sha256={name: campaign.binary_digest(task / "input" / name) for name in INPUTS},
        runner_sha256=campaign.binary_digest(task / "build/opengothic/gothic-body-runner"),
        contract_sha256=campaign.binary_digest(task / "build/opengothic/gothic-navmemoryteacher-contract"),
        limit_ms=options.limit_ms, include_follow=include_follow, seed=SEED, max_bytes=options.max_bytes)
    if v5(options):
        result.update(corpus_version=5, teacher_variant="v3b", student_input="hd", teacher_familiar_hint=False,
            archive_root=str(options.archive_root), local_min_free_bytes=V5_MIN_FREE_BYTES,
            selection_sha256={name: campaign.binary_digest(task / "recording-input" / f"{name}.jsonl")
                              for name in (*FAMILIES, "water")},
            v5_input_sha256={name: campaign.binary_digest(task / "input" / name) for name in
                             ("v4-manifest.json", "detour-val.jsonl", "detour-test.jsonl", "familiar.bin")},
            knownroads_sha256={name: campaign.binary_digest(task / name) for name in
                ("knownroads/roads-snapshots.jsonl", "discovery/roads-graph.json", "discovery/points-v3.jsonl")})
    return result


def v2_memory4_upgrade_base(base_identity, prior_settings, *, upgrade_v2_memory=False):
    """Authorize only the one-time v5 runner replacement for v2 memory recording."""
    error = "Recording state belongs to another binary, input, selection or follow gate"
    if prior_settings is None:
        if upgrade_v2_memory:
            raise ValueError("The v2 memory upgrade requires an existing frozen v5 collection")
        return None
    prior_base = (prior_settings.get("base_recording_identity")
                  if prior_settings.get("schema_version") == 2 else prior_settings)
    previous = prior_settings.get("v2_memory4_previous_base")
    if previous is not None:
        if (prior_settings.get("schema_version") != 2 or not isinstance(previous, dict)
                or previous.get("corpus_version") != 5 or previous.get("navigation_memory_version") != 4
                or previous.get("demo_version") != 3 or previous.get("runner_sha256") == prior_base.get("runner_sha256")
                or dict(previous, runner_sha256=prior_base.get("runner_sha256")) != prior_base):
            raise ValueError("Invalid frozen v2 memory upgrade provenance")
    if prior_base == base_identity:
        if upgrade_v2_memory and previous is None:
            raise ValueError("The v2 memory upgrade requires a changed runner")
        return previous
    if (not upgrade_v2_memory or previous is not None or prior_settings.get("schema_version") != 2
            or base_identity.get("corpus_version") != 5 or base_identity.get("navigation_memory_version") != 4
            or base_identity.get("demo_version") != 3
            or not prior_base.get("runner_sha256") or not base_identity.get("runner_sha256")
            or dict(prior_base, runner_sha256=base_identity["runner_sha256"]) != base_identity):
        raise ValueError(error)
    return prior_base


def collection_settings(base_identity, prior_settings=None, *, follow_below_gate=False, source_follow_sha256=None,
                        upgrade_v2_memory=False):
    """Authorize an additive family while preserving the original recording identity.

    All old core inputs must match before the caller creates selections, changes
    state, or launches a runner. Subsequent resumes require exactly the same
    explicit opt-in and family policy.
    """
    previous = v2_memory4_upgrade_base(base_identity, prior_settings, upgrade_v2_memory=upgrade_v2_memory)
    if prior_settings is not None and prior_settings.get("schema_version") == 2:
        if not follow_below_gate:
            raise ValueError("Existing follow_below_gate recordings require the explicit --follow-below-gate opt-in")
    if not follow_below_gate:
        return base_identity
    if base_identity.get("include_follow") is not False:
        raise ValueError("The additive follow_below_gate family cannot enable the ordinary follow family")
    if not source_follow_sha256:
        raise ValueError("The additive follow family requires its frozen source selection hash")
    policy = dict(dataset_family=FOLLOW_BELOW_GATE, follow_below_gate=True,
        minimum_follow_fraction=FOLLOW_BELOW_GATE_MIN_FRACTION,
        metric="time_weighted_2m_5m_after_first_contact", require_full_horizon=True,
        require_surviving_actor_and_target=True, source_family="follow",
        source_selection_sha256=source_follow_sha256,
        source_validation_attempt_2_sha256=base_identity["validation_attempt_2_sha256"])
    result = dict(schema_version=2, base_recording_identity=base_identity,
                  family_policies={FOLLOW_BELOW_GATE: policy})
    if previous is not None:
        result["v2_memory4_previous_base"] = previous
    if prior_settings is not None and prior_settings.get("schema_version") == 2:
        expected = dict(prior_settings, base_recording_identity=base_identity)
        if previous is not None:
            expected["v2_memory4_previous_base"] = previous
        if expected != result:
            raise ValueError("Frozen follow_below_gate authorization policy or source selection changed")
    return result


def recording_identity(identity, value):
    """Old families retain their exact schema-v1 identity after state migration."""
    if identity.get("schema_version") != 2:
        if family(value) == FOLLOW_BELOW_GATE:
            raise ValueError("follow_below_gate recordings require an additive family identity")
        return identity
    base = identity["base_recording_identity"]
    if family(value) != FOLLOW_BELOW_GATE:
        return base
    policy = identity["family_policies"][FOLLOW_BELOW_GATE]
    if (value.get("goal_kind") != "follow" or value.get("follow_below_gate") is not True
            or value.get("minimum_follow_fraction") != policy["minimum_follow_fraction"]):
        raise ValueError("Scenario differs from the frozen follow_below_gate policy")
    return dict(base, schema_version=2, dataset_family=FOLLOW_BELOW_GATE, follow_below_gate=True,
                minimum_follow_fraction=policy["minimum_follow_fraction"], family_policy=policy)


def resumed_recording_identity(identity, value, metadata):
    current = recording_identity(identity, value)
    recorded = metadata.get("recording_identity")
    if "id" not in metadata or recorded == current:
        return current
    previous = identity.get("v2_memory4_previous_base")
    if previous is not None:
        prior = recording_identity(dict(identity, base_recording_identity=previous), value)
        if recorded == prior:
            return prior
    raise ValueError(f"Completed recording belongs to another frozen binary: {value['id']}")


def v2_memory3_file_hashes(directory):
    """Inventory obsolete native v2 output before moving it out of the corpus."""
    directory = Path(directory)
    if directory.is_symlink():
        raise ValueError(f"Symbolic link in obsolete v2 recording: {directory}")
    result = {}
    for path in directory.rglob("*"):
        if path.is_symlink() or (not path.is_file() and not path.is_dir()):
            raise ValueError(f"Nonregular file in obsolete v2 recording: {path}")
        if path.is_file():
            result[str(path.relative_to(directory))] = campaign.binary_digest(path)
    return result


def archive_v2_memory3(options, selections, state):
    """Preserve raw v2 attempts superseded by the authorized memory-v4 runner."""
    if not state.data["settings"].get("v2_memory4_previous_base"):
        return
    archived = dict(state.data.get("superseded_v2_memory3", {}))
    archive_root = Path(options.archive_root)
    for value in selections["jump"]:
        directory = directory_for(options.task, value)
        if not (directory / "episode.json").is_file():
            continue
        metadata = v3.read_json(directory / "episode.json")
        if (metadata.get("teacher_version") != 2 or metadata.get("demo_version") != 2
                or metadata.get("navigation_memory_version") != 3 or metadata.get("journey_memory") is not False):
            continue
        key = state.key(directory)
        if ("id" in metadata or "recording_identity" in metadata or "collection_metrics" in metadata
                or "samples_sha256" in metadata or key in state.data["integrity"]["episodes"]
                or state.saved_metrics(directory) is not None or state.rejected_candidate(directory) is not None
                or directory_for(archive_root, value).exists()):
            raise ValueError(f"Obsolete v2 recording collides with finalized corpus state: {directory}")
        if not v3.complete(directory):
            raise ValueError(f"Incomplete obsolete v2 recording retained: {directory}")
        old_identity = dict(state.data["settings"]["v2_memory4_previous_base"],
                            navigation_memory_version=3, demo_version=2)
        validate_native(metadata, value, old_identity)
        checked = demos.summarize(directory, validate=True)
        if checked.get("navigation_memory_versions") != [3] or checked.get("format_versions") != [2]:
            raise ValueError(f"Obsolete v2 sample format differs: {directory}")
        destination = archive_root / "superseded-v2-memory3" / key
        expected = v2_memory3_file_hashes(directory)
        present = v2_memory3_file_hashes(destination)
        if any(expected.get(name) != digest for name, digest in present.items()):
            raise ValueError(f"Conflicting obsolete v2 archive retained: {destination}")
        additional = sum((directory / name).stat().st_size for name in expected if name not in present)
        if campaign.tree_bytes(options.task / "recordings") + campaign.tree_bytes(archive_root) + additional > options.max_bytes:
            raise ValueError("Obsolete v2 archival would exceed the corpus byte budget")
        destination.mkdir(parents=True, exist_ok=True)
        subprocess.run(["rsync", "-a", "--no-links", "--", str(directory) + "/", str(destination) + "/"], check=True)
        if v2_memory3_file_hashes(destination) != expected:
            raise ValueError(f"Obsolete v2 archive checksum mismatch: {destination}")
        event = dict(id=value["id"], directory=str(destination), files_sha256=expected)
        if key in archived and archived[key] != event:
            raise ValueError(f"Obsolete v2 archive event differs: {directory}")
        archived[key] = event
        # Save the completed transfer before deleting its local copy. A retry
        # checks the same files if interrupted between these two operations.
        state.update(superseded_v2_memory3=archived)
        shutil.rmtree(directory)
        campaign.emit("recording_v2_memory3_archived", id=value["id"], directory=str(destination))


def summarize(episodes, selections, include_follow, used, follow_below_gate=False, point_limit=None,
              rejected_candidates=(), point_candidates=None, corpus_version=4):
    groups = {}
    for name in (*FAMILIES, *((FOLLOW_BELOW_GATE,) if follow_below_gate else ()),
                 *(("water",) if corpus_version == 5 else ())):
        values = [value for value in episodes if value["family"] == name]
        rejected = [value for value in rejected_candidates if value["family"] == name]
        keys = sum((Counter(value["key_counts"]) for value in values), Counter())
        decisions = sum(value["decisions"] for value in values)
        eligible = sum(value["training_eligible"] for value in values)
        needed = (point_limit if name == "point" and point_limit is not None else
                  len(selections[name]) if name in ("point", "jump") else 100 if name == "object"
                  else 40 if name == "water" else 60)
        complete = len(values) >= needed if name in ("point", "jump") else eligible >= needed
        if name == "follow" and not include_follow:
            complete = True
        groups[name] = dict(episodes=len(values),
            successes=eligible if name in ("jump", FOLLOW_BELOW_GATE) else sum(bool(value["success"]) for value in values),
            teacher_goal_successes=sum(bool(value["success"]) for value in values),
            training_eligible=eligible, decisions=decisions, backward=keys["back"], strafe=keys["strafe"],
            backward_fraction=keys["back"] / decisions if decisions else None,
            strafe_fraction=keys["strafe"] / decisions if decisions else None, jump=keys["jump"], climb=keys["climb"],
            dataset_bytes=sum(campaign.tree_bytes(value["episode"]) for value in values), complete=complete,
            target=needed, available_candidates=len(selections[name]), excluded=name == "follow" and not include_follow,
            rejected_candidates=len(rejected),
            candidate_pool_exhausted=(len(values) >= point_candidates if name == "point" and point_candidates is not None
                                      else len(values) + len(rejected) >= len(selections[name])) and not complete)
        if name == FOLLOW_BELOW_GATE:
            groups[name].update(follow_below_gate=True,
                                minimum_follow_fraction=FOLLOW_BELOW_GATE_MIN_FRACTION)
        if corpus_version == 5:
            groups[name].update(local_episodes=sum(value["local"] for value in values),
                local_fraction=sum(value["local"] for value in values) / len(values) if values else None,
                water_entries=sum(value["water_entries"] for value in values),
                deep_water_entries=sum(value["deep_water_entries"] for value in values),
                deep_water_frames=sum(value["deep_water_frames"] for value in values),
                swimming_frames=sum(value["swimming_frames"] for value in values))
    result = dict(complete=all(value["complete"] for value in groups.values()), by_family=groups,
                point_limit=groups["point"]["target"],
                navigation_memory_version=4 if corpus_version == 5 else 3,
                demo_version=3 if corpus_version == 5 else 2, dataset_bytes=used, episodes=episodes,
                rejected_candidates=list(rejected_candidates),
                student_training_run=False)
    if corpus_version == 5:
        result.update(corpus_version=5, student_input="hd", teacher_familiar_hint=False,
            local_episodes=sum(value["local"] for value in episodes),
            local_fraction=sum(value["local"] for value in episodes) / len(episodes) if episodes else None)
    return result


def run(options):
    deadline = time.monotonic() + options.wall_minutes * 60
    if sys.platform != "linux":
        raise ValueError("Run collection on Ryzen/Linux inside the caller's gq batch")
    follow_below_gate = v5(options) or getattr(options, "follow_below_gate", False)
    point_limit = getattr(options, "point_limit", 400)
    verify_only = getattr(options, "verify_existing_only", False)
    expect_existing = getattr(options, "expect_existing", None)
    upgrade_v2_memory = getattr(options, "upgrade_v2_memory", False)
    if upgrade_v2_memory and not v5(options):
        raise ValueError("The v2 memory upgrade is restricted to the v5 corpus")
    if v5(options):
        if point_limit != 400 or getattr(options, "archive_root", None) is None:
            raise ValueError("V5 requires all 400 road pairs and an absolute archive root")
        if not Path(options.archive_root).is_absolute():
            raise ValueError("V5 archive root must be absolute")
        os.environ["OPENGOTHIC_NAV_JOURNEY_MEMORY"] = "1"
        # Unset attaches familiar memory while keeping the teacher hint disabled.
        os.environ.pop("OPENGOTHIC_NAV_FAMILIAR_MAP", None)
    if follow_below_gate and options.include_follow == "yes":
        raise ValueError("--follow-below-gate is a separate family; it cannot enable ordinary follow")
    include_follow = False if follow_below_gate else follow_enabled(options.task, options.include_follow)
    base_identity = settings(options, include_follow)
    output = options.task / "recordings"
    state_path = output / "collection-state.json"
    prior_state = None
    prior_settings = None
    if state_path.exists():
        prior_state = v3.read_json(state_path)
        prior_settings = prior_state.get("settings")
        if not isinstance(prior_settings, dict):
            raise ValueError("Existing recording state lacks its frozen collection identity")
        # Check every old core identity field before making any new file. The
        # policy check below additionally freezes the new family's source hash.
        v2_memory4_upgrade_base(base_identity, prior_settings, upgrade_v2_memory=upgrade_v2_memory)
        if prior_settings.get("schema_version") == 2 and not follow_below_gate:
            raise ValueError("Existing follow_below_gate recordings require the explicit --follow-below-gate opt-in")
        for name in (*FAMILIES, *((FOLLOW_BELOW_GATE, "water") if v5(options) else ("moving-all-splits",))):
            if not (options.task / "recording-input" / f"{name}.jsonl").is_file():
                raise ValueError(f"Existing collection lacks its frozen selection: {name}")
    elif output.exists() and any(output.iterdir()):
        raise ValueError("Existing recordings lack frozen collection identity")
    if verify_only and prior_settings is None:
        raise ValueError("Verification requires an existing frozen collection")
    if follow_below_gate:
        if any(path.is_file() for path in (output / "follow").rglob("*")):
            raise ValueError("The additive override cannot migrate existing ordinary follow recordings")
        if (prior_settings is None or prior_settings.get("schema_version") != 2) and any(
                path.is_file() for path in (output / FOLLOW_BELOW_GATE).rglob("*")):
            raise ValueError("Existing follow_below_gate recordings lack their frozen authorization policy")
    if prior_settings is None and not v5(options):
        # Fresh collections have no old selections to preserve.
        prepare_selections(options.task)
    source_follow_sha256 = (campaign.binary_digest(options.task / "recording-input/follow.jsonl")
                            if follow_below_gate else None)
    identity = collection_settings(base_identity, prior_settings, follow_below_gate=follow_below_gate,
                                   source_follow_sha256=source_follow_sha256, upgrade_v2_memory=upgrade_v2_memory)
    families = (*FAMILIES, *((FOLLOW_BELOW_GATE,) if follow_below_gate else ()), *(("water",) if v5(options) else ()))
    if prior_settings is None:
        selections = prepare_v5_selections(options.task) if v5(options) else prepare_selections(options.task, follow_below_gate)
    else:
        # Completed IDs refer to these frozen candidate lists. Do not regenerate
        # old seeds, qualification or route parameters while resuming.
        selections = {name: list(v3.rows(options.task / "recording-input" / f"{name}.jsonl"))
                      for name in families}
        if any(value.get("split") != "train" for values in selections.values() for value in values):
            raise ValueError("Held-out scenario entered a recording selection")
    allowed = {root / directory_for(options.task, value).relative_to(output)
               for root in recording_roots(options) for values in selections.values() for value in values}
    if any(path.parent not in allowed or path.is_symlink()
           for root in recording_roots(options) for path in root.rglob("samples.bin*")):
        raise ValueError("Recording exists outside frozen train selections")
    state = CollectionState(state_path, identity, prior_state, getattr(options, "archive_root", None))
    state.update(status="verifying_existing" if verify_only else "running")
    if v5(options):
        archive_v2_memory3(options, selections, state)
    budget = StorageBudget(output, options.max_bytes, archive_root=getattr(options, "archive_root", None),
                           min_free_bytes=V5_MIN_FREE_BYTES if v5(options) else 256 * 1024**2)
    workers = [Worker(options, index, budget, identity, state) for index in range(options.parallel)]
    episodes, done = [], set()
    new = 0
    verified = 0
    stop_reason = None
    scheduled_points = []
    def metrics():
        return dict(summarize(episodes, selections, include_follow, budget.used(), follow_below_gate, point_limit,
                              state.rejections(), len(scheduled_points), 5 if v5(options) else 4),
                    resume_verified_episodes=verified)

    try:
        # Rejected setup candidates have diagnostics rather than NAVDEMO
        # episodes. Their durable terminal IDs are never dispatched again.
        by_directory = {str(directory_for(options.task, value).relative_to(output)): value
                        for values in selections.values() for value in values}
        for rejected in state.rejections():
            candidate = by_directory.get(state.key(rejected["directory"]))
            directory = resolve_directory(options, candidate) if candidate is not None else Path(rejected["directory"])
            if (candidate is None or rejected.get("id") != candidate["id"]
                    or rejected.get("family") != family(candidate)
                    or rejected.get("candidate_rejected") is not True
                    or rejected.get("recorded") is not False or rejected.get("training_eligible") is not False):
                raise ValueError(f"Rejected candidate is outside its frozen selection: {directory}")
            require_no_recording_output(directory)
            done.add((family(candidate), candidate["id"]))
        rejected_ids = set(done)
        # Verify prior attempts first; partial output is never automatically rerun.
        existing = []
        for name in families:
            expected = {root / directory_for(options.task, value).relative_to(output)
                        for root in recording_roots(options) for value in selections[name]}
            for root in recording_roots(options):
                for directory in (root / name / "train").glob("*"):
                    if directory not in expected or directory.is_symlink():
                        raise ValueError(f"Recording outside frozen train selection: {directory}")
            for value in selections[name]:
                directory = resolve_directory(options, value)
                if (name, value["id"]) in rejected_ids:
                    continue
                if directory.exists():
                    if name == "follow" and not include_follow:
                        raise ValueError("Follow recordings exist despite a closed follow gate")
                    existing.append(value)
        if expect_existing is not None and len(existing) != expect_existing:
            raise ValueError(f"Expected {expect_existing} existing episodes, found {len(existing)}")
        # Validate the cap against every already attempted candidate before
        # dispatch. Recorded failures count as attempts.
        scheduled_points = point_schedule(selections["point"],
            {(family(value), value["id"]) for value in existing}, point_limit, rejected_ids)
        campaign.emit("collection_resume_started", episodes=len(existing), parallel=options.parallel)
        with ThreadPoolExecutor(max_workers=options.parallel) as executor:
            for offset in range(0, len(existing), options.parallel):
                if time.monotonic() >= deadline:
                    state.update(status="resume_verification_incomplete")
                    campaign.emit("collection_resume_bounded", verified_episodes=len(episodes),
                                  total_episodes=len(existing), new_episodes=0)
                    return dict(complete=False, resume_verification_complete=False,
                                verified_episodes=len(episodes), total_episodes=len(existing), new_episodes=0)
                batch = existing[offset:offset + options.parallel]
                futures = [executor.submit(worker.trial, value) for worker, value in zip(workers, batch)]
                measured, completed, fatal = [], [], None
                for value, future in zip(batch, futures):
                    try:
                        measured.append(future.result())
                        completed.append(value)
                    except Exception as error:
                        fatal = fatal or error
                episodes.extend(measured)
                verified += len(measured)
                done.update((family(value), value["id"]) for value in completed)
                if fatal is not None:
                    raise fatal
                campaign.emit("collection_resume_progress", verified_episodes=len(episodes), total_episodes=len(existing))
            campaign.emit("collection_resume_complete", episodes=len(existing))
            while not verify_only and stop_reason is None:
                progressed = False
                for name in families:
                    result = metrics()
                    if result["by_family"][name]["complete"]:
                        continue
                    candidates = scheduled_points if name == "point" else selections[name]
                    pending = [value for value in candidates if (name, value["id"]) not in done]
                    # Round-robin families prevent long road collections starving other sets.
                    for offset in range(0, min(len(pending), options.round_size), options.parallel):
                        if time.monotonic() + 900 >= deadline or new >= options.max_new:
                            stop_reason = "wall_limit" if time.monotonic() + 900 >= deadline else "episode_limit"
                            break
                        batch = pending[offset:min(offset + options.parallel, options.max_new - new + offset)]
                        if not batch:
                            break
                        if not budget.can_reserve_batch(options.limit_ms, len(batch), jump=name == "jump"):
                            stop_reason = "storage_limit"
                            break
                        futures = [executor.submit(worker.trial, value) for worker, value in zip(workers, batch)]
                        # Always drain and preserve all concurrently started attempts.
                        measured, completed, fatal = [], [], None
                        for value, future in zip(batch, futures):
                            try:
                                outcome = future.result()
                                if not outcome.get("candidate_rejected", False):
                                    measured.append(outcome)
                                completed.append(value)
                            except StorageLimit:
                                stop_reason = "storage_limit"
                            except Exception as error:
                                fatal = fatal or error
                        episodes.extend(measured)
                        done.update((family(value), value["id"]) for value in completed)
                        new += len(measured)
                        # Point rejections may require another frozen candidate
                        # from the same region to fill its original quota.
                        if name == "point":
                            rejected_ids = {(value["family"], value["id"]) for value in state.rejections()}
                            scheduled_points = point_schedule(selections["point"], done - rejected_ids,
                                                              point_limit, rejected_ids)
                        progressed = bool(completed) or progressed
                        result = metrics()
                        atomic_json(output / "collection-metrics.json", dict(result, settings=identity))
                        campaign.emit("collection_progress", family=name, new_episodes=new,
                                      by_family=result["by_family"], bytes=budget.used())
                        if fatal is not None:
                            raise fatal
                        if stop_reason is not None or result["by_family"][name]["complete"]:
                            break
                    if stop_reason is not None:
                        break
                if not progressed or metrics()["complete"]:
                    break
    finally:
        for worker in workers:
            worker.close()
    if (settings(options, include_follow) != base_identity
            or (follow_below_gate and campaign.binary_digest(options.task / "recording-input/follow.jsonl") != source_follow_sha256)):
        raise ValueError("Frozen recording inputs or binaries changed during the phase")
    result = metrics()
    result["resume_verification_complete"] = True
    result["stop_reason"] = "complete" if result["complete"] else "existing_verified" if verify_only else stop_reason or "candidate_pool_exhausted"
    atomic_json(output / "collection-metrics.json", dict(result, settings=identity))
    state.update(status="existing_verified" if verify_only else
                 "complete" if result["complete"] else "bounded_phase_complete")
    campaign.emit("collection_phase_complete", complete=result["complete"], new_episodes=new,
                  bytes=result["dataset_bytes"], by_family=result["by_family"], stop_reason=result["stop_reason"])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--task", type=Path, required=True)
    parser.add_argument("--corpus-version", type=int, choices=(4, 5), default=4)
    parser.add_argument("--archive-root", type=Path,
                        help="V5 shared corpus root; archived attempts are resumed directly")
    parser.add_argument("--include-follow", choices=("auto", "yes", "no"), default="auto")
    parser.add_argument("--follow-below-gate", action="store_true",
                        help="Explicitly add separate follow_below_gate training data with the authorized 70%% per-episode rule")
    parser.add_argument("--verify-existing-only", action="store_true",
                        help="Validate all existing recordings and refresh the global summary without starting scenarios")
    parser.add_argument("--upgrade-v2-memory", action="store_true",
                        help="Authorize the one-time v5 runner update that records teacher-v2 episodes with memory 4")
    parser.add_argument("--expect-existing", type=int,
                        help="Require this exact number of existing episodes before verification")
    parser.add_argument("--wall-minutes", type=float, default=100)
    parser.add_argument("--max-new", type=int, default=300)
    parser.add_argument("--point-limit", type=int, default=400,
                        help="Road target: 400, or a smaller multiple of six with equal region quotas; prior attempts are retained")
    parser.add_argument("--round-size", type=int, default=12)
    parser.add_argument("--parallel", type=int, choices=(1, 2, 3), default=3)
    parser.add_argument("--limit-ms", type=int, default=240000)
    parser.add_argument("--max-bytes", type=int)
    options = parser.parse_args()
    os.environ.update(module("ryzen_game").game_environment())
    if options.max_bytes is None:
        options.max_bytes = V5_MAX_BYTES if v5(options) else v3.MAX_BYTES
    if not options.task.is_absolute() or options.limit_ms != jumps.LIMIT_MS:
        parser.error("An absolute task and the unchanged 240000-ms collection horizon are required")
    if not 15 < options.wall_minutes <= 110 or min(options.max_new, options.round_size) <= 0:
        parser.error("Wall budget must be in (15,110] minutes; positive episode bounds are required")
    if options.expect_existing is not None and options.expect_existing < 0:
        parser.error("Expected existing episode count cannot be negative")
    if options.point_limit != 400 and (options.point_limit <= 0 or options.point_limit >= 400 or options.point_limit % 6):
        parser.error("Point limit must be 400 or a positive multiple of six below 400")
    if v5(options) and (options.archive_root is None or not options.archive_root.is_absolute() or options.point_limit != 400):
        parser.error("V5 requires an absolute archive root and all 400 road pairs")
    options.task = options.task.resolve()
    try:
        run(options)
    except Exception as exc:
        atomic_json(options.task / "recordings/collection-error.json", dict(error=type(exc).__name__, detail=str(exc)))
        raise
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
