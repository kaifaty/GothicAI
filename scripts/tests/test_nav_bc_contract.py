"""The parity fixture preserves the NAVDEMO road schema selected by the weights."""
import gzip
import json
from pathlib import Path
import sys

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_nav_policy as contract


@pytest.mark.parametrize("version", [1, 2, 3])
def test_synthetic_records_use_requested_road_schema(version):
    layout, samples = contract.synthetic_records(2, version)
    records = np.concatenate(list(samples))
    assert layout.shape == (contract.demos.RAY_COUNT, 3)
    assert records.dtype == contract.demos.RECORD_DTYPES[version]
    roads = records["roads"]
    target = "carrot_direction" if version >= 2 else "next_direction"
    np.testing.assert_allclose(np.linalg.norm(roads[target], axis=1), 1, atol=1e-6)
    np.testing.assert_array_equal(roads["beyond_known" if version >= 2 else "goal_in_known"], [0, 1])


@pytest.mark.parametrize("version", [1, 2])
@pytest.mark.parametrize("weight_version", [1, 2])
def test_synthetic_weight_header_declares_input_composition(tmp_path, version, weight_version):
    weights = tmp_path / "fixture.navbc"
    contract.synthetic_weights(weights, version, weight_version)
    with weights.open("rb") as stream:
        header = json.loads(stream.readline())
    preprocessing = header["preprocessing"]
    assert preprocessing["navdemo_version"] == version
    assert preprocessing["roads_schema"] == ("carrot_v2" if version == 2 else "next_v1")
    assert preprocessing["roads_fields"] == contract.ROAD_FIELDS[version]
    assert preprocessing["input_fields"] == ["body", "roads", "eye", "memory_near", "memory_far"]
    assert preprocessing["vector_order"] == ["eye", "roads", "body"]
    assert header["architecture"]["inputs"]["vector"] == [2166]
    assert header["version"] == weight_version
    assert {name: head["classes"] for name, head in header["architecture"]["heads"].items()} == contract.HEAD_SIZES[weight_version]
    policy = contract.NumpyPolicy(weights)
    assert policy.format_version == weight_version
    assert policy.head_names == tuple(contract.HEAD_SIZES[weight_version])
    assert policy.temperatures == dict.fromkeys(policy.head_names, 1.0)
    assert contract.weight_input_version(header, 2166) == version
    # Fixture generation must not alter the training metadata globally.
    assert contract.data.PREPROCESSING["navdemo_version"] == 2
    assert contract.data.PREPROCESSING["roads_schema"] == "carrot_v2"


@pytest.mark.parametrize("weight_version", [1, 2])
def test_numpy_softmax_uses_each_head_temperature(tmp_path, weight_version):
    weights = tmp_path / "fixture.navbc"
    sizes = contract.HEAD_SIZES[weight_version]
    temperatures = {name: .5 + index for index, name in enumerate(sizes)}
    contract.synthetic_weights(weights, format_version=weight_version, temperatures=temperatures)
    policy = contract.NumpyPolicy(weights)
    logits = {name: np.arange(classes, dtype=np.float32) for name, classes in sizes.items()}
    for name, actual in policy.probabilities(logits).items():
        expected = np.exp((logits[name].astype(np.float64) - logits[name].max()) / temperatures[name])
        np.testing.assert_allclose(actual, expected / expected.sum(), rtol=1e-14)


@pytest.mark.parametrize("action", [0, 1, 2])
def test_numpy_v2_decodes_one_mutually_exclusive_maneuver(tmp_path, action):
    weights = tmp_path / "fixture.navbc"
    contract.synthetic_weights(weights)
    policy = contract.NumpyPolicy(weights)
    logits = {name: np.zeros(classes, dtype=np.float32) for name, classes in policy.head_sizes.items()}
    logits["maneuver"][:] = -1000
    logits["maneuver"][action] = 1000
    keys = policy.sample(logits, np.random.default_rng(3))
    assert keys["jump"] is (action == 1)
    assert keys["climb"] is (action == 2)


