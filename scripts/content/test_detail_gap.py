"""The gap report follows retained recipes, never a previous generation attempt."""
import csv
import json

import detail_gap
from pilot import antitile_mode


def test_prepared_final_and_selected_city_albedos_use_original_targets(tmp_path, monkeypatch):
    mapping = tmp_path / 'mapping.csv'
    targets = dict(zip(detail_gap.METRICS, (2, 4, 8, 16)))
    rows = [dict(texture=texture, decision='include', **{'class': 'flagstone'}, scale=1,
                 target_luminance=.15, **{'target_' + key: value for key, value in targets.items()})
            for texture in ('PILOT.TGA', 'CITY.TGA')]
    with mapping.open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    records = [dict(id=f'city-3-{index}', kind='material', status='draft', pipeline='imagegen-filter',
                    replaces=[row['texture']], files={'albedo': f'city-{index}.png'},
                    generation={'outcome': 'complete', 'attempts': [{'files': {'albedo': 'old.png'}}]},
                    parameters=dict(set='city-3', scale=1, material_class='flagstone',
                                    target_luminance=.15, antitile=antitile_mode(row)))
               for index, row in enumerate(rows)]
    records.append(dict(records[0], id='city-3-old', files={'albedo': 'old.png'},
                        parameters=dict(records[0]['parameters'], selected=False)))
    registry = tmp_path / 'registry.jsonl'
    registry.write_text('\n'.join(json.dumps(record) for record in records))
    pilot_registry = tmp_path / 'pilot.jsonl'
    pilot_registry.write_text(json.dumps(dict(kind='material', replaces=['PILOT.TGA'],
        parameters={'set': 'pilot-2'}, generation={'outcome': 'complete'},
        files={'albedo': 'final.png'})))
    city_root, pilot_root, prepared_root = (tmp_path / name for name in ('city', 'pilot', 'prepared'))
    factors = {prepared_root / 'PILOT.png': 1, pilot_root / 'final.png': .5,
               city_root / 'city-0.png': 2, city_root / 'city-1.png': 3}
    seen = []

    def measure(path):
        seen.append(path)
        return {metric: value * factors[path] for metric, value in targets.items()}

    monkeypatch.setattr(detail_gap, 'measure_material', measure)
    output = tmp_path / 'output' / 'texture-detail.csv'
    assert detail_gap.compare_materials(mapping, registry, city_root, pilot_registry,
                                        pilot_root, prepared_root, output) == 4
    assert set(seen) == set(factors)
    with output.open(newline='') as stream:
        measured = list(csv.DictReader(stream))
    assert [(row['variant'], row['texture']) for row in measured] == [
        ('pilot-2-prepared', 'PILOT.TGA'), ('pilot-2', 'PILOT.TGA'),
        ('city-3', 'PILOT.TGA'), ('city-3', 'CITY.TGA')]
    for row, factor in zip(measured, (1, .5, 2, 3)):
        for metric, target in targets.items():
            assert float(row[f'target_{metric}']) == target
            assert float(row[metric]) == target * factor
            assert float(row[f'ratio_{metric}']) == factor
