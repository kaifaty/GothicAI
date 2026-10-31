#!/usr/bin/env python3
"""Prepare held-out detour geometry inside one caller-owned gq batch.

No policy is loaded, no teacher drives, and no NAVDEMO is recorded. The native
geometry and material witnesses precede classification and freezing of the two
evaluation sets. The v4 corpus is read-only; every output belongs to TASK.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
from types import SimpleNamespace


SEED = 22102026
RESERVE_BYTES = 20 * 1024**3
SPLITS = ("validation", "test")


def module(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + ".py"))
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def rows(path):
    with Path(path).open(encoding="utf-8") as stream:
        return [json.loads(line) for line in stream if line.strip()]


def write_rows(path, values):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    content = "".join(json.dumps(value, sort_keys=True) + "\n" for value in values)
    if path.exists():
        if path.read_text(encoding="utf-8") != content:
            raise ValueError(f"Refusing to replace geometry inputs: {path}")
    else:
        path.write_text(content, encoding="utf-8")


def require_room(task):
    free = shutil.disk_usage(task).free
    if free < RESERVE_BYTES + 512 * 1024**2:
        raise ValueError(f"Disk reserve: free={free}, required={RESERVE_BYTES + 512 * 1024**2}")


def coordinate(value):
    return "cm:" + ",".join(f"{part:.9g}" for part in value)


def identifier(*parts):
    return "detour-" + hashlib.sha256("\0".join(map(str, parts)).encode()).hexdigest()[:16]


def polygon_starts(polygons):
    # Convex ground polygon centres preserve their actual storey. Physical
    # support and endpoint snapping are checked by the native probes below.
    cells = {}
    for polygon in polygons:
        vertices = polygon["vertices"]
        if not vertices or any(len(vertex) != 3 or not all(math.isfinite(v) for v in vertex) for vertex in vertices):
            raise ValueError("Invalid navigation polygon vertices")
        point = [100 * sum(vertex[axis] for vertex in vertices) / len(vertices) for axis in range(3)]
        cell = tuple(math.floor(v / 100) for v in point)
        cells[cell] = min(cells.get(cell, point), point)
    return sorted(cells.values())


def candidates(manifest, corpus, polygons):
    """Keep point pairs exact and object place membership from split_policy."""
    if (manifest.get("dataset") != "nav-demos-v4" or manifest.get("complete") is not True
            or Path(manifest["root"]).resolve() != corpus.resolve()):
        raise ValueError("A complete original v4 corpus is required")
    result = {}
    for split in SPLITS:
        source = manifest["split_policy"]["point"][f"{split}_source"]
        expected = {frozenset((value["start"], value["goal"])) for value in rows(corpus / source)}
        points = manifest["splits"][split]["point"]
        if len(points) != 60 or {frozenset((p["start"], p["goal"])) for p in points} != expected:
            raise ValueError(f"Point {split} differs from its frozen pairs")
        for value in points:
            value = dict(value, id=identifier(split, "point", value["start"], value["goal"]))
            result[value["id"]] = value
    groups = manifest["split_policy"]["object"]["groups"]
    place_splits = {}
    for split, places in groups.items():
        for place in places:
            if place in place_splits:
                raise ValueError(f"Object place appears in multiple splits: {place}")
            place_splits[place] = split
    source = rows(corpus / "input/object-candidates.jsonl")
    targets = {}
    for original in source:
        if original["goal_kind"] != "object" or original["split"] not in SPLITS:
            continue
        value = dict(original, family="object", limit_ms=240000)
        if place_splits[value["place_id"]] != value["split"]:
            raise ValueError(f"Object candidate changed its place split: {value['id']}")
        result[value["id"]] = value
        key = value["place_id"], value["goal"]
        target = {key: value[key] for key in ("place_id", "goal", "goal_kind", "split", "region",
                  "target_position", "target_type", "entrance", "entrance_in", "entrance_out") if key in value}
        if key in targets and targets[key] != target:
            raise ValueError(f"Conflicting object target: {key}")
        targets[key] = target
    starts = polygon_starts(polygons)
    for _, target in sorted(targets.items()):
        goal = target["target_position"]
        nearby = [point for point in starts if 500 <= math.hypot(point[0] - goal[0], point[2] - goal[2]) <= 10000
                  and abs(point[1] - goal[1]) <= 2000]
        # A fixed hash order spreads starts around each entrance without using
        # geometry outcomes, teacher success or any student result for ranking.
        nearby.sort(key=lambda point: identifier(SEED, target["place_id"], target["goal"], coordinate(point)))
        for point in nearby[:1024]:
            start = coordinate(point)
            value = dict(target, start=start, seed=SEED, family="object", limit_ms=240000,
                         id=identifier(target["split"], target["place_id"], target["goal"], start))
            result[value["id"]] = value
    excluded = module("nav_teacher_v3").exclusion_pairs(corpus / "input/holdout-cv.jsonl", corpus / "input/ppo-validation.jsonl")
    return sorted((value for value in result.values() if value["goal_kind"] == "point"
                   or module_isolated(value, excluded)), key=lambda value: value["id"])


def module_isolated(value, excluded):
    goals = value.get("goal_aliases", [value["goal"]])
    goals = set(goals) | {value["goal"], value.get("entrance_in"), value.get("entrance_out")}
    return all((value["start"], goal) not in excluded and (goal, value["start"]) not in excluded
               for goal in goals if goal)


def qualified(value, probe, plan, mesh):
    for native in (probe, plan):
        if (native["label"] != value["id"] or native["start"] != value["start"] or native["goal"] != value["goal"]):
            raise ValueError("Native geometry identity differs from candidate")
    direct = math.dist(plan["start_cm"], plan["goal_cm"])
    length = plan["length_cm"]
    corners = plan["corners_cm"]
    if not corners:
        return False
    # Match the native endpoint-on-surface check. Recast ground can be about
    # half a metre below the physical feet; a total 20-cm snap gate is wrong.
    def on_surface(requested, snapped):
        return (math.hypot(requested[0] - snapped[0], requested[2] - snapped[2]) <= 100 * mesh["cell_size_m"]
                and abs(requested[1] - snapped[1]) <= 100 * (mesh["climb_m"] + 2 * mesh["cell_height_m"]))
    if not (all(math.isfinite(v) for v in (direct, length, plan["start_snap_cm"], plan["end_snap_cm"]))
            and direct > 0 and length >= 1.3 * direct
            and plan["complete"] is True and plan["straight_walkable"] is False
            and probe["detour_complete"] is True and probe["straight_blocked"] is True
            and probe["start_supported"] is True
            and on_surface(plan["start_cm"], corners[0]) and on_surface(plan["goal_cm"], corners[-1])):
        return False
    return value["goal_kind"] != "object" or (probe["visible"] is True
        and probe["visibility_criterion"] == "centre_or_opening_edge_v1")


def profile_queries(values):
    """Static physical witnesses, never per-frame trajectories or observations."""
    for value in values:
        start, goal = value["geometry"]["start_cm"], value["geometry"]["goal_cm"]
        for height in (40, 100, 180):
            yield value["id"], f"horizontal-{height}", [start[0], start[1] + height, start[2],
                                                        goal[0], goal[1] + height, goal[2], 0]
        count = max(1, math.ceil(math.hypot(goal[0] - start[0], goal[2] - start[2]) / 50))
        for index in range(count + 1):
            point = [a + (b - a) * index / count for a, b in zip(start, goal)]
            yield value["id"], f"floor-{index}-of-{count}", [point[0], point[1] + 200, point[2],
                                                           point[0], point[1] - 1000, point[2], 0]


def obstacle_type(samples):
    """Use positive physical witnesses; an unnamed STONE hit is ambiguous."""
    walls = sorted({ray["sector"] for sample, ray in samples if sample.startswith("horizontal-")
                    and ray["hit"] and any(name in ray.get("sector", "").upper()
                                           for name in ("HOLZWAND", "INSIDEWALL", "RICH_WALL"))})
    if walls:
        return "wall_or_fence", dict(wall_material_names=walls)
    floor = sorted((int(sample.split("-")[1]), ray) for sample, ray in samples if sample.startswith("floor-"))
    steps = []
    for (ia, a), (ib, b) in zip(floor, floor[1:]):
        if (ib == ia + 1 and a["hit"] and b["hit"] and min(a["normal"][1], b["normal"][1]) >= math.sqrt(.5)
                and math.hypot(a["point"][0] - b["point"][0], a["point"][2] - b["point"][2]) <= 50.01):
            steps.append(abs(a["point"][1] - b["point"][1]))
    if steps and max(steps) > 100:
        return "cliff_or_ledge", dict(supported_floor_step_cm=max(steps))
    return "unknown", dict(reason="No unambiguous obstacle-type witness; geometry still qualifies")


def site_id(value):
    if value["goal_kind"] == "object":
        return value["place_id"]
    if value["split"] == "test":
        # Eleven object places already meet the site requirement. Do not add
        # potentially overlapping point sites merely to increase that count.
        return None
    # These are the original validation road pairs. Merge whole neighbourhoods
    # and all Onar/uptown pairs into existing object sites, conservatively.
    return {"harbour": "point-harbour", "port_streets": "point-harbour", "market": "point-market",
            "lobart": "point-lobart-forest", "onar": "BIGFARM_HOUSE",
            "upper_streets": "CITY_UPTOWN_HUT#2"}[value["region"]]


def freeze(task, corpus):
    output = task / "detours"
    values = rows(output / "qualified.jsonl")
    probes = {value["label"]: value for value in rows(output / "probes/object-candidates.jsonl")}
    mesh = json.loads((output / "runner-work/prepare/navmesh-stats.json").read_text(encoding="utf-8"))
    samples = {value["id"]: [] for value in values}
    for directory in sorted((output / "profiles").iterdir()):
        queries = rows(directory / "queries.jsonl")
        rays = {ray["id"]: ray for ray in rows(directory / "native/physics-results.jsonl") if ray["kind"] == "ray"}
        if set(rays) != {query["id"] for query in queries}:
            raise ValueError("Physical profile query IDs differ")
        for query in queries:
            samples[query["scenario"]].append((query["sample"], rays[query["id"]]))
    # Revalidate membership against the original corpus, without adding or
    # changing any route after looking at a student's outcome.
    manifest = json.loads((corpus / "manifest.json").read_text(encoding="utf-8"))
    frozen_points = {split: {(p["start"], p["goal"]) for p in manifest["splits"][split]["point"]} for split in SPLITS}
    selected = {split: [] for split in SPLITS}
    for value in values:
        split = value["split"]
        if not qualified(value, probes[value["id"]], value["geometry"], mesh):
            raise ValueError(f"Previously qualified detour no longer qualifies: {value['id']}")
        if value["goal_kind"] == "point":
            if (value["start"], value["goal"]) not in frozen_points[split]:
                raise ValueError("Point detour left its original split")
        elif value["place_id"] not in manifest["split_policy"]["object"]["groups"][split]:
            raise ValueError("Object detour left its original place split")
        if len([sample for sample, _ in samples[value["id"]] if sample.startswith("horizontal-")]) != 3:
            raise ValueError(f"Missing physical obstacle profiles: {value['id']}")
        kind, evidence = obstacle_type(samples[value["id"]])
        selected[split].append(dict(value, family="detour", source_family=value["goal_kind"], site_id=site_id(value),
                                    obstacle_type=kind, obstacle_evidence=evidence))
    for split, name in (("validation", "detour-val"), ("test", "detour-test")):
        if len(selected[split]) < 60:
            raise ValueError(f"Only {len(selected[split])} strict {name} scenarios")
        path = output / f"{name}.jsonl"
        write_rows(path, sorted(selected[split], key=lambda value: value["id"]))
        print(json.dumps(dict(event="detours_frozen", split=split, scenarios=len(selected[split]),
            sites_lower_bound=len({value["site_id"] for value in selected[split] if value["site_id"]}),
            by_goal_kind=dict(Counter(value["goal_kind"] for value in selected[split])),
            by_obstacle_type=dict(Counter(value["obstacle_type"] for value in selected[split])),
            confirmed_rock=0, sha256=hashlib.sha256(path.read_bytes()).hexdigest())), flush=True)


def prepare(task, corpus):
    v3 = module("nav_teacher_v3")
    output = task / "detours"
    if (output / "qualified.jsonl").exists():
        raise ValueError("Detour geometry already exists; inspect it before starting another phase")
    require_room(task)
    args = SimpleNamespace(output=output, runner=task / "build/opengothic/gothic-body-runner",
        assets=Path(os.environ.get("GOTHIC_DIR", Path.home() / "gothic-ai/game-settings")), save=corpus / "input/save.sav", cache=task / "navmesh-cache",
        links=corpus / "input/jump-links.txt", roads=corpus / "input/places.json", ini=None,
        npc="BAU_954_MALETH", seed=SEED, watchdog_seconds=1800)
    v3.checked_paths(args)
    runner = v3.new_runner(args)
    try:
        manifest = json.loads((corpus / "manifest.json").read_text(encoding="utf-8"))
        prepare_output = output / "runner-work/prepare"
        mesh = json.loads((prepare_output / "navmesh-stats.json").read_text(encoding="utf-8"))
        values = candidates(manifest, corpus, rows(prepare_output / "navmesh-polygons.jsonl"))
        # Project generated mesh centres onto physical floor before asking the
        # unchanged object probe to check foot support. Ray profiles do not
        # affect native eye/memory inputs or teacher labels.
        generated = sorted({value["start"] for value in values if value["start"].startswith("cm:")})
        if generated:
            query_file = output / "support-queries.tsv"
            points = [[float(v) for v in start[3:].split(",")] for start in generated]
            query_file.write_text("".join(f"{p[0]:.9g} {p[1] + 150:.9g} {p[2]:.9g} {p[0]:.9g} {p[1] - 100:.9g} {p[2]:.9g} 0\n"
                                         for p in points), encoding="utf-8")
            v3.runner_scenario(args, runner, "detour-support", ["navdemos physics cv", "navdemos record off",
                f"physics dump 1 {SEED} {query_file}"], output / "support", SEED)
            support = {row["id"]: row for row in rows(output / "support/physics-results.jsonl") if row["kind"] == "ray"}
            if set(support) != set(range(len(generated))):
                raise ValueError("Physical support queries are incomplete")
            projected = {}
            for index, start in enumerate(generated):
                hit, point = support[index], points[index]
                if (hit["hit"] is True and hit["normal"][1] >= .65
                        and abs(hit["point"][1] - point[1]) <= 100 * (mesh["climb_m"] + 2 * mesh["cell_height_m"])):
                    projected[start] = coordinate(hit["point"])
            values = [dict(value, start=projected[value["start"]],
                           id=identifier(value["split"], value["place_id"], value["goal"], projected[value["start"]]))
                      if value["start"] in projected else value for value in values
                      if not value["start"].startswith("cm:") or value["start"] in projected]
        values = sorted({value["id"]: value for value in values}.values(), key=lambda value: value["id"])
        write_rows(output / "candidates.jsonl", values)
        probe_file = output / "probes.txt"
        probe_file.write_text("".join(v3.probe_row(value) + "\n" for value in values), encoding="utf-8")
        print(json.dumps(dict(event="geometry_started", candidates=len(values))), flush=True)
        v3.runner_scenario(args, runner, "detour-probes", ["navdemos physics cv", "navdemos record off",
            f"navdemos probes-v3 {args.npc} {probe_file}"], output / "probes", SEED)
        probes = {row["label"]: row for row in rows(output / "probes/object-candidates.jsonl")}
        if set(probes) != {value["id"] for value in values}:
            raise ValueError("Native probes did not cover exactly the candidate pool")
        # Plan only supported, blocked routes meeting the ratio. Object
        # visibility is resolved before any subsequent experiment is allowed.
        values = [value for value in values if probes[value["id"]]["start_supported"] is True
            and probes[value["id"]]["detour_complete"] is True and probes[value["id"]]["straight_blocked"] is True
            and probes[value["id"]]["direct_cm"] > 0
            and probes[value["id"]]["detour_length_cm"] >= 1.3 * probes[value["id"]]["direct_cm"]
            and (value["goal_kind"] != "object" or probes[value["id"]]["visible"] is True)]
        if not values:
            raise ValueError("No supported detour candidates survived native probes")
        plan_file = output / "plans.txt"
        plan_file.write_text("".join(f"{value['id']} {value['start']} {value['goal']}\n" for value in values), encoding="utf-8")
        v3.runner_scenario(args, runner, "detour-plans", ["navdemos physics cv", "navdemos record off",
            f"navdemos detour-plans-v3 {plan_file}"], output / "plans", SEED)
        plans = {row["label"]: row for row in rows(output / "plans/detour-plans-v3.jsonl")}
        if set(plans) != {value["id"] for value in values}:
            raise ValueError("Native plans did not cover exactly the prequalified candidates")
        values = [dict(value, geometry=plans[value["id"]], visibility=probes[value["id"]].get("visible"))
                  for value in values if qualified(value, probes[value["id"]], plans[value["id"]], mesh)]
        # These are geometric candidates, not the frozen detour-val/test sets.
        # Classification uses the saved physical/material evidence below first.
        queries = list(profile_queries(values))
        for first in range(0, len(queries), 20000):
            require_room(task)
            batch = queries[first:first + 20000]
            directory = output / "profiles" / f"{first // 20000:04d}"
            directory.mkdir(parents=True, exist_ok=True)
            write_rows(directory / "queries.jsonl", [dict(id=i, scenario=case, sample=sample) for i, (case, sample, _) in enumerate(batch)])
            query_file = directory / "queries.tsv"
            query_file.write_text("".join(" ".join(f"{v:.9g}" for v in query) + "\n" for _, _, query in batch), encoding="utf-8")
            v3.runner_scenario(args, runner, f"detour-profile-{first // 20000}", ["navdemos physics cv",
                "navdemos record off", f"physics dump 1 {SEED} {query_file}"], directory / "native", SEED)
            physical = rows(directory / "native/physics-results.jsonl")
            if len(physical) != 2 * len(batch) or {(r["kind"], r["id"]) for r in physical} != {
                    (kind, index) for kind in ("ray", "sphere") for index in range(len(batch))}:
                raise ValueError("Physical geometry profile is incomplete")
        write_rows(output / "qualified.jsonl", values)
        print(json.dumps(dict(event="geometry_complete", qualified=len(values),
            by_split_family=dict(Counter(f"{v['split']}/{v['goal_kind']}" for v in values)),
            object_places={split: sorted({v["place_id"] for v in values if v["split"] == split
                                         and v["goal_kind"] == "object"}) for split in SPLITS},
            profile_queries=len(queries), policies_evaluated=0)), flush=True)
    finally:
        runner.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "freeze"), nargs="?", default="prepare")
    parser.add_argument("--task", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, required=True)
    options = parser.parse_args()
    if options.command == "prepare":
        os.environ.update(module("ryzen_game").game_environment())
    if (not options.task.is_absolute() or not options.corpus.is_absolute()
            or options.task.resolve().is_relative_to(options.corpus.resolve())
            or options.corpus.resolve().is_relative_to(options.task.resolve())):
        parser.error("Task and read-only corpus must be distinct absolute paths")
    {"prepare": prepare, "freeze": freeze}[options.command](options.task.resolve(), options.corpus.resolve())
