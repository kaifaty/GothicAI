"""One house-wide colour transform and geometry-preserving preparation."""

import numpy as np
from PIL import Image
import pytest

import market_house


TEXTURES = (
    'NW_CITY_WALL_03.TGA',
    'NW_CITY_WINDOW_03.TGA',
    'NW_CITY_FACHWERKBASEMENT_WINDOW_01.TGA',
    'NW_CITY_FACHWERKBASEMENT_EMPTY_01.TGA',
    'NW_CITY_FACHWERKBASEMENT_CHAIN_01.TGA',
    'NW_CITY_WOODBEAM_02.TGA',
)


def linear(pixels):
    srgb = np.asarray(pixels, dtype=float) / 255
    return np.where(srgb <= .04045, srgb / 12.92, ((srgb + .055) / 1.055) ** 2.4)


def encoded(rgb):
    rgb = np.asarray(rgb, dtype=float)
    srgb = np.where(rgb <= .0031308, rgb * 12.92, 1.055 * rgb ** (1 / 2.4) - .055)
    return np.rint(srgb * 255).astype(np.uint8)


def house_mean(images, areas):
    return sum(areas[name] * linear(pixels).mean(axis=(0, 1))
               for name, pixels in images.items()) / sum(areas.values())


def test_house_colour_matches_area_weighted_linear_target_despite_different_image_sizes():
    rng = np.random.default_rng(192)
    sizes = ((1, 2), (3, 5), (11, 7), (2, 4), (8, 1), (6, 3))
    images = {name: rng.integers(40, 200, (*size, 3), dtype=np.uint8)
              for name, size in zip(TEXTURES, sizes)}
    areas = dict(zip(TEXTURES, (1., 2., 7., 3., 13., 5.)))
    targets = {name: np.array([.12 + i * .008, .14 + i * .006, .10 + i * .01])
               for i, name in enumerate(TEXTURES)}
    expected = sum(areas[name] * targets[name] for name in TEXTURES) / sum(areas.values())
    original = {name: pixels.copy() for name, pixels in images.items()}

    corrected, metadata = market_house.correct_house_colour(images, areas, targets)

    np.testing.assert_allclose(house_mean(corrected, areas), expected, atol=.005, rtol=0)
    np.testing.assert_allclose(metadata['target_linear_rgb'], expected, atol=1e-12, rtol=0)
    np.testing.assert_allclose(metadata['linear_rgb_mean_after'], house_mean(corrected, areas))
    assert metadata['linear_rgb_max_error'] == pytest.approx(
        np.max(np.abs(house_mean(corrected, areas) - expected)))
    assert len(metadata['channel_gains']) == 3
    for name in TEXTURES:
        assert corrected[name].dtype == np.uint8
        assert corrected[name].shape == images[name].shape
        np.testing.assert_array_equal(images[name], original[name])
        np.testing.assert_array_equal(corrected[name],
                                      market_house.apply_gains(images[name], metadata['channel_gains']))


def test_identical_house_materials_receive_identical_pixels_despite_individual_targets():
    pixels = np.array([[[48, 70, 96], [138, 119, 83]],
                       [[190, 174, 153], [80, 92, 104]]], dtype=np.uint8)
    images = {name: pixels.copy() for name in TEXTURES}
    areas = dict(zip(TEXTURES, (1., 3., 8., 2., 6., 4.)))
    targets = {name: np.array([.09 + i * .025, .18 - i * .01, .08 + i * .02])
               for i, name in enumerate(TEXTURES)}

    corrected, metadata = market_house.correct_house_colour(images, areas, targets)

    for name in TEXTURES:
        np.testing.assert_array_equal(corrected[name], corrected[TEXTURES[0]])
        np.testing.assert_array_equal(corrected[name],
                                      market_house.apply_gains(pixels, metadata['channel_gains']))
    expected = sum(areas[name] * targets[name] for name in TEXTURES) / sum(areas.values())
    np.testing.assert_allclose(house_mean(corrected, areas), expected, atol=.005, rtol=0)


def test_house_gain_solver_accounts_for_highlight_clipping():
    # A mean-ratio gain gives red ~= .58 after clipping. The correct gain is
    # about six, retaining a saturated highlight while lifting the dark pixel.
    pixels = encoded([[[.1, .3, .2], [.9, .5, .4]]])
    images = {name: pixels.copy() for name in TEXTURES}
    areas = dict(zip(TEXTURES, (1., 7., 3., 11., 2., 5.)))
    target = np.array([.8, .4, .3])
    targets = {name: target.copy() for name in TEXTURES}

    corrected, metadata = market_house.correct_house_colour(images, areas, targets)

    np.testing.assert_allclose(house_mean(corrected, areas), target, atol=.005, rtol=0)
    assert metadata['channel_gains'][0] > 5
    assert corrected[TEXTURES[0]][0, 1, 0] == 255


