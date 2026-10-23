"""Feature privacy, pooling, pair grouping and compact-cache round trips."""
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest


MODULE_PATH = Path(__file__).resolve().parents[1] / "nav_bc_data.py"
SPEC = importlib.util.spec_from_file_location("nav_bc_data", MODULE_PATH)
data = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(data)
demos = data.nav_demos


def fixture_records(count=3):
    records = np.zeros(count, dtype=demos.RECORD_V2_DTYPE)
    layout = np.tile(np.array([0, 0, 1000], dtype=np.float32), (demos.RAY_COUNT, 1))
    records["eye"]["distance_cm"] = 1000
    records["roads"]["carrot_direction"][:, 2] = 1
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
    assert data.PREPROCESSING["navdemo_version"] == 2
    assert data.PREPROCESSING["roads_schema"] == "carrot_v2"
    assert "previous_keys" not in data.PREPROCESSING["input_fields"]
    assert "previous_keys" not in data.PREPROCESSING


def test_action_change_compares_only_six_output_heads():
    records, _ = fixture_records(10)
    for index, name in enumerate(data.HEADS):
        records["keys"][name][index + 1:] = 1
    records["keys"]["head_yaw_deg"][7:] = 90
    records["keys"]["head_pitch_deg"][8:] = -45
    # Actor commands can differ throughout DAgger without a teacher change.
    records["previous_keys"]["turn"] = -1
    expected = [0, 1, 1, 1, 1, 1, 1, 0, 0, 0]
    np.testing.assert_array_equal(data.action_change_mask(records), expected)
    first = data.action_change_mask(records[:4])
    last_teacher = data.encode_labels(records[:4]["keys"])[-1]
    rest = data.action_change_mask(records[4:], last_teacher)
    np.testing.assert_array_equal(np.concatenate((first, rest)), expected)


def test_v1_guidance_is_rejected():
    records = np.zeros(1, dtype=demos.RECORD_DTYPE)
    _, layout = fixture_records(1)
    with pytest.raises(ValueError, match="NAVDEMO v2"):
        data.encode_features(demos.learner_input(records), layout)


def test_v2_guidance_vector_has_carrot_remaining_route_and_beyond_flag():
    records, layout = fixture_records(1)
    records["roads"]["carrot_direction"] = [1, 0, 0]
    records["roads"]["carrot_distance_cm"] = 200
    records["roads"]["goal_direction"] = [0, 0, -1]
    records["roads"]["goal_distance_cm"] = 75000
    records["roads"]["remaining_known_cm"] = 1234
    records["roads"]["beyond_known"] = 1
    vector = data.encode_features(demos.learner_input(records), layout)["vector"]
    offset = demos.RAY_COUNT * 9
    np.testing.assert_allclose(vector[0, offset:offset + 10],
                               [1, 0, 0, .2, 0, 0, -1, 50, 1.234, 1])


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
    records["roads"]["carrot_direction"] = [[1, 0, 0], [0, 0, 1], [-1, 0, 0]]
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
            stream.write(demos.HEADER.pack(demos.MAGIC, 2, demos.RECORD_BYTES,
                                           demos.RAY_COUNT, demos.NEAR_SIZE, demos.FAR_SIZE, demos.PERIOD_MS))
            stream.write(records.tobytes())
    index = data.prepare(source, output, chunk_records=1)
    assert index["excluded_unsuccessful_episodes"] == 1
    assert index["navdemo_version"] == 2
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
        stream.write(demos.HEADER.pack(demos.MAGIC, 2, demos.RECORD_BYTES,
                                      demos.RAY_COUNT, demos.NEAR_SIZE, demos.FAR_SIZE, demos.PERIOD_MS))
        stream.write(records.tobytes())


