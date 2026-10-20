"""City-4 retains source histories and fits the full material scope uniformly."""
from copy import deepcopy
import hashlib
import json
from pathlib import Path
from types import SimpleNamespace

import numpy as np
from PIL import Image
import pytest

import city4


def source(directory, name='NW_MISC_OLDSTONEPATH_01.TGA', attempt=1, mode='RGB', colour='#776655'):
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / f'{Path(name).stem}-attempt-{attempt}.png'
    Image.new(mode, (16, 16), colour).save(path)
    metadata = dict(texture=name, attempt=attempt, prompt='text-only material',
        utc='2026-10-20T12:00:00Z', seconds=1.25, tool='image_gen.imagegen',
        source_sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    path.with_suffix('.json').write_text(json.dumps(metadata))
    return path


def test_scope_counts_and_module_parts_come_only_from_csv():
    records = city4.recipes()
    assert len(records) == 65
    names = [record['replaces'][0] for record in records]
    assert len([name for name in names if name.startswith('NW_MISC_')]) == 7
    assert set(city4.pilot.TEXTURES) <= set(names)
    measured = {row['texture']: row for row in city4.rows(city4.ROOT/'data/content/city-materials.csv')}
    for record in records:
        row = measured[record['replaces'][0]]
        params = record['parameters']
        assert params['scale'] == 1
        assert params['detail_ceiling'] is True
        assert params['detail_targets'] == {
            field: float(row['target_' + field]) for field in city4.FIELDS}
        assert params['physical_uv_metres'] == [float(row['uv_m_u']), float(row['uv_m_v'])]
        assert 'Lighting: even shadowless diffuse illumination, identical exposure from corner to corner.' in record['prompt']
        for axis in ('u', 'v'):
            assert params['detail_count_target'][axis] == (float(row['count_' + axis]) if row['count_' + axis] else None)
            if row['count_' + axis]:
                assert f'{float(row["count_" + axis]):g} details across {axis.upper()}, each {float(row["element_cm_" + axis]):g} cm along {axis.upper()}' in record['prompt']
        if row['decision'] != 'include':
            assert f'Subject: {row["parts"]}.' in record['prompt']
            assert f'parts: {row["parts"]}' in record['prompt']
    with pytest.raises(ValueError, match='outside the 65'):
        city4.recipes(textures=['NW_UNKNOWN_01.TGA'])


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
    updated = json.loads(image.with_suffix('.json').read_text())
    updated['source_sha256'] = hashlib.sha256(image.read_bytes()).hexdigest()
    image.with_suffix('.json').write_text(json.dumps(updated))
    with pytest.raises(ValueError, match='refusing to overwrite retained file'):
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


def test_unflagged_and_alpha_materials_do_not_read_iid(tmp_path):
    prepared = tmp_path / 'NW_CITY_FLAG_01.png'
    Image.new('RGB', (16, 16), '#776655').save(prepared)
    record = dict(replaces=['NW_CITY_FLAG_01.TGA'],
        generation=dict(detector_before=dict(lighting_risk=False),
            prepared_sha256=city4.pilot.file_digest(prepared)),
        _maps={'albedo': np.ones((16, 16, 4))})
    before = record['_maps']
    pixels = city4.apply_iid(record, SimpleNamespace(prepared_dir=tmp_path), {})
    assert np.array_equal(pixels[0, 0], [0x77, 0x66, 0x55])
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
    called = []
    def low_frequency(before, after):
        called.append((before, after))
        return np.full((16, 16, 3), 77, dtype=np.uint8), {'gaussian_sigma_px': 32}
    monkeypatch.setattr(city4, 'low_frequency_iid', low_frequency)
    pixels = city4.apply_iid(record, args, log)
    generation = record['generation']
    assert generation['marigold_applied'] is True
    assert generation['iid_source']['source_png'].startswith('city-4/marigold/')
    assert (store/generation['iid_source']['source_png']).read_bytes() == iid.read_bytes()
    assert record['parameters']['iid_power'] == .5
    assert record['parameters']['iid_gaussian_sigma_px'] == 32
    assert 'iid_linear_weight' not in record['parameters']
    assert len(called) == 1 and np.all(pixels == 77)
    assert np.allclose(called[0][0], 0x88 / 255)
    assert np.allclose(called[0][1], 0x99 / 255)
    Image.new('RGB', (16, 16), '#333333').save(prepared/(Path(name).stem+'.png'))
    with pytest.raises(ValueError, match='frozen SHA'):
        city4.apply_iid(record, args, log)


def test_pilot_reuses_selected_raw_history_without_copy_or_reselection(tmp_path, monkeypatch):
    name = city4.pilot.TEXTURES[0]
    store = tmp_path / 'store'
    first = source(store/'quality-pilot-2', name, attempt=1)
    second = source(store/'quality-pilot-2', name, attempt=2, colour='#112233')
    attempts = []
    for path in (first, second):
        item = json.loads(path.with_suffix('.json').read_text())
        attempts.append({key: item[key] for key in ('attempt', 'prompt', 'utc', 'seconds', 'source_sha256')} |
            dict(source_png='quality-pilot-2/' + path.name, source_size=[16, 16]))
    previous = dict(id='pilot-2-roof', replaces=[name], models=[deepcopy(city4.MODEL)],
        generation=dict(attempts=attempts, selected_attempt=1, model_identity='retained imagegen provenance'))
    old_registry = tmp_path/'pilot-2.jsonl'
    old_registry.write_text(json.dumps(previous)+'\n')
    before = {p.relative_to(store): p.read_bytes() for p in store.rglob('*') if p.is_file()}
    monkeypatch.setattr(city4.generate, 'target_corrected_maps', lambda image, record:
        ({'albedo': np.asarray(image)/255}, {'linear_rgb_max_error': 0}, None))
    monkeypatch.setattr(city4, 'measure_light', lambda path: dict(lighting_risk=False))
    monkeypatch.setattr(city4, 'seamless', lambda image, axes, overlap_fraction:
        (image, dict(axes=axes, overlap_fraction=overlap_fraction)))
    args = SimpleNamespace(input_dir=tmp_path/'unused-input', source_root=store,
        prepared_dir=tmp_path/'prepared', pilot2_registry=old_registry, select=[], command='prepare')
    record = city4.prepare(args, city4.recipes(textures=[name]))[0]
    assert record['id'].startswith('city-4-')
    assert record['generation']['attempts'] == attempts
    assert record['generation']['selected_attempt'] == 1
    assert record['generation']['source_registry_id'] == previous['id']
    assert record['generation']['model_identity'] == 'retained imagegen provenance'
    assert record['generation']['seam']['overlap_fraction'] == 1/64
    with Image.open(args.prepared_dir/(Path(name).stem+'.png')) as prepared:
        assert np.array_equal(np.asarray(prepared)[0, 0], [0x77, 0x66, 0x55])
    assert {p.relative_to(store): p.read_bytes() for p in store.rglob('*') if p.is_file()} == before
    args.select = [name + ':2']
    with pytest.raises(ValueError, match='selected pilot-2 raw attempt 1'):
        city4.prepare(args, city4.recipes(textures=[name]))
    args.select = []
    Image.new('RGB', (16, 16), '#123456').save(first)
    with pytest.raises(city4.registry.RegistryError, match='source_sha256: mismatch'):
        city4.prepare(args, city4.recipes(textures=[name]))


def test_detail_fit_uses_visible_alpha_and_retains_mask(tmp_path, monkeypatch):
    record = city4.recipes(textures=['NW_CITY_FLAG_01.TGA'])[0]
    mask = np.zeros((16, 16), dtype=bool)
    mask[2:14, 3:13] = True
    rgba = np.full((16, 16, 4), 100, dtype=np.uint8)
    rgba[..., 3] = mask * 255
    record['_maps'] = {'albedo': rgba.astype(np.float32)/255}
    record['generation'] = {'colour_correction': {'before': True}}
    def fitted(rgb, targets, *, target_oklab, ceiling, mask):
        assert ceiling is True
        assert np.array_equal(mask, rgba[..., 3] == 255)
        assert targets == record['parameters']['detail_targets']
        return np.full((16, 16, 3), 80, dtype=np.uint8), dict(colour={'fitted': True}, converged=True)
    monkeypatch.setattr(city4, 'fit_material_detail', fitted)
    monkeypatch.setattr(city4.generate, 'numeric_rejection', lambda record, correction: None)
    city4.fit_detail(record, rgba[..., :3])
    output = np.rint(record['_maps']['albedo']*255).astype(np.uint8)
    assert np.array_equal(output[..., 3], rgba[..., 3])
    assert np.all(output[..., :3] == 80)
    assert record['generation']['colour_correction'] == {'fitted': True}
    assert record['generation']['colour_correction_before_fit'] == {'before': True}
    assert set(city4.FIELDS) <= record['generation']['detail_measures_before'].keys()
    assert set(city4.FIELDS) <= record['generation']['detail_measures_after'].keys()


def test_build_saves_fitted_albedo_for_unflagged_material(tmp_path, monkeypatch):
    record = city4.recipes(textures=['NW_CITY_BODENDIELEN_02.TGA'])[0]
    name = record['replaces'][0]
    prepared = tmp_path/'prepared'
    prepared.mkdir()
    source_png = prepared/(Path(name).stem+'.png')
    Image.new('RGB', (16, 16), '#888888').save(source_png)
    record['generation'] = dict(outcome='complete', selected_attempt=1, attempts=[],
        prepared_sha256=city4.pilot.file_digest(source_png), detector_before=dict(lighting_risk=False))
    def fit(record, pixels):
        record['_maps'] = {key: np.full((16, 16, 3) if key in ('albedo', 'normal') else (16, 16), .25)
            for key in city4.registry.MAP_NAMES}
        record['generation'].update(detail_fit={'converged': True}, numeric_rejection_after_fit=None)
    monkeypatch.setattr(city4, 'fit_detail', fit)
    monkeypatch.setattr(city4, 'measure_light', lambda path: dict(lighting_risk=False))
    counts = tmp_path/'counts.json'
    counts.write_text(json.dumps({name: dict(v=5)}))
    args = SimpleNamespace(visual_counts=counts, marigold_log=None, prepared_dir=prepared,
        registry_output=tmp_path/'city-4.jsonl', derived_root=tmp_path/'derived', source_root=tmp_path/'store')
    result = city4.build(args, [record])
    assert result['batch'] == result['total'] == 1
    with Image.open(args.derived_root/record['files']['albedo']) as output:
        assert np.all(np.asarray(output) == 64)
    assert source_png.read_bytes() != (args.derived_root/record['files']['albedo']).read_bytes()
