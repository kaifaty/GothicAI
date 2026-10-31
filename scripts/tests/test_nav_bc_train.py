"""Action-change evaluation contracts; run on Ryzen with the other BC tests."""

from pathlib import Path
import json
import lzma
import random
import sys
from types import SimpleNamespace

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import nav_bc_train as train


@pytest.mark.parametrize("version,input_variant,workers,prefetch", [
    (2, "hd", 4, 1), (3, "hd-familiar", 8, 2)])
def test_parallel_loader_preserves_packed_bytes_batch_order_and_training_rng(
        tmp_path, version, input_variant, workers, prefetch):
    demos = train.nav_demos
    entries = []
    layout = np.tile(np.array([0, 0, 1000], dtype=np.float32), (demos.RAY_COUNT, 1))
    for number, steps in enumerate((2, 4, 3)):
        directory = tmp_path / str(number)
        directory.mkdir()
        (directory / "episode.json").write_text(json.dumps({"eye_layout": layout.tolist()}))
        records = np.zeros(steps, dtype=demos.RECORD_DTYPES[version])
        records["time_ms"] = np.arange(steps) * demos.PERIOD_MS
        records["eye"]["distance_cm"] = 1000
        records["roads"]["carrot_direction"][:, 2] = 1
        records["keys"]["forward"] = number - 1
        records["keys"]["turn"] = np.arange(steps) % 3 - 1
        if version == 3:
            records["memory_near"]["channels"][:, 3, 4, 7] = 128 + number
            records["memory_far"]["channels"][:, 60, 70, 7] = 255 - number
        samples = directory / "samples.bin.xz"
        with lzma.open(samples, "wb") as stream:
            stream.write(demos.HEADER.pack(demos.MAGIC, version, demos.RECORD_BYTES_BY_VERSION[version],
                demos.RAY_COUNT, demos.NEAR_SIZE, demos.FAR_SIZE, demos.PERIOD_MS))
            stream.write(records.tobytes())
        entries.append(dict(id=str(number), storage="navdemo", file=samples.relative_to(tmp_path).as_posix(),
            teacher_version=None, input_variant=input_variant, steps=steps))
    expected = {entry["id"]: train.load(tmp_path, entry, packed=True) for entry in entries}
    random.seed(317)
    np.random.seed(317)
    train.torch.manual_seed(317)
    python_rng, numpy_rng = random.getstate(), np.random.get_state()
    torch_rng = train.torch.random.get_rng_state().clone()
    with train.BatchLoader(tmp_path, workers=workers, prefetch=prefetch) as loader:
        for seed in (17, 18):
            batches = train.training_batches(entries, 1, seed)
            actual_order = []
            for batch, group in loader.loaded_batches(batches, packed=True):
                actual_order.append([entry["id"] for entry in batch])
                for entry, episode in zip(batch, group):
                    reference = expected[entry["id"]]
                    assert episode.keys() == reference.keys()
                    for name, values in episode.items():
                        assert values.dtype == reference[name].dtype
                        assert values.shape == reference[name].shape
                        assert values.tobytes() == reference[name].tobytes()
            assert actual_order == [[entry["id"] for entry in batch] for batch in batches]
            waits = loader.wait_stats()
            per_batch = waits["load_wait_per_batch_seconds"]
            assert len(per_batch) == len(batches)
            assert all(value >= 0 for value in per_batch)
            assert waits["load_wait_seconds"] == sum(per_batch)
            assert waits["load_wait_first_batch_seconds"] == per_batch[0]
            assert waits["load_wait_after_first_seconds"] == sum(per_batch[1:])
            assert waits["load_wait_max_batch_seconds"] == max(per_batch)
    assert random.getstate() == python_rng
    numpy_after = np.random.get_state()
    assert numpy_after[0] == numpy_rng[0] and numpy_after[2:] == numpy_rng[2:]
    np.testing.assert_array_equal(numpy_after[1], numpy_rng[1])
    assert train.torch.equal(train.torch.random.get_rng_state(), torch_rng)


