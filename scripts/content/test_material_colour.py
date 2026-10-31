"""Saturation caps apply to stored pixels and preserve luminance and detail."""
from pathlib import Path
import sys

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent))
from material_colour import correct_mean_colour, limit_saturation, linear_to_oklab, oklab_to_linear


def linear(rgb):
    return np.where(rgb <= 0.04045, rgb / 12.92, ((rgb + 0.055) / 1.055) ** 2.4)


def saturation(rgb):
    rgb = np.asarray(rgb, dtype=float)
    hi, lo = rgb.max(axis=-1), rgb.min(axis=-1)
    return np.divide(hi - lo, hi, out=np.zeros_like(hi), where=hi > 0)


@pytest.mark.parametrize('limit', [0, 0.3, 0.4, 1])
def test_strong_colours_meet_cap_after_quantization(limit):
    source = np.array([[1., 0, 0], [0, 1, 0], [0, 0, 1], [.95, .3, .05], [.05, .1, .4]])
    result, metadata = limit_saturation(source, limit)
    assert result.dtype == np.uint8 and result.shape == source.shape
    assert saturation(result).mean() <= limit
    assert metadata['mean_hsv_s_after'] == pytest.approx(saturation(result).mean())
    assert metadata['threshold'] == limit
    assert 0 <= metadata['gray_blend_factor'] <= 1


def test_cap_is_mean_per_pixel_not_per_pixel_maximum():
    source = np.array([[.8, .6, .4]] + [[.5, .5, .5]] * 3)
    result, metadata = limit_saturation(source, .3)
    np.testing.assert_array_equal(result, np.rint(source * 255))
    assert saturation(result).max() > .3
    assert metadata['gray_blend_factor'] == 0


def test_quantization_cannot_take_boundary_colour_over_cap():
    source = np.array([[[1., .7, .7]]])
    assert saturation(source).mean() == pytest.approx(.3)
    assert saturation(np.rint(source * 255)).mean() > .3
    result, metadata = limit_saturation(source, .3)
    assert saturation(result).mean() <= .3
    assert metadata['gray_blend_factor'] > 0


def test_grey_black_and_white_are_unchanged_without_invalid_values():
    greys = np.array([0, .01, .1, .5, 1.])
    source = np.repeat(greys[:, None], 3, axis=1)
    result, metadata = limit_saturation(source, .3)
    np.testing.assert_array_equal(result, np.rint(source * 255))
    assert metadata['mean_hsv_s_before'] == metadata['mean_hsv_s_after'] == 0
    assert all(np.isfinite(value) for value in metadata.values() if isinstance(value, float))


def test_one_shared_blend_preserves_pixel_luminance_and_colour_structure():
    source = np.array([[[.9, .3, .1], [.4, .8, .3]], [[.2, .3, .85], [.8, .7, .1]]])
    result, metadata = limit_saturation(source, .3)
    weights = np.array([.2126, .7152, .0722])
    before = linear(source)
    after = linear(result.astype(float) / 255)
    luminance = before @ weights
    np.testing.assert_allclose(after @ weights, luminance, atol=.0045, rtol=0)
    # Recover the shared factor from independent coloured pixels, tolerating the
    # quantization of their channels. This catches per-pixel saturation clipping.
    chroma = before - luminance[..., None]
    retained = np.sum((after - luminance[..., None]) * chroma, axis=-1) / np.sum(chroma ** 2, axis=-1)
    np.testing.assert_allclose(retained, 1 - metadata['gray_blend_factor'], atol=.015, rtol=0)
    assert metadata['linear_mean_before'] == pytest.approx(luminance.mean())
    assert metadata['linear_mean_after'] == pytest.approx((after @ weights).mean())


def test_mixed_dark_pixels_and_random_colours_remain_finite_and_compliant():
    source = np.random.default_rng(8).random((19, 23, 3)) ** 4
    source[:3] = 0
    result, metadata = limit_saturation(source, .3)
    assert saturation(result).mean() <= .3
    assert np.isfinite(metadata['mean_hsv_s_after'])
    assert not result[:3].any()


