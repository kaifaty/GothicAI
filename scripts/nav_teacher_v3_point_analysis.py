#!/usr/bin/env python3
"""Bounded offline evidence for the frozen second point attempt.

Reads saved observations and exact diagnostic counters only. This script does
not invoke a game, change a teacher, run C++ probes, or create a report.
"""
from __future__ import annotations

import argparse
from collections import Counter
import csv
import importlib.util
import json
import math
from pathlib import Path
import time

import numpy as np

_SPEC = importlib.util.spec_from_file_location("nav_teacher_v3_failures", Path(__file__).with_name("nav_teacher_v3_failures.py"))
failures = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(failures)
v3, demos = failures.v3, failures.demos

THRESHOLDS = dict(corridor_radius_cm=15, near_limit_cm=800, ignore_body_prefix_cm=100,
                  corridor_sample_step_cm=25, unknown_plan_fraction=.5,
                  classification_positive_frames=2, classification_frame_fraction=.5,
                  endpoint_pass_distance_cm=100, familiar_endpoint_verification_error_cm=100,
                  familiar_lookahead_cm=600,
                  blocked_window_ms=3000, blocked_net_displacement_lt_cm=50, blocked_commanded_min_ms=1500)


def cumulative_path(trajectory):
    positions = np.asarray(trajectory, dtype=float)[:, [1, 3]]
    return np.r_[0., np.cumsum(np.linalg.norm(np.diff(positions, axis=0), axis=1))]


def segment_summary(trajectory, beyond):
    beyond = np.asarray(beyond, dtype=bool)
    cumulative = cumulative_path(trajectory)
    if len(cumulative) != len(beyond) + 1:
        raise ValueError("Trajectory must include the final held action endpoint")
    edges = np.diff(np.r_[False, beyond, False].astype(int))
    segments = [dict(start_index=int(start), end_index=int(end), path_m=float((cumulative[end]-cumulative[start])/100))
                for start, end in zip(np.flatnonzero(edges == 1), np.flatnonzero(edges == -1))]
    total = float(cumulative[-1]/100)
    length = float(sum(s["path_m"] for s in segments))
    return dict(total_path_m=total, beyond_path_m=length, beyond_path_fraction=length/total if total else 0.,
                beyond_segments=segments)


def distance_summary(trajectory, beyond, endpoint, onset_index):
    trajectory = np.asarray(trajectory, dtype=float)
    cumulative = cumulative_path(trajectory)
    segments = segment_summary(trajectory, beyond)["beyond_segments"]
    handoff = segments[-1]["start_index"] if segments else None
    onset_index = int(onset_index) if onset_index is not None else None
    if onset_index is not None and not 0 <= onset_index < len(beyond):
        raise ValueError("Invalid onset index")
    result = dict(last_handoff_index=handoff,
                  after_handoff_path_m=float((cumulative[-1]-cumulative[handoff])/100) if handoff is not None else None,
                  onset_after_handoff_path_m=float((cumulative[onset_index]-cumulative[handoff])/100)
                    if handoff is not None and onset_index is not None and onset_index >= handoff else None,
                  terminal_endpoint_distance_m=None, onset_endpoint_distance_m=None, endpoint_pass_index=None,
                  after_endpoint_pass_path_m=None, onset_after_endpoint_pass_path_m=None, closest_endpoint_distance_m=None)
    if endpoint is None:
        return result
    positions = trajectory[:, [1, 3]]
    endpoint = np.asarray(endpoint, dtype=float)[[0, 2]]
    distances = np.linalg.norm(positions-endpoint, axis=1)
    # Unsigned XZ separation is distinct from recorded path after an observed
    # <=1-m approach. Never invent a signed 'beyond node' coordinate.
    result.update(terminal_endpoint_distance_m=float(distances[-1]/100),
                  onset_endpoint_distance_m=float(distances[onset_index]/100) if onset_index is not None else None,
                  closest_endpoint_distance_m=float(distances.min()/100))
    contacts = np.flatnonzero(distances <= THRESHOLDS["endpoint_pass_distance_cm"])
    if len(contacts):
        index = int(contacts[0])
        result.update(endpoint_pass_index=index, after_endpoint_pass_path_m=float((cumulative[-1]-cumulative[index])/100),
                      onset_after_endpoint_pass_path_m=float((cumulative[onset_index]-cumulative[index])/100)
                        if onset_index is not None and onset_index >= index else None)
    return result


