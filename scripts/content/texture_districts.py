"""Share of each city texture's area per Khorinis district, for the art bible.

Every patch of `intent.json` (71 NW_CITY_/NW_HARBOUR_ textures, the whole world) and every
NW_MISC_ triangle of the harbour-house `triangles.json` (city bounds only: house shell triangles,
and upward ground faces for the NW_MISC_ textures the shell does not carry) goes to the district of
the nearest waypoint. A waypoint's district comes from its name; an NW_CITY_ name the rules do not
know defers to the nearest waypoint with a known district. Numbers only: no pixels are read.

    python3 scripts/content/texture_districts.py --intent intent.json --triangles triangles.json \
        --out reports/content/2026-10-khorinis-bible/texture-districts.csv
"""

import argparse
import csv
import json
from pathlib import Path

import numpy as np

DISTRICTS = ('harbour', 'lower', 'temple', 'upper', 'barracks', 'gates', 'lighthouse', 'sewer', 'outside')

# Named spots first: the routine points of known people, then the district prefixes.
NAMED = (
    ('upper', ('NW_CITY_SALANDRIL', 'NW_CITY_CORNELIUS', 'NW_CITY_LARIUS', 'NW_CITY_LUTERO', 'NW_CITY_HAGEN',
               'NW_CITY_ALBRECHT')),
    ('lower', ('NW_CITY_HOTEL', 'NW_CITY_MATTEO', 'NW_CITY_ZURIS')),
    ('harbour', ('NW_CITY_SONJA', 'NW_CITY_NADJA', 'NW_CITY_KARDIF', 'NW_CITY_SHIP_GUARD', 'NW_CITY_SIT_BRAHIM',
                 'NW_CITY_POOR_SMITH', 'NW_CITY_WATCH_FIGHT', 'NW_CITY_BED_BROMOR', 'NW_CITY_BED_BRAHIM')),
    ('barracks', ('NW_CITY_TRAIN', 'NW_CITY_ARMORY_PECK')),
    ('outside', ('NW_CITYFOREST', 'NW_CITY_SMFOREST', 'NW_CITY_TO_FOREST', 'NW_CITY_PIRATESCAMP',
                 'NW_CITY_TO_FARM2')),
)
PREFIXES = (
    ('barracks', ('NW_CITY_HABOUR_KASERN', 'NW_CITY_BARRACK', 'NW_CITY_ANDRE', 'NW_CITY_KASERN')),
    ('harbour', ('NW_CITY_HABOUR', 'NW_CITY_PATH_HABOUR', 'NW_CITY_WAY_TO_SHIP', 'NW_CITY_PALCAMP', 'NW_CITY_PUFF',
                 'SHIP', 'FP_ROAM_FISHER', 'NW_WAITFOR_SHIP')),
    ('temple', ('NW_CITY_MERCHANT_TEMPLE',)),
    ('lower', ('NW_CITY_MERCHANT', 'NW_CITY_MAINSTREET', 'MARKT', 'NW_CITY_SMITH', 'NW_CITY_BOGNER', 'NW_CITY_TAVERN',
               'NW_CITY_SHOP', 'NW_CITY_HAKON', 'NW_CITY_SARAH', 'DOWNTOWN', 'NW_CITY_BED_', 'NW_CITY_CHAIR_',
               'NW_CITY_BOOK_')),
    ('upper', ('NW_CITY_UPTOWN', 'NW_CITYHALL', 'NW_CITY_CITYHALL', 'NW_CITY_REICH', 'NW_CITY_RICHTER',
               'NW_CITY_LEOMAR', 'NW_CITY_GERBRANDT', 'NW_CITY_JUDGE', 'NW_CITY_GUARD')),
    ('gates', ('NW_CITY_ENTRANCE', 'NW_FARM1_CONNECT_CITY', 'CITY1', 'CITY2', 'NW_CITY_CONNECT_FOREST',
               'NW_CITY_DITCH')),
    ('lighthouse', ('NW_LIGHTHOUSE', 'LIGHTHOUSE', 'NW_CITY_TO_LIGHTHOUSE')),
    ('sewer', ('NW_CITY_KANAL', 'NW_CITY_TO_KANAL')),
)


def district(name):
    """District of a waypoint name; None for an NW_CITY_ name no rule knows."""
    for rules in (NAMED, PREFIXES):
        for label, prefixes in rules:
            if name.startswith(prefixes):
                return label
    return None if name.startswith('NW_CITY') else 'outside'


class Locator:
    def __init__(self, waypoints):
        self.xyz = np.array([w['position'] for w in waypoints], dtype=float)
        self.labels = [district(w['name']) for w in waypoints]
        self.known = np.array([label is not None for label in self.labels])

    def __call__(self, point):
        d2 = ((self.xyz - np.asarray(point, dtype=float)) ** 2).sum(axis=1)
        nearest = int(np.argmin(d2))
        if self.labels[nearest] is None:
            nearest = int(np.argmin(np.where(self.known, d2, np.inf)))
        return self.labels[nearest]


def shares(items, locate):
    """{texture: {district: m²}} from (texture, centre, area) items."""
    table = {}
    for texture, centre, area in items:
        row = table.setdefault(texture, dict.fromkeys(DISTRICTS, 0.0))
        row[locate(centre)] += area
    return table


def centre_area(points):
    """Centre (cm) and area (m²) of a triangle given by three points in cm."""
    a, b, c = np.asarray(points, dtype=float)
    return ((a + b + c) / 3).tolist(), float(np.linalg.norm(np.cross(b - a, c - a)) / 20000)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--intent', required=True, help='intent.json of scripts/content-textures.sh --intent')
    parser.add_argument('--triangles', required=True, help='triangles.json of world-textures --triangles')
    parser.add_argument('--materials', default='data/content/city-materials.csv',
                        help='only textures of this table (the 78 of the city)')
    parser.add_argument('--out', required=True)
    args = parser.parse_args()
    triangles = json.loads(Path(args.triangles).read_text())
    locate = Locator(triangles['waypoints'])
    intent = json.loads(Path(args.intent).read_text())
    items = [(t['texture'], p['center'], p['area_m2']) for t in intent['textures'] for p in t['patches']]
    shell = [(t['texture'], t['center'], t['area_m2']) for t in triangles['candidate_house_triangles']
             if t['texture'].startswith('NW_MISC_')]
    in_shell = {texture for texture, _, _ in shell}
    ground = [(texture, *centre_area(points)) for texture, points
              in zip(triangles['ground_textures'], triangles['ground_triangles'], strict=True)
              if texture.startswith('NW_MISC_') and texture not in in_shell]
    items += shell + ground
    with open(args.materials, newline='') as table_file:
        wanted = {row['texture'] for row in csv.DictReader(table_file)}
    table = shares([item for item in items if item[0] in wanted], locate)
    with open(args.out, 'w', newline='') as out:
        writer = csv.writer(out)
        writer.writerow(['texture', 'source', 'area_m2', *(f'{d}_pct' for d in DISTRICTS)])
        for texture, row in sorted(table.items(), key=lambda kv: -sum(kv[1].values())):
            total = sum(row.values())
            source = 'triangles (city bounds)' if texture.startswith('NW_MISC_') else 'intent (world)'
            writer.writerow([texture, source, f'{total:.1f}',
                             *(f'{100 * row[d] / total:.1f}' if total else '0.0' for d in DISTRICTS)])


if __name__ == '__main__':
    main()
