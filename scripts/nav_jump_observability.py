#!/usr/bin/env python3
"""Offline NAVDEMO v2 jump/climb/drop visibility, using learner_input() only.

    python3 -I scripts/nav_jump_observability.py /absolute/nav-demos \
        --metadata /absolute/routes.json --output /absolute/visibility.json \
        --csv /absolute/decisions.csv

Metadata is optional JSON (a list, or {"episodes": [...]}/{"routes": [...]}) or
CSV. Rows identify an episode by episode/directory/id, or by start and goal, and
may contain site_id, transition_id and split (train/validation/test). Existing
episode.json fields are also accepted. No split is invented: unsplit data never
fits the rule. The JSON explicitly states limitations of reconstructing ray
geometry; a visible surface sample is not a measurement of an obstacle's top.
--successful-only restricts sensor/rule statistics to transition_success=true AND
raw goal success, with --success-criterion (default demo_goal_action_landing_side_v1).
All inputs still contribute to tally_all, including failed transitions. Outcomes
come only from original episode.json, never overridden by route metadata.
This fits only small descriptive threshold rules, never a navigation model.
"""
from __future__ import annotations

import argparse
import csv
from collections import Counter
from functools import lru_cache
import json
from pathlib import Path
import sys
from typing import Any

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nav_demos


INPUT_NAMES = frozenset(("body", "roads", "previous_keys", "eye", "memory_near", "memory_far"))
PARTS = ("train", "validation", "test", "unsplit")
DEMO_CRITERION = "demo_goal_action_landing_side_v1"
MEMORY_THRESHOLDS = ((.10, "p10"), (.25, "p25"), (.50, "p50"))
LABELS = ("jump", "climb", "jump_onset", "climb_onset", "drop_onset", "drop_navdemo_onset", "drop_native_onset", "drop_navdemo_filtered_onset", "drop_navdemo_rejected_native_action", "drop_navdemo_unconfirmed_onset", "drop_navdemo_matched_native", "drop_navdemo_duplicate_run", "native_motion", "ordinary_walk", "walk", "run")
LIMITATIONS = [
    "NAVDEMO v2 omits eye origin, head roll and cached ray poses: exact ray hit height above feet cannot be recovered.",
    "Reported eye geometry assumes zero head roll and no horizontal eye offset; only fresh rays are used.",
    "Feet height is the nearest valid remembered ground, not privileged actor position. Eye height is inferred from fresh ground rays and matching memory cells.",
    "Visible hit height is a surface sample, not an obstacle top; height is unknown when ground rays cannot anchor eye height.",
    "Movement direction uses observed horizontal velocity >=20cm/s; stationary bodies use body-forward as an explicit estimate of a prospective forward action, independently of the carrot. Current teacher keys never select input rays.",
    "Drop labels use actual dynamics (100ms NAVDEMO or exact-id physical frames) only as outcomes, never as rule inputs; short falls can be absent from 100ms flags.",
    "Raw learner_input maps are inspected before the policy's block pooling/quantization; visibility here does not guarantee the trained network retained the signal.",
    "The eye-age filter is conservative: the policy encoder does not encode age_ms, and cached far rays lack their old pose in NAVDEMO.",
    "Memory ledge means remembered ground >=50cm above feet; drop means lower ground or clearance. Ledge alone is not used to assert a drop.",
    "Ordinary walking controls require movement, no jump/climb, no body flags or physical native jump/climb/air/fall at the decision tick, and exclude 800ms around labelled drops; FPR applies to this stated control set.",
]


def _angle(value: np.ndarray) -> np.ndarray:
    return (value + 180) % 360 - 180


@lru_cache(maxsize=4)
def _memory_geometry(size: int, cell_cm: float):
    z, x = np.indices((size, size), dtype=float)
    z, x = (z + .5 - size / 2)*cell_cm, (x + .5 - size / 2)*cell_cm
    distance = np.hypot(z, x)
    return distance, np.rad2deg(np.arctan2(x, z)), np.argsort(distance.ravel(), kind="stable")


def _reference(memory: np.ndarray, cell_cm: float) -> tuple[float, bool]:
    order = _memory_geometry(memory.shape[0], cell_cm)[2]
    known = order[memory["height_valid"].ravel()[order] != 0]
    return (float(memory["ground_height_cm"].ravel()[known[0]]), True) if len(known) else (0., False)


def ray_directions(layout: np.ndarray, head_yaw_deg: float, head_pitch_deg: float) -> np.ndarray:
    """Body x=right,y=up,z=forward; zero-roll approximation to EyePose basis.

    Compose rotations (adding pitch/yaw separately is incorrect off-axis).
    NavigationEye uses forward*cos(v)*cos(h)+up*sin(v)+right*cos(v)*sin(h).
    """
    yaw, pitch = np.deg2rad([head_yaw_deg, head_pitch_deg])
    forward = np.array([np.sin(yaw)*np.cos(pitch), np.sin(pitch), np.cos(yaw)*np.cos(pitch)])
    right = np.array([np.cos(yaw), 0., -np.sin(yaw)])
    up = np.cross(forward, right)
    h, v = np.deg2rad(np.asarray(layout)[:, :2]).T
    return (np.cos(v)*np.cos(h))[:, None]*forward + np.sin(v)[:, None]*up + (np.cos(v)*np.sin(h))[:, None]*right