def classify_evidence(frames):
    evaluated = [f for f in frames if f.get("evaluable", True) and f.get("role") != "handoff"]
    count = len(evaluated)
    near = sum(bool(f.get("near_blocking")) for f in evaluated)
    local = sum(bool(f.get("local_start_problem")) for f in evaluated)
    frontier = sum(bool(f.get("frontier_supported")) for f in evaluated)
    near_fraction = near/count if count else None
    frontier_fraction = frontier/count if count else None
    positive = lambda hits: count > 0 and hits >= 2 and hits/count >= .5
    is_near, is_frontier = positive(near), positive(frontier)
    classification = ("mixed" if is_frontier and (is_near or local > 0) else "near_visible_obstacle" if is_near
                      else "memory_frontier" if is_frontier else "undetermined")
    return dict(classification=classification, near_visible_frames=near, frontier_frames=frontier, frames=count,
                near_fraction=near_fraction, frontier_fraction=frontier_fraction, local_start_problem_frames=local)


def memory_values(record, points):
    points = np.asarray(points, dtype=float).reshape(-1, 2)
    channels = np.zeros((len(points), 7), dtype=np.uint8)
    inside = np.zeros(len(points), dtype=bool)
    for near, size, step, name in ((True, 64, 25, "memory_near"), (False, 100, 200, "memory_far")):
        mask = (np.max(np.abs(points), axis=1) < 800) == near
        indices = np.floor(points[mask]/step + size/2).astype(int)
        valid = (indices >= 0).all(axis=1) & (indices < size).all(axis=1)
        locations = np.flatnonzero(mask)[valid]
        channels[locations] = record[name]["channels"][indices[valid, 1], indices[valid, 0]]
        inside[locations] = True
    danger = (channels[:, 2:] > 0).any(axis=1)
    return channels, inside, danger


def near_corridor(record, points):
    """Circle versus near occupied squares on a 25-cm sampled first corridor.

    This is geometric evidence, not a replacement planner. The first metre is
    excluded so the already diagnosed raw-body overlap cannot label a whole
    failure as a remote visible obstacle.
    """
    points = np.asarray(points, dtype=float).reshape(-1, 2)
    if len(points):
        distance = np.linalg.norm(points, axis=1)
        points = points[(distance >= 100) & (distance <= 800) & (np.max(np.abs(points), axis=1) < 800)]
    radius = THRESHOLDS["corridor_radius_cm"]
    hit_cells, hit_samples = set(), 0
    channels = record["memory_near"]["channels"]
    for right, forward in points:
        low = np.maximum(0, np.floor((np.array([right, forward])-radius)/25+32).astype(int))
        high = np.minimum(63, np.floor((np.array([right, forward])+radius)/25+32).astype(int))
        hit = False
        for row in range(int(low[1]), int(high[1])+1):
            for column in range(int(low[0]), int(high[0])+1):
                if not channels[row, column, 2:].any():
                    continue
                center = (np.array([column, row])+.5-32)*25
                delta = np.maximum(0, np.abs(np.array([right, forward])-center)-12.5)
                if float(delta @ delta) <= radius*radius:
                    hit = True;hit_cells.add((row, column))
        hit_samples += hit
    return dict(samples=len(points), blocked_samples=hit_samples, occupied_cells=len(hit_cells))


