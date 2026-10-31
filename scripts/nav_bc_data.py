#!/usr/bin/env python3
"""Compact, sensor-only navigation examples for recurrent behaviour cloning.

    python3 -I scripts/nav_bc_data.py prepare --source /absolute/nav-demos \
        --output /absolute/nav-bc/cache

Only numpy is required. Preparation streams the recordings one episode at a time.
The default cache accepts NAVDEMO v2 recordings and contains successful teacher
episodes and all DAgger episodes from training pairs. A saved split preserves
the held-out (start, goal) pairs; new successful v2 pairs join training.
With --manifest, BC3 uses exactly the listed memory-v3 training episodes,
including failed follow trials, without creating a new split. BC4 uses the v5
manifest's memory-v4 NAVDEMO v3 recordings and HD maps with familiar appended.
The same episode encoder also feeds BC directly from gzip/XZ recordings, without
writing tensor caches; converted metadata carries the lightweight label counts.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
from typing import Any

import numpy as np

# Isolated mode deliberately omits the script directory from sys.path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import nav_demos


HEADS = ("forward", "strafe", "turn", "walk", "jump", "climb")
CLASSES = (3, 3, 3, 2, 2, 2)
BC3_HEADS = ("forward", "strafe", "turn", "walk", "maneuver")
BC3_CLASSES = (3, 3, 3, 2, 3)
SITUATIONS = ("wall", "turn", "beginning")
VECTOR_SIZE = nav_demos.RAY_COUNT * 9 + 10 + 14
CACHE_VERSION = 3
BC3_CACHE_VERSION = 4
HD_CACHE_VERSION = 5
BC4_CACHE_VERSION = 6
HD_NEAR_SHAPE = (10, 64, 64)
HD_FAR_SHAPE = (10, 50, 100)
BC4_NEAR_SHAPE = (11, 64, 64)
BC4_FAR_SHAPE = (11, 50, 100)
INPUT_NAMES = frozenset(("body", "roads", "previous_keys", "eye", "memory_near", "memory_far"))

PREPROCESSING = {
    "navdemo_version": 2,
    "input_fields": ["body", "roads", "eye", "memory_near", "memory_far"],
    "eye": "per ray: distance/max_range, kind one-hot[0:6], normal_y, hit; flatten ray-major",
    "memory": "7 probability channels block mean (near 2x2, far 4x4); known-height mean and validity fraction",
    "height": "subtract nearest valid near-memory ground height (far fallback), /200cm, clip [-10,10]; unknown=0",
    "height_reference": "nearest to body origin in sensor map, no world position/yaw or teacher_hint access",
    "roads_schema": "carrot_v2",
    "roads_fields": ["carrot_direction", "carrot_distance_cm", "goal_direction", "goal_distance_cm", "remaining_known_cm", "beyond_known"],
    "roads": "carrot_direction[3], carrot_distance_cm/1000, goal_direction[3], goal_distance_cm/1000, remaining_known_cm/1000, beyond_known; distances clipped [0,50]",
    "body": "velocity[3]/500 clipped [-4,4], flags[5], ground_normal[3], slope/90, head_yaw/180, head_pitch/90",
    "vector_order": ["eye", "roads", "body"],
    "map_order": [*nav_demos.CHANNELS, "relative_ground_height", "height_valid_fraction"],
    "cache_quantization": "vector/height float16, map probabilities/validity round(255*x) uint8, labels uint8",
    "road_baseline": "always forward and walk, turn=-sign(atan2(carrot_direction.x,carrot_direction.z)) outside 5deg",
}

HD_PREPROCESSING = {
    **PREPROCESSING,
    "input_variant": "hd",
    "input_fields": [*PREPROCESSING["input_fields"], "feet_height_cm"],
    "memory": "7 probability channels unpooled; near 64x64 at 25cm; far forward rows[50:100] and all 100 columns at 200cm; one recorded height per cell",
    "height": "known ground max and min minus physical feet height, /200cm, clip [-10,10]; unknown=0; unpooled recorded max=min",
    "height_reference": "own physical feet height only; no absolute world position, x/z, yaw or teacher_hint access",
    "map_order": [*nav_demos.CHANNELS, "relative_ground_height_max", "relative_ground_height_min", "height_valid_fraction"],
    "cache_quantization": "vector/height extrema float16, map probabilities/validity round(255*x) uint8, labels uint8",
}

BC4_PREPROCESSING = {
    **HD_PREPROCESSING,
    "navdemo_version": 3,
    "navigation_memory_version": 4,
    "input_variant": "hd-familiar",
    "map_order": [*HD_PREPROCESSING["map_order"], "familiar"],
}


def _nearest_height(memory: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Infer the height origin entirely from the learner's remembered ground."""
    count, height, width = memory.shape
    # Cell centres straddle the origin; deterministic tie breaking uses row order.
    z, x = np.indices((height, width), dtype=np.float32)
    order = np.argsort(((z + .5 - height / 2) ** 2 + (x + .5 - width / 2) ** 2).ravel(), kind="stable")
    valid = memory["height_valid"].reshape(count, -1)[:, order] != 0
    known = valid.any(axis=1)
    first = valid.argmax(axis=1)
    heights = memory["ground_height_cm"].reshape(count, -1)[:, order]
    selected = heights[np.arange(count), first]
    return np.where(known, selected, 0).astype(np.float32), known


def _pool_memory(memory: np.ndarray, factor: int, reference: np.ndarray) -> np.ndarray:
    count, height, width = memory.shape
    out_h, out_w = height // factor, width // factor
    if height % factor or width % factor:
        raise ValueError("memory dimensions must be divisible by pooling factor")
    probability = memory["channels"][..., :7].astype(np.float32) / 255
    probability = probability.reshape(count, out_h, factor, out_w, factor, 7).mean(axis=(2, 4))
    valid = memory["height_valid"].astype(np.float32)
    relative = memory["ground_height_cm"].astype(np.float32) - reference[:, None, None]
    # Subtract before pooling, so the absolute world height never becomes a feature.
    height_sum = (relative * valid).reshape(count, out_h, factor, out_w, factor).sum(axis=(2, 4))
    valid_sum = valid.reshape(count, out_h, factor, out_w, factor).sum(axis=(2, 4))
    pooled_height = np.zeros_like(height_sum)
    np.divide(height_sum, valid_sum * 200, out=pooled_height, where=valid_sum > 0)
    np.clip(pooled_height, -10, 10, out=pooled_height)
    features = np.concatenate((probability, pooled_height[..., None],
                               (valid_sum / (factor * factor))[..., None]), axis=-1)
    return np.ascontiguousarray(features.transpose(0, 3, 1, 2), dtype=np.float32)