@pytest.mark.parametrize("bad_value", [0, -1, float("nan"), float("inf")])
def test_numpy_rejects_invalid_exported_temperature(tmp_path, bad_value):
    weights = tmp_path / "fixture.navbc"
    temperatures = dict.fromkeys(contract.HEAD_SIZES[2], 1.0)
    temperatures["maneuver"] = bad_value
    contract.synthetic_weights(weights, temperatures=temperatures)
    with pytest.raises(ValueError, match="temperatures"):
        contract.NumpyPolicy(weights)


def test_numpy_rejects_v2_legacy_head_mapping(tmp_path):
    weights = tmp_path / "fixture.navbc"
    contract.synthetic_weights(weights)
    with weights.open("rb") as stream:
        header, payload = json.loads(stream.readline()), stream.read()
    header["architecture"]["heads"]["maneuver"]["values"] = [-1, 0, 1]
    weights.write_bytes(json.dumps(header).encode() + b"\n" + payload)
    with pytest.raises(ValueError, match="maneuver"):
        contract.NumpyPolicy(weights)


@pytest.mark.parametrize("version", [1, 2])
def test_fixture_features_preserve_each_versions_road_values(version):
    layout, samples = contract.synthetic_records(1, version)
    record = next(samples)
    roads = record["roads"]
    target, distance, goal, goal_distance, remainder, flag = contract.ROAD_FIELDS[version]
    roads[target] = [.25, -.5, .75]
    roads[distance] = 1500
    roads[goal] = [-.1, .2, .3]
    roads[goal_distance] = 60000
    roads[remainder] = 1750
    roads[flag] = 1
    original = record.copy()
    features = contract.fixture_features(record, layout, version)
    offset = contract.demos.RAY_COUNT * 9
    np.testing.assert_array_equal(features["vector"][0, offset:offset + 10],
                                  np.array([.25, -.5, .75, 1.5, -.1, .2, .3, 50, 1.75, 1], dtype=np.float32))
    np.testing.assert_array_equal(record, original)
    if version == 1:
        with pytest.raises(ValueError, match="requires NAVDEMO v2"):
            contract.data.encode_features(contract.demos.learner_input(record), layout)
    with pytest.raises(ValueError, match="record dtype"):
        contract.fixture_features(record, layout, 3 - version)


def test_supplied_weights_require_matching_vector_size_and_version_schema():
    header = {"preprocessing": contract.fixture_preprocessing(2)}
    with pytest.raises(ValueError, match="vector_size=2166"):
        contract.weight_input_version(header, 2174)
    with pytest.raises(ValueError, match="roads_schema"):
        contract.weight_input_version({"preprocessing": {**header["preprocessing"], "navdemo_version": 1}}, 2166)
    legacy_header = {"preprocessing": {"vector_order": ["eye", "roads", "body"]}}
    assert contract.weight_input_version(legacy_header, 2166) == 1


def test_recorded_fixture_filters_navdemo_version(tmp_path):
    for version in (1, 2):
        folder = tmp_path / str(version)
        folder.mkdir()
        layout, samples = contract.synthetic_records(2, version)
        (folder / "episode.json").write_text(json.dumps({"decisions": 2, "eye_layout": layout.tolist()}))
        with gzip.open(folder / "samples.bin.gz", "wb") as stream:
            demos = contract.demos
            stream.write(demos.HEADER.pack(demos.MAGIC, version, demos.RECORD_BYTES, demos.RAY_COUNT,
                                           demos.NEAR_SIZE, demos.FAR_SIZE, demos.PERIOD_MS))
            for record in samples:
                stream.write(record.tobytes())
    source, _, samples = contract.recorded_steps(tmp_path, 2, 2)
    assert source.parent.name == "2"
    assert all(sample.dtype == contract.demos.RECORD_V2_DTYPE for sample in samples)
    with pytest.raises(ValueError, match="NAVDEMO v2"):
        contract.recorded_steps(tmp_path / "1", 2, 2)


