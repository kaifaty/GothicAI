"""Drive independent GothicAI society worlds from one policy batch."""

import numpy as np

from . import _native


class VecSociety(_native.VecSociety):
    def act(self, choices):
        """Apply one integer option index per pending batch row."""
        array = np.asarray(choices)
        if array.ndim != 1 or array.dtype.kind not in ("i", "u"):
            raise TypeError("choices must be a one-dimensional integer array")
        super().act(np.ascontiguousarray(array, dtype=np.int64))


__all__ = ["VecSociety"]
