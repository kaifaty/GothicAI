#!/usr/bin/env python3
"""GPU encoder intervals of perf.sh runs with OPENGOTHIC_GPU_PASSES=1, for the temporal upscale (graphics plan 1г).

    python3 passes.py <perf dir> [<perf dir> ...]

Per run (place/repeat): the frames of the perf segments; the mean per frame of each encoder row by its labels (rows
that share labels in one frame are summed), the whole row and its fragment stage (a render pass starts its vertices
early and waits, so the row is longer than its work); the 'metalfx' row of MetalFX Temporal (the gap between its timed
neighbours) with the rows around it. Rows overlap on the GPU: they do not add up to the frame. Rows over 1 s are
broken samples and are left out.
"""
import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path


def frames_of(gpu_csv: Path):
    frames = defaultdict(list)
    with gpu_csv.open() as source:
        for row in csv.DictReader(source):
            if not row['segment'].startswith('perf-'):
                continue
            frames[(row['segment'], row['attempt'])].append(row)
    return [sorted(rows, key=lambda r: int(r['encoder'])) for rows in frames.values()]


def report(gpu_csv: Path, top: int) -> None:
    frames = frames_of(gpu_csv)
    if not frames:
        print('  no perf frames')
        return
    per_label = defaultdict(list)
    for rows in frames:
        sums = defaultdict(lambda: [0.0, 0.0])
        for r in rows:
            if r['valid'] == '1' and int(r['gpu_ns']) < 1e9:
                sums[(r['type'], r['labels'])][0] += int(r['gpu_ns'])/1e6
                sums[(r['type'], r['labels'])][1] += int(r['fragment_ns'])/1e6
        for key, ms in sums.items():
            per_label[key].append(ms)
    n = len(frames)
    print(f'  {n} frames')
    ranked = sorted(per_label.items(), key=lambda kv: -sum(v[0] for v in kv[1])/n)
    for (kind, labels), values in ranked[:top]:
        row = sum(v[0] for v in values)/n
        frag = sum(v[1] for v in values)/n
        print(f'  row {row:7.3f} ms, fragments {frag:6.3f} ms  {kind:8s} {labels[:100]}')
    # MetalFX and its neighbours
    around = defaultdict(list)
    for rows in frames:
        for i, r in enumerate(rows):
            if r['type'] != 'metalfx':
                continue
            for d in (-2, -1, 0, 1, 2):
                if 0 <= i+d < len(rows) and rows[i+d]['valid'] == '1' and int(rows[i+d]['gpu_ns']) < 1e9:
                    around[(d, rows[i+d]['type'], rows[i+d]['labels'])].append(int(rows[i+d]['gpu_ns'])/1e6)
    if around:
        print('  metalfx and its neighbours (mean, median, frames):')
        for (d, kind, labels), values in sorted(around.items()):
            print(f'    {d:+d} {statistics.mean(values):7.3f} {statistics.median(values):7.3f} ms {len(values):4d}  '
                  f'{kind:8s} {labels[:90]}')


def main() -> None:
    top = 14
    for root in map(Path, sys.argv[1:]):
        for gpu_csv in sorted(root.glob('*/repeat-*/frameprofile.csv.gpu.csv')):
            leaf = gpu_csv.parent
            print(f'{root.name} {leaf.parent.name}/{leaf.name}')
            report(gpu_csv, top)


if __name__ == '__main__':
    main()
