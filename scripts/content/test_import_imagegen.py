"""CPU-only admission and provenance regressions for built-in tool imports."""

from copy import deepcopy
import json
import hashlib
import os
from pathlib import Path
import subprocess
import sys

import numpy as np
from PIL import Image
import pytest

sys.path.insert(0, str(Path(__file__).parent))
import generate
import import_imagegen as importer
import registry


ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture
def prepared(tmp_path, monkeypatch):
    monkeypatch.setattr(registry, 'DEFAULT_SOURCE_ROOT', tmp_path / 'permanent-sources')
    records = generate.recipes(ROOT / 'data/content/city-materials.csv',
        ROOT / 'docs/content-style.md', 'city-2')
    staging, flux = tmp_path / 'imagegen', tmp_path / 'flux'
    sources = staging / 'sources'
    sources.mkdir(parents=True)
    for record in records:
        # This fixture represents pre-pilot recipes with only scalar brightness.
        record['parameters'].pop('target_oklab', None)
        record['parameters'].pop('colour_policy', None)
        record['files'] = {name: f'city-2/{record["id"]}/attempt-1/{name}.png'
            for name in importer.bakeoff.MAPS}
        record['generation'] = dict(outcome='complete', attempts=[dict(number=1,
            source_width=1024, source_height=1024, seed=record['seed'])],
            selected_attempt=1, visual_review=dict(outcome='best_available', note='FLUX pilot provenance'))
        for path in record['files'].values():
            target = flux / 'sources' / path
            target.parent.mkdir(parents=True, exist_ok=True)
            Image.new('RGB', (8, 8), (90, 85, 80)).save(target)
        if record['replaces'][0] in importer.TEXTURES:
            write_attempt(sources, record['replaces'][0], 1)
    path = tmp_path / 'registry.jsonl'
    # Deliberately retain noncanonical spacing and key order from an existing row.
    path.write_text(''.join(json.dumps(record, ensure_ascii=False, separators=(', ', ': ')) + '\n'
        for record in records), encoding='utf-8')
    return staging, flux, path, records


def write_attempt(sources, texture, number, color=(100, 90, 80)):
    stem = f'{Path(texture).stem}-attempt-{number}'
    source = sources / f'{stem}.png'
    Image.new('RGB', (24, 16), color).save(source)
    (sources / f'{stem}.json').write_text(json.dumps(dict(texture=texture,
        attempt=number, source=str(source.resolve()), prompt=f'Text only recipe {number}',
        utc='2026-10-19T16:00:00Z', seconds=30 + number)), encoding='utf-8')


def light_filter(image, record):
    """Keep full-set orchestration tests small; one real filter test is below."""
    assert image.size == (1024, 1024)
    maps = {name: np.full((8, 8, 3) if name in ('albedo', 'normal') else (8, 8), .5)
        for name in importer.bakeoff.MAPS}
    correction = dict(target=record['parameters']['target_luminance'],
        linear_mean_after=record['parameters']['target_luminance'],
        saturation=dict(mean_hsv_s_after=.1))
    return maps, correction, None


def test_full_import_is_idempotent_and_preserves_flux_recipes_and_files(prepared, monkeypatch):
    staging, flux, path, originals = prepared
    write_attempt(staging / 'sources', 'NW_CITY_WINDOW_03.TGA', 2, (80, 90, 100))
    before = path.read_bytes()
    monkeypatch.setattr(importer.generate, 'target_corrected_maps', light_filter)
    result = importer.import_set(staging, flux, path,
        selections={'NW_CITY_WINDOW_03.TGA': 1})
    assert len(result) == 22
    assert path.read_bytes().startswith(before)
    original_by_texture = {record['replaces'][0]: record for record in originals}
    for record in result:
        original = original_by_texture[record['replaces'][0]]
        assert record['parameters']['scale'] == original['parameters']['scale']
        assert record['parameters']['antitile'] == original['parameters']['antitile']
        if record['pipeline'] == 'flux-filter':
            expected = deepcopy(original)
            expected['id'] = importer._new_id(original)
            expected['parameters']['set'] = importer.SET
            assert record == expected
            for relative in record['files'].values():
                assert (staging / 'sources' / relative).read_bytes() == (flux / 'sources' / relative).read_bytes()
        else:
            assert record['seed'] is None
            assert record['models'] == [importer.MODEL]
            assert not importer.FLUX_PARAMETERS.intersection(record['parameters']) - {'tiling'}
            assert record['parameters']['source_aspect'] == original['parameters']['source_aspect']
            registry.validate_entry(record, content_root=staging / 'sources')
    window = next(record for record in result if record['replaces'] == ['NW_CITY_WINDOW_03.TGA'])
    assert window['generation']['selected_attempt'] == 1
    assert [attempt['selected'] for attempt in window['generation']['attempts']] == [True, False]
    assert [attempt['prompt'] for attempt in window['generation']['attempts']] == ['Text only recipe 1', 'Text only recipe 2']
    raw_attempt = window['generation']['attempts'][0]
    permanent = registry.DEFAULT_SOURCE_ROOT / raw_attempt['source_png']
    staged = staging / 'sources/NW_CITY_WINDOW_03-attempt-1.png'
    assert permanent.is_file() and staged.is_symlink()
    assert staged.resolve() == permanent.resolve()
    assert not Path(os.readlink(staged)).is_absolute()
    assert hashlib.sha256(permanent.read_bytes()).hexdigest() == raw_attempt['source_sha256']
    metadata = json.loads(staged.with_suffix('.json').read_text())
    assert metadata['source'] == raw_attempt['source_png']
    assert metadata['source_sha256'] == raw_attempt['source_sha256']
    once = path.read_bytes()
    assert importer.import_set(staging, flux, path,
        selections={'NW_CITY_WINDOW_03.TGA': 1}) == result
    assert path.read_bytes() == once
    assert len(registry.check_registry(path)) == 44


