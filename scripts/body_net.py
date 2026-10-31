#!/usr/bin/env python3
"""Export P30 GRU weights and replay Torch/NumPy decision parity offline.

Export/golden need Torch; check/fixture need only NumPy and the standard library.
Recorded observations keep the reset metadata used by body_policy_net_check
beside each observations.jsonl. No game or running policy service is needed.
"""
from __future__ import annotations

import argparse
from copy import deepcopy
import gzip
import hashlib
import json
from pathlib import Path
import sys

import numpy as np

# Isolated Python (-I) omits the script directory from module search paths.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from body_policy_net_check import RuntimeReference, replay_requests
from body_trajectory_dataset import canonical_feature_names, strict_json


def _load_checkpoint(checkpoint: Path):
    package = Path(__file__).resolve().parents[1] / "body-service" / "src"
    if str(package) not in sys.path:
        sys.path.insert(0, str(package))
    from gothic_body.checkpoint import load_checkpoint
    from gothic_body.schema import ACTIONS, FEATURE_VERSION

    loaded = load_checkpoint(checkpoint, device="cpu")
    config = loaded["metadata"]["model_config"]
    names = canonical_feature_names(FEATURE_VERSION)
    if (config["architecture"] != "gru" or config["hidden_size"] != 256 or len(names) != 184
            or loaded["metadata"]["feature_names"] != names
            or loaded["metadata"]["actions"] != list(ACTIONS)):
        raise ValueError("bodynet v1 requires the P30 GRU-256 actor with 184 inputs and 12 actions")
    return loaded


def export_weights(checkpoint: Path, output: Path) -> dict:
    """Write a JSON header line, then contiguous little-endian float32 tensors.

    Offsets are bytes from the first byte after the header newline. Linear
    weights use PyTorch's output/input layout; GRUCell gates are reset/update/new.
    The checkpoint SHA is verified against its publication sidecar before export.
    """
    checkpoint, output = Path(checkpoint), Path(output)
    if output.resolve() in (checkpoint.resolve(), checkpoint.with_suffix(checkpoint.suffix + ".json").resolve()):
        raise ValueError("export output must differ from the checkpoint and its sidecar")
    loaded = _load_checkpoint(checkpoint)
    names = loaded["metadata"]["feature_names"]
    arrays = {name: tensor.detach().cpu().numpy() for name, tensor in loaded["model"].state_dict().items()}
    arrays.update({f"normalizer.{statistic}": [loaded["normalizer"][statistic][name] for name in names]
                   for statistic in ("mean", "std")})
    tensors, payload, offset = [], [], 0
    for name, values in arrays.items():
        array = np.asarray(values, dtype="<f4")
        if not np.isfinite(array).all() or name == "normalizer.std" and not (array > 0).all():
            raise ValueError(f"tensor {name} cannot be represented as finite float32 with positive scales")
        data = array.tobytes(order="C")
        tensors.append({"name": name, "shape": list(array.shape), "dtype": "float32",
                        "offset": offset, "nbytes": len(data)})
        payload.append(data)
        offset += len(data)
    header = {"format": "gothic-body-net", "version": 1, "byte_order": "little",
              "offset_origin": "first byte after JSON header newline", "data_nbytes": offset,
              "checkpoint_sha256": loaded["checkpoint_sha256"], "actions": loaded["metadata"]["actions"],
              "feature_names": names, "feature_version": loaded["normalizer"]["feature_version"],
              "tensors": tensors}
    with output.open("wb") as stream:
        stream.write(json.dumps(header, separators=(",", ":"), allow_nan=False).encode("utf-8") + b"\n")
        for data in payload:
            stream.write(data)
    return header


