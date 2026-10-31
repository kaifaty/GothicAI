"""Known synthetic periods and orientation exercise the shared decoder core."""
import numpy as np
import pytest
from scipy.ndimage import uniform_filter
import struct

from material_measures import measure_material


DETAIL_FIELDS = ("log_spread", "detail_k3", "detail_k9", "detail_k27")


def stripes(k_u, k_v, size=128):
    y, x = np.mgrid[:size, :size]
    gray = np.round(128 + 60 * np.cos(2 * np.pi * (k_u*x + k_v*y) / size)).astype(np.uint8)
    return np.repeat(gray[:, :, None], 3, axis=2)


@pytest.mark.parametrize("ku,kv,direction,angle", [(8, 0, "V", 90), (0, 4, "U", 0), (6, 3, "V", 116.565)])
def test_known_stripe_period_and_angle(ku, kv, direction, angle):
    result = measure_material(stripes(ku, kv), uv_m_per_unit=8)
    assert result["period_u_uv"] == pytest.approx(1/ku if ku else 0)
    assert result["period_v_uv"] == pytest.approx(1/kv if kv else 0)
    assert result["period_u_m"] == pytest.approx(8/ku if ku else 0)
    assert result["direction"] == direction
    assert result["direction_angle_deg"] == pytest.approx(angle, abs=1)
    assert result["direction_confidence"] > .5


def test_diagonal_stripes_have_no_claimed_axial_direction():
    result = measure_material(stripes(4, 4))
    assert result["direction"] == "none"
    assert result["direction_confidence"] < .01
    assert result["period_u_uv"] == pytest.approx(.25)
    assert result["direction_angle_deg"] == pytest.approx(135, abs=.1)


def test_solid_fill_has_no_period_or_direction_and_known_linear_color():
    result = measure_material(np.full((64, 64, 3), 128, dtype=np.uint8))
    assert result["period_u_uv"] == result["period_v_uv"] == 0
    assert result["direction"] == "none"
    assert result["direction_confidence"] == 0
    assert result["mean_linear_rgb"] == pytest.approx([.21586050011389926] * 3)
    assert result["oklab"] == pytest.approx([.5998708017, 0, 0], abs=1e-7)


def test_alpha_cutoff_excludes_hidden_color_and_rejects_empty():
    image = np.zeros((16, 16, 4), dtype=np.uint8)
    image[:, :8] = [255, 255, 255, 128]
    image[:, 8:] = [255, 0, 0, 127]
    assert measure_material(image)["mean_linear_rgb"] == pytest.approx([1, 1, 1])
    image[:, :, 3] = 0
    with pytest.raises(ValueError, match="no pixels"):
        measure_material(image)


def test_broad_shading_field_is_not_one_repeating_detail():
    x = np.arange(128, dtype=np.float64)
    gray = np.round(80 + x * .5).astype(np.uint8)
    image = np.repeat(np.tile(gray, (128, 1))[:, :, None], 3, axis=2)
    result = measure_material(image)
    assert result["period_u_uv"] == result["period_v_uv"] == 0


def test_mean_color_averages_linear_rgb_before_oklab():
    image = np.zeros((16, 16, 3), dtype=np.uint8)
    image[:, 8:] = 255
    result = measure_material(image)
    assert result["mean_linear_rgb"] == pytest.approx([.5] * 3)
    assert result["oklab"][0] == pytest.approx(.5 ** (1/3), abs=1e-7)


@pytest.mark.parametrize("gray", [0, 128, 255])
def test_flat_image_has_exactly_zero_log_contrast_and_detail(gray):
    measured = measure_material(np.full((64, 64, 3), gray, dtype=np.uint8))
    assert [measured[field] for field in DETAIL_FIELDS] == [0.] * 4