def _eye_height(memory: np.ndarray, points: np.ndarray, ground: np.ndarray,
                reference: float, reference_known: bool) -> tuple[float, int, float]:
    """Anchor ray y to memory ground; do not assume a fixed 180 cm head height."""
    if not reference_known:
        return np.nan, 0, np.nan
    size = memory.shape[0]
    col = np.floor(points[:, 0] / 25 + size / 2).astype(int)
    row = np.floor(points[:, 2] / 25 + size / 2).astype(int)
    inside = ground & (col >= 0) & (col < size) & (row >= 0) & (row < size)
    ids = np.flatnonzero(inside)
    cells = memory[row[ids], col[ids]]
    ids = ids[cells["height_valid"] != 0]
    estimates = memory["ground_height_cm"][row[ids], col[ids]] - reference - points[ids, 1]
    estimates = estimates[(estimates >= 40) & (estimates <= 260)]
    if len(estimates) < 3:
        return np.nan, len(estimates), np.nan
    spread = float(np.percentile(estimates, 75) - np.percentile(estimates, 25))
    if spread > 50:
        return np.nan, len(estimates), spread
    return float(np.median(estimates)), len(estimates), spread


def _memory_ahead(memory: np.ndarray, cell_cm: float, movement_deg: float,
                  reference: float, known: bool) -> dict[str, float]:
    distance, angle, _ = _memory_geometry(memory.shape[0], cell_cm)
    ahead = (distance >= 25) & (distance <= 600) & (np.abs(_angle(angle - movement_deg)) <= 20)
    cells, distances = memory[ahead], distance[ahead]
    result = {}
    for name in ("ledge", "drop"):
        probability = cells["channels"][..., nav_demos.CHANNELS.index(name)].astype(float) / 255
        candidates = probability > 0
        result[name] = float(np.max(probability, initial=0))
        result[f"{name}_distance_cm"] = float(np.min(distances[candidates])) if candidates.any() else np.nan
        # A confidence and distance must belong to the SAME cell. Pairing a
        # near weak trace with a distant strong one would invent evidence.
        if name == "drop":
            for threshold, token in MEMORY_THRESHOLDS:
                mask = probability >= threshold
                result[f"drop_distance_{token}_cm"] = float(np.min(distances[mask])) if mask.any() else np.nan
    heights = cells["ground_height_cm"][cells["height_valid"] != 0] - reference
    result["min_ground_height_cm"] = float(heights.min()) if known and len(heights) else np.nan
    result["max_ground_height_cm"] = float(heights.max()) if known and len(heights) else np.nan
    return result


