"""Synthetic albedo tests keep fitting independent from original game images."""
import numpy as np
import pytest

from material_colour import _linear, _srgb
from material_detail import FIELDS, fit_material_detail, low_frequency_iid
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
