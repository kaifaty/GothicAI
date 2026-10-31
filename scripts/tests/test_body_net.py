"""Exported P30 weights and recurrent replay agree with the Torch actor."""
from __future__ import annotations

from copy import deepcopy
import gzip
import json
from pathlib import Path
from types import SimpleNamespace
import sys

import numpy as np
import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "body-service" / "src"))

import body_net
from body_net_numpy import BodyNet, NumpyRuntime
from body_policy_net_check import RuntimeReference
from body_trajectory_dataset import actor_features, canonical_feature_names
from test_body_fast_features import normalizer, row


@pytest.fixture
def artifact(tmp_path, monkeypatch):
    # These small engine policies are permitted on the Mac; service guards stay intact.
    from gothic_body import schema
    monkeypatch.setattr(schema, "model_host_guard", lambda: None)
    import torch
    from gothic_body import checkpoint
    from gothic_body.models import ActorConfig, create_actor
    from gothic_body.runtime import PolicyRuntime

    monkeypatch.setattr(checkpoint, "model_host_guard", lambda: None)
    torch.set_num_threads(1)
    torch.manual_seed(30)
    model = create_actor(ActorConfig("gru", 184, encoder_size=16, head_size=12)).eval()
    norm = normalizer()
    raw = actor_features(row(), sequence_dt_game_ms=50)
    norm["std"] = {name: scale * max(1, abs(raw[name])) for name, scale in norm["std"].items()}
    norm.update(source_split="train", episode_ids=["train-one"])
    path = tmp_path / "policy.pt"
    saved = checkpoint.save_checkpoint(
        path, model, normalizer=norm,
        provenance={"train_episode_ids": ["train-one"], "normalization_sha256": schema.json_sha256(norm),
                    "model_sampling": {"kind": "periodic_only", "version": 1}},
        training={"kind": "test"})

    class Backend:
        normalizer = norm
        metadata = saved["metadata"]

        def step(self, features, mask, state):
            with torch.inference_mode():
                logits, hidden = model.step(torch.tensor([features], dtype=torch.float32),
                                            torch.tensor([mask], dtype=torch.bool), state)
            return logits[0].tolist(), hidden

    def runtime():
        return PolicyRuntime(backend=Backend(), checkpoint_sha256=saved["checkpoint_sha256"])

    return SimpleNamespace(path=path, model=model, normalizer=norm, saved=saved, runtime=runtime)


def observation(index, *, epoch=7, generation=3, game_ms=None):
    value = row(session_epoch=epoch, observation_seq=41 + index * 7,
                t_game_ms=16850 + index * 50 if game_ms is None else game_ms,
                sample_period_ms=50, control_generation=generation)
    value["self"]["hp_fraction"] = 0.8 - index * 0.02
    return value


def write_episode(path, epoch):
    path.mkdir()
    initial = {"primary_entity_id": 11, "session_epoch": epoch, "world_epoch": 9, "game_ms": 16800}
    (path / "episode.json").write_text(json.dumps({"initial": initial}), encoding="utf-8")
    rows = []
    for index in range(12):
        value = observation(index, epoch=epoch, generation=3 + (index >= 9),
                            game_ms=16850 + index * 50 + (100 if index >= 6 else 0))
        if index in (1, 3):
            mask = [False] * 12 if index == 3 else [True] + [False] * 11
            value.update(can_decide=False, native_decision_eligible=False,
                         action_mask_valid=False, action_mask=mask, availability_mask=mask)
        elif index == 2:
            mask = [item in (0, 4, 9) for item in range(12)]
            value.update(action_mask=mask, availability_mask=mask)
        elif index == 4:
            value["target"] = {"present": True, "visible": False, "current_valid": False,
                               "last_known_age_ms": 400}
        rows.append(value)
    observations = path / "observations.jsonl"
    observations.write_text("".join(json.dumps(value) + "\n" for value in rows), encoding="utf-8")
    return observations, rows


def hidden(runtime, value):
    identity = tuple(value[key] for key in ("session_epoch", "world_epoch", "entity_id", "control_generation"))
    state = runtime.states.get(identity)
    if state is None or state.hidden is None:
        return np.zeros(256, dtype=np.float32)
    result = state.hidden
    if hasattr(result, "detach"):
        result = result.detach().numpy()
    return np.asarray(result).reshape(256)


def test_export_preserves_actor_and_normalizer_tensors(artifact, tmp_path):
    output = tmp_path / "policy.bodynet"
    header = body_net.export_weights(artifact.path, output)
    network = BodyNet(output)
    assert header == network.header
    assert header["format"] == "gothic-body-net" and header["version"] == 1
    assert header["checkpoint_sha256"] == artifact.saved["checkpoint_sha256"]
    assert header["feature_names"] == canonical_feature_names()
    assert len(header["feature_names"]) == 184 and len(header["actions"]) == 12
    tensors = {name: value.detach().numpy() for name, value in artifact.model.state_dict().items()}
    for name in ("mean", "std"):
        tensors["normalizer." + name] = np.asarray(
            [artifact.normalizer[name][feature] for feature in header["feature_names"]], dtype=np.float32)
    assert network.tensors.keys() == tensors.keys()
    offset = 0
    for spec in header["tensors"]:
        expected = tensors[spec["name"]]
        assert spec["dtype"] == "float32" and spec["shape"] == list(expected.shape)
        assert spec["offset"] == offset
        offset += expected.nbytes
        np.testing.assert_array_equal(network.tensors[spec["name"]], expected)
    assert header["data_nbytes"] == offset


