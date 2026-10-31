#!/usr/bin/env python3
"""Compare saved honest-v3 memory with evaluator-only Detour path corridors.

No actors are moved and no teacher is called. Query poses come from the saved
per-decision debug trace and are checked against original NAVDEMO v2 records.
Each gzip is scanned once, retaining only classifications of requested cells.
"""
from __future__ import annotations

import argparse
from collections import Counter
from contextlib import ExitStack
import csv
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
from types import SimpleNamespace

import numpy as np

ROOT = Path(__file__).resolve().parent


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / f"{name}.py")
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


v3 = module("nav_teacher_v3")
demos = module("nav_demos")
KINDS = ("point", "object", "follow", "approach")
EXPECTED = dict(point=30, object=30, follow=18, approach=18)
LAYERS = dict(near=(64, 25), far=(100, 200))
CLASS_NAMES = ("free", "unknown", "danger")
OUTCOMES = ("success", "failure")


def vector_token(value):
    value = np.asarray(value, dtype=float)
    if value.shape != (3,) or not np.isfinite(value).all():
        raise ValueError("Expected finite three-dimensional centimetre position")
    return "cm:" + ",".join(format(float(item), ".9g") for item in value)


def goal_from_record(record):
    """Invert the documented body-frame goal input, including its height."""
    right, up, forward = (np.asarray(record["roads"]["goal_direction"], dtype=float)
                          * float(record["roads"]["goal_distance_cm"]))
    yaw = math.radians(float(record["body_yaw_deg"]))
    delta = [-right * math.sin(yaw) + forward * math.cos(yaw), up,
             right * math.cos(yaw) + forward * math.sin(yaw)]
    return np.asarray(record["position_cm"], dtype=float) + delta


def sample_offset(identifier, seed):
    """A fixed seeded 1-Hz phase on the original 10-Hz decision sequence."""
    return int.from_bytes(hashlib.sha256(f"{seed}:{identifier}".encode()).digest()[:4], "little") % 10


def sample_indices(decisions, identifier, seed):
    if decisions < 1:
        raise ValueError("Cannot sample an empty honest-v3 recording")
    offset = sample_offset(identifier, seed)
    return sorted(set(range(offset if offset < decisions else 0, decisions, 10)) | {decisions - 1})


def episode_outcome(kind, metadata, directory):
    """Classify saved honest-v3 performance, keeping native success separate.

    Native follow success denotes surviving the episode, so its outcome must
    instead use the brief's time-weighted 2–5m occupancy after first contact.
    """
    native_success = bool(metadata["success"])
    result = dict(metadata_success=native_success)
    if kind == "follow":
        trace = np.loadtxt(Path(directory) / "target-trajectory.txt", ndmin=2)
        if trace.shape[1] < 5 or not np.isfinite(trace).all():
            raise ValueError(f"Invalid honest-v3 follow trajectory: {directory}")
        metrics = v3.follow_metrics(trace[:, 0], trace[:, 4], metadata["elapsed_ms"])
        fraction = metrics["follow_fraction"]
        succeeded = (metrics["first_contact_ms"] is not None
                     and metrics["follow_observed_ms"] > 0 and fraction is not None and fraction >= .8)
        reason = ("no_first_contact" if metrics["first_contact_ms"] is None else
                  "no_post_contact_time" if metrics["follow_observed_ms"] <= 0 else
                  "band_fraction_below_80_percent" if not succeeded else None)
        result.update(metrics, outcome_basis="first <=5m contact, then >=80% of observed time within inclusive 2–5m band",
                      outcome_failure_reason=reason)
    elif kind in ("point", "object", "approach"):
        succeeded = native_success
        result.update(outcome_basis="native episode.json success", outcome_failure_reason=None if succeeded else metadata.get("failure"))
    else:
        raise ValueError(f"Unknown honest-v3 goal kind: {kind}")
    result.update(outcome="success" if succeeded else "failure", outcome_success=bool(succeeded))
    return result


