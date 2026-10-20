"""City-4 keeps the exact text scope, raw bytes and pilot-2 admission gates."""
from copy import deepcopy
import hashlib
import json
from pathlib import Path
from types import SimpleNamespace

import numpy as np
from PIL import Image
import pytest

import city4


def source(directory, name='NW_MISC_OLDSTONEPATH_01.TGA', attempt=1, mode='RGB'):
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / f'{Path(name).stem}-attempt-{attempt}.png'
    Image.new(mode, (16, 16), '#776655').save(path)
    metadata = dict(texture=name, attempt=attempt, prompt='text-only material',
        utc='2026-10-20T12:00:00Z', seconds=1.25, tool='image_gen.imagegen',
        source_sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    path.with_suffix('.json').write_text(json.dumps(metadata))
    return path


def test_scope_counts_and_module_parts_come_only_from_csv():
    records = city4.recipes()
    assert len(records) == 58
    names = [record['replaces'][0] for record in records]
    assert len([name for name in names if name.startswith('NW_MISC_')]) == 7
    assert not set(names) & set(city4.pilot.TEXTURES)
    measured = {row['texture']: row for row in city4.rows(city4.ROOT/'data/content/city-materials.csv')}
    for record in records:
        row = measured[record['replaces'][0]]
        params = record['parameters']
        assert params['scale'] == 1
        assert params['physical_uv_metres'] == [float(row['uv_m_u']), float(row['uv_m_v'])]
        assert 'Lighting: even shadowless diffuse illumination, identical exposure from corner to corner.' in record['prompt']
        for axis in ('u', 'v'):
            assert params['detail_count_target'][axis] == (float(row['count_' + axis]) if row['count_' + axis] else None)
            if row['count_' + axis]:
                assert f'{float(row["count_" + axis]):g} details across {axis.upper()}, each {float(row["element_cm_" + axis]):g} cm along {axis.upper()}' in record['prompt']
        if row['decision'] != 'include':
            assert f'Subject: {row["parts"]}.' in record['prompt']
            assert f'parts: {row["parts"]}' in record['prompt']
    with pytest.raises(ValueError, match='outside the 58'):
        city4.recipes(textures=[city4.pilot.TEXTURES[0]])


def test_strip_seams_use_uv_boundaries_instead_of_detail_direction():
    expected = {
        'NW_HARBOUR_WOOD_IRON_01.TGA': ('v', 'U'),
        'NW_CITY_WOOD_ORNAMENT_02.TGA': ('u', 'V'),
        'NW_MISC_STRONG_STONEWALL_BLOCKBORDER_01.TGA': ('u', 'U'),
        'NW_MISC_STRONG_STONEWALL_ROUNDBORDER_01.TGA': ('u', 'U'),
    }
    for record in city4.recipes(textures=expected):
        params = record['parameters']
        assert (params['tiling_axes'], params['count_direction']) == expected[record['replaces'][0]]


def test_sources_keep_png_and_metadata_bytes_and_reject_rewriting(tmp_path):
    name = 'NW_MISC_OLDSTONEPATH_01.TGA'
    image = source(tmp_path/'input', name)
    raw, metadata = image.read_bytes(), image.with_suffix('.json').read_bytes()
    store = tmp_path/'store'
    attempts = city4.read_sources(tmp_path/'input', [name], store)
    assert attempts[name][0]['source_sha256'] == hashlib.sha256(raw).hexdigest()
    assert (store/'city-4'/image.name).read_bytes() == raw
    assert (store/'city-4'/image.with_suffix('.json').name).read_bytes() == metadata
    Image.new('RGB', (16, 16), '#112233').save(image)
    with pytest.raises(ValueError, match='source_sha256 differs'):
        city4.read_sources(tmp_path/'input', [name], store)


def test_third_attempt_and_opaque_alpha_are_rejected(tmp_path):
    source(tmp_path/'input', attempt=3)
    with pytest.raises(ValueError, match='third attempt'):
        city4.read_sources(tmp_path/'input', ['NW_MISC_OLDSTONEPATH_01.TGA'], tmp_path/'store')
    name = 'NW_CITY_FLAG_01.TGA'
    source(tmp_path/'alpha', name, mode='RGBA')
    with pytest.raises(ValueError, match='transparent gaps'):
        city4.read_sources(tmp_path/'alpha', [name], tmp_path/'store')


def test_visual_admission_uses_only_known_counts_and_twenty_percent_tolerance():
    record = city4.recipes(textures=['NW_CITY_BODENDIELEN_02.TGA'])[0]
    assert city4.visual_measurement(record, dict(v=6))['visual_count_pass']
    assert not city4.visual_measurement(record, dict(v=6.01))['visual_count_pass']
    with pytest.raises(ValueError, match='positive and finite'):
        city4.visual_measurement(record, dict(v=True))
    ground = city4.recipes(textures=['NW_CITY_DIRTYGROUND_02.TGA'])[0]
    with pytest.raises(ValueError, match='inspection note'):
        city4.visual_measurement(ground, {})
    assert city4.visual_measurement(ground, dict(note='fine nonperiodic mud'))['visual_count_pass']


def test_unflagged_and_alpha_materials_do_not_read_iid():
    record = dict(replaces=['NW_CITY_FLAG_01.TGA'],
        generation=dict(detector_before=dict(lighting_risk=False)),
        _maps={'albedo': np.ones((16, 16, 4))})
    before = record['_maps']
    city4.apply_iid(record, SimpleNamespace(), {})
    assert record['_maps'] is before
    assert record['generation']['marigold_applied'] is False
    record['generation']['detector_before']['lighting_risk'] = True
    with pytest.raises(ValueError, match='excluded for alpha'):
        city4.apply_iid(record, SimpleNamespace(), {})


def test_failed_first_visual_attempt_requires_retry_before_output(tmp_path):
    record = city4.recipes(textures=['NW_CITY_BROKENSTONE_01.TGA'])[0]
    record['generation'] = dict(selected_attempt=1)
    counts = tmp_path/'counts.json'
    counts.write_text(json.dumps({record['replaces'][0]: dict(u=10, v=12)}))
    args = SimpleNamespace(visual_counts=counts, marigold_log=None)
    with pytest.raises(ValueError, match='second attempt required'):
        city4.build(args, [record])
    assert list(tmp_path.iterdir()) == [counts]


def test_iid_checks_input_and_retains_output_in_city4_namespace(tmp_path, monkeypatch):
    record = city4.recipes(textures=['NW_MISC_OLDSTONEPATH_01.TGA'])[0]
    name = record['replaces'][0]
    prepared, output, store = tmp_path/'prepared', tmp_path/'iid', tmp_path/'store'
    prepared.mkdir()
    output.mkdir()
    Image.new('RGB', (16, 16), '#888888').save(prepared/(Path(name).stem+'.png'))
    iid = output/(Path(name).stem+'-albedo.png')
    Image.new('RGB', (16, 16), '#999999').save(iid)
    prepared_hash = city4.pilot.file_digest(prepared/(Path(name).stem+'.png'))
    record['generation'] = dict(detector_before=dict(lighting_risk=True), prepared_sha256=prepared_hash)
    args = SimpleNamespace(prepared_dir=prepared, marigold_dir=output,
        source_root=store, marigold_log=tmp_path/'log.jsonl')
    log = {Path(name).stem: dict(source_sha256='a'*64, output_sha256=city4.pilot.file_digest(iid), seconds=2)}
    with pytest.raises(ValueError, match='does not match prepared'):
        city4.apply_iid(deepcopy(record), args, log)
    assert not store.exists()
    log[Path(name).stem]['source_sha256'] = prepared_hash
    monkeypatch.setattr(city4.generate, 'target_corrected_maps',
        lambda image, record: ({'albedo': np.asarray(image)/255}, {'linear_rgb_max_error': .001}, None))
    city4.apply_iid(record, args, log)
    generation = record['generation']
    assert generation['marigold_applied'] is True
    assert generation['iid_source']['source_png'].startswith('city-4/marigold/')
    assert (store/generation['iid_source']['source_png']).read_bytes() == iid.read_bytes()
    assert record['parameters']['iid_linear_weight'] == .5
