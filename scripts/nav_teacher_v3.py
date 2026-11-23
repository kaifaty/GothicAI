#!/usr/bin/env python3
"""Seeded scenarios, recording and measurements for the memory-only teacher.

Runner commands execute inside a caller-owned gq batch request on Ryzen. This
script never trains a policy. Candidate selection precedes teacher measurement;
object places and moving-target regions have disjoint seeded splits.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from copy import copy
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import random
import re
import shutil
import statistics
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parent.parent
KINDS = ("point", "object", "follow", "approach")
SPLITS = ("train", "validation", "test")
MAX_BYTES = 15 * 1024**3
ESTIMATED_DECISION_BYTES = 20 * 1024
SCENARIO_IDENTITY_FIELDS = ("id", "goal_kind", "split", "region", "place_id", "start", "goal", "seed", "route", "target")


def module(name):
    spec = importlib.util.spec_from_file_location(name, REPO / "scripts" / f"{name}.py")
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def rows(path):
    with Path(path).open(encoding="utf-8") as stream:
        for number, line in enumerate(stream, 1):
            if line.strip():
                try:
                    yield json.loads(line)
                except ValueError as exc:
                    raise ValueError(f"Invalid JSON: {path}:{number}") from exc


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def write_selection(path, values):
    """Never silently change a frozen, seed-specific scenario selection."""
    path = Path(path)
    content = "".join(json.dumps(value, ensure_ascii=False, sort_keys=True) + "\n" for value in values)
    if path.exists() and path.read_text(encoding="utf-8") != content:
        raise ValueError(f"Refusing to replace scenario selection: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def token(value):
    value = str(value)
    if not value or any(c.isspace() for c in value):
        raise ValueError(f"Runner token must be nonempty and have no whitespace: {value!r}")
    return value


def scenario_id(kind, *parts):
    digest = hashlib.sha256("\0".join(map(str, (kind, *parts))).encode()).hexdigest()[:16]
    return f"{kind}-{digest}"


def split_groups(groups, seed):
    """Assign whole places/regions before geometry qualification or measurement."""
    ordered = sorted(set(groups))
    if len(ordered) < 3:
        raise ValueError("Need at least three groups for train/validation/test isolation")
    random.Random(seed).shuffle(ordered)
    validation = max(1, round(len(ordered) * .2))
    test = max(1, round(len(ordered) * .2))
    train = len(ordered) - validation - test
    return {group: SPLITS[0 if index < train else 1 if index < train + validation else 2]
            for index, group in enumerate(ordered)}


def exclusion_pairs(holdout, validation):
    # Both files are mandatory: absence is not evidence of split isolation.
    return module("nav_rl").historical_pairs([holdout, validation])


def legs(scenario):
    if scenario["goal_kind"] == "object":
        return [(scenario["start"], goal) for goal in scenario.get("goal_aliases", [scenario["goal"]])]
    route = [scenario["start"], scenario["goal"]]
    if scenario["goal_kind"] in ("follow", "approach"):
        route = [scenario["start"], *scenario["route"]]
        route.append(scenario["route"][0])
    return list(zip(route, route[1:]))


def isolated(scenario, excluded):
    return all(start != goal and (start, goal) not in excluded and (goal, start) not in excluded
               for start, goal in legs(scenario))


def object_targets(places):
    """Use entrances, actual mills and named gates/passes, not nearby fields."""
    targets = []
    for place in sorted(places, key=lambda p: p["id"]):
        entrances = place.get("entrances", []) if place.get("kind") in ("cave", "house") else []
        for index, entrance in enumerate(entrances):
            if entrance.get("out") and entrance.get("pos"):
                goal = "cm:" + ",".join(format(float(value), ".9g") for value in entrance["pos"])
                aliases = sorted({entrance[field] for field in ("in", "out") if entrance.get(field)})
                targets.append(dict(place_id=place["id"], goal=goal, goal_aliases=aliases,
                                    entrance_in=entrance.get("in"), entrance_out=entrance.get("out"),
                                    target_position=entrance["pos"], target_type=place["kind"], entrance=index))
        identifier = place["id"].upper()
        name = place.get("en", "").lower()
        landmark = (identifier in ("BIGMILL", "FARM1_MILL")
                    or re.search(r"(?:^|_)(?:GATE|PASSAGE|PASS)(?:_|$)", identifier)
                    or re.search(r"^(?:the )?(?:gate|passage|mountain pass)\b", name))
        if landmark and place.get("anchor"):
            targets.append(dict(place_id=place["id"], goal=place["anchor"],
                                target_position=place["center"], target_type="landmark", entrance=-1))
    return targets


def build_candidates(places, training, excluded, seed, starts_per_object=12, moving_per_region=3,
                     point_count=24, object_places=30, start_positions=None):
    targets = object_targets(places)
    place_splits = split_groups([p["place_id"] for p in targets], seed)
    region_splits = split_groups([p["region"] for p in training], seed + 1)
    rng = random.Random(seed)
    result = []
    points = defaultdict(list)
    for pair in sorted(training, key=lambda p: (p["region"], p["start"], p["goal"])):
        candidate = dict(goal_kind="point", split="train", region=pair["region"],
                         start=pair["start"], goal=pair["goal"], seed=seed,
                         id=scenario_id("point", pair["start"], pair["goal"]))
        if isolated(candidate, excluded):
            points[pair["region"]].append(candidate)
    for group in points.values():
        rng.shuffle(group)
    while any(points.values()) and sum(p["goal_kind"] == "point" for p in result) < point_count:
        for region in sorted(points):
            if points[region] and len(result) < point_count:
                result.append(points[region].pop())
    target_places = sorted(place_splits)
    rng.shuffle(target_places)
    chosen_places = set(target_places[:object_places])
    starts = {place["anchor"]: place["center"] for place in places if place.get("anchor") and place.get("center")}
    starts.update(start_positions or {})
    for target in targets:
        if target["place_id"] not in chosen_places:
            continue
        center = target["target_position"]
        nearby = sorted((name for name, pos in starts.items()
                         if name != target["goal"] and 500 <= math.hypot(pos[0] - center[0], pos[2] - center[2]) <= 20000))
        rng.shuffle(nearby)
        # Prefer starts within 5–100 m before using the more distant fallback.
        # Stable sorting retains seeded ordering for equal-distance candidates.
        nearby.sort(key=lambda name: math.hypot(starts[name][0] - center[0], starts[name][2] - center[2]))
        added = 0
        for start in nearby:
            candidate = dict(target, goal_kind="object", start=start, region=target["place_id"],
                             split=place_splits[target["place_id"]], seed=seed,
                             id=scenario_id("object", target["place_id"], target["entrance"], start))
            if isolated(candidate, excluded):
                result.append(candidate)
                added += 1
                if added >= starts_per_object:
                    break
    for region in sorted(region_splits):
        pool = sorted((p for p in training if p["region"] == region), key=lambda p: (p["start"], p["goal"]))
        rng.shuffle(pool)
        count = 0
        for pair in pool:
            # The native target follows actual waynet edges between endpoints;
            # alternating walk/run and turning follow its seeded controller.
            for kind in ("follow", "approach"):
                route = [pair["goal"], pair["start"]]
                if kind == "follow":
                    others = sorted({p[field] for p in pool for field in ("start", "goal")}
                                    - set(route))
                    rng.shuffle(others)
                    third = next((waypoint for waypoint in others
                                  if (route[-1], waypoint) not in excluded and (waypoint, route[-1]) not in excluded
                                  and (waypoint, route[0]) not in excluded and (route[0], waypoint) not in excluded), None)
                    if third is None:
                        continue
                    route.append(third)
                candidate = dict(goal_kind=kind, split=region_splits[region], region=region,
                                 start=pair["start"], goal=pair["goal"], route=route,
                                 target="BAU_954_MALETH" if kind == "follow" else ("WOLF", "SCAVENGER", "SHEEP")[count % 3],
                                 seed=seed + count, id=scenario_id(kind, region, *route, seed))
                if isolated(candidate, excluded):
                    result.append(candidate)
            count += 1
            if count >= moving_per_region:
                break
    # Repeated source rows must not create repeated measurement scenarios.
    unique = {value["id"]: value for value in result}
    return sorted(unique.values(), key=lambda p: (KINDS.index(p["goal_kind"]), p["split"], p["id"]))


def load_start_positions(path):
    """Load named starts from runner point discovery without guessing coordinates."""
    if path is None:
        return {}
    values = read_json(path) if Path(path).suffix == ".json" else list(rows(path))
    if isinstance(values, dict):
        values = [dict(name=name, position_cm=position) for name, position in values.items()]
    result = {}
    for value in values:
        name = value.get("name", value.get("waypoint"))
        position = value.get("position_cm", value.get("pos", value.get("position")))
        if (not isinstance(name, str) or not name or not isinstance(position, (list, tuple))
                or len(position) != 3 or not all(math.isfinite(float(coordinate)) for coordinate in position)):
            raise ValueError(f"Invalid named start position in {path}: {value}")
        result[name] = [float(coordinate) for coordinate in position]
    return result


def candidates(args):
    excluded = exclusion_pairs(args.holdout, args.ppo_validation)
    training = list(rows(args.training_pairs))
    result = build_candidates(read_json(args.roads)["places"], training, excluded, args.seed,
                              args.starts_per_object, args.moving_per_region, args.point_count, args.object_places,
                              load_start_positions(getattr(args, "start_points", None)))
    write_selection(args.selection_output or args.output / "candidates.jsonl", result)
    print(json.dumps(dict(candidates=len(result), by_kind=dict(Counter(p["goal_kind"] for p in result)))))


def beyond_path_fraction(directory, demos=None):
    """Path length, not decision count; preserve the preceding chunk's label."""
    import numpy as np
    demos = demos or module("nav_demos")
    if demos.episode_version(directory) != 2:
        raise ValueError(f"Point qualification needs NAVDEMO v2: {directory}")
    total = beyond = 0.0
    previous = None
    for data in demos.iter_chunks(directory):
        for record in data:
            if previous is not None:
                distance = float(np.linalg.norm(record["position_cm"][[0, 2]] - previous["position_cm"][[0, 2]]))
                total += distance
                beyond += distance * bool(previous["roads"]["beyond_known"])
            previous = record.copy()
    # Include the last held action using the final trajectory position if present.
    trajectory = Path(directory) / "trajectory.txt"
    if previous is not None and trajectory.is_file():
        end = np.loadtxt(trajectory, ndmin=2)[-1, [1, 3]]
        distance = float(np.linalg.norm(end - previous["position_cm"][[0, 2]]))
        total += distance
        beyond += distance * bool(previous["roads"]["beyond_known"])
    return beyond / total if total else 0.0