def snapshot_evidence(record, local_plan=None, diagnostic=None):
    goal = np.asarray(record["roads"]["goal_direction"])[[0, 2]].astype(float)*float(record["roads"]["goal_distance_cm"])
    distance = float(np.linalg.norm(goal))
    ray_end = goal*min(1., 800/distance) if distance > 0 else goal
    straight = failures.sample_polyline([[0, 0], ray_end], 25)
    direct = near_corridor(record, straight)
    path = np.asarray(local_plan if local_plan is not None else [], dtype=float).reshape(-1, 2)
    sampled = failures.sample_polyline(path, 25) if len(path) else np.empty((0, 2))
    first_path = failures.sample_polyline(path, 25, 800) if len(path) else np.empty((0, 2))
    path_near = near_corridor(record, first_path)
    ch, inside, danger = memory_values(record, sampled)
    # Unknown is current absence of memory, not proof that the NPC never saw it.
    unknown = ((ch[:, 0] == 0) | (ch[:, 1] == 0)) & ~danger
    unknown_fraction = float(unknown.mean()) if len(sampled) else None
    goal_channels, goal_inside, goal_danger = memory_values(record, [goal])
    goal_unknown = bool(not goal_inside[0] or goal_channels[0, 0] == 0)
    optimistic = bool(diagnostic and (diagnostic.get("actual_target_reached") or
                       (diagnostic.get("plan_complete") and diagnostic.get("target_clamped"))))
    if diagnostic is None and len(path):
        optimistic = bool(np.linalg.norm(path[-1]-goal) < 1)
    beyond = bool(record["roads"]["beyond_known"])
    raw_body_danger = bool(record["memory_near"]["channels"][32, 32, 2:].any())
    start_found = diagnostic.get("start_found") if diagnostic is not None else None
    escape_required = diagnostic.get("start_escape_required") if diagnostic is not None else None
    local_start_problem = bool(raw_body_danger or start_found is False or escape_required is True)
    frontier = bool(beyond and goal_unknown and unknown_fraction is not None and unknown_fraction >= .5
                    and optimistic and not local_start_problem)
    eye = record["eye"]
    fresh_eye = ((eye["hit"] != 0) & (eye["distance_cm"] <= 800) & (eye["near_age_ms"] <= 200)
                 & ((eye["normal_y"] < .3) | np.isin(eye["kind"], [3, 4, 5])))
    return dict(evaluable=bool(distance > 1), beyond_known=beyond, goal_distance_m=distance/100,
                near_blocking=bool(direct["occupied_cells"] or path_near["occupied_cells"]),
                local_start_problem=local_start_problem, raw_body_cell_danger=raw_body_danger,
                start_found=start_found, start_escape_required=escape_required,
                overlap_danger_cells=diagnostic.get("overlap_danger_cells") if diagnostic is not None else None,
                frontier_supported=frontier, direct_near_corridor=direct, planned_near_corridor=path_near,
                plan_samples=len(sampled), plan_unknown_fraction=unknown_fraction,
                plan_outside_map_fraction=float((~inside).mean()) if len(sampled) else None,
                plan_danger_fraction=float(danger.mean()) if len(sampled) else None,
                goal_in_memory=bool(goal_inside[0]), goal_currently_unknown=goal_unknown,
                goal_danger=bool(goal_danger[0]), goal_confidence=int(goal_channels[0, 0]) if goal_inside[0] else None,
                optimistic_goal_or_proxy_connected=optimistic, fresh_eye_obstacles=int(fresh_eye.sum()),
                diagnostic_available=diagnostic is not None)


def project_polyline(point, points):
    points = np.asarray(points, dtype=float).reshape(-1, 2)
    point = np.asarray(point, dtype=float).reshape(2)
    if not len(points):
        return None
    if len(points) == 1:
        return dict(distance_cm=float(np.linalg.norm(point-points[0])), remaining_cm=0.)
    delta = np.diff(points, axis=0)
    lengths = np.linalg.norm(delta, axis=1)
    t = np.clip(np.sum((point-points[:-1])*delta, axis=1)/np.maximum(lengths*lengths, 1.e-12), 0, 1)
    distances = np.linalg.norm(point-(points[:-1]+t[:, None]*delta), axis=1)
    nearest = int(np.argmin(distances))
    remaining = float((1-t[nearest])*lengths[nearest] + lengths[nearest+1:].sum())
    return dict(distance_cm=float(distances[nearest]), remaining_cm=remaining)