@pytest.mark.parametrize("weight_version, recording_version", [(3, 2), (4, 3)])
def test_hd_synthetic_weight_header_declares_full_resolution_maps(tmp_path, weight_version, recording_version):
    weights = tmp_path / "fixture.navbc"
    contract.synthetic_weights(weights, navdemo_version=recording_version, format_version=weight_version)
    policy = contract.NumpyPolicy(weights)
    header = policy.header
    channels = 11 if weight_version == 4 else 10
    assert header["version"] == weight_version
    assert header["architecture"]["inputs"]["near"] == [channels, 64, 64]
    assert header["architecture"]["inputs"]["far"] == [channels, 50, 100]
    assert header["architecture"]["map_encoders"]["layers"][0]["in_channels"] == channels
    assert header["preprocessing"] == (contract.data.BC4_PREPROCESSING if weight_version == 4 else contract.data.HD_PREPROCESSING)
    assert contract.weight_input_version(header, 2166) == recording_version
    assert contract.weight_input_variant(header) == ("hd-familiar" if weight_version == 4 else "hd")
    assert policy.head_names == tuple(contract.HEAD_SIZES[2])
    first_conv = policy.weights["near_encoder.0.weight"]
    assert first_conv.shape == (8, channels, 3, 3)
    with pytest.raises(ValueError, match="requires NAVDEMO v2"):
        contract.synthetic_weights(tmp_path / "invalid.navbc", navdemo_version=1, format_version=3)


def test_weight_input_variant_preserves_old_headers_and_requires_declared_hd():
    assert contract.weight_input_variant({"version": 1}) == "compact"
    assert contract.weight_input_variant({"version": 2}) == "compact"
    assert contract.weight_input_variant({"version": 3, "preprocessing": {"input_variant": "hd"}}) == "hd"
    with pytest.raises(ValueError, match="requires hd input_variant"):
        contract.weight_input_variant({"version": 3})
    with pytest.raises(ValueError, match="requires compact input_variant"):
        contract.weight_input_variant({"version": 2, "preprocessing": {"input_variant": "hd"}})


@pytest.mark.parametrize("recording_version, input_variant, channels", [(2, "hd", 10), (3, "hd-familiar", 11)])
def test_hd_fixture_uses_physical_feet_and_preserves_full_raw_record(recording_version, input_variant, channels):
    layout, samples = contract.synthetic_records(1, recording_version)
    record = next(samples)
    record["position_cm"] = [3100, 700, -4900]
    for name in ("memory_near", "memory_far"):
        record[name]["height_valid"] = 0
        record[name]["ground_height_cm"] = 0
    near = record["memory_near"]
    near["height_valid"][0, 2, 3] = 1
    near["ground_height_cm"][0, 2, 3] = 940
    far = record["memory_far"]
    far["height_valid"][0, 49, 25] = 1
    far["ground_height_cm"][0, 49, 25] = 2000
    far["height_valid"][0, 50, 25] = 1
    far["ground_height_cm"][0, 50, 25] = 600
    far["height_valid"][0, 99, 99] = 1
    far["ground_height_cm"][0, 99, 99] = 900
    original = record.copy()
    features = contract.fixture_features(record, layout, recording_version, input_variant)
    assert features["near"].shape == (1, channels, 64, 64)
    assert features["far"].shape == (1, channels, 50, 100)
    np.testing.assert_allclose(features["near"][0, 7:10, 2, 3], [1.2, 1.2, 1])
    np.testing.assert_array_equal(features["far"][0, 7:10, 0, 25], [-.5, -.5, 1])
    np.testing.assert_array_equal(features["far"][0, 7:10, 49, 99], [1, 1, 1])
    np.testing.assert_array_equal(record, original)

    translated = record.copy()
    translated["position_cm"] += [12345, 300, -23456]
    translated["body_yaw_deg"] = -170
    translated["teacher_hint"]["path_cm"] = 88888
    for name in ("memory_near", "memory_far"):
        memory = translated[name]
        memory["ground_height_cm"][memory["height_valid"] != 0] += 300
    moved = contract.fixture_features(translated, layout, recording_version, input_variant)
    for name in features:
        np.testing.assert_array_equal(features[name], moved[name])
