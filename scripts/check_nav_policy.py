#!/usr/bin/env python3
"""Create the asset-free C++/NumPy navigation-policy contract fixture.

The default is deterministic small v2 weights and 1000 synthetic NAVDEMO v2 records.
Use --navdemo-version 1 for the synthetic v1 preprocessing contract, or
--weight-version 3 for the HD input contract. Supplied weights select their input
version and map layout from their own header.
For the recorded-step acceptance check, pass the trained weights and an episode
with at least 1000 decisions (or a recording directory containing such an episode):

    OPENBLAS_NUM_THREADS=1 python3 -I scripts/check_nav_policy.py \
        --output /absolute/build/navpolicy-real --weights /absolute/policy.navbc \
        --source /absolute/nav-demos --steps 1000 --torch
    /absolute/build/gothic-navpolicy-contract \
        /absolute/policy.navbc /absolute/build/navpolicy-real/parity.bin.gz
    /absolute/build/gothic-navpolicy-contract --benchmark \
        /absolute/policy.navbc /absolute/build/navpolicy-real/parity.bin.gz \
        --npcs 1 --decisions 2000

Only NumPy and the standard library are required. No weights or records belong in
git; the output directory is deliberately explicit.
"""

from __future__ import annotations

import argparse
import gzip
import json
from pathlib import Path
import sys
from typing import Iterator

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nav_bc_data as data
from nav_bc_numpy import HEAD_SIZES, NumpyPolicy
import nav_demos as demos


ROAD_FIELDS = {
    1: ["next_direction", "next_distance_cm", "goal_direction", "goal_distance_cm", "remaining_visual_cm", "goal_in_known"],
    2: ["carrot_direction", "carrot_distance_cm", "goal_direction", "goal_distance_cm", "remaining_known_cm", "beyond_known"],
}
ROAD_FIELDS[3] = ROAD_FIELDS[2].copy()


def fixture_preprocessing(navdemo_version: int, input_variant: str = "compact") -> dict:
    """Describe the checker input without changing training metadata globally."""
    if navdemo_version not in ROAD_FIELDS:
        raise ValueError(f"unsupported NAVDEMO version: {navdemo_version}")
    if input_variant not in ("compact", "hd", "hd-familiar"):
        raise ValueError(f"unsupported fixture input variant: {input_variant}")
    if input_variant == "hd" and navdemo_version != 2:
        raise ValueError("HD fixture input requires NAVDEMO v2")
    if input_variant == "hd-familiar" and navdemo_version != 3:
        raise ValueError("HD familiar fixture input requires NAVDEMO v3")
    preprocessing = {**(data.BC4_PREPROCESSING if input_variant == "hd-familiar" else
                         data.HD_PREPROCESSING if input_variant == "hd" else data.PREPROCESSING),
                     "navdemo_version": navdemo_version,
                     "roads_schema": "carrot_v2" if navdemo_version >= 2 else "next_v1",
                     "roads_fields": ROAD_FIELDS[navdemo_version].copy()}
    if navdemo_version == 1:
        preprocessing["roads"] = (
            "next_direction[3], next_distance_cm/1000, goal_direction[3], goal_distance_cm/1000, "
            "remaining_visual_cm/1000, goal_in_known; distances clipped [0,50]")
        preprocessing["road_baseline"] = (
            "always forward and walk, turn=-sign(atan2(next_direction.x,next_direction.z)) outside 5deg")
    return preprocessing


def weight_input_version(header: dict, vector_size: int) -> int:
    """Keep fixture inputs consistent with the supplied policy's declared shape."""
    preprocessing = header.get("preprocessing", {})
    if vector_size != data.VECTOR_SIZE or preprocessing.get("vector_order") != ["eye", "roads", "body"]:
        raise ValueError("weights must declare sensor-only vector_order: eye, roads, body and vector_size=2166")
    version = preprocessing.get("navdemo_version", 1)
    if type(version) is not int or version not in ROAD_FIELDS:
        raise ValueError(f"unsupported weight NAVDEMO version: {version}")
    expected = fixture_preprocessing(version, weight_input_variant(header))
    for key in ("roads_schema", "roads_fields"):
        if (version >= 2 or key in preprocessing) and preprocessing.get(key) != expected[key]:
            raise ValueError(f"weight {key} differs from NAVDEMO v{version} preprocessing")
    return version


