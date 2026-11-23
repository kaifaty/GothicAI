#!/usr/bin/env python3
"""Resolution-only GPU passes as a clock thermometer along a whole queue job.

usage: clock.py LEAF... (folders with frameprofile.csv and .gpu.csv), in job order.
Prints JSON lines per leaf: absolute start/end, and 4 s bins of [t_abs_s, segment, frames, gpu_ms, pass means...].
"""
import csv
import json
import sys
from pathlib import Path

csv.field_size_limit(1 << 30)
PASSES = ('TAA', 'SSAO-blur', 'Tonemapping', 'VSM-visibility', 'AmbientLight', 'GI-Alloc', 'Fog')
BIN = 4e9

for arg in sys.argv[1:]:
    leaf = Path(arg)
    frames = {}
    with (leaf / 'frameprofile.csv').open() as f:
        r = csv.reader(f)
        head = next(r)
        ix = {n: head.index(n) for n in ('attempt', 'segment', 'begin_ns', 'outcome', 'gpu_ns', 'gpu_done')}
        for raw in r:
            if raw[ix['outcome']] != '1' or raw[ix['gpu_done']] == '0' or int(raw[ix['gpu_ns']]) <= 0:
                continue
            frames[int(raw[ix['attempt']])] = [int(raw[ix['begin_ns']]), raw[ix['segment']], int(raw[ix['gpu_ns']]), {}]
    with (leaf / 'frameprofile.csv.gpu.csv').open() as f:
        for row in csv.DictReader(f):
            a = int(row['attempt'])
            if row['valid'] != '1' or a not in frames or row['labels'] not in PASSES:
                continue
            d = frames[a][3]
            d[row['labels']] = d.get(row['labels'], 0) + int(row['gpu_ns'])
    bins = {}
    for b, seg, g, d in frames.values():
        k = int(b // BIN)
        e = bins.setdefault(k, {'seg': seg, 'n': 0, 'gpu': 0, **{p: [0, 0] for p in PASSES}})
        e['n'] += 1
        e['gpu'] += g
        for p, ns in d.items():
            e[p][0] += ns
            e[p][1] += 1
    out = []
    for k, e in sorted(bins.items()):
        out.append([round(k * BIN / 1e9, 1), e['seg'], e['n'], round(e['gpu'] / e['n'] / 1e6, 3)] +
                   [round(e[p][0] / e[p][1] / 1e6, 4) if e[p][1] else None for p in PASSES])
    starts = [v[0] for v in frames.values()]
    print(json.dumps({'leaf': str(leaf), 'passes': PASSES, 't0': min(starts) / 1e9, 't1': max(starts) / 1e9, 'bins': out}))
