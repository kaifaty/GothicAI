#!/usr/bin/env python3
"""Freeze train-only live-water routes inside one caller-owned gq batch.

Physical probes prepare scenarios only. They never supply observations, labels
or hints to the ordinary v3b teacher, and this module records no NAVDEMO data.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
from types import SimpleNamespace


SEED = 22102026
MINIMUM_CANDIDATES = 40
REGIONS = {
    "harbour": ("NW_CITY_HABOUR_06_B", "NW_CITY_HABOUR", 6000),
    "market": ("NW_CITY_MERCHANT_PATH_20", "NW_CITY_MERCHANT", 5000),
    "port_streets": ("NW_CITY_HABOUR_POOR_AREA_HUT_03", "NW_CITY_HABOUR_POOR", 3500),
    "upper_streets": ("NW_CITY_UPTOWN_PATH_13", "NW_CITY_UPTOWN", 5000),
}
INPUTS = ("training-pairs.jsonl", "holdout-cv.jsonl", "ppo-validation.jsonl", "v4-manifest.json",
          "detour-val.jsonl", "detour-test.jsonl", "fixtures.json", "places.json", "save.sav", "accepted-links.txt")


def module(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + ".py"))
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


v3 = module("nav_teacher_v3")


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def horizontal(first, second):
    return math.hypot(first[0] - second[0], first[2] - second[2])


def position(name, positions):
    result = [float(part) for part in name[3:].split(",")] if name.startswith("cm:") else positions[name]
    if len(result) != 3 or not all(math.isfinite(value) for value in result):
        raise ValueError(f"Invalid water isolation coordinate: {name}")
    return result


def segment_distance(point, start, goal):
    dx, dz = goal[0] - start[0], goal[2] - start[2]
    length = dx * dx + dz * dz
    fraction = min(1., max(0., ((point[0] - start[0]) * dx + (point[2] - start[2]) * dz) / length)) if length else 0.
    return math.hypot(point[0] - start[0] - fraction * dx, point[2] - start[2] - fraction * dz)


def heldout_rows(manifest, detours):
    if manifest.get("dataset") != "nav-demos-v4" or manifest.get("complete") is not True:
        raise ValueError("Water selection needs the complete original v4 manifest")
    result = []
    for split in ("validation", "test"):
        for values in manifest["splits"][split].values():
            result.extend(values)
    for value in detours:
        if value["split"] not in ("validation", "test"):
            raise ValueError("Detour isolation source contains a training scenario")
        result.append(value)
    return result


def candidates(training, positions, places, manifest, detours, fixtures, excluded, per_region=3000):
    """Use the frozen train districts and native place membership before probes."""
    if not 0 < per_region <= 3000:
        raise ValueError("Water candidates require 1..3000 pairs per train district")
    if set(REGIONS) - {value["region"] for value in training}:
        raise ValueError("Frozen v4 road training input lacks a water train district")
    heldout = heldout_rows(manifest, detours)
    forbidden_pairs = set(excluded)
    for value in heldout:
        forbidden_pairs.update(v3.legs(value))
    heldout_places = {place for split in ("validation", "test")
                      for place in manifest["split_policy"]["object"]["groups"][split]}
    # Named points carry the same place IDs as knownroads. A held-out entrance
    # or road place cannot become a new water route through another alias.
    forbidden_names = {name for name, index in places["points"].items()
                       if places["places"][index]["id"] in heldout_places}
    for place in places["places"]:
        if place["id"] in heldout_places:
            forbidden_names.update([place.get("anchor", "")])
            for entrance in place.get("entrances", []):
                forbidden_names.update(entrance.get(key, "") for key in ("in", "out"))
    safety = []
    for fixture in fixtures:
        for key in ("projected_start_cm", "goal_cm", "hazard_cm"):
            if key in fixture:
                safety.append(fixture[key])
    if not safety:
        raise ValueError("Water selection requires the frozen safety-fixture coordinates")
    # Exact held-out endpoints also exclude coordinate aliases of those sites.
    excluded_sites = []
    for value in heldout:
        for key in ("start", "goal"):
            try:
                excluded_sites.append(position(value[key], positions))
            except KeyError:
                raise ValueError(f"Missing held-out endpoint in native discovery: {value[key]}") from None
    allowed = {name: point for name, point in positions.items()
               if name not in forbidden_names
               and all(horizontal(point, site) >= 200 for site in excluded_sites)
               and all(horizontal(point, site) >= 500 for site in safety)}
    groups = defaultdict(list)
    used = set()
    for region, (anchor, prefix, radius) in REGIONS.items():
        center = positions[anchor]
        starts = [name for name in allowed if name.startswith(prefix) and horizontal(allowed[name], center) <= radius]
        # Both endpoints stay in the same named district. This avoids reusing
        # the Onar/Lobart held-out moving regions as a water training shore.
        for start in sorted(starts):
            for goal in sorted(starts):
                if start == goal or (start, goal) in forbidden_pairs or (goal, start) in forbidden_pairs:
                    continue
                first, last = allowed[start], allowed[goal]
                distance = horizontal(first, last)
                if not 500 <= distance <= 8000 or abs(first[1] - last[1]) > 600:
                    continue
                if any(segment_distance(site, first, last) < 500 for site in safety):
                    continue
                identifier = v3.scenario_id("water", start, goal)
                if identifier in used:
                    continue
                used.add(identifier)
                groups[region].append(dict(id=identifier, goal_kind="point", dataset_family="water", split="train",
                    region=region, start=start, goal=goal, seed=SEED,
                    provenance="native waypoints in frozen v4 train district; knownroads place isolation"))
    selected = []
    for region in sorted(groups):
        # Seeded hash ordering freezes geometry-independent selection and
        # spreads source/goal pairs before any teacher is attempted.
        selected.extend(sorted(groups[region], key=lambda row: hashlib.sha256(
            f"{SEED}:{row['id']}".encode()).hexdigest())[:per_region])
    return selected


def qualified_candidates(values, probes):
    by_id = {value["label"]: value for value in probes}
    if len(by_id) != len(probes) or set(by_id) != {value["id"] for value in values}:
        raise ValueError("Native water probes must cover exactly the frozen candidate pool")
    result = []
    for value in values:
        probe = by_id[value["id"]]
        if (probe["start"], probe["goal"]) != (value["start"], value["goal"]):
            raise ValueError("Native water probe changed a frozen route")
        crossing = probe["direct_water_samples"] > 0 and probe["straight_blocked"] is True
        shore = probe["consecutive_shore_samples"] >= 3
        required = ("qualified", "start_dry_supported", "goal_dry_supported", "detour_complete", "endpoints_on_surface")
        if all(probe[key] is True for key in required) and (crossing or shore):
            if probe["water_relation"] != ("crossing" if crossing else "along_shore"):
                raise ValueError("Native water relation differs from physical samples")
            result.append(dict(value, water_qualification=probe))
    # Round-robin source points prevents the first successful shoreline from
    # monopolising collection while preserving all qualified reserves.
    groups = defaultdict(list)
    for value in result:
        groups[value["start"]].append(value)
    result = []
    while any(groups.values()):
        for start in sorted(groups):
            if groups[start]:
                result.append(groups[start].pop(0))
    if len(result) < MINIMUM_CANDIDATES:
        raise ValueError(f"Only {len(result)} train water routes qualified; need at least {MINIMUM_CANDIDATES}")
    return result


def prepare(task, archive_root):
    task, archive_root = Path(task), Path(archive_root)
    if not task.is_absolute() or not archive_root.is_absolute():
        raise ValueError("Water task and archive paths must be absolute")
    output = task / "water-prepare"
    output.mkdir(parents=True, exist_ok=True)
    source = task / "input"
    identity = dict(input_sha256={name: digest(source / name) for name in INPUTS},
        discovery_sha256={name: digest(task / "discovery" / name) for name in ("roads-graph.json", "points-v3.jsonl")},
        runner_sha256=digest(task / "build/opengothic/gothic-body-runner"), script_sha256=digest(__file__), seed=SEED)
    identity_file = output / "identity.json"
    if identity_file.exists() and v3.read_json(identity_file) != identity:
        raise ValueError("Water preparation belongs to another binary, input or candidate implementation")
    v3.write_json(identity_file, identity)
    positions = v3.load_start_positions(task / "discovery/points-v3.jsonl")
    values = candidates(list(v3.rows(source / "training-pairs.jsonl")), positions,
        v3.read_json(source / "places.json"), v3.read_json(source / "v4-manifest.json"),
        [*v3.rows(source / "detour-val.jsonl"), *v3.rows(source / "detour-test.jsonl")],
        v3.read_json(source / "fixtures.json"),
        v3.exclusion_pairs(source / "holdout-cv.jsonl", source / "ppo-validation.jsonl"))
    if not values:
        raise ValueError("No isolated water candidate pairs remain")
    v3.write_selection(output / "candidates.jsonl", values)
    probe_file = output / "candidates.txt"
    probe_file.write_text("".join(f"{value['id']} {value['start']} {value['goal']}\n" for value in values), encoding="utf-8")
    native = output / "native/water-probes.jsonl"
    if not native.exists():
        if shutil.disk_usage(task).free < 20 * 1024**3 + 512 * 1024**2:
            raise ValueError("Water preparation needs the 20 GiB local disk reserve")
        args = SimpleNamespace(output=output, runner=task / "build/opengothic/gothic-body-runner",
            assets=Path(os.environ.get("GOTHIC_DIR", Path.home() / "gothic-ai/game-settings")), save=source / "save.sav", cache=task / "cache",
            links=source / "accepted-links.txt", roads=source / "places.json", ini=None,
            npc="BAU_954_MALETH", seed=SEED, watchdog_seconds=3600)
        v3.checked_paths(args)
        runner = v3.new_runner(args)
        try:
            v3.runner_scenario(args, runner, "demos-v5-water-prepare", ["navdemos record off", "navdemos physics cv",
                f"navdemos water-probes {probe_file}"], native.parent, SEED)
        finally:
            runner.close()
    selected = qualified_candidates(values, list(v3.rows(native)))
    v3.write_selection(source / "water-scenarios.jsonl", selected)
    v3.write_selection(task / "recording-input/water.jsonl", selected)
    print(json.dumps(dict(event="water_scenarios_prepared", candidates=len(values), qualified=len(selected),
        by_region=dict(Counter(value["region"] for value in selected)),
        by_relation=dict(Counter(value["water_qualification"]["water_relation"] for value in selected)),
        demonstration_recorded=False)), flush=True)
    return selected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--task", type=Path, required=True)
    parser.add_argument("--archive-root", type=Path, required=True)
    options = parser.parse_args()
    os.environ.update(module("ryzen_game").game_environment())
    prepare(options.task, options.archive_root)


if __name__ == "__main__":
    main()
