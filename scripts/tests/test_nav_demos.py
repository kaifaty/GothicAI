"""Asset-free NAVDEMO compatibility and sensor-input checks."""

import gzip
import json
import lzma
from pathlib import Path
import sys

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import nav_demos as demos


def write_episode(path, version, records=None):
    path.mkdir(parents=True)
    if records is None:
        records = np.zeros(3, dtype=demos.RECORD_DTYPES[version])
        records['time_ms'] = [100, 200, 300]
        records['keys']['forward'] = 1
        records['keys']['walk'] = 1
        records['previous_keys'][1:] = records['keys'][:-1]
    with gzip.open(path / 'samples.bin.gz', 'wb') as output:
        output.write(demos.HEADER.pack(demos.MAGIC, version, demos.RECORD_BYTES_BY_VERSION[version],
                                      demos.RAY_COUNT, demos.NEAR_SIZE, demos.FAR_SIZE,
                                      demos.PERIOD_MS))
        output.write(records.tobytes())
    metadata = dict(decisions=len(records), elapsed_ms=250, start='A', goal='B',
                    success=True, eye_layout=[[0, 0, 4000]] * demos.RAY_COUNT)
    if version == 3:
        metadata['navigation_memory_version'] = 4
    (path / 'episode.json').write_text(json.dumps(metadata))
    actions = np.zeros((len(records) + 1, 9))
    if len(records):
        actions[:-1, 0] = np.arange(len(records)) * demos.PERIOD_MS
        for column, name in enumerate(('forward', 'strafe', 'turn', 'walk', 'jump', 'climb',
                                        'head_yaw_deg', 'head_pitch_deg'), start=1):
            actions[:-1, column] = records['keys'][name]
    actions[-1, [0, 4]] = [250, 1]
    np.savetxt(path / 'actions.txt', actions)
    return path


@pytest.mark.parametrize('version', [1, 2, 3])
def test_binary_round_trip_selects_road_schema_and_keeps_layout(version, tmp_path):
    records = np.zeros(3, dtype=demos.RECORD_DTYPES[version])
    direction, distance = (('next_direction', 'next_distance_cm') if version == 1 else
                           ('carrot_direction', 'carrot_distance_cm'))
    records['roads'][direction] = [0, 0, 1]
    records['roads'][distance] = [600, 580, 560]
    records['teacher_hint']['path_cm'] = [1000, 990, 980]
    path = write_episode(tmp_path / f'v{version}', version, records)
    chunks = list(demos.iter_chunks(path, chunk_records=2))
    assert [len(chunk) for chunk in chunks] == [2, 1]
    assert demos.episode_version(path) == version
    assert chunks[0].dtype == demos.RECORD_DTYPES[version]
    assert chunks[0].dtype.itemsize == demos.RECORD_BYTES_BY_VERSION[version]
    np.testing.assert_array_equal(np.concatenate(chunks), records)
    np.testing.assert_array_equal(demos.load_episode(path), records)


@pytest.mark.parametrize('version', [2, 3])
def test_xz_preserves_record_bits_and_prefers_compact_discovery(version, tmp_path):
    path = write_episode(tmp_path / f'v{version}', version)
    original = path / 'samples.bin.gz'
    with gzip.open(original, 'rb') as stream:
        legacy = stream.read()
    rng = np.random.default_rng(17)
    record_bits = rng.integers(0, 256, 3 * demos.RECORD_BYTES_BY_VERSION[version], dtype=np.uint8).tobytes()
    compact = path / 'samples.bin.xz'
    compact.write_bytes(lzma.compress(legacy[:demos.HEADER.size] + record_bits, preset=6))

    assert demos.samples_path(path) == compact
    assert demos.samples_path(original) == original
    assert demos.episode_paths(path) == [compact]
    assert demos.episode_paths(tmp_path) == [compact]
    assert demos.episode_version(compact) == version
    chunks = list(demos.iter_chunks(path, chunk_records=2))
    assert [len(chunk) for chunk in chunks] == [2, 1]
    assert b''.join(chunk.tobytes() for chunk in chunks) == record_bits
    assert demos.load_episode(path).tobytes() == record_bits
    assert demos.load_episode(original).tobytes() == legacy[demos.HEADER.size:]