def references(root):
    result = {}
    if root is not None and Path(root).exists():
        for metadata in sorted(Path(root).rglob("episode.json")):
            episode = read_json(metadata)
            if episode.get("teacher_version") != 2 or not (metadata.parent / "samples.bin.gz").is_file():
                continue
            if "start" in episode and "goal" in episode and not episode.get("drive") and episode.get("dagger") is None:
                key = episode["start"], episode["goal"]
                result.setdefault(key, metadata.parent)
                if episode.get("id"):
                    result.setdefault(episode["id"], metadata.parent)
    return result


def object_qualification_reason(probe):
    if probe is None:
        return "missing_probe"
    if probe.get("start_supported") is not True:
        return "unsupported_start"
    if probe.get("visible") is not True:
        return "not_visible"
    if probe.get("detour_complete") is not True:
        return "no_detour"
    direct = float(probe.get("direct_cm", 0))
    detour = float(probe.get("detour_length_cm", probe.get("detour_cm", 0)))
    if not math.isfinite(direct) or not math.isfinite(detour) or direct <= 0 or detour < 0:
        return "invalid_distance"
    if detour < 1.5 * direct and probe.get("straight_blocked") is not True:
        return "detour_too_short"
    return None


def object_qualified(probe):
    return object_qualification_reason(probe) is None