def matched_endpoint(reference, previous_known_record):
    routes = [row.get("points_cm", []) for row in v3.rows(Path(reference)/"known-paths.jsonl")]
    routes = [np.asarray(route, dtype=float).reshape(-1, 3) for route in routes if len(route)]
    result = dict(source="matched_v2_familiar_endpoint", verified=False, endpoint_cm=None,
                  candidate_endpoint_count=len({tuple(route[-1]) for route in routes}), reason="no_previous_known_observation")
    if previous_known_record is None or not routes:
        if not routes:
            result["reason"] = "v2_has_no_saved_familiar_polyline"
        return result
    roads = previous_known_record["roads"]
    carrot = np.asarray(roads["carrot_direction"])[[0, 2]]*float(roads["carrot_distance_cm"])
    world = failures.local_to_world([carrot], previous_known_record["position_cm"], previous_known_record["body_yaw_deg"])[0]
    expected = max(0., float(roads["remaining_known_cm"])-600)
    candidates = []
    for route in routes:
        projection = project_polyline(world, route[:, [0, 2]])
        residual = abs(projection["remaining_cm"]-expected)
        if projection["distance_cm"] <= 100 and residual <= 100:
            candidates.append((projection["distance_cm"]+residual, route[-1], projection, residual))
    endpoints = {tuple(candidate[1]) for candidate in candidates}
    if len(endpoints) != 1:
        result["reason"] = "unmatched_or_ambiguous_v3_carrot"
        return result
    _, endpoint, projection, residual = min(candidates, key=lambda item: item[0])
    result.update(verified=True, endpoint_cm=endpoint.tolist(), reason="v3_carrot_projects_to_v2_polyline_with_matching_remaining_length",
                  carrot_projection_error_cm=projection["distance_cm"], remaining_length_error_cm=residual)
    return result


def selected_records(directory, wanted, trajectory, decisions):
    """One decompression pass; retain only onset, tail and last handoff maps."""
    records, labels = {}, []
    offset, previous_flag, previous_record = 0, False, None
    latest_handoff = None
    latest_previous = None
    origin = None
    for chunk in demos.iter_chunks(directory, chunk_records=16):
        if origin is None:
            origin = int(chunk["time_ms"][0])
        flags = chunk["roads"]["beyond_known"].astype(bool)
        end = offset+len(chunk)
        if end > decisions or not np.array_equal(chunk["time_ms"]-origin, trajectory[offset:end, 0]):
            raise ValueError(f"Observation/trajectory time mismatch: {directory}")
        if (np.linalg.norm(chunk["position_cm"]-trajectory[offset:end, 1:4], axis=1) > 1).any():
            raise ValueError(f"Observation/trajectory position mismatch: {directory}")
        for local in np.flatnonzero(flags & ~np.r_[previous_flag, flags[:-1]]):
            latest_handoff = offset+int(local)
            latest_previous = (chunk[local-1] if local else previous_record)
            latest_previous = latest_previous.copy() if latest_previous is not None else None
            records[latest_handoff] = chunk[local].copy()
        for index in wanted:
            if offset <= index < end:
                records[index] = chunk[index-offset].copy()
        labels.append(flags.copy())
        previous_flag = bool(flags[-1]);previous_record = chunk[-1]
        offset = end
    if offset != decisions or not set(wanted).issubset(records):
        raise ValueError(f"Incomplete recorded observations: {directory}")
    # Prior handoffs are small in practice; expose only the last one plus wanted.
    records = {index: record for index, record in records.items() if index in wanted or index == latest_handoff}
    if latest_handoff is None and previous_record is not None and not previous_flag:
        latest_previous = previous_record.copy()  # Still on familiar roads at termination.
    return records, np.concatenate(labels), latest_handoff, latest_previous


def load_diagnostics(path, decisions):
    values = list(v3.rows(path))
    summaries = [row for row in values if row.get("type") == "summary"]
    if (len(summaries) != 1 or summaries[0].get("records") != decisions or summaries[0].get("mismatches") != 0
            or summaries[0].get("previous_mismatches") != 0 or summaries[0].get("counterfactual", False)):
        raise ValueError(f"Invalid exact retry replay: {path}")
    summary = summaries[0]
    if summary.get("steering_start_evaluable_records") != decisions:
        raise ValueError(f"Missing all-decision exact start counters: {path}")
    frames = {}
    for row in values:
        if row.get("type") != "record":
            continue
        goal = [v for v in row["diagnostics"] if v.get("name") == "production-goal"]
        if len(goal) != 1:
            raise ValueError(f"Missing production-goal diagnostic: {path}")
        frames[row["index"]] = goal[0]
    return frames, summary