def test_v1_aliases_keep_existing_training_schema():
    assert demos.VERSION == 1
    assert demos.RECORD_DTYPE == demos.RECORD_DTYPES[1]
    assert demos.RECORD_BYTES == 173838
    assert demos.CELL_DTYPE.itemsize == 12
    assert demos.CHANNELS == ('confidence', 'free', 'wall', 'ledge', 'drop', 'water', 'movable')
    assert demos.ROADS_DTYPE.names == ('next_direction', 'next_distance_cm', 'goal_direction',
                                     'goal_distance_cm', 'remaining_visual_cm', 'goal_in_known')
    for name in demos.RECORD_DTYPE.names:
        assert demos.RECORD_DTYPE.fields[name][1] == demos.RECORD_V2_DTYPE.fields[name][1]


def test_v3_round_trip_keeps_familiarity_separate_from_live_confidence(tmp_path):
    records = np.zeros(3, dtype=demos.RECORD_V3_DTYPE)
    records['memory_far']['channels'][:, 7, 12, 0] = 140
    records['memory_far']['channels'][:, 7, 12, 2] = 140
    records['memory_far']['channels'][:, 7, 12, 7] = 255
    records['memory_far']['ground_height_cm'][:, 7, 12] = 123.5
    records['memory_far']['height_valid'][:, 7, 12] = 1
    records['teacher_hint']['path_cm'] = [1000, 990, 980]
    path = write_episode(tmp_path / 'v3', 3, records)
    loaded = demos.load_episode(path)
    assert demos.CHANNELS_V3 == demos.CHANNELS + ('familiar',)
    assert loaded.dtype.itemsize == 187934
    assert loaded['roads'].dtype == demos.ROADS_V2_DTYPE
    assert loaded['memory_far'].dtype == demos.CELL_V3_DTYPE
    np.testing.assert_array_equal(loaded, records)
    np.testing.assert_array_equal(loaded['memory_far']['channels'][:, 7, 12, [0, 7]],
                                  [[140, 255]] * 3)
    assert demos.episode_memory_version(path) == 4


def test_v2_learner_receives_carrot_fields_without_teacher_debug():
    records = np.zeros(2, dtype=demos.RECORD_V2_DTYPE)
    records['roads']['carrot_direction'] = [1, 0, 0]
    records['roads']['carrot_distance_cm'] = 600
    records['roads']['remaining_known_cm'] = [1400, 1380]
    records['roads']['goal_direction'] = [0, 0, 1]
    records['roads']['goal_distance_cm'] = 1500
    records['roads']['beyond_known'] = [0, 1]
    inputs = demos.learner_input(records)
    assert set(inputs) == {'body', 'roads', 'previous_keys', 'eye', 'memory_near', 'memory_far'}
    assert inputs['roads'].dtype.names == ('carrot_direction', 'carrot_distance_cm',
                                         'goal_direction', 'goal_distance_cm',
                                         'remaining_known_cm', 'beyond_known')
    np.testing.assert_array_equal(inputs['roads'], records['roads'])
    assert np.shares_memory(inputs['roads'], records)


@pytest.mark.parametrize('version', [1, 2, 3])
def test_check_validates_each_version_and_reports_its_version(version, tmp_path):
    path = write_episode(tmp_path / f'v{version}', version)
    summary = demos.summarize(path, validate=True)
    assert summary['validated'] is True
    assert summary['format_version'] == version
    assert summary['format_versions'] == [version]
    assert summary['decisions'] == 3
    assert summary['episodes'] == 1
    assert summary['record_bytes'] == demos.RECORD_BYTES_BY_VERSION[version]
    assert summary['uncompressed_bytes'] == 3 * demos.RECORD_BYTES_BY_VERSION[version] + demos.HEADER.size


def test_mixed_dataset_is_not_reported_as_one_version(tmp_path):
    write_episode(tmp_path / 'v1', 1)
    write_episode(tmp_path / 'v2', 2)
    summary = demos.summarize(tmp_path, validate=True)
    assert summary['format_version'] is None
    assert summary['format_versions'] == [1, 2]
    assert summary['decisions'] == 6


def test_mixed_record_sizes_are_summed_per_episode(tmp_path):
    write_episode(tmp_path / 'v2', 2)
    write_episode(tmp_path / 'v3', 3)
    summary = demos.summarize(tmp_path)
    assert summary['format_versions'] == [2, 3]
    assert summary['record_bytes'] is None
    assert summary['uncompressed_bytes'] == 3 * (173838 + 187934) + 2 * demos.HEADER.size