def object_visibility(probe):
    if probe is None:
        return "missing_probe"
    if probe.get("visible_centre") is True:
        return "visible_centre"
    if probe.get("visible") is True and probe.get("visible_edge") is True:
        return "visible_edge_only"
    return "legacy_visible" if probe.get("visible") is True else "not_visible"


def qualification_gate(selected, *, minimum_points=30, minimum_objects=30, required_point_regions=(),
                       minimum_points_per_region=1, minimum_object_places=0):
    """Reject undersized comparisons before any v3 result is used to select data."""
    points = [value for value in selected if value["goal_kind"] == "point"]
    objects = [value for value in selected if value["goal_kind"] == "object"]
    regions = Counter(value["region"] for value in points)
    places = Counter(value["place_id"] for value in objects)
    underfilled = {region: regions[region] for region in sorted(set(required_point_regions))
                   if regions[region] < minimum_points_per_region}
    counts = dict(points=len(points), objects=len(objects), point_regions=dict(sorted(regions.items())),
                  object_places=len(places), underfilled_point_regions=underfilled)
    if (len(points) < minimum_points or len(objects) < minimum_objects or underfilled
            or len(places) < minimum_object_places):
        raise ValueError(f"Qualification gate: points={len(points)}/{minimum_points}, objects={len(objects)}/{minimum_objects}, "
                         f"object_places={len(places)}/{minimum_object_places}, point_regions={dict(sorted(regions.items()))}, "
                         f"underfilled_regions={underfilled}, required_per_region={minimum_points_per_region}")
    return counts