@pytest.mark.parametrize('source, limit', [([], .3), ([[1, 0]], .3), ([[float('nan'), 0, 0]], .3),
                                           ([[2, 0, 0]], .3), ([[.1, 0, 0]], -.1),
                                           ([[.1, 0, 0]], float('nan'))])
def test_invalid_inputs_are_rejected(source, limit):
    with pytest.raises(ValueError):
        limit_saturation(source, limit)


def test_oklab_uses_linear_srgb_and_roundtrips_mean_colours():
    np.testing.assert_allclose(linear_to_oklab([1, 0, 0]), [.62795536, .22486306, .12584630], atol=1e-8)
    colours = np.array([[0, 0, 0], [1, 1, 1], [.08, .3, .12]])
    np.testing.assert_allclose(oklab_to_linear(linear_to_oklab(colours)), colours, atol=2e-15)


def test_mean_colour_uses_three_uniform_linear_gains_and_quantized_measurement():
    source = np.array([[[.3, .5, .6], [.4, .6, .5]], [[.6, .4, .3], [.5, .3, .4]]])
    before = linear(source)
    target = before.mean(axis=(0, 1))*[1.4, .6, 1.1]
    result, metadata = correct_mean_colour(source, linear_to_oklab(target))
    after = linear(result.astype(float)/255)
    np.testing.assert_allclose(after, before*metadata['channel_gains'], atol=.004)
    np.testing.assert_allclose(after.mean(axis=(0, 1)), target, atol=.005)
    np.testing.assert_allclose(metadata['linear_rgb_mean_after'], after.mean(axis=(0, 1)))
    # Averaging pixel OKLab is a different operation and must not be used.
    np.testing.assert_allclose(metadata['mean_oklab_after'], linear_to_oklab(after.mean(axis=(0, 1))))
    assert metadata['attainable'] and metadata['saturation']['cap_applied'] is False


def test_channel_gain_solver_accounts_for_clipping():
    source_linear = np.array([[.1, .3, .2], [.9, .5, .4]])
    source = np.where(source_linear <= .0031308, source_linear*12.92,
        1.055*source_linear**(1/2.4)-.055)
    target = np.array([.8, .4, .3])
    result, metadata = correct_mean_colour(source, linear_to_oklab(target))
    np.testing.assert_allclose(linear(result/255).mean(axis=0), target, atol=.005)
    assert metadata['channel_gains'][0] == pytest.approx(6.)
    assert metadata['clipped_channel_fraction'] == pytest.approx(1/6)


def test_colour_mask_excludes_background_without_modifying_weights():
    source = np.array([[.6, .3, .1], [1., 1., 1.]])
    mask = np.array([.5, 0.])
    target = np.array([.12, .07, .03])
    result, metadata = correct_mean_colour(source, linear_to_oklab(target), mask)
    np.testing.assert_array_equal(mask, [.5, 0.])
    np.testing.assert_allclose(linear(result[0]/255), target, atol=.005)
    np.testing.assert_allclose(metadata['linear_rgb_mean_before'], linear(source[0]))


def test_zero_channels_report_unreachable_colour_instead_of_creating_detail():
    result, metadata = correct_mean_colour(np.zeros((4, 3)), linear_to_oklab([.2, .1, .05]))
    assert not result.any()
    assert metadata['attainable'] is False
    assert metadata['linear_rgb_max_error'] == pytest.approx(.2)


@pytest.mark.parametrize('target, mask', [([float('nan'), 0, 0], None), ([.5, 2, 2], None),
    ([.5, 0, 0], [0, 0]), ([.5, 0, 0], [1, -1]), ([.5, 0, 0], [1])])
def test_invalid_colour_targets_and_masks_are_rejected(target, mask):
    with pytest.raises(ValueError):
        correct_mean_colour(np.full((2, 3), .5), target, mask)