def test_cli_golden_replays_two_episodes_with_numpy_parity(artifact, tmp_path):
    episodes = [write_episode(tmp_path / f"episode-{index}", 7 + index) for index in range(2)]
    output, golden = tmp_path / "policy.bodynet", tmp_path / "golden.jsonl.gz"
    assert body_net.main(["export", str(artifact.path), str(output)]) == 0
    assert body_net.main(["golden", str(artifact.path), *(str(path) for path, _ in episodes), str(golden)]) == 0
    assert body_net.main(["check", str(output), str(golden)]) == 0
    with gzip.open(golden, "rt", encoding="utf-8") as stream:
        records = [json.loads(line) for line in stream]
    runtime = NumpyRuntime(BodyNet(output))
    reference = RuntimeReference(runtime)
    steps, decisions, errors = 0, 0, []
    source_rows = [value for _, rows in episodes for value in rows]
    for record in records:
        kind = record["kind"]
        if kind == "reset":
            reference.handle({"id": 1, "cmd": "reset", "session_epoch": record["session_epoch"],
                              "world_epoch": record["world_epoch"], "initial_game_ms": record["initial_game_ms"]})
        elif kind == "end_episode":
            reference.handle({"id": 1, "cmd": kind, "session_epoch": record["session_epoch"],
                              "world_epoch": record["world_epoch"]})
        else:
            assert kind == "step" and record["observation"] == source_rows[steps]
            value = record["observation"]
            response = reference.handle({"id": 1, "cmd": "step", "observation": value,
                                         "now_game_ms": record["now_game_ms"]})
            processed = response["process"]
            for name in ("status", "reason", "action_id"):
                assert response[name] == record[name]
            for name in ("model_processed", "hidden_reset"):
                assert processed[name] == record[name]
            if record["hidden_reset"]:
                assert record["reset_reason"] in ("new_identity", "unexpected_periodic_gap")
            else:
                assert record["reset_reason"] is None
            np.testing.assert_allclose(hidden(runtime, value), record["hidden_state"], rtol=0, atol=1e-5)
            assert len(record["hidden_state"]) == 256
            if processed["model_processed"]:
                assert len(record["features"]) == 184 and len(record["logits"]) == 12
                np.testing.assert_allclose(runtime.last_features, record["features"], rtol=1e-6, atol=1e-5)
                np.testing.assert_allclose(processed["logits"], record["logits"], rtol=0, atol=1e-5)
            else:
                assert record["features"] is None and record["logits"] is None
                errors.append(record["reason"])
            steps += 1
            decisions += record["action_id"] is not None
    assert steps == 24 and decisions > 0
    assert errors == ["unexpected_periodic_gap"] * 2
    assert sum(record["kind"] == "reset" for record in records) == 2
    # The check command must reject changed decisions as well as pass parity.
    next(record for record in records if record["kind"] == "step")["action_id"] = -1
    with gzip.open(golden, "wt", encoding="utf-8") as stream:
        for record in records:
            stream.write(json.dumps(record) + "\n")
    assert body_net.main(["check", str(output), str(golden)]) == 1


def test_identity_and_sequence_gap_reset_like_policy_runtime(artifact, tmp_path):
    output = tmp_path / "policy.bodynet"
    body_net.export_weights(artifact.path, output)
    torch_runtime, numpy_runtime = artifact.runtime(), NumpyRuntime(BodyNet(output))
    for runtime in (torch_runtime, numpy_runtime):
        runtime.begin_episode(7, 9, initial_game_ms=16800)
    for index, sequence in enumerate((1, 2, 4, 5, 1, 2)):
        value = observation(index, generation=3 + (index >= 4))
        request = {"stream_seq": sequence, "observation": value, "now_game_ms": value["t_game_ms"],
                   "command_enabled": True, "memory_only": False}
        expected, actual = torch_runtime.process(deepcopy(request)), numpy_runtime.process(deepcopy(request))
        for name in ("status", "reason", "action_id", "hidden_reset", "model_processed"):
            assert actual[name] == expected[name]
        np.testing.assert_allclose(hidden(numpy_runtime, value), hidden(torch_runtime, value), rtol=0, atol=1e-5)
        if expected["model_processed"]:
            np.testing.assert_allclose(actual["logits"], expected["logits"], rtol=0, atol=1e-5)
            left = torch_runtime.arbitrate(value, stream_seq=sequence)
            right = numpy_runtime.arbitrate(value, stream_seq=sequence)
            assert left["action_id"] == right["action_id"]
        if index == 2:
            assert expected["reason"] == "unexpected_gap" and expected["hidden_reset"]
            assert not np.any(hidden(torch_runtime, value))
        if index == 4:
            assert expected["hidden_reset"] and expected["model_processed"]


@pytest.mark.parametrize("damage,message", [
    ("truncated", "payload|trunc|data_nbytes"),
    ("format", "format"),
    ("data_nbytes", "payload|data_nbytes"),
])
def test_corrupt_export_is_rejected(artifact, tmp_path, damage, message):
    path = tmp_path / "policy.bodynet"
    body_net.export_weights(artifact.path, path)
    header_line, payload = path.read_bytes().split(b"\n", 1)
    header = json.loads(header_line)
    if damage == "truncated":
        payload = payload[:-4]
    elif damage == "format":
        header["format"] = "wrong-body-net"
    else:
        header["data_nbytes"] += 4
    path.write_bytes(json.dumps(header).encode() + b"\n" + payload)
    with pytest.raises(ValueError, match=message):
        BodyNet(path)
