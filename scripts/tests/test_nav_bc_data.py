"""Feature privacy, pooling, pair grouping and compact-cache round trips."""
import gzip
import importlib.util
import json
from pathlib import Path

import numpy as np
import pytest


MODULE_PATH = Path(__file__).resolve().parents[1] / "nav_bc_data.py"
SPEC = importlib.util.spec_from_file_location("nav_bc_data", MODULE_PATH)
data = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(data)
demos = data.nav_demos


def fixture_records(count=3):
    records = np.zeros(count, dtype=demos.RECORD_DTYPE)
    layout = np.tile(np.array([0, 0, 1000], dtype=np.float32), (demos.RAY_COUNT, 1))
    records["eye"]["distance_cm"] = 1000
    records["roads"]["next_direction"][:, 2] = 1
    for name in ("memory_near", "memory_far"):
        memory = records[name]
        middle = memory.shape[1] // 2
        memory["height_valid"][:, middle, middle] = 1
        memory["ground_height_cm"][:, middle, middle] = 12345
        memory["channels"][:, middle, middle] = [255, 255, 0, 0, 0, 0, 0]
    return records, layout


def test_feature_boundary_excludes_privileged_fields_and_absolute_height():
    records, layout = fixture_records()
    expected = data.encode_features(demos.learner_input(records), layout)
    assert expected["near"].shape == (3, 9, 32, 32)
    assert expected["far"].shape == (3, 9, 25, 25)
    assert expected["vector"].shape == (3, data.VECTOR_SIZE)
    records["teacher_hint"]["next_corner_cm"] = 99999
    records["teacher_hint"]["path_cm"] = 99999
    records["position_cm"] = 99999
    records["body_yaw_deg"] = 333
    records["keys"]["turn"] = -1
    for name in records["previous_keys"].dtype.names:
        records["previous_keys"][name] = 1
    # Sensor memory stores world ground heights: translation must cancel locally.
    for name in ("memory_near", "memory_far"):
        records[name]["ground_height_cm"][records[name]["height_valid"] != 0] += 10000
    actual = data.encode_features(demos.learner_input(records), layout)
    for name in expected:
        np.testing.assert_array_equal(actual[name], expected[name])
    with pytest.raises(ValueError, match="learner_input"):
        data.encode_features({**demos.learner_input(records), "teacher_hint": records["teacher_hint"]}, layout)
    assert data.VECTOR_SIZE == 2166
    assert data.PREPROCESSING["vector_order"] == ["eye", "roads", "body"]
    assert "previous_keys" not in data.PREPROCESSING


def test_action_change_compares_only_six_output_heads():
    records, _ = fixture_records(8)
    for index, name in enumerate(data.HEADS):
        records["keys"][name][index] = 1
    records["keys"]["head_yaw_deg"][6] = 90
    records["previous_keys"]["head_pitch_deg"][6] = -45
    # Both nonzero values mean the same flag action.
    records["keys"]["walk"][7] = 1
    records["previous_keys"]["walk"][7] = 1
    np.testing.assert_array_equal(data.action_change_mask(records), [1, 1, 1, 1, 1, 1, 0, 0])


def test_pooling_ignores_unknown_ground_and_averages_channels():
    records, layout = fixture_records(1)
    near = records["memory_near"]
    near["height_valid"][0, 32, 33] = 1
    near["ground_height_cm"][0, 32, 33] = 12545
    encoded = data.encode_features(demos.learner_input(records), layout)["near"]
    assert encoded[0, 0, 16, 16] == .25
    assert encoded[0, 1, 16, 16] == .25
    assert encoded[0, 7, 16, 16] == .5  # average 0cm and 200cm, divided by 200cm
    assert encoded[0, 8, 16, 16] == .5
    assert encoded[0, 7, 0, 0] == 0


def test_axis_labels_and_engine_turn_sign():
    records, _ = fixture_records()
    records["keys"]["forward"] = [-1, 0, 1]
    records["keys"]["turn"] = [-1, 0, 1]
    records["roads"]["next_direction"] = [[1, 0, 0], [0, 0, 1], [-1, 0, 0]]
    labels = data.encode_labels(records["keys"])
    np.testing.assert_array_equal(labels[:, 0], [0, 1, 2])
    np.testing.assert_array_equal(labels[:, 2], [0, 1, 2])
    baseline = data.road_baseline(demos.learner_input(records))
    np.testing.assert_array_equal(baseline[:, 2], [0, 1, 2])
    np.testing.assert_array_equal(baseline[:, 0], [2, 2, 2])


def test_pair_split_is_deterministic_grouped_and_stratified():
    episodes = [{"start": f"{region}-{index}", "goal": "goal", "region": region}
                for region in ("city", "farm", "forest") for index in range(10)]
    split = data.split_pairs(episodes + episodes[::2])
    assert split == data.split_pairs(episodes[::-1])
    assert sum(part == "validation" for part in split.values()) == 6
    for region in ("city", "farm", "forest"):
        assert sum(part == "validation" and pair[0].startswith(region) for pair, part in split.items()) == 2