def sensor_features(inputs: dict[str, np.ndarray], layout: np.ndarray,
                    max_ray_age_ms: int = 0) -> list[dict[str, Any]]:
    """Extract descriptive features from the strict learner boundary, never labels.

    Null/NaN heights mean unavailable geometry, never a fabricated zero height.
    Output is small: raw sensor maps are discarded after each input chunk.
    """
    if frozenset(inputs) != INPUT_NAMES:
        raise ValueError("sensor_features accepts only nav_demos.learner_input() fields")
    if inputs["roads"].dtype != nav_demos.ROADS_V2_DTYPE:
        raise ValueError("NAVDEMO v2 is required")
    layout = np.asarray(layout, dtype=float)
    if layout.shape != (nav_demos.RAY_COUNT, 3) or not np.isfinite(layout).all() or (layout[:, 2] <= 0).any():
        raise ValueError("invalid ray layout")
    if max_ray_age_ms < 0:
        raise ValueError("max_ray_age_ms must be nonnegative")
    result = []
    for i in range(len(inputs["body"])):
        body, roads, eye = inputs["body"][i], inputs["roads"][i], inputs["eye"][i]
        velocity = body["velocity_cm_s"]
        carrot = roads["carrot_direction"]
        speed = float(np.hypot(velocity[0], velocity[2]))
        direction, source = (velocity, "velocity") if speed >= 20 else (np.array([0., 0., 1.]), "body_forward_stationary")
        if np.hypot(direction[0], direction[2]) < 1e-6:
            direction, source = np.array([0., 0., 1.]), "body_forward"
        movement = float(np.rad2deg(np.arctan2(direction[0], direction[2])))
        carrot_angle = float(np.rad2deg(np.arctan2(carrot[0], carrot[2]))) if np.hypot(carrot[0], carrot[2]) > 1e-6 else np.nan
        near, far = inputs["memory_near"][i], inputs["memory_far"][i]
        reference, known = _reference(near, 25)
        if not known:
            reference, known = _reference(far, 200)
        directions = ray_directions(layout, body["head_yaw_deg"], body["head_pitch_deg"])
        points = directions * eye["distance_cm"][:, None]
        distance = np.hypot(points[:, 0], points[:, 2])
        fresh = eye["age_ms"] <= max_ray_age_ms
        hit = fresh & (eye["hit"] != 0) & np.isin(eye["kind"], [1, 2, 3])
        ground = hit & np.isin(eye["kind"], [1, 2]) & (eye["normal_y"] >= .7) & (directions[:, 1] < -.05)
        eye_height, anchors, spread = _eye_height(near, points, ground, reference, known)
        ray_angle = np.rad2deg(np.arctan2(directions[:, 0], directions[:, 2]))
        ahead = np.abs(_angle(ray_angle - movement)) <= 15
        visible = hit & ahead & (np.abs(eye["normal_y"]) < .7)
        # Heights refer to sampled vertical surfaces, never to their unseen top.
        height = points[:, 1] + eye_height
        obstacle = visible & (height >= 25) & (height <= 300)
        nearest = int(np.argmin(np.where(obstacle, distance, np.inf))) if obstacle.any() else -1
        nearest_visible = int(np.argmin(np.where(visible, distance, np.inf))) if visible.any() else -1
        landing = hit & ahead & (eye["normal_y"] >= .7) & (height < -50)
        drop_ray = int(np.argmin(np.where(landing, distance, np.inf))) if landing.any() else -1
        row = dict(movement_deg=movement, movement_source=source, speed_cm_s=speed,
                   carrot_angle_deg=carrot_angle,
                   carrot_movement_angle_deg=float(_angle(carrot_angle - movement)),
                   fresh_rays=int(fresh.sum()), ahead_hit_rays=int((hit & ahead).sum()),
                   eye_height_proxy_cm=eye_height, eye_height_anchors=anchors,
                   eye_height_iqr_cm=spread, ground_reference_known=bool(known),
                   obstacle_ray=nearest,
                   obstacle_distance_cm=float(distance[nearest]) if nearest >= 0 else np.nan,
                   visible_hit_height_cm=float(height[nearest]) if nearest >= 0 else np.nan,
                   ahead_surface_distance_cm=float(distance[nearest_visible]) if nearest_visible >= 0 else np.nan,
                   ahead_surface_height_cm=float(height[nearest_visible]) if nearest_visible >= 0 else np.nan,
                   drop_ray=drop_ray,
                   drop_distance_cm=float(distance[drop_ray]) if drop_ray >= 0 else np.nan,
                   drop_height_cm=float(height[drop_ray]) if drop_ray >= 0 else np.nan)
        for name, memory, cell in (("near", near, 25), ("far", far, 200)):
            row.update({f"{name}_{key}": value for key, value in _memory_ahead(memory, cell, movement, reference, known).items()})
        result.append(row)
    return result


def drop_onsets(times: np.ndarray, height_cm: np.ndarray, flags: np.ndarray,
                jump: np.ndarray, climb: np.ndarray, moving: np.ndarray,
                horizon_ms: int = 800, minimum_drop_cm: float = 50) -> np.ndarray:
    """Label the last supported decision before a substantial uncommanded fall.

    World height/future body flags are used here only as outcome labels, never
    passed to sensor_features() or the rule. A slope or a held walking key alone
    is insufficient. An airborne episode prefix cannot create an onset.
    """
    count = len(times)
    result = np.zeros(count, dtype=bool)
    airborne = (flags[:, 0] != 0) | (flags[:, 1] != 0)
    for i in range(1, count):
        if not airborne[i] or airborne[i - 1] or not moving[i - 1]:
            continue
        start = np.searchsorted(times, max(0, int(times[i]) - horizon_ms))
        end = np.searchsorted(times, int(times[i]) + horizon_ms, side="right")
        if jump[start:i + 1].any() or climb[start:i + 1].any() or flags[start:i + 1, 3:].any():
            continue
        # Stop at the first landing: a later unrelated lower surface is not this fall.
        landing = np.flatnonzero(~airborne[i:end])
        if len(landing):
            end = i + int(landing[0]) + 1
        if end > i and float(height_cm[i - 1]) - float(np.min(height_cm[i:end])) >= minimum_drop_cm:
            result[i - 1] = True
    return result


def predict_rule(rows: list[dict], rule: dict | None) -> np.ndarray:
    if rule is None:
        return np.zeros(len(rows), dtype=bool)
    return np.asarray([rule["min_height_cm"] <= row["visible_hit_height_cm"] <= rule["max_height_cm"] and
                       row["obstacle_distance_cm"] < rule["distance_cm"] for row in rows], dtype=bool)


def _binary_metrics(target: np.ndarray, predicted: np.ndarray) -> dict:
    tp = int((target & predicted).sum()); fp = int((~target & predicted).sum())
    fn = int((target & ~predicted).sum()); tn = int((~target & ~predicted).sum())
    return dict(positives=tp + fn, controls=fp + tn, explained=tp, false_positives=fp,
                coverage=tp / (tp + fn) if tp + fn else None,
                walking_false_positive_rate=fp / (fp + tn) if fp + tn else None,
                precision=tp / (tp + fp) if tp + fp else None)