def source_cases(task, data):
    selected = list(v3.rows(task / "expanded-scenarios.jsonl"))
    if Counter(value["goal_kind"] for value in selected) != EXPECTED:
        raise ValueError("Coverage requires the frozen 30/30/18/18 scenario cohort")
    if len({value["id"] for value in selected}) != len(selected):
        raise ValueError("Duplicate scenario IDs in the frozen selection")
    cases = []
    for value in selected:
        cohort = "v3-retry" if value["goal_kind"] == "point" else "v3-visible"
        directory = data / cohort / value["goal_kind"] / value["split"] / value["id"]
        metadata = v3.read_json(directory / "episode.json")
        if not v3.same_scenario(metadata, value, 3) or not v3.complete(directory):
            raise ValueError(f"Honest-v3 recording identity or payload mismatch: {directory}")
        case = dict(id=value["id"], goal_kind=value["goal_kind"], split=value["split"],
                    region=value.get("region"), place_id=value.get("place_id"), source=str(directory),
                    cohort=cohort, decisions=int(metadata["decisions"]), success=bool(metadata["success"]))
        case.update(episode_outcome(value["goal_kind"], metadata, directory))
        cases.append(case)
    return cases


def prepare(args):
    args.output.mkdir(parents=True, exist_ok=True)
    cases = source_cases(args.task, args.data)
    query_count = 0
    with (args.output / "corridor-queries.txt").open("w", encoding="utf-8") as queries:
        for case in cases:
            wanted = set(sample_indices(case["decisions"], case["id"], args.seed))
            directory = args.output / "cases" / case["id"]
            directory.mkdir(parents=True, exist_ok=True)
            written = 0
            previous_elapsed = -1
            with (directory / "queries.jsonl").open("w", encoding="utf-8") as manifest:
                count = 0
                for index, row in enumerate(v3.rows(Path(case["source"]) / "teacher-paths.jsonl")):
                    count += 1
                    if row["elapsed_ms"] <= previous_elapsed:
                        raise ValueError(f"Nonmonotonic saved plan times: {case['id']}")
                    previous_elapsed = row["elapsed_ms"]
                    if index not in wanted:
                        continue
                    yaw = float(row["yaw_deg"])
                    if not math.isfinite(yaw):
                        raise ValueError(f"Nonfinite saved body yaw: {case['id']}")
                    start, goal = vector_token(row["position_cm"]), vector_token(row["goal_cm"])
                    label = f"{case['id']}-frame{index}"
                    queries.write(f"{label} {start} {yaw:.9g} {goal}\n")
                    frame = dict(label=label, index=index, elapsed_ms=int(row["elapsed_ms"]),
                                 position_cm=row["position_cm"], goal_cm=row["goal_cm"], yaw_deg=yaw)
                    manifest.write(json.dumps(frame, separators=(",", ":")) + "\n")
                    written += 1
                if count != case["decisions"] or written != len(wanted):
                    raise ValueError(f"Saved plan/record decision count mismatch: {case['id']}")
            case["sampled_frames"] = written
            query_count += written
    v3.write_json(args.output / "preparation.json", dict(cases=cases, queries=query_count, seed=args.seed,
                  sampling="one decision per 10 at a scenario-specific SHA256(seed:id) phase, plus last decision; 10-Hz source, 1-Hz samples",
                  requested_corridor=dict(begin_cm=500, end_cm=1500, radius_cm=15)))
    print(json.dumps(dict(event="coverage_prepared", scenarios=len(cases), queries=query_count)), flush=True)