def weight_input_variant(header: dict) -> str:
    """Read the declared HD layout while preserving headers from v1 and v2."""
    variant = header.get("preprocessing", {}).get("input_variant", "compact")
    expected = "hd-familiar" if header.get("version") == 4 else "hd" if header.get("version") == 3 else "compact"
    if variant != expected:
        raise ValueError(f"weight format v{header.get('version')} requires {expected} input_variant")
    return variant


def fixture_features(record: np.ndarray, layout: np.ndarray, navdemo_version: int,
                     input_variant: str = "compact") -> dict[str, np.ndarray]:
    """Encode v1 only here; its ten road slots share the v2 numeric transform."""
    if navdemo_version not in demos.RECORD_DTYPES or record.dtype != demos.RECORD_DTYPES[navdemo_version]:
        raise ValueError("record dtype differs from the weight NAVDEMO version")
    inputs = demos.learner_input(record)
    if navdemo_version == 1:
        # Packed road layouts and normalizations match: next/carrot direction,
        # distance, goal, remainder, then the version's own unchanged flag bit.
        # The checker only reads this view; the source record stays unchanged.
        inputs = {**inputs, "roads": inputs["roads"].view(demos.ROADS_V2_DTYPE)}
    return data.encode_features(inputs, layout, input_variant=input_variant,
                                feet_height_cm=record["position_cm"][:, 1] if input_variant in ("hd", "hd-familiar") else None)


def synthetic_weights(path: Path, navdemo_version: int = 2, format_version: int = 2,
                      temperatures: dict[str, float] | None = None) -> None:
    """A small valid export built without importing the Torch training module."""
    input_variant = "hd-familiar" if format_version == 4 else "hd" if format_version == 3 else "compact"
    preprocessing = fixture_preprocessing(navdemo_version, input_variant)
    near_shape = data.BC4_NEAR_SHAPE if format_version == 4 else data.HD_NEAR_SHAPE if format_version == 3 else (9, 32, 32)
    far_shape = data.BC4_FAR_SHAPE if format_version == 4 else data.HD_FAR_SHAPE if format_version == 3 else (9, 25, 25)
    channels = near_shape[0]
    head_sizes = HEAD_SIZES[format_version]
    hidden = 8
    shapes = []
    for prefix in ("near_encoder", "far_encoder"):
        shapes += [(prefix + ".0.weight", (8, channels, 3, 3)), (prefix + ".0.bias", (8,)),
                   (prefix + ".2.weight", (16, 8, 3, 3)), (prefix + ".2.bias", (16,))]
    shapes += [("vector_encoder.0.weight", (128, data.VECTOR_SIZE)), ("vector_encoder.0.bias", (128,)),
               ("fusion.0.weight", (128, 256)), ("fusion.0.bias", (128,)),
               ("gru.weight_ih_l0", (3 * hidden, 128)), ("gru.weight_hh_l0", (3 * hidden, hidden)),
               ("gru.bias_ih_l0", (3 * hidden,)), ("gru.bias_hh_l0", (3 * hidden,))]
    for name, classes in head_sizes.items():
        shapes += [("head_" + name + ".weight", (classes, hidden)), ("head_" + name + ".bias", (classes,))]
    rng = np.random.default_rng(1705)
    tensors, payload, offset = [], [], 0
    for name, shape in shapes:
        scale = .4 / np.sqrt(np.prod(shape[1:])) if len(shape) > 1 else .02
        values = rng.uniform(-scale, scale, shape).astype("<f4").tobytes()
        tensors.append({"name": name, "shape": list(shape), "dtype": "float32", "offset": offset, "nbytes": len(values)})
        payload.append(values)
        offset += len(values)
    architecture = {
        "name": "map-cnn-vector-gru", "version": 1,
        "inputs": {"near": list(near_shape), "far": list(far_shape), "vector": [data.VECTOR_SIZE],
                   "dtype": "float32", "map_layout": "CHW"},
        "map_encoders": {"separate_weights": True, "layers": [
            {"type": "conv2d", "in_channels": channels, "out_channels": 8, "kernel": 3, "stride": 2, "padding": 1, "activation": "relu"},
            {"type": "conv2d", "in_channels": 8, "out_channels": 16, "kernel": 3, "stride": 2, "padding": 1, "activation": "relu"},
            {"type": "adaptive_avg_pool2d", "output": [2, 2],
             "bins": "start=floor(i*input/output); end=ceil((i+1)*input/output)"},
            {"type": "flatten", "order": "CHW", "output": 64}]},
        "vector_encoder": {"in": data.VECTOR_SIZE, "out": 128, "activation": "relu"},
        "fusion": {"concat": ["near", "far", "vector"], "in": 256, "out": 128, "activation": "relu"},
        "gru": {"input_size": 128, "hidden_size": hidden, "layers": 1,
                "gate_order": ["reset", "update", "new"], "initial_hidden": "zeros"},
        "heads": {name: {"classes": classes, "values": [-1, 0, 1] if index < 3 else list(range(classes))}
                  for index, (name, classes) in enumerate(head_sizes.items())},
    }
    header = {"format": "gothic-nav-bc", "version": format_version, "byte_order": "little", "data_nbytes": offset,
              "offset_origin": "first byte after JSON header newline", "architecture": architecture,
              "preprocessing": preprocessing, "tensors": tensors}
    if temperatures is not None:
        header["temperatures"] = temperatures
    with path.open("wb") as stream:
        stream.write(json.dumps(header, separators=(",", ":")).encode() + b"\n")
        for values in payload:
            stream.write(values)