def test_training_batches_cap_dagger_and_retain_every_episode():
    teachers = [{"id": f"teacher-{i}", "source_kind": "teacher"} for i in range(3)]
    dagger = [{"id": f"dagger-{i}", "source_kind": "dagger"} for i in range(11)]
    batches = train.training_batches(teachers + dagger, 5, 17)
    assert batches == train.training_batches(teachers + dagger, 5, 17)
    assert {entry["id"] for batch in batches for entry in batch} == {
        entry["id"] for entry in teachers + dagger}
    for batch in batches:
        assert len(batch) <= 5
        assert sum(entry["source_kind"] == "dagger" for entry in batch) <= len(batch) // 2


def test_training_batches_require_teacher_and_mixed_batch_capacity():
    with pytest.raises(ValueError, match="successful teacher"):
        train.training_batches([{"source_kind": "dagger"}], 8, 17)
    with pytest.raises(ValueError, match="at least two"):
        train.training_batches([{"source_kind": "teacher"}, {"source_kind": "dagger"}], 1, 17)


def test_dagger_step_cap_uses_valid_steps_when_episode_lengths_differ():
    mask = np.array([[1, 1, 0, 0], [1, 0, 0, 0], [1, 1, 1, 1], [1, 1, 1, 1]], bool)
    train.cap_dagger_steps(mask, [False, False, True, True], np.random.default_rng(17))
    assert mask[:2].sum() == 3
    assert mask[2:].sum() == 3
    assert not mask[:2, 2:].any()
    # A tail with no teacher labels cannot form a DAgger-only update.
    mask = np.array([[0, 0], [1, 1]], bool)
    train.cap_dagger_steps(mask, [False, True], np.random.default_rng(17))
    assert not mask.any()


def test_index_rejects_v1_semantics_even_with_same_vector_dimensions(tmp_path):
    index = {"cache_version": train.CACHE_VERSION, "vector_size": train.VECTOR_SIZE,
             "preprocessing": {**train.PREPROCESSING, "roads_schema": "next_node_v1"},
             "episodes": []}
    (tmp_path / "index.json").write_text(json.dumps(index))
    with pytest.raises(ValueError, match="NAVDEMO v2"):
        train.read_index(tmp_path)


@pytest.mark.parametrize("mask, expected_count, expected_accuracy", [
    ([1, 0, 1], 2, .5),
    ([0, 0, 0], 0, None),
])
def test_evaluate_action_changes_uses_mask_and_handles_no_changes(monkeypatch, mask,
                                                                expected_count, expected_accuracy):
    truth = np.tile(np.array([2, 1, 1, 1, 0, 0], dtype=np.int64), (3, 1))
    prediction = truth.copy()
    prediction[0, :3] = [1, 0, 0]
    prediction[1, 3:] = [0, 1, 1]
    episode = {"labels": truth, "baseline": truth, "situations": np.zeros((3, 3), np.uint8),
               "action_change": np.array(mask, np.uint8)}
    monkeypatch.setattr(train, "load", lambda *_: episode)
    monkeypatch.setattr(train, "predict_episode", lambda *_: prediction)
    result = train.evaluate(SimpleNamespace(eval=lambda: None), None, [{"region": "farm"}], "cpu")
    changed = result["action_change"]
    assert changed["steps"] == expected_count
    assert changed["all_heads_accuracy"] == expected_accuracy
    for head in train.HEADS:
        assert sum(changed["model"][head]["support"]) == expected_count
        if expected_count:
            expected = .5 if head in ("forward", "strafe", "turn") else 1.
            assert changed["model"][head]["accuracy"] == expected
        else:
            assert changed["model"][head]["accuracy"] == 0