def qualified_candidates(pool, probes, refs, *, minimum_points=0, minimum_objects=0, required_point_regions=(),
                         minimum_points_per_region=1, minimum_object_places=0):
    """Qualify only v2-successful routes; input selections and episode files stay unchanged."""
    result = []
    candidates_by_kind = Counter()
    accepted_by_kind = Counter()
    rejections = defaultdict(Counter)
    v2_failures = Counter()
    visibility = Counter()
    qualified_visibility = Counter()
    seen = set()
    for value in pool:
        kind = value["goal_kind"]
        candidates_by_kind[kind] += 1
        if value["id"] in seen:
            raise ValueError(f"Duplicate candidate id: {value['id']}")
        seen.add(value["id"])
        selected = dict(value)
        if kind == "object":
            probe = probes.get(value["id"])
            visibility[object_visibility(probe)] += 1
            reason = object_qualification_reason(probe)
            if reason is not None:
                rejections[kind][reason] += 1
                continue
            if (probe.get("start"), probe.get("goal")) != (value["start"], value["goal"]):
                raise ValueError(f"Probe label/route mismatch: {value['id']}")
            selected.update(qualification=probe)
        if kind in ("point", "object"):
            reference = refs.get(value["id"], refs.get((value["start"], value["goal"])))
            if reference is None:
                rejections[kind]["missing_v2"] += 1
                continue
            metadata = read_json(Path(reference) / "episode.json")
            if (metadata.get("teacher_version") != 2 or metadata.get("drive") or metadata.get("dagger") is not None
                    or metadata.get("goal_kind", kind) != kind
                    or (metadata.get("start"), metadata.get("goal")) != (value["start"], value["goal"])):
                raise ValueError(f"Mismatched v2 reference for {value['id']}: {reference}")
            if metadata.get("success") is not True:
                rejections[kind]["v2_failed"] += 1
                v2_failures[kind] += 1
                continue
            selected.update(teacher_v2_episode=str(reference), teacher_v2_success=True)
            if kind == "point":
                cached = metadata.get("beyond_path_fraction")
                fraction = (float(cached) if isinstance(cached, (float, int)) and not isinstance(cached, bool)
                            and math.isfinite(cached) else beyond_path_fraction(reference))
                if not 0 <= fraction <= 1:
                    raise ValueError(f"Invalid beyond-known fraction in {reference}: {fraction}")
                if fraction <= .3:
                    rejections[kind]["beyond_at_most_30pct"] += 1
                    continue
                selected["beyond_path_fraction"] = fraction
        result.append(selected)
        accepted_by_kind[kind] += 1
        if kind == "object":
            qualified_visibility[object_visibility(probe)] += 1
    validate_splits(result)
    qualification_gate(result, minimum_points=minimum_points, minimum_objects=minimum_objects,
                       required_point_regions=required_point_regions, minimum_points_per_region=minimum_points_per_region,
                       minimum_object_places=minimum_object_places)
    counters = dict(candidates=dict(candidates_by_kind), qualified=dict(accepted_by_kind),
                    rejections={kind: dict(counts) for kind, counts in sorted(rejections.items())},
                    v2_failures=dict(v2_failures),
                    object_visibility=dict(visibility), qualified_object_visibility=dict(qualified_visibility),
                    point_regions=dict(sorted(Counter(value["region"] for value in result if value["goal_kind"] == "point").items())),
                    object_places=dict(sorted(Counter(value["place_id"] for value in result if value["goal_kind"] == "object").items())),
                    object_regions=dict(sorted(Counter(value["region"] for value in result if value["goal_kind"] == "object").items())))
    return result, counters


def qualify(args):
    excluded = exclusion_pairs(args.holdout, args.ppo_validation)
    pool = list(rows(args.scenarios or args.output / "candidates.jsonl"))
    for value in pool:
        if not isolated(value, excluded):
            raise ValueError(f"Excluded route in candidate selection: {value['id']}")
    probes = {p.get("label", p.get("id")): p for p in rows(args.probes)} if args.probes else {}
    required_regions = tuple(region for region in args.require_point_regions.split(",") if region)
    result, counters = qualified_candidates(pool, probes, references(args.teacher_root), minimum_points=args.minimum_points,
                                           minimum_objects=args.minimum_objects, required_point_regions=required_regions,
                                           minimum_points_per_region=args.minimum_points_per_region,
                                           minimum_object_places=args.minimum_object_places)
    write_selection(args.selection_output or args.output / "scenarios.jsonl", result)
    print(json.dumps(counters))


def same_scenario(metadata, value, version):
    """Qualification enriches a dataset, without changing a recorded episode's identity."""
    return (metadata.get("teacher_version") == version and metadata.get("demo_version") == 2
            and all(metadata.get(key) == value.get(key) for key in SCENARIO_IDENTITY_FIELDS))


def validate_splits(values):
    seen = {}
    for value in values:
        kind = value["goal_kind"]
        if kind == "object":
            key = "object", value["place_id"]
        elif kind in ("follow", "approach"):
            key = "moving", value["region"]
        else:
            continue
        if key in seen and seen[key] != value["split"]:
            raise ValueError(f"Split overlap for {key}: {seen[key]}, {value['split']}")
        seen[key] = value["split"]