def test_dagger_preserves_heldout_and_trains_new_successful_v2_pairs(tmp_path):
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
    write_episode(first, "004", records, layout, "no-teacher", success=False, split="train")
    write_episode(second, "000", records, layout, "train")
    index = data.prepare(teacher, output, chunk_records=1, split_index=split_path,
                         dagger_sources=[first, second])
    assert index["excluded_unsuccessful_episodes"] == 1
    assert index["excluded_unsplit_teacher_episodes"] == 0
    assert index["excluded_dagger_episodes"] == 3
    assert len(index["episodes"]) == 6
    assert index["splits"]["train"] == {"pairs": 2, "episodes": 5, "steps": 10}
    assert index["splits"]["validation"] == {"pairs": 1, "episodes": 1, "steps": 2}
    dagger = [ep for ep in index["episodes"] if ep["source_kind"] == "dagger"]
    assert len(dagger) == 3 and any(not ep["success"] for ep in dagger)
    assert all(ep["split"] == "train" and ep["start"] in ("train", "new") for ep in dagger)
    assert len({ep["id"] for ep in index["episodes"]}) == 6
    for ep in index["episodes"]:
        np.testing.assert_array_equal(data.load_cached(output / ep["file"])["action_change"], [0, 1])
    assert json.loads(split_path.read_text()) == old_index


def test_dagger_requires_saved_split(tmp_path):
    with pytest.raises(ValueError, match="--split-index"):
        data.prepare(tmp_path / "teacher", tmp_path / "cache", dagger_sources=[tmp_path / "drive"])


def test_dagger_retains_original_train_pair_when_initial_teacher_failed(tmp_path):
    teacher, drive, output = (tmp_path / name for name in ("teacher", "drive", "cache"))
    split_path = tmp_path / "old-index.json"
    split_path.write_text(json.dumps({"episodes": [
        {"pair": ["successful", "goal"], "split": "train"},
        {"pair": ["teacher-failed", "goal"], "split": "train"},
        {"pair": ["heldout", "goal"], "split": "validation"},
    ]}))
    records, layout = fixture_records(2)
    write_episode(teacher, "000", records, layout, "successful")
    write_episode(teacher, "001", records, layout, "heldout")
    write_episode(teacher, "002", records, layout, "teacher-failed", success=False)
    write_episode(drive, "000", records, layout, "teacher-failed", success=False, split="train")
    index = data.prepare(teacher, output, split_index=split_path, dagger_sources=[drive])
    assert index["excluded_unsuccessful_episodes"] == 1
    assert index["excluded_dagger_episodes"] == 0
    assert [(ep["pair"], ep["split"]) for ep in index["episodes"] if ep["source_kind"] == "dagger"] == [
        (["teacher-failed", "goal"], "train")]


def test_saved_split_rejects_pair_leakage(tmp_path):
    split_path = tmp_path / "index.json"
    split_path.write_text(json.dumps({"episodes": [
        {"pair": ["a", "b"], "split": "train"},
        {"pair": ["a", "b"], "split": "validation"},
    ]}))
    with pytest.raises(ValueError, match="both partitions"):
        data.read_pair_split(split_path)


def test_bc3_maneuver_labels_are_exclusive_and_keep_legacy_heads():
    records, _ = fixture_records(3)
    records["keys"]["jump"] = [0, 1, 0]
    records["keys"]["climb"] = [0, 0, 1]
    labels = data.encode_bc3_labels(records["keys"])
    np.testing.assert_array_equal(labels[:, 4], [0, 1, 2])
    assert labels.shape == (3, 5)
    assert data.encode_labels(records["keys"]).shape == (3, 6)
    records["keys"]["jump"][2] = 1
    with pytest.raises(ValueError, match="simultaneous jump and climb"):
        data.encode_bc3_labels(records["keys"])


def test_bc3_global_change_weight_and_press_windows():
    assert data.action_change_weight(100, 20) == 5
    assert data.action_change_weight(100, 1) == 10
    assert data.action_change_weight(100, 0) == 1
    assert data.action_change_weight(1, 10) == 1
    labels = np.tile([2, 1, 1, 0, 0], (7, 1))
    labels[3:5, 4] = 1
    np.testing.assert_array_equal(data.label_action_changes(labels), [0, 0, 0, 1, 0, 1, 0])
    # Both window boundaries are included; held jump at t=4 s is not a new press.
    np.testing.assert_array_equal(data.maneuver_weights(np.arange(7) * 1000, labels[:, 4]),
                                  [1, 1, 5, 5, 5, 1, 1])


def test_bc3_stuck_target_requires_full_horizon_and_requested_movement():
    stamps = np.arange(6) * 1000
    labels = np.tile([2, 1, 1, 0, 0], (6, 1))
    position = np.zeros((6, 3), np.float32)
    position[3:, 0] = 50
    target, valid = data.future_stuck(stamps, position, labels)
    np.testing.assert_array_equal(target, [1, 0, 0, 1, 0, 0])
    np.testing.assert_array_equal(valid, [1, 1, 1, 1, 0, 0])
    labels[0, 0] = 1
    assert data.future_stuck(stamps, position, labels)[0][0] == 0
    with pytest.raises(ValueError, match="increase strictly"):
        data.future_stuck(np.zeros(6), position, labels)


