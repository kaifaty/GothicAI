"""CPU colour processing for alpha-tested material PNGs, without game pixels.

The engine discards alpha below 0.5. Colour admission therefore measures only
the retained silhouette; RGB under transparent texels never dilutes it.
"""
from __future__ import annotations

import numpy as np
from PIL import Image

import bakeoff
import generate
from material_colour import _linear, _srgb, correct_mean_colour, limit_saturation

ALPHA_TEST_THRESHOLD = .5
ALPHA_TEST_BYTE = 128
ALPHA_TEXTURES = frozenset({'NW_CITY_FLAG_01.TGA', 'NW_CITY_IVY_BORDER_01.TGA',
    'NW_CITY_IVY_FULL_01.TGA'})
COVERAGE_TOLERANCE = .10
LUMINANCE = np.array([.2126, .7152, .0722], dtype=np.float64)


def visible_mask(alpha):
    alpha = np.asarray(alpha)
    if not np.isfinite(alpha).all():
        raise ValueError('alpha values must be finite')
    if np.issubdtype(alpha.dtype, np.integer):
        if np.any(alpha < 0) or np.any(alpha > 255):
            raise ValueError('integer alpha must be in [0, 255]')
        return alpha >= ALPHA_TEST_BYTE
    if np.any(alpha < 0) or np.any(alpha > 1):
        raise ValueError('float alpha must be in [0, 1]')
    return alpha >= ALPHA_TEST_THRESHOLD


def coverage(alpha):
    """Fraction surviving the engine cutoff: byte >=128 or float >=0.5."""
    mask = visible_mask(alpha)
    if mask.size == 0:
        raise ValueError('alpha must contain pixels')
    return float(mask.mean())


def harden_and_dilate(rgba):
    """Return uint8 RGBA with hard alpha and nearest visible RGB in cutouts."""
    rgba = np.asarray(rgba)
    if rgba.ndim != 3 or rgba.shape[-1] != 4 or not np.isfinite(rgba).all():
        raise ValueError('expected finite HxWx4 RGBA pixels')
    if np.issubdtype(rgba.dtype, np.integer):
        if np.any(rgba < 0) or np.any(rgba > 255):
            raise ValueError('integer RGBA must be in [0, 255]')
        pixels = rgba.astype(np.uint8, copy=True)
    else:
        if np.any(rgba < 0) or np.any(rgba > 1):
            raise ValueError('float RGBA must be in [0, 1]')
        pixels = np.rint(rgba*255).astype(np.uint8)
    mask = visible_mask(pixels[..., 3])
    if not mask.any():
        raise ValueError('alpha-tested material has no visible pixels')
    if not mask.all():
        from scipy.ndimage import distance_transform_edt
        indices = distance_transform_edt(~mask, return_distances=False, return_indices=True)
        pixels[~mask, :3] = pixels[indices[0][~mask], indices[1][~mask], :3]
    pixels[..., 3] = mask.astype(np.uint8)*255
    return pixels


def resize_rgba(rgba, size=(1024, 1024)):
    """Resize foreground colour without black fringes, then harden the mask."""
    source = harden_and_dilate(rgba)
    rgb = Image.fromarray(source[..., :3]).resize(size, Image.Resampling.LANCZOS)
    alpha = Image.fromarray(source[..., 3]).resize(size, Image.Resampling.LANCZOS)
    return harden_and_dilate(np.dstack((np.asarray(rgb), np.asarray(alpha))))


def corrected_maps(rgba, record):
    """Match measured visible colour or luminance, preserving the silhouette."""
    pixels = harden_and_dilate(rgba)
    mask = pixels[..., 3] == 255
    rgb = pixels[..., :3].astype(np.float64)/255
    linear = _linear(rgb)
    visible = linear[mask]
    target = record['parameters']['target_luminance']
    before = float((visible @ LUMINANCE).mean())
    colour = None
    if 'target_oklab' in record['parameters']:
        rgb8, colour = correct_mean_colour(rgb, record['parameters']['target_oklab'], mask)
        gain, saturation = colour['channel_gains'], colour['saturation']
    else:
        left, right = 0., 1.
        while float((np.clip(visible*right, 0, 1) @ LUMINANCE).mean()) < target and right < 65536:
            right *= 2
        for _ in range(36):
            gain = (left+right)/2
            if float((np.clip(visible*gain, 0, 1) @ LUMINANCE).mean()) < target:
                left = gain
            else:
                right = gain
        adjusted = np.clip(linear*gain, 0, 1)
        visible_pixels, saturation = limit_saturation(np.clip(_srgb(adjusted[mask]), 0, 1),
            record['parameters']['saturation_limit'])
        # The blend factor was admitted on visible pixels only. Apply that
        # uniform factor to the whole RGB field, then redilate invisible texels.
        blend = saturation['gray_blend_factor']
        adjusted = (1-blend)*adjusted + blend*(adjusted @ LUMINANCE)[..., None]
        rgb8 = np.rint(np.clip(_srgb(adjusted), 0, 1)*255).astype(np.uint8)
        rgb8[mask] = visible_pixels
    corrected = harden_and_dilate(np.dstack((rgb8, pixels[..., 3])))
    maps = bakeoff.filtered_maps(Image.fromarray(corrected[..., :3]), record)
    maps['albedo'] = corrected.astype(np.float32)/255
    rlo, rhi = record['parameters']['roughness_range']
    maps['roughness'] = np.clip(maps['roughness'], np.ceil(rlo*255)/255, np.floor(rhi*255)/255)
    lo, hi = record['parameters']['luminance_range']
    measured_coverage = coverage(corrected[..., 3])
    target_coverage = record['parameters']['target_coverage']
    tolerance = record['parameters'].get('coverage_tolerance', COVERAGE_TOLERANCE)
    correction = dict(method='uniform linear RGB gain on visible foreground; no spatial colour edits',
        weighting='engine-visible pixels only, alpha >=0.5; transparent background excluded',
        linear_mean_before=before, linear_mean_after=saturation['linear_mean_after'],
        target=target, gain=gain, clipped_channel_fraction=float((visible*gain > 1).mean()),
        range=[lo, hi], class_range_advisory=True,
        outside_class_range=not lo <= saturation['linear_mean_after'] <= hi,
        target_tolerance=record['parameters']['target_luminance_tolerance'], saturation=saturation,
        alpha=dict(threshold=ALPHA_TEST_THRESHOLD, source_coverage=measured_coverage,
            output_coverage=measured_coverage, target_coverage=target_coverage,
            tolerance=tolerance, absolute_error=abs(measured_coverage-target_coverage),
            hard_alpha=True, transparent_rgb='nearest visible foreground dilation'))
    if colour is not None:
        correction.update(colour)
        correction['weighting'] = 'engine-visible pixels only, alpha >=0.5; transparent background excluded'
    rejection = generate.numeric_rejection(record, correction)
    if abs(measured_coverage-target_coverage) > tolerance+1e-12:
        coverage_error = f'visible alpha coverage {measured_coverage:.6f} differs from original {target_coverage:.6f} by more than {tolerance:.2f}'
        rejection = coverage_error if rejection is None else rejection+'; '+coverage_error
    return maps, correction, rejection