def fit_rule(rows: list[dict]) -> dict | None:
    """Fixed small height/distance grid on train only, maximizing control-aware F1."""
    training = [row for row in rows if row["split"] == "train" and (row["jump"] or row["climb"] or row["ordinary_walk"])]
    target = np.asarray([row["jump"] or row["climb"] for row in training], dtype=bool)
    if not target.any() or target.all():
        return None
    height = np.asarray([row["visible_hit_height_cm"] for row in training])
    nearby = np.asarray([row["obstacle_distance_cm"] for row in training])
    best = None
    for low in (25, 50, 75, 100):
        for high in (100, 150, 200, 250, 300):
            if high <= low:
                continue
            for distance in (75, 100, 150, 200, 300, 400, 600):
                prediction = (height >= low) & (height <= high) & (nearby < distance)
                tp, fp, fn = int((target & prediction).sum()), int((~target & prediction).sum()), int((target & ~prediction).sum())
                score = (2*tp / (2*tp + fp + fn) if 2*tp + fp + fn else 0, -fp, -distance, low, -high)
                if best is None or score > best[0]:
                    best = (score, dict(min_height_cm=low, max_height_cm=high, distance_cm=distance))
    return {**best[1], "fit_split": "train", "objective": "F1 against ordinary walking", "train_f1": best[0][0]}


def predict_drop_rule(rows: list[dict], rule: dict | None) -> np.ndarray:
    if rule is None:
        return np.zeros(len(rows), dtype=bool)
    eye = np.asarray([row.get("drop_distance_cm", np.nan) < rule["distance_cm"] and
                      row.get("drop_height_cm", np.nan) < -rule["min_drop_cm"] for row in rows], dtype=bool)
    token = next(token for probability, token in MEMORY_THRESHOLDS if probability == rule["memory_min_probability"])
    memory = np.asarray([any(row.get(f"{layer}_drop_distance_{token}_cm", np.nan) < rule["distance_cm"]
                             for layer in ("near", "far")) for row in rows], dtype=bool)
    return (eye if rule["mode"] == "eye" else memory if rule["mode"] == "memory" else eye | memory).astype(bool)


def fit_drop_rule(rows: list[dict]) -> dict | None:
    """Eye lower ground OR a coupled memory-drop cell, on train labels only."""
    training = [row for row in rows if row["split"] == "train" and (row["drop_onset"] or row["ordinary_walk"])]
    target = np.asarray([row["drop_onset"] for row in training], dtype=bool)
    if not target.any() or target.all():
        return None
    eye_distance = np.asarray([row.get("drop_distance_cm", np.nan) for row in training])
    eye_height = np.asarray([row.get("drop_height_cm", np.nan) for row in training])
    memory = {token: np.fmin([row.get(f"near_drop_distance_{token}_cm", np.nan) for row in training],
                            [row.get(f"far_drop_distance_{token}_cm", np.nan) for row in training])
              for _, token in MEMORY_THRESHOLDS}
    best = None
    for mode in ("eye", "memory", "either"):
        for depth in (50, 100, 150):
            for probability, token in MEMORY_THRESHOLDS:
                for distance in (75, 100, 150, 200, 300, 400, 600):
                    eye = (eye_distance < distance) & (eye_height < -depth)
                    remembered = memory[token] < distance
                    prediction = eye if mode == "eye" else remembered if mode == "memory" else eye | remembered
                    tp, fp, fn = int((target & prediction).sum()), int((~target & prediction).sum()), int((target & ~prediction).sum())
                    score = (2*tp / (2*tp + fp + fn) if 2*tp + fp + fn else 0, -fp, -(mode == "either"), -distance, probability, depth)
                    if best is None or score > best[0]:
                        best = (score, dict(mode=mode, min_drop_cm=depth, memory_min_probability=probability, distance_cm=distance))
    return {**best[1], "fit_split": "train", "objective": "F1 against ordinary walking", "train_f1": best[0][0]}


def _number_summary(rows: list[dict], name: str) -> dict:
    values = np.asarray([row.get(name, np.nan) for row in rows], dtype=float)
    values = values[np.isfinite(values)]
    return dict(known=len(values), known_fraction=len(values)/len(rows) if rows else None, median=float(np.median(values)) if len(values) else None,
                p10=float(np.percentile(values, 10)) if len(values) else None,
                p90=float(np.percentile(values, 90)) if len(values) else None)