def _hd_memory(memory: np.ndarray, reference: np.ndarray, familiar: bool = False) -> np.ndarray:
    """Keep every stored cell; NAVDEMO has one height, so its max equals min."""
    probability = memory["channels"][..., :7].astype(np.float32) / 255
    valid = memory["height_valid"] != 0
    relative = (memory["ground_height_cm"].astype(np.float32) - reference[:, None, None]) / 200
    relative = np.where(valid, np.clip(relative, -10, 10), 0)
    features = np.concatenate((probability, relative[..., None], relative[..., None],
                               valid[..., None].astype(np.float32)), axis=-1)
    if familiar:
        if memory["channels"].shape[-1] != 8:
            raise ValueError("HD familiar input requires NAVDEMO v3 memory channels")
        features = np.concatenate((features, memory["channels"][..., 7:8].astype(np.float32) / 255), axis=-1)
    return np.ascontiguousarray(features.transpose(0, 3, 1, 2), dtype=np.float32)


def encode_features(inputs: dict[str, np.ndarray], layout: np.ndarray,
                    input_variant: str = "compact",
                    feet_height_cm: np.ndarray | None = None) -> dict[str, np.ndarray]:
    """Encode only nav_demos.learner_input(), returning float32 NCHW maps/vector.

    The exact allowed-key check catches accidental use of full records or privileged
    annotations at the model boundary. The recorder's previous_keys field is
    accepted but never encoded: only the GRU carries action history. Unknown
    ground contributes zero height.
    """
    if frozenset(inputs) != INPUT_NAMES:
        raise ValueError("encode_features accepts only nav_demos.learner_input() fields")
    if inputs["roads"].dtype != nav_demos.ROADS_V2_DTYPE:
        raise ValueError("encode_features requires NAVDEMO v2 carrot guidance")
    layout = np.asarray(layout, dtype=np.float32)
    if layout.shape != (nav_demos.RAY_COUNT, 3) or not np.isfinite(layout).all() or (layout[:, 2] <= 0).any():
        raise ValueError("invalid eye layout")
    count = len(inputs["eye"])
    if input_variant == "compact":
        reference, known = _nearest_height(inputs["memory_near"])
        far_reference, _ = _nearest_height(inputs["memory_far"])
        reference = np.where(known, reference, far_reference)
        near = _pool_memory(inputs["memory_near"], 2, reference)
        far = _pool_memory(inputs["memory_far"], 4, reference)
    elif input_variant in ("hd", "hd-familiar"):
        reference = np.asarray(feet_height_cm, dtype=np.float32)
        if reference.shape != (count,) or not np.isfinite(reference).all():
            raise ValueError("HD input requires one finite physical feet height per decision")
        if inputs["memory_near"].shape[1:] != (64, 64) or inputs["memory_far"].shape[1:] != (100, 100):
            raise ValueError("HD input requires the recorded 64x64 near and 100x100 far maps")
        near = _hd_memory(inputs["memory_near"], reference, input_variant == "hd-familiar")
        far = _hd_memory(inputs["memory_far"][:, 50:100, :], reference, input_variant == "hd-familiar")
    else:
        raise ValueError(f"unsupported navigation input variant: {input_variant}")

    eye = inputs["eye"]
    kinds = eye["kind"].astype(np.int64)
    if (kinds > 5).any():
        raise ValueError("unknown eye hit kind")
    rays = np.concatenate((
        np.clip(eye["distance_cm"] / layout[None, :, 2], 0, 1)[..., None],
        np.eye(6, dtype=np.float32)[kinds], eye["normal_y"][..., None],
        eye["hit"][..., None]), axis=-1).reshape(count, -1)
    roads = inputs["roads"]
    distance = lambda name: np.clip(roads[name] / 1000, 0, 50)[:, None]
    road_features = np.concatenate((roads["carrot_direction"], distance("carrot_distance_cm"),
                                   roads["goal_direction"], distance("goal_distance_cm"),
                                   distance("remaining_known_cm"), roads["beyond_known"][:, None]), axis=1)
    body = inputs["body"]
    body_features = np.concatenate((np.clip(body["velocity_cm_s"] / 500, -4, 4), body["flags"],
                                    body["ground_normal"], body["ground_slope_deg"][:, None] / 90,
                                    body["head_yaw_deg"][:, None] / 180,
                                    body["head_pitch_deg"][:, None] / 90), axis=1)
    vector = np.ascontiguousarray(np.concatenate((rays, road_features, body_features), axis=1), dtype=np.float32)
    if vector.shape != (count, VECTOR_SIZE):
        raise ValueError(f"unexpected vector dimensions: {vector.shape}")
    if not all(np.isfinite(value).all() for value in (near, far, vector)):
        raise ValueError("non-finite learner feature")
    return {"near": near, "far": far, "vector": vector}


def encode_labels(keys: np.ndarray) -> np.ndarray:
    labels = np.empty((len(keys), len(HEADS)), dtype=np.uint8)
    for index, name in enumerate(HEADS):
        labels[:, index] = np.sign(keys[name]).astype(np.int8) + 1 if index < 3 else keys[name] != 0
    return labels


def encode_bc3_labels(keys: np.ndarray) -> np.ndarray:
    """Mutually exclusive maneuver labels: none=0, jump=1, climb=2."""
    jump, climb = keys["jump"] != 0, keys["climb"] != 0
    if (jump & climb).any():
        raise ValueError("simultaneous jump and climb cannot form a BC3 maneuver label")
    return np.column_stack((encode_labels(keys)[:, :4], jump + 2 * climb)).astype(np.uint8)


def label_action_changes(labels: np.ndarray) -> np.ndarray:
    """The initial episode action has no preceding teacher decision."""
    changed = np.zeros(len(labels), dtype=np.uint8)
    changed[1:] = np.any(labels[1:] != labels[:-1], axis=1)
    return changed


def action_change_weight(decisions: int, changes: int) -> float:
    """Global Wijmans weight; it is applied only to changed teacher decisions."""
    return float(np.clip(decisions / changes, 1, 10)) if changes else 1.


def maneuver_weights(time_ms: np.ndarray, maneuver: np.ndarray) -> np.ndarray:
    """Weight the inclusive +/-1 s neighborhood of each teacher press by five."""
    pressed = (maneuver != 0) & np.r_[True, maneuver[1:] != maneuver[:-1]]
    result = np.ones(len(maneuver), dtype=np.float32)
    # Search on game time rather than assuming every episode is exactly 10 Hz.
    stamps = np.asarray(time_ms, dtype=np.int64)
    for stamp in stamps[pressed]:
        result[np.searchsorted(stamps, stamp - 1000):np.searchsorted(stamps, stamp + 1000, side="right")] = 5
    return result


