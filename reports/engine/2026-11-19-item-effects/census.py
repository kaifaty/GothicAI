#!/usr/bin/env python3
"""Census of item effects: which items carry an effect (C_ITEM.effect), what it is, and how many lie in each world.

    uv run --no-project --with zenkit python reports/engine/2026-11-19-item-effects/census.py GOTHIC_DIR

reports/engine/2026-11-19-item-effects. Reads GOTHIC.DAT, VISUALFX.DAT and PARTICLEFX.DAT of the game and the worlds
of Worlds*.vdf (the addon's archive wins, as in the game).
"""
from __future__ import annotations

import argparse
import collections
import importlib.util
from pathlib import Path

import zenkit
from zenkit import DaedalusInstanceType

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('census', ROOT / 'scripts/content/fire/magic_fire_census.py')
census = importlib.util.module_from_spec(spec)
spec.loader.exec_module(census)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('gothic_dir', type=Path)
    args = parser.parse_args()
    compiled = args.gothic_dir / '_work/Data/Scripts/_compiled'
    items = census.instances(compiled / 'GOTHIC.DAT', 'C_ITEM', DaedalusInstanceType.ITEM)
    vfx = census.instances(compiled / 'VISUALFX.DAT', 'CFX_BASE', DaedalusInstanceType.EFFECT_BASE)
    pfx = census.instances(compiled / 'PARTICLEFX.DAT', 'C_PARTICLEFX', DaedalusInstanceType.PARTICLE_EFFECT)
    effect = {name.upper(): str(fields['effect']).upper() for name, fields in items.items() if fields.get('effect')}
    by_effect = collections.defaultdict(list)
    for name, fx in effect.items():
        by_effect[fx].append(name)

    vfs = zenkit.Vfs()
    for archive in sorted((args.gothic_dir / 'Data').glob('Worlds*.vdf')):
        vfs.mount_disk(str(archive))
    lying = {}
    for world in ('NEWWORLD.ZEN', 'OLDWORLD.ZEN', 'ADDONWORLD.ZEN'):
        node = vfs.find(world)
        if node is None:
            continue
        count = collections.Counter()

        def walk(vobs):
            for vob in vobs:
                fx = effect.get(str(getattr(vob, 'instance', '') or '').upper())
                if fx:
                    count[fx] += 1
                walk(vob.children)
        walk(zenkit.World.load(node).root_objects)
        lying[world] = count

    worlds = list(lying)
    print('| Эффект | Предметов | Частицы | Сетка | Свет | ' + ' | '.join(w.split('.')[0] for w in worlds) + ' | Пример |')
    print('|---|---:|---|---|---|' + '---:|' * len(worlds) + '---|')
    for fx, names in sorted(by_effect.items(), key=lambda kv: -len(kv[1])):
        v = vfx.get(fx, {})
        p = pfx.get(str(v.get('vis_name_s', '')).upper(), {})
        row = [f'`{fx}`', str(len(names)), f"`{v.get('vis_name_s', '')}` {p.get('pps_value', '')}/с",
               str(p.get('shp_type_s', '')), str(v.get('light_preset_name', '')) or '—']
        row += [str(lying[w][fx]) for w in worlds]
        row.append(f'`{sorted(names)[0]}`')
        print('| ' + ' | '.join(row) + ' |')
    print()
    for world in worlds:
        print(f'{world}: {sum(lying[world].values())} предметов с эффектом в мире')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
