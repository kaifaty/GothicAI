#!/usr/bin/env python3
"""Compare the first N game minutes of two npc-day runs row by row (journal and events)."""
import csv, sys
from pathlib import Path
a, b, limit = Path(sys.argv[1]), Path(sys.argv[2]), float(sys.argv[3])
skip = set(sys.argv[4].split(',')) if len(sys.argv) > 4 else set()

def rows(path, timecol):
    with path.open(newline='') as f:
        r = csv.DictReader(f)
        return r.fieldnames, [row for row in r if float(row[timecol]) <= limit]

for name, timecol in (('npc-day.csv', 't_min'), ('npc-day-events.csv', None)):
    fa = a / name; fb = b / name
    if timecol is None:
        with fa.open() as f: timecol = next(csv.reader(f))[0]
    ha, ra = rows(fa, timecol); hb, rb = rows(fb, timecol)
    cols = [c for c in ha if c not in skip]
    diff_rows = 0; diff_cols = {}
    first = None
    for x, y in zip(ra, rb):
        d = [c for c in cols if x.get(c) != y.get(c)]
        if d:
            diff_rows += 1
            for c in d: diff_cols[c] = diff_cols.get(c, 0) + 1
            if first is None: first = (x, y, d)
    print(f'{name}: header_same={ha==hb} rows {len(ra)} vs {len(rb)}, differing rows {diff_rows}, by column {diff_cols}')
    if first:
        x, y, d = first
        print('  first:', {c: (x[c], y[c]) for c in d}, 'key', x.get('key'), 't', x.get(timecol))
