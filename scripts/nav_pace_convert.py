#!/usr/bin/env python3
"""Convert an HD navigation policy (.navbc v3) into the pace-input format v5.

    python3 -I scripts/nav_pace_convert.py --source /abs/hd-run --output /abs/pace-from-hd

The source directory holds the HD training checkpoint ``last.pt`` and the
``policy.navbc`` exported from it. Every tensor is copied; the vector encoder
gains one zero column for the pace input (walk=1, run=0) and the walk head is
dropped. The output directory receives ``last.pt`` (training initialization,
auxiliary heads included, no optimizer) and ``policy.navbc``. Before anything is
written, NumPy checks on random inputs, with either pace, that the converted
policy gives the HD distributions of every remaining head.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from collections.abc import Mapping
from pathlib import Path
from typing import Any

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nav_bc_data import PACE_HEADS, PACE_VECTOR_SIZE, VECTOR_SIZE, pace_preprocessing
from nav_bc_numpy import NumpyPolicy


def pace_tensors(state: Mapping[str, np.ndarray]) -> dict[str, np.ndarray]:
    """Copy HD tensors; add a zero pace column and drop the walk head."""
    weight = np.asarray(state["vector_encoder.0.weight"])
    if weight.shape != (128, VECTOR_SIZE) or "head_walk.weight" not in state:
        raise ValueError("pace conversion requires an HD policy with a walk head and the sensor vector")
    result = {name: np.array(values, copy=True) for name, values in state.items()
              if not name.startswith("head_walk.")}
    result["vector_encoder.0.weight"] = np.concatenate(
        (weight, np.zeros((weight.shape[0], 1), dtype=weight.dtype)), axis=1)
    return result


def equivalence(hd: NumpyPolicy, pace: NumpyPolicy, steps: int = 64, seed: int = 20261103) -> dict[str, Any]:
    """Largest HD/v5 difference of each shared head over random recurrent inputs."""
    if hd.format_version != 3 or not pace.pace_input or hd.hidden_size != pace.hidden_size:
        raise ValueError("equivalence compares HD v3 weights with their pace-input conversion")
    rng = np.random.default_rng(seed)
    errors = {name: dict(logits=0., probabilities=0.) for name in PACE_HEADS}
    for walk in (False, True):
        hidden_hd = hidden_pace = None
        for _ in range(steps):
            near = rng.uniform(-2, 2, hd.near_shape).astype(np.float32)
            far = rng.uniform(-2, 2, hd.far_shape).astype(np.float32)
            vector = rng.normal(0, 1, hd.vector_size).astype(np.float32)
            logits_hd, hidden_hd = hd.step(near, far, vector, hidden_hd)
            logits_pace, hidden_pace = pace.step(near, far, np.append(vector, np.float32(walk)), hidden_pace)
            probabilities_hd, probabilities_pace = hd.probabilities(logits_hd), pace.probabilities(logits_pace)
            for name in PACE_HEADS:
                errors[name]["logits"] = max(errors[name]["logits"],
                                             float(np.abs(logits_hd[name] - logits_pace[name]).max()))
                errors[name]["probabilities"] = max(errors[name]["probabilities"],
                    float(np.abs(probabilities_hd[name] - probabilities_pace[name]).max()))
    return dict(steps_per_pace=steps, seed=seed, max_errors=errors,
                max_probability_error=max(value["probabilities"] for value in errors.values()))


def convert(source: Path, output: Path, steps: int = 64, tolerance: float = 1e-5) -> dict[str, Any]:
    import torch
    from nav_bc_model import NavPolicy, export_weights

    hd = NumpyPolicy(source / "policy.navbc")
    if hd.format_version != 3:
        raise ValueError("pace conversion requires .navbc v3 (HD) weights")
    checkpoint = torch.load(source / "last.pt", map_location="cpu", weights_only=False)
    config = checkpoint["config"]
    if (config.get("format_version") != 3 or config.get("vector_size") != VECTOR_SIZE
            or config.get("hidden_size") != hd.hidden_size):
        raise ValueError("source checkpoint is not the HD policy of this directory")
    state = {name: value.detach().cpu().numpy() for name, value in checkpoint["model"].items()}
    # The pair must be one policy: exported tensors equal the checkpoint bit for bit.
    if set(hd.weights) != {name for name in state if not name.startswith("aux_")} or any(
            not np.array_equal(hd.weights[name], state[name]) for name in hd.weights):
        raise ValueError("source policy.navbc differs from last.pt")
    preprocessing = pace_preprocessing(hd.header["preprocessing"])
    model = NavPolicy(PACE_VECTOR_SIZE, hd.hidden_size, format_version=5,
                      auxiliary=bool(config.get("auxiliary"))).eval()
    model.load_state_dict({name: torch.from_numpy(values) for name, values in pace_tensors(state).items()})
    temperatures = hd.header.get("temperatures")
    if temperatures is not None:
        temperatures = {name: temperatures[name] for name in PACE_HEADS}
    output.mkdir(parents=True, exist_ok=True)
    temporary = output / "policy.navbc.tmp"
    export_weights(model, temporary, preprocessing=preprocessing, temperatures=temperatures)
    check = equivalence(hd, NumpyPolicy(temporary), steps)
    if check["max_probability_error"] > tolerance:
        temporary.unlink()
        raise ValueError(f"converted distributions differ from HD: {check['max_probability_error']}")
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    conversion = dict(source=str(source), source_policy_sha256=digest(source / "policy.navbc"),
                      source_checkpoint_sha256=digest(source / "last.pt"),
                      vector_encoder="HD columns copied; pace column zero", dropped=["head_walk.weight", "head_walk.bias"])
    torch.save(dict(config=dict(config, vector_size=PACE_VECTOR_SIZE, format_version=5),
                    model=model.state_dict(), history=[], preprocessing=preprocessing,
                    navigation_memory_version=preprocessing.get("navigation_memory_version", 3),
                    conversion=conversion), output / "last.pt")
    temporary.replace(output / "policy.navbc")
    result = dict(conversion, output=str(output), policy_sha256=digest(output / "policy.navbc"),
                  equivalence=check, tolerance=tolerance)
    (output / "conversion.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=Path, required=True, help="HD run directory with last.pt and policy.navbc")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--steps", type=int, default=64, help="random recurrent steps per pace value")
    args = parser.parse_args()
    if not args.source.is_absolute() or not args.output.is_absolute() or args.steps < 1:
        parser.error("--source and --output must be absolute, --steps positive")
    if args.output.resolve() == args.source.resolve():
        parser.error("--output must differ from --source")
    print(json.dumps(convert(args.source, args.output, args.steps), indent=2))


if __name__ == "__main__":
    main()
