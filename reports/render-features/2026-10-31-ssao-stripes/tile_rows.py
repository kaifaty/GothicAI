"""Mean |difference| of two captures (grey, 0..255), overall and by row/column inside the 8x8 blur tile.

ssao_blur.comp works on 8x8 tiles: a fix to the halo rows of its horizontal pass may move only tile rows 0-2
and 5-7; rows 3-4 read no halo and stay at the noise level of two runs of the same build.
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float64).mean(axis=2)

if __name__ == "__main__":
    if len(sys.argv) < 3 or len(sys.argv) % 2 == 0:
        raise SystemExit("usage: tile_rows.py A.png B.png [A2.png B2.png ...]")
    print(f"{'pair':58s} {'mean':>6s}  rows 0..7 of the tile (mean |d|)                 cols 0..7 max/min")
    for a, b in zip(sys.argv[1::2], sys.argv[2::2]):
        d = np.abs(load(a) - load(b))
        h, w = d.shape
        d = d[:h - h % 8, :w - w % 8]
        rows = d.reshape(-1, 8, d.shape[1]).mean(axis=(0, 2))
        cols = d.reshape(d.shape[0], -1, 8).mean(axis=(0, 1))
        name = f"{Path(a).parent.name}/{Path(a).stem} vs {Path(b).parent.name}"
        print(f"{name[-58:]:58s} {d.mean():6.3f}  " + " ".join(f"{r:5.3f}" for r in rows)
              + f"   {cols.max() / max(cols.min(), 1e-9):4.2f}")
