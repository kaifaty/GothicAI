"""CPU replay of tree_bough_hiz.glsl and the R32F HiZ base level."""

import numpy as np


EPSILON = np.finfo(np.float32).eps
CORNERS = np.arange(8)[:, None] & np.array([1, 2, 4]) != 0


def project(lo, hi, object_matrix, view_projections, near):
    """Return viewport-intersecting projections with a bound over the supplied jitter phases."""
    matrices = view_projections @ object_matrix
    matrix = matrices[0]
    rounding = np.abs(view_projections) @ (np.abs(object_matrix) * (32 * EPSILON))
    # Enclose each phase with one interval per clip component. An exact VP uses one phase.
    error_matrix = (np.abs(matrices - matrix) + rounding).max(axis=0)
    magnitude = np.maximum(np.abs(lo), np.abs(hi))
    error = magnitude @ error_matrix[:, :3].T + error_matrix[:, 3] + 32 * EPSILON
    positions = np.where(CORNERS, hi[:, None, :], lo[:, None, :])
    clip = positions @ matrix[:, :3].T + matrix[:, 3]
    low, high = clip - error[:, None, :], clip + error[:, None, :]
    side = np.concatenate((high[..., :2], -low[..., :2]), axis=-1) + high[..., 3:4]
    outside = (side.max(axis=1) < 0).any(axis=1)
    outside |= (high[..., 2].max(axis=1) < 0) | ((high[..., 3] - low[..., 2]).max(axis=1) < 0)
    valid = ~outside & np.isfinite(low).all(axis=(1, 2)) & np.isfinite(high).all(axis=(1, 2))
    valid &= (low[..., 3] > near).all(axis=1) & (low[..., 2] > 0).all(axis=1)
    low, high = low[valid], high[valid]
    lower = np.minimum(low[..., :3] / low[..., 3:4], low[..., :3] / high[..., 3:4]).min(axis=1)
    upper = np.maximum(high[..., :2] / low[..., 3:4], high[..., :2] / high[..., 3:4]).max(axis=1)
    return valid, outside, lower[:, :2], upper, np.minimum(lower[:, 2], 1) - 1 / 65535


class Depth:
    def __init__(self, depth):
        if not (np.isfinite(depth).all() and ((0 <= depth) & (depth <= 1)).all()):
            raise ValueError("Invalid R32F depth plane")
        height, width = depth.shape
        pw, ph = [1 << (n - 1).bit_length() for n in (width, height)]
        bw, bh = pw, ph
        while max(bw, bh) > 128:
            bw, bh = max(1, bw // 2), max(1, bh // 2)
        tw, th = pw // bw, ph // bh
        self.screen = np.array([width, height])
        self.tile = np.array([tw, th])
        padded = np.zeros((ph, pw), dtype=np.float32)
        padded[:height, :width] = depth
        base = padded.reshape(bh, th, bw, tw).max(axis=(1, 3))
        self.base = np.where(base > 0, base, np.inf)
        # Sparse maxima for inclusive rectangles. This also lets us count the shader's early-out fetches.
        self.maxima = {(0, 0): self.base}
        for dy in range(bh.bit_length()):
            for dx in range(bw.bit_length()):
                if dx:
                    prior = self.maxima[dy, dx - 1]
                    half = 1 << (dx - 1)
                    self.maxima[dy, dx] = np.maximum(prior[:, :-half], prior[:, half:])
                elif dy:
                    prior = self.maxima[dy - 1, dx]
                    half = 1 << (dy - 1)
                    self.maxima[dy, dx] = np.maximum(prior[:-half], prior[half:])
        self.log2 = np.array([n.bit_length() - 1 for n in range(max(bw, bh) + 1)])

    def maximum(self, x0, y0, x1, y1):
        dx, dy = self.log2[x1 - x0 + 1], self.log2[y1 - y0 + 1]
        rx, ry = x1 - (1 << dx) + 1, y1 - (1 << dy) + 1
        code = dy * len(self.log2) + dx
        result = np.empty(x0.size, dtype=np.float32)
        for key in np.unique(code):
            rows = code == key
            table = self.maxima[divmod(int(key), len(self.log2))]
            left, right = x0[rows], rx[rows]
            top, bottom = y0[rows], ry[rows]
            result[rows] = np.maximum.reduce((table[top, left], table[top, right], table[bottom, left], table[bottom, right]))
        return result

    def test(self, lower, upper, guarded):
        pixel_lo = (np.clip(lower, -1, 1) * 0.5 + 0.5) * self.screen - 1
        pixel_hi = (np.clip(upper, -1, 1) * 0.5 + 0.5) * self.screen + 1
        x0, y0 = (np.clip(pixel_lo, 0, self.screen - 1).astype(int) // self.tile).T
        x1, y1 = (np.clip(pixel_hi, 0, self.screen - 1).astype(int) // self.tile).T
        width = x1 - x0 + 1
        footprint = width * (y1 - y0 + 1)
        hidden = guarded > self.maximum(x0, y0, x1, y1)
        fetched = np.where(hidden, footprint, 1)
        # For a failing footprint whose first cell passes, locate the earliest bad row, then column.
        search = ~hidden & (guarded > self.base[y0, x0])
        left, top, right, bottom, guard = (item[search] for item in (x0, y0, x1, y1, guarded))
        first_row = self.first_failure(top, bottom, lambda stop: guard > self.maximum(left, top, right, stop))
        first_col = self.first_failure(left, right, lambda stop: guard > self.maximum(left, first_row, stop, first_row))
        fetched[search] = (first_row - top) * width[search] + first_col - left + 1
        return hidden, fetched, footprint

    @staticmethod
    def first_failure(start, end, passes):
        while np.any(start < end):
            middle = (start + end) // 2
            keep_searching = (start < end) & passes(middle)
            start, end = np.where(keep_searching, middle + 1, start), np.where(keep_searching, end, middle)
        return start
