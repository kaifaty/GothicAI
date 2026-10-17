#!/usr/bin/env python3
"""Create the asset-free C++/NumPy navigation-policy contract fixture.

The default is deterministic small weights and 1000 synthetic NAVDEMO records.
For the recorded-step acceptance check, pass the trained weights and an episode
with at least 1000 decisions (or a recording directory containing such an episode):

    OPENBLAS_NUM_THREADS=1 python3 -I scripts/check_nav_policy.py \
        --output /absolute/build/navpolicy-real --weights /absolute/policy.navbc \
        --source /absolute/nav-demos --steps 1000
    /absolute/build/gothic-navpolicy-contract \
        /absolute/policy.navbc /absolute/build/navpolicy-real/parity.bin.gz

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
from nav_bc_numpy import NumpyPolicy
import nav_demos as demos


def synthetic_weights(path: Path) -> None:
    """A small valid export built without importing the Torch training module."""
    hidden = 8
    shapes = []
    for prefix in ("near_encoder", "far_encoder"):
        shapes += [(prefix + ".0.weight", (8, 9, 3, 3)), (prefix + ".0.bias", (8,)),
                   (prefix + ".2.weight", (16, 8, 3, 3)), (prefix + ".2.bias", (16,))]
    shapes += [("vector_encoder.0.weight", (128, data.VECTOR_SIZE)), ("vector_encoder.0.bias", (128,)),
               ("fusion.0.weight", (128, 256)), ("fusion.0.bias", (128,)),
               ("gru.weight_ih_l0", (3 * hidden, 128)), ("gru.weight_hh_l0", (3 * hidden, hidden)),
               ("gru.bias_ih_l0", (3 * hidden,)), ("gru.bias_hh_l0", (3 * hidden,))]
    for name, classes in zip(data.HEADS, data.CLASSES):
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
        "inputs": {"near": [9, 32, 32], "far": [9, 25, 25], "vector": [data.VECTOR_SIZE],
                   "dtype": "float32", "map_layout": "CHW"},
        "map_encoders": {"separate_weights": True, "layers": [
            {"type": "conv2d", "in_channels": 9, "out_channels": 8, "kernel": 3, "stride": 2, "padding": 1, "activation": "relu"},
            {"type": "conv2d", "in_channels": 8, "out_channels": 16, "kernel": 3, "stride": 2, "padding": 1, "activation": "relu"},
            {"type": "adaptive_avg_pool2d", "output": [2, 2],
             "bins": "start=floor(i*input/output); end=ceil((i+1)*input/output)"},
            {"type": "flatten", "order": "CHW", "output": 64}]},
        "vector_encoder": {"in": data.VECTOR_SIZE, "out": 128, "activation": "relu"},
        "fusion": {"concat": ["near", "far", "vector"], "in": 256, "out": 128, "activation": "relu"},
        "gru": {"input_size": 128, "hidden_size": hidden, "layers": 1,
                "gate_order": ["reset", "update", "new"], "initial_hidden": "zeros"},
        "heads": {name: {"classes": classes, "values": [-1, 0, 1] if classes == 3 else [0, 1]}
                  for name, classes in zip(data.HEADS, data.CLASSES)},
    }
    header = {"format": "gothic-nav-bc", "version": 1, "byte_order": "little", "data_nbytes": offset,
              "offset_origin": "first byte after JSON header newline", "architecture": architecture,
              "preprocessing": data.PREPROCESSING, "tensors": tensors}
    with path.open("wb") as stream:
        stream.write(json.dumps(header, separators=(",", ":")).encode() + b"\n")
        for values in payload:
            stream.write(values)


def synthetic_records(steps: int) -> tuple[np.ndarray, Iterator[np.ndarray]]:
    layout = np.zeros((demos.RAY_COUNT, 3), dtype=np.float32)
    layout[:, 0] = np.linspace(-179, 179, demos.RAY_COUNT)
    layout[:, 1] = np.tile(np.arange(-3, 4), 34) * 10
    layout[:, 2] = np.where(np.abs(layout[:, 0]) < 45, 10000, 3000)

    def records() -> Iterator[np.ndarray]:
        rng = np.random.default_rng(1731)
        base = np.zeros(1, dtype=demos.RECORD_DTYPE)
        for name in ("memory_near", "memory_far"):
            memory = base[name]
            # A varied small tile keeps the generated contract compact even
            # though its payload contains 1000 full-size raw sensor records.
            z, x = np.indices(memory.shape[1:]) % 8
            memory["channels"] = rng.integers(0, 256, (8, 8, 7), dtype=np.uint8)[z, x]
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
            for name in ("next_direction", "goal_direction"):
                direction = rng.normal(size=3)
                record["roads"][name] = direction / np.linalg.norm(direction)
            for name in ("next_distance_cm", "goal_distance_cm", "remaining_visual_cm"):
                record["roads"][name] = rng.uniform(0, 75000)
            record["roads"]["goal_in_known"] = step % 2
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


def recorded_steps(source: Path, steps: int) -> tuple[Path, np.ndarray, Iterator[np.ndarray]]:
    candidates = demos.episode_paths(source)
    # Metadata permits selection without decompressing other recordings.
    for samples in candidates:
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
    raise ValueError(f"no episode with {steps} consecutive recorded steps in {source}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--weights", type=Path)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--steps", type=int, default=1000)
    args = parser.parse_args()
    if not args.output.is_absolute() or args.steps < 1:
        parser.error("--output must be absolute and --steps must be positive")
    args.output.mkdir(parents=True, exist_ok=True)
    weights = args.weights or args.output / "fixture.navbc"
    if args.weights is None:
        synthetic_weights(weights)
    policy = NumpyPolicy(weights)
    if (policy.vector_size != data.VECTOR_SIZE or
            policy.header.get("preprocessing", {}).get("vector_order") != data.PREPROCESSING["vector_order"]):
        raise ValueError("weights must declare sensor-only vector_order: eye, roads, body")
    if args.source:
        source, layout, records = recorded_steps(args.source, args.steps)
        source_name = str(source)
    else:
        layout, records = synthetic_records(args.steps)
        source_name = "deterministic synthetic NAVDEMO records"
    header = {"format": "navpolicy-parity", "version": 1, "steps": args.steps,
              "record_bytes": demos.RECORD_BYTES, "vector_size": data.VECTOR_SIZE,
              "hidden_size": policy.hidden_size, "heads": list(data.HEADS), "classes": list(data.CLASSES),
              "eye_layout": layout.tolist(), "source": source_name, "carry_hidden": True}
    destination = args.output / "parity.bin.gz"
    hidden = None
    count = 0
    with gzip.open(destination, "wb", compresslevel=1) as stream:
        stream.write(json.dumps(header, separators=(",", ":")).encode() + b"\n")
        for record in records:
            features = data.encode_features(demos.learner_input(record), layout)
            logits, hidden = policy.step(features["near"][0], features["far"][0], features["vector"][0], hidden)
            stream.write(record.tobytes())
            for name in ("near", "far", "vector"):
                stream.write(features[name].astype("<f4", copy=False).tobytes())
            for name in data.HEADS:
                stream.write(logits[name].astype("<f4", copy=False).tobytes())
            stream.write(hidden.astype("<f4", copy=False).tobytes())
            count += 1
    if count != args.steps:
        raise ValueError(f"expected {args.steps} steps, wrote {count}")
    print(json.dumps({"weights": str(weights), "fixture": str(destination), "steps": count,
                      "source": source_name, "hidden_size": policy.hidden_size, "carry_hidden": True}))


if __name__ == "__main__":
    main()