def disk_guard(output, limit_ms, max_bytes=MAX_BYTES):
    if not 0 < max_bytes <= MAX_BYTES:
        raise ValueError("Disk budget must be positive and at most 15 GiB")
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    used = sum(path.stat().st_size for path in output.rglob("*") if path.is_file())
    reserve = math.ceil(limit_ms / 100) * ESTIMATED_DECISION_BYTES
    free = shutil.disk_usage(output).free
    if used + reserve > max_bytes or free < reserve + 256 * 1024**2:
        raise ValueError(f"Disk budget: used={used}, next_estimate={reserve}, limit={max_bytes}, free={free}")
    return used


def checked_paths(args):
    if sys.platform != "linux":
        raise ValueError("Run teacher measurements on Ryzen/Linux inside gq batch")
    os.environ.update(module("ryzen_game").game_environment(assets=args.assets))
    for name in ("runner", "assets", "save", "cache", "links", "roads"):
        value = getattr(args, name)
        if value is None:
            raise ValueError(f"--{name} is required")
        value = Path(value).resolve()
        token(value)
        if name != "cache" and not value.exists():
            raise ValueError(f"--{name} must exist: {value}")
        setattr(args, name, value)
    args.cache.mkdir(parents=True, exist_ok=True)
    args.output = args.output.resolve()
    token(args.output)
    args.output.mkdir(parents=True, exist_ok=True)
    os.environ["OPENGOTHIC_BODY_TRACE"] = "0"


def new_runner(args):
    collector = module("collect_nav_demos")
    worker = copy(args)
    worker.ini = getattr(args, "ini", None)
    worker.output = args.output / "runner-work"
    worker.output.mkdir(parents=True, exist_ok=True)
    prepare = collector.scenario_file(args, "v3-prepare", [f"navmesh build {args.cache} {args.links}"])
    runner = collector.runner_type()(worker, prepare)
    try:
        runner.scenario(prepare, worker.output / "prepare", args.seed, timeout=720)
        collector.check_log(worker.output / "prepare")
    except BaseException:
        runner.close()
        raise
    return runner


def runner_scenario(args, runner, name, commands, directory, seed):
    collector = module("collect_nav_demos")
    seeded = copy(args)
    seeded.seed = seed
    scenario = collector.scenario_file(seeded, name, commands)
    runner.scenario(scenario, directory, seed, timeout=args.watchdog_seconds)
    collector.check_log(directory)


def chosen_scenarios(args):
    source = args.scenarios or args.output / ("candidates.jsonl" if args.command == "discovery" else "scenarios.jsonl")
    values = list(rows(source))
    validate_splits(values)
    selected = [p for p in values if (args.goal_kind == "all" or p["goal_kind"] == args.goal_kind)
                and (args.split == "all" or p["split"] == args.split)]
    selected = selected[args.start_index:]
    if args.count is not None:
        selected = selected[:args.count]
    for value in selected:
        for field in ("id", "start", "goal"):
            token(value[field])
        if value["goal_kind"] in ("follow", "approach"):
            token(value["target"])
            for waypoint in value["route"]:
                token(waypoint)
    return selected


def discovery(args):
    if args.scenarios is None:
        args.scenarios = args.output / "candidates.jsonl"
    selected = [p for p in chosen_scenarios(args) if p["goal_kind"] == "object"]
    if not selected:
        raise ValueError("No object candidates selected")
    checked_paths(args)
    probe_file = args.output / "object-probes.txt"
    probe_file.write_text("".join(probe_row(p) + "\n" for p in selected), encoding="utf-8")
    runner = new_runner(args)
    try:
        runner_scenario(args, runner, "object-probes", [f"navdemos probes-v3 {args.npc} {probe_file}"],
                        args.output / "discovery", args.seed)
    finally:
        runner.close()


def probe_row(value):
    fields = [value["id"], value["start"], value["goal"]]
    if value.get("entrance_in") and value.get("entrance_out"):
        fields.extend((value["entrance_in"], value["entrance_out"]))
    return " ".join(token(field) for field in fields)


def start_command(value, version, npc, limit_ms):
    kind, start, goal = value["goal_kind"], token(value["start"]), token(value["goal"])
    if kind == "point":
        return f"navdemos start-v{version} {npc} {start} {goal} {limit_ms}"
    if kind == "object":
        return f"navdemos object-v3 {version} {npc} {start} {goal} {limit_ms}"
    route = ",".join(token(waypoint) for waypoint in value["route"])
    return (f"navdemos moving-v3 {version} {npc} {start} {token(value['target'])} {route} "
            f"{kind} {limit_ms} {value['seed']}")


def episode_directory(output, value, version):
    return Path(output) / f"v{version}" / value["goal_kind"] / value["split"] / value["id"]


def complete(directory):
    return all((directory / name).is_file() for name in ("episode.json", "samples.bin.gz", "actions.txt", "trajectory.txt"))


