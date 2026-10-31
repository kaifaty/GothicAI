#!/usr/bin/env python3
"""Offline evidence for every failed point in the frozen first teacher-v3 take.

prepare extracts bounded tails and full-resolution diagnostic observations;
detour requests static evaluator paths without moving actors; analyze replays
ORIGINAL complete recordings through the production C++ diagnostic executable.
Counterfactual plans describe recorded memory, never a counterfactual rollout.
"""
from __future__ import annotations

import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from copy import copy
import csv
import html
import importlib.util
import json
import math
import os
from pathlib import Path
import subprocess
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


def wrapped(angle):
    return (np.asarray(angle) + 180) % 360 - 180


def local_to_world(points, position, yaw):
    """Recorded [right,forward] centimetres to world XZ; offline debug only."""
    points = np.asarray(points, dtype=float).reshape(-1, 2)
    radians = np.deg2rad(yaw)
    right, forward = points[:, 0], points[:, 1]
    return np.column_stack((-right * np.sin(radians) + forward * np.cos(radians),
                            right * np.cos(radians) + forward * np.sin(radians))) + np.asarray(position)[[0, 2]]


def world_to_local(points, position, yaw):
    points = np.asarray(points, dtype=float).reshape(-1, 2) - np.asarray(position)[[0, 2]]
    radians = np.deg2rad(yaw)
    return np.column_stack((-np.sin(radians) * points[:, 0] + np.cos(radians) * points[:, 1],
                            np.cos(radians) * points[:, 0] + np.sin(radians) * points[:, 1]))


def polyline_length(points):
    points = np.asarray(points, dtype=float).reshape(-1, 2)
    return float(np.linalg.norm(np.diff(points, axis=0), axis=1).sum())


def sample_polyline(points, step=25., maximum=None):
    points = np.asarray(points, dtype=float).reshape(-1, 2)
    if not len(points):
        return points
    lengths = np.linalg.norm(np.diff(points, axis=0), axis=1)
    cumulative = np.r_[0., np.cumsum(lengths)]
    end = min(cumulative[-1], maximum) if maximum is not None else cumulative[-1]
    if end <= 0:
        return points[:1]
    distances = np.unique(np.r_[np.arange(0., end, step), end])
    # np.interp also handles duplicate vertices without inventing extra length.
    return np.column_stack([np.interp(distances, cumulative, points[:, axis]) for axis in (0, 1)])


def lookahead(points, distance=150.):
    sampled = sample_polyline(points, step=distance, maximum=distance)
    return sampled[-1] if len(sampled) else None


def distances_to_path(points, path):
    points = np.asarray(points, dtype=float).reshape(-1, 2)
    path = np.asarray(path, dtype=float).reshape(-1, 2)
    if not len(path):
        return np.full(len(points), np.nan)
    if len(path) == 1:
        return np.linalg.norm(points - path[0], axis=1)
    start, delta = path[:-1], np.diff(path, axis=0)
    squared = np.sum(delta * delta, axis=1)
    result = np.full(len(points), np.inf)
    # Bound temporary memory even for a long Detour corridor.
    for offset in range(0, len(points), 128):
        query = points[offset:offset + 128, None, :] - start[None, :, :]
        t = np.clip(np.sum(query * delta, axis=2) / np.maximum(squared, 1.e-12), 0, 1)
        result[offset:offset + 128] = np.linalg.norm(query - t[..., None] * delta, axis=2).min(axis=1)
    return result


def path_hausdorff(first, second):
    first = sample_polyline(first, 50, 1000)
    second = sample_polyline(second, 50, 1000)
    if not len(first) or not len(second):
        return None
    return float(max(distances_to_path(first, second).max(), distances_to_path(second, first).max()))


def stationary_spans(times, positions, window_ms=3000, motion_cm=50.):
    """Consecutive windows with <=50 cm travelled, including their 3 s onset."""
    times = np.asarray(times, dtype=float)
    positions = np.asarray(positions, dtype=float)
    if not len(times) or len(times) != len(positions) or (np.diff(times) <= 0).any():
        raise ValueError("Invalid motion trace timestamps")
    travelled = np.r_[0., np.cumsum(np.linalg.norm(np.diff(positions[:, [0, 2]], axis=0), axis=1))]
    starts = np.searchsorted(times, times - window_ms, side="left")
    stalled = (times - times[starts] >= window_ms) & (travelled - travelled[starts] <= motion_cm)
    edges = np.diff(np.r_[False, stalled, False].astype(int))
    result = []
    for first, end in zip(np.flatnonzero(edges == 1), np.flatnonzero(edges == -1)):
        result.append(dict(start_ms=float(times[starts[first]]), detected_ms=float(times[first]),
                           end_ms=float(times[end - 1]), duration_ms=float(times[end - 1] - times[starts[first]])))
    return result


def blocked_spans(times, positions, actions, window_ms=3000, displacement_cm=50., commanded_ms=1500):
    """Recorded 100-ms analogue of the production net-motion blocked criterion."""
    times = np.asarray(times, dtype=float)
    positions = np.asarray(positions, dtype=float)
    starts = np.searchsorted(times, times - window_ms, side="left")
    commanded = (actions[:-1, 1] != 0) | (actions[:-1, 2] != 0)
    held = np.r_[0., np.cumsum(np.diff(times) * commanded)]
    displacement = np.linalg.norm(positions[:, [0, 2]] - positions[starts][:, [0, 2]], axis=1)
    blocked = ((times - times[starts] >= window_ms) & (displacement < displacement_cm)
               & (held - held[starts] >= commanded_ms))
    edges = np.diff(np.r_[False, blocked, False].astype(int))
    return [dict(start_ms=float(times[starts[first]]), detected_ms=float(times[first]), end_ms=float(times[end - 1]),
                 duration_ms=float(times[end - 1] - times[starts[first]]))
            for first, end in zip(np.flatnonzero(edges == 1), np.flatnonzero(edges == -1))]


def select_indices(decisions, actions, spans):
    tail_start = max(0, decisions - 100)
    tail = list(range(tail_start, decisions))
    diagnostic = set(range(decisions - 1, max(-1, decisions - 102), -10))
    diagnostic.add(decisions - 1)
    onset = max(spans, key=lambda span: (span["duration_ms"], span["start_ms"])) if spans else None
    onset_time = onset["start_ms"] if onset else float(actions[tail_start, 0])
    onset_indices = {max(0, int(np.searchsorted(actions[:-1, 0], onset_time + delta, side="right") - 1))
                     for delta in (-3000, -2000, -1000, 0)}
    if onset:
        onset_indices.add(min(decisions - 1, int(np.searchsorted(actions[:-1, 0], onset["detected_ms"]))))
    diagnostic.update(onset_indices)
    return tail, sorted(index for index in diagnostic if index >= 0), onset, sorted(onset_indices)


