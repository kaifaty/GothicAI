#!/usr/bin/env python3
"""Measure the grass of the fringe layer on frames of the game against the original's painted grass.

    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/landscape_grass_measure.py \
        --mask <frames of the mask set> --original <frames with OPENGOTHIC_CONTENT=> \
        --layer now=<frames> --layer new=<frames> [--views ledge road-3] [--json out.json]

The frames are those of scripts/bench/landscape-rock-material.bench, one folder per set. The mask set
(landscape_grass_layer.py --mask) paints the solid band of the layer magenta; its pixels, less a rim of 2 px, are
the pixels compared. For each view and for all of them together: the mean per channel and the spread of luma (its
standard deviation) in linear display RGB, the ratios original / layer (the on-screen correction of the mean is
their product with the one the set was built with), and the spread of luma within 16 px (luma less its blur): the
contrast of the blades without that of sun and shade.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np

LUMA = np.array((.2126, .7152, .0722))


def linear(path: Path) -> np.ndarray:
    from PIL import Image
    with Image.open(path) as image:
        value = np.asarray(image.convert('RGB'), np.float64) / 255
    return np.where(value <= .04045, value / 12.92, ((value + .055) / 1.055) ** 2.4)


def band(frame: np.ndarray) -> np.ndarray:
    """Pixels of the magenta band: red and blue well over green, away from its rim."""
    from scipy.ndimage import binary_erosion
    r, g, b = frame[..., 0], frame[..., 1], frame[..., 2]
    return binary_erosion((np.minimum(r, b) > 3 * g + .004) & (np.abs(r - b) < .5 * np.maximum(r, b)), iterations=2)


def stats(frame: np.ndarray, mask: np.ndarray) -> dict:
    from scipy.ndimage import gaussian_filter
    luma = frame @ LUMA
    weight = gaussian_filter(mask.astype(np.float64), 16)
    near = gaussian_filter(luma * mask, 16) / np.maximum(weight, 1e-6)     # the blur of the band's pixels alone
    return dict(mean=frame[mask].mean(axis=0), luma=float(luma[mask].mean()), spread=float(luma[mask].std()),
                blades=float((luma - near)[mask].std()))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--mask', type=Path, required=True)
    parser.add_argument('--original', type=Path, required=True)
    parser.add_argument('--layer', action='append', required=True, metavar='NAME=DIR')
    parser.add_argument('--views', nargs='+', default=['ledge', 'road-3'])
    parser.add_argument('--json', type=Path)
    args = parser.parse_args()
    sets = dict(original=args.original, **{name: Path(folder) for name, folder in (v.split('=', 1) for v in args.layer)})

    report = {}
    frames = {name: [] for name in sets}
    masks = []
    for view in args.views:
        mask = band(linear(args.mask / f'{view}.png'))
        masks.append(mask)
        report[view] = dict(pixels=int(mask.sum()))
        for name, folder in sets.items():
            frame = linear(folder / f'{view}.png')
            frames[name].append(frame[mask])
            report[view][name] = stats(frame, mask)
    report['all'] = dict(pixels=int(sum(m.sum() for m in masks)))
    for name in sets:
        value = np.concatenate(frames[name])
        luma = value @ LUMA
        # the blades' contrast of the views together: by pixels
        blades = np.sqrt(sum(report[v][name]['blades'] ** 2 * report[v]['pixels'] for v in args.views)
                         / report['all']['pixels'])
        report['all'][name] = dict(mean=value.mean(axis=0), luma=float(luma.mean()), spread=float(luma.std()),
                                   blades=float(blades))
    for view, row in report.items():
        print(f'{view}: {row["pixels"]} px')
        original = row['original']
        for name in sets:
            value = row[name]
            line = (f'  {name:9s} mean {value["mean"][0]:.4f} {value["mean"][1]:.4f} {value["mean"][2]:.4f}'
                    f'  luma {value["luma"]:.4f}  spread {value["spread"]:.4f}  blades {value["blades"]:.4f}')
            if name != 'original':
                ratio = original['mean'] / value['mean']
                value['original_over'] = ratio
                line += (f'  | original/layer {ratio[0]:.3f} {ratio[1]:.3f} {ratio[2]:.3f}'
                         f'  luma {original["luma"] / value["luma"]:.3f}  spread {original["spread"] / value["spread"]:.3f}'
                         f'  blades {original["blades"] / value["blades"]:.3f}')
            print(line)
    if args.json:
        args.json.write_text(json.dumps(report, indent=2, default=lambda v: np.round(v, 5).tolist()) + '\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