def reference_stats(directory, expected):
    metadata = v3.read_json(Path(directory)/"episode.json")
    if (metadata.get("teacher_version") != 2 or metadata.get("success") is not True
            or (metadata.get("start"), metadata.get("goal")) != (expected["start"], expected["goal"])):
        raise ValueError(f"Mismatched successful v2 reference: {directory}")
    trajectory = np.loadtxt(Path(directory)/"trajectory.txt", ndmin=2)
    rows = list(v3.rows(Path(directory)/"teacher-paths.jsonl"))
    if len(trajectory) != metadata["decisions"]+1 or not rows:
        raise ValueError(f"Incomplete v2 trajectory/road labels: {directory}")
    # traceV2Plan deduplicates geometry, but its signature contains beyondKnown:
    # every boolean transition is emitted. Its previous-row value is therefore
    # an exact interval label, not interpolation of a continuous measurement.
    trace_times = np.asarray([row["elapsed_ms"] for row in rows], dtype=float)
    decision_times = trajectory[:-1, 0]
    if (trace_times[0] != 0 or (np.diff(trace_times) <= 0).any()
            or not np.isin(trace_times, decision_times).all()):
        raise ValueError(f"Invalid sparse v2 road-label transitions: {directory}")
    labels = np.asarray([row["beyond_known"] for row in rows], dtype=bool)
    assignment = np.searchsorted(trace_times, decision_times, side="right")-1
    beyond = labels[assignment]
    result = segment_summary(trajectory, beyond)
    result.update(last_beyond_segment_m=result["beyond_segments"][-1]["path_m"] if result["beyond_segments"] else None,
                  initial_remaining_known_m=float(rows[0]["remaining_known_cm"])/100 if rows else None,
                  detour_length_m=float(metadata["detour_length_cm"])/100 if metadata.get("detour_length_cm") else None,
                  elapsed_ms=metadata["elapsed_ms"], decisions=metadata["decisions"])
    if result["beyond_segments"]:
        index = result["beyond_segments"][-1]["start_index"]
        goal = np.asarray(metadata["target_position"], dtype=float)[[0, 2]]
        result["goal_distance_at_last_handoff_m"] = float(np.linalg.norm(goal-trajectory[index, [1, 3]])/100)
        result["remaining_known_at_last_handoff_m"] = float(rows[int(assignment[index])]["remaining_known_cm"])/100
    else:
        result["goal_distance_at_last_handoff_m"] = None
        result["remaining_known_at_last_handoff_m"] = None
    result["road_label_source"] = "exact_piecewise_bool_from_sparse_trace_signature"
    return result


