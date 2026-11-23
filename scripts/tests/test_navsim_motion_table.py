"""Known 100-ms key/history motion survives NAVDEMO loading and open-loop replay."""

from dataclasses import replace
import gzip
import hashlib
import json
from pathlib import Path
import sys

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import nav_demos as demos
from navsim import motion_table as motion


def synthetic_episode(root, name, version, initial_yaw):
    directory = root / "point" / "train" / name
    directory.mkdir(parents=True)
    rng = np.random.default_rng(17)
    keys = np.column_stack((np.ones(180), rng.integers(-1, 2, (180, 2)), rng.integers(0, 2, 180)))
    previous = [np.vstack((np.zeros((h, 4)), keys[:-h])) for h in (1, 2, 3)]
    known = np.column_stack((12 + keys[:, 3] + .5 * previous[0][:, 1],
                             2 * keys[:, 1] + .5 * previous[1][:, 2] + .75 * previous[2][:, 2],
                             .1 * previous[1][:, 2], -9 * keys[:, 2]))
    pose = np.zeros((len(keys) + 1, 4))
    pose[0] = [123, 5, -234, initial_yaw]
    for i, (forward, right, up, yaw) in enumerate(known):
        angle = np.deg2rad(pose[i, 3])
        pose[i + 1] = pose[i] + [forward * np.cos(angle) - right * np.sin(angle), up,
                                 forward * np.sin(angle) + right * np.cos(angle), yaw]
        pose[i + 1, 3] = motion.wrap(pose[i + 1, 3])
    trajectory = np.column_stack((np.arange(len(pose)) * 100, pose))
    actions = np.zeros((len(pose), 9))
    actions[:, 0] = trajectory[:, 0]
    actions[:-1, 1:5] = keys
    actions[-1, 4] = 1
    np.savetxt(directory / "trajectory.txt", trajectory, fmt="%.17g")
    np.savetxt(directory / "actions.txt", actions, fmt="%.12g")
    records = np.zeros(len(keys), dtype=demos.RECORD_DTYPES[version])
    records["time_ms"] = trajectory[:-1, 0] + 10000
    records["position_cm"] = pose[:-1, :3]
    records["body_yaw_deg"] = pose[:-1, 3]
    records["body"]["ground_normal"][:, 1] = 1
    with gzip.open(directory / "samples.bin.gz", "wb") as stream:
        stream.write(demos.HEADER.pack(demos.MAGIC, version, records.dtype.itemsize,
                                       demos.RAY_COUNT, demos.NEAR_SIZE, demos.FAR_SIZE, 100))
        stream.write(records.tobytes())
    (directory / "episode.json").write_text(json.dumps({"decisions": len(keys)}))
    with (directory / "perception-safety-frames.jsonl").open("w") as stream:
        for ms in trajectory[:, 0]:
            stream.write(json.dumps(dict(elapsed_ms=int(ms), supported=True, in_air=False,
                                         falling=False, native_jump=False, native_climb=False,
                                         swimming=False, water_depth_cm=0,
                                         deep_water_threshold_cm=140)) + "\n")
    return motion.load_episode(directory, root), known


@pytest.mark.parametrize("version", [2, 3])
def test_exact_known_history_table_and_rollouts(version, tmp_path):
    train, known = synthetic_episode(tmp_path, "train", version, 170)
    test, _ = synthetic_episode(tmp_path, "test", version, -110)
    np.testing.assert_allclose(train.target, known, atol=1e-8)
    tables, reference = motion.fit([train])
    predicted, used = motion.lookup(test.ids, tables, 3)
    np.testing.assert_allclose(predicted, known, atol=1e-8)
    assert (used == 3).all()
    free = motion.masks(test, reference)["free"]
    assert free.all()
    for horizon in (10, 30, 100):
        error = motion.rollout(test, predicted, free, horizon)
        assert len(error) == len(known) - horizon + 1
        np.testing.assert_allclose(error[:, :4], 0, atol=1e-7)
        assert (error[:, 4] > 1).all()
    result = motion.evaluate([train], [test], tables, reference)
    assert result["history_comparison"]["3"]["one_step_free"]["position_cm"]["p99"] < 1e-8
    assert result["history_comparison"]["0"]["one_step_free"]["position_cm"]["p90"] > .1
    assert sum(row["steps"] for row in result["turn_in_place"]["groups"]) == 0
    split1 = motion.split_episodes([train, test], 73)
    split2 = motion.split_episodes([test, train], 73)
    assert [[e.identifier for e in part] for part in split1] == [[e.identifier for e in part] for part in split2]
    assert {e.identifier for e in split1[0]}.isdisjoint(e.identifier for e in split1[1])


def test_free_filter_and_rollout_reject_obstacles_water_and_missing_keys(tmp_path):
    train, _ = synthetic_episode(tmp_path, "train", 3, 170)
    tables, reference = motion.fit([train])
    target = train.target.copy()
    target[20, :2] = 0
    wet = train.wet.copy()
    wet[60] = True
    test = replace(train, target=target, wet=wet)
    masks = motion.masks(test, reference)
    assert np.flatnonzero(masks["probable_blocked"]).tolist() == [20]
    assert np.flatnonzero(~masks["free"]).tolist() == [20, 60]
    predicted, _ = motion.lookup(test.ids, tables, 3)
    assert len(motion.rollout(test, predicted, masks["free"], 10)) == 151
    # A missing current key cannot fall back to an unrelated movement.
    missing = train.ids[:1].copy()
    missing[:] = -1
    predicted, used = motion.lookup(missing, tables, 3)
    assert np.isnan(predicted).all() and used.tolist() == [-2]


def test_table_export_keeps_full_precision_in_sidecar(tmp_path):
    row = [101, 7, 1.234567, -2.345678, .000499, -8.765432]
    table = {"columns": ["key", "count", "forward", "right", "up", "yaw"],
             "levels": {str(level): [row] for level in range(-1, 4)}}
    assert motion.export_table(table) == table
    full_path = tmp_path / "table-full.json"
    exported = motion.export_table(table, (-1, 0, 1), full_path)
    assert json.loads(full_path.read_bytes()) == table
    assert exported["levels"] == {str(level): [[101, 7, 1.235, -2.346, 0., -8.765]]
                                  for level in (-1, 0, 1)}
    assert exported["full"] == {"path": str(full_path),
                                "sha256": hashlib.sha256(full_path.read_bytes()).hexdigest(),
                                "levels": {"2": 1, "3": 1}}
