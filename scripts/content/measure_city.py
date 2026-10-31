#!/usr/bin/env python3
"""Write numeric physical-detail comparisons for a retained material set.

Only our registered albedo PNGs are read. Original measurements come from the
scalar mapping; this tool cannot read game archives or change a staged set.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path

from map_textures import DEFAULT_MAPPING, ROOT, _read_csv
from material_measures import measure_material
from material_scale import match_detail_scale, target_detail_plan


def measure_set(mapping, registry, content_root, output, set_name='city-3'):
    rows = _read_csv(mapping, mapping=True)
    records = [json.loads(line) for line in registry.read_text().splitlines() if line.strip()]
    selected = {r['replaces'][0]: r for r in records if r.get('parameters', {}).get('set') == set_name}
    output.mkdir(parents=True, exist_ok=True)
    plans, comparisons = [], []
    for row in rows:
        plan = target_detail_plan(row)
        plans.append(dict(texture=row['texture'], decision=row['decision'], **plan))
        record = selected.get(row['texture'])
        if record is None:
            continue
        path = content_root / record['files']['albedo']
        measured = measure_material(path)
        match = match_detail_scale(row, measured)
        current_scale = record['parameters']['scale']
        result = dict(texture=row['texture'], albedo_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                      current_scale=current_scale, proposed_scale=match['scale'], status=match['status'],
                      target_direction=row['target_direction'], own_direction=measured['direction'],
                      own_direction_confidence=measured['direction_confidence'])
        for axis in ('u', 'v'):
            target_period = float(row[f'target_period_{axis}_uv'])
            own_period = measured[f'period_{axis}_uv']
            uv = float(row['uv_m_per_unit_median'])
            result.update({f'target_period_{axis}_uv': target_period,
                           f'own_period_{axis}_uv': own_period,
                           f'target_detail_{axis}_m': target_period * uv,
                           f'current_detail_{axis}_m': own_period * uv / current_scale,
                           f'proposed_error_{axis}': match['relative_errors'].get(axis, ''),
                           f'own_details_{axis}_per_tile': 1 / own_period if own_period else 0.})
        for index, channel in enumerate(('l', 'a', 'b')):
            result[f'target_oklab_{channel}'] = row[f'target_oklab_{channel}']
            result[f'own_oklab_{channel}'] = measured['oklab'][index]
        comparisons.append(result)
    if len(comparisons) != len(selected):
        raise ValueError('registry and mapping texture sets differ')
    for name, table in (('scale-plan.csv', plans), ('city3-measures.csv', comparisons)):
        if not table:
            raise ValueError('empty material table')
        with (output / name).open('w', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=list(table[0]), lineterminator='\n')
            writer.writeheader()
            writer.writerows(table)
    return len(plans), len(comparisons)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mapping', type=Path, default=DEFAULT_MAPPING)
    parser.add_argument('--registry', type=Path, default=ROOT / 'data/content/registry.jsonl')
    parser.add_argument('--content-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(argv)
    counts = measure_set(args.mapping, args.registry, args.content_root, args.output)
    print(f'target plans={counts[0]}, measured own albedos={counts[1]}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