def analyze_case(task, data, value):
    directory = data/"v3-retry"/"point"/value["split"]/value["id"]
    metadata = v3.read_json(directory/"episode.json")
    if not v3.same_scenario(metadata, value, 3):
        raise ValueError(f"Second-take scenario identity mismatch: {directory}")
    trajectory = np.loadtxt(directory/"trajectory.txt", ndmin=2)
    actions = np.loadtxt(directory/"actions.txt", ndmin=2)
    decisions = metadata["decisions"]
    if (trajectory.shape != (decisions+1, 5) or actions.shape != (decisions+1, 9)
            or not np.array_equal(trajectory[:, 0], actions[:, 0]) or trajectory[-1, 0] != metadata["elapsed_ms"]):
        raise ValueError(f"Second-take action/trajectory mismatch: {directory}")
    spans = failures.blocked_spans(trajectory[:, 0], trajectory[:, 1:4], actions)
    _, selected, onset, onset_indices = failures.select_indices(decisions, actions, spans)
    onset_index = int(np.searchsorted(actions[:-1, 0], onset["start_ms"], side="right")-1) if onset else None
    tail = set(range(decisions-1, max(-1, decisions-102), -10))
    wanted = (set(selected) if onset is not None else set()) | tail
    records, beyond, handoff, previous_known = selected_records(directory, wanted, trajectory, decisions)
    diagnostic, exact = load_diagnostics(task/"retry-diagnostics/new"/f"{value['id']}.jsonl", decisions)
    local_plans = {}
    plan_count = 0
    for index, line in enumerate((directory/"teacher-paths.jsonl").open(encoding="utf-8")):
        plan_count += 1
        if index in records:
            plan = json.loads(line)
            if plan["elapsed_ms"] != actions[index, 0]:
                raise ValueError(f"Teacher plan/action mismatch: {value['id']}, {index}")
            local_plans[index] = plan["local_plan_cm"]
    if plan_count != decisions or local_plans.keys() != records.keys():
        raise ValueError(f"Incomplete teacher plans: {directory}")
    endpoint = matched_endpoint(value["teacher_v2_episode"], previous_known)
    distances = distance_summary(trajectory, beyond, endpoint["endpoint_cm"] if endpoint["verified"] else None, onset_index)
    route = segment_summary(trajectory, beyond)
    route["last_beyond_segment_m"] = route["beyond_segments"][-1]["path_m"] if route["beyond_segments"] else None
    frames = []
    for index in sorted(records):
        record = records[index]
        evidence = snapshot_evidence(record, local_plans[index], diagnostic.get(index))
        role = "onset" if index in onset_indices and onset is not None else "tail" if index in tail else "handoff"
        frames.append(dict(index=index, elapsed_ms=float(actions[index, 0]), role=role, **evidence))
    onset_record = records.get(onset_index) if onset_index is not None else None
    onset_frame = next((frame for frame in frames if frame["index"] == onset_index), None)
    onset_context = classify_evidence([frame for frame in frames if frame["role"] == "onset"])
    tail_context = classify_evidence([frame for frame in frames if frame["role"] == "tail"])
    terminal_record = records[decisions-1]
    return dict(id=value["id"], region=value["region"], start=value["start"], goal=value["goal"], seed=value["seed"],
                success=bool(metadata["success"]), failure=metadata.get("failure"), decisions=decisions,
                elapsed_ms=metadata["elapsed_ms"], exact_start=dict(start_not_found=exact["steering_start_found_false"],
                    decisions=exact["steering_start_evaluable_records"], escape_required=exact["steering_escape_required_records"]),
                onset=onset, onset_index=onset_index, onset_kind="longest_commanded_net_stall" if onset else "no_commanded_net_stall",
                onset_remaining_known_m=float(onset_record["roads"]["remaining_known_cm"])/100 if onset_record is not None else None,
                terminal_remaining_known_m=float(terminal_record["roads"]["remaining_known_cm"])/100,
                onset_goal_distance_m=float(onset_record["roads"]["goal_distance_cm"])/100 if onset_record is not None else None,
                terminal_goal_distance_m=float(terminal_record["roads"]["goal_distance_cm"])/100,
                familiar_endpoint=endpoint, distances=distances, actual_v3=route,
                v2_reference=reference_stats(value["teacher_v2_episode"], value),
                memory_evidence=classify_evidence(frames), onset_context=onset_context, tail_context=tail_context,
                first_onset_evidence=onset_frame, frames=frames)


def distribution(values):
    values = [float(value) for value in values if value is not None and math.isfinite(value)]
    return dict(n=len(values), p25=float(np.percentile(values, 25)) if values else None,
                median=float(np.median(values)) if values else None, p75=float(np.percentile(values, 75)) if values else None,
                maximum=max(values) if values else None)


