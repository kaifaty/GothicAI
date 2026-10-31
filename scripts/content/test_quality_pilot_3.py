"""Synthetic IID provenance gates and protection of earlier staging sets."""
import json
from pathlib import Path
from types import SimpleNamespace

import numpy as np
from PIL import Image
import pytest

import quality_pilot_3


def test_frozen_prepared_png_preserves_exact_iid_bytes_across_encoders(tmp_path):
    target = tmp_path / 'input.png'
    image = Image.new('RGB', (32, 32), '#607080')
    image.save(target, compress_level=0)
    before = target.read_bytes()
    digest = quality_pilot_3.prepared_input(image, target, frozen=True)
    assert digest == quality_pilot_3.pilot.file_digest(target)
    assert target.read_bytes() == before
    changed = image.copy()
    changed.putpixel((0, 0), (1, 2, 3))
    with pytest.raises(ValueError, match='rebuilt pixels differ'):
        quality_pilot_3.prepared_input(changed, target, frozen=True)
    assert target.read_bytes() == before


@pytest.fixture
def iid_input(tmp_path):
    prepared = tmp_path / 'prepared' / 'pilot-3a'
    prepared.mkdir(parents=True)
    source = prepared / 'NW_CITY_ROOF_01.png'
    Image.new('RGB', (32, 32), '#606060').save(source)
    digest = quality_pilot_3.pilot.file_digest(source)
    record = dict(replaces=['NW_CITY_ROOF_01.TGA'], parameters=dict(set='pilot-3a'),
        models=[], generation=dict(detector_before=dict(lighting_risk=True), prepared_sha256=digest))
    args = SimpleNamespace(prepared_dir=prepared.parent, source_root=tmp_path / 'sources')
    return record, args, source


def test_unflagged_material_does_not_consult_iid_candidates(iid_input):
    record, args, source = iid_input
    record['generation']['detector_before']['lighting_risk'] = False

    class UnreadableCandidates(dict):
        def get(self, key):
            pytest.fail('unflagged material consulted IID outputs')

    # No model output directory or source-root argument is needed at this gate.
    result = quality_pilot_3.apply_iid(record,
        SimpleNamespace(prepared_dir=args.prepared_dir), UnreadableCandidates())
    with Image.open(source) as original:
        assert np.array_equal(result, np.asarray(original))
    assert record['generation']['marigold_applied'] is False
    assert record['models'] == []
    assert not args.source_root.exists()


@pytest.mark.parametrize('candidate_sha', [None, 'b' * 64])
def test_flagged_material_requires_exact_prepared_sha(iid_input, candidate_sha):
    record, args, _ = iid_input
    candidates = {} if candidate_sha is None else {candidate_sha: (Path('unreadable.png'), {}, True)}
    with pytest.raises(ValueError, match='no IID output for exact prepared SHA'):
        quality_pilot_3.apply_iid(record, args, candidates)
    assert record['generation']['marigold_applied'] is False
    assert not args.source_root.exists()


def test_exact_prepared_sha_can_reuse_iteration2_iid(iid_input, tmp_path):
    record, args, source = iid_input
    args.iteration2_marigold_dir = tmp_path / 'old-iid'
    args.iteration2_marigold_dir.mkdir()
    # The old texture filename deliberately differs: reuse depends on bytes.
    output = args.iteration2_marigold_dir / 'iteration2-different-name-albedo.png'
    Image.new('RGB', (32, 32), '#a0a0a0').save(output)
    item = dict(input=str(source), output=str(output),
        source_sha256=record['generation']['prepared_sha256'],
        output_sha256=quality_pilot_3.pilot.file_digest(output), seconds=2.5)
    args.iteration2_marigold_log = tmp_path / 'old.jsonl'
    args.iteration2_marigold_log.write_text(json.dumps(item) + '\n')
    args.marigold_dir = tmp_path / 'new-iid'
    args.marigold_log = tmp_path / 'new.jsonl'

    result = quality_pilot_3.apply_iid(record, args, quality_pilot_3.iid_candidates(args))

    assert result.shape == (32, 32, 3)
    assert result.dtype == np.uint8
    assert np.all(result > 96)
    generation = record['generation']
    assert generation['marigold_applied'] is True
    assert generation['iid_reused_iteration2'] is True
    assert generation['iid_seconds'] == 2.5
    assert generation['iid_source']['source_sha256'] == item['output_sha256']
    retained = args.source_root / generation['iid_source']['source_png']
    assert retained.read_bytes() == output.read_bytes()
    assert record['models'][0]['id'] == quality_pilot_3.marigold_iid.MODEL_ID


