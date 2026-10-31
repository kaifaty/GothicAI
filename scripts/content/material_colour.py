"""Colour limits for generated material albedo; never change spatial detail."""

import numpy as np


_LUMINANCE = np.array([0.2126, 0.7152, 0.0722], dtype=np.float64)
TARGET_COLOUR_TOLERANCE = .005  # maximum absolute error of a mean linear RGB channel

# Björn Ottosson's 2021 D65 matrices, https://bottosson.github.io/posts/oklab/.
_RGB_TO_LMS = np.array([[.4122214708, .5363325363, .0514459929],
    [.2119034982, .6806995451, .1073969566], [.0883024619, .2817188376, .6299787005]])
_LMS_TO_LAB = np.array([[.2104542553, .7936177850, -.0040720468],
    [1.9779984951, -2.4285922050, .4505937099], [.0259040371, .7827717662, -.8086757660]])


def linear_to_oklab(linear_rgb):
    """Convert linear sRGB triples to D65 OKLab, with no averaging or clipping."""
    return np.cbrt(np.asarray(linear_rgb, dtype=np.float64) @ _RGB_TO_LMS.T) @ _LMS_TO_LAB.T


def oklab_to_linear(oklab):
    """Inverse of linear_to_oklab; retain out-of-gamut values for validation."""
    lms = np.asarray(oklab, dtype=np.float64) @ np.linalg.inv(_LMS_TO_LAB).T
    return lms**3 @ np.linalg.inv(_RGB_TO_LMS).T


def colour_target(row):
    """Read optional all-or-none numeric colour measurements from a CSV row."""
    raw = [row.get('target_oklab_'+channel, '') for channel in ('l', 'a', 'b')]
    if not any(value != '' for value in raw):
        return None
    try:
        target = np.asarray(raw, dtype=np.float64)
    except (TypeError, ValueError) as error:
        raise ValueError(f'{row.get("texture", "material")}: incomplete or invalid target_oklab') from error
    linear = oklab_to_linear(target)
    if not np.isfinite(target).all() or np.any(linear < -1e-6) or np.any(linear > 1+1e-6):
        raise ValueError(f'{row.get("texture", "material")}: target_oklab must describe an in-gamut mean colour')
    return target.tolist()


def correct_mean_colour(rgb_float_0_1, target_oklab, mask=None):
    """Match OKLab(mean linear RGB) with three uniform channel gains.

    Input pixels are sRGB. ``mask`` may select visible pixels or supply their
    nonnegative weights; gains apply to every pixel. Each gain is solved with
    clipping included, then measurements are repeated on the returned uint8
    pixels. Zero channels cannot be created by multiplication: unreachable
    targets remain visible in ``linear_rgb_max_error`` and ``attainable``.
    No saturation cap is applied to measured original colour targets.
    """
    rgb = np.asarray(rgb_float_0_1, dtype=np.float64)
    if rgb.ndim < 1 or rgb.shape[-1] != 3 or rgb.size == 0:
        raise ValueError('expected nonempty RGB pixels with a final dimension of 3')
    if not np.isfinite(rgb).all() or np.any(rgb < 0) or np.any(rgb > 1):
        raise ValueError('RGB values must be finite and in [0, 1]')
    target_oklab = np.asarray(target_oklab, dtype=np.float64)
    if target_oklab.shape != (3,) or not np.isfinite(target_oklab).all():
        raise ValueError('expected three finite target OKLab coordinates')
    target = oklab_to_linear(target_oklab)
    if np.any(target < -1e-6) or np.any(target > 1+1e-6):
        raise ValueError('target OKLab must describe an in-gamut linear RGB mean')
    target = np.clip(target, 0, 1)
    weights = np.ones(rgb.shape[:-1], dtype=np.float64) if mask is None else np.array(mask, dtype=np.float64, copy=True)
    if weights.shape != rgb.shape[:-1] or not np.isfinite(weights).all() or np.any(weights < 0) or weights.sum() <= 0:
        raise ValueError('mask must have the pixel shape, finite nonnegative weights, and positive total weight')
    weights = weights.reshape(-1)
    weights /= weights.sum()
    linear = _linear(rgb)
    samples = linear.reshape(-1, 3)
    before = weights @ samples
    gains = np.ones(3, dtype=np.float64)
    for channel in range(3):
        values = samples[:, channel]
        left, right = 0., 1.
        while weights @ np.minimum(values*right, 1) < target[channel] and right < 65536:
            right *= 2
        for _ in range(40):
            gain = (left+right)/2
            if weights @ np.minimum(values*gain, 1) < target[channel]:
                left = gain
            else:
                right = gain
        gains[channel] = (left+right)/2
    adjusted = np.clip(linear*gains, 0, 1)
    pixels = np.rint(np.clip(_srgb(adjusted), 0, 1)*255).astype(np.uint8)
    after = weights @ _linear(pixels.astype(np.float64)/255).reshape(-1, 3)
    maximum, minimum = pixels.max(axis=-1), pixels.min(axis=-1)
    saturation = np.divide(maximum.astype(float)-minimum, maximum,
        out=np.zeros_like(maximum, dtype=np.float64), where=maximum > 0)
    error = float(np.abs(after-target).max())
    metadata = dict(method='uniform per-channel linear RGB gains with clipping; no spatial edits',
        target_oklab=target_oklab.tolist(), target_linear_rgb=target.tolist(),
        linear_rgb_mean_before=before.tolist(), linear_rgb_mean_after=after.tolist(),
        mean_oklab_before=linear_to_oklab(before).tolist(), mean_oklab_after=linear_to_oklab(after).tolist(),
        channel_gains=gains.tolist(), linear_rgb_max_error=error,
        target_colour_tolerance=TARGET_COLOUR_TOLERANCE, attainable=error <= TARGET_COLOUR_TOLERANCE,
        clipped_channel_fraction=float(weights @ (samples*gains > 1).mean(axis=-1)),
        linear_mean_before=float(before @ _LUMINANCE), linear_mean_after=float(after @ _LUMINANCE),
        saturation=dict(cap_applied=False, exemption='measured original mean colour',
            gray_blend_factor=0., mean_hsv_s_after=float(weights @ saturation.reshape(-1)),
            linear_mean_after=float(after @ _LUMINANCE)))
    return pixels, metadata


