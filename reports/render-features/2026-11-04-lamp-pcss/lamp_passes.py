#!/usr/bin/env python3
"""The lighting encoder's GPU interval per view from perf.sh runs with OPENGOTHIC_GPU_PASSES=1.

    python3 lamp_passes.py <perf dir> [<perf dir> ...]

Prints, per run (place/repeat) and view, the mean per frame of the encoders whose labels contain
'Point lights' (on Metal the sun's VSM pass and the lamps share one render encoder), and the mean of the four views.
"""
import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path


def lighting_ms(gpu_csv: Path) -> dict[str, float]:
    frames: dict[tuple[str, str], float] = defaultdict(float)
    with gpu_csv.open() as source:
        for row in csv.DictReader(source):
            if row['valid'] != '1' or not row['segment'].startswith('perf-'):
                continue
            if 'Point lights' not in row['labels']:
                continue
            frames[(row['segment'], row['frame'])] += int(row['gpu_ns']) / 1e6
    views: dict[str, list[float]] = defaultdict(list)
    for (segment, _), ms in frames.items():
        views[segment.split('/')[-1]].append(ms)
    return {view: statistics.mean(values) for view, values in sorted(views.items())}


def main() -> None:
    for root in map(Path, sys.argv[1:]):
        for gpu_csv in sorted(root.glob('*/repeat-*/frameprofile.csv.gpu.csv')):
            views = lighting_ms(gpu_csv)
            if not views:
                continue
            leaf = gpu_csv.parent
            cells = ' '.join(f'{view}={ms:.2f}' for view, ms in views.items())
            print(f'{root.name} {leaf.parent.name}/{leaf.name}: mean={statistics.mean(views.values()):.2f} ms  {cells}')


if __name__ == '__main__':
    main()
