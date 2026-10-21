"""Synthetic albedo tests keep fitting independent from original game images."""
import numpy as np
import pytest

from material_colour import _linear, _srgb, linear_to_oklab
from material_detail import (FIELDS, _highlight_curve, fit_material_detail,
                             limit_material_highlights, low_frequency_iid)
from material_measures import measure_material


def bands_image(amplitude, size=1024):
    y, x = np.mgrid[:size, :size]
    bands = (.22*np.cos(2*np.pi*16*x/size) + .15*np.cos(2*np.pi*48*y/size)
             + .10*np.cos(2*np.pi*96*(x+y)/size))
    luminance = np.exp(-2+amplitude*bands)-.001
    linear = luminance[..., None]*np.array([1.25, .95, .75])
    return np.rint(_srgb(linear)*255).astype(np.uint8)


@pytest.mark.parametrize("gray", [0, 83, 255])
def test_flat_image_is_unchanged(gray):
    source = np.full((64, 64, 3), gray, dtype=np.uint8)
    measured = measure_material(source)
    result, metadata = fit_material_detail(source/255, {field: 0 for field in FIELDS},
                                          target_oklab=measured["oklab"])
    np.testing.assert_array_equal(result, source)
    assert metadata["converged"]
    assert metadata["iterations"] == 0
    assert metadata["gains"] == dict(g0=1, g1=1, g2=1, g3=1)
    assert metadata["shoulder_pixel_fraction"] == (1 if gray == 255 else 0)


@pytest.mark.parametrize("amplitude", [.5, 1.5])
def test_band_amplitudes_fit_targets_in_both_directions_and_keep_mean_colour(amplitude):
    reference = measure_material(bands_image(1))
    targets = {field: reference[field] for field in FIELDS}
    result, metadata = fit_material_detail(bands_image(amplitude)/255, targets,
                                          target_oklab=reference["oklab"])
    measured = measure_material(result)
    assert metadata["converged"]
    assert metadata["iterations"] <= 8
    for field in FIELDS:
        assert measured[field] == pytest.approx(targets[field], rel=.03)
        assert metadata["measured"][field] == measured[field]
    assert metadata["gains"]["g1"] > 1 if amplitude < 1 else metadata["gains"]["g1"] < 1
    assert metadata["subcell_gain"] == min(metadata["gains"]["g1"], 1.5)
    assert max(abs(np.array(measured["mean_linear_rgb"])-reference["mean_linear_rgb"])) <= .005
    assert metadata["colour"]["linear_rgb_max_error"] <= .005


def test_unreachable_detail_reports_bounded_gains_and_residual_ratios():
    source = bands_image(.5)
    measured = measure_material(source)
    targets = {field: measured[field]*10 for field in FIELDS}
    _, metadata = fit_material_detail(source/255, targets, target_oklab=measured["oklab"])
    assert metadata["converged"] is False
    assert metadata["iterations"] == 8
    assert metadata["limits"] == dict(g0="maximum", g1="maximum", g2="maximum", g3="maximum")
    assert metadata["subcell_gain"] == 1.5
    assert max(metadata["ratios"].values()) < .5
    assert 0 <= metadata["clipped_pixel_fraction"] <= 1


def test_shoulder_fraction_uses_final_gains_before_colour_correction():
    # Alternating pixels have no broad bands: only the subcell gain changes
    # their contrast. At 1.5 it lifts the bright half above linear RGB 0.7.
    linear = np.broadcast_to(np.where(np.arange(64) % 2, .6, .04), (64, 64))
    source = np.repeat(np.rint(_srgb(linear)*255).astype(np.uint8)[..., None], 3, axis=-1)
    before = measure_material(source)
    assert np.max(_linear(source/255)) < .7

    _, metadata = fit_material_detail(source/255,
        {field: before[field]*10 for field in FIELDS}, target_oklab=before["oklab"])

    assert metadata["subcell_gain"] == 1.5
    assert metadata["shoulder_pixel_fraction"] == .5


