"""City-3 retains all tool calls while admitting independent staged batches."""
from copy import deepcopy
import csv
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image
import pytest

import city_set


ROOT = Path(__file__).resolve().parents[2]
MAPPING = ROOT/'data/content/city-materials.csv'
BIBLE = ROOT/'docs/content-style.md'
GROUND = 'NW_CITY_DIRTYGROUND_02.TGA'


def mapping_with_targets(tmp_path):
    with MAPPING.open(newline='') as stream:
        reader = csv.DictReader(stream)
        fields, rows = list(reader.fieldnames), list(reader)
    if 'target_coverage' not in fields:
        fields.append('target_coverage')
    for row in rows:
        row['target_luminance'] = '.18'
        for field in ('target_oklab_l', 'target_oklab_a', 'target_oklab_b'):
            if field in row:
                row[field] = ''
        row['target_coverage'] = '.5' if row['texture'] in city_set.alpha_material.ALPHA_TEXTURES else ''
    path = tmp_path/'mapping.csv'
    with path.open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    return path


def source(tmp_path, texture=GROUND, number=1, prompt=None, colour=(120, 100, 80)):
    staging, store = tmp_path/'staging', tmp_path/'raw'
    relative = Path('city-3')/f'{Path(texture).stem}-attempt-{number}.png'
    path = store/relative
    path.parent.mkdir(parents=True, exist_ok=True)
    # Small input is enough to test source retention; CPU filters are stubbed.
    Image.new('RGB', (16, 16), colour).save(path)
    metadata = dict(texture=texture, attempt=number, prompt=prompt or 'actual tool prompt',
        utc='2026-10-19T01:02:03Z', seconds=1.25, source=relative.as_posix(),
        tool='image_gen.imagegen')
    meta = staging/'sources'/f'{Path(texture).stem}-attempt-{number}.json'
    meta.parent.mkdir(parents=True, exist_ok=True)
    meta.write_text(json.dumps(metadata)+'\n')
    return staging, store, path, meta


@pytest.fixture
def fast_filter(monkeypatch):
    def maps(image, record):
        values = {name: np.full((16, 16, 3 if name in ('albedo', 'normal') else 1),
            .5, dtype=np.float32).squeeze() for name in city_set.bakeoff.MAPS}
        return values, dict(saturation=dict(mean_hsv_s_after=.1)), None
    monkeypatch.setattr(city_set.generate, 'target_corrected_maps', maps)
    monkeypatch.setattr(city_set.seam_fix, 'seamless',
        lambda image, axes, overlap_fraction: (image, dict(axes=axes, overlap_fraction=overlap_fraction)))


def registry_file(tmp_path, mapping, textures=(GROUND,)):
    path = tmp_path/'registry.jsonl'
    rows = city_set.recipes(mapping, BIBLE, textures=textures)
    path.write_text(''.join(json.dumps(row)+'\n' for row in rows))
    return path


def test_recipes_are_complete_text_only_and_do_not_mutate_mapping(tmp_path):
    mapping = mapping_with_targets(tmp_path)
    before = mapping.read_bytes()
    rows = city_set.recipes(mapping, BIBLE)
    assert len(rows) == len({row['replaces'][0] for row in rows}) == 58
    assert mapping.read_bytes() == before
    for row in rows:
        assert row['pipeline'] == 'imagegen-filter' and row['seed'] is None
        assert city_set.NO_BAKED_LIGHT in row['prompt']
        params = row['parameters']
        assert params['target_luminance'] == .18
        assert params['metallic'] == 0
        assert params['input'] == 'text only; no original pixels or game screenshots'
        if params['decision'] != 'include':
            assert params['scale'] == 1 and params['antitile'] == 'off'
            assert params['physical_uv_metres'][0]/params['physical_uv_metres'][1] == params['source_aspect']
    gable = next(row for row in rows if row['replaces'] == ['NW_CITY_HAFEN_GIEBEL_01.TGA'])
    assert 'horizontal overlapping' in gable['prompt']
    assert 'vertical' not in gable['prompt']