def summarize(rows: list[dict], rule: dict | None, drop_rule: dict | None = None) -> dict:
    result = {}
    metrics = ("obstacle_distance_cm", "visible_hit_height_cm", "ahead_surface_distance_cm", "ahead_surface_height_cm", "drop_distance_cm", "drop_height_cm",
               "near_ledge", "near_drop", "far_ledge", "far_drop", "near_ledge_distance_cm", "near_drop_distance_cm", "far_ledge_distance_cm", "far_drop_distance_cm",
               "near_min_ground_height_cm", "far_min_ground_height_cm", "carrot_angle_deg", "carrot_movement_angle_deg", "eye_height_proxy_cm", "fresh_rays", "ahead_hit_rays")
    for split in (*PARTS, "all"):
        selected = [row for row in rows if split == "all" or row["split"] == split]
        grouped = {}
        for label in ("jump", "climb", "jump_onset", "climb_onset", "drop_onset", "ordinary_walk"):
            group = [row for row in selected if row[label]]
            primary = drop_rule if label == "drop_onset" else rule
            prediction = predict_drop_rule(group, primary) if label == "drop_onset" else predict_rule(group, primary)
            drop_prediction = predict_drop_rule(group, drop_rule)
            grouped[label] = dict(decisions=len(group), rule_matches=int(prediction.sum()),
                                  rule_coverage=float(prediction.mean()) if len(group) and primary else None,
                                  drop_rule_matches=int(drop_prediction.sum()),
                                  drop_rule_rate=float(drop_prediction.mean()) if len(group) and drop_rule else None,
                                  features={name: _number_summary(group, name) for name in metrics})
        controls = [row for row in selected if row["jump"] or row["climb"] or row["ordinary_walk"]]
        drops = [row for row in selected if row["drop_onset"] or row["ordinary_walk"]]
        reasons = Counter()
        for row, predicted in zip(selected, predict_rule(selected, rule)):
            if not (row["jump"] or row["climb"]) or predicted:
                continue
            if not row["ahead_hit_rays"]:
                reason = "no_fresh_direction_hit"
            elif not np.isfinite(row["eye_height_proxy_cm"]):
                reason = "no_eye_height_anchor"
            elif not np.isfinite(row["ahead_surface_distance_cm"]):
                reason = "no_vertical_surface_in_direction"
            elif not np.isfinite(row["visible_hit_height_cm"]):
                reason = "no_surface_sample_25_to_300cm"
            else:
                reason = "no_train_rule" if rule is None else "outside_fitted_height_or_distance"
            reasons[reason] += 1
        drop_reasons = Counter()
        for row, predicted in zip(selected, predict_drop_rule(selected, drop_rule)):
            if not row["drop_onset"] or predicted:
                continue
            if not np.isfinite(row["drop_distance_cm"]) and not (row["near_drop"] or row["far_drop"]):
                reason = "no_lower_ground_eye_hit_or_memory_drop"
            elif drop_rule is None:
                reason = "no_train_drop_rule"
            else:
                reason = "outside_fitted_depth_confidence_or_distance"
            drop_reasons[reason] += 1
        result[split] = dict(decisions=len(selected), missed_jump_climb_reasons=dict(reasons), missed_drop_reasons=dict(drop_reasons),
                             rule=_binary_metrics(np.asarray([row["jump"] or row["climb"] for row in controls], dtype=bool), predict_rule(controls, rule)) if rule else None,
                             drop_rule=_binary_metrics(np.asarray([row["drop_onset"] for row in drops], dtype=bool), predict_drop_rule(drops, drop_rule)) if drop_rule else None,
                             groups=grouped)
    return result


def _metadata_rows(path: Path | None) -> list[dict]:
    if path is None:
        return []
    if path.suffix.lower() == ".csv":
        with path.open(newline="", encoding="utf-8") as stream:
            return list(csv.DictReader(stream))
    if path.suffix.lower() == ".jsonl":
        return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
    value = json.loads(path.read_text(encoding="utf-8"))
    if isinstance(value, dict):
        value = value.get("episodes", value.get("routes"))
    if not isinstance(value, list) or not all(isinstance(row, dict) for row in value):
        raise ValueError("metadata must contain episode/route rows")
    return value


def _episode_metadata(samples: Path, external: list[dict]) -> dict:
    original = json.loads((samples.parent / "episode.json").read_text(encoding="utf-8"))
    names, pairs = [], []
    identities = {str(samples), str(samples.parent), samples.parent.name}
    for row in external:
        identity = row.get("episode", row.get("directory", row.get("id")))
        by_name = identity is not None and str(identity) in identities
        by_pair = row.get("start") == original.get("start") and row.get("goal") == original.get("goal") and row.get("start") is not None
        if by_name:
            names.append(row)
        elif by_pair:
            pairs.append(row)
    matches = names or pairs
    if len(matches) > 1:
        raise ValueError(f"ambiguous metadata for {samples}")
    metadata = {**original, **(matches[0] if matches else {})}
    part = metadata.get("split", "unsplit") or "unsplit"
    part = "validation" if part == "val" else part
    if part not in PARTS:
        raise ValueError(f"unsupported split {part!r} in {samples}")
    evidence = original.get("transition_evidence", {})
    return dict(split=part, site_id=str(metadata.get("site_id", metadata.get("site", metadata.get("cluster", ""))) or ""),
                transition_id=str(metadata.get("transition_id", metadata.get("transition", "")) or ""),
                region=str(metadata.get("region", "unknown")), type=str(metadata.get("type", "unknown")),
                success=original.get("success"), transition_success=original.get("transition_success"),
                success_criterion=original.get("success_criterion"),
                _native_witnessed=evidence.get("native_dynamics") is True, _native_event_ms=evidence.get("event_ms"))


def _selection_reason(metadata, successful_only, criterion):
    if criterion is not None and metadata["success_criterion"] != criterion:
        return "success_criterion_mismatch"
    if successful_only and metadata["transition_success"] is not True:
        return "transition_not_successful"
    if successful_only and metadata["success"] is not True:
        return "goal_not_reached"
    return None