def test_numeric_failure_never_writes_registry(prepared, monkeypatch):
    staging, flux, path, _ = prepared
    before = path.read_bytes()
    monkeypatch.setattr(importer.generate, 'target_corrected_maps',
        lambda image, record: ({}, {}, 'brightness correction failed'))
    with pytest.raises(ValueError, match='numeric admission failed: brightness'):
        importer.import_set(staging, flux, path)
    assert path.read_bytes() == before


@pytest.fixture
def seam_sensitive_filter(monkeypatch):
    """Make rewrites observable without running the expensive overlap cut."""
    state = {'reject_texture': None}

    def seam(image, axes='', overlap_fraction=.125):
        if axes:
            image = Image.new('RGB', image.size, (180, 90, 80))
        return image, dict(applied=bool(axes), axes=axes)

    def filtered(image, record):
        if record['replaces'][0] == state['reject_texture']:
            return {}, {}, 'brightness correction failed'
        maps, correction, rejection = light_filter(image, record)
        value = image.getpixel((0, 0))[0] / 255
        return {name: np.full_like(array, value) for name, array in maps.items()}, correction, rejection

    monkeypatch.setattr(importer.seam_fix, 'seamless', seam)
    monkeypatch.setattr(importer.generate, 'target_corrected_maps', filtered)
    return state


def registered_map_bytes(records, sources):
    paths = {relative for record in records for relative in record['files'].values()}
    paths.update(relative for record in records
        for attempt in record['generation']['attempts']
        for relative in attempt.get('files', {}).values())
    return {relative: (sources / relative).read_bytes() for relative in paths}


@pytest.mark.parametrize(('options', 'message'), [
    ({'selections': {'NW_CITY_DUNGEONFLOOR_02.TGA': 2}}, 'selected attempt 2 does not exist'),
    ({'fixes': {'NW_CITY_WINDOW_03.TGA': 'u'}}, 'exceeds panel repeat axes'),
    ({'fixes': {'NW_CITY_WOODBEAM_02.TGA': 'u'}}, 'exceeds strip repeat axes'),
])
def test_failed_reimport_options_preserve_registered_maps(prepared, seam_sensitive_filter, options, message):
    staging, flux, path, _ = prepared
    registered = importer.import_set(staging, flux, path)
    before_registry = path.read_bytes()
    before_maps = registered_map_bytes(registered, staging / 'sources')
    assert len(before_maps) == 22 * len(importer.bakeoff.MAPS)
    # DIRTYGROUND is processed before every invalid option above. Its changed
    # seam output must not replace the maps from the previous successful import.
    options = deepcopy(options)
    options.setdefault('fixes', {})['NW_CITY_DIRTYGROUND_02.TGA'] = 'uv'
    with pytest.raises(ValueError, match=message):
        importer.import_set(staging, flux, path, **options)
    assert path.read_bytes() == before_registry
    assert registered_map_bytes(registered, staging / 'sources') == before_maps


def test_late_numeric_failure_preserves_registered_maps(prepared, seam_sensitive_filter):
    staging, flux, path, _ = prepared
    registered = importer.import_set(staging, flux, path)
    before_registry = path.read_bytes()
    before_maps = registered_map_bytes(registered, staging / 'sources')
    assert len(before_maps) == 22 * len(importer.bakeoff.MAPS)
    seam_sensitive_filter['reject_texture'] = 'NW_CITY_DUNGEONFLOOR_02.TGA'
    with pytest.raises(ValueError, match='numeric admission failed: brightness'):
        importer.import_set(staging, flux, path,
            fixes={'NW_CITY_DIRTYGROUND_02.TGA': 'uv'})
    assert path.read_bytes() == before_registry
    assert registered_map_bytes(registered, staging / 'sources') == before_maps