def test_bc3_detour_target_is_local_supervision_and_excludes_memory_teacher():
    records, _ = fixture_records(2)
    records["position_cm"] = [100, 200, 300]
    records["teacher_hint"]["next_corner_cm"] = [200, 200, 300]
    records["teacher_hint"]["path_cm"] = [1234, 100000]
    records["body_yaw_deg"] = [0, 90]
    target, valid = data.detour_targets(records, 2)
    np.testing.assert_allclose(target, [[1.234, 0, 0, 1], [50, -1, 0, 0]], atol=1e-6)
    np.testing.assert_array_equal(valid, [1, 1])
    unavailable, valid = data.detour_targets(records, 3)
    assert not unavailable.any() and not valid.any()


def write_bc3_manifest(tmp_path, families=("point", "follow_below_gate")):
    source, output = tmp_path / "source", tmp_path / "cache"
    records, layout = fixture_records(4)
    records["time_ms"] = np.arange(4) * 1000
    records["keys"]["forward"] = [1, 1, 0, 0]
    entries = []
    train = {}
    for number, family in enumerate(families):
        identity = f"{family}-{number}"
        success = family != "follow_below_gate"
        write_episode(source, identity, records, layout, identity, success=success,
                      navigation_memory_version=3, teacher_version=3)
        directory = source / "episodes" / identity
        with (directory / "samples.bin.gz").open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        scenario = {"id": identity, "split": "train", "region": "forest", "start": identity,
                    "goal": "goal", "goal_kind": "follow" if family == "follow_below_gate" else family,
                    "navigation_memory_version": 3, "teacher_version": 3, "seed": 7}
        entries.append({"id": identity, "family": family, "split": "train", "episode": str(directory),
                        "decisions": len(records), "training_eligible": True,
                        "teacher_goal_success": success, "samples_sha256": digest, "scenario": scenario})
        train[family] = [identity]
    manifest = {"schema_version": 1, "dataset": "nav-demos-v4", "consumer": "nav-bc3", "complete": True,
                "format": {"name": "NAVDEMO", "version": 2, "navigation_memory_version": 3,
                           "perception_mode": "gaze", "physics": "cv"},
                "root": str(source), "episodes": entries, "splits": {"train": train,
                    "validation": {"point": [{"id": "heldout", "episode": None, "recorded": False}]},
                    "test": {"point": [{"id": "final-only", "episode": None, "recorded": False}]}},
                "totals": {"train_episodes": len(entries), "train_decisions": len(entries) * len(records)}}
    path = tmp_path / "manifest.json"
    path.write_text(json.dumps(manifest))
    return source, output, path, manifest, records, layout


def test_bc3_manifest_uses_exact_train_refs_including_failed_follow(tmp_path):
    source, output, path, manifest, records, layout = write_bc3_manifest(tmp_path)
    # Even a real extra successful recording is not admitted without a train ref.
    write_episode(source, "not-selected", records, layout, "not-selected", navigation_memory_version=3)
    index = data.prepare(source, output, manifest=path, chunk_records=1)
    assert index["cache_version"] == data.BC3_CACHE_VERSION
    assert index["heads"] == list(data.BC3_HEADS)
    assert index["classes"] == list(data.BC3_CLASSES)
    assert index["splits"]["train"] == {"pairs": 2, "episodes": 2, "steps": 8}
    assert [ep["id"] for ep in index["episodes"]] == [ep["id"] for ep in manifest["episodes"]]
    assert [ep["success"] for ep in index["episodes"]] == [True, False]
    assert index["action_change_weight"] == 4
    assert index["action_change"]["N"] == 8 and index["action_change"]["n_I"] == 2
    assert index["auxiliary"]["detour"]["available_steps"] == 0
    assert index["auxiliary"]["true_map"]["available"] is False
    expected = data.encode_features(demos.learner_input(records), layout)
    for entry in index["episodes"]:
        cached = data.load_cached(output / entry["file"])
        assert cached["labels"].shape == (4, 5)
        np.testing.assert_array_equal(cached["action_change"], [0, 0, 1, 0])
        np.testing.assert_array_equal(cached["maneuver_weight"], 1)
        np.testing.assert_array_equal(cached["stuck_valid"], [1, 1, 0, 0])
        np.testing.assert_array_equal(cached["next_near_valid"], [1, 1, 1, 0])
        assert not cached["detour_valid"].any()
        for name in ("near", "far", "vector"):
            np.testing.assert_allclose(cached[name], expected[name], atol=1 / 255)