def replay_contract(binary, directory):
    process = subprocess.run([str(Path(binary).resolve()), str((Path(directory) / "samples.bin.gz").resolve())],
                             check=False, capture_output=True, text=True)
    if process.returncode:
        raise ValueError(f"Teacher contract failed in {directory}: {process.stdout.strip()} {process.stderr.strip()}")
    result = json.loads(process.stdout)
    if result.get("mismatches") != 0 or result.get("previous_mismatches") != 0:
        raise ValueError(f"Teacher contract mismatch: {directory}: {result}")
    if not isinstance(result.get("records"), int) or result["records"] <= 0:
        raise ValueError(f"Empty contract replay: {directory}")
    write_json(Path(directory) / "contract.json", result)
    return result


def run(args):
    selected = chosen_scenarios(args)
    if not selected:
        raise ValueError("No teacher scenarios selected")
    checked_paths(args)
    if args.teacher_version == 3 and (args.contract is None or not args.contract.is_file()):
        raise ValueError("--contract executable is required for v3 recordings")
    runner = new_runner(args)
    demos = module("nav_demos")
    try:
        for value in selected:
            directory = episode_directory(args.output, value, args.teacher_version)
            if complete(directory):
                metadata = read_json(directory / "episode.json")
                if not same_scenario(metadata, value, args.teacher_version):
                    raise ValueError(f"Existing recording belongs to another scenario: {directory}")
                if not args.resume:
                    raise ValueError(f"Recording exists; use --resume: {directory}")
                demos.summarize(directory, validate=True)
                if args.teacher_version == 3:
                    contract_file = directory / "contract.json"
                    contract = read_json(contract_file) if contract_file.is_file() else {}
                    if (contract.get("records") != metadata["decisions"] or contract.get("mismatches") != 0
                            or contract.get("previous_mismatches") != 0):
                        replay_contract(args.contract, directory)
                continue
            disk_guard(args.output, args.limit_ms, args.max_bytes)
            commands = [f"roads load {args.roads}", f"roads on {args.npc}",
                        start_command(value, args.teacher_version, args.npc, args.limit_ms), "navdemos wait"]
            started = time.monotonic()
            runner_scenario(args, runner, f"v{args.teacher_version}-{value['id']}", commands, directory, value["seed"])
            if not complete(directory):
                raise ValueError(f"Incomplete teacher recording: {directory}")
            metadata = read_json(directory / "episode.json")
            if metadata.get("teacher_version") != args.teacher_version or metadata.get("demo_version") != 2:
                raise ValueError(f"Wrong teacher/NAVDEMO version: {directory}")
            metadata.update(value, limit_ms=args.limit_ms, collector_wall_s=time.monotonic() - started)
            write_json(directory / "episode.json", metadata)
            demos.summarize(directory, validate=True)
            if args.teacher_version == 3:
                replay_contract(args.contract, directory)
            used = sum(p.stat().st_size for p in args.output.rglob("*") if p.is_file())
            if used > args.max_bytes:
                raise ValueError(f"Recording disk budget exceeded: {used} > {args.max_bytes}")
            print(json.dumps(dict(id=value["id"], teacher=args.teacher_version, success=metadata["success"],
                                  decisions=metadata["decisions"], bytes=used)), flush=True)
    finally:
        runner.close()


def follow_metrics(times, distances, elapsed_ms):
    """Time-weighted occupancy after first <=5m contact, including lost contact."""
    import numpy as np
    times, distances = np.asarray(times, dtype=float), np.asarray(distances, dtype=float)
    if len(times) != len(distances) or not len(times) or not np.isfinite(times).all() or not np.isfinite(distances).all():
        raise ValueError("Invalid target trace")
    if times[0] != 0 or (np.diff(times) <= 0).any() or times[-1] > elapsed_ms or (distances < 0).any():
        raise ValueError("Invalid target trace times/distances")
    contacts = np.flatnonzero(distances <= 500)
    if not len(contacts):
        return dict(first_contact_ms=None, follow_observed_ms=0, follow_in_band_ms=0, follow_fraction=None)
    first = int(contacts[0])
    intervals = np.diff(np.r_[times, elapsed_ms])[first:]
    duration = float(intervals.sum())
    in_band = float(intervals[(distances[first:] >= 200) & (distances[first:] <= 500)].sum())
    return dict(first_contact_ms=float(times[first]), follow_observed_ms=duration,
                follow_in_band_ms=in_band, follow_fraction=in_band / duration if duration else None)