def query_truth(args):
    worker = SimpleNamespace(output=args.output / "runner", runner=args.runner or args.task / "build/opengothic/gothic-body-runner",
                             assets=args.assets or Path(os.environ.get("GOTHIC_DIR", Path.home() / "gothic-ai/game-settings")), save=args.save or args.task / "input/save.sav",
                             cache=args.cache or args.task / "cache", links=args.links or args.task / "input/accepted-links.txt",
                             roads=args.roads or args.task / "input/places.json", ini=None, npc="BAU_954_MALETH", seed=args.seed,
                             physics="cv", watchdog_seconds=3600)
    query = args.output / "corridor-queries.txt"
    if not query.is_file():
        raise ValueError("Coverage queries are missing; run prepare first")
    v3.checked_paths(worker)
    runner = v3.new_runner(worker)
    try:
        v3.runner_scenario(worker, runner, "v3-memory-coverage", [f"navdemos oracle-corridors {query}"],
                           args.output / "truth", worker.seed)
    finally:
        runner.close()


def validate_truth(frame, value):
    for key, expected in (("start_cm", frame["position_cm"]), ("goal_cm", frame["goal_cm"])):
        position = np.asarray(value.get(key), dtype=float)
        if position.shape != (3,) or not np.isfinite(position).all() or np.linalg.norm(position - expected) > 1:
            raise ValueError(f"Truth-query pose mismatch: {frame['label']}, {key}")
    yaw = float(value.get("yaw_deg", math.nan))
    if not math.isfinite(yaw) or abs((yaw - frame["yaw_deg"] + 180) % 360 - 180) > .01:
        raise ValueError(f"Truth-query yaw mismatch: {frame['label']}")
    if (value.get("corridor_begin_cm"), value.get("corridor_end_cm"), value.get("radius_cm")) != (500, 1500, 15):
        raise ValueError(f"Unexpected corridor geometry: {frame['label']}")
    if not isinstance(value.get("complete"), bool) or not isinstance(value.get("corridor_available"), bool):
        raise ValueError(f"Missing true-path status: {frame['label']}")
    if not math.isfinite(float(value.get("length_cm", math.nan))) or value["length_cm"] < 0:
        raise ValueError(f"Invalid true-path length: {frame['label']}")
    if value["corridor_available"] and (not value["complete"] or value["length_cm"] < 500):
        raise ValueError(f"Invalid available-corridor status: {frame['label']}")
    if not isinstance(value.get("native_cells"), list):
        raise ValueError(f"Missing native corridor cells: {frame['label']}")
    if not value["corridor_available"] and value["native_cells"]:
        raise ValueError(f"Unavailable corridor contains cells: {frame['label']}")
    statuses = ("ok", "incomplete_path", "offmesh_transition", "short_path", "unsupported_pose")
    if value.get("corridor_status") not in statuses or (value["corridor_status"] == "ok") != value["corridor_available"]:
        raise ValueError(f"Missing or contradictory corridor status: {frame['label']}")
    if type(value.get("raster_evaluated")) is not bool or type(value.get("transition_count")) is not int:
        raise ValueError(f"Missing grounded-path support status: {frame['label']}")
    if ((value["raster_evaluated"] and type(value.get("raster_support_valid")) is not bool)
            or (not value["raster_evaluated"] and value.get("raster_support_valid") is not None)):
        raise ValueError(f"Invalid evaluated-raster support status: {frame['label']}")
    if value["corridor_available"] and (not value["raster_support_valid"] or value["transition_count"]):
        raise ValueError(f"Unsupported or offmesh route exported as ground corridor: {frame['label']}")


def distribute_truth(args, cases):
    """Validate completeness once, then keep only one case of native probes in RAM."""
    expected = {}
    for case in cases:
        for frame in v3.rows(args.output / "cases" / case["id"] / "queries.jsonl"):
            if frame["label"] in expected:
                raise ValueError("Duplicate prepared corridor-query label")
            expected[frame["label"]] = (case["id"], frame)
    source = args.truth or args.output / "truth" / "oracle-corridors.jsonl"
    if not source.is_file():
        raise ValueError(f"Evaluator corridor result is missing: {source}")
    seen = set()
    with ExitStack() as stack:
        handles = {case["id"]: stack.enter_context((args.output / "cases" / case["id"] / "truth.jsonl").open("w", encoding="utf-8"))
                   for case in cases}
        for value in v3.rows(source):
            label = value.get("label")
            if label not in expected or label in seen:
                raise ValueError(f"Unexpected or duplicate truth-query label: {label}")
            case_id, frame = expected[label]
            validate_truth(frame, value)
            handles[case_id].write(json.dumps(value, separators=(",", ":")) + "\n")
            seen.add(label)
    if seen != expected.keys():
        raise ValueError(f"Incomplete true corridors: {len(seen)}/{len(expected)} queries")


