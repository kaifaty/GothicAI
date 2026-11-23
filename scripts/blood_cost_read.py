#!/usr/bin/env python3
"""Blood decal cost (physics stage 8, step 4c): app-frame p50/p95 of blood-cost runs, ON against OFF.

    python3 scripts/blood_cost_read.py ON_DIR [ON_DIR...] -- OFF_DIR [OFF_DIR...]

Each directory is a `scripts/bench.sh blood-cost` output with a complete 30-second normal-clock
`appcadence.csv`. Prints each run and the median of p95 per mode; the target is ON - OFF <= 0.2 ms.
"""
import csv
import json
import statistics
import sys
from pathlib import Path


def percentile(values, q):
    values = sorted(values)
    return values[min(len(values)-1, int(round(q*(len(values)-1))))]


def read(directory):
    directory = Path(directory)
    meta = json.loads((directory / 'appcadence.csv.json').read_text())
    if not meta.get('complete') or meta.get('overflow'):
        raise SystemExit(f'{directory}: incomplete cadence window')
    with (directory / 'appcadence.csv').open() as source:
        frames = [int(row['interval_ns'])/1e6 for row in csv.DictReader(source)]
    return {'run': directory.name, 'mode': meta.get('mode'), 'frames': len(frames),
            'p50_ms': percentile(frames, 0.5), 'p95_ms': percentile(frames, 0.95)}


def main(argv):
    if '--' not in argv:
        raise SystemExit(__doc__)
    split = argv.index('--')
    on, off = [read(d) for d in argv[:split]], [read(d) for d in argv[split+1:]]
    for row in on+off:
        print(f"{row['run']:>16} {row['mode']:>3} frames {row['frames']:5} "
              f"p50 {row['p50_ms']:.3f} ms  p95 {row['p95_ms']:.3f} ms")
    on95 = statistics.median(r['p95_ms'] for r in on)
    off95 = statistics.median(r['p95_ms'] for r in off)
    delta = on95-off95
    print(f'median p95: on {on95:.3f} ms, off {off95:.3f} ms, delta {delta:+.3f} ms '
          f"({'within' if delta <= 0.2 else 'over'} the 0.2 ms target)")


if __name__ == '__main__':
    main(sys.argv[1:])