def episode_metrics(directory, demos=None):
    import numpy as np
    directory = Path(directory)
    demos = demos or module("nav_demos")
    metadata = read_json(directory / "episode.json")
    demos.summarize(directory, validate=True)
    actions = np.loadtxt(directory / "actions.txt", ndmin=2)[:-1]
    trajectory = np.loadtxt(directory / "trajectory.txt", ndmin=2)
    length = float(np.linalg.norm(np.diff(trajectory[:, [1, 3]], axis=0), axis=1).sum())
    detour = metadata.get("detour_length_cm")
    result = {name: metadata.get(name) for name in ("id", "goal_kind", "split", "region", "place_id", "start", "goal",
                                                  "teacher_version", "success", "elapsed_ms", "decisions", "blocked_spans",
                                                  "blocked_ms", "max_blocked_ms", "failure")}
    result.update(episode=str(directory), travelled_cm=length, detour_length_cm=detour,
                  length_over_detour=length / detour if detour and detour > 0 else None,
                  backward=int(np.count_nonzero(actions[:, 1] < 0)), strafe=int(np.count_nonzero(actions[:, 2] != 0)),
                  strafe_left=int(np.count_nonzero(actions[:, 2] < 0)), strafe_right=int(np.count_nonzero(actions[:, 2] > 0)))
    if result["goal_kind"] in ("follow", "approach"):
        trace = np.loadtxt(directory / "target-trajectory.txt", ndmin=2)
        if trace.shape[1] < 5:
            raise ValueError(f"Expected time_ms,x,y,z,distance_cm target trace: {directory}")
        moving = follow_metrics(trace[:, 0], trace[:, 4], metadata["elapsed_ms"])
        result.update(moving)
        result["approach_success"] = bool((trace[:, 4] <= 200).any()
                                           or (metadata["success"] and metadata.get("distance_cm", math.inf) <= 200))
    if result["teacher_version"] == 3:
        contract = read_json(directory / "contract.json")
        if contract.get("mismatches") != 0 or contract.get("previous_mismatches") != 0 or contract.get("records") != result["decisions"]:
            raise ValueError(f"Invalid or incomplete contract replay: {directory}")
        result["contract_records"] = contract["records"]
    return result


def average(values):
    numbers = [value for value in values if value is not None]
    return statistics.mean(numbers) if numbers else None


def group_metrics(episodes):
    decisions = sum(e["decisions"] for e in episodes)
    counts = {field: sum(e[field] for e in episodes) for field in ("backward", "strafe")}
    observed = sum(e.get("follow_observed_ms", 0) for e in episodes)
    band = sum(e.get("follow_in_band_ms", 0) for e in episodes)
    return dict(episodes=len(episodes), successes=sum(bool(e["success"]) for e in episodes),
                success_fraction=average([bool(e["success"]) for e in episodes]), decisions=decisions,
                mean_elapsed_ms=average([e["elapsed_ms"] for e in episodes]),
                mean_travelled_cm=average([e["travelled_cm"] for e in episodes]),
                mean_length_over_detour=average([e["length_over_detour"] for e in episodes]),
                blocked_spans=sum(e.get("blocked_spans") or 0 for e in episodes),
                blocked_ms=sum(e.get("blocked_ms") or 0 for e in episodes), **counts,
                backward_fraction=counts["backward"] / decisions if decisions else None,
                strafe_fraction=counts["strafe"] / decisions if decisions else None,
                follow_fraction=band / observed if observed else None,
                follow_first_contacts=sum(e.get("first_contact_ms") is not None for e in episodes),
                approach_success_fraction=average([e["approach_success"] for e in episodes if e["goal_kind"] == "approach"]),
                contract_records=sum(e.get("contract_records", 0) for e in episodes))


def comparison_metrics(episodes):
    refs = {e["id"]: e for e in episodes if e["teacher_version"] == 2}
    trials = {e["id"]: e for e in episodes if e["teacher_version"] == 3}
    eligible = [key for key in refs.keys() & trials.keys() if refs[key]["success"]]
    recovered = [key for key in eligible if trials[key]["success"]]
    return dict(paired=len(refs.keys() & trials.keys()), v2_successes=len(eligible), v3_successes=len(recovered),
                conditional_success_fraction=len(recovered) / len(eligible) if eligible else None,
                mean_elapsed_ratio_on_joint_success=average([trials[k]["elapsed_ms"] / refs[k]["elapsed_ms"]
                                                            for k in recovered if refs[k]["elapsed_ms"] > 0]),
                mean_length_ratio_on_joint_success=average([trials[k]["travelled_cm"] / refs[k]["travelled_cm"]
                                                           for k in recovered if refs[k]["travelled_cm"] > 0]))