def synthetic_records(steps: int, navdemo_version: int = 2) -> tuple[np.ndarray, Iterator[np.ndarray]]:
    if navdemo_version not in demos.RECORD_DTYPES:
        raise ValueError(f"unsupported NAVDEMO version: {navdemo_version}")
    layout = np.zeros((demos.RAY_COUNT, 3), dtype=np.float32)
    layout[:, 0] = np.linspace(-179, 179, demos.RAY_COUNT)
    layout[:, 1] = np.tile(np.arange(-3, 4), 34) * 10
    layout[:, 2] = np.where(np.abs(layout[:, 0]) < 45, 10000, 3000)

    def records() -> Iterator[np.ndarray]:
        rng = np.random.default_rng(1731)
        base = np.zeros(1, dtype=demos.RECORD_DTYPES[navdemo_version])
        for name in ("memory_near", "memory_far"):
            memory = base[name]
            # A varied small tile keeps the generated contract compact even
            # though its payload contains 1000 full-size raw sensor records.
            z, x = np.indices(memory.shape[1:]) % 8
            memory["channels"] = rng.integers(0, 256, (8, 8, memory["channels"].shape[-1]), dtype=np.uint8)[z, x]
            memory["height_valid"] = rng.integers(0, 2, (8, 8), dtype=np.uint8)[z, x]
            heights = rng.integers(-1500, 1501, (8, 8)).astype(np.float32) + 12000
            memory["ground_height_cm"] = heights[z, x]
            memory["ground_height_cm"][memory["height_valid"] == 0] = 0
        for step in range(steps):
            record = base.copy()
            record["time_ms"] = (step + 1) * 100
            record["position_cm"] = [step, 12000, -step * 2]
            record["body_yaw_deg"] = step * .25
            record["body"]["velocity_cm_s"] = rng.uniform(-2500, 2500, (1, 3))
            record["body"]["flags"] = rng.integers(0, 2, (1, 5), dtype=np.uint8)
            record["body"]["ground_normal"] = rng.uniform(-1, 1, (1, 3))
            record["body"]["ground_slope_deg"] = rng.uniform(0, 180)
            record["body"]["head_yaw_deg"] = rng.uniform(-180, 180)
            record["body"]["head_pitch_deg"] = rng.uniform(-90, 90)
            target = "carrot_direction" if navdemo_version >= 2 else "next_direction"
            for name in (target, "goal_direction"):
                direction = rng.normal(size=3)
                record["roads"][name] = direction / np.linalg.norm(direction)
            distances = (("carrot_distance_cm", "goal_distance_cm", "remaining_known_cm") if navdemo_version >= 2 else
                         ("next_distance_cm", "goal_distance_cm", "remaining_visual_cm"))
            for name in distances:
                record["roads"][name] = rng.uniform(0, 75000)
            record["roads"]["beyond_known" if navdemo_version >= 2 else "goal_in_known"] = step % 2
            for name in ("previous_keys", "keys"):
                for axis in ("forward", "strafe", "turn"):
                    record[name][axis] = rng.integers(-1, 2)
                record[name]["head_yaw_deg"] = rng.uniform(-180, 180)
                record[name]["head_pitch_deg"] = rng.uniform(-90, 90)
                for flag in ("walk", "jump", "climb"):
                    record[name][flag] = rng.integers(0, 2)
            eye = record["eye"]
            eye["distance_cm"] = rng.uniform(0, 1, (1, demos.RAY_COUNT)) * layout[:, 2]
            eye["kind"] = rng.integers(0, 6, eye.shape, dtype=np.uint8)
            eye["normal_y"] = rng.uniform(-1, 1, eye.shape)
            eye["hit"] = rng.integers(0, 2, eye.shape, dtype=np.uint8)
            eye["complete"] = 1
            # Cover near-reference ties, a far-height fallback and all-unknown maps.
            if step % 17 == 0:
                record["memory_near"]["height_valid"] = 0
                record["memory_near"]["ground_height_cm"] = 0
            if step % 29 == 0:
                for name in ("memory_near", "memory_far"):
                    record[name]["height_valid"] = 0
                    record[name]["ground_height_cm"] = 0
            record["teacher_hint"]["path_cm"] = 99999
            yield record
    return layout, records()


