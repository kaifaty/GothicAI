"""Set recipes, physical levels and retained retries without loading FLUX."""
from pathlib import Path
from copy import deepcopy
import csv
import sys

import numpy as np
from PIL import Image
import pytest

sys.path.insert(0, str(Path(__file__).parent))
import generate
import registry

ROOT = Path(__file__).resolve().parents[2]


def recipes():
    records = generate.recipes(ROOT/'data/content/city-materials.csv', ROOT/'docs/content-style.md', 'city-1')
    # These cases retain the historical class/saturation policy. Measured
    # original colour has separate admission tests and overrides that policy.
    for record in records:
        record['parameters'].pop('target_oklab', None)
        record['parameters'].pop('colour_policy', None)
    return records


def test_real_inputs_cover_includes_with_unique_pinned_text_recipes():
    records = recipes()
    with (ROOT/'data/content/city-materials.csv').open(newline='') as stream:
        included = {row['texture'] for row in csv.DictReader(stream) if row['decision'] == 'include'
                    and not row['texture'].startswith('NW_MISC_')}
    assert {r['replaces'][0] for r in records} == included
    assert len({r['seed'] for r in records}) == len({r['id'] for r in records}) == len(included)
    for record in records:
        registry.validate_entry(record)
        assert record['parameters']['state'] in record['prompt']
        assert '<' not in record['prompt']
        assert record['parameters']['steps'] == 4
        assert record['parameters']['guidance_scale'] == 0
        assert record['models'][0]['revision'] == generate.bakeoff.FLUX_REVISION


def test_module_panel_strip_and_atlas_decisions_do_not_enter_tile_generation(tmp_path):
    mapping = tmp_path/'materials.csv'
    mapping.write_text(
        'texture,decision,class,state,scale,anti_repeat\n'
        'PAVING.TGA,include,cobble,very worn,1,on\n'
        'FRAME.TGA,module,timber_frame_tile,dirty edges,1,off\n'
        'INFILL.TGA,panel,plaster_panel,dirty edges,1,off\n'
        'BEAM.TGA,strip,timber_strip,very worn,1,off\n'
        'DOOR.TGA,exclude,,,,off\n'
    )
    records = generate.recipes(mapping, ROOT/'docs/content-style.md', 'test')
    assert [record['replaces'] for record in records] == [['PAVING.TGA']]


def test_prepare_preserves_progress_and_detects_changed_inputs(tmp_path):
    path = tmp_path/'registry.jsonl'
    mapping, bible = ROOT/'data/content/city-materials.csv', ROOT/'docs/content-style.md'
    records = generate.prepare(path, mapping, bible, 'city-1')
    records[0]['generation']['attempts'].append({'seed': 1, 'outcome': 'rejected'})
    generate.bakeoff.write_registry(path, records)
    assert generate.prepare(path, mapping, bible, 'city-1')[0]['generation']['attempts']
    copy = tmp_path/'style.md'
    copy.write_text(bible.read_text().replace('very unlikely string', 'unused')+'\n')
    with pytest.raises(ValueError, match='inputs changed'):
        generate.prepare(path, mapping, copy, 'city-1')


@pytest.mark.parametrize('colour', [(30, 24, 20), (230, 220, 200), (110, 95, 75)])
def test_level_correction_and_quantized_maps_obey_class_ranges(colour):
    for record in recipes():
        maps, correction, rejection = generate.corrected_maps(Image.new('RGB', (32, 32), colour), record)
        lo, hi = record['parameters']['luminance_range']
        assert lo <= correction['linear_mean_after'] <= hi
        assert rejection is None
        rlo, rhi = record['parameters']['roughness_range']
        stored = np.rint(maps['roughness']*255)/255
        assert stored.min() >= rlo and stored.max() <= rhi
        assert np.all(maps['metallic'] == 0)


def test_retry_preserves_rejected_attempt_and_enforces_three_attempts():
    record = recipes()[0]
    record['generation'] = dict(outcome='complete', attempts=[dict(seed=record['seed'], outcome='complete', files={'source': 'tile.png'})])
    record['files'] = {'albedo': 'tile.png'}
    generate.reject([record], record['id'], 'looks unlike floorboards')
    assert record['generation']['attempts'][0]['files'] == {'source': 'tile.png'}
    assert record['generation']['attempts'][0]['outcome'] == 'rejected'
    assert record['files'] == {}
    assert record['seed'] != generate.seed_for(record['replaces'][0], 2)
    record['generation'].update(outcome='complete', attempts=[{}]*3)
    with pytest.raises(ValueError, match='three attempts exhausted'):
        generate.reject([record], record['id'], 'wrong class')