def sign_changes(values):
    signs = np.sign(np.asarray(values))
    signs = signs[signs != 0]
    return int(np.count_nonzero(signs[1:] != signs[:-1]))


def behavior_metrics(actions, trajectory, spans, elapsed_ms):
    tail = actions[:-1][actions[:-1, 0] >= elapsed_ms - 10000]
    path = trajectory[trajectory[:, 0] >= elapsed_ms - 10000]
    intervals = np.diff(actions[:, 0])[-len(tail):]
    masks = dict(forward=tail[:, 1] > 0, backward=tail[:, 1] < 0, strafe=tail[:, 2] != 0,
                 strafe_left=tail[:, 2] < 0, strafe_right=tail[:, 2] > 0,
                 turn_left=tail[:, 3] < 0, turn_right=tail[:, 3] > 0,
                 turn_only=(tail[:, 1:3] == 0).all(axis=1) & (tail[:, 3] != 0),
                 walk=tail[:, 4] != 0, run=tail[:, 4] == 0,
                 idle=(tail[:, [1, 2, 3, 5, 6]] == 0).all(axis=1))
    yaw_delta = wrapped(np.diff(path[:, 4]))
    yaw_nonzero = yaw_delta[np.abs(yaw_delta) >= 1]
    stalled_ms = sum(max(0., min(span["end_ms"], elapsed_ms) - max(span["start_ms"], elapsed_ms - 10000)) for span in spans)
    return dict(decisions=len(tail), duration_ms=float(intervals.sum()),
                action_counts={key: int(mask.sum()) for key, mask in masks.items()},
                action_ms={key: float(intervals[mask].sum()) for key, mask in masks.items()},
                turn_sign_changes=sign_changes(tail[:, 3]), strafe_sign_changes=sign_changes(tail[:, 2]),
                travelled_cm=polyline_length(path[:, [1, 3]]),
                displacement_cm=float(np.linalg.norm(path[-1, [1, 3]] - path[0, [1, 3]])),
                yaw_absolute_deg=float(np.abs(yaw_delta).sum()), yaw_net_deg=float(wrapped(path[-1, 4] - path[0, 4])),
                yaw_velocity_sign_changes=sign_changes(yaw_nonzero), stalled_ms=min(10000., stalled_ms))


def world_plan_metrics(rows):
    """World-coordinate corridor switching; body yaw alone cannot create a flip."""
    states = []
    bearings = []
    hausdorff = []
    previous_path = None
    previous_turn_row = None
    turn_flips = same_corridor_turn_flips = evaluable_turn_flips = 0
    for row in rows:
        points = np.asarray(row["world_plan_xz_cm"], dtype=float).reshape(-1, 2)
        aim = lookahead(points)
        position = np.asarray(row["position_cm"])[[0, 2]]
        goal = np.asarray(row["goal_cm"])[[0, 2]] - position
        turn = row.get("applied_actions", {}).get("turn", 0)
        if turn:
            if previous_turn_row is not None and np.sign(turn) != np.sign(previous_turn_row["turn"]):
                turn_flips += 1
                gap = path_hausdorff(previous_turn_row["path"], points)
                if gap is not None:
                    evaluable_turn_flips += 1
                    if gap <= 50 and np.linalg.norm(previous_turn_row["position"] - position) <= 75:
                        same_corridor_turn_flips += 1
            previous_turn_row = dict(turn=turn, path=points, position=position)
        vector = aim - position if aim is not None else np.zeros(2)
        if np.linalg.norm(vector) < 25 or np.linalg.norm(goal) < 1:
            continue
        bearing = math.degrees(math.atan2(vector[1], vector[0]))
        bearings.append(bearing)
        angle = math.degrees(math.atan2(goal[0] * vector[1] - goal[1] * vector[0], float(goal @ vector)))
        side = int(np.sign(angle)) if abs(angle) >= 20 else 0
        if previous_path is not None:
            hausdorff.append(path_hausdorff(previous_path, points))
        previous_path = points
        if side and (not states or states[-1]["side"] != side):
            states.append(dict(side=side, path=points, index=row["index"], position=position))
    aba = 0
    for a, b, c in zip(states, states[1:], states[2:]):
        ab, ac = path_hausdorff(a["path"], b["path"]), path_hausdorff(a["path"], c["path"])
        displacement = max(np.linalg.norm(a["position"] - b["position"]), np.linalg.norm(a["position"] - c["position"]))
        if (a["side"] == c["side"] != b["side"] and c["index"] - a["index"] <= 30
                and displacement <= 75 and ab is not None and ac is not None and ab >= 100 and ac <= 75):
            aba += 1
    differences = wrapped(np.diff(bearings))
    values = [value for value in hausdorff if value is not None]
    return dict(evaluable_decisions=len(bearings), world_bearing_jumps_ge45=int(np.count_nonzero(np.abs(differences) >= 45)),
                corridor_side_switches=max(0, len(states) - 1), world_path_aba=aba,
                turn_flip_events=turn_flips, turn_flip_evaluable_events=evaluable_turn_flips,
                turn_flip_same_corridor_events=same_corridor_turn_flips,
                successive_path_hausdorff_cm_mean=float(np.mean(values)) if values else None,
                same_corridor_fraction_le50=float(np.mean(np.asarray(values) <= 50)) if values else None,
                criteria=dict(lookahead_cm=150, side_angle_deg=20, ab_hausdorff_min_cm=100,
                              ac_hausdorff_max_cm=75, actor_displacement_max_cm=75, aba_max_ms=3000))


def vector_token(values):
    return "cm:" + ",".join(format(float(value), ".9g") for value in values)


def failure_selection(task, data, expected_points=30, expected_successes=8):
    selected = [value for value in v3.rows(task / "expanded-scenarios.jsonl") if value["goal_kind"] == "point"]
    cases, successes = [], 0
    for value in selected:
        directory = v3.episode_directory(data, value, 3)
        metadata = v3.read_json(directory / "episode.json")
        if not v3.same_scenario(metadata, value, 3):
            raise ValueError(f"Recording does not match the frozen first take: {directory}")
        if metadata["success"]:
            successes += 1
        else:
            cases.append((value, metadata, directory))
    if len(selected) != expected_points or successes != expected_successes:
        raise ValueError(f"First-take preflight: points={len(selected)}/{expected_points}, successes={successes}/{expected_successes}")
    return cases