@pytest.mark.parametrize("channel_scales", [[1, 1, 1], [1, .55, .3]], ids=["neutral", "coloured"])
def test_bright_half_retains_mean_colour_without_clipping_when_detail_is_amplified(channel_scales):
    size = 1024
    y, x = np.mgrid[:size, :size]
    linear = np.where(x < size//2, .04, .72)*np.exp(.10*np.cos(2*np.pi*32*y/size))
    source = np.rint(_srgb(linear[..., None]*channel_scales)*255).astype(np.uint8)
    before = measure_material(source)
    targets = {field: before[field]*2 for field in FIELDS}

    result, metadata = fit_material_detail(source/255, targets, target_oklab=before["oklab"])
    after = measure_material(result)

    # The bright plateau exceeds linear 1 under this base gain without the
    # shoulder; retaining the source mean alone would leave that detail clipped.
    assert metadata["gains"]["g0"] >= 1.3
    assert metadata["gains"]["g1"] > 1
    assert metadata["clipped_pixel_fraction"] <= .01
    assert metadata["colour"]["clipped_channel_fraction"] <= .01
    assert max(abs(np.array(after["mean_linear_rgb"])-before["mean_linear_rgb"])) <= .005


def test_low_frequency_iid_keeps_four_pixel_log_band_within_two_percent():
    size = 1024
    x = np.arange(size)[None, :]
    fine = np.cos(2*np.pi*x/4)
    broad = np.cos(2*np.pi*x/size)

    def image(logs):
        linear = np.broadcast_to(np.exp(logs)-.001, (size, size))
        return np.repeat(np.rint(_srgb(linear)*255).astype(np.uint8)[..., None], 3, axis=-1)

    source = image(-2+.16*fine+.25*broad)
    iid = image(-2+.4*broad)
    result, metadata = low_frequency_iid(source/255, iid/255)

    def amplitude(pixels):
        logs = np.log((_linear(pixels/255) @ np.array([.2126, .7152, .0722]))+.001)
        return float(2*np.mean(logs*fine))

    assert amplitude(result)/amplitude(source) == pytest.approx(1, rel=.02)
    assert not np.array_equal(result, source)
    assert metadata["gaussian_sigma_px"] == 32
    assert metadata["boundary"] == "wrap"


@pytest.mark.parametrize("amplitude", [.5, 1.5])
def test_ceiling_measures_recoloured_quantized_pixels_without_any_overshoot(amplitude):
    source = bands_image(amplitude, size=256)
    before = measure_material(source)
    targets = {field: before[field]*.35 for field in FIELDS}
    target_colour = linear_to_oklab(np.array(before["mean_linear_rgb"])*[1.15, .8, 1.1])

    result, metadata = fit_material_detail(source/255, targets,
        target_oklab=target_colour, ceiling=True)
    measured = measure_material(result)

    assert result.dtype == np.uint8
    assert metadata["ceiling"]["feasible"]
    assert min(metadata["gains"].values()) < .5
    for field in FIELDS:
        assert measured[field] <= targets[field]
        assert metadata["measured"][field] == measured[field]
        assert metadata["ceiling"]["before"][field] == before[field]
        assert metadata["ceiling"]["after"][field] == measured[field]
        assert metadata["ratios"][field] <= 1
    assert metadata["colour"]["attainable"]
    assert metadata["colour"]["linear_rgb_max_error"] <= .005


def test_ceiling_uses_visible_rgba_measure_and_ignores_hidden_rgb():
    source = bands_image(1, size=256)
    mask = np.zeros(source.shape[:2], dtype=bool)
    mask[32:224, 48:208] = True
    alpha = mask.astype(np.uint8)*255
    before = measure_material(np.dstack((source, alpha)))
    targets = {field: before[field]*.7 for field in FIELDS}
    hidden_variants = []
    for hidden in (0, 255):
        variant = source.copy()
        variant[~mask] = hidden
        result, metadata = fit_material_detail(variant/255, targets,
            target_oklab=before["oklab"], ceiling=True, mask=mask)
        measured = measure_material(np.dstack((result, alpha)))
        assert metadata["ceiling"]["feasible"]
        assert metadata["ceiling"]["visible_pixels"] == mask.sum()
        for field in FIELDS:
            assert measured[field] <= targets[field]
            assert metadata["measured"][field] == measured[field]
        np.testing.assert_allclose(measured["mean_linear_rgb"], before["mean_linear_rgb"], atol=.005)
        hidden_variants.append(result[mask])
    np.testing.assert_array_equal(*hidden_variants)


def test_ceiling_reports_unreachable_lower_detail_and_black_mean_colour():
    source = np.zeros((32, 32, 3))
    targets = {field: .1 for field in FIELDS}
    result, metadata = fit_material_detail(source, targets,
        target_oklab=linear_to_oklab([.2, .1, .05]), ceiling=True)

    assert not result.any()
    assert metadata["ceiling"]["feasible"]
    assert not metadata["converged"]
    assert not metadata["colour"]["attainable"]
    assert metadata["ceiling"]["lower_gaps"] == dict.fromkeys(FIELDS, 1.)


def test_ceiling_reports_infeasible_zero_target_with_retained_chromatic_variation():
    source = np.empty((64, 64, 3))
    source[:, :32] = [.8, .1, .2]
    source[:, 32:] = [.1, .7, .3]
    before = measure_material(np.rint(source*255).astype(np.uint8))
    _, metadata = fit_material_detail(source, dict.fromkeys(FIELDS, 0.),
        target_oklab=before["oklab"], ceiling=True)

    assert not metadata["ceiling"]["feasible"]
    assert not metadata["converged"]
    assert metadata["ceiling"]["infeasibility"]


@pytest.mark.parametrize("mask", [np.zeros((16, 16)), np.ones((15, 16)), np.full((16, 16), -.1)])
def test_ceiling_rejects_invalid_visibility_masks(mask):
    with pytest.raises(ValueError, match="mask"):
        fit_material_detail(np.full((16, 16, 3), .5), dict.fromkeys(FIELDS, 0.),
                            target_oklab=[.5, 0, 0], ceiling=True, mask=mask)


def highlight_image(size=128):
    """A textured midtone with a two-percent, nonuniform bright tail."""
    y, x = np.mgrid[:size, :size]
    luminance = .11+.012*np.cos(2*np.pi*x/12)+.008*np.cos(2*np.pi*y/37)
    tail = max(2, int(size*size*.02))
    luminance.ravel()[:tail] = np.linspace(.5, .9, tail)
    return np.rint(_srgb(np.repeat(luminance[..., None], 3, axis=2))*255).astype(np.uint8)


def test_highlight_ceiling_rechecks_quantized_pixels_and_mean_colour():
    source = highlight_image()
    before = measure_material(source)
    targets = {field: before[field]*1.5 for field in FIELDS}
    ceiling = float(_linear(np.array(150/255)))-1e-7

    result, metadata = fit_material_detail(source/255, targets,
        target_oklab=before["oklab"], ceiling=True, target_highlight_y=ceiling)
    measured = measure_material(result)

    assert metadata["highlight"]["before"] > ceiling
    assert metadata["highlight"]["iterations"] >= 1
    assert metadata["highlight"]["feasible"]
    assert metadata["highlight"]["after"] == measured["highlight_y"]
    assert measured["highlight_y"] <= ceiling
    assert metadata["colour"]["attainable"]
    assert max(abs(np.array(measured["mean_linear_rgb"])-before["mean_linear_rgb"])) <= .005
    assert metadata["ceiling"]["feasible"]
    for field in FIELDS:
        assert measured[field] <= targets[field]
    # Even after quantization a bright gradient with at least three source
    # levels must retain at least three output levels, rather than a plateau.
    assert np.unique(source.reshape(-1, 3)[:327], axis=0).shape[0] >= 3
    assert np.unique(result.reshape(-1, 3)[:327], axis=0).shape[0] >= 3


def test_highlight_curve_is_strictly_increasing_and_bounds_p99_not_maximum():
    source = np.linspace(0, 1, 10001)
    mapped = _highlight_curve(source, .15, .7, .3)
    assert np.all(np.diff(mapped) > 0)
    np.testing.assert_array_equal(mapped[source <= .15], source[source <= .15])
    assert _highlight_curve(np.array(.7), .15, .7, .3) == pytest.approx(.3)
    assert mapped[-1] > .3
    epsilon = 1e-7
    slope = (_highlight_curve(np.array(.15+epsilon), .15, .7, .3)-.15)/epsilon
    assert slope == pytest.approx(1, rel=1e-5)


def test_highlight_only_projection_preserves_bright_levels_and_repeats_fixed_dither():
    source = highlight_image()
    before = measure_material(source)
    outputs = [limit_material_highlights(source, .3, target_oklab=before["oklab"])
               for _ in range(2)]
    np.testing.assert_array_equal(outputs[0][0], outputs[1][0])
    result, metadata = outputs[0]
    assert metadata["feasible"]
    assert metadata["after"] <= .3
    assert (_linear(result/255) @ np.array([.2126, .7152, .0722])).max() > .3
    assert np.unique(result.reshape(-1, 3)[:327], axis=0).shape[0] >= 3
    assert metadata["dither"] == dict(seed=20261021, distribution="triangular",
        amplitude_srgb_lsb=.5, scope="visible shoulder pixels", applied=True)


def test_highlight_ceiling_uses_only_visible_full_resolution_pixels():
    source = highlight_image()
    mask = np.zeros(source.shape[:2], dtype=bool)
    mask[:96] = True
    alpha = mask.astype(np.uint8)*255
    before = measure_material(np.dstack((source, alpha)))
    outputs = []
    for hidden in (0, 255):
        variant = source.copy()
        variant[~mask] = hidden
        result, metadata = fit_material_detail(variant/255,
            {field: before[field]*1.5 for field in FIELDS},
            target_oklab=before["oklab"], ceiling=True, mask=mask,
            target_highlight_y=.3)
        after = measure_material(np.dstack((result, alpha)))
        assert metadata["highlight"]["after"] == after["highlight_y"]
        assert after["highlight_y"] <= .3
        assert metadata["highlight"]["feasible"]
        outputs.append(result[mask])
    np.testing.assert_array_equal(*outputs)


def test_highlight_reports_incompatible_colour_without_silent_success():
    source = np.full((32, 32, 3), 200, dtype=np.uint8)
    measured = measure_material(source)
    result, metadata = fit_material_detail(source/255, dict.fromkeys(FIELDS, 0.),
        target_oklab=measured["oklab"], ceiling=True, target_highlight_y=.1)
    assert not metadata["converged"]
    assert not metadata["highlight"]["feasible"]
    assert metadata["highlight"]["infeasibility"]
    assert measure_material(result)["highlight_y"] > .1


def test_fine_gain_bound_never_amplifies_g2_or_g3():
    source = bands_image(.5, size=128)
    before = measure_material(source)
    _, metadata = fit_material_detail(source/255,
        {field: before[field]*4 for field in FIELDS},
        target_oklab=before["oklab"], ceiling=True, fine_gain_max=1.)
    assert metadata["gains"]["g2"] <= 1
    assert metadata["gains"]["g3"] <= 1
    assert metadata["gain_upper_bounds"] == dict(g0=1.6, g1=3., g2=1., g3=1.)
    assert metadata["ceiling"]["feasible"]


def test_highlight_only_projection_preserves_an_already_compliant_flat_material():
    source = np.full((32, 32, 3), [100, 130, 150], dtype=np.uint8)
    before = measure_material(source)
    result, metadata = limit_material_highlights(source, before["highlight_y"],
                                                target_oklab=before["oklab"])
    np.testing.assert_array_equal(result, source)
    assert metadata["feasible"]
    assert metadata["iterations"] == 0
    assert metadata["after"] == before["highlight_y"]
    assert metadata["colour"]["attainable"]


@pytest.mark.parametrize("mask", [np.zeros((8, 8)), np.ones((7, 8))])
def test_highlight_only_projection_rejects_invalid_visible_masks(mask):
    with pytest.raises(ValueError, match="mask"):
        limit_material_highlights(np.full((8, 8, 3), 100, dtype=np.uint8), .4,
                                  target_oklab=[.5, 0, 0], mask=mask)


@pytest.mark.parametrize("options", [dict(target_highlight_y=.3), dict(fine_gain_max=1.)])
def test_round_two_limits_require_ceiling_mode(options):
    with pytest.raises(ValueError, match="ceiling=True"):
        fit_material_detail(np.full((8, 8, 3), .3), dict.fromkeys(FIELDS, 0.),
                            target_oklab=[.5, 0, 0], **options)


@pytest.mark.parametrize("options", [dict(target_highlight_y=-.1),
    dict(target_highlight_y=np.nan), dict(fine_gain_max=4.), dict(fine_gain_max=-1.)])
def test_round_two_limits_reject_invalid_values(options):
    with pytest.raises(ValueError, match="target_highlight_y|fine_gain_max"):
        fit_material_detail(np.full((8, 8, 3), .3), dict.fromkeys(FIELDS, 0.),
                            target_oklab=[.5, 0, 0], ceiling=True, **options)