def _physical_fall_runs(frames):
    """Contiguous airborne/falling phases, including native-action provenance.

    Do not split a jump's late falling phase into a new drop when BS_JUMP ends.
    The preceding physical tick is included to catch a climb/jump at takeoff.
    """
    runs = []
    index = 0
    while index < len(frames):
        if not (frames[index].get("in_air") or frames[index].get("falling")):
            index += 1
            continue
        first = index
        while index < len(frames) and (frames[index].get("in_air") or frames[index].get("falling")):
            index += 1
        phase = frames[max(0, first - 1):index]
        runs.append(dict(first=first, after=index, start_ms=frames[first]["elapsed_ms"],
                         end_ms=frames[index]["elapsed_ms"] if index < len(frames) else frames[-1]["elapsed_ms"],
                         native_action=any(row.get("native_jump") or row.get("native_climb") for row in phase)))
    return runs


def _drop_labels(samples, metadata, times, jumps, climbs, flags, diagnostic):
    """Reconcile outcome labels by physical fall-run, never by a time-radius.

    Native/teacher frames are labels only. Raw NAVDEMO diagnostics stay visible
    in their own counters even when native action provenance rejects them.
    """
    names = ("native", "filtered", "rejected_native", "unconfirmed", "matched_native", "duplicate_run", "primary", "native_motion")
    result = {name: np.zeros(len(times), dtype=bool) for name in names}
    path = samples.parent / "transition-frames.jsonl"
    scheduled = metadata["type"] == "drop" and metadata["_native_witnessed"] and metadata["_native_event_ms"] is not None
    if not path.is_file():
        result["filtered"] = diagnostic.copy()
        result["primary"] = diagnostic.copy()
        return result
    with path.open(encoding="utf-8") as stream:
        frames = sorted((json.loads(line) for line in stream if line.strip()), key=lambda row: row["elapsed_ms"])
    if not frames:
        result["filtered"] = diagnostic.copy()
        result["primary"] = diagnostic.copy()
        return result
    relative = times - times[0]
    physical_times = np.asarray([row["elapsed_ms"] for row in frames], dtype=np.int64)
    corresponding = np.searchsorted(physical_times, relative, side="right") - 1
    for decision, physical_index in enumerate(corresponding):
        if physical_index >= 0 and relative[decision] <= physical_times[-1]:
            frame = frames[int(physical_index)]
            result["native_motion"][decision] = any(frame.get(name) for name in ("native_jump", "native_climb", "in_air", "falling"))
    runs = _physical_fall_runs(frames)
    authoritative_run = None
    if scheduled:
        event = metadata["_native_event_ms"]
        for run_index, run in enumerate(runs):
            # The witness can be at the first landing tick (maxDrop became
            # large enough there), hence the inclusive physical-run end.
            if not run["start_ms"] <= event <= run["end_ms"] or run["native_action"]:
                continue
            phase = frames[run["first"]:min(len(frames), run["after"] + 1)]
            if not any(str(row.get("transition_id")) == metadata["transition_id"] and row.get("type") == "drop" for row in phase):
                continue
            index = int(np.searchsorted(relative, run["start_ms"], side="left")) - 1
            if index >= 0:
                # The recorded witness and this exact physical run are
                # authoritative; an unrelated earlier jump is not this fall.
                authoritative_run = run_index
                result["native"][index] = result["primary"][index] = True
            break
    airborne = (flags[:, 0] != 0) | (flags[:, 1] != 0)
    used_runs = set()
    for index in np.flatnonzero(diagnostic):
        first_air_index = min(int(index) + 1, len(relative) - 1)
        begin, first_air = int(relative[index]), int(relative[first_air_index])
        after = first_air_index
        while after < len(relative) and airborne[after]:
            after += 1
        end = int(relative[after]) if after < len(relative) else int(relative[-1])
        previous_tick = max(0, int(np.searchsorted(physical_times, begin, side="right")) - 1)
        through_first_air = int(np.searchsorted(physical_times, first_air, side="right"))
        neighbourhood = frames[previous_tick:through_first_air]
        # NAVDEMO may call the preceding decision grounded although its native
        # body is still executing a jump/climb begun over 800ms earlier.
        if any(row.get("native_jump") or row.get("native_climb") for row in neighbourhood):
            result["rejected_native"][index] = True
            continue
        matching = [(run_index, run) for run_index, run in enumerate(runs)
                    if run["start_ms"] < end and run["end_ms"] > begin]
        if matching:
            run_index, run = min(matching, key=lambda pair: abs(pair[1]["start_ms"] - first_air))
            if run["native_action"]:
                result["rejected_native"][index] = True
                continue
            result["filtered"][index] = True
            if run_index == authoritative_run:
                result["matched_native"][index] = True
                continue
            if run_index in used_runs:
                result["duplicate_run"][index] = True
                continue
            used_runs.add(run_index)
            result["primary"][index] = True
        elif physical_times[0] <= begin and physical_times[-1] >= first_air:
            # Fully observed native time contains no matching fall. Preserve
            # the raw diagnostic, but do not invent a second physical event.
            result["unconfirmed"][index] = True
        else:
            # Before/after the writer's observed transition span there is no
            # native evidence with which to confirm or reject this diagnostic.
            result["filtered"][index] = result["primary"][index] = True
    return result