def aligned_records(directory, wanted, expected_decisions, actions, trajectory):
    wanted = set(wanted)
    records = {}
    offset = 0
    origin = None
    for chunk in demos.iter_chunks(directory, chunk_records=16):
        if origin is None:
            origin = int(chunk["time_ms"][0])
        for local, record in enumerate(chunk):
            index = offset + local
            if index not in wanted:
                continue
            if index >= expected_decisions or int(record["time_ms"]) - origin != actions[index, 0]:
                raise ValueError(f"Observation/action time mismatch: {directory}, index={index}")
            if (trajectory[index, 0] != actions[index, 0]
                    or np.linalg.norm(trajectory[index, 1:4] - record["position_cm"]) > 1):
                raise ValueError(f"Observation/trajectory mismatch: {directory}, index={index}")
            records[index] = record.copy()
        offset += len(chunk)
    if offset != expected_decisions or records.keys() != wanted:
        raise ValueError(f"Incomplete observations: {directory}: records={offset}, selected={len(records)}/{len(wanted)}")
    return records


def write_csv(path, cases):
    fields = ("id", "region", "failure", "elapsed_ms", "final_dead", "final_down", "onset_kind", "stalled_ms",
              "tail_travelled_cm", "tail_displacement_cm", "tail_yaw_absolute_deg", "turn_sign_changes",
              "world_path_aba", "corridor_side_switches", "backward", "strafe", "run", "residual_detour_cm",
              "h1_far_hit_frames", "h2_radius_open_frames", "h3_unknown_direction_frames", "h5_execution_gap_frames")
    with Path(path).open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for case in cases:
            tail = case["tail_behavior"]
            counts = tail["action_counts"]
            hypothesis = case.get("hypotheses", {})
            writer.writerow(dict(id=case["id"], region=case["region"], failure=case["failure"], elapsed_ms=case["elapsed_ms"],
                                 final_dead=case["final_dead"], final_down=case["final_down"], onset_kind=case["onset_kind"],
                                 stalled_ms=tail["stalled_ms"], tail_travelled_cm=tail["travelled_cm"],
                                 tail_displacement_cm=tail["displacement_cm"], tail_yaw_absolute_deg=tail["yaw_absolute_deg"],
                                 turn_sign_changes=tail["turn_sign_changes"], world_path_aba=case["world_plans"]["world_path_aba"],
                                 corridor_side_switches=case["world_plans"]["corridor_side_switches"],
                                 backward=counts["backward"], strafe=counts["strafe"], run=counts["run"],
                                 residual_detour_cm=case.get("residual_detour_cm"),
                                 h1_far_hit_frames=hypothesis.get("H1_far_memory", {}).get("hit_frames"),
                                 h2_radius_open_frames=hypothesis.get("H2_body_margin", {}).get("r0_open_r30_closed"),
                                 h3_unknown_direction_frames=hypothesis.get("H3_unknown_cost", {}).get("direction_changed_frames"),
                                 h5_execution_gap_frames=hypothesis.get("H5_execution", {}).get("gap_ge50_frames")))


def prepare(args):
    cases = failure_selection(args.task, args.data, args.expected_points, args.expected_successes)
    args.output.mkdir(parents=True, exist_ok=True)
    queries, summaries = [], []
    for value, metadata, directory in cases:
        output = args.output / value["id"]
        output.mkdir(parents=True, exist_ok=True)
        actions = np.loadtxt(directory / "actions.txt", ndmin=2)
        trajectory = np.loadtxt(directory / "trajectory.txt", ndmin=2)
        decisions = metadata["decisions"]
        if actions.shape != (decisions + 1, 9) or trajectory.shape != (decisions + 1, 5):
            raise ValueError(f"Unexpected action/trajectory rows: {directory}")
        if actions[-1, 0] != metadata["elapsed_ms"] or not np.array_equal(actions[:, 0], trajectory[:, 0]):
            raise ValueError(f"Action/trajectory time mismatch: {directory}")
        travel_spans = stationary_spans(trajectory[:, 0], trajectory[:, 1:4])
        spans = blocked_spans(trajectory[:, 0], trajectory[:, 1:4], actions)
        tail_indices, diagnostic_indices, onset, onset_indices = select_indices(decisions, actions, spans)
        records = aligned_records(directory, set(tail_indices) | set(diagnostic_indices), decisions, actions, trajectory)
        plan_rows = {}
        count = 0
        for index, plan in enumerate(v3.rows(directory / "teacher-paths.jsonl")):
            count += 1
            if index not in records:
                continue
            record = records[index]
            if (plan["elapsed_ms"] != actions[index, 0] or abs(float(wrapped(plan["yaw_deg"] - record["body_yaw_deg"]))) > .01
                    or np.linalg.norm(np.asarray(plan["position_cm"]) - record["position_cm"]) > 1):
                raise ValueError(f"Teacher-path/observation alignment failed: {directory}, index={index}")
            row = dict(plan, index=index, world_plan_xz_cm=local_to_world(plan["local_plan_cm"], plan["position_cm"], plan["yaw_deg"]).tolist(),
                       applied_actions={name: float(actions[index, column]) for column, name in enumerate(
                           ("forward", "strafe", "turn", "walk", "jump", "climb", "head_yaw_deg", "head_pitch_deg"), 1)},
                       body_flags=record["body"]["flags"].tolist(),
                       goal_distance_cm=float(record["roads"]["goal_distance_cm"]))
            plan_rows[index] = row
        if count != decisions or plan_rows.keys() != records.keys():
            raise ValueError(f"Incomplete teacher path stream: {directory}")
        (output / "tails.jsonl").write_text("".join(json.dumps(plan_rows[index]) + "\n" for index in tail_indices), encoding="utf-8")
        (output / "frames.jsonl").write_text("".join(json.dumps(plan_rows[index]) + "\n" for index in diagnostic_indices), encoding="utf-8")
        (output / "diagnostic-indices.txt").write_text("".join(f"{index}\n" for index in diagnostic_indices), encoding="utf-8")
        np.savez_compressed(output / "memory-frames.npz", indices=np.array(diagnostic_indices),
                            records=np.array([records[index] for index in diagnostic_indices], dtype=demos.RECORD_V2_DTYPE))
        goal = metadata["target_position"]
        queries.append(f"{value['id']}-reference {vector_token(trajectory[0, 1:4])} {vector_token(goal)}")
        for index in diagnostic_indices:
            queries.append(f"{value['id']}-frame{index} {vector_token(records[index]['position_cm'])} {vector_token(goal)}")
        summary = dict(id=value["id"], region=value["region"], start=value["start"], goal=value["goal"], source_episode=str(directory),
                       source_samples=str(directory / "samples.bin.gz"), elapsed_ms=metadata["elapsed_ms"], decisions=decisions,
                       failure=metadata.get("failure"), final_dead=metadata.get("final_dead"), final_down=metadata.get("final_down"),
                       final_hp=metadata.get("final_hp"), target_position=goal, diagnostic_indices=diagnostic_indices,
                       onset_indices=onset_indices, onset=onset, onset_kind="net_displacement_blocked" if onset else "no_commanded_net_stall_detected",
                       stalled_spans=spans, stationary_travel_spans=travel_spans,
                       blocked_criterion=dict(window_ms=3000, net_displacement_lt_cm=50, movement_commanded_min_ms=1500,
                                              sampling_ms=100, timing="rolling recorded windows, not engine substep event timestamps"),
                       tail_behavior=behavior_metrics(actions, trajectory, spans, metadata["elapsed_ms"]),
                       world_plans=world_plan_metrics([plan_rows[index] for index in tail_indices]))
        v3.write_json(output / "case.json", summary)
        summaries.append(summary)
    (args.output / "detour-queries.txt").write_text("\n".join(queries) + "\n", encoding="utf-8")
    v3.write_json(args.output / "extraction.json", dict(selected_points=args.expected_points, successes=args.expected_successes,
                  failures=len(summaries), query_count=len(queries), cases=summaries,
                  memory_resolution=dict(near=[64, 64, 25], far=[100, 100, 200]),
                  diagnostics_replay="Original complete samples.bin.gz; memory-frames.npz is visualization data only"))
    write_csv(args.output / "summary.csv", summaries)
    print(json.dumps(dict(event="prepared", failures=len(summaries), queries=len(queries))))