@pytest.mark.parametrize("relative", ["point/train/point-0", "../outside", "/outside"])
def test_compact_v5_manifest_uses_supplied_root_and_only_eligible_train(tmp_path, monkeypatch, relative):
    source = tmp_path / "relocated-corpus"
    directory = source / "point/train/point-0"
    directory.mkdir(parents=True)
    metadata = {"id": "point-0", "goal_kind": "point", "split": "train", "training_eligible": True,
                "decisions": 2, "navigation_memory_version": 4, "journey_memory": True,
                "demo_version": 3, "perception_mode": "gaze", "physics_mode": "cv",
                "start": "start", "goal": "goal", "region": "forest", "teacher_version": 3}
    (directory / "episode.json").write_text(json.dumps(metadata))
    entry = {"id": "point-0", "family": "point", "split": "train", "training_eligible": True,
             "decisions": 2, "samples_sha256": "a" * 64, "episode": relative}
    manifest = {"schema_version": 2, "dataset": "nav-demos-v5", "consumer": "nav-bc",
                "input_variant": "hd", "complete": True, "root": "/original/corpus",
                "format": {"name": "NAVDEMO", "version": 3, "navigation_memory_version": 4,
                           "perception_mode": "gaze", "physics": "cv"},
                "full_manifest": {"path": "/unavailable/full/manifest.json", "sha256": "b" * 64},
                "episodes": [entry, {**entry, "id": "failed", "training_eligible": False,
                                     "episode": "missing/failed"},
                             {**entry, "id": "heldout", "split": "validation",
                              "episode": "missing/heldout"}],
                "totals": {"training_episodes": 1, "training_decisions": 2}}
    path = tmp_path / "manifest.json"
    path.write_text(json.dumps(manifest, separators=(",", ":")))
    if relative != "point/train/point-0":
        with pytest.raises(ValueError, match="outside the corpus"):
            data.load_training_manifest(path, source)
        return
    loaded, selected = data.load_training_manifest(path, source)
    assert loaded == manifest
    assert selected == [{**entry, "scenario": metadata, "directory": directory}]
    assert not (source / "missing").exists()
    records = np.zeros(2, dtype=demos.RECORD_V3_DTYPE)
    records["time_ms"] = [100, 200]
    records["position_cm"][:, 1] = 700
    records["memory_near"]["height_valid"][:, 3, 4] = 1
    records["memory_near"]["ground_height_cm"][:, 3, 4] = 940
    records["memory_near"]["channels"][:, 3, 4, 7] = [255, 128]
    records["memory_far"]["channels"][:, 60, 70, 7] = [128, 255]
    layout = np.tile([0., 0., 1000.], (demos.RAY_COUNT, 1)).astype(np.float32)
    metadata["eye_layout"] = layout.tolist()
    (directory / "episode.json").write_text(json.dumps(metadata))
    with gzip.open(directory / "samples.bin.gz", "wb") as stream:
        stream.write(demos.HEADER.pack(demos.MAGIC, 3, demos.RECORD_BYTES_BY_VERSION[3],
                                      demos.RAY_COUNT, demos.NEAR_SIZE, demos.FAR_SIZE, demos.PERIOD_MS))
        stream.write(records.tobytes())
    entry["samples_sha256"] = hashlib.sha256((directory / "samples.bin.gz").read_bytes()).hexdigest()
    path.write_text(json.dumps(manifest))
    output = tmp_path / "cache"
    original_stat = Path.stat
    def cache_stat(path, *args, **kwargs):
        result = original_stat(path, *args, **kwargs)
        if path.is_relative_to(output):
            return type(result)((*result[:6], 1024, *result[7:]))
        return result
    with monkeypatch.context() as storage:
        storage.setattr(data.shutil, "disk_usage", lambda path: SimpleNamespace(free=32 * 1024**3))
        storage.setattr(Path, "stat", cache_stat)
        index = data.prepare(source, output, manifest=path, input_variant="hd-familiar", chunk_records=1)
    assert index["cache_version"] == data.BC4_CACHE_VERSION
    assert index["navigation_memory_version"] == 4 and index["navdemo_version"] == 3
    assert index["preprocessing"] == data.BC4_PREPROCESSING
    cached = data.load_cached(tmp_path / "cache" / index["episodes"][0]["file"])
    assert cached["near"].shape == (2, 11, 64, 64)
    assert cached["far"].shape == (2, 11, 50, 100)
    np.testing.assert_allclose(cached["near"][:, 7:10, 3, 4], [[1.2, 1.2, 1]] * 2, atol=1e-3)
    np.testing.assert_allclose(cached["near"][:, 10, 3, 4], [1, 128 / 255])
    np.testing.assert_allclose(cached["far"][:, 10, 10, 70], [128 / 255, 1])
    assert cached["aux_near"].shape == (2, 9, 32, 32)
    dagger = tmp_path / "dagger" / "point" / "learner"
    dagger.mkdir(parents=True)
    (dagger / "samples.bin.gz").write_bytes((directory / "samples.bin.gz").read_bytes())
    (dagger / "episode.json").write_text(json.dumps({**metadata, "drive": True, "dagger": True,
        "dagger_scenario": {"split": "train"}, "training_valid": True, "teacher_version": 3}))
    (dagger / "samples.jsonl").write_text("\n".join(json.dumps({
        "time_ms": int(stamp), "teacher_control": number == 0, "maneuver_mask": number == 1,
    }) for number, stamp in enumerate(records["time_ms"])))
    hd_output = tmp_path / "hd-cache"
    links = tmp_path / "accepted-links.txt"
    links.write_text("0 7 0 1 7 0 1 1\n")
    with monkeypatch.context() as storage:
        storage.setattr(data.shutil, "disk_usage", lambda path: SimpleNamespace(free=32 * 1024**3))
        hd_index = data.prepare(source, hd_output, manifest=path, input_variant="hd", chunk_records=1,
                                dagger_sources=[tmp_path / "dagger"], maneuver_links=links)
    assert hd_index["cache_version"] == data.HD_CACHE_VERSION
    assert hd_index["navigation_memory_version"] == 4
    assert hd_index["preprocessing"] == {**data.HD_PREPROCESSING, "navigation_memory_version": 4}
    assert [ep["source_kind"] for ep in hd_index["episodes"]] == ["teacher", "dagger"]
    entry = hd_index["episodes"][1]
    assert entry["effective_steps"] == 1.5 and entry["teacher_control_steps"] == 1
    cached = data.load_cached(hd_output / entry["file"])
    assert cached["near"].shape == (2, 10, 64, 64)
    assert cached["far"].shape == (2, 10, 50, 100)
    np.testing.assert_array_equal(cached["sample_weight"], [.5, 1])
    np.testing.assert_array_equal(cached["maneuver_valid"], [1, 0])
    assert not cached["labels"][:, 4].any()
    teacher = data.load_cached(hd_output / hd_index["episodes"][0]["file"])
    assert not teacher["maneuver_valid"].any()