def native_cell(cell):
    layer = cell.get("layer")
    if layer not in LAYERS:
        raise ValueError(f"Unknown corridor memory layer: {layer}")
    size, step = LAYERS[layer]
    row, column = cell.get("row"), cell.get("column")
    if type(row) is not int or type(column) is not int or not (0 <= row < size and 0 <= column < size):
        raise ValueError("Invalid native corridor cell index")
    right, forward = (column + .5 - size / 2) * step, (row + .5 - size / 2) * step
    if layer == "far" and abs(right) < 800 and abs(forward) < 800:
        raise ValueError("Far corridor cell duplicates the near-grid footprint")
    if type(cell.get("truth_passable")) is not bool or type(cell.get("height_valid")) is not bool:
        raise ValueError("Missing native-cell truth or height status")
    height = cell.get("ground_height_cm")
    if cell["height_valid"] and (height is None or not math.isfinite(float(height))):
        raise ValueError("Nonfinite native-cell ground height")
    if cell["truth_passable"] and not cell["height_valid"]:
        raise ValueError("Passable native cell has no ground height")
    return layer, row, column, step


def classify_cells(record, cells):
    totals = Counter()
    layers = {layer: Counter() for layer in LAYERS}
    seen = set()
    for cell in cells:
        layer, row, column, step = native_cell(cell)
        key = layer, row, column
        if key in seen:
            raise ValueError("Duplicate native cell in one true corridor")
        seen.add(key)
        memory = record[f"memory_{layer}"][row, column]
        channels = memory["channels"]
        danger = bool((channels[2:] > 0).any())
        unknown = bool(channels[0] == 0 or channels[1] == 0)
        category = "danger" if danger else "unknown" if unknown else "free"
        counters = Counter(cells=1, area_cm2=step * step)
        counters[category] += 1
        counters[f"{category}_area_cm2"] += step * step
        counters["confidence_quantized_sum"] += int(channels[0])
        counters["free_quantized_sum"] += int(channels[1])
        counters["unknown_exposure_quantized_sum"] += 255 - int(min(channels[:2]))
        counters["truth_passable"] += int(cell["truth_passable"])
        counters[f"truth_passable_{category}"] += int(cell["truth_passable"])
        counters["truth_unpassable"] += int(not cell["truth_passable"])
        counters["truth_height_boundary"] += int(bool(cell.get("height_boundary", False)))
        counters["false_danger"] += int(danger and cell["truth_passable"])
        counters["false_danger_area_cm2"] += int(danger and cell["truth_passable"]) * step * step
        counters["unknown_zero_confidence"] += int(category == "unknown" and channels[0] == 0)
        counters["unknown_positive_confidence_zero_free"] += int(category == "unknown" and channels[0] > 0 and channels[1] == 0)
        for index, name in enumerate(demos.CHANNELS[2:], 2):
            counters[f"danger_{name}"] += int(channels[index] > 0)
        matched_height = bool(memory["height_valid"] and cell["height_valid"])
        counters["height_comparable"] += int(matched_height)
        mismatch = matched_height and abs(float(memory["ground_height_cm"]) - float(cell["ground_height_cm"])) > 100
        counters["height_mismatch_gt100"] += int(mismatch)
        counters["false_danger_height_mismatch_gt100"] += int(mismatch and danger and cell["truth_passable"])
        totals.update(counters)
        layers[layer].update(counters)
    return dict(totals), {key: dict(value) for key, value in layers.items()}