def detour(args):
    worker = SimpleNamespace(output=args.output / "detour-work", runner=args.runner or args.task / "build/opengothic/gothic-body-runner",
                             assets=args.assets or Path(os.environ.get("GOTHIC_DIR", Path.home() / "gothic-ai/game-settings")), save=args.save or args.task / "input/save.sav",
                             cache=args.cache or args.task / "cache", links=args.links or args.task / "input/accepted-links.txt",
                             roads=args.roads or args.task / "input/places.json", ini=None, npc="BAU_954_MALETH", seed=19102026,
                             physics="cv", watchdog_seconds=900)
    query = args.output / "detour-queries.txt"
    if not query.is_file():
        raise ValueError("Run prepare before requesting Detour plans")
    v3.checked_paths(worker)
    runner = v3.new_runner(worker)
    try:
        v3.runner_scenario(worker, runner, "failure-static-detour", [f"navdemos detour-plans-v3 {query}"],
                           args.output / "detour", worker.seed)
    finally:
        runner.close()


def diagnostic_rows(args, case):
    output = args.output / case["id"]
    result = output / "diagnostics.jsonl"
    if not args.reuse_diagnostics or not result.is_file():
        if args.diagnostics is None or not args.diagnostics.is_file():
            raise ValueError("--diagnostics must name the production C++ diagnostic executable")
        with result.open("w", encoding="utf-8") as stream:
            process = subprocess.run([str(args.diagnostics), case["source_samples"], "--indices", str(output / "diagnostic-indices.txt"), "--paths"],
                                     stdout=stream, stderr=subprocess.PIPE, text=True, check=False)
        if process.returncode:
            raise ValueError(f"C++ diagnostics failed for {case['id']}: {process.stderr[-2000:]}")
    values = list(v3.rows(result))
    summaries = [value for value in values if value.get("type") == "summary"]
    records = {value["index"]: value for value in values if value.get("type") == "record"}
    if (len(summaries) != 1 or summaries[0].get("records") != case["decisions"] or summaries[0].get("mismatches") != 0
            or summaries[0].get("previous_mismatches") != 0 or sorted(records) != case["diagnostic_indices"]):
        raise ValueError(f"Incomplete original-episode diagnostic replay: {case['id']}")
    return records, summaries[0]


def channel_evidence(memory, points, step, heights=None):
    size = len(memory)
    points = np.asarray(points).reshape(-1, 2)
    columns = np.floor(points[:, 0] / step + size / 2).astype(int)
    rows = np.floor(points[:, 1] / step + size / 2).astype(int)
    inside = (columns >= 0) & (columns < size) & (rows >= 0) & (rows < size)
    cells = memory["channels"][rows[inside], columns[inside]]
    unique = np.unique(np.column_stack((rows[inside], columns[inside])), axis=0)
    unique_channels = memory["channels"][unique[:, 0], unique[:, 1]] if len(unique) else np.empty((0, 7), dtype=np.uint8)
    danger = cells[:, 2:].max(axis=1) if len(cells) else np.array([])
    danger_cells = unique_channels[:, 2:].max(axis=1) if len(unique_channels) else np.array([])
    hits = danger > 0
    height_valid = memory["height_valid"][rows[inside], columns[inside]].astype(bool)
    ground = memory["ground_height_cm"][rows[inside], columns[inside]]
    height_difference = np.abs(ground - np.asarray(heights)[inside]) if heights is not None else np.full(len(cells), np.nan)
    return dict(samples=len(points), in_map_samples=int(inside.sum()), danger_samples=int(hits.sum()),
                occupied_cells=int((danger_cells > 0).sum()),
                danger_channel_samples={name: int((cells[:, column] > 0).sum()) for column, name in enumerate(
                    ("wall", "ledge", "drop", "water", "movable"), 2)},
                danger_bins=dict(q1_25=int(((danger > 0) & (danger <= 25)).sum()),
                                 q26_127=int(((danger >= 26) & (danger <= 127)).sum()), q128_255=int((danger >= 128).sum())),
                danger_median=float(np.median(danger[hits])) if hits.any() else None,
                hit_confidence_median=float(np.median(cells[hits, 0])) if hits.any() else None,
                hit_free_median=float(np.median(cells[hits, 1])) if hits.any() else None,
                hit_height_within100_samples=int((hits & height_valid & (height_difference <= 100)).sum()),
                hit_height_mismatch_gt100_samples=int((hits & height_valid & (height_difference > 100)).sum()),
                hit_height_unknown_samples=int((hits & (~height_valid | ~np.isfinite(height_difference))).sum()),
                known_free_samples=int(((cells[:, 0] > 0) & (cells[:, 1] > 0) & ~hits).sum()) if len(cells) else 0)