def summarize(cases, retry):
    distributions = {}
    for name, group in (("successes", [c for c in cases if c["success"]]), ("failures", [c for c in cases if not c["success"]])):
        fields = dict(actual_v3_beyond_path_m=[c["actual_v3"]["beyond_path_m"] for c in group],
                      actual_v3_last_beyond_segment_m=[c["actual_v3"]["last_beyond_segment_m"] for c in group],
                      actual_v3_total_path_m=[c["actual_v3"]["total_path_m"] for c in group],
                      v2_beyond_reference_path_m=[c["v2_reference"]["beyond_path_m"] for c in group],
                      v2_last_beyond_reference_segment_m=[c["v2_reference"]["last_beyond_segment_m"] for c in group],
                      v2_total_reference_path_m=[c["v2_reference"]["total_path_m"] for c in group],
                      v2_goal_distance_at_last_handoff_m=[c["v2_reference"]["goal_distance_at_last_handoff_m"] for c in group],
                      onset_node_unsigned_distance_m=[c["distances"]["onset_endpoint_distance_m"] for c in group],
                      terminal_node_unsigned_distance_m=[c["distances"]["terminal_endpoint_distance_m"] for c in group],
                      onset_after_handoff_recorded_path_m=[c["distances"]["onset_after_handoff_path_m"] for c in group],
                      terminal_after_handoff_recorded_path_m=[c["distances"]["after_handoff_path_m"] for c in group],
                      onset_after_node_approach_recorded_path_m=[c["distances"]["onset_after_endpoint_pass_path_m"] for c in group],
                      terminal_after_node_approach_recorded_path_m=[c["distances"]["after_endpoint_pass_path_m"] for c in group])
        distributions[name] = {field: distribution(values) for field, values in fields.items()}
    total = sum(c["exact_start"]["decisions"] for c in cases)
    missing = sum(c["exact_start"]["start_not_found"] for c in cases)
    failed = [c for c in cases if not c["success"]]
    return dict(episodes=len(cases), successes=sum(c["success"] for c in cases), failures=len(failed),
                start_not_found=dict(count=missing, denominator=total, fraction=missing/total if total else None,
                                    cached_retry_metrics=retry.get("start_diagnostics", {}).get("start_not_found", {})),
                failed_memory_evidence=dict(Counter(c["memory_evidence"]["classification"] for c in failed)),
                failed_onset_context_evidence=dict(Counter(c["onset_context"]["classification"] for c in failed)),
                failed_tail_context_evidence=dict(Counter(c["tail_context"]["classification"] for c in failed)),
                failed_exact_first_onset=dict(evaluable=sum(c["first_onset_evidence"] is not None for c in failed),
                    goal_currently_unknown=sum(bool(c["first_onset_evidence"] and c["first_onset_evidence"]["goal_currently_unknown"]) for c in failed),
                    near_blocking=sum(bool(c["first_onset_evidence"] and c["first_onset_evidence"]["near_blocking"]) for c in failed),
                    optimistic_goal_or_proxy_connected=sum(bool(c["first_onset_evidence"] and c["first_onset_evidence"]["optimistic_goal_or_proxy_connected"]) for c in failed),
                    frontier_supported=sum(bool(c["first_onset_evidence"] and c["first_onset_evidence"]["frontier_supported"]) for c in failed)),
                late_frontier_without_first_onset_support=sum(c["tail_context"]["classification"] in ("memory_frontier", "mixed")
                    and not (c["first_onset_evidence"] and c["first_onset_evidence"]["frontier_supported"]) for c in failed),
                endpoint_verified_cases=sum(c["familiar_endpoint"]["verified"] for c in cases),
                failed_local_start_evidence=dict(
                    first_onset_raw_body_danger=sum(bool(c["first_onset_evidence"] and c["first_onset_evidence"]["raw_body_cell_danger"]) for c in failed),
                    onset_context_start_not_found_episodes=sum(any(f["role"] == "onset" and f["start_found"] is False for f in c["frames"]) for c in failed),
                    onset_context_start_diagnostic_evaluable_episodes=sum(any(f["role"] == "onset" and f["start_found"] is not None for f in c["frames"]) for c in failed),
                    tail_start_not_found_episodes=sum(any(f["role"] == "tail" and f["start_found"] is False for f in c["frames"]) for c in failed),
                    tail_start_diagnostic_evaluable_episodes=sum(any(f["role"] == "tail" and f["start_found"] is not None for f in c["frames"]) for c in failed),
                    tail_escape_required_episodes=sum(any(f["role"] == "tail" and f["start_escape_required"] is True for f in c["frames"]) for c in failed)),
                failed_with_observed_endpoint_approach=sum(c["distances"]["endpoint_pass_index"] is not None for c in failed),
                distributions=distributions, thresholds=THRESHOLDS,
                interpretation=dict(node_distance="Unsigned XZ distance to matched v2 terminal familiar node, only when the v3 carrot and remaining length verify the same endpoint.",
                    node_trajectory="Recorded path after first observed <=1m endpoint approach; null means no observed passage, not zero beyond-node distance.",
                    handoff="The permanent beyond_known handoff is up to 6m BEFORE the familiar endpoint. Path after handoff is not path after the node.",
                    memory="Unknown now may include forgotten observations. Near-visible means remembered near danger in a 15cm sampled corridor, not proof of current line of sight. Classification is evidence, not an experimentally established failure cause.",
                    onset="First-onset flags are separate from onset-context and tail classifications: late loss of memory is not evidence for the original cause of a stall.",
                    frontier="Requires beyond_known, currently unknown goal, >=50% unknown free-space samples on the actual optimistic plan and a connected optimistic final goal/proxy. Local raw-body danger, exact start_not_found or required escape suppress frontier support in that frame. A frontier-majority case with other local start-problem frames is mixed. Mixed and undetermined are retained."),
                cases=cases)