def recorded_steps(source: Path, steps: int, navdemo_version: int = 2) -> tuple[Path, np.ndarray, Iterator[np.ndarray]]:
    candidates = demos.episode_paths(source)
    # Metadata permits selection without decompressing other recordings.
    for samples in candidates:
        if demos.episode_version(samples) != navdemo_version:
            continue
        metadata = json.loads((samples.parent / "episode.json").read_text(encoding="utf-8"))
        if metadata.get("decisions", 0) < steps:
            continue
        layout = demos.ray_layout(samples)

        def records() -> Iterator[np.ndarray]:
            count = 0
            for chunk in demos.iter_chunks(samples, 8):
                for index in range(len(chunk)):
                    if count == steps:
                        return
                    count += 1
                    yield chunk[index:index + 1]
            if count != steps:
                raise ValueError(f"episode metadata promised {steps} steps but only {count} were read")
        return samples, layout, records()
    raise ValueError(f"no NAVDEMO v{navdemo_version} episode with {steps} consecutive recorded steps in {source}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--weights", type=Path)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--navdemo-version", type=int, choices=(1, 2, 3),
                        help="synthetic weight/record input version (default: 2); supplied weights use their header")
    parser.add_argument("--weight-version", type=int, choices=(1, 2, 3, 4),
                        help="synthetic weight format (default: 2); supplied weights use their header")
    parser.add_argument("--torch", action="store_true", help="also compare exported weights with Torch on every step")
    parser.add_argument("--steps", type=int, default=1000)
    args = parser.parse_args()
    if not args.output.is_absolute() or args.steps < 1:
        parser.error("--output must be absolute and --steps must be positive")
    if args.weights is not None and (args.navdemo_version is not None or args.weight_version is not None):
        parser.error("--navdemo-version/--weight-version are only for generated weights; supplied weights select their versions")
    args.output.mkdir(parents=True, exist_ok=True)
    weights = args.weights or args.output / "fixture.navbc"
    if args.weights is None:
        synthetic_weights(weights, args.navdemo_version or (3 if args.weight_version == 4 else 2), args.weight_version or 2)
    policy = NumpyPolicy(weights)
    torch_policy = torch_hidden = None
    torch_logit_error = torch_hidden_error = 0.0
    if args.torch:
        import torch
        from nav_bc_model import NavPolicy
        torch.set_num_threads(1)
        torch_policy = NavPolicy(policy.vector_size, policy.hidden_size, format_version=policy.format_version).eval()
        torch_policy.load_state_dict({name: torch.from_numpy(value.copy()) for name, value in policy.weights.items()})
    navdemo_version = weight_input_version(policy.header, policy.vector_size)
    input_variant = weight_input_variant(policy.header)
    if args.source:
        source, layout, records = recorded_steps(args.source, args.steps, navdemo_version)
        source_name = str(source)
    else:
        layout, records = synthetic_records(args.steps, navdemo_version)
        source_name = "deterministic synthetic NAVDEMO records"
    header = {"format": "navpolicy-parity", "version": 1, "steps": args.steps,
              "navdemo_version": navdemo_version,
              "weight_version": policy.format_version,
              "near_shape": policy.header["architecture"]["inputs"]["near"],
              "far_shape": policy.header["architecture"]["inputs"]["far"],
              "record_bytes": demos.RECORD_BYTES_BY_VERSION[navdemo_version], "vector_size": policy.vector_size,
              "hidden_size": policy.hidden_size, "heads": list(policy.head_names), "classes": list(policy.head_sizes.values()),
              "temperatures": policy.temperatures,
              "eye_layout": layout.tolist(), "source": source_name, "carry_hidden": True}
    if policy.format_version == 4:
        header["navigation_memory_version"] = policy.header["preprocessing"]["navigation_memory_version"]
    destination = args.output / "parity.bin.gz"
    hidden = None
    count = 0
    with gzip.open(destination, "wb", compresslevel=1) as stream:
        stream.write(json.dumps(header, separators=(",", ":")).encode() + b"\n")
        for record in records:
            features = fixture_features(record, layout, navdemo_version, input_variant)
            logits, hidden = policy.step(features["near"][0], features["far"][0], features["vector"][0], hidden)
            if torch_policy is not None:
                inputs = [torch.from_numpy(features[name][:, None]) for name in ("near", "far", "vector")]
                with torch.inference_mode():
                    torch_logits, torch_hidden = torch_policy(*inputs, torch_hidden)
                torch_logit_error = max(torch_logit_error, *(float(np.abs(logits[name] - torch_logits[name][0, 0].numpy()).max())
                                                            for name in policy.head_names))
                torch_hidden_error = max(torch_hidden_error, float(np.abs(hidden - torch_hidden[0, 0].numpy()).max()))
                if max(torch_logit_error, torch_hidden_error) > 1e-4:
                    raise ValueError(f"Torch/NumPy parity failed at step {count + 1}: logits={torch_logit_error}, hidden={torch_hidden_error}")
            stream.write(record.tobytes())
            for name in ("near", "far", "vector"):
                stream.write(features[name].astype("<f4", copy=False).tobytes())
            for name in policy.head_names:
                stream.write(logits[name].astype("<f4", copy=False).tobytes())
            stream.write(hidden.astype("<f4", copy=False).tobytes())
            count += 1
    if count != args.steps:
        raise ValueError(f"expected {args.steps} steps, wrote {count}")
    print(json.dumps({"weights": str(weights), "fixture": str(destination), "steps": count,
                      "source": source_name, "navdemo_version": navdemo_version,
                      "weight_version": policy.format_version,
                      "input_variant": input_variant,
                      "hidden_size": policy.hidden_size, "carry_hidden": True,
                      "torch_checked": args.torch, "torch_max_logit_error": torch_logit_error,
                      "torch_max_hidden_error": torch_hidden_error}))


if __name__ == "__main__":
    main()