def _linear(srgb):
    return np.where(srgb <= 0.04045, srgb / 12.92, ((srgb + 0.055) / 1.055) ** 2.4)


def _srgb(linear):
    return np.where(linear <= 0.0031308, 12.92 * linear, 1.055 * linear ** (1 / 2.4) - 0.055)


def _mean_saturation(rgb):
    maximum = np.max(rgb, axis=-1)
    minimum = np.min(rgb, axis=-1)
    saturation = np.divide(maximum - minimum, maximum,
                           out=np.zeros_like(maximum, dtype=np.float64), where=maximum > 0)
    return float(saturation.mean())


def limit_saturation(rgb_float_0_1, limit):
    """Cap mean per-pixel sRGB HSV S in the returned 8-bit RGB image.

    Every pixel uses the same blend factor towards a neutral grey with that
    pixel's Rec.709 *linear* luminance. This preserves each pixel's luminance
    before 8-bit quantization. The cap is evaluated after quantization, including
    when rounding alone would take an otherwise compliant image over the limit.
    ``gray_blend_factor`` in the metadata is zero for no blend and one for grey.
    """
    rgb = np.asarray(rgb_float_0_1, dtype=np.float64)
    if rgb.ndim < 1 or rgb.shape[-1] != 3 or rgb.size == 0:
        raise ValueError('expected nonempty RGB pixels with a final dimension of 3')
    if not np.isfinite(rgb).all() or np.any(rgb < 0) or np.any(rgb > 1):
        raise ValueError('RGB values must be finite and in [0, 1]')
    limit = float(limit)
    if not np.isfinite(limit) or not 0 <= limit <= 1:
        raise ValueError('saturation limit must be finite and in [0, 1]')

    linear = _linear(rgb)
    luminance = linear @ _LUMINANCE
    original_quantized = np.rint(rgb * 255).astype(np.uint8)
    quantized_before = _mean_saturation(original_quantized.astype(np.float64))
    factor = 0.0
    quantized = original_quantized

    if quantized_before > limit:
        # A shared grey blend preserves channel order. Only the two extrema are
        # needed to measure HSV S, avoiding three-channel arrays at each step.
        minimum = linear.min(axis=-1)
        maximum = linear.max(axis=-1)

        def quantized_saturation(blend):
            low = np.rint(np.clip(_srgb((1 - blend) * minimum + blend * luminance), 0, 1) * 255)
            high = np.rint(np.clip(_srgb((1 - blend) * maximum + blend * luminance), 0, 1) * 255)
            return float(np.divide(high - low, high, out=np.zeros_like(high), where=high > 0).mean())

        left, right = 0.0, 1.0
        for _ in range(28):
            candidate = (left + right) / 2
            if quantized_saturation(candidate) <= limit:
                right = candidate
            else:
                left = candidate
        factor = right
        adjusted = (1 - factor) * linear + factor * luminance[..., None]
        quantized = np.rint(np.clip(_srgb(adjusted), 0, 1) * 255).astype(np.uint8)

    after = _mean_saturation(quantized.astype(np.float64))
    metadata = dict(
        method='uniform linear RGB blend towards per-pixel Rec.709 luminance; no spatial edits',
        threshold=limit,
        mean_hsv_s_before=_mean_saturation(rgb),
        quantized_mean_hsv_s_before=quantized_before,
        mean_hsv_s_after=after,
        gray_blend_factor=factor,
        linear_mean_before=float(luminance.mean()),
        linear_mean_after=float((_linear(quantized.astype(np.float64) / 255) @ _LUMINANCE).mean()),
    )
    return quantized, metadata