def fractions(counts):
    counts = dict(counts)
    denominator = counts.get("cells", 0)
    area = counts.get("area_cm2", 0)
    result = dict(counts=counts, denominator_cells=denominator,
                  fractions={name: counts.get(name, 0) / denominator if denominator else None for name in CLASS_NAMES},
                  truth_passable_fractions={name: counts.get(f"truth_passable_{name}", 0) / counts["truth_passable"] if counts.get("truth_passable") else None
                                            for name in CLASS_NAMES},
                  area_fractions={name: counts.get(f"{name}_area_cm2", 0) / area if area else None for name in CLASS_NAMES},
                  false_danger_fraction_all_cells=counts.get("false_danger", 0) / denominator if denominator else None,
                  false_danger_fraction_truth_passable=counts.get("false_danger", 0) / counts["truth_passable"] if counts.get("truth_passable") else None,
                  false_danger_fraction_memory_danger=counts.get("false_danger", 0) / counts["danger"] if counts.get("danger") else None,
                  truth_blocked_fraction_all_cells=counts.get("truth_unpassable", 0) / denominator if denominator else None,
                  unknown_exposure_mean=counts.get("unknown_exposure_quantized_sum", 0) / (255 * denominator) if denominator else None)
    if sum(counts.get(name, 0) for name in CLASS_NAMES) != denominator:
        raise ValueError("Memory coverage categories are not an exhaustive disjoint partition")
    return result


def validate_record(record, frame, origin):
    if int(record["time_ms"]) - origin != frame["elapsed_ms"]:
        raise ValueError(f"Saved input/query time mismatch: {frame['label']}")
    if np.linalg.norm(np.asarray(record["position_cm"]) - frame["position_cm"]) > 1:
        raise ValueError(f"Saved input/query body position mismatch: {frame['label']}")
    delta = (float(record["body_yaw_deg"]) - frame["yaw_deg"] + 180) % 360 - 180
    if abs(delta) > .01 or np.linalg.norm(goal_from_record(record) - frame["goal_cm"]) > 1:
        raise ValueError(f"Saved input/query yaw or current moving goal mismatch: {frame['label']}")


def analyze_case(args, case):
    directory = args.output / "cases" / case["id"]
    frames = {value["index"]: value for value in v3.rows(directory / "queries.jsonl")}
    truth = {value["label"]: value for value in v3.rows(directory / "truth.jsonl")}
    if len(frames) != case["sampled_frames"] or len(truth) != len(frames):
        raise ValueError(f"Incomplete prepared case: {case['id']}")
    counts, layers, status = Counter(), {layer: Counter() for layer in LAYERS}, Counter()
    origin = None
    offset = 0
    matched = set()
    with (directory / "frame-counts.jsonl").open("w", encoding="utf-8") as output:
        for chunk in demos.iter_chunks(case["source"], chunk_records=16):
            if origin is None:
                origin = int(chunk["time_ms"][0])
            for local, record in enumerate(chunk):
                index = offset + local
                if index not in frames:
                    continue
                frame = frames[index]
                validate_record(record, frame, origin)
                value = truth[frame["label"]]
                matched.add(index)
                status["sampled_frames"] += 1
                status["path_complete_frames"] += int(value["complete"])
                status["path_unavailable_frames"] += int(not value["complete"])
                status["path_not_beyond5m_frames"] += int(value["complete"] and value["length_cm"] <= 500)
                status["corridor_available_frames"] += int(value["corridor_available"])
                status["corridor_unavailable_frames"] += int(not value["corridor_available"])
                status[f"corridor_{value['corridor_status']}_frames"] += 1
                status["raster_support_invalid_frames"] += int(value["raster_evaluated"] and not value["raster_support_valid"])
                status["raster_not_evaluated_frames"] += int(not value["raster_evaluated"])
                status["offmesh_transition_frames"] += int(value["transition_count"] > 0)
                status["corridor_empty_frames"] += int(value["corridor_available"] and not value["native_cells"])
                classified, per_layer = classify_cells(record, value["native_cells"])
                counts.update(classified)
                for layer in layers:
                    layers[layer].update(per_layer[layer])
                output.write(json.dumps(dict(index=index, elapsed_ms=frame["elapsed_ms"], complete=value["complete"],
                                  reason=value.get("reason"), length_cm=value["length_cm"], corridor_available=value["corridor_available"],
                                  counts=classified, layers=per_layer), separators=(",", ":")) + "\n")
            offset += len(chunk)
    if offset != case["decisions"] or matched != frames.keys():
        raise ValueError(f"Incomplete original honest NAVDEMO2 coverage scan: {case['id']}, {offset}/{case['decisions']}")
    summary = dict(case, status=dict(status), coverage=fractions(counts), layers={layer: fractions(value) for layer, value in layers.items()})
    v3.write_json(directory / "summary.json", summary)
    return summary