def test_material_colour_couples_painted_oak_to_final_beam_and_matches_group_targets():
    wall, window, *stone, oak = TEXTURES
    panels = {}
    for name in (wall, window):
        pixels = np.full((256, 256, 3), [210, 205, 190], dtype=np.uint8)
        pixels[:, 16:56] = [85, 65, 40]
        pixels[:, 200:240] = [85, 65, 40]
        panels[name] = pixels
    panels[window][70:194, 78:178] = [85, 65, 40]
    images = dict(panels)
    for name, colour in zip(stone, ([125, 130, 110], [155, 145, 125], [100, 100, 95])):
        images[name] = np.full((64, 64, 3), colour, dtype=np.uint8)
    images[oak] = np.full((64, 64, 3), [90, 60, 30], dtype=np.uint8)
    originals = {name: pixels.copy() for name, pixels in images.items()}
    areas = dict(zip(TEXTURES, (3., 1., 2., 5., 1., 4.)))
    targets = dict(zip(TEXTURES, map(np.array, (
        [.25, .24, .20], [.16, .15, .12], [.10, .15, .12],
        [.12, .13, .11], [.08, .14, .10], [.025, .020, .010]))))

    corrected, _, masks = market_house.correct_material_colours(images, areas, targets)

    for group in ((wall, window), stone):
        group_areas = {name: areas[name] for name in group}
        expected = sum(areas[name] * targets[name] for name in group) / sum(group_areas.values())
        actual = house_mean({name: corrected[name] for name in group}, group_areas)
        np.testing.assert_allclose(actual, expected, atol=.005, rtol=0)
    final_oak_mean = linear(corrected[oak]).mean(axis=(0, 1))
    np.testing.assert_allclose(final_oak_mean, targets[oak], atol=.005, rtol=0)
    wood_pixels, pixel_areas = [], []
    for name in (wall, window):
        pixels = linear(corrected[name])[masks[name] > .5]
        assert pixels.size
        wood_pixels.append(pixels)
        # A selected texel represents its share of this panel's world area;
        # the shutter therefore contributes more oak than a plain wall panel.
        pixel_areas.append(np.full(len(pixels), areas[name] / masks[name].size))
    painted_oak_mean = np.average(np.concatenate(wood_pixels), axis=0,
                                 weights=np.concatenate(pixel_areas))
    np.testing.assert_allclose(painted_oak_mean, final_oak_mean, atol=.005, rtol=0)
    for name in TEXTURES:
        np.testing.assert_array_equal(images[name], originals[name])


def test_oak_masks_average_light_wood_grain_and_keep_soft_plaster_spots_light():
    y, x = np.mgrid[:256, :256]
    soft_spot = np.exp(-((x - 128) ** 2 + (y - 128) ** 2) / (2 * 18 ** 2))
    pixels = np.rint(np.array([210, 207, 198]) - 12 * soft_spot[..., None]).astype(np.uint8)
    posts = ((32 <= x) & (x < 96)) | ((160 <= x) & (x < 224))
    pixels[posts] = [54, 30, 15]
    pixels[posts & (x % 8 == 0)] = [182, 167, 135]
    images = {name: pixels.copy() for name in TEXTURES[:2]}

    masks, metadata = market_house.oak_masks(images)

    for name, mask in masks.items():
        assert mask.shape == pixels.shape[:2]
        assert np.issubdtype(mask.dtype, np.floating)
        assert np.isfinite(mask).all()
        assert 0 <= mask.min() <= mask.max() <= 1
        # Test broad interiors, away from the intentionally soft material edge.
        assert mask[:, 48:80].min() > .9
        assert mask[:, 176:208].min() > .9
        assert mask[:, 112:144].max() < .1
        np.testing.assert_array_equal(images[name], pixels)
    assert isinstance(metadata, dict)