def test_black_source_is_rejected_instead_of_claiming_valid_colour():
    _, _, rejection = generate.corrected_maps(Image.new('RGB', (32, 32), 'black'), recipes()[0])
    assert rejection == 'brightness correction could not reach class range'


def test_prompt_rules_and_saturation_thresholds_come_from_bible():
    for record in recipes():
        cls = record['parameters']['material_class']
        styles, _ = generate.read_style(ROOT/'docs/content-style.md')
        assert record['parameters']['saturation_limit'] == styles[cls]['saturation_limit']
        assert 'evenly distributed small details, no large isolated features' in record['prompt']
        if cls in ('mud', 'dirt_path'):
            assert 'grime in the cracks' not in record['prompt']
            assert 'continuous' in record['prompt']


def test_explicit_refresh_preserves_history_and_grants_only_three_new_attempts(tmp_path):
    records = recipes()
    target = records[0]
    for record in records:
        record['parameters'].pop('saturation_limit')
        record['prompt'] = 'previous real prompt'
        record['files'] = {key: 'old/'+key+'.png' for key in generate.bakeoff.MAPS}
        record['generation'] = dict(outcome='complete', attempts=[dict(number=n, seed=n, outcome='complete', files=deepcopy(record['files'])) for n in (1,2,3)])
    old = deepcopy(records)
    mapping, bible = ROOT/'data/content/city-materials.csv', ROOT/'docs/content-style.md'
    generate.refresh(records, mapping, bible, 'city-1', target['replaces'], 'user revised prompt')
    assert target['generation']['attempt_limit'] == 6
    assert target['generation']['series_start'] == 3
    assert target['generation']['outcome'] == 'pending' and target['files'] == {}
    assert target['generation']['recipes'][0]['prompt'] == 'previous real prompt'
    assert len(target['generation']['recipes']) == 2
    assert all(a['recipe_revision']==1 for a in target['generation']['attempts'])
    assert target['generation']['previous_selections'][0]['files'] == old[0]['files']
    assert records[1]['prompt'] == old[1]['prompt'] and records[1]['files'] == old[1]['files']
    assert records[1]['generation']['outcome'] == 'complete'
    snapshot = deepcopy(records)
    generate.refresh(records, mapping, bible, 'city-1', target['replaces'], 'repeat')
    assert records == snapshot
    path = tmp_path/'registry.jsonl'
    generate.bakeoff.write_registry(path, records)
    assert generate.prepare(path, mapping, bible, 'city-1') == records


def test_postprocess_changes_only_albedo_and_is_idempotent(tmp_path):
    record = recipes()[0]
    record['generation'].update(outcome='complete', prepared_inputs='a'*64)
    record['files'] = {key: 'source/'+key+'.png' for key in generate.bakeoff.MAPS}
    source = tmp_path/'out/source'
    source.mkdir(parents=True)
    for key in generate.bakeoff.MAPS:
        Image.new('RGB', (16,16), (180,40,12)).save(source/(key+'.png'))
    before = deepcopy(record)
    old_bytes = {p.name:p.read_bytes() for p in source.iterdir()}
    generate.postprocess([record], tmp_path, 'city-1')
    assert record['files']['albedo'] != before['files']['albedo']
    assert all(record['files'][k] == before['files'][k] for k in generate.bakeoff.MAPS if k!='albedo')
    assert record['seed']==before['seed'] and record['prompt']==before['prompt']
    assert old_bytes == {p.name:p.read_bytes() for p in source.iterdir()}
    assert record['generation']['saturation_correction']['mean_hsv_s_after'] <= .3
    snapshot = deepcopy(record)
    generate.postprocess([record], tmp_path, 'city-1')
    assert record == snapshot