def aggregate(cases):
    counts, status = Counter(), Counter()
    layers = {layer: Counter() for layer in LAYERS}
    for case in cases:
        counts.update(case["coverage"]["counts"])
        status.update(case["status"])
        for layer in layers:
            layers[layer].update(case["layers"][layer]["counts"])
    return dict(scenarios=len(cases), status=dict(status), coverage=fractions(counts),
                layers={layer: fractions(value) for layer, value in layers.items()})


def aggregate_outcomes(cases):
    """Partition episode outcomes without mixing native follow termination in."""
    for case in cases:
        if case.get("outcome") not in OUTCOMES or type(case.get("outcome_success")) is not bool:
            raise ValueError("Missing honest-v3 outcome classification")
        if (case["outcome"] == "success") != case["outcome_success"]:
            raise ValueError("Contradictory honest-v3 outcome classification")
    return {outcome: aggregate([case for case in cases if case["outcome"] == outcome]) for outcome in OUTCOMES}


def refresh_outcomes(args, cases):
    """Add outcome strata to old prepared queries without recomputing truth."""
    current = {case["id"]: case for case in source_cases(args.task, args.data)}
    if len(cases) != len(current) or {case["id"] for case in cases} != current.keys():
        raise ValueError("Prepared coverage scenario identities changed")
    result = []
    identity = ("id", "goal_kind", "split", "region", "place_id", "source", "cohort", "decisions", "success")
    for case in cases:
        source = current[case["id"]]
        if any(case.get(key) != source.get(key) for key in identity):
            raise ValueError(f"Prepared honest-v3 recording identity changed: {case['id']}")
        result.append(dict(case, **source))
    return result