def test_missing_memory_tag_is_legacy_without_rewriting_episode(tmp_path):
    path = write_episode(tmp_path / 'legacy', 2)
    metadata_path = path / 'episode.json'
    original = metadata_path.read_bytes()
    assert demos.episode_memory_version(path) == 1
    summary = demos.summarize(path, validate=True)
    assert summary['navigation_memory_version'] == 1
    assert summary['navigation_memory_versions'] == [1]
    assert metadata_path.read_bytes() == original


def test_memory_versions_are_reported_independently_of_binary_layout(tmp_path):
    write_episode(tmp_path / 'legacy', 2)
    current = write_episode(tmp_path / 'local-terrain', 2)
    metadata_path = current / 'episode.json'
    metadata = json.loads(metadata_path.read_text())
    metadata['navigation_memory_version'] = 2
    metadata_path.write_text(json.dumps(metadata))
    assert demos.episode_memory_version(current) == 2
    summary = demos.summarize(tmp_path)
    assert summary['format_version'] == 2
    assert summary['navigation_memory_version'] is None
    assert summary['navigation_memory_versions'] == [1, 2]
    assert {value['navigation_memory_version'] for value in summary['episode_details']} == {1, 2}
    with pytest.raises(ValueError, match='mixed navigation memory versions:'):
        demos.summarize(tmp_path, validate=True)


@pytest.mark.parametrize('version', [0, -1, True, '2', 2.0, None])
def test_invalid_memory_version_is_rejected(tmp_path, version):
    path = write_episode(tmp_path / 'invalid', 2)
    metadata_path = path / 'episode.json'
    metadata = json.loads(metadata_path.read_text())
    metadata['navigation_memory_version'] = version
    metadata_path.write_text(json.dumps(metadata))
    with pytest.raises(ValueError, match='invalid navigation_memory_version'):
        demos.summarize(path)


@pytest.mark.parametrize(('field', 'value', 'error'), [
    ('beyond_known', 2, 'not a bit'),
    ('carrot_distance_cm', -1, 'negative roads.carrot_distance_cm'),
    ('remaining_known_cm', -1, 'negative roads.remaining_known_cm'),
    ('carrot_direction', [2, 0, 0], 'not unit or zero'),
    ('carrot_direction', [float('nan'), 0, 0], 'NaN or infinite'),
])
def test_v2_check_rejects_invalid_road_inputs(field, value, error):
    records = np.zeros(1, dtype=demos.RECORD_V2_DTYPE)
    records['roads'][field] = value
    with pytest.raises(ValueError, match=error):
        demos._validate_chunk(records, np.array([[0, 0, 4000]] * demos.RAY_COUNT))


@pytest.mark.parametrize('version', [2, 3])
def test_empty_episode_keeps_its_schema(tmp_path, version):
    path = write_episode(tmp_path / 'empty', version, np.zeros(0, dtype=demos.RECORD_DTYPES[version]))
    records = demos.load_episode(path)
    assert len(records) == 0
    assert records.dtype == demos.RECORD_DTYPES[version]


@pytest.mark.parametrize('version', [0, 4])
def test_unknown_header_version_is_rejected(tmp_path, version):
    path = tmp_path / 'samples.bin.gz'
    with gzip.open(path, 'wb') as output:
        output.write(demos.HEADER.pack(demos.MAGIC, version, demos.RECORD_BYTES,
                                      demos.RAY_COUNT, demos.NEAR_SIZE, demos.FAR_SIZE,
                                      demos.PERIOD_MS))
    with pytest.raises(ValueError, match='unsupported navigation demo header'):
        list(demos.iter_chunks(path))


@pytest.mark.parametrize(('version', 'record_bytes'), [(1, 187934), (2, 187934), (3, 173838)])
def test_record_size_must_match_header_version(tmp_path, version, record_bytes):
    path = tmp_path / 'samples.bin.gz'
    with gzip.open(path, 'wb') as output:
        output.write(demos.HEADER.pack(demos.MAGIC, version, record_bytes,
                                      demos.RAY_COUNT, demos.NEAR_SIZE, demos.FAR_SIZE,
                                      demos.PERIOD_MS))
    with pytest.raises(ValueError, match='unsupported navigation demo header'):
        list(demos.iter_chunks(path))


@pytest.mark.parametrize('version', [2, 3])
def test_truncated_record_is_rejected(tmp_path, version):
    path = write_episode(tmp_path / f'v{version}', version)
    with gzip.open(path / 'samples.bin.gz', 'ab') as output:
        output.write(b'\0')
    with pytest.raises(ValueError, match='truncated record'):
        list(demos.iter_chunks(path))