def test_refresh_other_texture_after_acknowledging_its_postprocess_inputs(tmp_path):
    records = recipes()[:2]
    for record in records:
        record['prompt'] = 'previous real prompt'
        record['parameters'].pop('saturation_limit')
        record['files'] = {key: 'old/'+key+'.png' for key in generate.bakeoff.MAPS}
        record['generation'] = dict(outcome='complete', attempts=[dict(
            number=n, seed=n, outcome='complete', files=deepcopy(record['files'])) for n in (1, 2, 3)])
    mapping, bible = ROOT/'data/content/city-materials.csv', ROOT/'docs/content-style.md'
    generate.refresh(records, mapping, bible, 'city-1', records[0]['replaces'], 'revise first')
    first = deepcopy(records[0])
    second = deepcopy(records[1])
    assert second['generation']['prepared_inputs']
    assert 'attempt_limit' not in second['generation']
    # The same distinction must survive saving/loading between separate CLI calls.
    path = tmp_path/'registry.jsonl'
    generate.bakeoff.write_registry(path, records)
    records = generate.bakeoff.read_registry(path)
    generate.refresh(records, mapping, bible, 'city-1', records[1]['replaces'], 'revise second')
    assert records[0] == first
    refreshed = records[1]
    assert refreshed['prompt'] == recipes()[1]['prompt']
    assert refreshed['generation']['outcome'] == 'pending'
    assert refreshed['generation']['attempt_limit'] == 6
    assert refreshed['generation']['series_start'] == 3
    assert refreshed['files'] == {}
    assert refreshed['generation']['previous_selections'][0]['files'] == second['files']
    assert refreshed['generation']['recipes'][0]['prompt'] == second['prompt']
    assert len(refreshed['generation']['recipes']) == 2
    snapshot = deepcopy(records)
    for record in records:
        generate.refresh(records, mapping, bible, 'city-1', record['replaces'], 'repeat')
    assert records == snapshot


def test_refresh_preserves_budget_already_granted_by_older_generator():
    records = recipes()[:2]
    for record in records:
        record['prompt'] = 'previous real prompt'
    mapping, bible = ROOT/'data/content/city-materials.csv', ROOT/'docs/content-style.md'
    generate.refresh(records, mapping, bible, 'city-1', records[0]['replaces'], 'previous generator')
    del records[0]['generation']['refreshed_inputs']
    snapshot = deepcopy(records)
    generate.refresh(records, mapping, bible, 'city-1', records[0]['replaces'], 'repeat after upgrade')
    assert records == snapshot


def city2_recipes(tmp_path):
    source = ROOT/'data/content/city-materials.csv'
    with source.open(newline='') as stream:
        reader = csv.DictReader(stream)
        fields = list(reader.fieldnames)
        rows = list(reader)
    if 'target_luminance' not in fields:
        fields.append('target_luminance')
    mapping = tmp_path/'mapping.csv'
    with mapping.open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            row['target_luminance'] = '.18'
            for field in ('target_oklab_l', 'target_oklab_a', 'target_oklab_b'):
                if field in row:
                    row[field] = ''
            writer.writerow(row)
    return generate.recipes(mapping, ROOT/'docs/content-style.md', 'city-2')


def test_city2_preserves_facade_layout_and_generates_only_twenty_two_unique_surfaces(tmp_path):
    records = city2_recipes(tmp_path)
    assert len(records) == len({r['replaces'][0] for r in records}) == 22
    modules = [r for r in records if r['parameters']['decision'] != 'include']
    assert len(modules) == 6
    for record in modules:
        params = record['parameters']
        assert params['scale'] == 1 and params['antitile'] == 'off'
        assert params['depicts'] in record['prompt']
        assert 'evenly distributed small details' not in record['prompt']
        assert 'material albedo texture, surface fills the entire image, no background' in record['prompt']
        assert params['source_width']/params['source_height'] == pytest.approx(params['source_aspect'], rel=.015)
        assert params['source_width'] % 16 == params['source_height'] % 16 == 0
        assert params['tiling_axes'] == ('uv' if params['layout'] == 'tiled' else
            'v' if params['layout'] == 'strip' else '')
    beam = next(r for r in modules if r['replaces'] == ['NW_CITY_WOODBEAM_02.TGA'])
    assert beam['parameters']['source_height'] > 8*beam['parameters']['source_width']
    for record in records:
        params = record['parameters']
        assert params['target_luminance'] == .18
        if params['decision'] == 'include':
            assert params['antitile'] in ('rotate', 'offset')
        if params['material_class'] == 'flagstone':
            assert 'not a regular grid' in record['prompt']
        if params['material_class'] == 'roof_tiles':
            assert params['saturation_limit'] == .25


def test_city2_target_takes_precedence_over_class_range_and_is_measured_after_quantization(tmp_path):
    record = city2_recipes(tmp_path)[0]
    record['parameters'].update(target_luminance=.32, luminance_range=[.06, .14])
    maps, correction, rejection = generate.corrected_maps(Image.new('RGB', (32,32), (190,80,30)), record)
    assert rejection is None
    assert abs(correction['linear_mean_after']-.32) <= .002
    assert correction['outside_class_range'] and correction['class_range_advisory']
    stored = np.rint(maps['albedo']*255)/255
    linear = np.where(stored <= .04045, stored/12.92, ((stored+.055)/1.055)**2.4)
    assert float((linear @ np.array([.2126,.7152,.0722])).mean()) == pytest.approx(
        correction['linear_mean_after'], abs=1e-7)