def analyze(args):
    preparation = v3.read_json(args.output / "preparation.json")
    cases = preparation["cases"]
    if Counter(case["goal_kind"] for case in cases) != EXPECTED:
        raise ValueError("Prepared coverage cohort changed")
    cases = refresh_outcomes(args, cases)
    distribute_truth(args, cases)
    results = []
    for case in cases:
        results.append(analyze_case(args, case))
        print(json.dumps(dict(event="coverage_case", id=case["id"], kind=case["goal_kind"],
                              sampled=results[-1]["status"]["sampled_frames"], cells=results[-1]["coverage"]["denominator_cells"])), flush=True)
    grouped = {kind: aggregate([case for case in results if case["goal_kind"] == kind]) for kind in KINDS}
    regions = {region: aggregate([case for case in results if case.get("region") == region])
               for region in sorted({case["region"] for case in results if case.get("region") is not None})}
    by_kind_region = {kind: {region: aggregate([case for case in results if case["goal_kind"] == kind and case.get("region") == region])
                           for region in sorted({case["region"] for case in results if case["goal_kind"] == kind and case.get("region") is not None})}
                      for kind in KINDS}
    summary = dict(seed=preparation["seed"], sampling=preparation["sampling"], corridor=preparation["requested_corridor"],
                   all=aggregate(results), by_goal_kind=grouped, by_region=regions, by_goal_kind_region=by_kind_region,
                   by_outcome=aggregate_outcomes(results),
                   by_goal_kind_outcome={kind: aggregate_outcomes([case for case in results if case["goal_kind"] == kind]) for kind in KINDS},
                   by_region_outcome={region: aggregate_outcomes([case for case in results if case.get("region") == region]) for region in regions},
                   definitions=dict(danger="any nonzero quantized wall/ledge/drop/water/movable; takes precedence",
                     unknown="no danger, and confidence==0 or free==0; observed confidence>0/free==0 is separately counted",
                     free="no danger, confidence>0 and free>0",
                     false_danger="memory danger AND evaluator truth_passable for this native cell at the path's grounded level",
                     denominator="native corridor cell observations pooled over fixed seeded 1-Hz frames; each cell counted once per frame",
                     near_far="near footprint owns [-800,800) cm on both axes; far cells in that footprint are excluded",
                     height="memory classifications unchanged by height; known memory-vs-truth height differences >100cm counted separately",
                     area_fraction="secondary weighting by native-cell area, near625cm2/far40000cm2; primary fractions weight cells equally",
                     outcome="saved honest-v3 episodes: point/object/approach use native success; follow requires first <=5m contact, positive post-contact time and >=80% within inclusive 2–5m band; no contact is failure",
                     outcome_denominator="all sampled corridor frames of episodes in that outcome stratum, including pre-contact follow frames; original near/far and truth-passable denominators retained"),
                   limitations=["Evaluator-only comparison of saved honest-memory trajectories; not a counterfactual rollout.",
                     "Corridor follows the complete current-pose-to-current-goal Detour path, 5–15m along ground path, radius15cm.",
                     "Paths not extending beyond5m and unavailable paths are explicit frame denominators, excluded from cell fractions.",
                     "The raster samples walkable Detour ground at native cell centers; native coarse cells touched by a corridor may have unpassable centers."])
    v3.write_json(args.output / "metrics.json", summary)
    identity_columns = ["id", "goal_kind", "split", "region", "place_id", "success", "outcome", "outcome_success", "outcome_failure_reason",
                        "first_contact_ms", "follow_observed_ms", "follow_in_band_ms", "follow_fraction"]
    status_columns = ["sampled_frames", "path_unavailable_frames", "path_not_beyond5m_frames", "corridor_available_frames"]
    columns = [*identity_columns, *status_columns,
               "cells", *CLASS_NAMES, "false_danger", "free_fraction", "unknown_fraction", "danger_fraction",
               "false_danger_fraction_all_cells", "false_danger_fraction_truth_passable"]
    with (args.output / "cases.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        for case in results:
            row = {key: case.get(key) for key in identity_columns}
            row.update({key: case["status"].get(key, 0) for key in status_columns})
            row.update({key: case["coverage"]["counts"].get(key, 0) for key in ("cells", *CLASS_NAMES, "false_danger")})
            row.update({f"{key}_fraction": case["coverage"]["fractions"][key] for key in CLASS_NAMES})
            for name in ("false_danger_fraction_all_cells", "false_danger_fraction_truth_passable"):
                row[name] = case["coverage"][name]
            writer.writerow(row)
    print(json.dumps(dict(event="coverage_complete", scenarios=len(results), queries=summary["all"]["status"]["sampled_frames"], output=str(args.output))), flush=True)
    return summary


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--task", type=Path, required=True)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--stage", choices=("all", "prepare", "truth", "analyze"), default="all")
    parser.add_argument("--seed", type=int, default=20102026)
    for field in ("runner", "assets", "save", "cache", "links", "roads", "truth"):
        parser.add_argument(f"--{field}", type=Path)
    args = parser.parse_args(argv)
    if args.stage in ("all", "truth"):
        os.environ.update(v3.module("ryzen_game").game_environment(assets=args.assets))
    for field in ("task", "data", "output"):
        value = getattr(args, field)
        if not value.is_absolute():
            parser.error(f"--{field} must be absolute")
    if args.stage in ("all", "prepare"):
        prepare(args)
    if args.stage in ("all", "truth"):
        query_truth(args)
    if args.stage in ("all", "analyze"):
        analyze(args)


if __name__ == "__main__":
    main()
