"""Action-change evaluation contracts; run on Ryzen with the other BC tests."""

from pathlib import Path
import sys
from types import SimpleNamespace

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import nav_bc_train as train


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