def torch_runtime(checkpoint: Path):
    """Use the real PolicyRuntime with a local CPU backend for the small actor.

    The service backend has a Linux-only startup gate. This offline adapter
    leaves that service and PolicyRuntime unchanged; checkpoint guards still
    apply (the Mac tests explicitly monkeypatch model_host_guard).
    """
    loaded = _load_checkpoint(Path(checkpoint))
    import torch
    from gothic_body.runtime import PolicyRuntime, configure_inference_cpu_threads

    if loaded["metadata"].get("training", {}).get("deployment_candidate") is False:
        raise ValueError("train-only diagnostic checkpoint cannot control a live body")
    configure_inference_cpu_threads(torch)

    class Backend:
        model, metadata, normalizer = loaded["model"], loaded["metadata"], loaded["normalizer"]

        def step(self, features, mask, state):
            with torch.inference_mode():
                logits, hidden = self.model.step(torch.tensor([features], dtype=torch.float32),
                                                 torch.tensor([mask], dtype=torch.bool), state=state)
            return logits[0].tolist(), hidden

    return PolicyRuntime(backend=Backend(), checkpoint_sha256=loaded["checkpoint_sha256"])


def step_record(runtime, request: dict, response: dict) -> dict:
    """Stable golden fields: absent inference is null, cleared memory is zeros."""
    row, processed = request["observation"], response["process"]
    identity = tuple(row[key] for key in ("session_epoch", "world_epoch", "entity_id", "control_generation"))
    state = runtime.states.get(identity)
    hidden = state.hidden if state is not None else None
    inferred = processed["model_processed"]
    return {"kind": "step", "observation": row, "stream_seq": processed["stream_seq"],
            "now_game_ms": request["now_game_ms"],
            **{key: response[key] for key in ("status", "action_id", "reason")},
            **{key: processed.get(key) for key in ("hidden_reset", "model_processed", "selected_action_id",
                                                  "sequence_dt_game_ms")},
            "reset_reason": (processed["reason"] if processed["status"] == "fallback" else "new_identity")
                            if processed["hidden_reset"] else None,
            "features": np.asarray(runtime.last_features, dtype=np.float32).tolist() if inferred else None,
            "logits": processed.get("logits") if inferred else None,
            "hidden_state": (np.zeros(256, dtype=np.float32) if hidden is None else
                             np.asarray(hidden, dtype=np.float32).reshape(256)).tolist()}


def step_response(runtime, driver, request: dict) -> dict:
    """Replay the fixture's explicit non-memory-only locked observation."""
    if "process_options" not in request:
        return driver.handle(request)
    row = request["observation"]
    identity = tuple(row[key] for key in ("session_epoch", "world_epoch", "entity_id", "control_generation"))
    previous = driver.streams.get(row["entity_id"])
    sequence = previous[1] + 1 if previous is not None and previous[0] == identity else 1
    processed = runtime.process({"observation": row, "stream_seq": sequence,
                                 "now_game_ms": request["now_game_ms"], **request["process_options"]})
    if runtime.active.get(row["entity_id"]) == identity:
        driver.streams[row["entity_id"]] = (identity, sequence)
    if processed["status"] == "fallback":
        result = processed
    elif row.get("can_decide") is True and row.get("action_mask_valid") is True:
        result = runtime.arbitrate(row, stream_seq=sequence)
    else:
        result = {"status": "locked", "action_id": None, "reason": processed["reason"]}
    return {"process": processed, **{key: result[key] for key in ("status", "action_id", "reason")}}


def arbitration_record(runtime, observation: dict, stream_seq: int, max_anchor_age_game_ms: int = 67) -> dict:
    """Golden arbitration output and memory, including rejected anchor queries."""
    identity = tuple(observation[key] for key in ("session_epoch", "world_epoch", "entity_id", "control_generation"))
    state = runtime.states.get(identity)
    hidden = state.hidden if state is not None else None
    return {"kind": "arbitrate", "observation": observation, "stream_seq": stream_seq,
            "max_anchor_age_game_ms": max_anchor_age_game_ms,
            **runtime.arbitrate(observation, stream_seq=stream_seq, max_anchor_age_game_ms=max_anchor_age_game_ms),
            "hidden_state": (np.zeros(256, dtype=np.float32) if hidden is None else
                             np.asarray(hidden, dtype=np.float32).reshape(256)).tolist()}


