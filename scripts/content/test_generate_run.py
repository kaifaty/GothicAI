"""CPU regressions for numerical admission at the end of a FLUX retry series."""
from copy import deepcopy
from pathlib import Path
from types import SimpleNamespace
import sys

import numpy as np
from PIL import Image
import pytest

sys.path.insert(0, str(Path(__file__).parent))
import generate
import pack_materials


def material():
    return dict(id='city-1-ground', replaces=['GROUND.TGA'], seed=1, files={},
        parameters=dict(set='city-1', material='city-1-ground',
            luminance_range=[.06, .14], saturation_limit=.3,
            roughness_range=[.8, .95], roughness_base=.875),
        generation=dict(outcome='pending', attempts=[], attempt_limit=3))


@pytest.fixture
def runtime(monkeypatch, tmp_path):
    """Replace only GPU/model plumbing; filtering and PNG/registry writes are real."""
    monkeypatch.setattr(generate.platform, 'system', lambda: 'Linux')
    monkeypatch.setattr(generate.Path, 'home', classmethod(lambda cls: tmp_path))
    root = tmp_path/'gothic-ai/materials'
    images, calls, loads = [], [], []

    class FakeGenerator:
        def __init__(self, device):
            assert device == 'cpu'

        def manual_seed(self, seed):
            return self

    class Embedding:
        def to(self, *args, **kwargs):
            return self

    class Pipeline:
        transformer = SimpleNamespace(dtype='fake', _tile_step=0)

        def __call__(self, **kwargs):
            calls.append(kwargs)
            return SimpleNamespace(images=[images.pop(0)])

    def load_flux(records, root, pending):
        loads.append(len(pending))
        return Pipeline(), {r['parameters']['material']: (Embedding(), Embedding()) for r in pending}, {
            r['parameters']['material']: {'fake_cpu_test': True} for r in pending}

    torch = SimpleNamespace(set_num_threads=lambda n: None,
        backends=SimpleNamespace(cuda=SimpleNamespace(matmul=SimpleNamespace(allow_tf32=False))),
        cuda=SimpleNamespace(reset_peak_memory_stats=lambda: None, max_memory_allocated=lambda: 0),
        Generator=FakeGenerator)
    monkeypatch.setitem(sys.modules, 'torch', torch)
    monkeypatch.setattr(generate.bakeoff, 'gpu_state', lambda *args: None)
    monkeypatch.setattr(generate.bakeoff, 'load_flux', load_flux)
    return SimpleNamespace(root=root, registry=tmp_path/'registry.jsonl', images=images,
        calls=calls, loads=loads)


def candidate(runtime, record, colour, number, visual_reason='large isolated patches'):
    image = Image.new('RGB', (32, 32), colour)
    maps, correction, rejection = generate.corrected_maps(image, record)
    return dict(number=number, seed=number, outcome='rejected',
        reason=rejection or visual_reason, brightness_correction=correction,
        files=generate.save_attempt(runtime.root, record, maps, number))


def test_three_real_black_attempts_fail_and_retry_does_not_reload_flux(runtime):
    record = material()
    runtime.images.extend([Image.new('RGB', (32, 32), 'black') for _ in range(3)])
    for retry in range(2):
        with pytest.raises(RuntimeError, match='3 attempts.*no numerically valid candidate.*brightness'):
            generate.run([record], runtime.registry, runtime.root, 'city-1')
        assert record['generation']['outcome'] == 'pending'
        assert record['files'] == {}
        assert len(record['generation']['attempts']) == 3
        assert len(runtime.calls) == 3 and runtime.loads == [1]
    saved = generate.bakeoff.read_registry(runtime.registry)[0]
    assert saved == record
    mapping = runtime.registry.with_name('mapping.csv')
    mapping.write_text('texture,class,scale,decision\nGROUND.TGA,mud,1,include\n')
    with pytest.raises(ValueError, match='missing completed materials for GROUND.TGA'):
        pack_materials.select_set_records([saved], 'city-1', mapping)
    for attempt in saved['generation']['attempts']:
        assert attempt['outcome'] == 'rejected' and not attempt['numeric_valid']
        assert attempt['numeric_rejection'] == 'brightness correction could not reach class range'
        assert attempt['brightness_correction']['linear_mean_after'] == 0
        assert all((runtime.root/'out'/path).is_file() for path in attempt['files'].values())
        assert (runtime.root/'out'/Path(attempt['files']['albedo']).parent/'source.png').is_file()