def future_stuck(time_ms: np.ndarray, position_cm: np.ndarray,
                 labels: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Requested translation with <25 cm XZ excursion over a complete next 2 s."""
    stamps = np.asarray(time_ms, dtype=np.int64)
    if len(stamps) > 1 and (np.diff(stamps) <= 0).any():
        raise ValueError("BC3 episode game time must increase strictly")
    target = np.zeros(len(stamps), dtype=np.float32)
    valid = (stamps + 2000 <= stamps[-1]).astype(np.uint8) if len(stamps) else np.zeros(0, np.uint8)
    translating = (labels[:, 0] != 1) | (labels[:, 1] != 1)
    xz = position_cm[:, [0, 2]]
    for index in np.flatnonzero(valid & translating):
        end = np.searchsorted(stamps, stamps[index] + 2000, side="right")
        target[index] = np.max(np.linalg.norm(xz[index:end] - xz[index], axis=1)) < 25
    return target, valid


def detour_targets(records: np.ndarray, teacher_version: int) -> tuple[np.ndarray, np.ndarray]:
    """Privileged supervision only; v3 teacher hints describe its remembered map."""
    target = np.zeros((len(records), 4), dtype=np.float32)
    valid = np.zeros(len(records), dtype=np.uint8)
    if teacher_version not in (1, 2) or "teacher_hint" not in records.dtype.names:
        return target, valid
    hint = records["teacher_hint"]
    delta = hint["next_corner_cm"] - records["position_cm"]
    yaw = np.deg2rad(records["body_yaw_deg"])
    local = np.column_stack((-delta[:, 0] * np.sin(yaw) + delta[:, 2] * np.cos(yaw),
                             delta[:, 1], delta[:, 0] * np.cos(yaw) + delta[:, 2] * np.sin(yaw)))
    norm = np.linalg.norm(local, axis=1)
    valid[:] = (np.isfinite(local).all(axis=1) & np.isfinite(hint["path_cm"]) & (hint["path_cm"] >= 0))
    target[:, 0] = np.clip(hint["path_cm"] / 1000, 0, 50)
    np.divide(local, norm[:, None], out=target[:, 1:], where=norm[:, None] > 1e-6)
    target[valid == 0] = 0
    return target, valid


def action_change_mask(records: np.ndarray, previous_teacher: np.ndarray | None = None) -> np.ndarray:
    """Changes between consecutive teacher answers, never preceding actor keys.

    The first episode decision is excluded. Pass the preceding chunk's final
    six encoded teacher heads when streaming an episode.
    """
    labels = encode_labels(records["keys"])
    changed = np.zeros(len(labels), dtype=np.uint8)
    changed[1:] = np.any(labels[1:] != labels[:-1], axis=1)
    if len(labels) and previous_teacher is not None:
        changed[0] = np.any(labels[0] != previous_teacher)
    return changed


def road_baseline(inputs: dict[str, np.ndarray], threshold_deg: float = 5) -> np.ndarray:
    """Road-only baseline uses the engine's negative turn for positive local x."""
    direction = inputs["roads"]["carrot_direction"]
    angle = np.degrees(np.arctan2(direction[:, 0], direction[:, 2]))
    baseline = np.tile(np.array([2, 1, 1, 1, 0, 0], dtype=np.uint8), (len(direction), 1))
    baseline[:, 2] = np.where(angle > threshold_deg, 0, np.where(angle < -threshold_deg, 2, 1))
    return baseline


def situation_masks(inputs: dict[str, np.ndarray], layout: np.ndarray, labels: np.ndarray,
                    offset: int = 0) -> np.ndarray:
    # Eyes are relative to the head; rotate their yaw/pitch to the body frame.
    body = inputs["body"]
    yaw = layout[None, :, 0] + body["head_yaw_deg"][:, None]
    yaw = (yaw + 180) % 360 - 180
    pitch = layout[None, :, 1] + body["head_pitch_deg"][:, None]
    eye = inputs["eye"]
    frontal = (np.abs(yaw) <= 30) & (np.abs(pitch) <= 20)
    wall = (frontal & (eye["hit"] != 0) & (eye["distance_cm"] < 100) &
            (np.abs(eye["normal_y"]) < .5)).any(axis=1)
    return np.column_stack((wall, labels[:, 2] != 1,
                            np.arange(offset, offset + len(labels)) < 20)).astype(np.uint8)


def split_pairs(episodes: list[dict[str, Any]], validation_fraction: float = .2,
                seed: int = 17) -> dict[tuple[str, str], str]:
    """Largest-remainder stratification by region; exact pairs never cross splits."""
    if not 0 < validation_fraction < 1:
        raise ValueError("validation fraction must lie between zero and one")
    grouped: dict[str, set[tuple[str, str]]] = {}
    owners: dict[tuple[str, str], str] = {}
    for episode in episodes:
        pair = (episode["start"], episode["goal"])
        region = episode["region"]
        if pair in owners and owners[pair] != region:
            raise ValueError(f"pair assigned to multiple regions: {pair}")
        owners[pair] = region
        grouped.setdefault(region, set()).add(pair)
    if len(owners) < 2:
        raise ValueError("need at least two distinct successful pairs")
    regions = sorted(grouped)
    target = max(1, min(len(owners) - 1, round(len(owners) * validation_fraction)))
    counts = {region: int(len(grouped[region]) * validation_fraction) for region in regions}
    remainders = sorted(regions, key=lambda region: (-(len(grouped[region]) * validation_fraction - counts[region]), region))
    for region in remainders[:target - sum(counts.values())]:
        counts[region] += 1
    rng = np.random.default_rng(seed)
    result = dict.fromkeys(owners, "train")
    for region in regions:
        pairs = sorted(grouped[region])
        for index in rng.permutation(len(pairs))[:counts[region]]:
            result[pairs[index]] = "validation"
    return result


def _pack(features: dict[str, np.ndarray]) -> dict[str, np.ndarray]:
    packed = {"vector": features["vector"].astype(np.float16)}
    for name in ("near", "far", *(["aux_near"] if "aux_near" in features else [])):
        values = features[name]
        valid_index = 9 if values.shape[1] == 11 else values.shape[1] - 1
        packed[f"{name}_channels"] = np.rint(values[:, :7] * 255).astype(np.uint8)
        packed[f"{name}_height"] = values[:, 7:valid_index].astype(np.float16)
        packed[f"{name}_valid"] = np.rint(values[:, valid_index:valid_index + 1] * 255).astype(np.uint8)
        if values.shape[1] == 11:
            packed[f"{name}_familiar"] = np.rint(values[:, 10:11] * 255).astype(np.uint8)
    return packed


def read_pair_split(path: Path) -> dict[tuple[str, str], str]:
    """Reuse an earlier cache's pair assignment without adding any new pairs."""
    index = json.loads(path.read_text(encoding="utf-8"))
    result = {}
    for episode in index["episodes"]:
        pair, part = tuple(episode["pair"]), episode["split"]
        if len(pair) != 2 or not all(isinstance(value, str) and value for value in pair):
            raise ValueError(f"invalid pair in split index: {pair}")
        if part not in ("train", "validation"):
            raise ValueError(f"invalid partition in split index: {part}")
        if pair in result and result[pair] != part:
            raise ValueError(f"pair occurs in both partitions: {pair}")
        result[pair] = part
    if not result:
        raise ValueError("split index contains no pairs")
    return result


def unpack_cached(source, start: int | None = None, stop: int | None = None) -> dict[str, np.ndarray]:
    """Expand only the requested decisions from a packed episode."""
    selected = slice(start, stop)
    result = {"vector": source["vector"][selected].astype(np.float32),
              "labels": source["labels"][selected].astype(np.int64),
              "baseline": source["baseline"][selected].astype(np.int64),
              "situations": source["situations"][selected].astype(np.uint8),
              "action_change": source["action_change"][selected].astype(np.uint8)}
    for name in ("maneuver_weight", "stuck", "detour"):
        if name in source:
            result[name] = source[name][selected].astype(np.float32)
    for name in ("stuck_valid", "next_near_valid", "detour_valid"):
        if name in source:
            result[name] = source[name][selected].astype(np.uint8)
    for name in ("near", "far", *(["aux_near"] if "aux_near_channels" in source else [])):
        result[name] = np.concatenate((source[f"{name}_channels"][selected].astype(np.float32) / 255,
                                       source[f"{name}_height"][selected].astype(np.float32),
                                       source[f"{name}_valid"][selected].astype(np.float32) / 255), axis=1)
        if f"{name}_familiar" in source:
            result[name] = np.concatenate((result[name],
                source[f"{name}_familiar"][selected].astype(np.float32) / 255), axis=1)
    return result


def load_cached(path: str | Path, *, packed: bool = False) -> dict[str, np.ndarray]:
    """Read one episode, retaining uint8/fp16 maps when requested for HD training."""
    with np.load(path, allow_pickle=False) as source:
        if packed:
            return {name: source[name] for name in source.files}
        return unpack_cached(source)


def load_training_manifest(path: Path, source: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    """Resolve explicit training entries; held-out scenarios are never opened."""
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if manifest.get("schema_version") == 2:
        expected = {"name": "NAVDEMO", "version": 3, "navigation_memory_version": 4,
                    "perception_mode": "gaze", "physics": "cv"}
        if (manifest.get("dataset") != "nav-demos-v5" or manifest.get("consumer") != "nav-bc"
                or manifest.get("input_variant") != "hd" or manifest.get("complete") is not True
                or manifest.get("format") != expected):
            raise ValueError("compact manifest requires complete nav-demos-v5 memory-v4 HD data")
        selected, seen = [], set()
        root = source.resolve()
        for entry in manifest["episodes"]:
            identity = entry["id"]
            if identity in seen:
                raise ValueError(f"duplicate episode ID in manifest: {identity}")
            seen.add(identity)
            if entry.get("split") != "train" or entry.get("training_eligible") is not True:
                continue
            relative = Path(entry["episode"])
            directory = (root / relative).resolve()
            if relative.is_absolute() or not directory.is_relative_to(root):
                raise ValueError(f"training episode is outside the corpus: {identity}")
            scenario = json.loads((directory / "episode.json").read_text(encoding="utf-8"))
            if (scenario.get("id") != identity or scenario.get("split") != "train"
                    or scenario.get("dataset_family", scenario.get("goal_kind")) != entry["family"]
                    or scenario.get("training_eligible") is not True
                    or scenario.get("navigation_memory_version") != 4
                    or scenario.get("journey_memory") is not True or scenario.get("demo_version") != 3
                    or scenario.get("perception_mode") != "gaze" or scenario.get("physics_mode") != "cv"
                    or scenario.get("decisions") != entry["decisions"]):
                raise ValueError(f"ineligible compact training episode: {identity}")
            selected.append({**entry, "scenario": scenario, "directory": directory})
        if not selected:
            raise ValueError("manifest has no training episodes")
        totals = manifest["totals"]
        if (len(selected) != totals["training_episodes"]
                or sum(entry["decisions"] for entry in selected) != totals["training_decisions"]):
            raise ValueError("manifest training totals differ from eligible train entries")
        return manifest, selected
    expected = {"name": "NAVDEMO", "version": 2, "navigation_memory_version": 3,
                "perception_mode": "gaze", "physics": "cv"}
    if (manifest.get("schema_version") != 1 or manifest.get("dataset") != "nav-demos-v4"
            or manifest.get("consumer") != "nav-bc3" or manifest.get("complete") is not True
            or manifest.get("format") != expected):
        raise ValueError("BC3 requires a complete nav-demos-v4 memory-v3 manifest")
    original_root, root = Path(manifest["root"]).resolve(), source.resolve()
    by_id = {}
    for entry in manifest["episodes"]:
        if entry["id"] in by_id:
            raise ValueError(f"duplicate episode ID in manifest: {entry['id']}")
        by_id[entry["id"]] = entry
    selected, seen = [], set()
    annotations = None
    for family, identities in manifest["splits"]["train"].items():
        if not isinstance(identities, list):
            raise ValueError(f"training split must list episode IDs: {family}")
        for identity in identities:
            if identity in seen:
                raise ValueError(f"duplicate training episode ID: {identity}")
            seen.add(identity)
            if identity not in by_id:
                raise ValueError(f"training episode missing from manifest: {identity}")
            entry = by_id[identity]
            scenario = entry["scenario"]
            if (entry.get("split") != "train" or entry.get("family") != family
                    or entry.get("training_eligible") is not True or scenario.get("split", entry["split"]) != "train"
                    or scenario.get("navigation_memory_version") != 3):
                raise ValueError(f"ineligible BC3 training episode: {identity}")
            recorded = Path(entry["episode"])
            if not recorded.is_absolute():
                raise ValueError(f"training episode is outside the corpus: {identity}")
            recorded = recorded.resolve()
            if not recorded.is_relative_to(original_root):
                raise ValueError(f"training episode is outside the corpus: {identity}")
            directory = (root / recorded.relative_to(original_root)).resolve()
            if not directory.is_relative_to(root):
                raise ValueError(f"training episode is outside the corpus: {identity}")
            if "region" not in scenario:
                # Early raw episodes lack collector annotations. Eligibility and
                # split belong to the manifest; only the missing region comes
                # from the finalized summary, never physical scenario values.
                if annotations is None:
                    recorded_summary = Path(manifest["sources"]["recordings/collection-metrics.json"]["path"]).resolve()
                    summary = root / recorded_summary.relative_to(original_root)
                    annotations = {value["id"]: value for value in
                                   json.loads(summary.read_text(encoding="utf-8"))["episodes"]}
                scenario = {**scenario, "region": annotations[identity]["region"]}
            selected.append({**entry, "scenario": scenario, "directory": directory})
    if not selected:
        raise ValueError("manifest has no BC3 training episodes")
    totals = manifest["totals"]
    if (len(selected) != totals["train_episodes"]
            or sum(entry["decisions"] for entry in selected) != totals["train_decisions"]):
        raise ValueError("manifest training totals differ from splits.train")
    return manifest, selected


def verify_samples(samples: Path, metadata: dict[str, Any], expected_sha256: str) -> None:
    """Bind converted storage to the frozen recording and check stored bytes."""
    if samples.suffix == ".xz" and "navdemo_storage" in metadata:
        storage = metadata["navdemo_storage"]
        if storage["source_samples_sha256"] != expected_sha256:
            raise ValueError(f"manifest source checksum differs: {samples.parent}")
        expected_sha256 = storage["samples_sha256"]
    with samples.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != expected_sha256:
            raise ValueError(f"manifest sample checksum differs: {samples.parent}")


def episode_stats(samples: Path, teacher_version: int, chunk_records: int = 32) -> dict[str, Any]:
    """Read only labels and supervision counts, without assembling map tensors."""
    counts = {head: np.zeros(count, np.int64) for head, count in zip(BC3_HEADS, BC3_CLASSES)}
    decisions, changes, detour_steps, previous = 0, 0, 0, None
    for records in nav_demos.iter_chunks(samples, chunk_records):
        labels = encode_bc3_labels(records["keys"])
        for number, (head, count) in enumerate(zip(BC3_HEADS, BC3_CLASSES)):
            counts[head] += np.bincount(labels[:, number], minlength=count)
        changes += int(label_action_changes(labels).sum())
        if previous is not None:
            changes += int(np.any(labels[0] != previous))
        previous = labels[-1].copy()
        detour_steps += int(detour_targets(records, teacher_version)[1].sum())
        decisions += len(records)
    return {"decisions": decisions, "class_counts": {head: count.tolist() for head, count in counts.items()},
            "action_changes": changes, "detour_available_steps": detour_steps}


def encode_episode(samples: Path, layout: np.ndarray, teacher_version: int,
                   input_variant: str = "compact", chunk_records: int = 32) -> dict[str, np.ndarray]:
    """Build the exact packed training tensors without writing a cache."""
    hd = input_variant in ("hd", "hd-familiar")
    parts: dict[str, list[np.ndarray]] = {}
    steps = 0
    for records in nav_demos.iter_chunks(samples, chunk_records):
        inputs = nav_demos.learner_input(records)
        labels = encode_bc3_labels(records["keys"])
        encoded = encode_features(inputs, layout, input_variant,
                                  records["position_cm"][:, 1] if hd else None)
        if hd:
            encoded["aux_near"] = encode_features(inputs, layout)["near"]
        features = _pack(encoded)
        detour, detour_valid = detour_targets(records, teacher_version)
        baseline = np.column_stack((road_baseline(inputs)[:, :4], np.zeros(len(records), np.uint8)))
        features.update(labels=labels, baseline=baseline,
                        situations=situation_masks(inputs, layout, labels, steps),
                        time_ms=records["time_ms"].copy(), position_cm=records["position_cm"].copy(),
                        detour=detour, detour_valid=detour_valid)
        for name, values in features.items():
            parts.setdefault(name, []).append(values)
        steps += len(records)
    if not steps:
        raise ValueError(f"recording has no decisions: {samples}")
    # Release each chunk list as soon as its concatenated array exists;
    # native-resolution maps otherwise hold two whole episodes at once.
    arrays = {}
    for name in list(parts):
        arrays[name] = np.concatenate(parts.pop(name))
    del parts
    stamps, positions = arrays.pop("time_ms"), arrays.pop("position_cm")
    arrays["action_change"] = label_action_changes(arrays["labels"])
    arrays["maneuver_weight"] = maneuver_weights(stamps, arrays["labels"][:, 4])
    arrays["stuck"], arrays["stuck_valid"] = future_stuck(stamps, positions, arrays["labels"])
    arrays["next_near_valid"] = np.r_[np.ones(steps - 1, np.uint8), np.uint8(0)]
    return arrays


def _manifest_index(manifest: dict[str, Any], manifest_path: Path,
                    input_variant: str) -> dict[str, Any]:
    if input_variant not in ("compact", "hd", "hd-familiar"):
        raise ValueError(f"unsupported navigation input variant: {input_variant}")
    familiar = input_variant == "hd-familiar"
    hd = input_variant in ("hd", "hd-familiar")
    cache_version = BC4_CACHE_VERSION if familiar else HD_CACHE_VERSION if hd else BC3_CACHE_VERSION
    navdemo_version, memory_version = (3, 4) if familiar else (2, 3)
    if (manifest["format"]["version"] != navdemo_version
            or manifest["format"]["navigation_memory_version"] != memory_version):
        raise ValueError("manifest recording and memory versions differ from the requested input variant")
    preprocessing = BC4_PREPROCESSING if familiar else HD_PREPROCESSING if hd else PREPROCESSING
    index: dict[str, Any] = {
        "cache_version": cache_version, "navdemo_version": navdemo_version, "navigation_memory_version": memory_version,
        "manifest": str(manifest_path), "heads": list(BC3_HEADS), "classes": list(BC3_CLASSES),
        "vector_size": VECTOR_SIZE, "near_shape": list(BC4_NEAR_SHAPE if familiar else HD_NEAR_SHAPE) if hd else [9, 32, 32],
        "far_shape": list(BC4_FAR_SHAPE if familiar else HD_FAR_SHAPE) if hd else [9, 25, 25],
        "preprocessing": preprocessing,
        "situations": list(SITUATIONS), "episodes": [],
        "class_counts": {"train": {head: [0] * count for head, count in zip(BC3_HEADS, BC3_CLASSES)}},
        "auxiliary": {"stuck": {"horizon_ms": 2000, "excursion_xz_cm": 25,
                                  "requires_requested_translation": True, "requires_full_horizon": True},
                      "next_near": {"target": "next encoded near map", "last_step_valid": False},
                      "detour": {"weight": .5, "available_steps": 0,
                                 "target_order": ["path_cm/1000 clipped [0,50]", "right", "up", "forward"],
                                 "teacher_versions": [1, 2], "memory_teacher_hints_excluded": True},
                      "true_map": {"available": False, "reason": f"not recorded in NAVDEMO v{navdemo_version}"}},
    }
    if hd:
        index["input_variant"] = input_variant
        index["auxiliary"]["next_near"]["target"] = "next compact encoded near map (aux_near only; not policy input)"
    return index


def training_index(source: Path, manifest_path: Path,
                   input_variant: str = "compact") -> dict[str, Any]:
    """Build the existing BC index in memory; training reads recordings directly."""
    if not source.is_absolute() or not manifest_path.is_absolute():
        raise ValueError("source and manifest paths must be absolute")
    source = source.resolve()
    manifest, entries = load_training_manifest(manifest_path, source)
    index = _manifest_index(manifest, manifest_path, input_variant)
    index["storage"] = {"preparation": "direct"}
    common_layout, decisions, changes = None, 0, 0
    for entry in entries:
        directory, scenario = entry["directory"], entry["scenario"]
        samples = nav_demos.samples_path(directory)
        if (nav_demos.episode_version(samples) != index["navdemo_version"]
                or nav_demos.episode_memory_version(samples) != index["navigation_memory_version"]):
            raise ValueError(f"training recording or memory version differs: {directory}")
        metadata = json.loads((directory / "episode.json").read_text(encoding="utf-8"))
        if metadata.get("decisions") != entry["decisions"]:
            raise ValueError(f"manifest decision count differs from recording: {directory}")
        verify_samples(samples, metadata, entry["samples_sha256"])
        layout = nav_demos.ray_layout(samples)
        if common_layout is None:
            common_layout = layout
            index["eye_layout"] = layout.tolist()
        elif not np.array_equal(layout, common_layout):
            raise ValueError("episodes have inconsistent ray layouts")
        teacher_version = metadata.get("teacher_version", scenario.get("teacher_version"))
        stats = metadata.get("nav_bc_stats")
        if stats is None:
            stats = episode_stats(samples, teacher_version)
        steps = entry["decisions"]
        if (stats["decisions"] != steps or type(stats["action_changes"]) is not int
                or not 0 <= stats["action_changes"] < steps
                or type(stats["detour_available_steps"]) is not int
                or not 0 <= stats["detour_available_steps"] <= steps
                or set(stats["class_counts"]) != set(BC3_HEADS)):
            raise ValueError(f"invalid direct training statistics: {directory}")
        for head, count in zip(BC3_HEADS, BC3_CLASSES):
            counts = stats["class_counts"][head]
            if (len(counts) != count or any(type(value) is not int or value < 0 for value in counts)
                    or sum(counts) != steps):
                raise ValueError(f"invalid direct training class counts: {directory}: {head}")
            index["class_counts"]["train"][head] = (
                np.asarray(index["class_counts"]["train"][head]) + counts).tolist()
        index["episodes"].append({
            "id": entry["id"], "family": entry["family"], "pair": [scenario["start"], scenario["goal"]],
            "start": scenario["start"], "goal": scenario["goal"], "region": scenario["region"],
            "split": "train", "steps": steps, "file": samples.relative_to(source).as_posix(),
            "source_kind": "teacher", "success": entry.get("teacher_goal_success") is True,
            "storage": "navdemo", "input_variant": input_variant, "teacher_version": teacher_version,
        })
        decisions += steps
        changes += stats["action_changes"]
        index["auxiliary"]["detour"]["available_steps"] += stats["detour_available_steps"]
    index["action_change_weight"] = action_change_weight(decisions, changes)
    index["action_change"] = {"N": decisions, "n_I": changes, "clip": [1, 10], "first_decision_excluded": True}
    index["maneuver_windows"] = {"radius_ms": 1000, "weight": 5, "event": "rising teacher press"}
    index["splits"] = {"train": {"pairs": len({tuple(ep["pair"]) for ep in index["episodes"]}),
                                 "episodes": len(entries), "steps": decisions}}
    return index


def prepare_manifest(source: Path, output: Path, manifest_path: Path,
                     chunk_records: int = 32, input_variant: str = "compact") -> dict[str, Any]:
    """Prepare the manifest's frozen training population without held-out reads."""
    if any(not path.is_absolute() for path in (source, output, manifest_path)):
        raise ValueError("source, output and manifest paths must be absolute")
    if output.resolve().is_relative_to(source.resolve()):
        raise ValueError("cache must be outside the source recordings")
    if input_variant not in ("compact", "hd", "hd-familiar"):
        raise ValueError(f"unsupported navigation input variant: {input_variant}")
    familiar = input_variant == "hd-familiar"
    hd = input_variant in ("hd", "hd-familiar")
    cache_version = BC4_CACHE_VERSION if familiar else HD_CACHE_VERSION if hd else BC3_CACHE_VERSION
    if (output / "index.json").exists():
        previous = json.loads((output / "index.json").read_text(encoding="utf-8"))
        if previous.get("cache_version") != cache_version:
            raise ValueError("input variants require separate cache directories")
    manifest, episodes = load_training_manifest(manifest_path, source)
    index = _manifest_index(manifest, manifest_path, input_variant)
    navdemo_version, memory_version = index["navdemo_version"], index["navigation_memory_version"]
    common_layout, changes, decisions = None, 0, 0
    (output / "episodes").mkdir(parents=True, exist_ok=True)
    cache_bytes = sum(path.stat().st_size for path in output.rglob("*") if path.is_file()) if hd else 0
    if hd:
        index["storage"] = {"compression": "npz", "reserve_bytes": 20 * 1024**3,
                            "cache_limit_bytes": 9 * 1024**3}
        if shutil.disk_usage(output).free < 20 * 1024**3 or cache_bytes > 9 * 1024**3:
            raise ValueError("HD cache requires 20 GiB free reserve and at most 9 GiB cache")
    for number, entry in enumerate(episodes, start=1):
        directory, scenario = entry["directory"], entry["scenario"]
        samples = nav_demos.samples_path(directory)
        if (nav_demos.episode_version(samples) != navdemo_version
                or nav_demos.episode_memory_version(samples) != memory_version):
            raise ValueError(f"training episode is not NAVDEMO v{navdemo_version} memory-v{memory_version}: {directory}")
        metadata = json.loads((directory / "episode.json").read_text(encoding="utf-8"))
        if metadata.get("decisions") != entry["decisions"]:
            raise ValueError(f"manifest decision count differs from recording: {directory}")
        verify_samples(samples, metadata, entry["samples_sha256"])
        layout = nav_demos.ray_layout(samples)
        if common_layout is None:
            common_layout = layout
            index["eye_layout"] = layout.tolist()
        elif not np.array_equal(layout, common_layout):
            raise ValueError("episodes have inconsistent ray layouts")
        teacher_version = metadata.get("teacher_version", scenario.get("teacher_version"))
        arrays = encode_episode(samples, layout, teacher_version, input_variant, chunk_records)
        steps = len(arrays["labels"])
        if steps != entry["decisions"]:
            raise ValueError(f"record count differs from manifest decisions: {directory}")
        identity = entry["id"]
        if not isinstance(identity, str) or Path(identity).name != identity or identity in (".", ".."):
            raise ValueError(f"invalid cache episode ID: {identity!r}")
        relative_file = Path("episodes") / f"{identity}.npz"
        destination = output / relative_file
        temporary = destination.with_suffix(".npz.tmp")
        if hd:
            episode_bytes = sum(values.nbytes for values in arrays.values())
            # Compression can expand incompressible data; allow ZIP overhead
            # before opening a temporary file that coexists with the old cache.
            write_bound = episode_bytes + episode_bytes // 100 + 1024**2
            if shutil.disk_usage(output).free - write_bound < 20 * 1024**3:
                raise ValueError("HD cache episode would consume the 20 GiB disk reserve")
        with temporary.open("wb") as stream:
            np.savez_compressed(stream, **arrays)
        if hd:
            new_bytes = temporary.stat().st_size
            replaced_bytes = destination.stat().st_size if destination.exists() else 0
            if (cache_bytes + new_bytes - replaced_bytes > 9 * 1024**3
                    or shutil.disk_usage(output).free < 20 * 1024**3):
                temporary.unlink()
                raise ValueError("HD cache exceeded its 9 GiB budget or 20 GiB free reserve")
            cache_bytes += new_bytes - replaced_bytes
        temporary.replace(destination)
        for head_index, (head, count) in enumerate(zip(BC3_HEADS, BC3_CLASSES)):
            counts = np.bincount(arrays["labels"][:, head_index], minlength=count)
            index["class_counts"]["train"][head] = (np.asarray(index["class_counts"]["train"][head]) + counts).tolist()
        index["episodes"].append({
            "id": identity, "family": entry["family"], "pair": [scenario["start"], scenario["goal"]],
            "start": scenario["start"], "goal": scenario["goal"], "region": scenario["region"],
            "split": "train", "steps": steps, "file": relative_file.as_posix(), "source_kind": "teacher",
            "success": entry.get("teacher_goal_success") is True,
        })
        changes += int(arrays["action_change"].sum())
        decisions += steps
        index["auxiliary"]["detour"]["available_steps"] += int(arrays["detour_valid"].sum())
        del arrays
        print(f"cache {number}/{len(episodes)} {identity}: {steps} steps (train)", flush=True)
    index["action_change_weight"] = action_change_weight(decisions, changes)
    index["action_change"] = {"N": decisions, "n_I": changes, "clip": [1, 10], "first_decision_excluded": True}
    index["maneuver_windows"] = {"radius_ms": 1000, "weight": 5, "event": "rising teacher press"}
    index["splits"] = {"train": {"pairs": len({tuple(ep["pair"]) for ep in index["episodes"]}),
                                 "episodes": len(episodes), "steps": decisions}}
    temporary_index = output / "index.json.tmp"
    temporary_index.write_text(json.dumps(index, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    temporary_index.replace(output / "index.json")
    print(json.dumps(index["splits"], ensure_ascii=False), flush=True)
    return index


def prepare(source: Path, output: Path, seed: int = 17, validation_fraction: float = .2,
            chunk_records: int = 32, split_index: Path | None = None,
            dagger_sources: list[Path] | None = None, manifest: Path | None = None,
            input_variant: str = "compact") -> dict[str, Any]:
    if manifest is not None:
        if split_index is not None or dagger_sources:
            raise ValueError("BC3 manifest preparation does not accept legacy split-index or DAgger sources")
        return prepare_manifest(source, output, manifest, chunk_records, input_variant)
    if input_variant != "compact":
        raise ValueError("HD preparation requires the frozen BC3 --manifest")
    dagger_sources = dagger_sources or []
    paths = [source, output, *dagger_sources, *([split_index] if split_index else [])]
    if any(not path.is_absolute() for path in paths):
        raise ValueError("source, output and split-index paths must be absolute")
    for recordings in (source, *dagger_sources):
        if recordings == output or recordings in output.parents:
            raise ValueError("cache must be outside the source recordings")
    if dagger_sources and split_index is None:
        raise ValueError("DAgger preparation requires --split-index")
    episodes, failed, unsplit_teacher, excluded_dagger = [], 0, 0, 0
    split = read_pair_split(split_index) if split_index else None
    for samples in nav_demos.episode_paths(source):
        if nav_demos.episode_version(samples) != 2:
            raise ValueError(f"only NAVDEMO v2 recordings are accepted: {samples}")
        metadata = json.loads((samples.parent / "episode.json").read_text(encoding="utf-8"))
        if metadata.get("success") is not True:
            failed += 1
            continue
        for key in ("start", "goal", "region"):
            if not isinstance(metadata.get(key), str) or not metadata[key]:
                raise ValueError(f"missing {key} in {samples.parent / 'episode.json'}")
        if split is not None:
            # Preserve every original held-out pair; successful v2 pairs absent
            # from the old successful-only index are training examples.
            split.setdefault((metadata["start"], metadata["goal"]), "train")
        episodes.append({**metadata, "samples": samples, "source": source, "source_kind": "teacher"})
    if not episodes:
        raise ValueError(f"no successful episodes in {source}")
    if split is None:
        split = split_pairs(episodes, validation_fraction, seed)
    seen_samples = {episode["samples"].resolve() for episode in episodes}
    for source_number, dagger_source in enumerate(dagger_sources, start=1):
        for samples in nav_demos.episode_paths(dagger_source):
            if nav_demos.episode_version(samples) != 2:
                raise ValueError(f"only NAVDEMO v2 recordings are accepted: {samples}")
            metadata = json.loads((samples.parent / "episode.json").read_text(encoding="utf-8"))
            pair = (metadata.get("start"), metadata.get("goal"))
            # Held-out and control drives cannot become training examples.
            # Original training pairs remain eligible even when teacher v2
            # failed on the initial route; the visited states still have labels.
            if metadata.get("split") == "control" or split.get(pair) != "train":
                excluded_dagger += 1
                continue
            if samples.resolve() in seen_samples:
                raise ValueError(f"duplicate recording source: {samples}")
            seen_samples.add(samples.resolve())
            if not isinstance(metadata.get("region"), str) or not metadata["region"]:
                raise ValueError(f"missing region in {samples.parent / 'episode.json'}")
            episodes.append({**metadata, "samples": samples, "source": dagger_source,
                             "source_kind": "dagger", "source_number": source_number})
    (output / "episodes").mkdir(parents=True, exist_ok=True)
    index: dict[str, Any] = {
        "cache_version": CACHE_VERSION, "navdemo_version": 2,
        "seed": seed, "validation_fraction": validation_fraction,
        "heads": list(HEADS), "classes": list(CLASSES), "vector_size": VECTOR_SIZE,
        "near_shape": [9, 32, 32], "far_shape": [9, 25, 25],
        "situations": list(SITUATIONS), "preprocessing": PREPROCESSING,
        "excluded_unsuccessful_episodes": failed,
        "excluded_unsplit_teacher_episodes": unsplit_teacher,
        "excluded_dagger_episodes": excluded_dagger,
        "split_index": str(split_index) if split_index else None, "episodes": [],
        "class_counts": {part: {head: [0] * classes for head, classes in zip(HEADS, CLASSES)}
                         for part in ("train", "validation")},
    }
    common_layout = None
    ids = set()
    for number, episode in enumerate(episodes, start=1):
        samples = episode["samples"]
        identity = samples.parent.relative_to(episode["source"]).as_posix().replace("/", "__")
        if not identity or identity == ".":
            identity = samples.parent.name
        if episode["source_kind"] == "dagger":
            identity = f"dagger{episode['source_number']}__{identity}"
        if identity in ids:
            raise ValueError(f"duplicate cache identity: {identity}")
        ids.add(identity)
        layout = nav_demos.ray_layout(samples)
        if common_layout is None:
            common_layout = layout
            index["eye_layout"] = layout.tolist()
        elif not np.array_equal(layout, common_layout):
            raise ValueError("episodes have inconsistent ray layouts")
        parts: dict[str, list[np.ndarray]] = {}
        steps = 0
        previous_teacher = None
        for records in nav_demos.iter_chunks(samples, chunk_records):
            inputs = nav_demos.learner_input(records)
            features = _pack(encode_features(inputs, layout))
            labels = encode_labels(records["keys"])
            features.update(labels=labels, baseline=road_baseline(inputs),
                            situations=situation_masks(inputs, layout, labels, steps),
                            action_change=action_change_mask(records, previous_teacher))
            previous_teacher = labels[-1].copy()
            for name, values in features.items():
                parts.setdefault(name, []).append(values)
            steps += len(records)
        if not steps or steps != episode["decisions"]:
            raise ValueError(f"record count differs from episode decisions: {samples}")
        arrays = {name: np.concatenate(values) for name, values in parts.items()}
        del parts
        relative_file = Path("episodes") / f"{identity}.npz"
        destination = output / relative_file
        temporary = destination.with_suffix(".npz.tmp")
        with temporary.open("wb") as stream:
            np.savez_compressed(stream, **arrays)
        temporary.replace(destination)
        part = split[(episode["start"], episode["goal"])]
        for head_index, (head, classes) in enumerate(zip(HEADS, CLASSES)):
            counts = np.bincount(arrays["labels"][:, head_index], minlength=classes)
            index["class_counts"][part][head] = (np.asarray(index["class_counts"][part][head]) + counts).tolist()
        index["episodes"].append({
            "id": identity, "pair": [episode["start"], episode["goal"]],
            "start": episode["start"], "goal": episode["goal"], "region": episode["region"],
            "split": part, "steps": steps, "file": relative_file.as_posix(),
            "source_kind": episode["source_kind"], "success": episode.get("success") is True,
        })
        del arrays
        print(f"cache {number}/{len(episodes)} {identity}: {steps} steps ({part})", flush=True)
    index["splits"] = {
        part: {"pairs": sum(value == part for value in split.values()),
               "episodes": sum(episode["split"] == part for episode in index["episodes"]),
               "steps": sum(episode["steps"] for episode in index["episodes"] if episode["split"] == part)}
        for part in ("train", "validation")
    }
    index["region_pairs"] = {
        region: {part: len({tuple(episode["pair"]) for episode in index["episodes"]
                            if episode["region"] == region and episode["split"] == part})
                 for part in ("train", "validation")}
        for region in sorted({episode["region"] for episode in episodes})
    }
    temporary_index = output / "index.json.tmp"
    temporary_index.write_text(json.dumps(index, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    temporary_index.replace(output / "index.json")
    print(json.dumps(index["splits"], ensure_ascii=False), flush=True)
    return index


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prepare_command = commands.add_parser("prepare")
    prepare_command.add_argument("--source", type=Path, required=True)
    prepare_command.add_argument("--output", type=Path, required=True)
    prepare_command.add_argument("--seed", type=int, default=17)
    prepare_command.add_argument("--validation-fraction", type=float, default=.2)
    prepare_command.add_argument("--chunk-records", type=int, default=32)
    prepare_command.add_argument("--split-index", type=Path,
                                 help="preserve train/validation pairs from an earlier cache index")
    prepare_command.add_argument("--dagger-source", type=Path, action="append", default=[],
                                 help="add all labelled learner drives from original training pairs; repeat per round")
    prepare_command.add_argument("--manifest", type=Path,
                                 help="use only frozen manifest training recordings (v4 BC3 or v5 BC4)")
    prepare_command.add_argument("--input-variant", choices=("compact", "hd", "hd-familiar"), default="compact",
                                 help="HD keeps native map cells and physical-feet-relative height extrema")
    args = parser.parse_args()
    prepare(args.source, args.output, args.seed, args.validation_fraction, args.chunk_records,
            args.split_index, args.dagger_source, args.manifest, args.input_variant)


if __name__ == "__main__":
    main()