def write_cases_csv(path, cases):
    columns = ("id", "region", "success", "failure", "classification", "onset_context_classification", "tail_classification", "first_onset_goal_unknown",
               "first_onset_near_blocking", "first_onset_frontier_supported", "onset_ms", "endpoint_verified", "endpoint_reason",
               "onset_endpoint_distance_m", "terminal_endpoint_distance_m", "onset_after_handoff_path_m", "after_handoff_path_m",
               "endpoint_pass_index", "onset_after_endpoint_pass_path_m", "after_endpoint_pass_path_m",
               "v3_beyond_path_m", "v3_total_path_m", "v2_beyond_path_m", "v2_total_path_m", "v2_goal_distance_at_handoff_m",
               "start_not_found", "decisions", "near_visible_frames", "frontier_frames", "evaluable_frames")
    with Path(path).open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        for case in cases:
            row = {key: case[key] for key in ("id", "region", "success", "failure", "decisions")}
            first_onset = case["first_onset_evidence"]
            row.update(classification=case["memory_evidence"]["classification"],
                       onset_context_classification=case["onset_context"]["classification"], tail_classification=case["tail_context"]["classification"],
                       first_onset_goal_unknown=first_onset["goal_currently_unknown"] if first_onset else None,
                       first_onset_near_blocking=first_onset["near_blocking"] if first_onset else None,
                       first_onset_frontier_supported=first_onset["frontier_supported"] if first_onset else None, onset_ms=case["onset"]["start_ms"] if case["onset"] else None,
                       endpoint_verified=case["familiar_endpoint"]["verified"], endpoint_reason=case["familiar_endpoint"]["reason"],
                       v3_beyond_path_m=case["actual_v3"]["beyond_path_m"], v3_total_path_m=case["actual_v3"]["total_path_m"],
                       v2_beyond_path_m=case["v2_reference"]["beyond_path_m"], v2_total_path_m=case["v2_reference"]["total_path_m"],
                       v2_goal_distance_at_handoff_m=case["v2_reference"]["goal_distance_at_last_handoff_m"],
                       start_not_found=case["exact_start"]["start_not_found"], near_visible_frames=case["memory_evidence"]["near_visible_frames"],
                       frontier_frames=case["memory_evidence"]["frontier_frames"], evaluable_frames=case["memory_evidence"]["frames"])
            row.update({key: case["distances"][key] for key in columns if key in case["distances"]})
            writer.writerow(row)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("task", "data", "output"):
        parser.add_argument("--"+name, type=Path, required=True)
    options = parser.parse_args()
    if any(not getattr(options, key).is_absolute() for key in ("task", "data", "output")):
        parser.error("All paths must be absolute")
    options.task, options.data, options.output = options.task.resolve(), options.data.resolve(), options.output.resolve()
    if not options.output.is_relative_to(options.task):
        parser.error("--output must stay inside --task")
    started = time.monotonic()
    selected = [value for value in v3.rows(options.task/"expanded-scenarios.jsonl") if value["goal_kind"] == "point"]
    if len(selected) != 30 or len({v["id"] for v in selected}) != 30:
        raise ValueError("Point analysis requires exactly the frozen 30 unique point IDs")
    retry = v3.read_json(options.task/"retry-metrics.json")
    metadata = [v3.read_json(options.data/"v3-retry"/"point"/value["split"]/value["id"]/"episode.json") for value in selected]
    if (sum(bool(row.get("success")) for row in metadata) != 10 or any(value["seed"] != 19102026 for value in selected)
            or any(not v3.same_scenario(row, value, 3) for row, value in zip(metadata, selected))):
        raise ValueError("Preflight needs the exact frozen seed19102026 second cohort with 10/30 successes")
    cases = []
    for value in selected:
        case = analyze_case(options.task, options.data, value)
        cases.append(case)
        print(json.dumps(dict(event="point_analysis_case", id=case["id"], success=case["success"],
                              memory_evidence=case["memory_evidence"]["classification"], endpoint_verified=case["familiar_endpoint"]["verified"])), flush=True)
    if sum(c["success"] for c in cases) != 10:
        raise ValueError("Second point cohort must contain exactly 10/30 successes")
    summary = summarize(cases, retry)
    options.output.mkdir(parents=True, exist_ok=True)
    v3.write_json(options.output/"summary.json", summary)
    write_cases_csv(options.output/"cases.csv", cases)
    print(json.dumps(dict(event="point_analysis_complete", wall_s=time.monotonic()-started,
                          **{key: value for key, value in summary.items() if key != "cases"}), ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