def test_recipes_propagate_numeric_mean_colour_without_modifying_old_records(tmp_path):
    from material_colour import linear_to_oklab
    mapping = mapping_with_targets(tmp_path)
    with mapping.open(newline='') as stream:
        reader = csv.DictReader(stream)
        fields, rows = list(reader.fieldnames), list(reader)
    colour_fields = ['target_oklab_'+channel for channel in ('l', 'a', 'b')]
    fields += [field for field in colour_fields if field not in fields]
    target = linear_to_oklab([.25, .16, .06]).tolist()
    for row in rows:
        row.update({field: '' for field in colour_fields})
        if row['texture'] == GROUND:
            row.update(zip(colour_fields, target))
    with mapping.open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    recipes = city_set.recipes(mapping, BIBLE)
    coloured = next(row for row in recipes if row['replaces'] == [GROUND])
    assert coloured['parameters']['target_oklab'] == target
    assert 'no saturation cap' in coloured['parameters']['colour_policy']
    assert all('target_oklab' not in row['parameters'] for row in recipes if row is not coloured)


def test_targets_required_for_import_but_not_for_prompt_preparation(tmp_path):
    mapping = mapping_with_targets(tmp_path)
    with mapping.open(newline='') as stream:
        reader = csv.DictReader(stream)
        fields, rows = reader.fieldnames, list(reader)
    next(row for row in rows if row['texture'] == GROUND)['target_luminance'] = ''
    with mapping.open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    with pytest.raises(ValueError, match=f'{GROUND}: city-3 requires target_luminance'):
        city_set.recipes(mapping, BIBLE)
    assert len(city_set.recipes(mapping, BIBLE, require_target=False)) == 58


def test_timber_shutter_wall_uses_wood_numeric_guidance_without_changing_window_intent(tmp_path):
    mapping = mapping_with_targets(tmp_path)
    textures = ['NW_CITY_HAFEN_HOLZWAND_02.TGA', 'NW_CITY_WINDOW_03.TGA']
    rows = {row['replaces'][0]: row for row in city_set.recipes(mapping, BIBLE, textures=textures)}
    wood, plaster = (rows[texture]['parameters'] for texture in textures)
    classes, _ = city_set.generate.read_style(BIBLE)
    assert wood['material_class'] == plaster['material_class'] == 'window_wall_tile'
    assert wood['numeric_substrate'] == 'planks_wall'
    assert wood['roughness_range'] == classes['planks_wall']['roughness']
    assert wood['roughness_base'] == sum(classes['planks_wall']['roughness'])/2
    assert 'small shuttered opening' in wood['depicts']
    assert plaster['numeric_substrate'] == 'plaster'
    assert plaster['roughness_range'] == classes['plaster']['roughness']


def test_partial_batch_import_retains_source_and_preserves_other_pending_rows(tmp_path, fast_filter):
    mapping = mapping_with_targets(tmp_path)
    other = 'NW_CITY_WALL_03.TGA'
    registry = registry_file(tmp_path, mapping, (GROUND, other))
    before = json.loads(registry.read_text().splitlines()[1])
    staging, store, raw, metadata = source(tmp_path)
    records = city_set.import_batch(staging, registry, textures=[GROUND],
        mapping=mapping, bible=BIBLE, source_root=store)
    assert len(records) == 1
    entries = city_set.registry_module.check_registry(registry, source_root=store)
    assert entries[1] == before and entries[1]['generation']['outcome'] == 'pending'
    attempt = records[0]['generation']['attempts'][0]
    assert attempt['source_sha256'] == hashlib.sha256(raw.read_bytes()).hexdigest()
    assert metadata.with_suffix('.png').is_symlink()
    assert not Path(metadata.with_suffix('.png').readlink()).is_absolute()
    assert records[0]['generation']['seam_fix']['overlap_fraction'] == 1/8
    city_set.registry_module.validate_entry(records[0], content_root=staging/'sources', source_root=store)