def output_args(tmp_path):
    return SimpleNamespace(staging_parent=tmp_path / 'staging', city3=tmp_path / 'staging/city-3',
        source_root=tmp_path / 'sources', prepared_dir=tmp_path / 'prepared', derived_root=tmp_path / 'derived',
        report_dir=tmp_path / 'report', registry_output=tmp_path / 'quality-pilot-3.jsonl',
        base_registry=tmp_path / 'materials.jsonl', iteration2_registry=tmp_path / 'quality-pilot-2.jsonl')


@pytest.mark.parametrize('protected', ['city-3', 'pilot-2', 'pilot-b', 'pilot-c'])
@pytest.mark.parametrize('destination', ['source_root', 'prepared_dir', 'derived_root', 'report_dir', 'registry_output'])
@pytest.mark.parametrize('nested', [False, True])
def test_outputs_cannot_overwrite_earlier_staging_sets(tmp_path, protected, destination, nested):
    args = output_args(tmp_path)
    base = args.staging_parent / protected
    setattr(args, destination, base / 'new-output' if nested else base)
    with pytest.raises(ValueError, match='overlaps a protected staging set'):
        quality_pilot_3.validate_outputs(args)
    assert not args.staging_parent.exists()


@pytest.mark.parametrize('registry', ['base_registry', 'iteration2_registry'])
def test_registry_output_cannot_replace_an_earlier_registry(tmp_path, registry):
    args = output_args(tmp_path)
    args.registry_output = getattr(args, registry)
    with pytest.raises(ValueError, match='iteration-3 registry must be separate'):
        quality_pilot_3.validate_outputs(args)
    assert not args.registry_output.exists()


def test_separate_iteration3_outputs_are_allowed_without_creating_them(tmp_path):
    quality_pilot_3.validate_outputs(output_args(tmp_path))
    assert not list(tmp_path.iterdir())