def memory_corridor(record, world_path):
    world_path = np.asarray(world_path).reshape(-1, 3)
    xz = world_path[:, [0, 2]]
    cumulative = np.r_[0., np.cumsum(np.linalg.norm(np.diff(xz, axis=0), axis=1))]
    sample_distances = np.unique(np.r_[np.arange(0., cumulative[-1], 25), cumulative[-1]]) if len(world_path) else np.array([])
    sampled = np.column_stack([np.interp(sample_distances, cumulative, xz[:, axis]) for axis in (0, 1)]) if len(world_path) else np.empty((0, 2))
    heights = np.interp(sample_distances, cumulative, world_path[:, 1]) if len(world_path) else np.array([])
    local = world_to_local(sampled, record["position_cm"], record["body_yaw_deg"])
    extent = np.max(np.abs(local), axis=1) if len(local) else np.array([])
    near = extent < 800
    far = (extent >= 800) & (extent < 10000)
    boundary = far & (extent <= 1200)
    return dict(near=channel_evidence(record["memory_near"], local[near], 25, heights[near]),
                far=channel_evidence(record["memory_far"], local[far], 200, heights[far]),
                far_boundary_8_12m=channel_evidence(record["memory_far"], local[boundary], 200, heights[boundary]),
                outside_map_samples=int((extent >= 10000).sum()),
                sample_step_cm=25, occupancy_threshold="any nonzero uint8 danger channel; known free requires confidence+free>0 and no danger")


def path_memory_exposure(record, local_path):
    points = sample_polyline(local_path, 25)
    channels = []
    for point in points:
        near = np.max(np.abs(point)) < 800
        size, step, field = (64, 25, "memory_near") if near else (100, 200, "memory_far")
        column, row = np.floor(point / step + size / 2).astype(int)
        if 0 <= row < size and 0 <= column < size:
            channels.append(record[field]["channels"][row, column])
    if not channels:
        return dict(samples=0, unknown_exposure_mean=None, zero_confidence_or_free_fraction=None)
    channels = np.asarray(channels)
    certainty = np.minimum(channels[:, 0], channels[:, 1]).astype(float) / 255
    return dict(samples=len(channels), unknown_exposure_mean=float(np.mean(1 - certainty)),
                zero_confidence_or_free_fraction=float(np.mean((channels[:, 0] == 0) | (channels[:, 1] == 0))),
                danger_fraction=float(np.mean((channels[:, 2:] > 0).any(axis=1))))


def direction_difference(first, second, origin):
    a, b = lookahead(first), lookahead(second)
    if a is None or b is None:
        return None
    a, b = a - origin, b - origin
    if np.linalg.norm(a) < 25 or np.linalg.norm(b) < 25:
        return None
    return abs(math.degrees(math.atan2(a[0] * b[1] - a[1] * b[0], float(a @ b))))


def variant(options, radius=30, unknown=4, near=False, known=False):
    return (options.get("body_radius_cm") == radius and options.get("unknown_cost") == unknown
            and bool(options.get("near_only")) == near and bool(options.get("known_free_only")) == known)


def select_variant(values, radius=30, unknown=4, near=False, known=False, target_kind="steering"):
    return next((value for value in values if value.get("target_kind", "steering") == target_kind
                 and variant(value["options"], radius, unknown, near, known)), None)


def execution_metrics(frame, path, trajectory, actions):
    index = frame["index"]
    end = min(len(trajectory) - 1, index + 10)
    future = trajectory[end, [1, 3]]
    origin = trajectory[index, [1, 3]]
    movement = future - origin
    aim = lookahead(path)
    vector = aim - origin if aim is not None else np.zeros(2)
    movement_cm = float(np.linalg.norm(movement))
    angle = None
    if movement_cm >= 10 and np.linalg.norm(vector) >= 25:
        angle = abs(math.degrees(math.atan2(vector[0] * movement[1] - vector[1] * movement[0], float(vector @ movement))))
    gap = float(distances_to_path([future], path)[0]) if len(path) else None
    next_index = min(index + 1, len(trajectory) - 1)
    next_point = trajectory[next_index, [1, 3]]
    local_delta = world_to_local([next_point], trajectory[index, 1:4], trajectory[index, 4])[0]
    # NpcKeys prioritizes strafe over forward. Preserve the observed sign rather
    # than assuming the engine implements the same right-positive convention.
    expected = np.array([actions[index, 2], 0.]) if actions[index, 2] else np.array([0., actions[index, 1]])
    alignment = None
    if np.linalg.norm(local_delta) >= 1 and np.linalg.norm(expected) > 0:
        alignment = float(expected @ local_delta / (np.linalg.norm(expected) * np.linalg.norm(local_delta)))
    return dict(interval_ms=float(trajectory[end, 0] - trajectory[index, 0]), displacement_cm=movement_cm,
                travelled_cm=polyline_length(trajectory[index:end + 1, [1, 3]]), future_distance_to_plan_cm=gap,
                motion_aim_angle_deg=angle, movement_requested=bool(actions[index, 1] or actions[index, 2]),
                action_interval_ms=float(trajectory[next_index, 0] - trajectory[index, 0]),
                next_delta_local_right_forward_cm=local_delta.tolist(), commanded_right_forward=expected.tolist(),
                next_delta_command_cosine=alignment)


def connectivity_summary(evidence, target_kind):
    base_key = "goal_production" if target_kind == "goal" else "production"
    base = [item[base_key] for item in evidence]
    known = [select_variant(item["variants"], known=True, target_kind=target_kind) for item in evidence]
    return dict(frames=len(base), beyond_known_frames=sum(bool(item["beyond_known"]) for item in evidence),
                exact_target_reached=sum(value["actual_target_reached"] for value in base),
                snapped_node_only=sum(value["plan_complete"] and not value["actual_target_reached"] for value in base),
                target_outside_map=sum(value["target_outside_map"] for value in base),
                target_clamped=sum(value.get("target_clamped", False) for value in base),
                start_not_found=sum(not value["start_found"] for value in base),
                known_free_evaluable_frames=sum(value is not None for value in known),
                known_free_exact_target_reached=sum(value is not None and value["actual_target_reached"] for value in known),
                optimistic_exact_but_not_known_free=sum(value["actual_target_reached"] and restricted is not None
                    and not restricted["actual_target_reached"] for value, restricted in zip(base, known)))