def test_all_attempts_are_retained_and_prior_source_cannot_be_rewritten(tmp_path, fast_filter):
    mapping = mapping_with_targets(tmp_path)
    registry = registry_file(tmp_path, mapping)
    staging, store, raw, metadata = source(tmp_path)
    initial = city_set.import_batch(staging, registry, textures=[GROUND], mapping=mapping, bible=BIBLE, source_root=store)[0]
    selected_files = {relative: (staging/'sources'/relative).read_bytes() for relative in initial['files'].values()}
    source(tmp_path, number=2, colour=(140, 110, 90))
    updated = city_set.import_batch(staging, registry, textures=[GROUND], mapping=mapping, bible=BIBLE,
        source_root=store, selections={GROUND: 1}, reviews={GROUND: 'first attempt preferred'})[0]
    assert len(updated['generation']['attempts']) == 2
    assert updated['generation']['selected_attempt'] == 1
    assert all((staging/'sources'/relative).read_bytes() == value for relative, value in selected_files.items())
    changed = json.loads(metadata.read_text())
    changed['prompt'] = 'rewritten history'
    metadata.write_text(json.dumps(changed))
    previous_registry = registry.read_bytes()
    with pytest.raises(ValueError, match='cannot be removed or rewritten'):
        city_set.import_batch(staging, registry, textures=[GROUND], mapping=mapping, bible=BIBLE, source_root=store)
    assert registry.read_bytes() == previous_registry


def test_attempt_sequence_and_transparency_are_rejected(tmp_path):
    staging, store, raw, metadata = source(tmp_path, number=2)
    with pytest.raises(ValueError, match='consecutive attempts'):
        city_set.read_sources(staging/'sources', [GROUND], store)
    metadata.unlink()
    staging, store, raw, metadata = source(tmp_path)
    Image.new('RGBA', (16, 16), (100, 80, 60, 0)).save(raw)
    with pytest.raises(ValueError, match='must be opaque'):
        city_set.read_sources(staging/'sources', [GROUND], store)


def test_prepare_preserves_already_retained_pending_attempt(tmp_path):
    mapping = mapping_with_targets(tmp_path)
    registry = registry_file(tmp_path, mapping)
    records = [json.loads(registry.read_text())]
    records[0]['generation'].update(attempts=[dict(number=1, prompt='actual tool prompt')],
        reason='waiting for a superseded decision')
    registry.write_text(json.dumps(records[0])+'\n')
    updated = city_set.prepare(registry, mapping, BIBLE, tmp_path/'raw')
    retained = next(row for row in updated if row['replaces'] == [GROUND])
    assert retained['generation']['attempts'] == records[0]['generation']['attempts']
    assert 'reason' not in retained['generation']
    assert len(updated) == 58


def test_failed_selection_requires_review_and_modules_use_narrow_overlap(tmp_path, fast_filter):
    mapping = mapping_with_targets(tmp_path)
    texture = 'NW_CITY_WOODBEAM_02.TGA'
    staging, store, _, _ = source(tmp_path, texture=texture)
    record = city_set.recipes(mapping, BIBLE, textures=[texture])[0]
    attempts = city_set.read_sources(staging/'sources', [texture], store)[texture]
    with pytest.raises(ValueError, match='requires a review note'):
        city_set.import_material(record, attempts, staging/'sources', store, failed=True)
    imported = city_set.import_material(record, attempts, staging/'sources', store,
        failed=True, review='best of three, contact shading still visible')
    assert imported['generation']['selected_failed']
    assert imported['generation']['seam_fix'] == dict(axes='v', overlap_fraction=1/32)


def test_reuse_keeps_raw_provenance_without_new_tool_call(tmp_path, fast_filter):
    mapping = mapping_with_targets(tmp_path)
    registry = registry_file(tmp_path, mapping)
    old_stage, store, raw, _ = source(tmp_path)
    record = city_set.import_batch(old_stage, registry, textures=[GROUND], mapping=mapping,
        bible=BIBLE, source_root=store)[0]
    original = deepcopy(record)
    original['id'] = original['id'].replace('city-3-', 'city-2-imagegen-', 1)
    original['parameters']['set'] = 'city-2-imagegen'
    registry.write_text(json.dumps(original)+'\n')
    new_stage = tmp_path/'city-3'
    reused = city_set.reuse(new_stage, old_stage, registry, textures=[GROUND], mapping=mapping,
        bible=BIBLE, source_root=store)[0]
    assert reused['generation']['reused_from'] == original['id']
    assert reused['generation']['new_tool_calls'] == 0
    assert reused['generation']['attempts'] == original['generation']['attempts']
    assert city_set.registry_module.source_references(reused) == city_set.registry_module.source_references(original)
    assert any(path.is_symlink() for path in (new_stage/'sources/raw-sources').rglob('*.png'))