def test_window_prepare_uses_selected_albedo_and_one_physical_u_seamed_module(tmp_path, monkeypatch):
    window = quality_pilot_3.WINDOW
    legacy = 'NW_CITY_WALL_03.TGA'
    monkeypatch.setattr(quality_pilot_3, 'TEXTURES', (legacy, window))
    monkeypatch.setattr(quality_pilot_3, 'NEW_TEXTURES', (window,))
    args = output_args(tmp_path)
    args.command = 'prepare'
    args.input_dir = tmp_path / 'input'
    args.input_dir.mkdir()
    args.mapping = tmp_path / 'mapping.csv'
    args.selection = tmp_path / 'selection.json'
    args.selection.write_text(json.dumps(dict(selected_attempts={window: 2})))
    args.source_root.mkdir()
    Image.new('RGB', (32, 32), '#202830').save(args.source_root / 'legacy.png')
    # Neither the selected generator PNG nor another attempt is the city-3
    # final albedo: 3a must use the latter, and 3b must select attempt 2.
    Image.new('RGB', (32, 32), '#010203').save(args.source_root / 'raw-window.png')
    selected = args.city3 / 'sources/final-window.png'
    selected.parent.mkdir(parents=True)
    Image.new('RGB', (48, 53), '#607080').save(selected)
    for attempt, colour in ((1, '#102030'), (2, '#90a0b0')):
        source = args.input_dir / f'NW_CITY_WINDOW_03-attempt-{attempt}.png'
        Image.new('RGB', (60, 66), colour).save(source)
        source.with_suffix('.json').write_text(json.dumps(dict(texture=window,
            attempt=attempt, prompt=f'window attempt {attempt}', utc='2026-10-20T00:00:00Z', seconds=1)))

    def record(name, source, *, city=False):
        attempt = dict(prompt='previous source', source_png=source)
        attempt['number' if city else 'attempt'] = 1
        return dict(id='city-window' if city else 'legacy-wall', replaces=[name],
            files=dict(albedo='final-window.png'), models=[dict(id='synthetic')],
            parameters=dict(layout='panel', tiling_axes='UV', antitile='rotate', scale=1.),
            generation=dict(attempts=[attempt], selected_attempt=1, model_identity='synthetic'))

    old = record(legacy, 'legacy.png')
    city_window = record(window, 'raw-window.png', city=True)
    monkeypatch.setattr(quality_pilot_3.bakeoff, 'read_registry',
        lambda path: [old] if path == args.iteration2_registry else [city_window])
    monkeypatch.setattr(quality_pilot_3.pack_materials, 'select_set_records', lambda records, *_: records)
    row = {f'target_{key}': '.1' for key in quality_pilot_3.METRICS}
    parts = 'two posts; top binding; shutter; sill; lower plaster; two braces'
    monkeypatch.setattr(quality_pilot_3.pilot, 'csv_rows', lambda _: {
        legacy: row, window: dict(row, uv_m_u='1.93', uv_m_v='2.14',
            target_oklab_l='.4', target_oklab_a='.01', target_oklab_b='.02', parts=parts)})
    seams = []

    def seam(image, *, axes, overlap_fraction):
        seams.append((axes, image.size, overlap_fraction))
        return image, dict(axes=axes)

    monkeypatch.setattr(quality_pilot_3, 'seamless', seam)
    monkeypatch.setattr(quality_pilot_3.generate, 'target_corrected_maps',
        lambda image, _: (dict(albedo=np.asarray(image)/255), {}, None))
    monkeypatch.setattr(quality_pilot_3, 'measure_light', lambda _: dict(lighting_risk=False))

    records = quality_pilot_3.prepare(args)

    assert seams == [('', (1024, 1024), 1/64)] * 2
    windows = [r for r in records if r['replaces'] == [window]]
    for index, record in enumerate(windows):
        params, generation = record['parameters'], record['generation']
        assert {key: params[key] for key in ('decision', 'layout', 'scale', 'antitile', 'tiling_axes')} == dict(
            decision='module', layout='panel', scale=1., antitile='off', tiling_axes='u')
        assert params['detail_count_target'] == dict(u=1, v=1)
        assert params['parts'] == parts
        assert params['physical_uv_metres'] == [1.93, 2.14]
        assert params['source_aspect'] == pytest.approx(1.93/2.14)
        assert generation['geometry']['modules_u'] == generation['geometry']['modules_v'] == 1
        assert generation['geometry']['resamples_to_uv'] == 1
        assert generation['geometry']['source_size'] == ([48, 53] if index == 0 else [60, 66])
        assert [params['source_width'], params['source_height']] == generation['geometry']['source_size']
        assert generation['seam']['applied'] is True
        assert generation['seam']['axes'] == 'u'
        assert generation['seam']['output_size'] == [1024, 1024]
        assert [operation['axis'] for operation in generation['seam']['operations']] == ['u']
        prepared = args.prepared_dir / params['set'] / 'NW_CITY_WINDOW_03.png'
        with Image.open(prepared) as image:
            assert image.size == (1024, 1024)
            assert image.getpixel((0, 0)) == ((96, 112, 128) if index == 0 else (144, 160, 176))
    retained = windows[0]['generation']['input_source']
    assert retained['selected_albedo'] == 'final-window.png'
    assert retained['source_sha256'] == quality_pilot_3.pilot.file_digest(selected)
    assert (args.source_root / retained['source_png']).read_bytes() == selected.read_bytes()
    assert windows[1]['generation']['selected_attempt'] == 2
    assert windows[1]['prompt'] == 'window attempt 2'
    assert [r['parameters']['antitile'] for r in records if r['replaces'] == [legacy]] == ['rotate'] * 2
    assert (args.prepared_dir / 'pilot-3a/NW_CITY_WALL_03.png').read_bytes() == (
        args.prepared_dir / 'pilot-3b/NW_CITY_WALL_03.png').read_bytes()
    assert old['parameters'] == dict(layout='panel', tiling_axes='UV', antitile='rotate', scale=1.)