def _new_tally():
    return dict(episodes=0, decisions=0, goal_success_episodes=0, transition_success_episodes=0,
                counts={name: 0 for name in LABELS}, by_type={}, by_split={}, by_region={})


def _add_tally(tally, metadata, labels):
    count = {name: sum(bool(row[name]) for row in labels) for name in LABELS}
    tally["episodes"] += 1
    tally["decisions"] += len(labels)
    tally["goal_success_episodes"] += metadata["success"] is True
    tally["transition_success_episodes"] += metadata["transition_success"] is True
    for name, value in count.items():
        tally["counts"][name] += value
    for field in ("type", "split", "region"):
        group = tally["by_" + field].setdefault(metadata[field], dict(episodes=0, decisions=0, counts={name: 0 for name in LABELS}))
        group["episodes"] += 1
        group["decisions"] += len(labels)
        for name, value in count.items():
            group["counts"][name] += value
    return count


def analyze(paths: list[Path], metadata_path: Path | None = None, chunk_records: int = 32,
            max_input_gb: float = 10, max_ray_age_ms: int = 0,
            successful_only: bool = False, success_criterion: str | None = None) -> tuple[dict, list[dict]]:
    files = []
    for path in paths:
        found = nav_demos.episode_paths(path)
        if not found:
            raise ValueError(f"no NAVDEMO recordings in {path}")
        files.extend(file.resolve() for file in found)
    if len(files) != len(set(files)):
        raise ValueError("duplicate recording source")
    size = sum(file.stat().st_size for file in files)
    if max_input_gb <= 0 or size > max_input_gb*1024**3:
        raise ValueError(f"compressed recordings exceed {max_input_gb:g} GiB input budget")
    external = _metadata_rows(metadata_path)
    episodes = [(file, _episode_metadata(file, external)) for file in files]
    owners = {}
    for file, metadata in episodes:
        for key in ("site_id", "transition_id"):
            if metadata[key]:
                identity = (key, metadata[key])
                if identity in owners and owners[identity] != metadata["split"]:
                    raise ValueError(f"{key} {metadata[key]!r} crosses splits")
                owners[identity] = metadata["split"]
    criterion = success_criterion or (DEMO_CRITERION if successful_only else None)
    rows, episode_tallies, exclusions = [], [], Counter()
    tally_all, tally_selected = _new_tally(), _new_tally()
    for samples, metadata in episodes:
        if nav_demos.episode_version(samples) != 2:
            raise ValueError(f"NAVDEMO v2 required: {samples}")
        reason = _selection_reason(metadata, successful_only, criterion)
        include = reason is None
        if reason:
            exclusions[reason] += 1
        layout = nav_demos.ray_layout(samples) if include else None
        features, times, heights, flags, jumps, climbs, moving, walks = [], [], [], [], [], [], [], []
        for records in nav_demos.iter_chunks(samples, chunk_records):
            if include:
                features.extend(sensor_features(nav_demos.learner_input(records), layout, max_ray_age_ms))
            times.extend(records["time_ms"].tolist())
            heights.extend(records["position_cm"][:, 1].tolist())
            flags.extend(records["body"]["flags"].tolist())
            jumps.extend((records["keys"]["jump"] != 0).tolist())
            climbs.extend((records["keys"]["climb"] != 0).tolist())
            moving.extend(((records["keys"]["forward"] != 0) | (records["keys"]["strafe"] != 0)).tolist())
            walks.extend((records["keys"]["walk"] != 0).tolist())
        if not times:
            raise ValueError(f"empty recording: {samples}")
        times_array, flag_array = np.asarray(times, dtype=np.int64), np.asarray(flags, dtype=np.uint8)
        jump_array, climb_array = np.asarray(jumps), np.asarray(climbs)
        diagnostic = drop_onsets(times_array, np.asarray(heights), flag_array, jump_array, climb_array, np.asarray(moving))
        drop_labels = _drop_labels(samples, metadata, times_array, jump_array, climb_array, flag_array, diagnostic)
        native, drops = drop_labels["native"], drop_labels["primary"]
        drop_times = times_array[drops]
        labels = []
        for i in range(len(times)):
            labels.append(dict(jump=jumps[i], climb=climbs[i],
                               jump_onset=jumps[i] and (i == 0 or not jumps[i - 1]),
                               climb_onset=climbs[i] and (i == 0 or not climbs[i - 1]),
                               drop_onset=bool(drops[i]), drop_navdemo_onset=bool(diagnostic[i]), drop_native_onset=bool(native[i]),
                               drop_navdemo_filtered_onset=bool(drop_labels["filtered"][i]),
                               drop_navdemo_rejected_native_action=bool(drop_labels["rejected_native"][i]),
                               drop_navdemo_unconfirmed_onset=bool(drop_labels["unconfirmed"][i]),
                               drop_navdemo_matched_native=bool(drop_labels["matched_native"][i]),
                               drop_navdemo_duplicate_run=bool(drop_labels["duplicate_run"][i]), native_motion=bool(drop_labels["native_motion"][i]),
                               drop_onset_source="native_frames" if native[i] else "navdemo_dynamics" if drops[i] else "none",
                               ordinary_walk=moving[i] and not (jumps[i] or climbs[i]) and not flag_array[i].any() and not drop_labels["native_motion"][i] and not np.any(np.abs(drop_times - times_array[i]) <= 800),
                               walk=moving[i] and walks[i], run=moving[i] and not walks[i]))
        count = _add_tally(tally_all, metadata, labels)
        public = {name: value for name, value in metadata.items() if not name.startswith("_")}
        episode_tallies.append(dict(episode=str(samples.parent), **public, decisions=len(times), counts=count,
                                    analysis_selected=include, exclusion_reason=reason))
        if include:
            _add_tally(tally_selected, metadata, labels)
            rows.extend(dict(episode=str(samples.parent), decision=i, time_ms=times[i], **public, **feature, **label)
                        for i, (feature, label) in enumerate(zip(features, labels)))
    rule, drop_rule = fit_rule(rows), fit_drop_rule(rows)
    limitations = list(LIMITATIONS)
    if any(not metadata["site_id"] for _, metadata in episodes):
        limitations.append("Some episodes lack site_id: transition identity alone cannot verify the required 30m spatial split separation.")
    if any(metadata["split"] == "unsplit" for _, metadata in episodes):
        limitations.append("Unsplit episodes are descriptive only and never used to fit thresholds or claim held-out coverage.")
    if rule is None:
        limitations.append("No jump/climb rule fitted: train requires both positive labels and ordinary walking controls.")
    if drop_rule is None:
        limitations.append("No drop rule fitted: train requires both drop onset labels and ordinary walking controls.")
    result = dict(navdemo_version=2, episodes=tally_selected["episodes"], input_bytes=size, decisions=len(rows),
                  selection=dict(successful_only=successful_only, success_criterion=criterion, exclusion_reasons=dict(exclusions)),
                  tally_all=tally_all, tally_selected=tally_selected, episode_tallies=episode_tallies,
                  label_definitions=dict(drop_onset="one primary label per physical fall-run; scheduled native event is authoritative",
                                         drop_navdemo_onset="RAW 100ms diagnostic only; can include late jump/climb continuations and is not a count of real drops",
                                         drop_native_onset="decision strictly before the physical fall-run containing the scheduled witnessed drop event",
                                         drop_navdemo_filtered_onset="diagnostics after native jump/climb/unconfirmed rejection, before physical-run deduplication",
                                         drop_navdemo_matched_native="diagnostics belonging to the authoritative native fall-run, suppressed from primary labels"),
                  rule=rule, drop_rule=drop_rule, limitations=limitations, counts=tally_selected["counts"],
                  site_split_consistent=True, spatial_separation_verified=False,
                  geometry=dict(exact=False, max_ray_age_ms=max_ray_age_ms,
                                decisions_with_eye_height=sum(bool(np.isfinite(row["eye_height_proxy_cm"])) for row in rows)),
                  by_split=summarize(rows, rule, drop_rule))
    return result, rows