def test_doubling_log_amplitude_doubles_all_detail_measures():
    y, x = np.mgrid[:256, :256]
    pattern = np.where(((x // 7 + y // 11) % 2) == 0, -1., 1.)

    def image(amplitude):
        linear = np.exp(-2 + amplitude * pattern) - .001
        srgb = np.where(linear <= .0031308, linear * 12.92,
                        1.055 * linear ** (1 / 2.4) - .055)
        return np.repeat(np.round(255 * srgb).astype(np.uint8)[:, :, None], 3, axis=2)

    first, second = measure_material(image(.35)), measure_material(image(.70))
    for field in DETAIL_FIELDS:
        assert second[field] == pytest.approx(2 * first[field], rel=.01)


def test_hidden_pixels_do_not_affect_log_measures():
    y, x = np.mgrid[:256, :256]
    image = np.empty((256, 256, 4), dtype=np.uint8)
    image[:, :, :3] = ((x * 7 + y * 13) % 256)[:, :, None]
    image[:, :, 3] = np.where((x + y) % 3, 128, 127)
    first = measure_material(image)
    image[image[:, :, 3] < 128, :3] = [255, 0, 193]
    second = measure_material(image)
    assert [first[field] for field in DETAIL_FIELDS] == [second[field] for field in DETAIL_FIELDS]


def test_fixed_uv_grid_agrees_for_small_and_nearest_upscaled_image():
    image = np.random.default_rng(71).integers(0, 256, (64, 64, 4), dtype=np.uint8)
    upscaled = image.repeat(8, axis=0).repeat(8, axis=1)
    small, large = measure_material(image), measure_material(upscaled)
    for field in DETAIL_FIELDS:
        assert large[field] == pytest.approx(small[field], rel=.01)


def test_grid_uses_pixel_centres_visible_cell_means_and_wrapped_boxes():
    image = np.random.default_rng(256).integers(0, 256, (259, 263, 4), dtype=np.uint8)
    rgb = image[:, :, :3] / 255.
    linear = np.where(rgb <= .04045, rgb / 12.92, ((rgb + .055) / 1.055) ** 2.4)
    luminance = linear @ np.array([.2126, .7152, .0722])
    y, x = np.indices(image.shape[:2])
    cells = (((2*y+1)*256 // (2*259))*256 + (2*x+1)*256 // (2*263))
    visible = image[:, :, 3] >= 128
    counts = np.bincount(cells[visible], minlength=256**2)
    sums = np.bincount(cells[visible], weights=luminance[visible], minlength=256**2)
    included = counts > 0
    grid = np.divide(sums, counts, out=np.zeros_like(sums), where=included)
    grid[~included] = grid[included].mean()
    logs = np.log(grid.reshape(256, 256) + .001)
    expected = [np.percentile(logs.ravel()[included], 90) - np.percentile(logs.ravel()[included], 10)]
    expected += [np.abs(logs - uniform_filter(logs, size=k, mode="wrap")).ravel()[included].mean()
                 for k in (3, 9, 27)]
    measured = measure_material(image)
    assert [measured[field] for field in DETAIL_FIELDS] == pytest.approx(expected, abs=1e-12)


def test_legacy_fields_are_byte_identical_on_stripes_with_alpha():
    y, x = np.mgrid[:128, :128]
    phase = (6 * x + 3 * y) % 128
    triangle = np.minimum(phase, 128 - phase)
    image = np.stack((48 + 2 * triangle, 32 + triangle, 200 - triangle,
                      np.where((x + 3 * y) % 19 == 0, 127,
                               np.where((x + y) % 2, 128, 255))), axis=2).astype(np.uint8)
    # Measured through the thirteen-double C ABI from origin/main
    # aa6e284214892c3c3bc52960dd98c0f8525dea3f, with c++ -std=c++17 -O2
    # -shared -fPIC (Apple clang 21.0.0, arm64 macOS). All thirteen are nonzero.
    expected = [float.fromhex(value) for value in (
        "0x1.5555555555555p-3", "0x1.5555555555555p-2",
        "0x1.d831cc2752717p-1", "0x1.d618dea9905dfp-1",
        "0x1.0000000000000p+1", "0x1.927e8fd42be02p-2",
        "0x1.d611a3aac31b4p+6", "0x1.faaead6548f0ap-2",
        "0x1.765cec96d760ep-4", "-0x1.0d9aed1941c7dp-3",
        "0x1.7b94cbe543441p-3", "0x1.cb726282338bep-5",
        "0x1.97eecd4db32b9p-2",
    )]
    measured = measure_material(image)
    values = [measured[key] for key in ("period_u_uv", "period_v_uv", "period_u_confidence",
                                      "period_v_confidence")]
    values += [("none", "U", "V").index(measured["direction"]), measured["direction_confidence"],
               measured["direction_angle_deg"], *measured["oklab"], *measured["mean_linear_rgb"]]
    assert struct.pack("=13d", *values) == struct.pack("=13d", *expected)