def test_successful_reimport_retains_previous_maps_and_reuses_identical_version(prepared, seam_sensitive_filter):
    staging, flux, path, _ = prepared
    sources = staging / 'sources'
    registered = importer.import_set(staging, flux, path)
    before_maps = registered_map_bytes(registered, sources)
    fixes = {'NW_CITY_DIRTYGROUND_02.TGA': 'uv'}
    updated = importer.import_set(staging, flux, path, fixes=fixes)
    original_ground = next(record for record in registered
        if record['replaces'] == ['NW_CITY_DIRTYGROUND_02.TGA'])
    updated_ground = next(record for record in updated
        if record['replaces'] == ['NW_CITY_DIRTYGROUND_02.TGA'])
    assert set(original_ground['files'].values()).isdisjoint(updated_ground['files'].values())
    assert registered_map_bytes(registered, sources) == before_maps
    for name, relative in updated_ground['files'].items():
        assert (sources / relative).read_bytes() != before_maps[original_ground['files'][name]]

    once_registry = path.read_bytes()
    once_maps = registered_map_bytes(updated, sources)
    once_paths = {file.relative_to(sources) for file in sources.rglob('*.png')}
    assert importer.import_set(staging, flux, path, fixes=fixes) == updated
    assert path.read_bytes() == once_registry
    assert registered_map_bytes(updated, sources) == once_maps
    assert registered_map_bytes(registered, sources) == before_maps
    assert {file.relative_to(sources) for file in sources.rglob('*.png')} == once_paths


def test_exact_source_scope_and_layout_axes_are_required(prepared):
    staging, _, _, records = prepared
    sources = staging / 'sources'
    found = importer.read_sources(sources)
    assert len(found) == 12
    beam = next(record for record in records if record['replaces'] == ['NW_CITY_WOODBEAM_02.TGA'])
    with pytest.raises(ValueError, match='exceeds strip repeat axes'):
        importer.import_material(beam, found[beam['replaces'][0]], sources, axes='u')
    (sources / 'NW_CITY_WINDOW_03-attempt-1.json').unlink()
    with pytest.raises(ValueError, match='source PNGs have no attempt metadata'):
        importer.read_sources(sources)


def test_real_filter_retains_native_dimensions_physical_aspect_and_numeric_limits(prepared):
    staging, _, _, records = prepared
    sources = staging / 'sources'
    original = next(record for record in records if record['replaces'] == ['NW_CITY_WINDOW_03.TGA'])
    before = deepcopy(original)
    found = importer.read_sources(sources)
    result = importer.import_material(original, found['NW_CITY_WINDOW_03.TGA'], sources)
    assert original == before
    assert result['parameters']['source_width'] == 24
    assert result['parameters']['source_height'] == 16
    assert result['parameters']['source_aspect'] == pytest.approx(4 / 3.75)
    attempt = result['generation']['attempts'][0]
    assert attempt['numeric_valid'] and attempt['numeric_rejection'] is None
    assert generate.numeric_rejection(result, attempt['brightness_correction']) is None
    assert attempt['seam_fix']['applied'] is False
    for relative in result['files'].values():
        with Image.open(sources / relative) as image:
            assert image.size == (1024, 1024)
    with Image.open(registry.DEFAULT_SOURCE_ROOT / attempt['source_png']) as image:
        assert image.size == (24, 16)


def test_option_parser_rejects_duplicates_and_unknown_textures():
    assert importer.option_pairs(['NW_CITY_WOODBEAM_02:v']) == {'NW_CITY_WOODBEAM_02.TGA': 'v'}
    with pytest.raises(ValueError, match='duplicate'):
        importer.option_pairs(['NW_CITY_WOODBEAM_02:v', 'NW_CITY_WOODBEAM_02.TGA:v'])
    with pytest.raises(ValueError, match='unknown imported texture'):
        importer.option_pairs(['NW_CITY_ROOF_04:1'], selection=True)


def test_cli_backs_up_after_batch_import(tmp_path, monkeypatch):
    events = []

    def imported(*args, **kwargs):
        events.append('import')
        assert kwargs['source_root'] == tmp_path / 'permanent'
        return [dict(pipeline='imagegen-filter', generation=dict(attempts=[{}]))]

    def backed_up(command, *, check):
        events.append('backup')
        assert check
        assert command[0].endswith('/backup-sources.sh')
        assert command[1:] == ['--registry', str(tmp_path / 'registry.jsonl'),
            '--source-root', str(tmp_path / 'permanent')]

    monkeypatch.setattr(importer, 'import_set', imported)
    monkeypatch.setattr(importer.subprocess, 'run', backed_up)
    assert importer.main(['import', '--staging', str(tmp_path / 'staging'),
        '--flux-root', str(tmp_path / 'flux'), '--registry', str(tmp_path / 'registry.jsonl'),
        '--source-root', str(tmp_path / 'permanent')]) == 0
    assert events == ['import', 'backup']


def test_cli_backup_failure_returns_nonzero(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(importer, 'import_set', lambda *args, **kwargs: [])

    def failed(command, *, check):
        raise subprocess.CalledProcessError(23, command)

    monkeypatch.setattr(importer.subprocess, 'run', failed)
    assert importer.main(['import', '--staging', str(tmp_path / 'staging'),
        '--flux-root', str(tmp_path / 'flux')]) == 1
    assert 'Imagegen import failed:' in capsys.readouterr().err
