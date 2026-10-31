"""Top-down map of Khorinis for the art bible: roofs by material, wall stone, quay, frame cameras.

Shell triangles of the harbour-house `triangles.json` are drawn from above (X right, Z up), coloured
by texture. Cameras come from the survey's `architecture-cameras.csv` and `khorinis-bible.bench`;
their numbers follow FRAMES, the order of the frames in docs/khorinis-art-bible.md.

    uv run --no-project --with matplotlib python scripts/content/khorinis_map.py \
        --triangles ~/.local/share/gothic-ai/harbour-house/triangles.json \
        --cameras ~/.local/share/gothic-ai/khorinis-survey/architecture-cameras.csv \
        --bench scripts/content/khorinis-bible.bench --out reports/content/2026-10-khorinis-bible/map.png
"""

import argparse
import csv
import json
import re
from pathlib import Path

import matplotlib

matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.collections import PolyCollection

GROUPS = (
    (('NW_CITY_ROOF_01',), 'кровля гонтом ROOF_01', '#9a7b4f'),
    (('NW_CITY_ROOF_04',), 'кровля сланцем ROOF_04', '#4f5d6e'),
    (('NW_MISC_STRONG_STONEWALL',), 'камень стены, ворот, башен', '#5b3a29'),
    (('NW_CITY_HAFENKAI_MAUER_01',), 'стенка набережной', '#7f8c8d'),
)
FRAMES = (
    'city-wall', 'city-lighthouse', 'city-hill',
    'habour-overhead', 'habour-quay', 'habour-tavern', 'habour-poor-huts',
    'merchant-overhead', 'merchant-market', 'merchant-house',
    'merchant-temple', 'temple-high',
    'uptown-overhead', 'uptown-house', 'uptown-street', 'uptown-judge',
    'kasern-overhead', 'kasern-courtyard', 'kasern-barrack', 'kasern-armory',
    'gate-uptown-high', 'gate-main-out', 'gate-main-in', 'gate-back-out', 'gate-back-in',
    'lighthouse-high', 'lighthouse-near',
)
LABELS = (  # text, (x, z) in metres, font size
    ('ПОРТ', (-40, 0), 13), ('бедный квартал', (22, -50), 9), ('верфь', (4, 24), 8),
    ('корабль\nпаладинов', (-92, -125), 8), ('вода гавани', (-75, -45), 8),
    ('НИЖНИЙ ГОРОД', (80, 7), 12), ('рынок', (100, 47), 9), ('храм\nАданоса', (98, -9), 8),
    ('ВЕРХНИЙ КВАРТАЛ', (135, -37), 11), ('ратуша', (150, 10), 8), ('КАЗАРМЫ', (45, 75), 11),
    ('главные\nворота', (80, -74), 8), ('задние\nворота', (122, 60), 8),
    ('ворота верхнего\nквартала', (98, -52), 7), ('МАЯК', (-16, 148), 11),
)
BENCH_CAMERA = re.compile(r'# .*?; eye \((-?\d+), (-?\d+), (-?\d+)\), target \((-?\d+), (-?\d+), (-?\d+)\)\.\n'
                          r'(?:.*\n)*?shot (\S+)')


def cameras(survey_csv, bench):
    """{frame: (eye, target)} in cm from the survey table and the bible's capture scenario."""
    found = {}
    with open(survey_csv, newline='') as table:
        for row in csv.DictReader(table):
            found[row['id']] = ([float(row[f'eye_{a}_cm']) for a in 'xyz'],
                                [float(row[f'target_{a}_cm']) for a in 'xyz'])
    for match in BENCH_CAMERA.finditer(Path(bench).read_text()):
        *numbers, name = match.groups()
        found[name] = ([float(v) for v in numbers[:3]], [float(v) for v in numbers[3:]])
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--triangles', required=True)
    parser.add_argument('--cameras', required=True)
    parser.add_argument('--bench', required=True)
    parser.add_argument('--out', required=True)
    args = parser.parse_args()
    shell = json.loads(Path(args.triangles).read_text())['candidate_house_triangles']
    fig, ax = plt.subplots(figsize=(11.5, 10.5), dpi=110)
    ax.set_facecolor('#f4f1ea')
    for prefixes, label, colour in GROUPS:
        polygons = [[(p[0] / 100, p[2] / 100) for p in t['points']] for t in shell if t['texture'].startswith(prefixes)]
        ax.add_collection(PolyCollection(polygons, facecolors=colour, edgecolors='none', alpha=0.9, label=label))
    for text, (x, z), size in LABELS:
        ax.text(x, z, text, ha='center', va='center', fontsize=size, weight='bold', color='#1b1b1b',
                bbox={'boxstyle': 'round,pad=0.25', 'fc': 'white', 'ec': 'none', 'alpha': 0.75})
    found = cameras(args.cameras, args.bench)
    for number, frame in enumerate(FRAMES, 1):
        eye, target = found[frame]
        ex, ez, tx, tz = eye[0] / 100, eye[2] / 100, target[0] / 100, target[2] / 100
        ax.annotate('', xy=(ex + (tx - ex) * 0.35, ez + (tz - ez) * 0.35), xytext=(ex, ez),
                    arrowprops={'arrowstyle': '-|>', 'color': '#c0392b', 'lw': 1.2})
        ax.plot(ex, ez, 'o', color='#c0392b', ms=4)
        ax.text(ex, ez, f' {number}', color='#c0392b', fontsize=8, weight='bold', va='bottom')
    ax.set_xlim(-125, 215)
    ax.set_ylim(-175, 175)
    ax.set_aspect('equal')
    ax.set_xlabel('X, м')
    ax.set_ylabel('Z, м')
    ax.legend(loc='lower right', fontsize=9, framealpha=0.9)
    ax.grid(color='#d8d2c4', lw=0.5)
    fig.tight_layout()
    fig.savefig(args.out, dpi=110)


if __name__ == '__main__':
    main()