@pytest.mark.parametrize('unreachable', [False, True])
def test_reuse_admits_stored_albedos_against_real_measured_colour(tmp_path, fast_filter, unreachable):
    from material_colour import _linear, oklab_to_linear
    texture = 'NW_CITY_HAFENKAI_BODEN_01.TGA'
    mapping = mapping_with_targets(tmp_path)
    registry = registry_file(tmp_path, mapping, textures=[texture])
    old_stage, store, _, _ = source(tmp_path, texture=texture)
    source(tmp_path, texture=texture, number=2)
    original = city_set.import_batch(old_stage, registry, textures=[texture], mapping=mapping,
        bible=BIBLE, source_root=store)[0]
    original['id'] = original['id'].replace('city-3-', 'city-2-imagegen-', 1)
    original['parameters']['set'] = 'city-2-imagegen'
    registry.write_text(json.dumps(original)+'\n')
    if unreachable:
        # A selected old output with no RGB signal cannot attain a nonblack
        # measured target by multiplication, even if older metadata said valid.
        Image.new('RGB', (16, 16), 'black').save(old_stage/'sources'/original['files']['albedo'])
    original_bytes = {path: (old_stage/'sources'/path).read_bytes()
        for path in city_set._map_references(original)}
    before_registry = registry.read_bytes()
    destination = tmp_path/'city-3'
    # MAPPING retains actual target_oklab; the legacy source fixture does not.
    options = dict(textures=[texture], mapping=MAPPING, bible=BIBLE, source_root=store)
    if unreachable:
        with pytest.raises(ValueError, match='reused albedo fails current colour admission'):
            city_set.reuse(destination, old_stage, registry, **options)
        assert registry.read_bytes() == before_registry
    else:
        reused = city_set.reuse(destination, old_stage, registry, **options)[0]
        target = oklab_to_linear(reused['parameters']['target_oklab'])
        with Image.open(old_stage/'sources'/original['files']['albedo']) as image:
            before = _linear(np.asarray(image.convert('RGB'), dtype=float)/255).mean(axis=(0, 1))
        assert abs(before-target).max() > .005
        for selected in (reused, *reused['generation']['attempts']):
            with Image.open(destination/'sources'/selected['files']['albedo']) as image:
                actual = _linear(np.asarray(image.convert('RGB'), dtype=float)/255).mean(axis=(0, 1))
            np.testing.assert_allclose(actual, target, atol=.005)
            correction = selected.get('brightness_correction', selected.get('generation', {}).get('brightness_correction'))
            np.testing.assert_allclose(correction['linear_rgb_mean_after'], actual)
        for old, new in zip(original['generation']['attempts'], reused['generation']['attempts']):
            assert new['numeric_valid'] and new['numeric_rejection'] is None
            for field in ('source_png', 'source_sha256', 'prompt', 'utc', 'seconds'):
                assert new[field] == old[field]
            assert all(new['files'][name] == old['files'][name] for name in city_set.bakeoff.MAPS if name != 'albedo')
        assert reused['files'] == reused['generation']['attempts'][1]['files']
        assert reused['generation']['new_tool_calls'] == 0
        assert reused['generation']['reused_from'] == original['id']
        assert city_set.registry_module.source_references(reused) == city_set.registry_module.source_references(original)
        saved = registry.read_bytes()
        assert city_set.reuse(destination, old_stage, registry, **options)[0] == reused
        assert registry.read_bytes() == saved
    assert all((old_stage/'sources'/path).read_bytes() == value for path, value in original_bytes.items())


def test_cli_import_runs_backup_only_after_registry_was_written(tmp_path, fast_filter, monkeypatch):
    mapping = mapping_with_targets(tmp_path)
    registry = registry_file(tmp_path, mapping)
    staging, store, _, _ = source(tmp_path)
    calls = []
    def backup(command, check):
        assert city_set.registry_module.check_registry(registry, source_root=store)[0]['generation']['outcome'] == 'complete'
        calls.append(command)
    monkeypatch.setattr(city_set.subprocess, 'run', backup)
    assert city_set.main(['import', '--registry', str(registry), '--mapping', str(mapping),
        '--bible', str(BIBLE), '--source-root', str(store), '--staging', str(staging), '--texture', GROUND]) == 0
    assert len(calls) == 1 and calls[0][0].endswith('backup-sources.sh')