def test_oak_masks_use_one_pooled_threshold_and_treat_identical_inputs_identically():
    wall, window = TEXTURES[:2]
    images = {wall: np.full((64, 64, 3), 45, dtype=np.uint8),
              window: np.full((64, 64, 3), 200, dtype=np.uint8)}

    masks, _ = market_house.oak_masks(images)

    # An independent Otsu threshold on either constant input would put both
    # materials at its sigmoid midpoint instead of separating the two classes.
    assert masks[wall].min() > .9
    assert masks[window].max() < .1

    pixels = np.full((64, 64, 3), 200, dtype=np.uint8)
    pixels[:, 16:48] = 45
    masks, _ = market_house.oak_masks({wall: pixels.copy(), window: pixels.copy()})
    # There is no V variation, so wrap and reflect must give the same result.
    np.testing.assert_array_equal(masks[wall], masks[window])


def test_oak_masks_blur_log_luminance_with_periodic_u_and_only_wall_periodic_v():
    from scipy.ndimage import gaussian_filter

    wall, window = TEXTURES[:2]
    y = np.linspace(0, 1, 1024)[:, None]
    x = np.arange(1024)[None, :] * 2 * np.pi / 1024
    luminance = np.exp(-2 + .5 * (2 * y - 1) + .2 * np.sin(x))
    pixels = encoded(np.repeat(luminance[..., None], 3, axis=2))
    images = {wall: pixels.copy(), window: pixels.copy()}

    masks, _ = market_house.oak_masks(images)

    log_y = np.log(linear(pixels) @ np.array([.2126, .7152, .0722]))
    thresholds = []
    for name, modes in ((wall, ('wrap', 'wrap')), (window, ('reflect', 'wrap'))):
        blurred = gaussian_filter(log_y, sigma=6, mode=modes)
        mask = masks[name]
        assert np.all((mask > 0) & (mask < 1))
        # Inverting the stated sigmoid must recover one threshold at every
        # pixel. This checks blur order, sigma, boundary modes and width .3.
        threshold = blurred + .3 * np.log(mask / (1 - mask))
        np.testing.assert_allclose(threshold, threshold.mean(), atol=1e-6, rtol=0)
        thresholds.append(threshold.mean())
    assert thresholds[0] == pytest.approx(thresholds[1], abs=1e-6)
    assert np.max(np.abs(masks[wall][0] - masks[window][0])) > .1


def test_oak_masks_have_a_soft_transition_on_a_brown_to_plaster_ramp():
    start = np.array([96, 44, 12], dtype=float)
    end = np.array([208, 208, 204], dtype=float)
    ramp = np.rint(start + np.linspace(0, 1, 256)[:, None] * (end - start)).astype(np.uint8)
    pixels = np.broadcast_to(ramp[None, ...], (16, 256, 3)).copy()

    masks, _ = market_house.oak_masks({name: pixels.copy() for name in TEXTURES[:2]})

    # A thresholded segmentation would have only zeroes and ones. A broad
    # transition also catches a nominal sigmoid that is effectively a step.
    values = masks[TEXTURES[0]][0]
    assert np.count_nonzero((values > .1) & (values < .9)) >= 10
    assert np.unique(values).size >= 20
    assert np.max(np.abs(np.diff(values))) < .15


@pytest.mark.parametrize('name', TEXTURES)
def test_prepare_resizes_once_with_lanczos_then_seams_only_allowed_axes(name, monkeypatch):
    assert market_house.TEXTURES == TEXTURES
    expected_axes = 'u' if name == 'NW_CITY_WINDOW_03.TGA' else 'uv'
    assert market_house.tiling_axes(name) == expected_axes
    pixels = np.random.default_rng(13).integers(0, 256, (41, 63, 3), dtype=np.uint8)
    source = Image.fromarray(pixels)
    original_resize = Image.Image.resize
    expected = original_resize(source, (1024, 1024), Image.Resampling.LANCZOS)
    calls = []

    def resize(image, size, resample=None, **kwargs):
        calls.append(('resize', size, resample))
        return original_resize(image, size, resample=resample, **kwargs)

    seam_metadata = dict(axes=expected_axes)

    def seamless(image, *, axes, overlap_fraction):
        calls.append(('seamless', image.size, axes, overlap_fraction))
        np.testing.assert_array_equal(image, expected)
        return image, seam_metadata

    monkeypatch.setattr(Image.Image, 'resize', resize)
    monkeypatch.setattr(market_house, 'seamless', seamless)

    result, metadata = market_house.prepare_image(source, name)

    assert calls == [('resize', (1024, 1024), Image.Resampling.LANCZOS),
                     ('seamless', (1024, 1024), expected_axes, 1 / 64)]
    assert result.size == (1024, 1024)
    assert metadata == seam_metadata
    np.testing.assert_array_equal(source, pixels)