def test_bc3_loss_weights_action_changes_and_maneuver_windows():
    import torch
    logits = {head: torch.zeros(1, 2, classes, requires_grad=True)
              for head, classes in zip(train.BC3_HEADS, train.BC3_CLASSES)}
    labels = torch.zeros(1, 2, 5, dtype=torch.long)
    mask = torch.ones(1, 2, dtype=torch.bool)
    auxiliary = dict(stuck=torch.zeros(1, 2, requires_grad=True),
                     next_near=torch.zeros(1, 2, 9, 32, 32, requires_grad=True),
                     detour=torch.zeros(1, 2, 4, requires_grad=True))
    targets = dict(action_change=torch.tensor([[0., 1.]]),
                   maneuver_weight=torch.tensor([[1., 5.]]),
                   stuck=torch.zeros(1, 2), stuck_valid=torch.zeros(1, 2),
                   next_near=torch.zeros(1, 2, 9, 32, 32), next_near_valid=torch.zeros(1, 2),
                   detour=torch.zeros(1, 2, 4), detour_valid=torch.zeros(1, 2))
    loss, terms = train.bc3_loss(logits, auxiliary, labels, mask, targets,
                                [torch.ones(c) for c in train.BC3_CLASSES], 10.)
    expected = np.log(3) * (1 - 1/3)**2 * (1 + 10 * 5) / 11
    assert terms["maneuver"].item() == pytest.approx(expected)
    loss.backward()
    for head in train.BC3_HEADS:
        gradient = logits[head].grad
        ratio = gradient[0, 1].abs().sum() / gradient[0, 0].abs().sum()
        assert ratio.item() == pytest.approx(50 if head == "maneuver" else 10)
    assert all(terms[name].item() == 0 for name in auxiliary)


def test_bc3_next_frame_targets_cross_tbptt_boundary_without_episode_leak():
    episode = dict(labels=np.zeros((3, 5), np.int64),
                   near=np.stack([np.full((9, 32, 32), i, np.float32) for i in range(3)]))
    for name in ("action_change", "maneuver_weight", "stuck", "stuck_valid",
                 "next_near_valid", "detour_valid"):
        episode[name] = np.ones(3, np.float32)
    episode["next_near_valid"][-1] = 0
    episode["detour"] = np.zeros((3, 4), np.float32)
    targets = train.bc3_targets([episode], 1, 3, "cpu")
    np.testing.assert_array_equal(targets["next_near"][0, 0].numpy(), 2)
    np.testing.assert_array_equal(targets["next_near"][0, 1:].numpy(), 0)
    np.testing.assert_array_equal(targets["next_near_valid"].numpy(), [[1, 0, 0]])


def test_bc3_initialization_preserves_actor_and_conditions_exclusive_maneuvers(tmp_path):
    import torch
    from nav_bc_model import NavPolicy, export_weights
    legacy = NavPolicy(train.VECTOR_SIZE, 8)
    current = NavPolicy(train.VECTOR_SIZE, 8, format_version=2, auxiliary=True)
    index = dict(preprocessing=train.PREPROCESSING, eye_layout=[[0, 0, 1]])
    path = tmp_path / "initial.navbc"
    export_weights(legacy, path, preprocessing={**index["preprocessing"], "eye_layout": index["eye_layout"]})
    train.initialize_bc3(current, path, index)
    torch.testing.assert_close(current.gru.weight_ih_l0, legacy.gru.weight_ih_l0)
    torch.testing.assert_close(current.head_forward.weight, legacy.head_forward.weight)
    jump, climb = legacy.head_jump, legacy.head_climb
    torch.testing.assert_close(current.head_maneuver.weight,
                               torch.stack((jump.weight[0] + climb.weight[0],
                                            jump.weight[1] + climb.weight[0],
                                            jump.weight[0] + climb.weight[1])))
    with pytest.raises(ValueError, match="eye layout"):
        train.initialize_bc3(current, path, {**index, "eye_layout": [[1, 0, 1]]})