def test_exhausted_mixed_series_selects_only_numerically_valid_visual_reject(runtime):
    record = material()
    attempts = [candidate(runtime, record, 'black', 1),
        candidate(runtime, record, (90, 90, 90), 2), candidate(runtime, record, 'black', 3)]
    # Numerical metadata is checked again; an old or stale boolean is no proof.
    for attempt in attempts:
        attempt.update(numeric_valid=True, numeric_rejection=None)
    record['generation']['attempts'] = attempts
    generate.run([record], runtime.registry, runtime.root, 'city-1')
    assert record['generation']['outcome'] == 'complete'
    assert record['generation']['selected_attempt'] == 2
    assert record['files'] == attempts[1]['files']
    assert [a['numeric_valid'] for a in attempts] == [False, True, False]
    assert attempts[1]['outcome'] == 'rejected'  # Preserve its visual review.
    assert 'best numerically valid' in record['generation']['selection_note']
    assert runtime.loads == [] and runtime.calls == []


def test_exhausted_real_colour_recipe_retains_valid_visual_reject_without_loading_flux(runtime):
    root = Path(__file__).resolve().parents[2]
    record = generate.recipes(root/'data/content/city-materials.csv', root/'docs/content-style.md', 'city-1')[0]
    assert 'target_oklab' in record['parameters']
    assert 'target_luminance' not in record['parameters']
    attempts = [candidate(runtime, record, colour, number) for number, colour in enumerate(
        ((120, 100, 80), (120, 100, 80), 'black'), 1)]
    assert attempts[0]['brightness_correction']['target'] is None
    record['generation']['attempts'] = attempts
    generate.run([record], runtime.registry, runtime.root, 'city-1')
    assert record['generation']['outcome'] == 'complete'
    assert record['generation']['selected_attempt'] == 1
    assert record['files'] == attempts[0]['files']
    assert [attempt['numeric_valid'] for attempt in attempts] == [True, True, False]
    assert generate.bakeoff.read_registry(runtime.registry) == [record]
    assert runtime.loads == [] and runtime.calls == []


@pytest.mark.parametrize('valid', [True, False])
def test_colour_fallback_ranks_against_current_rgb_target_not_cached_error_or_luminance(runtime, valid):
    from material_colour import _linear, linear_to_oklab
    record = material()
    target = _linear(np.array([.7, .24, .1]))
    record['parameters'].update(set='city-2', target_oklab=linear_to_oklab(target).tolist(),
        target_luminance=.9)
    attempts = [candidate(runtime, record, (100, 100, 100), number) for number in (1, 2, 3)]
    errors = (.004, .001) if valid else (.03, .01)
    for attempt, error in zip(attempts[:2], errors):
        correction = attempt['brightness_correction']
        mean = target + [error, 0, 0]
        correction['linear_rgb_mean_after'] = mean.tolist()
        correction['linear_mean_after'] = correction['saturation']['linear_mean_after'] = float(mean @ [.2126, .7152, .0722])
        # Both legacy luminance and stale cached colour errors favour attempt 1.
        correction['linear_rgb_max_error'] = .1-error
        correction['target_linear_rgb'] = (target+[errors[0], 0, 0]).tolist()
    attempts[2] = candidate(runtime, record, 'black', 3)
    record['generation']['attempts'] = attempts
    generate.run([record], runtime.registry, runtime.root, 'city-2')
    assert record['generation']['selected_attempt'] == 2
    assert [a['numeric_valid'] for a in attempts] == [valid, valid, False]
    assert record['generation']['selected_failed'] is True  # Its visual rejection remains recorded.
    assert ('failure_warning' in record['generation']) is not valid
    assert runtime.loads == [] and runtime.calls == []


def test_all_visual_rejects_can_retain_best_available_without_loading_flux(runtime):
    record = material()
    attempts = [candidate(runtime, record, (v, v, v), n) for n, v in enumerate((75, 85, 99), 1)]
    record['generation']['attempts'] = attempts
    generate.run([record], runtime.registry, runtime.root, 'city-1')
    assert record['generation']['outcome'] == 'complete'
    assert all(a['numeric_valid'] for a in attempts)
    selected = attempts[record['generation']['selected_attempt']-1]
    assert record['files'] == selected['files']
    assert selected['reason'] == 'large isolated patches'
    assert runtime.loads == [] and runtime.calls == []