def unknown_cost_summary(evidence):
    result = {}
    for cost in (1, 2, 4, 8, 16):
        values = [select_variant(item["variants"], unknown=cost) for item in evidence]
        values = [value for value in values if value is not None]
        fields = dict(unknown_exposure_mean=[value["memory_exposure"].get("unknown_exposure_mean") for value in values],
                      zero_confidence_or_free_fraction=[value["memory_exposure"].get("zero_confidence_or_free_fraction") for value in values],
                      plan_length_vs_production=[value.get("plan_length_vs_production") for value in values],
                      direction_vs_recorded_plan_deg=[value.get("direction_vs_recorded_plan_deg") for value in values],
                      direction_vs_final_goal_detour_deg=[value.get("direction_vs_final_goal_detour_deg") for value in values],
                      plan_length_over_final_goal_detour=[value.get("plan_length_over_final_goal_detour") for value in values])
        summary = dict(frames=len(values), exact_target_reached=sum(value["actual_target_reached"] for value in values))
        for name, numbers in fields.items():
            present = [number for number in numbers if number is not None]
            summary[name] = dict(frames=len(present), mean=float(np.mean(present)) if present else None)
        result[str(cost)] = summary
    return result


def analyze_case(args, case, detours):
    output = args.output / case["id"]
    diagnostics, replay = diagnostic_rows(args, case)
    frames = {frame["index"]: frame for frame in v3.rows(output / "frames.jsonl")}
    archive = np.load(output / "memory-frames.npz", allow_pickle=False)
    records = {int(index): record for index, record in zip(archive["indices"], archive["records"])}
    trajectory = np.loadtxt(Path(case["source_episode"]) / "trajectory.txt", ndmin=2)
    actions = np.loadtxt(Path(case["source_episode"]) / "actions.txt", ndmin=2)
    reference = detours.get(f"{case['id']}-reference")
    reference_xyz = np.asarray(reference.get("corners_cm", []), dtype=float).reshape(-1, 3) if reference else np.empty((0, 3))
    reference_path = reference_xyz[:, [0, 2]]
    evidence = []
    for index in case["diagnostic_indices"]:
        frame, record = frames[index], records[index]
        values = diagnostics[index]["diagnostics"]
        production = select_variant(values)
        goal_production = select_variant(values, target_kind="goal")
        if production is None or goal_production is None:
            raise ValueError(f"Missing production steering/goal diagnostic variant for {case['id']}, frame={index}")
        query = detours.get(f"{case['id']}-frame{index}")
        current_xyz = np.asarray(query.get("corners_cm", []), dtype=float).reshape(-1, 3) if query else np.empty((0, 3))
        current_path = current_xyz[:, [0, 2]]
        world_path = np.asarray(frame["world_plan_xz_cm"]).reshape(-1, 2)
        item = dict(index=index, elapsed_ms=frame["elapsed_ms"], beyond_known=frame["beyond_known"],
                    production={key: value for key, value in production.items() if key != "path"},
                    goal_production={key: value for key, value in goal_production.items() if key != "path"},
                    detour=query, execution=execution_metrics(frame, world_path, trajectory, actions),
                    residual_corridor=memory_corridor(record, current_xyz) if query and query.get("complete") else None,
                    reference_corridor=memory_corridor(record, reference_xyz) if reference and reference.get("complete") else None,
                    variants=[])
        for value in values:
            local_path = value.get("path", [])
            world = local_to_world(local_path, record["position_cm"], record["body_yaw_deg"])
            origin = record["position_cm"][[0, 2]]
            angle = direction_difference(world_path, world, origin)
            same_detour_target = frame["beyond_known"] or value.get("target_kind") == "goal"
            item["variants"].append(dict({key: entry for key, entry in value.items() if key != "path"},
                                          direction_vs_recorded_plan_deg=angle,
                                          direction_vs_final_goal_detour_deg=direction_difference(current_path, world, origin) if query and query.get("complete") else None,
                                          plan_length_over_final_goal_detour=value["plan_length_cm"] / query["length_cm"]
                                          if same_detour_target and query and query.get("complete") and query["length_cm"] > 0 else None,
                                          memory_exposure=path_memory_exposure(record, local_path),
                                          path_hausdorff_vs_recorded_cm=path_hausdorff(world_path, world),
                                          plan_length_vs_production=value["plan_length_cm"] / production["plan_length_cm"]
                                          if production["plan_length_cm"] > 0 else None))
        evidence.append(item)
    h1 = [item for item in evidence if item["residual_corridor"] is not None]
    h2 = [item for item in evidence if not item["production"]["target_outside_map"] and not item["production"].get("target_clamped")]
    h2_open = [item for item in h2 if not item["production"]["actual_target_reached"]
               and (select_variant(item["variants"], radius=0) or {}).get("actual_target_reached")]
    h3 = [item for item in evidence if any(value.get("direction_vs_recorded_plan_deg") is not None
                                          and value.get("target_kind", "steering") == "steering"
                                          and variant(value["options"], 30, value["options"]["unknown_cost"])
                                          for value in item["variants"])]
    h3_changed = [item for item in h3 if any((value.get("direction_vs_recorded_plan_deg") or 0) >= 45
                                            and (value.get("path_hausdorff_vs_recorded_cm") or 0) >= 100
                                            for value in item["variants"] if value.get("target_kind", "steering") == "steering"
                                            and variant(value["options"], 30, value["options"]["unknown_cost"]))]
    h5 = [item for item in evidence if item["production"]["actual_target_reached"] and item["execution"]["interval_ms"] >= 1000]
    goal_h2 = [item for item in evidence if not item["goal_production"]["target_outside_map"] and not item["goal_production"].get("target_clamped")]
    case = copy(case)
    case["diagnostic_replay"] = replay
    case["hypotheses"] = dict(
        H1_far_memory=dict(evaluable_frames=len(h1), hit_frames=sum(item["residual_corridor"]["far"]["danger_samples"] > 0 for item in h1),
                           boundary_hit_frames=sum(item["residual_corridor"]["far_boundary_8_12m"]["danger_samples"] > 0 for item in h1),
                           note="Detour corridor intersects remembered occupied coarse cells; does not establish physical or causal closure"),
        H2_body_margin=dict(evaluable_frames=len(h2), r0_open_r30_closed=len(h2_open),
                            r15_open_r30_closed=sum(not item["production"]["actual_target_reached"] and
                                (select_variant(item["variants"], radius=15) or {}).get("actual_target_reached", False) for item in h2),
                            target_within_near_frames=sum(bool(np.max(np.abs(item["production"]["target"])) < 800) for item in h2),
                            goal_evaluable_frames=len(goal_h2),
                            goal_r0_open_r30_closed=sum(not item["goal_production"]["actual_target_reached"] and
                                (select_variant(item["variants"], radius=0, target_kind="goal") or {}).get("actual_target_reached", False) for item in goal_h2),
                            goal_r15_open_r30_closed=sum(not item["goal_production"]["actual_target_reached"] and
                                (select_variant(item["variants"], radius=15, target_kind="goal") or {}).get("actual_target_reached", False) for item in goal_h2),
                            note="Hybrid-grid radius sensitivity; far cells are also dilated, so this alone does not prove a near doorway closure"),
        H3_unknown_cost=dict(evaluable_frames=len(h3), direction_changed_frames=len(h3_changed),
                             note="Offline alternative paths only; no causal rollout claim"),
        H4_replanning=dict(**case["world_plans"], turn_flips_same_corridor=case["world_plans"]["turn_flip_same_corridor_events"]),
        H5_execution=dict(evaluable_frames=len(h5), gap_ge50_frames=sum((item["execution"]["future_distance_to_plan_cm"] or 0) >= 50 for item in h5),
                           alignment_gt45_frames=sum((item["execution"]["motion_aim_angle_deg"] or 0) > 45 for item in h5),
                           motion_under5_requested_frames=sum(item["execution"]["displacement_cm"] < 5 and item["execution"]["movement_requested"] for item in h5),
                           note="Spatial execution proxies with an exact memory-target route, not proof of corner cutting"))
    residuals = [item["detour"]["length_cm"] for item in evidence if item["detour"] and item["detour"].get("complete")
                 and item["index"] >= case["decisions"] - 100]
    case["residual_detour_cm"] = residuals[-1] if residuals else None
    case["tail_detour_progress_cm"] = residuals[0] - residuals[-1] if len(residuals) >= 2 else None
    if reference is not None and reference.get("complete"):
        tail = trajectory[trajectory[:, 0] >= case["elapsed_ms"] - 10000, :]
        gaps = distances_to_path(tail[:, [1, 3]], reference_path)
        case["tail_distance_to_original_detour_cm"] = dict(mean=float(np.mean(gaps)), maximum=float(np.max(gaps)))
    case["frames"] = evidence
    case["steering_connectivity"] = connectivity_summary(evidence, "steering")
    case["goal_connectivity"] = connectivity_summary(evidence, "goal")
    case["unknown_cost_sensitivity"] = unknown_cost_summary(evidence)
    alignments = [item["execution"]["next_delta_command_cosine"] for item in evidence
                  if item["execution"]["next_delta_command_cosine"] is not None]
    case["action_motion_alignment"] = dict(evaluable_frames=len(alignments),
                                           opposite_direction_frames=sum(value < 0 for value in alignments),
                                           mean_cosine=float(np.mean(alignments)) if alignments else None,
                                           interval="command at t versus physical delta[t,t+100ms], final interval may be shorter")
    v3.write_json(output / "analysis.json", case)
    return case


