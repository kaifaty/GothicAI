#!/usr/bin/env python3
"""Outlines of the holes painted on NW_CITY_ROOF_01 (our roofs-8 bake), for real holes in the world mesh.

The holes are where shingle_bake.py put them (HOLES, U and V of the square, V from the top). In each one
the black of the attic is the hole; the light bands inside it are the lath (across) and, in the big
one, the rafter (down the slope). Reads the albedo and writes, per hole, the outline as a stepped
polygon (row extents of the closed black area), the V of its lath and the U of its rafter:

    uv run --no-project --with numpy --with pillow --with scipy python \
        scripts/content/world/roof_holes_layout.py <roofs-8 albedo.png> data/content/world/roof01-holes.json
"""
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from shingle_bake import HOLES  # noqa: E402

DARK = 14          # albedo max channel (0..255) of the attic black
PAD = (.012, .025)  # search this far around the hole's box (U, V), square units: the black runs past it
PAD_BIG = .025      # U pad of the big hole: its black runs on past the box on the right
STEP_PX = 6        # outline rows are merged while the extents move less than this


def outline(mask):
    """Stepped polygon (u, v pixel corners) around a filled area: left edges down, right edges up."""
    rows = [(y, *np.flatnonzero(r)[[0, -1]]) for y, r in enumerate(mask) if r.any()]
    runs = []
    for y, x0, x1 in rows:
        if runs and abs(runs[-1][2] - x0) < STEP_PX and abs(runs[-1][3] - x1) < STEP_PX:
            runs[-1][1] = y
        else:
            runs.append([y, y, x0, x1])
    left = [p for y0, y1, x0, _ in runs for p in ((x0, y0), (x0, y1 + 1))]
    right = [p for y0, y1, _, x1 in reversed(runs) for p in ((x1 + 1, y1 + 1), (x1 + 1, y0))]
    return left + right


def main(albedo, out):
    a = np.asarray(Image.open(albedo).convert('RGB')).max(2).astype(float)
    h, w = a.shape
    holes = []
    for u0, u1, v0, v1, rafter in HOLES:
        pu = PAD_BIG if rafter else PAD[0]
        x0, x1 = int((u0 - pu) * w), int((u1 + pu) * w)
        y0, y1 = int((v0 - PAD[1]) * h), int((v1 + PAD[1]) * h)
        box = a[y0:y1, x0:x1]
        dark = box <= DARK
        # the dark band under each course is painted shadow, thinner than this: it is not the hole
        dark = ndimage.binary_opening(dark, structure=np.ones((29, 1)))
        # the lath and rafter are light bands inside the black: close over them, keep the hole
        filled = ndimage.binary_closing(dark, structure=np.ones((41, 41)), border_value=0)
        filled = ndimage.binary_fill_holes(filled)
        lab, n = ndimage.label(filled)
        sizes = ndimage.sum(filled, lab, range(1, n + 1))
        # the hole and the black pieces beside it (a broken shingle edge splits them) inside the box
        bx0, bx1 = int(pu * w), int((u1 - u0 + pu) * w)
        by0, by1 = int(PAD[1] * h), int((v1 - v0 + PAD[1]) * h)
        keep = [k + 1 for k in range(n) if sizes[k] > .05 * sizes.max() and (lab[by0:by1, bx0:bx1] == k + 1).any()]
        hole = np.isin(lab, keep)
        light = hole & ~dark
        rows = light.sum(1) / np.maximum(hole.sum(1), 1)          # the lath: the row band most covered by light
        band = ndimage.uniform_filter1d(rows, 15)
        ys = np.flatnonzero(hole.any(1)); inner = slice(ys[0] + 20, ys[-1] - 20)   # not the hole's light rim
        k_lath = inner.start + int(np.argmax(band[inner])) if inner.stop > inner.start else None
        lath = (k_lath + y0) / h if k_lath is not None and band[k_lath] > .5 else None
        cols = light.sum(0) / np.maximum(hole.sum(0), 1)
        cols[:bx0], cols[bx1:] = 0, 0     # the rafter is inside the hole's own box
        raft = (int(np.argmax(ndimage.uniform_filter1d(cols, 9))) + x0) / w if rafter else None
        poly = [((x + x0) / w, (y + y0) / h) for x, y in outline(hole)]
        holes.append({'box': [u0, u1, v0, v1], 'outline': [[round(u, 4), round(v, 4)] for u, v in poly],
                      'lath_v': None if lath is None else round(lath, 4), 'rafter_u': None if raft is None else round(raft, 4)})
        print(f'hole {u0:.3f}-{u1:.3f}: {len(poly)} corners, lath v {lath}, rafter u {raft}')
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    Path(out).write_text(json.dumps({'texture': 'NW_CITY_ROOF_01', 'set': 'roofs-8', 'v_from': 'top',
                                     'holes': holes}, indent=1) + '\n')


if __name__ == '__main__':
    main(*sys.argv[1:3])
