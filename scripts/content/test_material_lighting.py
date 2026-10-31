"""Spatial lighting signals are quantitative, directional and alpha-aware."""
import numpy as np
import pytest

import material_lighting as lighting


def srgb(linear):
    return np.where(linear <= .0031308, 12.92*linear, 1.055*linear**(1/2.4)-.055)


def grayscale(linear):
    return np.repeat(srgb(np.asarray(linear))[..., None], 3, axis=2)


def test_flat_fill_has_no_spatial_lighting_signal():
    measured = lighting.detect_baked_lighting(np.full((128, 128, 3), 128, dtype=np.uint8))
    for key in ("edge_max", "lowfreq_log_spread", "plane_log_amplitude", "plane_r2"):
        assert measured[key] == pytest.approx(0, abs=1e-12)
    assert not lighting.lighting_risk(measured)


def test_log_brightness_plane_recovers_direction_and_relative_strength():
    y, x = np.mgrid[0:128, 0:128] / 127
    field = .1 * np.exp(.6*x+.3*y)
    measured = lighting.detect_baked_lighting(grayscale(field))
    assert measured['plane_bright_direction_deg'] == pytest.approx(np.degrees(np.arctan2(.3, .6)), abs=1)
    assert measured['plane_u'] > measured['plane_v'] > 0
    assert measured['plane_r2'] > .99
    assert measured['lowfreq_log_spread'] > .3


def test_one_sided_dark_band_is_detected_on_both_edge_orientations():
    field = np.full((256, 256), .4)
    # A narrow dark band just below a horizontal material edge.
    field[128:] = (.4-.24*np.exp(-np.arange(128)/3))[:, None]
    horizontal = lighting.detect_baked_lighting(grayscale(field))
    vertical = lighting.detect_baked_lighting(grayscale(field.T))
    assert horizontal['edge_horizontal'] > .15
    assert horizontal['edge_vertical'] < 1e-10
    assert vertical['edge_vertical'] == pytest.approx(horizontal['edge_horizontal'])
    assert vertical['edge_horizontal'] < 1e-10


def test_uniform_luminance_gain_preserves_spatial_log_metrics():
    y, x = np.mgrid[0:128, 0:128] / 127
    field = .04*np.exp(.2*x+.4*y)
    original = lighting.detect_baked_lighting(grayscale(field))
    brighter = lighting.detect_baked_lighting(grayscale(field*2))
    for key in ('edge_max', 'lowfreq_log_spread', 'plane_log_amplitude', 'plane_r2'):
        assert brighter[key] == pytest.approx(original[key], abs=1e-10)


def test_hidden_rgb_does_not_create_a_lighting_field():
    rgba = np.ones((128, 128, 4))*.5
    rgba[..., 3] = 0
    rgba[16:112, 16:112, 3] = 1
    other = rgba.copy()
    other[rgba[..., 3] == 0, :3] = (1, 0, 0)
    before = lighting.detect_baked_lighting(rgba)
    after = lighting.detect_baked_lighting(other)
    assert before == after
    assert before['edge_max'] == pytest.approx(0, abs=1e-12)
    assert before['lowfreq_log_spread'] == pytest.approx(0, abs=1e-12)


@pytest.mark.parametrize('value', [np.zeros((8, 8, 3)), np.full((16, 16, 3), np.nan),
                                  np.full((16, 16, 3), 1.1), np.zeros((16, 16, 4))])
def test_invalid_or_invisible_image_is_rejected(value):
    with pytest.raises(ValueError):
        lighting.detect_baked_lighting(value)