def svg_memory(record, field, step, world_plan, detour_path):
    memory = record[field]
    size = len(memory)
    channels = memory["channels"]
    danger = channels[..., 2:].max(axis=2) > 0
    known = (channels[..., 0] > 0) & (channels[..., 1] > 0) & ~danger
    colors = [(known, "#deeadb"), (danger, "#666666"), ((channels[..., 3] > 0) | (channels[..., 4] > 0), "#b34d48"),
              (channels[..., 5] > 0, "#668eae")]
    content = []
    for mask, color in colors:
        indices = np.argwhere(mask)
        commands = "".join(f"M{column},{size - row - 1}h1v1h-1z" for row, column in indices)
        content.append(f'<path d="{commands}" fill="{color}"/>')
    for path, color in ((detour_path, "#247c47"), (world_plan, "#e78b22")):
        local = world_to_local(path, record["position_cm"], record["body_yaw_deg"])
        points = " ".join(f"{right / step + size / 2:.2f},{size / 2 - forward / step:.2f}" for right, forward in local)
        content.append(f'<polyline points="{points}" fill="none" stroke="{color}" stroke-width=".65"/>')
    content.append(f'<circle cx="{size / 2}" cy="{size / 2}" r="1" fill="#172230"/>')
    return f'<svg viewBox="0 0 {size} {size}" width="260" height="260" overflow="hidden">' + "".join(content) + '</svg>'


def render_case(args, case):
    output = args.output / case["id"]
    archive = np.load(output / "memory-frames.npz", allow_pickle=False)
    index = case["diagnostic_indices"][-1]
    record = archive["records"][list(archive["indices"]).index(index)]
    frame = next(value for value in v3.rows(output / "frames.jsonl") if value["index"] == index)
    selected = next(value for value in case["frames"] if value["index"] == index)
    world = np.asarray(frame["world_plan_xz_cm"]).reshape(-1, 2)
    detour_value = selected.get("detour")
    detour_path = np.asarray(detour_value.get("corners_cm", []), dtype=float).reshape(-1, 3)[:, [0, 2]] if detour_value else np.empty((0, 2))
    title = html.escape(f"{case['id']} · {case['region']} · t={frame['elapsed_ms'] / 1000:g}s")
    svg = f'<svg xmlns="http://www.w3.org/2000/svg" width="800" height="325" viewBox="0 0 800 325"><rect width="800" height="325" fill="white"/><text x="12" y="20" font-family="sans-serif" font-size="13">{title}</text>'
    trajectory = np.loadtxt(Path(case["source_episode"]) / "trajectory.txt", ndmin=2)
    tail = trajectory[trajectory[:, 0] >= case["elapsed_ms"] - 10000][:, [1, 3]]
    bounds = np.concatenate([path for path in (tail, world, detour_path) if len(path)])
    low, high = bounds.min(axis=0), bounds.max(axis=0)
    scale = 240 / max(200, float((high - low).max()))
    for path, color in ((detour_path, "#247c47"), (world, "#e78b22"), (tail, "#172230")):
        points = " ".join(f"{12 + (x - low[0]) * scale:.2f},{282 - (z - low[1]) * scale:.2f}" for x, z in path)
        svg += f'<polyline points="{points}" fill="none" stroke="{color}" stroke-width="2"/>'
    svg += '<text x="12" y="305" font-family="sans-serif" font-size="11">world: actual tail / memory plan / Detour</text>'
    svg += '<g transform="translate(270,35)">' + svg_memory(record, "memory_near", 25, world, detour_path) + '</g>'
    svg += '<g transform="translate(535,35)">' + svg_memory(record, "memory_far", 200, world, detour_path) + '</g>'
    svg += '<text x="280" y="315" font-family="sans-serif" font-size="11">near 64×64, 25 cm</text><text x="545" y="315" font-family="sans-serif" font-size="11">far 100×100, 200 cm</text></svg>'
    (args.output / f"{case['id']}.svg").write_text(svg, encoding="utf-8")