@pytest.mark.parametrize("input_variant", ["compact", "hd", "hd-familiar"])
def test_packed_cache_slices_match_expanded_episode(tmp_path, input_variant):
    records, layout = fixture_records(4)
    if input_variant == "hd-familiar":
        familiar_records = np.zeros(4, dtype=demos.RECORD_V3_DTYPE)
        for name in records.dtype.names:
            if name.startswith("memory_"):
                for field in records[name].dtype.names:
                    if field == "channels":
                        familiar_records[name][field][..., :7] = records[name][field]
                    else:
                        familiar_records[name][field] = records[name][field]
                familiar_records[name]["channels"][..., 7] = 128
            else:
                familiar_records[name] = records[name]
        records = familiar_records
    features = data.encode_features(demos.learner_input(records), layout, input_variant,
                                    records["position_cm"][:, 1] if input_variant != "compact" else None)
    if input_variant != "compact":
        features["aux_near"] = data.encode_features(demos.learner_input(records), layout)["near"]
    packed = data._pack(features)
    packed.update(labels=data.encode_bc3_labels(records["keys"]),
                  baseline=np.zeros((4, 5), np.uint8), situations=np.zeros((4, 3), np.uint8),
                  action_change=np.array([0, 1, 0, 1], np.uint8),
                  next_near_valid=np.array([1, 1, 1, 0], np.uint8))
    path = tmp_path / "episode.npz"
    np.savez_compressed(path, **packed)
    expanded = data.load_cached(path)
    loaded = data.load_cached(path, packed=True)
    assert "near" not in loaded and "far" not in loaded
    assert loaded["near_channels"].dtype == np.uint8
    assert loaded["near_height"].dtype == np.float16
    assert loaded["vector"].dtype == np.float16
    for start, stop in ((0, 3), (2, 4), (4, 7)):
        segment = data.unpack_cached(loaded, start, stop)
        assert segment.keys() == expanded.keys()
        for name in expanded:
            np.testing.assert_array_equal(segment[name], expanded[name][start:stop])