def test_prepare_reads_success_only_and_cache_round_trips(tmp_path):
    source, output = tmp_path / "source", tmp_path / "cache"
    records, layout = fixture_records(2)
    for index in range(6):
        episode = source / "episodes" / f"{index:03}"
        episode.mkdir(parents=True)
        metadata = {"success": index != 5, "start": f"start-{index // 2}", "goal": "goal",
                    "region": "forest", "decisions": 2, "eye_layout": layout.tolist()}
        (episode / "episode.json").write_text(json.dumps(metadata))
        with gzip.open(episode / "samples.bin.gz", "wb") as stream:
            stream.write(demos.HEADER.pack(demos.MAGIC, demos.VERSION, demos.RECORD_BYTES,
                                           demos.RAY_COUNT, demos.NEAR_SIZE, demos.FAR_SIZE, demos.PERIOD_MS))
            stream.write(records.tobytes())
    index = data.prepare(source, output, chunk_records=1)
    assert index["excluded_unsuccessful_episodes"] == 1
    assert len(index["episodes"]) == 5
    assignments = {}
    for entry in index["episodes"]:
        pair = tuple(entry["pair"])
        assert assignments.setdefault(pair, entry["split"]) == entry["split"]
        cached = data.load_cached(output / entry["file"])
        expected = data.encode_features(demos.learner_input(records), layout)
        for name in ("near", "far", "vector"):
            np.testing.assert_allclose(cached[name], expected[name], atol=1 / 255)
        assert cached["labels"].dtype == np.int64
        np.testing.assert_array_equal(cached["situations"][:, 2], [1, 1])
        np.testing.assert_array_equal(cached["action_change"], [0, 0])


def write_episode(source, identity, records, layout, start, success=True, **extra):
    episode = source / "episodes" / identity
    episode.mkdir(parents=True)
    metadata = {"success": success, "start": start, "goal": "goal", "region": "forest",
                "decisions": len(records), "eye_layout": layout.tolist(), **extra}
    (episode / "episode.json").write_text(json.dumps(metadata))
    with gzip.open(episode / "samples.bin.gz", "wb") as stream:
        stream.write(demos.HEADER.pack(demos.MAGIC, demos.VERSION, demos.RECORD_BYTES,
                                      demos.RAY_COUNT, demos.NEAR_SIZE, demos.FAR_SIZE, demos.PERIOD_MS))
        stream.write(records.tobytes())


def test_dagger_keeps_failed_training_drives_without_changing_saved_split(tmp_path):
    teacher, first, second, output = (tmp_path / name for name in ("teacher", "drive1", "drive2", "cache"))
    split_path = tmp_path / "old-index.json"
    old_index = {"episodes": [{"pair": ["train", "goal"], "split": "train"},
                              {"pair": ["heldout", "goal"], "split": "validation"}]}
    split_path.write_text(json.dumps(old_index))
    records, layout = fixture_records(2)
    records["keys"]["turn"] = [-1, 1]
    records["previous_keys"]["turn"] = [-1, 0]
    write_episode(teacher, "000", records, layout, "train")
    write_episode(teacher, "001", records, layout, "heldout")
    write_episode(teacher, "002", records, layout, "new")
    write_episode(teacher, "003", records, layout, "train", success=False)
    write_episode(first, "000", records, layout, "train", success=False, split="train")
    write_episode(first, "001", records, layout, "heldout", success=False, split="validation")
    write_episode(first, "002", records, layout, "new", success=False, split="train")
    write_episode(first, "003", records, layout, "train", success=False, split="control")
    write_episode(second, "000", records, layout, "train")
    index = data.prepare(teacher, output, chunk_records=1, split_index=split_path,
                         dagger_sources=[first, second])
    assert index["excluded_unsuccessful_episodes"] == 1
    assert index["excluded_unsplit_teacher_episodes"] == 1
    assert index["excluded_dagger_episodes"] == 3
    assert len(index["episodes"]) == 4
    assert index["splits"]["train"] == {"pairs": 1, "episodes": 3, "steps": 6}
    assert index["splits"]["validation"] == {"pairs": 1, "episodes": 1, "steps": 2}
    dagger = [ep for ep in index["episodes"] if ep["source_kind"] == "dagger"]
    assert len(dagger) == 2 and any(not ep["success"] for ep in dagger)
    assert all(ep["split"] == "train" and ep["start"] == "train" for ep in dagger)
    assert len({ep["id"] for ep in index["episodes"]}) == 4
    for ep in index["episodes"]:
        np.testing.assert_array_equal(data.load_cached(output / ep["file"])["action_change"], [0, 1])
    assert json.loads(split_path.read_text()) == old_index


def test_dagger_requires_saved_split(tmp_path):
    with pytest.raises(ValueError, match="--split-index"):
        data.prepare(tmp_path / "teacher", tmp_path / "cache", dagger_sources=[tmp_path / "drive"])


def test_saved_split_rejects_pair_leakage(tmp_path):
    split_path = tmp_path / "index.json"
    split_path.write_text(json.dumps({"episodes": [
        {"pair": ["a", "b"], "split": "train"},
        {"pair": ["a", "b"], "split": "validation"},
    ]}))
    with pytest.raises(ValueError, match="both partitions"):
        data.read_pair_split(split_path)