def parse_coordinate(value):
    if not value.startswith("cm:"):
        raise ValueError(f"Expected coordinate query, got {value}")
    result = np.asarray([float(number) for number in value[3:].split(",")])
    if result.shape != (3,) or not np.isfinite(result).all():
        raise ValueError(f"Invalid coordinate query: {value}")
    return result


def checked_detours(query_path, result_path):
    if not Path(result_path).is_file():
        raise ValueError(f"Missing Detour results: {result_path}")
    queries = {}
    for number, line in enumerate(Path(query_path).read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        fields = line.split()
        if len(fields) != 3 or fields[0] in queries:
            raise ValueError(f"Duplicate or malformed Detour query at line {number}")
        queries[fields[0]] = parse_coordinate(fields[1]), parse_coordinate(fields[2])
    values = {}
    for value in v3.rows(result_path):
        label = value["label"]
        if label in values:
            raise ValueError(f"Duplicate Detour result label: {label}")
        values[label] = value
    if values.keys() != queries.keys():
        raise ValueError(f"Detour label mismatch: missing={sorted(queries.keys() - values.keys())}, extra={sorted(values.keys() - queries.keys())}")
    for label, value in values.items():
        for field, expected in zip(("start_cm", "goal_cm"), queries[label]):
            actual = np.asarray(value.get(field), dtype=float)
            if actual.shape != (3,) or not np.isfinite(actual).all() or np.linalg.norm(actual - expected) > 1:
                raise ValueError(f"Detour {field} mismatch for {label}: {value.get(field)} versus {expected.tolist()}")
        if not isinstance(value.get("complete"), bool):
            raise ValueError(f"Missing Detour completion status for {label}")
        if value["complete"]:
            corners = np.asarray(value.get("corners_cm"), dtype=float)
            length = value.get("length_cm")
            if (corners.ndim != 2 or corners.shape[1] != 3 or not len(corners) or not np.isfinite(corners).all()
                    or not isinstance(length, (int, float)) or not math.isfinite(length) or length < 0):
                raise ValueError(f"Invalid completed Detour path for {label}")
    return values


def analyze(args):
    extraction = v3.read_json(args.output / "extraction.json")
    detour_path = args.detour_plans or args.output / "detour/detour-plans-v3.jsonl"
    detours = checked_detours(args.output / "detour-queries.txt", detour_path)
    with ThreadPoolExecutor(max_workers=args.parallel) as executor:
        cases = list(executor.map(lambda case: analyze_case(args, case, detours), extraction["cases"]))
    hypotheses = {}
    for name in ("H1_far_memory", "H2_body_margin", "H3_unknown_cost", "H5_execution"):
        fields = sorted({key for case in cases for key, value in case["hypotheses"][name].items() if isinstance(value, (int, float))})
        hypotheses[name] = {key: sum(case["hypotheses"][name].get(key, 0) for case in cases) for key in fields}
        hypotheses[name]["episodes_with_evaluable_frames"] = sum(case["hypotheses"][name]["evaluable_frames"] > 0 for case in cases)
    hypotheses["H4_replanning"] = dict(episodes_with_world_path_aba=sum(case["world_plans"]["world_path_aba"] > 0 for case in cases),
                                       world_path_aba=sum(case["world_plans"]["world_path_aba"] for case in cases),
                                       episodes_with_turn_flips_same_corridor=sum(case["hypotheses"]["H4_replanning"]["turn_flips_same_corridor"] > 0 for case in cases))
    connectivity = {kind: {key: sum(case[f"{kind}_connectivity"][key] for case in cases)
                            for key in cases[0][f"{kind}_connectivity"]} for kind in ("steering", "goal")} if cases else {}
    figures = sorted(cases, key=lambda case: (-case["world_plans"]["world_path_aba"], -case["tail_behavior"]["stalled_ms"], case["id"]))[:args.figures]
    for case in figures:
        render_case(args, case)
    result = dict(selected_points=extraction["selected_points"], successes=extraction["successes"], failures=len(cases),
                  analyzed_failures=len(cases), diagnosed_frames=sum(len(case["frames"]) for case in cases),
                  replayed_decisions=sum(case["diagnostic_replay"]["records"] for case in cases),
                  replay_mismatches=sum(case["diagnostic_replay"]["mismatches"] for case in cases),
                  actor_down_failures=sum(bool(case["final_down"] or case["final_dead"]) for case in cases),
                  no_commanded_net_stall_detected=sum(case["onset"] is None for case in cases),
                  detour_queries_available=len(detours), hypotheses=hypotheses, connectivity=connectivity,
                  figures=[f"{case['id']}.svg" for case in figures], cases=cases,
                  limitations=["plan_complete reaches a snapped node; only actual_target_reached denotes the exact memory target",
                               "near-only target proxies cannot establish connectivity to a goal outside the near map",
                               "optimistic routes can traverse unknown cells; observed known-free connectivity is reported separately",
                               "counterfactual plans and execution alignment are offline evidence, not causal rollout results"])
    v3.write_json(args.output / "metrics.json", result)
    write_csv(args.output / "summary.csv", cases)
    print(json.dumps({key: value for key, value in result.items() if key not in ("cases", "limitations")}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "detour", "analyze"))
    for name in ("task", "data", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--diagnostics", type=Path)
    parser.add_argument("--detour-plans", type=Path)
    parser.add_argument("--parallel", type=int, choices=(1, 2, 3), default=3)
    parser.add_argument("--figures", type=int, default=6)
    parser.add_argument("--reuse-diagnostics", action="store_true")
    parser.add_argument("--expected-points", type=int, default=30)
    parser.add_argument("--expected-successes", type=int, default=8)
    for name in ("runner", "assets", "save", "cache", "links", "roads"):
        parser.add_argument("--" + name, type=Path)
    args = parser.parse_args()
    if args.command == "detour":
        os.environ.update(v3.module("ryzen_game").game_environment(assets=args.assets))
    for name in ("task", "data", "output"):
        if not getattr(args, name).is_absolute():
            parser.error(f"--{name} must be absolute")
        setattr(args, name, getattr(args, name).resolve())
    if not args.output.is_relative_to(args.task):
        parser.error("--output must belong to this task directory")
    if not 0 <= args.figures <= 6:
        parser.error("--figures must be between 0 and 6")
    globals()[args.command](args)


if __name__ == "__main__":
    main()