def test_alpha_recipes_request_real_transparency_and_opaque_rope_axes(tmp_path):
    rows = {row['replaces'][0]: row for row in city_set.recipes(mapping_with_targets(tmp_path), BIBLE)}
    for name in city_set.alpha_material.ALPHA_TEXTURES:
        row = rows[name]
        assert 'real RGBA alpha transparency' in row['prompt']
        assert 'no painted checkerboard' in row['prompt']
        assert '50.0%' in row['prompt']
        assert row['parameters']['alpha_mode'] == 'test'
        assert row['parameters']['alpha_threshold'] == .5
        assert row['parameters']['target_coverage'] == .5
        assert row['parameters']['coverage_tolerance'] == .1
        assert 'albedo only' in row['parameters']['transparent_render']
        assert 'surface fills every pixel' not in row['prompt']
    assert rows['NW_CITY_ROPE_01.TGA']['parameters']['tiling_axes'] == 'v'
    assert rows['NW_HARBOUR_ROPEWALL_01.TGA']['parameters']['tiling_axes'] == 'u'
    assert rows['NW_HARBOUR_ROPEWALL_02.TGA']['parameters']['tiling_axes'] == 'u'
    assert all('alpha_mode' not in rows[name]['parameters'] for name in rows if 'ROPE' in name)
    border = rows['NW_CITY_IVY_BORDER_01.TGA']['prompt']
    assert 'LEFT side' in border and 'open RIGHT side' in border
    flag = rows['NW_CITY_FLAG_01.TGA']['prompt']
    assert 'plain unrecognizable nonfigurative charcoal weave patch' in flag
    assert 'badge' not in flag and 'emblem' not in flag


def test_source_reader_requires_real_alpha_for_only_known_cutouts(tmp_path):
    texture = 'NW_CITY_IVY_FULL_01.TGA'
    staging, store, raw, _ = source(tmp_path, texture=texture)
    with pytest.raises(ValueError, match='real RGBA PNG'):
        city_set.read_sources(staging/'sources', [texture], store)
    Image.new('RGBA', (16, 16), (100, 80, 60, 255)).save(raw)
    with pytest.raises(ValueError, match='requires visible foreground and transparent cutouts'):
        city_set.read_sources(staging/'sources', [texture], store)
    image = Image.new('RGBA', (16, 16), (100, 80, 60, 0))
    image.paste((100, 80, 60, 255), (0, 0, 8, 16))
    image.save(raw)
    assert len(city_set.read_sources(staging/'sources', [texture], store)[texture]) == 1


def test_alpha_import_keeps_rgba_and_records_failed_coverage_without_stopping(tmp_path, monkeypatch):
    mapping = mapping_with_targets(tmp_path)
    texture = 'NW_CITY_IVY_FULL_01.TGA'
    registry = registry_file(tmp_path, mapping, (texture,))
    staging, store, raw, _ = source(tmp_path, texture=texture)
    image = Image.new('RGBA', (16, 16), (0, 0, 0, 0))
    image.paste((120, 100, 80, 255), (0, 0, 4, 16))
    image.save(raw)
    original_source = raw.read_bytes()
    # Exercise processing and selection at the source dimensions; production
    # resize remains fixed to 1024 and is independently covered by core tests.
    monkeypatch.setattr(city_set.alpha_material, 'resize_rgba',
        lambda rgba: city_set.alpha_material.harden_and_dilate(rgba))
    imported = city_set.import_batch(staging, registry, textures=[texture],
        mapping=mapping, bible=BIBLE, source_root=store)[0]
    assert raw.read_bytes() == original_source
    assert imported['generation']['selected_failed']
    assert 'best available of 1' in imported['generation']['best_available']
    assert 'coverage' in imported['generation']['failure_warning']
    assert imported['generation']['brightness_correction']['alpha']['output_coverage'] == .25
    with Image.open(staging/'sources'/imported['files']['albedo']) as albedo:
        assert albedo.mode == 'RGBA'
        assert set(np.asarray(albedo)[..., 3].ravel()) == {0, 255}
    source(tmp_path, texture=texture, number=2)
    second = store/'city-3'/f'{Path(texture).stem}-attempt-2.png'
    image.paste((120, 100, 80, 255), (0, 0, 8, 16))
    image.save(second)
    imported = city_set.import_batch(staging, registry, textures=[texture],
        mapping=mapping, bible=BIBLE, source_root=store)[0]
    assert len(imported['generation']['attempts']) == 2
    assert imported['generation']['selected_attempt'] == 2
    assert not imported['generation'].get('selected_failed')
