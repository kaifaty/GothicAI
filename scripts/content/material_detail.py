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


def fit_material_detail(rgb_float_0_1, targets, *, target_oklab, ceiling=False, mask=None):
    """Fit four bounded log-luminance band gains, measuring final uint8 pixels.

    ``targets`` has log_spread and detail_k3/k9/k27 keys. Mean colour
    correction is included in every candidate, so the returned measurements
    describe the actual colour-corrected PNG rather than an intermediate image.
    """
    if ceiling:
        return _fit_detail_ceiling(rgb_float_0_1, targets, target_oklab, mask)
    if mask is not None:
        raise ValueError("a visible mask requires ceiling=True")
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


def _fit_detail_ceiling(rgb_float_0_1, targets, target_oklab, mask):
    """Keep only measured-feasible candidates, including after quantization.

    Lower gains may reach zero. After the band fit, a scalar search recovers
    the highest feasible point along its gain ray; it never assumes rounding
    preserves an unquantized bound. A failed search is reported explicitly.
    Alpha targets describe visible pixels, exactly as the shared RGBA measure.
    """
    rgb = _rgb(rgb_float_0_1)
    target = np.array([targets[field] for field in FIELDS], dtype=np.float64)
    if not np.isfinite(target).all() or np.any(target < 0):
        raise ValueError("detail targets must be finite and nonnegative")
    visible = np.ones(rgb.shape[:2], dtype=bool)
    if mask is not None:
        raw_mask = np.asarray(mask)
        if (raw_mask.shape != visible.shape or not np.isfinite(raw_mask).all()
                or not np.isin(raw_mask, [0, 1]).all() or not raw_mask.any()):
            raise ValueError("mask must be a nonempty visible-pixel boolean mask")
        visible = raw_mask.astype(bool)

    def measure(pixels):
        if mask is None:
            return measure_material(pixels)
        return measure_material(np.dstack((pixels, visible.astype(np.uint8)*255)))

    before = measure(np.rint(rgb*255).astype(np.uint8))
    linear = _linear(rgb)
    # Hidden RGB is dilation padding, not material evidence or filter input.
    if mask is not None:
        linear = linear.copy()
        linear[~visible] = linear[visible].mean(axis=0)
    logs = np.log(linear @ _LUMINANCE + .001)
    mean = logs[visible].mean()
    b4, b12, b36, b108 = [uniform_filter(logs, size=size, mode="wrap")
                           for size in (4, 12, 36, 108)]
    bands = (b108-mean, b4-b12, b12-b36, b36-b108)
    subcell = logs-b4
    evaluations = 0

    def render(gains):
        nonlocal evaluations
        evaluations += 1
        fitted = mean + sum(gain*band for gain, band in zip(gains, bands))
        fitted += min(gains[1], 1.5)*subcell
        adjusted = linear*np.exp(fitted-logs)[..., None]
        maximum = adjusted.max(axis=-1)
        shoulder_fraction = float((maximum[visible] > .7).mean())
        shoulder = np.where(maximum > .7, .7+.3*np.tanh((maximum-.7)/.3), maximum)
        adjusted *= np.divide(shoulder, maximum, out=np.ones_like(maximum),
                              where=maximum > 0)[..., None]
        srgb = np.clip(_srgb(np.clip(adjusted, 0, 1)), 0, 1)
        pixels, colour = correct_mean_colour(srgb, target_oklab,
                                             visible if mask is not None else None)
        clipped = np.any(adjusted*colour["channel_gains"] > 1, axis=-1)
        measured = measure(pixels)
        actual = np.array([measured[field] for field in FIELDS])
        return dict(pixels=pixels, measured=measured, actual=actual,
                    colour=colour, gains=gains.copy(),
                    clipped=float(clipped[visible].mean()), shoulder=shoulder_fraction)

    def feasible(candidate):
        return bool(np.all(candidate["actual"] <= target))

    def lower_gaps(candidate):
        return np.divide(np.maximum(target-candidate["actual"], 0), target,
                         out=np.zeros(4), where=target > 0)

    def score(candidate):
        return float(np.minimum(np.divide(candidate["actual"], target,
            out=np.ones(4), where=target > 0), 1).sum())

    candidates = []
    gains = np.ones(4)
    for iteration in range(9):
        candidate = render(gains)
        candidates.append(candidate)
        if feasible(candidate) and np.all(lower_gaps(candidate) <= .03):
            break
        multiplier = np.divide(target, candidate["actual"], out=np.ones(4),
                               where=candidate["actual"] > 1e-12)
        gains = np.clip(gains*multiplier, 0, GAIN_BOUNDS[:, 1])

    # The lower endpoint is measured too: retaining chromatic variation can
    # leave luminance detail after mean-colour correction even at zero gains.
    zero = render(np.zeros(4))
    candidates.append(zero)
    if feasible(zero):
        # Extend a feasible ray to its gain bound before bracketing, so a
        # conservative multiplicative step does not needlessly lose detail.
        endpoint = candidates[-2]
        if feasible(endpoint) and np.any(endpoint["gains"] > 0):
            positive = endpoint["gains"] > 0
            scale = float(np.min(GAIN_BOUNDS[positive, 1]/endpoint["gains"][positive]))
            if scale > 1:
                endpoint = render(np.minimum(endpoint["gains"]*scale, GAIN_BOUNDS[:, 1]))
                candidates.append(endpoint)
        if not feasible(endpoint):
            left, right = 0., 1.
            for _ in range(18):
                fraction = (left+right)/2
                candidate = render(endpoint["gains"]*fraction)
                candidates.append(candidate)
                if feasible(candidate):
                    left = fraction
                else:
                    right = fraction

    admitted = [candidate for candidate in candidates if feasible(candidate)]
    if admitted:
        best = max(admitted, key=score)
    else:
        best = min(candidates, key=lambda candidate: float(np.max(
            (candidate["actual"]-target)/np.maximum(target, 1e-12))))
    gaps = lower_gaps(best)
    actual, gains = best["actual"], best["gains"]
    met_ceiling = feasible(best)
    names = ("g0", "g1", "g2", "g3")
    measured_fields = {field: best["measured"][field] for field in FIELDS}
    return best["pixels"], dict(
        method="wrapped log-luminance bands B4/12/36/108; zero-based gains; quantized final-pixel ceiling; max-RGB tanh shoulder before mean RGB correction",
        gains=dict(zip(names, map(float, gains))), subcell_gain=float(min(gains[1], 1.5)),
        subcell_limit_hit=bool(gains[1] > 1.5), iterations=iteration,
        tolerance=.03, converged=bool(met_ceiling and np.all(gaps <= .03)
                                    and best["colour"]["attainable"]),
        targets=dict(zip(FIELDS, map(float, target))), measured=measured_fields,
        ratios={field: (float(actual[i]/target[i]) if target[i] else None)
                for i, field in enumerate(FIELDS)},
        limits={name: ("minimum" if gains[i] == 0 else
                       "maximum" if gains[i] == GAIN_BOUNDS[i, 1] else None)
                for i, name in enumerate(names)},
        clipped_pixel_fraction=best["clipped"], shoulder_pixel_fraction=best["shoulder"],
        colour=best["colour"], ceiling=dict(
            before={field: before[field] for field in FIELDS}, after=measured_fields,
            feasible=met_ceiling, upper_tolerance=0., lower_tolerance=.03,
            lower_gaps=dict(zip(FIELDS, map(float, gaps))),
            infeasibility=None if met_ceiling else "no measured candidate satisfies all four ceilings; colour-preserving zero gains may retain chromatic luminance detail",
            evaluations=evaluations, visible_pixels=int(visible.sum()),
            search="bounded multiplicative band fit, then highest measured-feasible point along the final gain ray"))
