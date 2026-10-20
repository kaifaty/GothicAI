"""Fit generated albedo contrast and detail to scalar material measurements."""
from __future__ import annotations

import numpy as np
from scipy.ndimage import gaussian_filter, uniform_filter

from material_colour import _linear, _srgb, correct_mean_colour
from material_measures import measure_material


FIELDS = ("log_spread", "detail_k3", "detail_k9", "detail_k27")
GAIN_BOUNDS = np.array([[.8, 1.6], [.5, 3.], [.5, 3.], [.5, 3.]])
_LUMINANCE = np.array([.2126, .7152, .0722])


def _rgb(image):
    rgb = np.asarray(image, dtype=np.float64)
    if rgb.ndim != 3 or rgb.shape[2] != 3 or not rgb.size:
        raise ValueError("expected a nonempty H x W x 3 sRGB image")
    if not np.isfinite(rgb).all() or np.any(rgb < 0) or np.any(rgb > 1):
        raise ValueError("sRGB pixels must be finite and in [0, 1]")
    return rgb


def low_frequency_iid(rgb_float_0_1, iid_rgb_float_0_1):
    """Apply only the wrapped, sigma-32 IID illumination correction in linear RGB."""
    rgb, iid = _rgb(rgb_float_0_1), _rgb(iid_rgb_float_0_1)
    if rgb.shape != iid.shape:
        raise ValueError("IID and prepared input must have the same dimensions")
    linear = _linear(rgb)
    blurred_input = gaussian_filter(linear, sigma=(32, 32, 0), mode="wrap")
    blurred_iid = gaussian_filter(_linear(iid), sigma=(32, 32, 0), mode="wrap")
    ratio = np.clip(blurred_iid / np.maximum(blurred_input, 1e-12), .25, 4.)
    adjusted = linear * np.sqrt(ratio)
    pixels = np.rint(np.clip(_srgb(np.clip(adjusted, 0, 1)), 0, 1)*255).astype(np.uint8)
    return pixels, dict(method="linear RGB * clamp(G(IID) / G(input), .25, 4) ** .5",
        gaussian_sigma_px=32, boundary="wrap", ratio_bounds=[.25, 4.],
        clipped_pixel_fraction=float(np.any(adjusted > 1, axis=-1).mean()))


def fit_material_detail(rgb_float_0_1, targets, *, target_oklab):
    """Fit four bounded log-luminance band gains, measuring final uint8 pixels.

    ``targets`` has log_spread and detail_k3/k9/k27 keys. Mean colour
    correction is included in every candidate, so the returned measurements
    describe the actual colour-corrected PNG rather than an intermediate image.
    """
    rgb = _rgb(rgb_float_0_1)
    target = np.array([targets[field] for field in FIELDS], dtype=np.float64)
    if not np.isfinite(target).all() or np.any(target < 0):
        raise ValueError("detail targets must be finite and nonnegative")
    linear = _linear(rgb)
    logs = np.log(linear @ _LUMINANCE + .001)
    b4, b12, b36, b108 = [uniform_filter(logs, size=size, mode="wrap")
                           for size in (4, 12, 36, 108)]
    mean = logs.mean()
    bands = (b108-mean, b4-b12, b12-b36, b36-b108)
    subcell = logs-b4

    def render(gains):
        fitted = mean + sum(gain*band for gain, band in zip(gains, bands))
        fitted += min(gains[1], 1.5)*subcell
        adjusted = linear*np.exp(fitted-logs)[..., None]
        maximum = adjusted.max(axis=-1)
        shoulder_pixel_fraction = float((maximum > .7).mean())
        shoulder = np.where(maximum > .7,
            .7 + .3*np.tanh((maximum-.7)/.3), maximum)
        adjusted *= np.divide(shoulder, maximum, out=np.ones_like(maximum),
                              where=maximum > 0)[..., None]
        clipped_pixels = np.any((adjusted < 0) | (adjusted > 1), axis=-1)
        srgb = np.clip(_srgb(np.clip(adjusted, 0, 1)), 0, 1)
        pixels, colour = correct_mean_colour(srgb, target_oklab)
        clipped_pixels |= np.any(np.clip(adjusted, 0, 1)*colour["channel_gains"] > 1, axis=-1)
        measured = measure_material(pixels)
        return pixels, measured, colour, float(clipped_pixels.mean()), shoulder_pixel_fraction

    gains = np.ones(4, dtype=np.float64)
    for iteration in range(9):
        pixels, measured, colour, clipped, shoulder_pixel_fraction = render(gains)
        actual = np.array([measured[field] for field in FIELDS])
        relative_error = np.divide(actual-target, target, out=actual.copy(), where=target > 0)
        if np.all(np.abs(relative_error) <= .03) or iteration == 8:
            break
        multiplier = np.divide(target, actual, out=np.ones(4), where=actual > 1e-12)
        gains = np.clip(gains*multiplier, GAIN_BOUNDS[:, 0], GAIN_BOUNDS[:, 1])
    names = ("g0", "g1", "g2", "g3")
    return pixels, dict(
        method="wrapped log-luminance bands B4/12/36/108; subcell gain capped at 1.5; tanh max-RGB shoulder above 0.7",
        gains=dict(zip(names, map(float, gains))), subcell_gain=float(min(gains[1], 1.5)),
        subcell_limit_hit=bool(gains[1] > 1.5),
        iterations=iteration, tolerance=.03, converged=bool(np.all(np.abs(relative_error) <= .03)),
        targets=dict(zip(FIELDS, map(float, target))),
        measured={field: measured[field] for field in FIELDS},
        ratios={field: (float(actual[i]/target[i]) if target[i] else None)
                for i, field in enumerate(FIELDS)},
        limits={name: ("minimum" if np.isclose(gains[i], GAIN_BOUNDS[i, 0]) else
                       "maximum" if np.isclose(gains[i], GAIN_BOUNDS[i, 1]) else None)
                for i, name in enumerate(names)},
        clipped_pixel_fraction=clipped, shoulder_pixel_fraction=shoulder_pixel_fraction,
        colour=colour)
