#!/usr/bin/env python3
"""Compare shader build variants from the A/B runs of perf.sh with OPENGOTHIC_GPU_PASSES=1.

    python3 compare.py <runs.tsv> [--top N]

runs.tsv rows: variant, kind, load, perf dir. Rows of kind 'warm' are dropped. Per place the script prints the frame
and GPU means of every run in order, then the work of each pass per variant. Work means the fragment stage plus the
vertex stage of a render encoder and the whole row of a compute encoder: a render row also holds its waiting time
(passes.py). Rows overlap on the GPU, so the work of the passes does not add up to the frame.
"""
import csv
import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path


def pass_work(gpu_csv: Path) -> dict[str, float]:
    per_frame = defaultdict(lambda: defaultdict(float))
    with gpu_csv.open() as source:
        for row in csv.DictReader(source):
            if not row['segment'].startswith('perf-') or row['valid'] != '1' or int(row['gpu_ns']) >= 1e9:
                continue
            work = int(row['gpu_ns']) if row['type'] != 'render' else int(row['fragment_ns'])+int(row['vertex_ns'])
            per_frame[(row['segment'], row['attempt'])][f"{row['type']}: {row['labels']}"] += work/1e6
    frames = len(per_frame)
    totals = defaultdict(float)
    for rows in per_frame.values():
        for label, ms in rows.items():
            totals[label] += ms
    return {label: ms/frames for label, ms in totals.items()}


def main() -> None:
    args = sys.argv[1:]
    top = 12
    if '--top' in args:
        at = args.index('--top')
        top = int(args[at+1])
        del args[at:at+2]
    table = Path(args[0])
    root = table.resolve().parents[2]
    runs = [line.split('\t') for line in table.read_text().splitlines() if line.strip()]
    runs = [(variant, kind, float(load), root/folder.strip()) for variant, kind, load, folder in runs if kind != 'warm']
    variants = list(dict.fromkeys(r[0] for r in runs))
    reports = {r[1]: json.loads((r[3]/'report.json').read_text()) for r in runs}
    places = sorted(next(iter(reports.values()))['places'])
    for place in places:
        print(f'== {place}')
        print('  run  variant  load   frame ms  p95 ms   gpu')
        means = defaultdict(list)
        for variant, kind, load, _ in runs:
            summary = reports[kind]['places'][place]['summary']
            frame = summary['frame_ms']
            gpu = summary.get('gpu') or {}
            means[variant].append(frame['mean'])
            print(f"  {kind}  {variant:7s} {load:5.1f}  {frame['mean']:7.3f}  {frame['p95']:7.3f}  {json.dumps(gpu)[:90]}")
        print('  frame mean per variant: '+', '.join(f'{v} {statistics.mean(means[v]):.3f}' for v in variants))
        work = {r[1]: pass_work(r[3]/place/'repeat-1'/'frameprofile.csv.gpu.csv') for r in runs}
        labels = sorted({label for rows in work.values() for label in rows},
                        key=lambda label: -statistics.mean(rows.get(label, 0.0) for rows in work.values()))
        print('  pass work, ms: '+'  '.join(f'{v} (runs)' for v in variants))
        for label in labels[:top]:
            cells = []
            for variant in variants:
                values = [work[kind].get(label, 0.0) for v, kind, _, _ in runs if v == variant]
                cells.append(f"{statistics.mean(values):6.3f} ({'/'.join(f'{x:.2f}' for x in values)})")
            print(f"  {'  '.join(cells)}  {label[:60]}")


if __name__ == '__main__':
    main()