def golden(checkpoint: Path, observations: list[Path], output: Path) -> dict:
    output = Path(output)
    sources = {Path(checkpoint).resolve(), Path(checkpoint).with_suffix(Path(checkpoint).suffix + ".json").resolve()}
    for path in observations:
        sources.update((Path(path).parent / name).resolve() for name in
                       (Path(path).name, "arena-events.jsonl", "episode.json", "trajectory-manifest.json"))
    if output.resolve() in sources:
        raise ValueError("golden output must differ from checkpoint and episode inputs")
    runtime = torch_runtime(checkpoint)
    driver = RuntimeReference(runtime)
    summary = {"episodes": 0, "steps": 0, "decisions": 0, "fallbacks": 0}
    with gzip.open(output, "wt", encoding="utf-8") as stream:
        for path in observations:
            steps = 0
            for request in replay_requests(Path(path)):
                response = driver.handle(request)
                command = request["cmd"]
                if command == "step":
                    record = step_record(runtime, request, response)
                    steps += 1
                    summary["steps"] += 1
                    summary["decisions"] += response["status"] == "decision"
                    summary["fallbacks"] += response["status"] == "fallback"
                elif command in ("reset", "end_episode"):
                    record = {"kind": command, "session_epoch": request["session_epoch"],
                              "world_epoch": request["world_epoch"], "checkpoint_sha256": runtime.checkpoint_sha256}
                    if command == "reset":
                        record["initial_game_ms"] = request["initial_game_ms"]
                else:
                    continue
                stream.write(json.dumps(record, separators=(",", ":"), allow_nan=False) + "\n")
            if steps == 0:
                raise ValueError(f"{path}: journal contains no periodic primary observations")
            summary["episodes"] += 1
    return summary


def check(bodynet: Path, expected: Path) -> dict:
    from body_net_numpy import BodyNet, NumpyRuntime

    runtime = NumpyRuntime(BodyNet(bodynet))
    driver = RuntimeReference(runtime)
    summary = {"episodes": 0, "steps": 0, "arbitrations": 0, "mismatches": 0,
               "max_logits_error": 0.0, "max_hidden_error": 0.0}
    first_mismatch = None
    in_episode = False
    with gzip.open(expected, "rt", encoding="utf-8") as stream:
        for number, line in enumerate(stream, 1):
            record = strict_json(line)
            kind = record.get("kind")
            if kind in ("reset", "end_episode"):
                if record.get("checkpoint_sha256") != runtime.checkpoint_sha256:
                    raise ValueError(f"golden line {number}: checkpoint SHA differs from bodynet")
                if kind == "reset" and in_episode or kind == "end_episode" and not in_episode:
                    raise ValueError(f"golden line {number}: invalid episode boundary")
                driver.handle({"id": number, "cmd": kind,
                               **{key: record[key] for key in ("session_epoch", "world_epoch")},
                               **({"initial_game_ms": record["initial_game_ms"]} if kind == "reset" else {})})
                in_episode = kind == "reset"
                summary["episodes"] += kind == "reset"
                continue
            if kind not in ("step", "arbitrate") or not in_episode:
                raise ValueError(f"golden line {number}: step/arbitrate inside an episode required")
            if kind == "arbitrate":
                actual = arbitration_record(runtime, record["observation"], record["stream_seq"],
                                            record["max_anchor_age_game_ms"])
                exact_keys = ("status", "action_id", "reason", "hidden_reset", "model_processed",
                              "effective_action_mask", "raw_action_mask")
                vectors = (("hidden_state", 256),)
                summary["arbitrations"] += 1
            else:
                request = {"id": number, "cmd": "step", "observation": record["observation"],
                           "now_game_ms": record["now_game_ms"]}
                if "process_options" in record:
                    request["process_options"] = record["process_options"]
                actual = step_record(runtime, request, step_response(runtime, driver, request))
                exact_keys = ("stream_seq", "status", "action_id", "reason", "hidden_reset", "reset_reason",
                              "model_processed", "selected_action_id", "sequence_dt_game_ms")
                vectors = (("features", 184), ("logits", 12), ("hidden_state", 256))
                summary["steps"] += 1
            errors = []
            for key in exact_keys:
                if (key in record) != (key in actual) or record.get(key) != actual.get(key):
                    errors.append(key)
            for key, size in vectors:
                left, right = record[key], actual[key]
                if left is None or right is None:
                    if left is not right:
                        errors.append(key)
                    continue
                left, right = np.asarray(left, dtype=np.float64), np.asarray(right, dtype=np.float64)
                if left.shape != (size,) or right.shape != (size,) or not np.isfinite(left).all():
                    raise ValueError(f"golden line {number}: invalid {key} vector")
                error = float(np.max(np.abs(left-right)))
                # Exported normalizer statistics are float32; the checkpoint's
                # JSON statistics are doubles. Allow their rounding in inputs.
                matches = np.allclose(left, right, atol=1e-5, rtol=1e-6 if key == "features" else 0)
                if not matches:
                    errors.append(key)
                if key in ("logits", "hidden_state"):
                    metric = "max_logits_error" if key == "logits" else "max_hidden_error"
                    summary[metric] = max(summary[metric], error)
            if errors:
                summary["mismatches"] += 1
                first_mismatch = first_mismatch or f"golden line {number}: mismatch in {', '.join(errors)}"
    if in_episode or not summary["steps"]:
        raise ValueError("golden stream must contain complete episodes and at least one step")
    if first_mismatch:
        summary["first_mismatch"] = first_mismatch
    return summary