@pytest.mark.parametrize("raw_annotations", [False, True])
def test_bc3_manifest_admits_early_episode_without_collector_annotations(tmp_path, raw_annotations):
    source, output, path, manifest, _, _ = write_bc3_manifest(tmp_path, ("point",))
    entry = manifest["episodes"][0]
    scenario = entry["scenario"]
    for name in ("id", "split", "region"):
        scenario.pop(name)
    scenario["seed"] = 0
    physical_scenario = scenario.copy()
    metadata_path = Path(entry["episode"]) / "episode.json"
    metadata = json.loads(metadata_path.read_text())
    for name in ("id", "split", "region", "success", "training_eligible"):
        metadata.pop(name, None)
    if raw_annotations:
        metadata.update(success=False, training_eligible=False)
    metadata_path.write_text(json.dumps(metadata))

    summary_path = source / "recordings" / "collection-metrics.json"
    summary_path.parent.mkdir()
    summary_path.write_text(json.dumps({"episodes": [{
        "id": entry["id"], "family": "point", "split": "train", "region": "forest",
        "training_eligible": True, "start": "summary-start", "goal": "summary-goal", "seed": 99,
    }]}))
    manifest["sources"] = {"recordings/collection-metrics.json": {
        "path": str(summary_path), "sha256": hashlib.sha256(summary_path.read_bytes()).hexdigest(),
    }}
    path.write_text(json.dumps(manifest))
    original_files = {file: file.read_bytes() for file in source.rglob("*") if file.is_file()}

    _, selected = data.load_training_manifest(path, source)
    assert selected[0]["scenario"] == {**physical_scenario, "region": "forest"}
    index = data.prepare(source, output, manifest=path, chunk_records=1)
    assert index["splits"]["train"] == {"pairs": 1, "episodes": 1, "steps": 4}
    cached_entry = index["episodes"][0]
    assert cached_entry["id"] == entry["id"]
    assert cached_entry["pair"] == [physical_scenario["start"], physical_scenario["goal"]]
    assert cached_entry["region"] == "forest" and cached_entry["success"] is True
    assert data.load_cached(output / cached_entry["file"])["labels"].shape == (4, 5)
    assert {file: file.read_bytes() for file in original_files} == original_files


@pytest.mark.parametrize("damage, expected", [
    ("ineligible", "ineligible"), ("heldout", "ineligible"), ("duplicate", "duplicate training"),
    ("totals", "totals"), ("checksum", "checksum"), ("memory", "memory-v3"),
])
def test_bc3_manifest_rejects_wrong_training_identity(tmp_path, damage, expected):
    source, output, path, manifest, _, _ = write_bc3_manifest(tmp_path, ("point",))
    entry = manifest["episodes"][0]
    if damage == "ineligible":
        entry["training_eligible"] = False
    elif damage == "heldout":
        entry["scenario"]["split"] = "validation"
    elif damage == "duplicate":
        manifest["splits"]["train"]["point"].append(entry["id"])
    elif damage == "totals":
        manifest["totals"]["train_decisions"] += 1
    elif damage == "checksum":
        entry["samples_sha256"] = "0" * 64
    elif damage == "memory":
        metadata_path = Path(entry["episode"]) / "episode.json"
        metadata = json.loads(metadata_path.read_text())
        metadata["navigation_memory_version"] = 2
        metadata_path.write_text(json.dumps(metadata))
    path.write_text(json.dumps(manifest))
    with pytest.raises(ValueError, match=expected):
        data.prepare(source, output, manifest=path)
