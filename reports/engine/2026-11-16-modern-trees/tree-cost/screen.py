#!/usr/bin/env python3
"""How big the spruces of a forest set are on the screen in the views of scripts/bench/perf-trees.bench.

    python3 reports/engine/2026-11-16-modern-trees/tree-cost/screen.py <set folder> [--width 2560 --height 1440]

For each still view (the `cam at` lines of the scenario) and each spruce of the set's layer
(worlds/NEWWORLD/vobs.d/tree-cost.json, sizes from tree-cost.json): whether its box is in the view's frustum, and the
height on the screen of its trunk axis from the root to the top, f * H / depth with f from the vertical field of view
(67.5 degrees, Settings::cameraFov). No occlusion: the frustum only, as the engine's frustum test sees it. Prints per
view how many spruces and what share of their triangles fall in each band of height in pixels.
"""
from __future__ import annotations

import argparse
import json
import math
import re
from pathlib import Path

import numpy as np

BANDS = (0, 12, 25, 50, 100, 200, 400, 800, 1e9)
FOV_Y = 67.5
SCENARIO = Path(__file__).resolve().parents[4] / 'scripts/bench/perf-trees.bench'
NAMES = ('view-1', 'view-2', 'view-3', 'view-4')


def cameras() -> list[tuple[np.ndarray, np.ndarray]]:
    lines = re.findall(r'^cam at ([-\d. ]+)$', SCENARIO.read_text(), re.M)
    return [(np.array(v[:3]), np.array(v[3:])) for v in (list(map(float, l.split())) for l in lines)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('set', type=Path)
    parser.add_argument('--width', type=int, default=2560)
    parser.add_argument('--height', type=int, default=1440)
    args = parser.parse_args()
    report = json.loads((args.set / 'tree-cost.json').read_text())
    spruces = {s['visual']: s for s in report['spruces']}
    layer = json.loads((args.set / 'worlds/NEWWORLD/vobs.d/tree-cost.json').read_text())['add']
    trees = [a for a in layer if a['key'] != 'tree-cost-lone']
    root = np.array([a['position'] for a in trees])
    tall = np.array([spruces[a['visual']]['height_m'] * 100 for a in trees])
    wide = np.array([max(spruces[a['visual']]['crown_width_m']) * 100 for a in trees])
    tris = np.array([spruces[a['visual']]['triangles'] for a in trees])
    focal = args.height / 2 / math.tan(math.radians(FOV_Y) / 2)
    aspect = args.width / args.height
    out = {}
    print(f'{len(trees)} spruces, {tris.sum() / 1e6:.1f} M triangles; f = {focal:.0f} px')
    for name, (eye, target) in zip(NAMES, cameras()):
        forward = (target - eye) / np.linalg.norm(target - eye)
        right = np.cross(forward, (0., 1., 0.))
        right /= np.linalg.norm(right)
        up = np.cross(right, forward)
        centre = root + np.stack([np.zeros_like(tall), tall / 2, np.zeros_like(tall)], axis=1) - eye
        depth = centre @ forward
        radius = np.hypot(tall / 2, wide / 2)
        x, y = centre @ right, centre @ up
        tan_y = math.tan(math.radians(FOV_Y) / 2)
        inside = ((depth > -radius) & (np.abs(x) <= depth * tan_y * aspect + radius * 1.2)
                  & (np.abs(y) <= depth * tan_y + radius * 1.2))
        px = focal * tall / np.maximum(depth, 1.)
        rows = []
        for low, high in zip(BANDS[:-1], BANDS[1:]):
            band = inside & (px >= low) & (px < high)
            rows.append(dict(px=[low, high if high < 1e9 else None], trees=int(band.sum()),
                             triangles_m=round(float(tris[band].sum()) / 1e6, 2),
                             share=round(float(tris[band].sum() / max(tris[inside].sum(), 1)), 3)))
        distance = np.linalg.norm(centre, axis=1) / 100
        out[name] = dict(in_frustum=int(inside.sum()), triangles_m=round(float(tris[inside].sum()) / 1e6, 2),
                         nearest_m=round(float(distance[inside].min()), 1) if inside.any() else None,
                         median_m=round(float(np.median(distance[inside])), 1) if inside.any() else None,
                         bands=rows)
        print(f'\n{name}: {inside.sum()} spruces in the frustum, {tris[inside].sum() / 1e6:.1f} M triangles, '
              f'nearest {out[name]["nearest_m"]} m, median {out[name]["median_m"]} m')
        for r in rows:
            high = r['px'][1] or '∞'
            print(f'  {r["px"][0]:>4}-{high:<5} px: {r["trees"]:4} spruces, {r["triangles_m"]:6.2f} M, {r["share"]:.0%}')
    (args.set / 'screen.json').write_text(json.dumps(out, indent=1) + '\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
