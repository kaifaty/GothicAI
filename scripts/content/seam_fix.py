"""CPU-only, minimum-error overlap cuts for reviewed material texture seams.

``u`` is the left/right join, ``v`` the top/bottom join.  This is image
quilting, not a blur: pixels on either side of the cut come from the source.
An overlap shortens that axis; periodic resampling restores the input size.
Callers decide whether a seam needs fixing after inspecting its tiled preview.
"""

import math

import numpy as np
from PIL import Image


def _minimum_path(error, closed=False):
    """Return the minimum-cost 8-connected vertical path through an error map.

    A closed path also joins its final row to its first row.  It is needed when
    cutting the second axis of a texture whose first axis is already periodic.
    Batching start positions bounds the traceback memory for large textures.
    """
    rows, columns = error.shape
    if not closed:
        cost = error[0].copy()
        steps = np.zeros((rows, columns), dtype=np.int8)
        for row in range(1, rows):
            options = np.stack((cost, np.r_[np.inf, cost[:-1]], np.r_[cost[1:], np.inf]))
            choice = options.argmin(axis=0)
            steps[row] = np.array([0, -1, 1], dtype=np.int8)[choice]
            cost = options[choice, np.arange(columns)] + error[row]
        position = int(cost.argmin())
        total = float(cost[position])
        path = np.empty(rows, dtype=np.int32)
        for row in range(rows - 1, -1, -1):
            path[row] = position
            position += int(steps[row, position])
        return path, total

    best_cost, best_path = math.inf, None
    for first in range(0, columns, 16):
        starts = np.arange(first, min(first + 16, columns))
        count = len(starts)
        cost = np.full((count, columns), np.inf)
        cost[np.arange(count), starts] = error[0, starts]
        steps = np.zeros((rows, count, columns), dtype=np.int8)
        for row in range(1, rows):
            previous = np.pad(cost[:, :-1], ((0, 0), (1, 0)), constant_values=np.inf)
            following = np.pad(cost[:, 1:], ((0, 0), (0, 1)), constant_values=np.inf)
            options = np.stack((cost, previous, following))
            choice = options.argmin(axis=0)
            steps[row] = np.array([0, -1, 1], dtype=np.int8)[choice]
            cost = np.take_along_axis(options, choice[None], axis=0)[0] + error[row]

        # A one-pixel diagonal across the periodic boundary is also connected.
        eligible = np.abs(np.arange(columns)[None] - starts[:, None]) <= 1
        terminal = np.where(eligible, cost, np.inf)
        start_index, position = np.unravel_index(terminal.argmin(), terminal.shape)
        total = float(terminal[start_index, position])
        if total < best_cost:
            best_cost = total
            best_path = np.empty(rows, dtype=np.int32)
            for row in range(rows - 1, -1, -1):
                best_path[row] = position
                position += int(steps[row, start_index, position])
    return best_path, best_cost


def _resize_periodic(array, width):
    """Resize only u, supplying real periodic neighbours to the Lanczos filter."""
    height = array.shape[0]
    repeated = Image.fromarray(np.concatenate([array] * 3, axis=1))
    repeated = repeated.resize((width * 3, height), Image.Resampling.LANCZOS)
    return np.asarray(repeated.crop((width, 0, width * 2, height))).copy()


def _cut_u(array, overlap_fraction, closed):
    height, width, _ = array.shape
    if width < 6:
        raise ValueError('a corrected axis must contain at least 6 pixels')
    overlap = min(width // 2, max(3, round(width * overlap_fraction)))
    period = width - overlap
    head = array[:, :overlap].astype(np.float64)
    tail = array[:, period:].astype(np.float64)
    error = np.mean(((head - tail) / 255) ** 2, axis=-1)

    # Keep the overlap's first pixel from the tail and its last from the head.
    # Then the wrap boundary and the exit from the overlap both retain genuine
    # source neighbours; an unconstrained edge cut could merely move the seam.
    path, total = _minimum_path(error[:, 1:-1], closed=closed)
    path += 1
    tile = array[:, :period].copy()
    tail_mask = np.arange(overlap)[None] <= path[:, None]
    tile[:, :overlap] = np.where(tail_mask[..., None], array[:, period:], array[:, :overlap])
    result = _resize_periodic(tile, width)
    metadata = dict(
        overlap_px=overlap,
        period_before_resize_px=period,
        restored_size_px=width,
        path=path.tolist(),
        closed_path=closed,
        mean_squared_rgb_error=total / height,
    )
    return result, metadata


def seamless(image, axes='uv', overlap_fraction=.125):
    """Quilt designated tile joins, returning an RGB image and JSON metadata.

    ``axes=''`` is an unchanged panel.  Only ``u``, ``v``, ``uv`` and ``''``
    are accepted, and ``0 < overlap_fraction <= .5``.  The result always has
    the source dimensions.  No pixels are blended at the cut; Lanczos only
    restores the dimensions after overlap cropping.  The second cut is closed
    around the already-corrected first axis so it cannot reopen that join.
    """
    if not isinstance(image, Image.Image) or image.mode != 'RGB':
        raise ValueError('expected a PIL RGB image')
    if axes not in ('', 'u', 'v', 'uv'):
        raise ValueError("axes must be '', 'u', 'v' or 'uv'")
    overlap_fraction = float(overlap_fraction)
    if not math.isfinite(overlap_fraction) or not 0 < overlap_fraction <= .5:
        raise ValueError('overlap_fraction must be finite and in (0, .5]')
    array = np.asarray(image).copy()
    operations = []
    for axis in axes:
        if axis == 'v':
            array = array.transpose(1, 0, 2)
        array, operation = _cut_u(array, overlap_fraction, closed=bool(operations))
        operation['axis'] = axis
        operations.append(operation)
        if axis == 'v':
            array = array.transpose(1, 0, 2)
    metadata = dict(
        method='minimum squared RGB error overlap cut; periodic Lanczos resize',
        axes=axes,
        overlap_fraction=overlap_fraction,
        source_size=list(image.size),
        output_size=list(image.size),
        applied=bool(axes),
        operations=operations,
    )
    return Image.fromarray(array), metadata
