#!/usr/bin/env python3
"""Compare own albedo contrast/detail with scalar original targets.

Reads only our PNGs: seven pilot-2 prepared/final pairs and selected city-3
albedos from their registries. A zero target leaves its undefined ratio empty.
No game archives or staged files are written.
"""
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

from map_textures import DEFAULT_MAPPING, ROOT
from material_measures import measure_material
from pack_materials import map_path, select_set_records


BASE = Path.home() / '.local/share/gothic-ai'
METRICS = ('log_spread', 'detail_k3', 'detail_k9', 'detail_k27')


def compare_materials(mapping, registry, city3_root, pilot_registry, pilot_root,
                      prepared_root, output):
    with mapping.open(newline='', encoding='utf-8') as stream:
        targets = {row['texture']: row for row in csv.DictReader(stream)}
    records = [json.loads(line) for line in registry.read_text().splitlines() if line.strip()]
    city3 = select_set_records(records, 'city-3', mapping)
    pilots = [json.loads(line) for line in pilot_registry.read_text().splitlines() if line.strip()]
    pilots = [r for r in pilots if r.get('kind') == 'material'
              and r.get('parameters', {}).get('set') == 'pilot-2'
              and r.get('parameters', {}).get('selected') is not False
              and r.get('generation', {}).get('outcome') == 'complete']
    if not pilots or len({r['replaces'][0] for r in pilots}) != len(pilots):
        raise ValueError('pilot-2 registry must have unique completed materials')
    inputs = []
    for record in pilots:
        texture = record['replaces'][0]
        inputs.append(('pilot-2-prepared', texture, prepared_root / (Path(texture).stem + '.png')))
        inputs.append(('pilot-2', texture, map_path(pilot_root, record['files']['albedo'])))
    inputs.extend(('city-3', r['replaces'][0], map_path(city3_root, r['files']['albedo'])) for r in city3)
    rows = []
    for variant, texture, path in inputs:
        measured = measure_material(path)
        row = dict(variant=variant, texture=texture)
        for metric in METRICS:
            target = float(targets[texture]['target_' + metric])
            value = measured[metric]
            row.update({f'target_{metric}': target, metric: value,
                        f'ratio_{metric}': value / target if target else ''})
        rows.append(row)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open('w', newline='', encoding='utf-8') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]), lineterminator='\n')
        writer.writeheader()
        writer.writerows(rows)
    return len(rows)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--mapping', type=Path, default=DEFAULT_MAPPING)
    parser.add_argument('--registry', type=Path, default=ROOT / 'data/content/registry.jsonl')
    parser.add_argument('--city3-root', type=Path, default=BASE / 'content-staging/city-3/sources')
    parser.add_argument('--pilot-registry', type=Path, default=ROOT / 'data/content/quality-pilot-2.jsonl')
    parser.add_argument('--pilot-root', type=Path, default=BASE / 'content-staging/quality-pilot-2-sources')
    parser.add_argument('--prepared-root', type=Path, default=BASE / 'quality-pilot-2/prepared')
    args = parser.parse_args(argv)
    count = compare_materials(args.mapping, args.registry, args.city3_root, args.pilot_registry,
                              args.pilot_root, args.prepared_root, args.output)
    print(f'measured own albedos={count}; output={args.output}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
