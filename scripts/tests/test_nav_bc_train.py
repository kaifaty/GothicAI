"""Action-change evaluation contracts; run on Ryzen with the other BC tests."""

from pathlib import Path
import json
import sys
from types import SimpleNamespace

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import nav_bc_train as train


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


def test_family_balance_keeps_v4_decision_shares_and_half_teacher_control_weight(tmp_path):
    v4 = [{"family": family, "steps": count} for family, count in
          (("point", 500), ("object", 200), ("follow_below_gate", 300))]
    entries = v4 + [{"family": "point", "steps": 1000, "effective_steps": 500}]
    weights, result = train.family_balance_weights(entries, v4)
    assert weights == pytest.approx({"point": 1., "object": 2., "follow": 2.})
    assert result["effective_decisions"] == {"point": 1000, "object": 200, "follow": 300}
    assert result["minimum_shares"] == {"object": .2, "follow": .3}
    assert result["weighted_shares"]["object"] == pytest.approx(.2)
    assert result["weighted_shares"]["follow"] == pytest.approx(.3)
    # Long DAgger tails disappear when teacher episodes end. Family weights
    # must use the exact randomly retained decisions, including half weights.
    teachers = [dict(family=family, steps=steps, source_kind="teacher") for family, steps in
                (("point", 2), ("object", 1), ("follow_below_gate", 1))]
    dagger = []
    for number, values in enumerate(([.5, 1.], [1., .5], [.5, .5])):
        file = f"dagger-{number}.npz"
        np.savez_compressed(tmp_path / file, sample_weight=np.tile(values, 3).astype(np.float32))
        dagger.append(dict(family="point", steps=6, source_kind="dagger", file=file))
    selected = np.array([[1, 1], [1, 0], [1, 0], [1, 1], [1, 1], [1, 1]], bool)
    train.cap_dagger_steps(selected, [False] * 3 + [True] * 3, np.random.default_rng(17))
    expected_point = 2 + sum(np.asarray(values)[selected[row + 3]].sum()
                             for row, values in enumerate(([.5, 1.], [1., .5], [.5, .5])))
    cached_weights = {}
    weights, actual = train.epoch_family_balance([teachers + dagger], tmp_path, 2, 17,
        result["v4_decisions"], cached_weights)
    assert actual["effective_decisions"] == {"point": expected_point, "object": 1, "follow": 1}
    assert len(cached_weights) == 3
    assert actual["weighted_shares"]["object"] == pytest.approx(.2)
    assert actual["weighted_shares"]["follow"] == pytest.approx(.3)
    assert train.epoch_family_balance([teachers + dagger], tmp_path, 2, 17,
        result["v4_decisions"], cached_weights)[0] == weights


def test_aggregate_training_index_combines_hd_versions_without_opening_episode_files(tmp_path):
    caches = [tmp_path / "v4", tmp_path / "v5"]
    for number, cache in enumerate(caches):
        cache.mkdir()
        version, memory = (2, 3) if number == 0 else (3, 4)
        preprocessing = (train.HD_PREPROCESSING if memory == 3 else
                         {**train.HD_PREPROCESSING, "navigation_memory_version": 4})
        entries = [{"id": family, "pair": [family, "goal"], "split": "train", "family": family,
                    "steps": steps, "source_kind": "teacher", "file": f"episodes/{family}.npz"}
                   for family, steps in (("point", 50 * (number + 1)), ("object", 20), ("follow_below_gate", 30))]
        index = dict(cache_version=train.HD_CACHE_VERSION, navdemo_version=version,
            navigation_memory_version=memory, preprocessing=preprocessing, vector_size=train.VECTOR_SIZE,
            near_shape=list(train.HD_NEAR_SHAPE), far_shape=list(train.HD_FAR_SHAPE),
            heads=list(train.BC3_HEADS), classes=list(train.BC3_CLASSES), eye_layout=[[0, 0, 1]],
            episodes=entries, action_change={"N": sum(ep["steps"] for ep in entries), "n_I": 10},
            class_counts={"train": {head: [10] * count for head, count in zip(train.BC3_HEADS, train.BC3_CLASSES)}},
            auxiliary={"detour": {"available_steps": 0}})
        (cache / "index.json").write_text(json.dumps(index))
    result = train.aggregate_training_index(caches[0], [str(caches[1])], str(caches[0]))
    assert len(result["episodes"]) == 6
    assert result["navigation_memory_version"] == 4
    assert result["preprocessing"]["navigation_memory_version"] == 4
    assert all(Path(ep["file"]).is_absolute() for ep in result["episodes"])
    assert result["family_balance"]["weighted_shares"]["object"] == pytest.approx(.2)
    assert result["family_balance"]["weighted_shares"]["follow"] == pytest.approx(.3)
    assert result["class_counts"]["train"]["maneuver"] == [20, 20, 20]


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

    # Teacher-control attenuation must survive normalization and apply to every
    # action/auxiliary term; a link mask removes only the maneuver gradient.
    for value in logits.values():
        value.grad = None
    for name in auxiliary:
        auxiliary[name].grad = None
        targets[name + "_valid"] = torch.ones(1, 2)
    targets["next_near"].fill_(1)
    targets["detour"].fill_(1)
    targets["sample_weight"] = torch.full((1, 2), .5)
    _, half = train.bc3_loss(logits, auxiliary, labels, mask, targets,
                            [torch.ones(c) for c in train.BC3_CLASSES], 10.)
    targets["sample_weight"] = torch.ones(1, 2)
    _, full = train.bc3_loss(logits, auxiliary, labels, mask, targets,
                            [torch.ones(c) for c in train.BC3_CLASSES], 10.)
    for name in full:
        torch.testing.assert_close(half[name], full[name] * .5)
    targets["maneuver_valid"] = torch.tensor([[1., 0.]])
    loss, _ = train.bc3_loss(logits, auxiliary, labels, mask, targets,
                            [torch.ones(c) for c in train.BC3_CLASSES], 10.)
    loss.backward()
    assert logits["maneuver"].grad[0, 1].abs().sum() == 0
    assert logits["forward"].grad[0, 1].abs().sum() > 0


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

    hd = NavPolicy(train.VECTOR_SIZE, 8, format_version=3, auxiliary=True)
    hd_index = dict(preprocessing={**train.HD_PREPROCESSING, "navigation_memory_version": 4},
                    eye_layout=index["eye_layout"])
    export_weights(hd, tmp_path / "policy.navbc", preprocessing={**hd_index["preprocessing"],
                                                                "eye_layout": hd_index["eye_layout"]})
    checkpoint = tmp_path / "last.pt"
    torch.save(dict(config=dict(format_version=3, auxiliary=True, vector_size=train.VECTOR_SIZE, hidden_size=8),
                    model=hd.state_dict()), checkpoint)
    learner = NavPolicy(train.VECTOR_SIZE, 8, format_version=3, auxiliary=True)
    initialization = train.initialize_bc3_hd(learner, checkpoint, hd_index)
    assert initialization["reset"] == [] and initialization["expanded"] == []
    for name, value in hd.state_dict().items():
        torch.testing.assert_close(learner.state_dict()[name], value)