def fixture(output: Path) -> dict:
    """Write deterministic actor weights and synthetic NumPy runtime episodes."""
    from body_net_numpy import BodyNet, NumpyRuntime
    from body_trajectory_dataset import ACTIONS, PERIODIC_FEATURE_VERSION

    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(30)
    names = canonical_feature_names(PERIODIC_FEATURE_VERSION)
    shapes = {"encoder.0.weight": (32, 184), "encoder.0.bias": (32,),
              "memory.weight_ih": (768, 32), "memory.weight_hh": (768, 256),
              "memory.bias_ih": (768,), "memory.bias_hh": (768,),
              "head.0.weight": (24, 256), "head.0.bias": (24,),
              "head.2.weight": (12, 24), "head.2.bias": (12,)}
    arrays = {name: rng.normal(0, 0.04, size=shape).astype("<f4") for name, shape in shapes.items()}
    arrays["normalizer.mean"] = rng.uniform(-0.2, 0.2, size=184).astype("<f4")
    scales = []
    for name in names:
        scale = 1 if name.endswith(".valid") else (250 if "cm" in name or "_ms" in name else 1)
        scales.append(scale)
    arrays["normalizer.std"] = (np.asarray(scales) * rng.uniform(0.75, 1.5, size=184)).astype("<f4")
    payload, tensors, offset = [], [], 0
    for name, array in arrays.items():
        data = array.tobytes(order="C")
        tensors.append({"name": name, "shape": list(array.shape), "dtype": "float32",
                        "offset": offset, "nbytes": len(data)})
        payload.append(data)
        offset += len(data)
    payload = b"".join(payload)
    header = {"format": "gothic-body-net", "version": 1, "byte_order": "little",
              "offset_origin": "first byte after JSON header newline", "data_nbytes": offset,
              "checkpoint_sha256": hashlib.sha256(payload).hexdigest(), "actions": list(ACTIONS),
              "feature_names": names, "feature_version": PERIODIC_FEATURE_VERSION, "tensors": tensors}
    weights = output / "fixture.bodynet"
    weights.write_bytes(json.dumps(header, separators=(",", ":"), allow_nan=False).encode() + b"\n" + payload)
    runtime = NumpyRuntime(BodyNet(weights))
    driver = RuntimeReference(runtime)
    summary = {"episodes": 4, "steps": 0, "arbitrations": 0, "decisions": 0, "memory_only": 0,
               "empty_raw_masks": 0, "hidden_resets": 0, "fallbacks": {}, "outcomes": {}}

    with gzip.open(output / "golden.jsonl.gz", "wt", encoding="utf-8") as stream:
        def write(record):
            stream.write(json.dumps(record, separators=(",", ":"), allow_nan=False) + "\n")
            if record["kind"] in ("step", "arbitrate"):
                outcome = record["status"] + "/" + record["reason"]
                summary["outcomes"][outcome] = summary["outcomes"].get(outcome, 0) + 1

        def arbitrate(observation, sequence):
            write(arbitration_record(runtime, observation, sequence))
            summary["arbitrations"] += 1

        for episode in range(summary["episodes"]):
            epochs = {"session_epoch": 7 + episode, "world_epoch": 9 + episode}
            boundary = 1000 * episode
            driver.handle({"id": 0, "cmd": "reset", **epochs, "initial_game_ms": boundary})
            write({"kind": "reset", **epochs, "initial_game_ms": boundary,
                   "checkpoint_sha256": runtime.checkpoint_sha256})
            for tick in range(160):
                for entity in (11, 22):
                    now = boundary + 50 * (tick + 1) + (100 if entity == 22 and tick >= 60 else 0)
                    generation = 3 + (tick >= 80)
                    mask = [True] + [bool((tick + action + entity) % 4) for action in range(1, 12)]
                    row = {"schema_version": 1, **epochs, "entity_id": entity,
                           "control_generation": generation, "observation_seq": tick * 7 + entity,
                           "t_game_ms": now, "dt_game_ms": 33, "sim_step": tick * 3 + 1,
                           "periodic": True, "sample_period_ms": 50,
                           "phase": "post_simulation_post_animation", "can_decide": True,
                           "native_decision_eligible": True, "action_mask_valid": True,
                           "action_mask": mask, "availability_mask": mask,
                           "mask_kind": "shared_melee_resolver",
                           "self": {"hp_fraction": 0.2 + 0.8 * ((tick + entity) % 31) / 31,
                                    "weapon_state": tick % 5, "weapon_length_cm": 90,
                                    "skill": tick % 3, "skill_talent": 2, "skill_valid": True,
                                    "dead": False, "down": False, "attack_anim": tick % 3 == 0,
                                    "prehit": tick % 7 == 0, "interruptible": tick % 5 != 0,
                                    "velocity_valid": True, "linear_velocity_cm_s": [12.5, -0.0, tick * 0.5],
                                    "gates": {"fai_wait_remaining_ms": 0, "ani_wait_remaining_ms": tick % 200}},
                           "target": {"present": True, "visible": True, "current_valid": True,
                                      "yaw_error_rad": (tick % 25 - 12) / 7,
                                      "fight_distance_cm": 80 + tick % 80,
                                      "relative_position_cm": [100 + tick * 0.5, 1.5, -2.25],
                                      "relative_velocity_cm_s": None if tick % 3 else [10, 0, -15],
                                      "hp_fraction": 0.3, "last_known_age_ms": 0,
                                      "last_seen_game_ms": now - 10},
                           "goal": {"kind": ("combat", "native_combat", "idle", "native_idle", None)[tick % 5],
                                    "move_hint": tick % 3 if tick % 7 else None},
                           "local_obstacles": {"rays": [] if tick % 13 == 0 else
                                               [{"distance_cm": 50.25 * index + tick, "hit": index % 3 == 0}
                                                for index in range(16)]},
                           "events": [{"source_game_ms": now - 10, "source_sim_step": tick * 3}],
                           "last_result": {"valid": True, "source_game_ms": now - 10,
                                           "source_sim_step": tick * 3}}
                    if tick % 23 == 5:
                        row["target"] = {"present": True, "visible": False, "current_valid": False,
                                         "last_known_age_ms": 400}
                    if tick % 17 == 4 or tick % 29 == 7:
                        mask = [tick % 29 != 7] + [False] * 11
                        row.update(can_decide=False, native_decision_eligible=False,
                                   action_mask_valid=False, action_mask=mask, availability_mask=mask)
                    # Rejections before state.sequence advances create a true stream gap
                    # on the following row when replayed by the standard golden driver.
                    if entity == 11:
                        if tick == 2:
                            row["can_decide"] = False
                        elif tick == 20:
                            row["phase"] = "pre_native_primitive"
                        elif tick == 40:
                            row["sample_period_ms"] = 25
                        elif tick == 70:
                            row["action_mask"] = [True] * 11
                        elif tick == 90:
                            row["control_generation"] = generation - 1
                        elif tick == 100:
                            row["self"]["skill"] = "invalid"
                        elif tick == 110:
                            row["world_epoch"] -= 1
                        elif tick == 120:
                            row["action_mask"] = row["availability_mask"] = [True] + [False] * 11
                    request = {"id": summary["steps"] + 1, "cmd": "step", "observation": row,
                               "now_game_ms": now + (150 if entity == 22 and tick == 50 else 0)}
                    if entity == 11 and tick == 2:
                        request["process_options"] = {"memory_only": False, "command_enabled": False}
                    response = step_response(runtime, driver, request)
                    record = step_record(runtime, request, response)
                    if "process_options" in request:
                        record["process_options"] = request["process_options"]
                    write(record)
                    summary["steps"] += 1
                    summary["decisions"] += response["status"] == "decision"
                    summary["memory_only"] += response["process"].get("memory_only") is True
                    summary["empty_raw_masks"] += not any(row["action_mask"])
                    summary["hidden_resets"] += record["hidden_reset"]
                    if response["status"] == "fallback":
                        reason = response["reason"]
                        summary["fallbacks"][reason] = summary["fallbacks"].get(reason, 0) + 1
                    if entity == 11:
                        sequence = record["stream_seq"]
                        if tick == 0:
                            arbitrate({**row, "entity_id": 999}, sequence)
                        elif tick == 1:
                            arbitrate({**row, "t_game_ms": now + 68}, sequence)
                            arbitrate({**row, "observation_seq": row["observation_seq"] + 1}, sequence)
                            invalid = deepcopy(row)
                            # Python anchor equality accepts bool/int numeric equality;
                            # arbitration still requires actual boolean mask entries.
                            invalid["action_mask"] = [int(bit) for bit in row["action_mask"]]
                            arbitrate(invalid, sequence)
                            arbitrate(row, sequence)
                        elif tick in (2, 4):
                            arbitrate(row, sequence)
            driver.handle({"id": 0, "cmd": "end_episode", **epochs})
            write({"kind": "end_episode", **epochs, "checkpoint_sha256": runtime.checkpoint_sha256})
    return summary


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    export_parser = commands.add_parser("export", help="write a P30 actor as .bodynet")
    export_parser.add_argument("checkpoint", type=Path)
    export_parser.add_argument("output", type=Path)
    golden_parser = commands.add_parser("golden", help="replay recorded episodes with Torch")
    golden_parser.add_argument("checkpoint", type=Path)
    golden_parser.add_argument("observations", type=Path, nargs="+")
    golden_parser.add_argument("output", type=Path)
    check_parser = commands.add_parser("check", help="compare NumPy decisions with golden data")
    check_parser.add_argument("bodynet", type=Path)
    check_parser.add_argument("golden", type=Path)
    fixture_parser = commands.add_parser("fixture", help="write synthetic NumPy weights and golden episodes")
    fixture_parser.add_argument("output", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "export":
            header = export_weights(args.checkpoint, args.output)
            result = {"tensors": len(header["tensors"]), "data_nbytes": header["data_nbytes"],
                      "checkpoint_sha256": header["checkpoint_sha256"]}
        elif args.command == "golden":
            result = golden(args.checkpoint, args.observations, args.output)
        elif args.command == "fixture":
            result = fixture(args.output)
        else:
            result = check(args.bodynet, args.golden)
        print(json.dumps(result, allow_nan=False))
        return int(result.get("mismatches", 0) != 0)
    except (OSError, ValueError, RuntimeError, KeyError, TypeError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
