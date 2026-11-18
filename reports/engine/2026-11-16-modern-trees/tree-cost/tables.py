#!/usr/bin/env python3
"""Tables of tree-cost.md from the runs of run.sh: GPU time by pass groups, triangles, the forest's share.

    python3 reports/engine/2026-11-16-modern-trees/tree-cost/tables.py /absolute/run/folder [more folders]

A folder holds <variant>/round-<n>/report.json (perf_report.py of each `perf.sh __run`); several folders are read
as one (a repeat of runs a hang stopped). Prints markdown: per view the mean of the rounds and their spread, and
the difference against the control C of the same view.
"""
from __future__ import annotations

import json
import statistics
import sys
from pathlib import Path

GROUPS = {  # the rows of gpu_passes each group sums (perf_report.py names the markers)
    'GBuffer': ('GBuffer',),
    'HiZ': ('HiZ-occluders', 'HiZ-mip'),
    'VSM': ('VSM-rendering', 'VSM-visibility', 'VSM-pages', 'VSM-clear', 'VSM-mark'),
    'Sun': ('DirectSunLight-VSM', 'DirectSunLight'),
    'GI': ('GI-Trace', 'GI-Lighting', 'GI-Alloc', 'GI-HashMap', 'GI-Reproject', 'Irradiance', 'RT-TLAS'),
    'Grass': ('Grass',),
}
VIEWS = ('view-1', 'view-2', 'view-3', 'view-4', 'view-5')
NAMES = {'view-1': 'внутри', 'view-2': '30 м', 'view-3': '100 м', 'view-4': '300 м', 'view-5': 'проход'}


def load(folders: list[Path]) -> dict:
    """variant -> view -> list of per-round dicts: gpu, triangles, draws, groups, passes."""
    data: dict = {}
    for folder in folders:
        for report in sorted(folder.glob('*/round-*/report.json')):
            variant = report.parent.parent.name
            views = json.loads(report.read_text())['places']['trees']['views']
            for view in VIEWS:
                if view not in views:
                    continue
                s = views[view]
                passes = {k: v['mean_ms'] for k, v in s.get('gpu_passes', {}).items()}
                groups = {g: sum(passes.get(p, 0.) for p in names) for g, names in GROUPS.items()}
                groups['rest'] = sum(passes.values()) - sum(groups.values())
                data.setdefault(variant, {}).setdefault(view, []).append(dict(
                    gpu=s['gpu']['mean_ms'], frame=s['frame_ms']['p50'],
                    triangles=s['amounts']['triangles']['per_frame'] / 1e6,
                    draws=s['amounts']['draw_calls']['per_frame'], groups=groups, passes=passes,
                    round=report.parent.name))
    return data


def mean(rows: list[dict], key, sub=None) -> tuple[float, float]:
    values = [r[key][sub] if sub else r[key] for r in rows]
    return statistics.fmean(values), (max(values) - min(values)) if len(values) > 1 else float('nan')


def fmt(x: float, digits: int = 2) -> str:
    return '—' if x != x else f'{x:.{digits}f}'.replace('.', ',')


def main() -> int:
    data = load([Path(a) for a in sys.argv[1:]])
    variants = [v for v in ('C', 'F64', 'F256', 'F1024', 'F1024h', 'F1024q', 'F1024e', 'C-rt0', 'F1024-rt0',
                            'F1024-wind0', 'F1024-crown0') if v in data]
    print('## Время видеокарты, мс (среднее прогонов; в скобках — разброс между ними), треугольники, млн за кадр\n')
    print('| Вариант | прогонов | ' + ' | '.join(NAMES[v] for v in VIEWS) + ' |')
    print('|---|---|' + '---|' * len(VIEWS))
    for variant in variants:
        cells = []
        for view in VIEWS:
            rows = data[variant].get(view, [])
            if not rows:
                cells.append('—')
                continue
            m, spread = mean(rows, 'gpu')
            t, _ = mean(rows, 'triangles')
            cells.append(f'{fmt(m)} ({fmt(spread)}); {fmt(t, 1)}')
        print(f'| {variant} | {len(data[variant]["view-1"])} | ' + ' | '.join(cells) + ' |')

    for base, label in (('C', 'против C'), ('C-rt0', 'против C-rt0')):
        if base not in data:
            continue
        print(f'\n## Разность с контролем по группам проходов, мс ({label})\n')
        print('| Вариант | Вид | всего | ' + ' | '.join(list(GROUPS) + ['rest']) + ' | треуг., млн |')
        print('|---|---|---|' + '---|' * (len(GROUPS) + 2))
        for variant in variants:
            if variant == base or (base == 'C') == variant.endswith('rt0'):
                continue
            for view in VIEWS:
                rows, control = data[variant].get(view), data[base].get(view)
                if not rows or not control:
                    continue
                cells = [fmt(mean(rows, 'gpu')[0] - mean(control, 'gpu')[0])]
                cells += [fmt(mean(rows, 'groups', g)[0] - mean(control, 'groups', g)[0]) for g in list(GROUPS) + ['rest']]
                cells.append(fmt(mean(rows, 'triangles')[0] - mean(control, 'triangles')[0], 1))
                print(f'| {variant} | {NAMES[view]} | ' + ' | '.join(cells) + ' |')
    (Path(sys.argv[1]) / 'tables.json').write_text(json.dumps(data, indent=1))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
