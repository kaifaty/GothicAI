#!/usr/bin/env python3
"""Measure baked-light risk on our albedos; never reads original game pixels.

The edge measurement looks for a dark band on just one side of an aligned
edge, with recovery to the local baseline further from that edge. Low-frequency
measurements operate on log *linear* Rec.709 luminance at sigma=1/8 tile.
These are review signals: material colour variation can look like illumination.

    uv run --no-project --with numpy --with pillow --with scipy python \\
        scripts/content/material_lighting.py --registry data/content/registry.jsonl \\
        --content-root ~/.local/share/gothic-ai/content-staging/city-3/sources \\
        --output reports/content/2026-10-pilot-measures/city3-lighting.csv
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import sys

import numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter, uniform_filter1d


GOOD = frozenset(("NW_CITY_PFLASTERSTEIN_01.TGA", "NW_CITY_DUNGEONFLOOR_02.TGA",
                  "NW_CITY_BODENDIELEN_01.TGA"))
BAD = frozenset(("NW_HARBOUR_STAIRS_01.TGA", "NW_HARBOUR_STAIRS_03.TGA",
                 "NW_HARBOUR_WOOD_IRON_01.TGA", "NW_HARBOUR_WOODCRATE_01.TGA",
                 "NW_HARBOUR_WOODCRATE_02.TGA", "NW_CITY_INSIDEWALL_11.TGA",
                 "NW_CITY_INSIDEWALL_12.TGA", "NW_CITY_INSIDEWALL_13.TGA"))


def _linear(srgb):
    return np.where(srgb <= .04045, srgb / 12.92, ((srgb + .055) / 1.055) ** 2.4)


def _edge_band(log_luma, visible, *, axis):
    """Long-edge one-sided near/far contrast in natural-log luminance units.

    Inputs have a longest dimension of 256. Average parallel to the edge over
    1/8 of that dimension. On either side compare the 1–3 px band with the
    6–10 px baseline; require a contrast across the edge and penalise symmetric
    dark joints. Compare the 99th percentiles of the two polarities: symmetric
    joints produce dark bands in both directions, whereas repeated one-sided
    light produces an imbalance. Excluding a 10 px border avoids wrap seams.
    """
    normal_size = log_luma.shape[axis]
    tangent = 1 - axis
    # Radii scale with the axis length, including non-square panels.
    near = max(1, round(normal_size / 128))
    far = max(near + 1, round(normal_size / 32))
    width = max(1, near)
    margin = far + width
    if normal_size <= 2 * margin:
        return 0.0, 0.0
    span = max(3, round(log_luma.shape[tangent] / 8))
    smoothed = uniform_filter1d(log_luma, size=span, axis=tangent, mode="reflect")
    smoothed = uniform_filter1d(smoothed, size=width, axis=axis, mode="reflect")
    side = np.moveaxis(smoothed, axis, 0)
    valid = np.moveaxis(visible, axis, 0)
    center = np.arange(margin, normal_size - margin)
    negative_near, positive_near = side[center-near], side[center+near]
    negative_far, positive_far = side[center-far], side[center+far]
    recovery_negative = negative_far - negative_near
    recovery_positive = positive_far - positive_near
    cross_edge = positive_near - negative_near
    dark_negative = np.minimum(recovery_negative, cross_edge) - np.maximum(recovery_positive, 0)
    dark_positive = np.minimum(recovery_positive, -cross_edge) - np.maximum(recovery_negative, 0)
    # Alpha silhouettes must not be interpreted as shadows.
    mask = (valid[center-near] & valid[center+near] &
            valid[center-far] & valid[center+far])
    mask = uniform_filter1d(mask.astype(float), size=span, axis=1, mode="constant") > .999
    if not mask.any():
        return 0.0, 0.0
    negative = float(np.percentile(np.maximum(dark_negative[mask], 0), 99))
    positive = float(np.percentile(np.maximum(dark_positive[mask], 0), 99))
    return abs(negative-positive), max(negative, positive)


def detect_baked_lighting(image, *, max_size=256):
    """Return resolution-normalised numeric lighting-risk measurements.

    Accept an 8-bit or unit-float RGB/RGBA image. Alpha uses the game's >=.5
    cutoff. RGB under transparent pixels is ignored by normalised convolution.
    Uniform luminance gains cancel before clipping because the spatial
    measures operate in log luminance. Colour gains can change the score if
    differently coloured regions have different channel weights. The fitted direction points towards the
    brighter side; 0 degrees is +U, 90 is +V (image down).
    """
    rgb = np.asarray(image)
    if rgb.ndim != 3 or rgb.shape[2] not in (3, 4) or min(rgb.shape[:2]) < 16:
        raise ValueError("expected RGB/RGBA image at least 16 pixels per axis")
    if np.issubdtype(rgb.dtype, np.integer):
        if rgb.min() < 0 or rgb.max() > 255:
            raise ValueError("integer image must contain 8-bit values")
        rgb = rgb.astype(np.float64) / 255
    else:
        rgb = rgb.astype(np.float64)
    if not np.isfinite(rgb).all() or rgb.min() < 0 or rgb.max() > 1:
        raise ValueError("image values must be finite and in [0, 1]")
    # Fixed analysis resolution makes thresholds independent of source size.
    ratio = min(1, max_size / max(rgb.shape[:2]))
    if ratio < 1:
        size = (max(16, round(rgb.shape[1]*ratio)), max(16, round(rgb.shape[0]*ratio)))
        rgb = np.asarray(Image.fromarray(np.rint(rgb*255).astype(np.uint8)).resize(
            size, Image.Resampling.BOX), dtype=float) / 255
    visible = rgb[..., 3] >= .5 if rgb.shape[2] == 4 else np.ones(rgb.shape[:2], dtype=bool)
    if not visible.any():
        raise ValueError("image has no visible pixels")
    luma = _linear(rgb[..., :3]) @ np.array([.2126, .7152, .0722])
    log_luma = np.log(np.maximum(luma, 1/65535))
    # Normalised convolution prevents transparent padding affecting the field.
    sigma = tuple(length / 8 for length in luma.shape)
    weights = gaussian_filter(visible.astype(float), sigma=sigma, mode="reflect")
    field = gaussian_filter(np.where(visible, log_luma, 0), sigma=sigma, mode="reflect") / np.maximum(weights, 1e-12)
    # Fill padding from this local field only for filtering; edge masks exclude it.
    filled = np.where(visible, log_luma, field)
    field_mask = visible & (weights > .5)
    if not field_mask.any():
        field_mask = visible
    yy, xx = np.mgrid[0:luma.shape[0], 0:luma.shape[1]]
    xx = xx / max(luma.shape[1]-1, 1) - .5
    yy = yy / max(luma.shape[0]-1, 1) - .5
    design = np.column_stack((np.ones(field_mask.sum()), xx[field_mask], yy[field_mask]))
    coefficients, *_ = np.linalg.lstsq(design, field[field_mask], rcond=None)
    fitted = design @ coefficients
    variance = float(np.var(field[field_mask]))
    explained = float(np.clip(np.var(fitted) / variance, 0, 1)) if variance > 1e-12 else 0.0
    amplitude = float(np.hypot(*coefficients[1:]))
    direction = float(np.degrees(np.arctan2(coefficients[2], coefficients[1])) % 360) if amplitude > 1e-9 else 0.0
    horizontal, horizontal_strength = _edge_band(filled, visible, axis=0)
    vertical, vertical_strength = _edge_band(filled, visible, axis=1)
    spread = float(np.percentile(field[field_mask], 90) - np.percentile(field[field_mask], 10))
    return dict(edge_horizontal=horizontal, edge_vertical=vertical,
                edge_horizontal_strength=horizontal_strength,
                edge_vertical_strength=vertical_strength,
                edge_cross_strength=min(horizontal_strength, vertical_strength),
                edge_max=max(horizontal, vertical), lowfreq_log_spread=spread,
                plane_log_amplitude=amplitude, plane_u=float(coefficients[1]),
                plane_v=float(coefficients[2]), plane_bright_direction_deg=direction,
                plane_r2=explained, visible_fraction=float(visible.mean()))


def lighting_risk(measurements):
    """Pilot review flag, calibrated on the eleven supplied city-3 examples."""
    # The one-axis boundary is narrow in this small labelled set (highest good
    # .0596, lowest detected bad .0639); this is a review flag, not admission.
    # Strong bands in both axes also catch the iron-bound wooden panel where
    # opposing bands cancel the directional imbalance.
    return (measurements["edge_max"] >= .062 or
            measurements["edge_cross_strength"] >= .50 or
            measurements["lowfreq_log_spread"] >= .35 or
            measurements["plane_log_amplitude"] >= .35)


def measure_file(path):
    with Image.open(path) as image:
        pixels = np.asarray(image.convert("RGBA") if "A" in image.getbands() else image.convert("RGB"))
    result = detect_baked_lighting(pixels)
    return dict(result, lighting_risk=lighting_risk(result))


def city3_rows(registry, content_root):
    """Read only selected albedo files, preserving registry replacement names."""
    entries = [json.loads(line) for line in Path(registry).read_text().splitlines() if line.strip()]
    entries = sorted((entry for entry in entries if entry.get("parameters", {}).get("set") == "city-3"),
                     key=lambda entry: entry["id"])
    for entry in entries:
        texture = entry["replaces"][0]
        relative = Path(entry["files"]["albedo"])
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError(f"unsafe albedo path for {entry['id']}")
        path = Path(content_root) / relative
        row = dict(texture=texture, label="bad" if texture in BAD else "good" if texture in GOOD else "unlabelled",
                   **measure_file(path), albedo=relative.as_posix(),
                   albedo_sha256=hashlib.sha256(path.read_bytes()).hexdigest())
        yield row


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("images", nargs="*", type=Path)
    parser.add_argument("--registry", type=Path)
    parser.add_argument("--content-root", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.registry:
            if not args.content_root or args.images:
                raise ValueError("--registry requires --content-root and no positional images")
            rows = list(city3_rows(args.registry, args.content_root.expanduser()))
        else:
            if not args.images:
                raise ValueError("provide images or --registry with --content-root")
            rows = [dict(image=str(path), **measure_file(path)) for path in args.images]
        if not rows:
            raise ValueError("no city-3 material records")
        stream = args.output.open("w", newline="", encoding="utf-8") if args.output else sys.stdout
        try:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]), lineterminator="\n")
            writer.writeheader()
            writer.writerows(rows)
        finally:
            if args.output:
                stream.close()
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"material_lighting: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
