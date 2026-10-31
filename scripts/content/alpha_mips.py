"""Hard alpha-tested colour mipmaps, with foreground RGB in cutout texels.

The engine keeps alpha >= 0.5. Every mip is hard again after box filtering;
when its coverage loses more than ten percentage points, an alpha gain keeps
the closest representable coverage to level zero (ties can only add coverage).
"""
from __future__ import annotations

import math

import numpy as np


ALPHA_CUTOFF = 0.5
COVERAGE_LOSS = 0.10


def dilate_rgb(image: np.ndarray) -> np.ndarray:
    """Copy the nearest surviving foreground colour into every cutout texel."""
    result = image.copy()
    mask = result[..., 3] >= ALPHA_CUTOFF
    if mask.any() and not mask.all():
        from scipy.ndimage import distance_transform_edt
        indices = distance_transform_edt(~mask, return_distances=False, return_indices=True)
        result[~mask, :3] = result[indices[0][~mask], indices[1][~mask], :3]
    return result


def cutout_mip_chain(image: np.ndarray):
    """Yield RGBA float mips whose decoded BC3 alpha keeps the cutoff coverage.

    Binary alpha has exact BC3 endpoints 0/255, including uniform blocks, so
    compression cannot move a texel across the cutoff. Colour is dilated anew
    after every mask change, before either colour compression or downsampling.
    """
    image = np.asarray(image, dtype=np.float64).copy()
    if (image.ndim != 3 or image.shape[2] != 4 or not np.isfinite(image).all()
            or np.any(image < 0) or np.any(image > 1)):
        raise ValueError("cutout mipmaps require finite HxWx4 RGBA in [0, 1]")
    h, w = image.shape[:2]
    if h != w or h < 1 or h & (h - 1):
        raise ValueError("cutout mipmaps require power-of-two square images")
    image[..., 3] = image[..., 3] >= ALPHA_CUTOFF
    target = float(image[..., 3].mean())
    if target == 0:
        raise ValueError("alpha-tested material has no visible pixels")
    while True:
        image = dilate_rgb(image)
        yield image
        h, w, channels = image.shape
        if h == w == 1:
            return
        image = image.reshape(h // 2, 2, w // 2, 2, channels).mean(axis=(1, 3))
        alpha = image[..., 3]
        mask = alpha >= ALPHA_CUTOFF
        if float(mask.mean()) + 1e-12 < target - COVERAGE_LOSS:
            # Scale the mip's alpha towards level-zero coverage. On tiny mips
            # each texel covers a large fraction: never round the target down.
            positive = alpha[alpha > 0]
            count = min(math.ceil(target * alpha.size), positive.size)
            threshold = np.partition(positive, positive.size - count)[positive.size - count]
            gain = np.nextafter(ALPHA_CUTOFF / threshold, np.inf)
            mask = np.minimum(alpha * gain, 1) >= ALPHA_CUTOFF
        image[..., 3] = mask
