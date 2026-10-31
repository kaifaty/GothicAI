#!/usr/bin/env python3
"""Analyse saved memory-v4 episodes, atlas and cached geometry without a game.

Compile nav_memory_v4_geometry.cpp as described in the report, then pass its
executable with --geometry. Large intermediate numeric data stays under DATA;
only compact results and three figures are written to the report directory.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess

import numpy as np

MODES = ("baseline", "baseline-stranger", "journey", "journey-stranger", "familiar-local", "familiar-stranger")
HAZARDS = dict(wall=2, ledge=4, drop=8, water=16)
EXAMPLES = (("familiar-regression", "beyond-roads", "point-534e7c999f7ea3c6"),
            ("familiar-detour", "detour-val", "detour-cc8c43e7e09b98f4"),
            ("familiar-shorter", "detour-val", "detour-4895485030e94b1a"))


def read(path):
    return json.loads(Path(path).read_text())


def rows(path):
    with Path(path).open() as stream:
        for line in stream:
            if line.strip():
                yield json.loads(line)


def write(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def atlas(path):
    data = Path(path).read_bytes()
    if data[:8] != b"NAVFAM4\n":
        raise ValueError("Expected the saved v4 eye atlas")
    name_size, = struct.unpack_from("<I", data, 8)
    offset = 12 + name_size
    sources, tiles = struct.unpack_from("<II", data, offset)
    offset += 8 + sources * 12
    dtype = np.dtype([("x", "<i4"), ("z", "<i4"), ("reference", "<f4"),
                      ("ground", "<f4"), ("source", "<u4"), ("channels", "u1")])
    if len(data) != offset + tiles * dtype.itemsize:
        raise ValueError("Saved atlas size differs from its header")
    return np.frombuffer(data, dtype=dtype, count=tiles, offset=offset)


def samples(line, radius=0):
    """Length-weighted midpoint samples every <=25 cm; three corridor strips."""
    line = np.asarray(line, dtype=float)
    points, weights = [], []
    for a, b in zip(line, line[1:]):
        length = float(np.linalg.norm((b - a)[[0, 2]]))
        if length < 1e-8:
            continue
        count = math.ceil(length / 25)
        mid = a + ((np.arange(count) + .5) / count)[:, None] * (b - a)
        if radius:
            normal = np.array([-(b[2] - a[2]), 0, b[0] - a[0]]) / length
            points.extend((mid - radius * normal, mid, mid + radius * normal))
            weights.extend(np.full(count, length / count * weight) for weight in (.25, .5, .25))
        else:
            points.append(mid)
            weights.append(np.full(count, length / count))
    return (np.concatenate(points), np.concatenate(weights)) if points else (line[:1], np.zeros(1))


class Map:
    def __init__(self, tiles):
        self.tiles = tiles
        self.lookup = {(int(v["x"]), int(v["z"])): i for i, v in enumerate(tiles)}

    def indices(self, points):
        return np.array([self.lookup.get((math.floor(p[0] / 200), math.floor(p[2] / 200)), -1)
                         for p in points], dtype=np.int64)

    def coverage(self, line, mask, radius=0):
        points, weights = samples(line, radius)
        indices = self.indices(points)
        known = (indices >= 0) & mask[np.maximum(indices, 0)]
        flags = np.where(known, self.tiles["channels"][np.maximum(indices, 0)], 0)
        total = float(weights.sum())
        fraction = lambda condition: float(weights[condition].sum() / total) if total else 0.
        return dict(length_cm=total, known_fraction=fraction(known),
                    hazard_fraction=fraction((flags & 30) != 0),
                    **{name + "_fraction": fraction((flags & bit) != 0) for name, bit in HAZARDS.items()})


def prepare_geometry(data, executable):
    directory = data / "analysis"
    directory.mkdir(exist_ok=True)
    profiles, queries = [], []
    for group in ("evaluation", "safety"):
        for path in sorted((data / group / "familiar-local").glob("**/roads-events.jsonl")):
            if "workers" in path.parts:
                continue
            events = list(rows(path))
            enabled = next((v for v in events if v["kind"] == "roads_enabled"), None)
            stats = next((v for v in events if v["kind"] == "familiar_stats"), None)
            if enabled and stats:
                profiles.append(dict(enabled, key=path.parent.name, native_stats=stats))
            if group == "evaluation" and enabled:
                first = next(rows(path.parent / "teacher-paths.jsonl"))
                queries.append(dict(id=path.parent.name, start_cm=first["position_cm"], goal_cm=first["goal_cm"]))
    (directory / "roads-input.jsonl").write_text("".join(json.dumps(v) + "\n" for v in profiles))
    write(directory / "path-queries.json", queries)
    subprocess.run([str(executable), "roads", "--graph", str(data / "discovery/roads-graph.json"),
                    "--events", str(directory / "roads-input.jsonl"),
                    "--places", str(data / "input/places.json"), "--atlas", str(data / "input/familiar.bin"),
                    "--output", str(directory / "native-masks.json")], check=True)
    caches = list((data / "cache").glob("*.navmesh"))
    if len(caches) != 1:
        raise ValueError("Expected the measurement's one frozen navigation cache")
    subprocess.run([str(executable), "mesh", "--cache", str(caches[0]), "--queries", str(directory / "path-queries.json"),
                    "--output", str(directory / "cached-geometry.json")], check=True)
    return read(directory / "native-masks.json"), read(directory / "cached-geometry.json")


def trajectory(data, mode, cohort, identifier):
    return np.loadtxt(data / "evaluation" / mode / cohort / identifier / "trajectory.txt", ndmin=2)


def first_difference(data, cohort, identifier, first, second):
    paths = [data / "evaluation" / mode / cohort / identifier for mode in (first, second)]
    actions = [np.loadtxt(path / "actions.txt", ndmin=2) for path in paths]
    traces = [np.loadtxt(path / "trajectory.txt", ndmin=2) for path in paths]
    count = min(map(len, actions))
    different = np.flatnonzero(np.any(actions[0][:count, 1:] != actions[1][:count, 1:], axis=1))
    action = int(different[0]) if len(different) else None
    result = dict(first_action_ms=float(actions[0][action, 0]) if action is not None else None,
                  actions_equal=bool(len(actions[0]) == len(actions[1]) and np.array_equal(*actions)))
    if action is not None:
        result["max_position_difference_before_action_cm"] = float(np.linalg.norm(
            traces[0][:action + 1, 1:4] - traces[1][:action + 1, 1:4], axis=1).max())
        result["travel_before_action_cm"] = float(np.linalg.norm(np.diff(traces[0][:action + 1, [1, 3]], axis=0), axis=1).sum())
    for a, b in zip(rows(paths[0] / "teacher-paths.jsonl"), rows(paths[1] / "teacher-paths.jsonl")):
        if a["local_plan_cm"] != b["local_plan_cm"]:
            result.update(first_plan_ms=a["elapsed_ms"], plan_position_difference_cm=float(np.linalg.norm(
                np.array(a["position_cm"]) - b["position_cm"])))
            break
    return result


def pair(data, by_mode, first, second):
    result = {}
    for cohort in ("beyond-roads", "detour-val"):
        a = {k: v for k, v in by_mode[first].items() if v["cohort"] == cohort}
        b = by_mode[second]
        wins, losses, changes, ratios = [], [], [], [[], []]
        for key, v in a.items():
            other = b[key]
            if v["success"] != other["success"]:
                item = dict(id=key, region=v["region"], before_success=v["success"], after_success=other["success"],
                            before_length_m=v["travelled_cm"] / 100, after_length_m=other["travelled_cm"] / 100,
                            before_failure=v["failure"], after_failure=other["failure"],
                            divergence=first_difference(data, cohort, key, first, second))
                (wins if other["success"] else losses).append(item)
            if v["success"] and other["success"]:
                ratios[0].append(v["length_over_detour"])
                ratios[1].append(other["length_over_detour"])
                changes.append(dict(id=key, length_delta_m=(other["travelled_cm"] - v["travelled_cm"]) / 100))
        result[cohort] = dict(wins=wins, losses=losses, common_successes=len(changes),
                              common_success_mean_length_ratio=[float(np.mean(v)) for v in ratios],
                              shorter_over_1m=sum(v["length_delta_m"] < -1 for v in changes),
                              longer_over_1m=sum(v["length_delta_m"] > 1 for v in changes),
                              length_changes=sorted(changes, key=lambda v: v["length_delta_m"]))
    return result


def matching_traces(data, by_mode, first, second):
    result = {}
    for cohort in ("beyond-roads", "detour-val"):
        identical, others = [], []
        for key, meta in by_mode[first].items():
            if meta["cohort"] != cohort:
                continue
            paths = [data / "evaluation" / mode / cohort / key for mode in (first, second)]
            equal = all(digest(paths[0] / name) == digest(paths[1] / name) for name in ("actions.txt", "trajectory.txt"))
            (identical if equal else others).append(key)
        def cost(mode):
            values = [by_mode[mode][key] for key in identical]
            count = sum(v["memory_views"] for v in values)
            return sum(v["memory_views"] * v["memory_view_us"] for v in values) / count if count else None
        result[cohort] = dict(identical=len(identical), differing=others,
                              same_trace_view_mean_us=[cost(first), cost(second)])
    return result


def first_plans(data, cohort, identifier):
    paths = [data / "evaluation" / mode / cohort / identifier / "teacher-paths.jsonl" for mode in ("journey", "familiar-local")]
    for a, b in zip(rows(paths[0]), rows(paths[1])):
        if a["local_plan_cm"] != b["local_plan_cm"]:
            return a, b
    return None


def world_plan(row):
    local = np.asarray(row["local_plan_cm"])
    yaw = math.radians(row["yaw_deg"])
    position = np.asarray(row["position_cm"])
    return position + np.column_stack((local[:, 1] * math.cos(yaw) - local[:, 0] * math.sin(yaw),
                                      np.zeros(len(local)), local[:, 1] * math.sin(yaw) + local[:, 0] * math.cos(yaw)))


def analyse(data, masks, geometry):
    tile_map = Map(atlas(data / "input/familiar.bin"))
    native = {v["key"]: v for v in masks["actors"]}
    references = {v["id"]: v for v in geometry["queries"]}
    metrics = {mode: read(data / "evaluation" / f"metrics-{mode}.json") for mode in MODES}
    by_mode = {mode: {v["id"]: v for v in value["episodes"]} for mode, value in metrics.items()}
    coverage = {}
    native_counts = {}
    for identifier, meta in by_mode["familiar-local"].items():
        actor = native[identifier]
        mask = np.zeros(len(tile_map.tiles), dtype=bool)
        mask[actor["known_tile_ids"]] = True
        if int(mask.sum()) != meta["familiar"]["known_tiles"]:
            raise ValueError("Reconstructed mask differs from the saved NPC mask")
        cohort = meta["cohort"]
        original = trajectory(data, "baseline", cohort, identifier)
        reference = references[identifier]
        if not reference["complete"]:
            raise ValueError("Frozen navigation cache cannot reproduce a complete reference")
        indices = tile_map.indices(original[:1, 1:4])
        coverage[identifier] = dict(cohort=cohort, npc=meta["resident_selection"]["npc"],
            start_known=bool(indices[0] >= 0 and mask[indices[0]]), known_tiles=int(mask.sum()),
            reference_corridor=tile_map.coverage(reference["corners_cm"], mask, 15),
            reference_centerline=tile_map.coverage(reference["corners_cm"], mask),
            baseline_path=tile_map.coverage(original[:, 1:4], mask),
            familiar_path=tile_map.coverage(trajectory(data, "familiar-local", cohort, identifier)[:, 1:4], mask),
            cached_detour_length_cm=reference["length_m"] * 100,
            saved_detour_length_cm=meta["detour_length_cm"])
        native_counts[identifier] = mask
    summaries = {}
    for cohort in ("beyond-roads", "detour-val"):
        group = [v for v in coverage.values() if v["cohort"] == cohort]
        summaries[cohort] = dict(episodes=len(group), starts_known=sum(v["start_known"] for v in group))
        for measure in ("reference_corridor", "reference_centerline", "baseline_path", "familiar_path"):
            length = sum(v[measure]["length_cm"] for v in group)
            summaries[cohort][measure] = {key: sum(v[measure]["length_cm"] * v[measure][key] for v in group) / length
                for key in ("known_fraction", "hazard_fraction", *(name + "_fraction" for name in HAZARDS))}
        summaries[cohort]["median_reference_known_fraction"] = float(np.median([v["reference_corridor"]["known_fraction"] for v in group]))
    pairs = {first + "_to_" + second: pair(data, by_mode, first, second) for first, second in (
        ("baseline", "journey"), ("journey", "familiar-local"),
        ("baseline-stranger", "journey-stranger"), ("journey-stranger", "familiar-stranger"))}
    trace_matches = {first + "_to_" + second: matching_traces(data, by_mode, first, second) for first, second in (
        ("baseline", "journey"), ("journey", "familiar-local"), ("journey-stranger", "familiar-stranger"))}
    examples = {}
    for _, cohort, identifier in EXAMPLES:
        changed = first_plans(data, cohort, identifier)
        examples[identifier] = dict(first_plan_ms=changed[0]["elapsed_ms"],
            position_difference_cm=float(np.linalg.norm(np.asarray(changed[0]["position_cm"]) - changed[1]["position_cm"])),
            plan_over_baked_tiles={mode: tile_map.coverage(world_plan(row), native_counts[identifier])
                                  for mode, row in zip(("journey", "familiar-local"), changed)})
    water_equal = {}
    for label in [v["label"] for v in read(data / "safety/metrics-baseline.json")["episodes"] if v["kind"] == "water"]:
        water_equal[label] = {name: len({digest(data / "safety" / mode / label / name) for mode in MODES}) == 1
                             for name in ("actions.txt", "trajectory.txt", "perception-safety-frames.jsonl", "perception-water-observations.jsonl")}
    safety_coverage = {}
    for meta in read(data / "safety/metrics-familiar-local.json")["episodes"]:
        label = meta["label"]
        mask = np.zeros(len(tile_map.tiles), dtype=bool)
        mask[native[label]["known_tile_ids"]] = True
        trace = np.loadtxt(data / "safety/familiar-local" / label / "trajectory.txt", ndmin=2)
        start = tile_map.indices(trace[:1, 1:4])[0]
        safety_coverage[label] = dict(kind=meta["kind"], npc=meta["resident_selection"]["npc"],
            start_known=bool(start >= 0 and mask[start]), path=tile_map.coverage(trace[:, 1:4], mask))
    result = dict(by_mode={mode: dict(cohorts=metrics[mode]["by_cohort"], memory_cost=metrics[mode]["memory_cost"][mode],
                      safety=read(data / "safety" / f"metrics-{mode}.json")["by_kind"]) for mode in MODES},
                  map_cost=read(data / "bake-npcs/memory-cost.json"), coverage=coverage, coverage_summary=summaries,
                  pairs=pairs, trace_matches=trace_matches, examples=examples, water_traces_identical=water_equal,
                  safety_coverage=safety_coverage,
                  atlas_classes=dict(tiles=len(tile_map.tiles),
                      free_only=int(np.count_nonzero(tile_map.tiles["channels"] == 1)),
                      hazardous=int(np.count_nonzero(tile_map.tiles["channels"] & 30)),
                      free_and_hazard=int(np.count_nonzero((tile_map.tiles["channels"] & 1 != 0) & (tile_map.tiles["channels"] & 30 != 0)))),
                  max_cached_path_length_difference_cm=max(abs(v["cached_detour_length_cm"] - v["saved_detour_length_cm"]) for v in coverage.values()),
                  sources=dict(runner_sha256=metrics["baseline"]["settings"]["baseline"]["runner_sha256"],
                               atlas_sha256=digest(data / "input/familiar.bin")),
                  limitations=["No live near/far grids or eye rays were recorded; cell-level causes cannot be replayed.",
                               "Cached Detour polygons describe walkable surfaces, not complete wall collision geometry.",
                               "Timing comes from batch collection, not a quiet paired microbenchmark; Mac was not measured."])
    # Raw per-actor stats remain on the collection host; retain their ranges here.
    stats = result["map_cost"].pop("measurements")
    locals_ = [v for v in stats if not v["stranger"]]
    result["map_cost"]["local_coverage_fraction"] = dict(min=min(v["coverage_fraction"] for v in locals_),
        median=float(np.median([v["coverage_fraction"] for v in locals_])), max=max(v["coverage_fraction"] for v in locals_))
    result["map_cost"]["layout"] = {k: locals_[0][k] for k in ("shared_tiles", "source_poses", "shared_bytes", "source_bytes", "mask_bytes_per_npc")}
    return result, tile_map, native_counts, by_mode, references


def figures(data, output, geometry, tile_map, masks, by_mode, references):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.collections import PolyCollection
    from matplotlib.patches import Patch

    polygons = [np.asarray(v)[:, [0, 2]] for v in geometry["polygons_cm"]]
    colors = {"baseline": "#777777", "journey": "#2863ad", "familiar-local": "#b12b37"}
    labels = {"baseline": "baseline", "journey": "journey v4", "familiar-local": "familiar"}
    for tag, cohort, identifier in EXAMPLES:
        paths = {mode: trajectory(data, mode, cohort, identifier) for mode in colors}
        reference = np.asarray(references[identifier]["corners_cm"])
        cloud = np.concatenate([v[:, [1, 3]] for v in paths.values()] + [reference[:, [0, 2]]])
        low, high = cloud.min(axis=0) - 400, cloud.max(axis=0) + 400
        fig, axes = plt.subplots(1, 3, figsize=(13, 4), sharex=True, sharey=True)
        for ax in axes:
            ax.set_aspect("equal");ax.set_xlim(low[0] / 100, high[0] / 100);ax.set_ylim(low[1] / 100, high[1] / 100)
            ax.set_xlabel("world X, m");ax.grid(alpha=.15)
        axes[0].set_ylabel("world Z, m")
        axes[0].set_title("Live-memory planner: saved paths")
        for mode, values in paths.items():
            axes[0].plot(values[:, 1] / 100, values[:, 3] / 100, color=colors[mode], lw=1.2, label=labels[mode])
        for mode, row in zip(("journey", "familiar-local"), first_plans(data, cohort, identifier)):
            plan = world_plan(row)
            axes[0].plot(plan[:, 0] / 100, plan[:, 2] / 100, color=colors[mode], lw=1, ls="--")
        axes[0].legend(fontsize=8, loc="upper right")
        axes[1].set_title("Eye-baked familiar tiles (NPC mask)")
        visible = masks[identifier] & (tile_map.tiles["x"] * 200 >= low[0] - 200) & (tile_map.tiles["x"] * 200 <= high[0])
        visible &= (tile_map.tiles["z"] * 200 >= low[1] - 200) & (tile_map.tiles["z"] * 200 <= high[1])
        picked = tile_map.tiles[visible]
        flags = picked["channels"]
        cell_colors = np.full(len(picked), "#b3d8a6", dtype=object)
        for bit, color in ((4, "#e3b64d"), (8, "#e3b64d"), (2, "#bf6971"), (16, "#82bedb")):
            cell_colors[(flags & bit) != 0] = color
        rects = [np.array([[v["x"], v["z"]], [v["x"] + 1, v["z"]], [v["x"] + 1, v["z"] + 1], [v["x"], v["z"] + 1]]) * 2 for v in picked]
        axes[1].add_collection(PolyCollection(rects, facecolors=cell_colors, edgecolors="none", alpha=.8))
        axes[1].plot(reference[:, 0] / 100, reference[:, 2] / 100, "k--", lw=1)
        axes[1].legend(handles=[Patch(color="#b3d8a6", label="free"), Patch(color="#bf6971", label="wall"),
                                Patch(color="#e3b64d", label="ledge/drop")], fontsize=8, loc="upper right")
        axes[2].set_title("Truth: cached Detour walkable surfaces")
        cropped = [p / 100 for p in polygons if (p.max(axis=0) >= low).all() and (p.min(axis=0) <= high).all()]
        axes[2].add_collection(PolyCollection(cropped, facecolors="#d2e0e5", edgecolors="#b5c7cf", linewidths=.3))
        axes[2].plot(reference[:, 0] / 100, reference[:, 2] / 100, "k--", lw=1, label="Detour reference")
        for mode in ("journey", "familiar-local"):
            values = paths[mode]
            axes[2].plot(values[:, 1] / 100, values[:, 3] / 100, color=colors[mode], lw=.9)
        for ax in axes:
            ax.scatter([cloud[0, 0] / 100], [cloud[0, 1] / 100], marker="o", c="black", s=12)
            ax.scatter([reference[-1, 0] / 100], [reference[-1, 2] / 100], marker="*", c="black", s=45)
        fig.suptitle(identifier, fontsize=10)
        fig.tight_layout()
        fig.savefig(output / (tag + ".png"), dpi=140, bbox_inches="tight")
        plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--geometry", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    data, executable, output = args.data.resolve(), args.geometry.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    masks, geometry = prepare_geometry(data, executable)
    result, tile_map, known, by_mode, references = analyse(data, masks, geometry)
    write(output / "results.json", result)
    figures(data, output, geometry, tile_map, known, by_mode, references)
    print(json.dumps(dict(coverage=result["coverage_summary"], map_cost=result["map_cost"]), ensure_ascii=False))


if __name__ == "__main__":
    main()
