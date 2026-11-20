#!/usr/bin/env python3
"""The tables of the price of a bough scheme in the forest, tree-bough-lod.md: the runs of cost.sh (or any runs of
perf-trees.bench with `perf_report.py run` done), the first one the scheme off.

  cost_tables.py RUNS off s122 b
"""
import argparse
import csv
import json
from pathlib import Path
import re

VIEWS = ('Изнутри леса', 'С 30 м', 'Со 100 м', 'С 300 м', 'Проход героя')
CUT = ('Visibility', 'HiZ-history-seeds', 'HiZ-history-retest', 'Bough-cut')
COUNTERS = ('nodes', 'source_cards', 'parent_cards', 'triangles', 'active_bands', 'version_pairs', 'band_triangles')
VSM = re.compile(r'vsm frame \d+: candidates \d+( LOST)?, .*demand (\d+), .*cut (none|bin [^,]*), .*spilled (\d+)')


def num(value, digits):
    return f'{value:,.{digits}f}'.replace(',', ' ').replace('.', ',').replace('-', '−')


def delta(value, digits):
    return ('+' if value >= 0 else '') + num(value, digits)


def times(run):
    """Per view: frames with a GPU sample, GPU frame, GBuffer, VSM-rendering, visibility and cut, ms; triangles."""
    out = []
    views = json.loads((run / 'report.json').read_text())['places']['trees']['views']
    for index in range(len(VIEWS)):
        if f'view-{index + 1}' not in views:
            break  # a shortened scenario
        view = views[f'view-{index + 1}']
        passes = {name: entry['mean_ms'] for name, entry in view['gpu_passes'].items()}
        out.append((view['gpu']['completed_frames'], view['gpu']['mean_ms'], passes['GBuffer'], passes['VSM-rendering'],
                    sum(passes.get(name, 0.0) for name in CUT), view['amounts']['triangles']['per_frame'] / 1e3))
    return out


def counters(run):
    """Per view: the greatest height, px, and the means of the scheme counters without the first two samples of
    the view, which still carry the slots of the view before."""
    with (run / 'trees/repeat-1/frameprofile.csv').open() as source:
        rows = list(csv.DictReader(source))
    out = []
    for index in range(len(VIEWS)):
        samples = [row for row in rows if row['segment'] == f'perf-trees/view-{index + 1}'][2:]
        if not samples:
            break
        out.append([max(int(row['tree_bough_height_mpx']) for row in samples) / 1e3]
                   + [sum(int(row[f'tree_bough_{name}']) for row in samples) / len(samples) for name in COUNTERS])
    return out


def vsm(run):
    """Frames of the VSM log, frames with a cut, frames with lost candidates, dropped pairs, the peak demand."""
    frames = cut = lost = spilled = peak = 0
    for log in sorted(run.rglob('game.log')):
        for line in log.read_text(errors='replace').splitlines():
            if not line.startswith('vsm frame '):
                continue
            match = VSM.match(line)
            if match is None:
                raise SystemExit(f'{log}: unknown VSM line: {line[:120]}')
            frames += 1
            lost += match[1] is not None
            peak = max(peak, int(match[2]))
            cut += match[3] != 'none'
            spilled += int(match[4])
    return frames, cut, lost, spilled, peak


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('runs', type=Path)
    parser.add_argument('off', help='the run with the scheme off')
    parser.add_argument('on', nargs='+', help='the runs with a scheme on')
    args = parser.parse_args()
    off = times(args.runs / args.off)
    print('| Вид | Состояние | Кадров | Кадр GPU, мс | `GBuffer`, мс | `VSM-rendering`, мс | Видимость и срез, мс '
          '| Треугольники, тыс. |\n|---|---|---:|---:|---:|---:|---:|---:|')
    table = {name: times(args.runs / name) for name in args.on}
    for index, view in enumerate(VIEWS[:len(off)]):
        base = off[index]
        print(f'| {view} | {args.off} | {base[0]} | ' + ' | '.join(
            num(value, digits) for value, digits in zip(base[1:], (3, 3, 3, 3, 1))) + ' |')
        for name in args.on:
            row = table[name][index]
            print(f'| {view} | {name} | {row[0]} | ' + ' | '.join(
                f'{num(value, digits)} ({delta(value - start, digits)})'
                for value, start, digits in zip(row[1:], base[1:], (3, 3, 3, 3, 1))) + ' |')
    print('\n| Вид | Состояние | Макс. высота ели, px | Узлы | Исходные карточки | Родительские карточки '
          '| Треугольники хвои | Активные полосы | Пары версий | Треугольники в полосах |\n'
          '|---|---|---:|---:|---:|---:|---:|---:|---:|---:|')
    table = {name: counters(args.runs / name) for name in [args.off] + args.on}
    for index, view in enumerate(VIEWS[:len(off)]):
        for name in [args.off] + args.on:
            row = table[name][index]
            print(f'| {view} | {name} | {num(row[0], 3)} | ' + ' | '.join(num(value, 0) for value in row[1:]) + ' |')
    print('\n| Состояние | Кадров в журнале VSM | С переполнением | С потерей кандидатов | Сброшено пар | Пик спроса |\n'
          '|---|---:|---:|---:|---:|---:|')
    for name in [args.off] + args.on:
        print(f'| {name} | ' + ' | '.join(num(value, 0) for value in vsm(args.runs / name)) + ' |')


if __name__ == '__main__':
    main()
