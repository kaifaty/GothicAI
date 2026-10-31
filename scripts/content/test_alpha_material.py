"""The visible silhouette owns colour admission and transparent RGB padding."""
import numpy as np
import pytest

import alpha_material


def record(target=.18, coverage=.5):
    return dict(parameters=dict(target_luminance=target, target_luminance_tolerance=.002,
        target_coverage=coverage, coverage_tolerance=.1, saturation_limit=.3,
        luminance_range=[.25, .4], roughness_range=[.85, .95], roughness_base=.9,
        tiling_axes=''))


def rgba(background=(0, 0, 0), width=16):
    value = np.zeros((16, width, 4), dtype=np.uint8)
    value[..., :3] = background
    value[:, :width//2] = (160, 95, 50, 255)
    return value


def test_engine_cutoff_boundary_is_identical_for_byte_and_float_alpha():
    assert alpha_material.coverage(np.array([0, 127, 128, 255], dtype=np.uint8)) == .5
    assert alpha_material.coverage(np.array([0., .499, .5, 1.])) == .5
    assert alpha_material.ALPHA_TEST_THRESHOLD == .5


def test_hardening_preserves_mask_and_dilates_nearest_foreground_rgb():
    value = np.array([[[30, 70, 90, 127], [120, 80, 40, 128], [250, 10, 250, 0]]], dtype=np.uint8)
    before = value.copy()
    result = alpha_material.harden_and_dilate(value)
    assert np.array_equal(value, before)
    assert result[..., 3].tolist() == [[0, 255, 0]]
    assert result[..., :3].tolist() == [[[120, 80, 40]]*3]


def test_transparent_background_rgb_does_not_dilute_visible_colour_or_maps():
    dark, dark_correction, dark_rejection = alpha_material.corrected_maps(rgba(), record())
    bright, bright_correction, bright_rejection = alpha_material.corrected_maps(rgba((255, 0, 255)), record())
    assert dark_rejection is bright_rejection is None
    assert np.array_equal(dark['albedo'], bright['albedo'])
    assert dark_correction == bright_correction
    correction = dark_correction
    assert abs(correction['linear_mean_after']-.18) <= .002
    assert correction['saturation']['mean_hsv_s_after'] <= .3
    assert correction['alpha']['output_coverage'] == .5
    assert dark['albedo'].shape == (16, 16, 4)
    for name in ('normal', 'height', 'roughness', 'metallic', 'ao'):
        assert np.array_equal(dark[name], bright[name])


def test_resize_retains_hard_alpha_and_foreground_padding():
    result = alpha_material.resize_rgba(rgba(), (32, 32))
    assert result.shape == (32, 32, 4)
    assert set(result[..., 3].ravel()) == {0, 255}
    assert alpha_material.coverage(result[..., 3]) == .5
    assert np.array_equal(result[0, -1, :3], result[0, 0, :3])


def test_coverage_admission_uses_absolute_ten_point_tolerance():
    _, correction, rejection = alpha_material.corrected_maps(rgba(), record(coverage=.7))
    assert correction['alpha']['absolute_error'] == pytest.approx(.2)
    assert 'visible alpha coverage' in rejection
    assert '0.10' in rejection


def test_no_visible_pixels_cannot_supply_colour_for_dilation():
    with pytest.raises(ValueError, match='no visible pixels'):
        alpha_material.harden_and_dilate(np.zeros((2, 2, 4), dtype=np.uint8))


def test_measured_foreground_colour_bypasses_saturation_cap_without_changing_alpha():
    from material_colour import _linear, linear_to_oklab
    target = _linear(np.array([.7, .24, .1]))
    value = record(target=float(target @ alpha_material.LUMINANCE))
    value['parameters']['target_oklab'] = linear_to_oklab(target).tolist()
    maps, correction, rejection = alpha_material.corrected_maps(rgba(), value)
    assert rejection is None
    mask = maps['albedo'][..., 3] == 1
    assert mask.mean() == .5
    actual = _linear(np.rint(maps['albedo'][mask, :3]*255).astype(float)/255).mean(axis=0)
    np.testing.assert_allclose(actual, target, atol=.005)
    assert correction['saturation']['mean_hsv_s_after'] > .3
    assert correction['saturation']['cap_applied'] is False
