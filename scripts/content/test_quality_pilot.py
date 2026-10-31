"""Pilot geometry and admission must not hide a wrong generated rhythm."""
import numpy as np
from PIL import Image
import pytest
from types import SimpleNamespace

import quality_pilot


def test_roof_square_modules_keep_three_supports_and_native_proportions():
    source = np.full((256, 256, 3), 140, dtype=np.uint8)
    # One centred vertical timber; twenty narrow horizontal board joints.
    source[:, 124:132] = 30
    for y in np.linspace(0, 255, 20, endpoint=False).astype(int):
        source[y:y+2] = 65
    canvas, geometry = quality_pilot.roof_canvas(Image.fromarray(source))
    assert canvas.size == (1536, 681)
    assert geometry['source_to_canvas_stretch'] == 1
    assert geometry['source_to_physical_stretch'] < 1.25
    profile = np.asarray(canvas).mean(axis=(0, 2))
    for centre in geometry['support_centres']:
        assert profile[centre] < profile[centre+60]*.5
    # A square source is a hard requirement, not silently stretched.
    with pytest.raises(ValueError, match='square module'):
        quality_pilot.roof_canvas(Image.new('RGB', (512, 256)))


def test_admission_uses_fixed_plan_scale_and_every_known_axis(monkeypatch):
    measured = dict(period_u_uv=.2, period_v_uv=.2, direction='U',
        period_u_confidence=1, period_v_confidence=1)
    monkeypatch.setattr(quality_pilot, 'measure_material', lambda image: measured)
    plan = dict(scale='3.2', tile_metres='1.875',
        planned_detail_u_metres='.375', planned_detail_v_metres='.375')
    mapping = dict(target_direction='U')
    result = quality_pilot.scale_measurement('material', None, mapping, plan)
    assert result['scale'] == 3.2
    assert result['scale_pass'] and result['direction_pass']
    measured['period_v_uv'] = .4
    result = quality_pilot.scale_measurement('material', None, mapping, plan)
    assert result['scale'] == 3.2
    assert not result['scale_pass']
    measured.update(period_v_uv=0, direction='V')
    result = quality_pilot.scale_measurement('material', None, mapping, plan)
    assert result['scale_status'] == 'missing_period'
    assert not result['direction_pass']


def test_build_cannot_rewrite_prepared_iid_input(tmp_path):
    target = tmp_path/'B.png'
    image = Image.new('RGB', (16, 16), '#776655')
    expected = quality_pilot.prepared_png(image, target, frozen=False)
    before = target.read_bytes()
    assert quality_pilot.prepared_png(image, target, frozen=True) == expected
    with pytest.raises(ValueError, match='differs from prepared IID input'):
        quality_pilot.prepared_png(Image.new('RGB', image.size, 'white'), target, frozen=True)
    assert target.read_bytes() == before


def test_build_rejects_live_derived_root_before_writing(tmp_path):
    args = SimpleNamespace(command='build', city3=tmp_path/'city-3',
        prepared_dir=tmp_path/'prepared', source_root=tmp_path/'sources',
        derived_root=quality_pilot.BASE/'content', report_dir=tmp_path/'report',
        registry_output=tmp_path/'pilot.jsonl', base_registry=tmp_path/'base.jsonl',
        staging_parent=tmp_path/'staging')
    with pytest.raises(ValueError, match='outside the live'):
        quality_pilot.validate_outputs(args)
    assert not args.prepared_dir.exists()
