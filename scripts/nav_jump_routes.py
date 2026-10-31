#!/usr/bin/env python3
"""Qualify jump-data routes against the actual v2 teacher's first local plan.

`run` belongs inside the caller's gq batch request on Ryzen. It only builds the
caller-owned mesh and asks plans-v2; it never moves the teacher or runs training.
`prepare` and `qualify` also work offline. Route acceptance requires the actual
transition id in the selected teacher plan, never geometric proximity to a link.
All variants of nearby transitions share one deterministic spatial split.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import importlib.util
import itertools
import json
import math
from pathlib import Path
import sys

REPO = Path(__file__).resolve().parent.parent
TYPES = {1: "jump", 2: "climb", 3: "drop"}
SPLITS = ("train", "validation", "test")


def module(name):
    spec = importlib.util.spec_from_file_location(name, REPO / "scripts" / f"{name}.py")
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def rows(path):
    with Path(path).open(encoding="utf-8") as stream:
        for number, line in enumerate(stream, 1):
            if line.strip():
                try:
                    yield json.loads(line)
                except ValueError as exc:
                    raise ValueError(f"Invalid JSON at {path}:{number}") from exc


def write_rows(path, values):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(json.dumps(value, ensure_ascii=False, sort_keys=True) + "\n"
                            for value in values), encoding="utf-8")


def read_links(path):
    links = module("nav_jumps").links(path)
    return [dict(link, type=TYPES[link["type"]]) for link in links]


def spatial_splits(links, seed=19, separation_m=30.):
    """Union both endpoints, transitively: no pair across splits is <30 m.

    Splitting before route generation also keeps rejected/unselected nearby links
    in the same location, so adding a new route variant cannot introduce leakage.
    """
    if not math.isfinite(separation_m) or separation_m <= 0:
        raise ValueError("Spatial split separation must be positive")
    links = sorted(links, key=lambda link: link["id"])
    if len({link["id"] for link in links}) != len(links):
        raise ValueError("Duplicate transition id")
    parents = list(range(len(links)))

    def root(index):
        while parents[index] != index:
            parents[index] = parents[parents[index]]
            index = parents[index]
        return index

    cells = defaultdict(list)
    for index, link in enumerate(links):
        for endpoint in (link["start"], link["end"]):
            cell = tuple(math.floor(value / separation_m) for value in endpoint)
            for offset in itertools.product((-1, 0, 1), repeat=3):
                neighbour = tuple(a + b for a, b in zip(cell, offset))
                for other, point in cells[neighbour]:
                    if math.dist(endpoint, point) < separation_m:
                        a, b = root(index), root(other)
                        parents[max(a, b)] = min(a, b)
            cells[cell].append((index, endpoint))
    groups = defaultdict(list)
    for index, link in enumerate(links):
        groups[root(index)].append(link["id"])
    targets = dict(zip(SPLITS, (.70 * len(links), .15 * len(links), .15 * len(links))))
    assigned = Counter()
    result = {}

    def order(group):
        digest = hashlib.sha256(f"{seed}:".encode() + ",".join(map(str, group)).encode()).hexdigest()
        return -len(group), digest

    for group in sorted(groups.values(), key=order):
        split = max(SPLITS, key=lambda key: targets[key] - assigned[key])
        assigned[split] += len(group)
        cluster = f"site-{min(group)}"
        for identifier in group:
            result[identifier] = dict(split=split, cluster=cluster, site_id=cluster)
    return result


def point_token(point_m):
    return "cm:" + ",".join(f"{value * 100:.6f}" for value in point_m)


def location(link, places):
    """Report the closest named place; location splitting never uses this label."""
    midpoint = tuple((a + b) * 50. for a, b in zip(link["start"], link["end"]))
    place = min(places, key=lambda entry: math.dist(midpoint, entry["center"]))
    prefix = place["id"].split("_", 1)[0].split("#", 1)[0]
    if prefix.startswith("CITY") or prefix in ("HAFEN", "KASERN", "VATRAS", "RICHTER", "PUFF"):
        region = "city"
    elif prefix.startswith("XARDAS"):
        region = "xardas"
    else:
        region = {"FARM1": "lobart", "BIGFARM": "onar", "MONASTERY": "monastery"}.get(prefix, prefix.lower())
    return dict(region=region, place_id=place["id"], place_name=place.get("ru", place["id"]))


def route_candidates(links, places, seed=19):
    splits = spatial_splits(links, seed)
    candidates = []
    for link in sorted(links, key=lambda value: value["id"]):
        start, end = link["start"], link["end"]
        length = math.hypot(end[0] - start[0], end[2] - start[2])
        if length < .01:
            continue
        forward = ((end[0] - start[0]) / length, 0., (end[2] - start[2]) / length)
        right = (-forward[2], 0., forward[0])
        variant = 0
        for approach, beyond, lateral in itertools.product((.8, 1.6, 3., 0.), (1.2, 2.4, .6, 0.), (0., -.35, .35)):
            source = tuple(a - approach * d + lateral * r for a, d, r in zip(start, forward, right))
            target = tuple(a + beyond * d + lateral * r for a, d, r in zip(end, forward, right))
            # Arrival is within 1 m, so a shorter request can finish before takeoff.
            if math.dist(source, target) <= 1.05:
                continue
            candidates.append(dict(label=f"link-{link['id']}-{variant:02d}",
                                   start=point_token(source), goal=point_token(target),
                                   transition_id=link["id"], type=link["type"],
                                   transition_start_m=list(start), transition_end_m=list(end),
                                   approach_m=approach, beyond_m=beyond, lateral_m=lateral,
                                   **location(link, places), **splits[link["id"]]))
            variant += 1
    return candidates


def write_pair_lines(path, candidates):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = []
    for candidate in candidates:
        fields = tuple(candidate[key] for key in ("label", "start", "goal"))
        if any(not isinstance(value, str) or not value or any(char.isspace() for char in value)
               for value in fields):
            raise ValueError("plans-v2 pair tokens must be nonempty and contain no whitespace")
        lines.append(" ".join(fields) + "\n")
    path.write_text("".join(lines), encoding="utf-8")


def qualifying_plan(candidate, plan):
    if (plan.get("label"), plan.get("start"), plan.get("goal")) != tuple(
            candidate[key] for key in ("label", "start", "goal")):
        raise ValueError(f"Plan identity mismatch: {candidate['label']}")
    if plan.get("complete") is not True:
        return "incomplete_path"
    if plan.get("start_supported") is not True:
        return "unsupported_start"
    if plan.get("direct_walkable") is True:
        return "direct_walk_has_no_transition"
    if not (0 <= plan.get("start_snap_m", math.inf) <= .30 and
            0 <= plan.get("end_snap_m", math.inf) <= .60):
        return "excessive_mesh_snap"
    transitions = plan.get("transitions", [])
    matched = [link for link in transitions if link.get("id") == candidate["transition_id"]]
    if len(matched) != 1:
        return "transition_not_in_teacher_path"
    if matched[0].get("type") != candidate["type"]:
        raise ValueError(f"Transition type mismatch: {candidate['label']}")
    index = matched[0].get("corner_index", -1)
    corners = plan.get("corners_cm", [])
    if not isinstance(index, int) or index < 0 or index + 1 >= len(corners):
        raise ValueError(f"Transition corners missing: {candidate['label']}")
    for endpoint, corner in (("start_cm", corners[index]), ("end_cm", corners[index + 1])):
        if math.dist(matched[0][endpoint], corner) > .1:
            raise ValueError(f"Transition endpoint does not match planned corners: {candidate['label']}")
    # Do not place one route through a transition allocated to another split.
    return None


def qualify(candidates, plans, per_link=4):
    if per_link < 1:
        raise ValueError("per_link must be positive")
    by_label = {plan["label"]: plan for plan in plans}
    if len(by_label) != len(plans):
        raise ValueError("Duplicate plans-v2 result label")
    if set(by_label) != {candidate["label"] for candidate in candidates}:
        raise ValueError("plans-v2 results do not cover exactly the candidate routes")
    allocation = {candidate["transition_id"]: candidate["split"] for candidate in candidates}
    accepted = defaultdict(list)
    rejected = Counter()
    for candidate in candidates:
        plan = by_label[candidate["label"]]
        reason = qualifying_plan(candidate, plan)
        if reason is None and any(allocation.get(link["id"]) != candidate["split"]
                                  for link in plan["transitions"]):
            reason = "path_crosses_another_split"
        if reason:
            rejected[reason] += 1
            continue
        accepted[candidate["transition_id"]].append(dict(candidate, teacher_plan=plan,
                                                         planned_transition_ids=[link["id"] for link in plan["transitions"]]))
    selected = []
    for identifier in sorted(accepted):
        # Start centrally when possible, then vary both approach distance and
        # lateral side among already qualified paths. Prefer a new distance on
        # ties, so a side available only at the same pose cannot crowd out a
        # distinct approach. No additional route or plan is fabricated here.
        group = sorted(accepted[identifier], key=lambda row: (abs(row["lateral_m"]), -row["beyond_m"], row["label"]))
        chosen = []
        while group and len(chosen) < per_link:
            if not chosen:
                row = group[0]
            else:
                approaches = {item["approach_m"] for item in chosen}
                sides = {math.copysign(1., item["lateral_m"]) if item["lateral_m"] else 0. for item in chosen}

                def diversity(item):
                    new_distance = item["approach_m"] not in approaches
                    side = math.copysign(1., item["lateral_m"]) if item["lateral_m"] else 0.
                    new_side = side not in sides
                    separation = min(abs(item["approach_m"] - other["approach_m"]) for other in chosen)
                    return new_distance + new_side, new_distance, new_side, separation, item["beyond_m"]

                row = max(group, key=diversity)
            chosen.append(row)
            group.remove(row)
        selected.extend(chosen)
    for index, route in enumerate(selected):
        route["index"] = index
    return selected, dict(candidates=len(candidates), qualified=sum(map(len, accepted.values())),
                          routes=len(selected), transitions=len(accepted), rejected=dict(rejected),
                          by_type=dict(Counter(route["type"] for route in selected)),
                          by_region=dict(Counter(route["region"] for route in selected)),
                          by_split=dict(Counter(route["split"] for route in selected)),
                          clusters=len({row["cluster"] for row in selected}))


def prepare(args):
    places = json.loads(args.places.read_text(encoding="utf-8"))["places"]
    candidates = route_candidates(read_links(args.links), places, args.seed)
    write_rows(args.output / "candidate-routes.jsonl", candidates)
    write_pair_lines(args.output / "candidate-pairs.txt", candidates)
    return candidates


def qualify_files(args):
    selected, counts = qualify(list(rows(args.candidates)), list(rows(args.plans)), args.per_link)
    write_rows(args.output / "routes.jsonl", selected)
    (args.output / "counts.json").write_text(json.dumps(counts, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(counts, ensure_ascii=False), flush=True)
    return selected, counts


def run(args):
    if sys.platform != "linux":
        raise RuntimeError("Run qualification on Ryzen inside gq batch")
    candidates = prepare(args)
    collector = module("navmesh_measure")
    args.cache.mkdir(parents=True, exist_ok=True)
    initial = collector.scenario_file(args, "route-prepare", [f"navmesh build {args.cache} {args.links}"])
    runner = collector.Runner(args, initial)
    plans = []
    try:
        runner.scenario(initial, args.output / "prepare", args.seed, timeout=720)
        for first in range(0, len(candidates), args.batch_size):
            batch = candidates[first:first + args.batch_size]
            source = args.output / "pairs" / f"{first:06d}.txt"
            destination = args.output / "plans" / f"{first:06d}"
            write_pair_lines(source, batch)
            result = destination / "plans-v2.jsonl"
            if not result.exists():
                task = collector.scenario_file(args, f"plans-{first:06d}", [f"roads load {args.roads}",
                    f"roads on {args.npc}", f"navdemos plans-v2 {args.npc} {source}"])
                runner.scenario(task, destination, args.seed, timeout=600)
            found = list(rows(result))
            if {(row["label"], row["start"], row["goal"]) for row in found} != {
                    tuple(row[key] for key in ("label", "start", "goal")) for row in batch} or len(found) != len(batch):
                raise ValueError(f"Incomplete or changed plans-v2 batch: {result}")
            plans.extend(found)
            print(json.dumps(dict(planned=len(plans), candidates=len(candidates))), flush=True)
    finally:
        runner.close()
    write_rows(args.output / "plans-v2.jsonl", plans)
    args.candidates, args.plans = args.output / "candidate-routes.jsonl", args.output / "plans-v2.jsonl"
    return qualify_files(args)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "qualify", "run"))
    parser.add_argument("--output", type=Path, required=True)
    for name in ("runner", "assets", "save", "cache", "links", "roads", "ini", "candidates", "plans"):
        parser.add_argument(f"--{name}", type=Path)
    parser.add_argument("--places", type=Path, default=REPO / "server/src/gothic_ai/gamedata/places.json")
    parser.add_argument("--npc", default="BAU_954_MALETH")
    parser.add_argument("--seed", type=int, default=19)
    parser.add_argument("--batch-size", type=int, default=128)
    parser.add_argument("--per-link", type=int, default=4)
    args = parser.parse_args()
    required = {"prepare": ("links",), "qualify": ("candidates", "plans"),
                "run": ("runner", "assets", "save", "cache", "links", "roads")}[args.command]
    for name in required:
        if getattr(args, name) is None:
            parser.error(f"--{name} is required for {args.command}")
    for name, value in vars(args).items():
        if isinstance(value, Path) and not value.is_absolute():
            parser.error(f"--{name} must be absolute")
    if not 1 <= args.batch_size <= 512 or args.per_link < 1:
        parser.error("Invalid bounded route budget")
    forbidden = (Path.home() / "gothic-ai/nav-ppo").resolve()
    for value in (args.output, args.cache):
        if value and (value.resolve() == forbidden or forbidden in value.resolve().parents):
            parser.error("Use a private output/cache outside nav-ppo")
    args.output.mkdir(parents=True, exist_ok=True)
    if args.command == "prepare":
        print(json.dumps(dict(candidates=len(prepare(args)))))
    elif args.command == "qualify":
        qualify_files(args)
    else:
        run(args)


if __name__ == "__main__":
    main()