def test_window_canvas_resamples_once_and_joins_u_without_wrapping_v(monkeypatch):
    y, x = np.mgrid[:64, :96]
    pixels = np.stack((20 + y*3, x*2, np.full_like(x, 60)), axis=-1).astype(np.uint8)
    source = Image.fromarray(pixels)
    resize = Image.Image.resize
    calls = []

    def counted_resize(image, size, resample=None, **kwargs):
        calls.append((size, resample))
        return resize(image, size, resample=resample, **kwargs)

    monkeypatch.setattr(Image.Image, 'resize', counted_resize)

    packed, geometry, seam = quality_pilot_3.window_canvas(source, [1.93, 2.14])

    assert calls == [((3072, 1024), Image.Resampling.LANCZOS)]
    assert packed.size == (1024, 1024)
    result = np.asarray(packed).astype(float)
    # The original U boundary has a 190-level green jump; the periodic U
    # resample joins adjacent pixels while the red top/bottom difference stays.
    assert np.max(np.abs(result[:, 0]-result[:, -1])) <= 2
    assert np.mean(np.abs(result[0]-result[-1])) > 50
    assert geometry['physical_uv_metres'] == [1.93, 2.14]
    assert geometry['resamples_to_uv'] == 1
    assert seam['axes'] == 'u'
    assert [operation['axis'] for operation in seam['operations']] == ['u']


def test_material_sheet_contains_eight_rows_at_1200_pixels_under_one_mb(tmp_path, monkeypatch):
    assert quality_pilot_3.TEXTURES == (*quality_pilot_3.pilot.TEXTURES, quality_pilot_3.WINDOW)
    args = output_args(tmp_path)
    args.mapping = tmp_path / 'mapping.csv'
    args.iteration2_root = tmp_path / 'iteration2'
    image = Image.fromarray(np.random.default_rng(123).integers(0, 256, (128, 128, 3), dtype=np.uint8))
    for directory in (args.iteration2_root, args.city3 / 'sources', args.derived_root):
        directory.mkdir(parents=True)
        image.save(directory / 'synthetic.png')
    args.report_dir.mkdir()
    previous = [dict(replaces=[name], files=dict(albedo='synthetic.png')) for name in quality_pilot_3.TEXTURES]
    monkeypatch.setattr(quality_pilot_3.bakeoff, 'read_registry', lambda _: previous)
    monkeypatch.setattr(quality_pilot_3.pack_materials, 'select_set_records', lambda records, *_: records)
    records = [dict(replaces=[name], parameters=dict(set=variant), files=dict(albedo='synthetic.png'))
        for variant in quality_pilot_3.SETS for name in quality_pilot_3.TEXTURES]

    quality_pilot_3.contact_sheet(records, args)

    sheet = args.report_dir / 'materials-2-3a-3b.jpg'
    assert sheet.stat().st_size <= 1_000_000
    with Image.open(sheet) as opened:
        assert opened.format == 'JPEG'
        assert opened.size == (1200, 428 * 8)
