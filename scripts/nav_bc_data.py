#!/usr/bin/env python3
"""Compact, sensor-only navigation examples for recurrent behaviour cloning.

    python3 -I scripts/nav_bc_data.py prepare --source /absolute/nav-demos \
        --output /absolute/nav-bc/cache

Only numpy is required. Preparation streams the recordings one episode at a time.
The cache contains successful teacher episodes and all DAgger episodes from
training pairs. A saved split keeps exact (start, goal) pairs in their original
partition across rounds.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import Any

import numpy as np

# Isolated mode deliberately omits the script directory from sys.path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import nav_demos


HEADS = ("forward", "strafe", "turn", "walk", "jump", "climb")
CLASSES = (3, 3, 3, 2, 2, 2)
SITUATIONS = ("wall", "turn", "beginning")
VECTOR_SIZE = nav_demos.RAY_COUNT * 9 + 10 + 14
CACHE_VERSION = 2
INPUT_NAMES = frozenset(("body", "roads", "previous_keys", "eye", "memory_near", "memory_far"))

PREPROCESSING = {
    "eye": "per ray: distance/max_range, kind one-hot[0:6], normal_y, hit; flatten ray-major",
    "memory": "7 probability channels block mean (near 2x2, far 4x4); known-height mean and validity fraction",
    "height": "subtract nearest valid near-memory ground height (far fallback), /200cm, clip [-10,10]; unknown=0",
    "height_reference": "nearest to body origin in sensor map, no world position/yaw or teacher_hint access",
    "roads": "next_direction[3], next_distance/1000, goal_direction[3], goal_distance/1000, remaining_visual/1000, goal_in_known; distances clipped [0,50]",
    "body": "velocity[3]/500 clipped [-4,4], flags[5], ground_normal[3], slope/90, head_yaw/180, head_pitch/90",
    "vector_order": ["eye", "roads", "body"],
    "map_order": [*nav_demos.CHANNELS, "relative_ground_height", "height_valid_fraction"],
    "cache_quantization": "vector/height float16, map probabilities/validity round(255*x) uint8, labels uint8",
    "road_baseline": "always forward and walk, turn=-sign(atan2(next_direction.x,next_direction.z)) outside 5deg",
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
    probability = memory["channels"].astype(np.float32) / 255
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


def encode_features(inputs: dict[str, np.ndarray], layout: np.ndarray) -> dict[str, np.ndarray]:
    """Encode only nav_demos.learner_input(), returning float32 NCHW maps/vector.

    The exact allowed-key check catches accidental use of full records or privileged
    annotations at the model boundary. The recorder's previous_keys field is
    accepted but never encoded: only the GRU carries action history. Unknown
    ground contributes zero height.
    """
    if frozenset(inputs) != INPUT_NAMES:
        raise ValueError("encode_features accepts only nav_demos.learner_input() fields")
    layout = np.asarray(layout, dtype=np.float32)
    if layout.shape != (nav_demos.RAY_COUNT, 3) or not np.isfinite(layout).all() or (layout[:, 2] <= 0).any():
        raise ValueError("invalid eye layout")
    count = len(inputs["eye"])
    reference, known = _nearest_height(inputs["memory_near"])
    far_reference, _ = _nearest_height(inputs["memory_far"])
    reference = np.where(known, reference, far_reference)
    near = _pool_memory(inputs["memory_near"], 2, reference)
    far = _pool_memory(inputs["memory_far"], 4, reference)

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
    road_features = np.concatenate((roads["next_direction"], distance("next_distance_cm"),
                                   roads["goal_direction"], distance("goal_distance_cm"),
                                   distance("remaining_visual_cm"), roads["goal_in_known"][:, None]), axis=1)
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


def action_change_mask(records: np.ndarray) -> np.ndarray:
    """Teacher's six action heads differ from the actor's preceding command.

    In DAgger recordings the actor is the learner; head yaw/pitch are not policy
    outputs and therefore do not define an action change.
    """
    return np.any(encode_labels(records["keys"]) !=
                  encode_labels(records["previous_keys"]), axis=1).astype(np.uint8)


def road_baseline(inputs: dict[str, np.ndarray], threshold_deg: float = 5) -> np.ndarray:
    """Road-only baseline uses the engine's negative turn for positive local x."""
    direction = inputs["roads"]["next_direction"]
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
    for name in ("near", "far"):
        values = features[name]
        packed[f"{name}_channels"] = np.rint(values[:, :7] * 255).astype(np.uint8)
        packed[f"{name}_height"] = values[:, 7:8].astype(np.float16)
        packed[f"{name}_valid"] = np.rint(values[:, 8:9] * 255).astype(np.uint8)
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


def load_cached(path: str | Path) -> dict[str, np.ndarray]:
    """Read one compressed episode; callers can bound memory with an episode LRU."""
    with np.load(path, allow_pickle=False) as source:
        result = {"vector": source["vector"].astype(np.float32),
                  "labels": source["labels"].astype(np.int64),
                  "baseline": source["baseline"].astype(np.int64),
                  "situations": source["situations"].astype(np.uint8),
                  "action_change": source["action_change"].astype(np.uint8)}
        for name in ("near", "far"):
            result[name] = np.concatenate((source[f"{name}_channels"].astype(np.float32) / 255,
                                           source[f"{name}_height"].astype(np.float32),
                                           source[f"{name}_valid"].astype(np.float32) / 255), axis=1)
    return result


def prepare(source: Path, output: Path, seed: int = 17, validation_fraction: float = .2,
            chunk_records: int = 32, split_index: Path | None = None,
            dagger_sources: list[Path] | None = None) -> dict[str, Any]:
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
        metadata = json.loads((samples.parent / "episode.json").read_text(encoding="utf-8"))
        if metadata.get("success") is not True:
            failed += 1
            continue
        for key in ("start", "goal", "region"):
            if not isinstance(metadata.get(key), str) or not metadata[key]:
                raise ValueError(f"missing {key} in {samples.parent / 'episode.json'}")
        if split is not None and (metadata["start"], metadata["goal"]) not in split:
            unsplit_teacher += 1
            continue
        episodes.append({**metadata, "samples": samples, "source": source, "source_kind": "teacher"})
    if not episodes:
        raise ValueError(f"no successful episodes in {source}")
    if split is None:
        split = split_pairs(episodes, validation_fraction, seed)
    seen_samples = {episode["samples"].resolve() for episode in episodes}
    for source_number, dagger_source in enumerate(dagger_sources, start=1):
        for samples in nav_demos.episode_paths(dagger_source):
            metadata = json.loads((samples.parent / "episode.json").read_text(encoding="utf-8"))
            pair = (metadata.get("start"), metadata.get("goal"))
            # The original split is authoritative, including for failed drives.
            # Control drives and new pairs cannot become training examples.
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
        "cache_version": CACHE_VERSION, "seed": seed, "validation_fraction": validation_fraction,
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
        for records in nav_demos.iter_chunks(samples, chunk_records):
            inputs = nav_demos.learner_input(records)
            features = _pack(encode_features(inputs, layout))
            labels = encode_labels(records["keys"])
            features.update(labels=labels, baseline=road_baseline(inputs),
                            situations=situation_masks(inputs, layout, labels, steps),
                            action_change=action_change_mask(records))
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
    args = parser.parse_args()
    prepare(args.source, args.output, args.seed, args.validation_fraction, args.chunk_records,
            args.split_index, args.dagger_source)


if __name__ == "__main__":
    main()