def _clean(value: Any) -> Any:
    if isinstance(value, np.generic):
        return _clean(value.item())
    if isinstance(value, dict):
        return {key: _clean(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_clean(item) for item in value]
    if isinstance(value, float) and not np.isfinite(value):
        return None
    return value


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sources", type=Path, nargs="+")
    parser.add_argument("--metadata", type=Path)
    parser.add_argument("--successful-only", action="store_true", help="analyze sensors only for transition_success=true AND goal success, with the demo criterion")
    parser.add_argument("--success-criterion", help="required original episode.json success_criterion; successful-only defaults to demo_goal_action_landing_side_v1")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--csv", type=Path, required=True)
    parser.add_argument("--chunk-records", type=int, default=32)
    parser.add_argument("--max-input-gb", type=float, default=10)
    parser.add_argument("--max-ray-age-ms", type=int, default=0)
    args = parser.parse_args()
    if args.output.resolve() == args.csv.resolve():
        parser.error("JSON and CSV destinations must differ")
    # Outputs cannot overwrite any source artifact, including episode.json.
    for destination in (args.output, args.csv):
        for source in args.sources:
            root = source if source.is_dir() else source.parent
            if destination.resolve().is_relative_to(root.resolve()):
                parser.error("output destinations must be outside recording source directories")
        if args.metadata and destination.resolve() == args.metadata.resolve():
            parser.error("output cannot overwrite metadata")
    result, rows = analyze(args.sources, args.metadata, args.chunk_records, args.max_input_gb, args.max_ray_age_ms, args.successful_only, args.success_criterion)
    for path in (args.output, args.csv):
        path.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(_clean(result), ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    with args.csv.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]) if rows else ["episode", "decision", "time_ms"])
        writer.writeheader()
        writer.writerows({key: "" if isinstance(value, float) and not np.isfinite(value) else value for key, value in row.items()} for row in rows)
    print(json.dumps(dict(episodes=result["episodes"], decisions=result["decisions"], counts=result["counts"], rule=result["rule"], drop_rule=result["drop_rule"], tally_all=result["tally_all"]), ensure_ascii=False))


if __name__ == "__main__":
    main()