def summarize(args):
    demos = module("nav_demos")
    episodes = []
    for version in (2, 3):
        for metadata in sorted((args.output / f"v{version}").rglob("episode.json")):
            episodes.append(episode_metrics(metadata.parent, demos))
    if not episodes:
        raise ValueError("No teacher recordings")
    groups = {}
    comparisons = {}
    for kind in KINDS:
        selected = [e for e in episodes if e["goal_kind"] == kind]
        groups[kind] = {f"v{version}": group_metrics([e for e in selected if e["teacher_version"] == version]) for version in (2, 3)}
        comparisons[kind] = comparison_metrics(selected)
    checks = {f"contract_{kind}": groups[kind]["v3"]["contract_records"] >= 1000 for kind in KINDS}
    checks.update({f"comparison_{kind}": (comparisons[kind]["conditional_success_fraction"] or 0) >= .8 for kind in ("point", "object")})
    follow = groups["follow"]["v3"]
    checks["follow"] = ((follow["follow_fraction"] or 0) >= .8 and follow["follow_first_contacts"] == follow["episodes"] and follow["episodes"] > 0)
    checks["approach"] = (groups["approach"]["v3"]["approach_success_fraction"] or 0) >= .8
    point_rate = comparisons["point"]["conditional_success_fraction"]
    result = dict(by_kind=groups, comparisons=comparisons, checks=checks, all_passed=all(checks.values()),
                  stop_required=point_rate is not None and point_rate < .5, episodes=episodes)
    write_json(args.output / "metrics.json", result)
    print(json.dumps({key: value for key, value in result.items() if key != "episodes"}, ensure_ascii=False))
    if result["stop_required"]:
        raise ValueError(f"STOP: v3 reaches {comparisons['point']['v3_successes']}/{comparisons['point']['v2_successes']} v2-successful beyond-known routes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("candidates", "discovery", "qualify", "run", "summarize"))
    parser.add_argument("--output", type=Path, default=Path.home() / "gothic-ai/nav-demos-v3")
    parser.add_argument("--training-pairs", type=Path, default=Path.home() / "gothic-ai/nav-ppo/training-pairs/pairs.jsonl")
    parser.add_argument("--holdout", type=Path, default=REPO / "reports/navigation/2026-10-18-rl-env/holdout-cv.jsonl")
    parser.add_argument("--ppo-validation", type=Path, default=Path.home() / "gothic-ai/nav-ppo/validation/holdout.jsonl")
    parser.add_argument("--teacher-root", type=Path, help="Ordinary v2 recordings for training pair beyond-known qualification")
    parser.add_argument("--scenarios", type=Path)
    parser.add_argument("--selection-output", type=Path, help="Optional named pilot selection; existing selections stay frozen")
    parser.add_argument("--probes", type=Path, help="discovery/object-candidates.jsonl")
    parser.add_argument("--start-points", type=Path, help="JSON/JSONL discovered waypoint coordinates for object starts")
    parser.add_argument("--minimum-points", type=int, default=0)
    parser.add_argument("--minimum-objects", type=int, default=0)
    parser.add_argument("--minimum-object-places", type=int, default=0)
    parser.add_argument("--minimum-points-per-region", type=int, default=1)
    parser.add_argument("--require-point-regions", default="", help="Comma-separated required beyond-known point regions")
    parser.add_argument("--contract", type=Path)
    parser.add_argument("--roads", type=Path, default=REPO / "server/src/gothic_ai/gamedata/places.json")
    parser.add_argument("--links", type=Path, default=REPO / "reports/navigation/2026-10-16-teacher/raw/accepted-links.txt")
    for name in ("runner", "assets", "save", "cache"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--npc", default="BAU_954_MALETH")
    parser.add_argument("--seed", type=int, default=19102026)
    parser.add_argument("--teacher-version", choices=(2, 3), type=int, default=3)
    parser.add_argument("--goal-kind", choices=("all", *KINDS), default="all")
    parser.add_argument("--split", choices=("all", *SPLITS), default="all")
    parser.add_argument("--starts-per-object", type=int, default=12)
    parser.add_argument("--object-places", type=int, default=30)
    parser.add_argument("--point-count", type=int, default=24)
    parser.add_argument("--moving-per-region", type=int, default=3)
    parser.add_argument("--start-index", type=int, default=0)
    parser.add_argument("--count", type=int)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--limit-ms", type=int, default=240000)
    parser.add_argument("--max-bytes", type=int, default=MAX_BYTES)
    parser.add_argument("--watchdog-seconds", type=float, default=900)
    parser.add_argument("--physics", choices=("cv", "native"), default="cv")
    args = parser.parse_args()
    if args.limit_ms <= 0 or args.start_index < 0 or (args.count is not None and args.count < 1):
        parser.error("Positive --limit-ms/--count and nonnegative --start-index are required")
    if min(args.starts_per_object, args.moving_per_region, args.object_places, args.point_count) < 1:
        parser.error("Candidate counts must be positive")
    if min(args.minimum_points, args.minimum_objects, args.minimum_object_places) < 0 or args.minimum_points_per_region < 1:
        parser.error("Qualification minima must be nonnegative and per-region minimum positive")
    token(args.npc)
    globals()[args.command](args)


if __name__ == "__main__":
    main()
