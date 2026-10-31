"""The IID gate must preserve unflagged albedo and blend physical light values."""
from types import SimpleNamespace

import numpy as np
from PIL import Image
import pytest

import quality_pilot_2


def test_half_strength_iid_mixes_radiance_instead_of_encoded_brightness():
    black = Image.new('RGB', (16, 16), 'black')
    white = Image.new('RGB', (16, 16), 'white')
    result = np.asarray(quality_pilot_2.blend_linear_half(black, white))
    # Half the radiance is sRGB 188; a blend of PNG values would be 128.
    assert np.all(result == 188)
    assert np.array_equal(result, np.asarray(quality_pilot_2.blend_linear_half(white, black)))
    with pytest.raises(ValueError, match='dimensions'):
        quality_pilot_2.blend_linear_half(black, Image.new('RGB', (32, 16)))


def test_unflagged_material_never_reads_or_changes_iid(tmp_path):
    maps = {'albedo': np.full((16, 16, 3), .4, dtype=np.float32)}
    before = maps['albedo'].tobytes()
    record = dict(replaces=['NW_CITY_ROOF_01.TGA'], _maps=maps,
        generation=dict(detector_before=dict(lighting_risk=False)))
    # No IID paths or output log are available. The detector gate must suffice.
    quality_pilot_2.apply_iid(record, SimpleNamespace(), {})
    assert record['_maps'] is maps
    assert maps['albedo'].tobytes() == before
    assert record['generation']['marigold_applied'] is False
    assert not list(tmp_path.iterdir())


def test_flagged_material_rejects_iid_from_different_prepared_bytes(tmp_path):
    output = tmp_path / 'NW_CITY_ROOF_01-albedo.png'
    Image.new('RGB', (16, 16), '#777777').save(output)
    record = dict(replaces=['NW_CITY_ROOF_01.TGA'],
        generation=dict(detector_before=dict(lighting_risk=True), prepared_sha256='a' * 64))
    args = SimpleNamespace(marigold_dir=tmp_path, marigold_log=tmp_path / 'iid.jsonl')
    iid_log = {'NW_CITY_ROOF_01': dict(source_sha256='b' * 64,
        output_sha256=quality_pilot_2.pilot.file_digest(output))}
    with pytest.raises(ValueError, match='does not match prepared input'):
        quality_pilot_2.apply_iid(record, args, iid_log)
    assert record['generation']['marigold_applied'] is False
    assert set(tmp_path.iterdir()) == {output}