def test_final_quantized_luminance_after_saturation_is_the_admission_measurement():
    record = material()
    image = Image.new('RGB', (32, 32), (150, 65, 30))
    rgb = np.asarray(image, dtype=np.float64)/255
    linear = np.where(rgb <= .04045, rgb/12.92, ((rgb+.055)/1.055)**2.4)
    before = float((linear @ np.array([.2126, .7152, .0722])).mean())
    record['parameters']['luminance_range'] = [before-1e-8, before+1e-8]
    maps, correction, rejection = generate.corrected_maps(image, record)
    rgb = np.rint(maps['albedo']*255).astype(np.float64)/255
    linear = np.where(rgb <= .04045, rgb/12.92, ((rgb+.055)/1.055)**2.4)
    stored_y = float((linear @ np.array([.2126, .7152, .0722])).mean())
    assert correction['linear_mean_after'] == pytest.approx(stored_y, abs=1e-12)
    assert abs(stored_y-before) > 1e-8
    assert rejection == 'brightness correction could not reach class range'


def test_original_mean_colour_bypasses_saturation_cap_and_is_checked_on_stored_pixels():
    from material_colour import _linear, linear_to_oklab
    record = material()
    target = _linear(np.array([.7, .24, .1]))
    record['parameters'].update(target_oklab=linear_to_oklab(target).tolist(),
        target_luminance=float(target @ [.2126, .7152, .0722]))
    maps, correction, rejection = generate.target_corrected_maps(
        Image.new('RGB', (16, 16), (100, 100, 100)), record)
    assert rejection is None
    assert correction['saturation']['mean_hsv_s_after'] > .3
    assert correction['saturation']['cap_applied'] is False
    stored = np.rint(maps['albedo']*255).astype(float)/255
    np.testing.assert_allclose(_linear(stored).mean(axis=(0, 1)), target, atol=.005)
    correction['linear_rgb_mean_after'][0] += .02
    assert 'colour correction' in generate.numeric_rejection(record, correction)


def test_original_mean_colour_supersedes_legacy_alpha_weighted_luminance():
    from material_colour import _linear, linear_to_oklab
    record = material()
    target = _linear(np.array([.7, .24, .1]))
    record['parameters'].update(target_oklab=linear_to_oklab(target).tolist(),
        target_luminance=.9, target_luminance_tolerance=.002)
    _, correction, rejection = generate.target_corrected_maps(
        Image.new('RGB', (16, 16), (100, 100, 100)), record)
    assert rejection is None
    assert correction['linear_mean_after'] < .2
    np.testing.assert_allclose(correction['linear_rgb_mean_after'], target, atol=.005)
    assert abs(correction['linear_mean_after']-.9) > .002
    correction['saturation']['linear_mean_after'] = float('nan')
    assert generate.numeric_rejection(record, correction) == 'final mean luminance is not finite or is not measured'


@pytest.mark.parametrize('change', ['saturation', 'clipping'])
def test_select_attempt_cannot_bypass_numerical_constraints(runtime, change):
    record = material()
    attempt = candidate(runtime, record, (90, 90, 90), 1)
    if change == 'saturation':
        attempt['brightness_correction']['saturation']['mean_hsv_s_after'] = .31
    else:
        attempt['brightness_correction']['clipped_channel_fraction'] = .41
    snapshot = deepcopy(record)
    with pytest.raises(ValueError, match='cannot select numerically invalid attempt'):
        generate.select_attempt(record, attempt)
    assert record == snapshot


def test_city2_exhausted_numerical_failures_keep_warned_best_without_reloading_gpu(runtime):
    record = material()
    record['parameters'].update(set='city-2', target_luminance=.18, target_luminance_tolerance=.002)
    record['generation']['attempts'] = [candidate(runtime, record, 'black', n) for n in (1, 2, 3)]
    generate.run([record], runtime.registry, runtime.root, 'city-2')
    assert record['generation']['outcome'] == 'complete'
    assert record['generation']['selected_failed'] is True
    assert '3 attempts' in record['generation']['failure_warning']
    assert record['files'] == record['generation']['attempts'][0]['files']
    assert len(record['generation']['attempts']) == 3
    assert runtime.calls == runtime.loads == []


def test_city2_third_visual_rejection_retains_warned_candidate_instead_of_requiring_decision(runtime):
    record = material()
    record['parameters'].update(set='city-2', target_luminance=.18)
    attempts = [candidate(runtime, record, (100,100,100), n) for n in (1, 2, 3)]
    attempts[-1]['outcome'] = 'complete'
    record['generation'].update(attempts=attempts, outcome='complete')
    record['files'] = attempts[-1]['files']
    generate.reject([record], record['id'], 'opening placement is imperfect')
    assert len(record['generation']['attempts']) == 3
    assert record['generation']['outcome'] == 'complete'
    assert record['generation']['selected_failed'] is True
    assert 'Visual review' in record['generation']['failure_warning']
    assert attempts[-1]['reason'] == 'opening placement is imperfect'
