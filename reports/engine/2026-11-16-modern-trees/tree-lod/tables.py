#!/usr/bin/env python3
"""The price table of tree-lod.md from the runs of run.sh: per view the frame, GBuffer, VSM-rendering, GI-Lighting,
triangles and the spruces at each level (the profile's mesh_level0..4), levels off and on.

    python3 reports/engine/2026-11-16-modern-trees/tree-lod/tables.py /run/folder

The folder holds L-off/round-1/report.json and L-on/round-1/report.json (perf_report.py of `perf.sh __run`).
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

VIEWS = {'view-1': 'внутри', 'view-2': '30 м', 'view-3': '100 м', 'view-4': '300 м', 'view-5': 'проход'}
PASSES = ('GBuffer', 'VSM-rendering', 'GI-Lighting')


def fmt(x: float) -> str:
    return f'{x:.2f}'.replace('.', ',')


def main() -> int:
    folder = Path(sys.argv[1])
    runs = {v: json.loads((folder / v / 'round-1/report.json').read_text())['places']['trees']['views']
            for v in ('L-off', 'L-on')}
    print('| Вид | кадр, мс | ' + ' | '.join(f'`{p}`, мс' for p in PASSES) + ' | треуг., млн | елей на уровнях 0/1/2/3/4 |')
    print('|---|---|' + '---|' * len(PASSES) + '---|---|')
    for view, name in VIEWS.items():
        cells = []
        for key in ('gpu',) + PASSES:
            pair = [runs[v][view]['gpu']['mean_ms'] if key == 'gpu' else
                    runs[v][view]['gpu_passes'].get(key, {}).get('mean_ms', 0.) for v in ('L-off', 'L-on')]
            cells.append(f'{fmt(pair[0])} → {fmt(pair[1])}')
        tri = [runs[v][view]['amounts']['triangles']['per_frame'] / 1e6 for v in ('L-off', 'L-on')]
        amounts = runs['L-on'][view]['amounts']
        levels = '/'.join(f"{amounts.get(f'mesh_level{i}', {}).get('per_frame', 0.):.0f}" for i in range(5))
        tris = ' → '.join(f'{t:.1f}'.replace('.', ',') for t in tri)
        print(f'| {name} | ' + ' | '.join(cells) + f' | {tris} | {levels} |')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
