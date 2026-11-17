#!/usr/bin/env python3
"""Census of the fire in spells and script effects: which VisualFx instances show which fire particle effects.

    uv run --no-project --with zenkit python scripts/content/fire/magic_fire_census.py GOTHIC_DIR [--out census.md]

reports/content/remaster/magic-fire. Reads VISUALFX.DAT and PARTICLEFX.DAT of the game: every particle effect whose
picture is one of the game's fire textures, the VisualFx instances and their emitter keys that show it, and how many
times GOTHIC.DAT names the instance (0 for a spell: the engine builds its name from the spell's own).
"""
from __future__ import annotations

import argparse
import collections
from pathlib import Path
import re

from zenkit import DaedalusDataType, DaedalusInstanceType, DaedalusVm

FIRE = {'FIREFLARE.TGA', 'HUMANBURN.TGA', 'MFX_FIREBALL.TGA', 'FIRETAIL.TGA', 'MFX_FIRERAIN_RAIN.TGA',
        'MFX_FIRERAIN_RAIN_BIG.TGA', 'PFXFEXP2_A0.TGA', 'FIREBALL_A0.TGA', 'FLAM_A0.TGA', 'FIRE_COMPLETE_A1.TGA',
        'FIRE_COMPLETE_A0.TGA', 'FIRE2_A0.TGA', 'FIRETEXTURE_A0.TGA', 'MFX_FIREBALL2.TGA', 'FLAME_A0.TGA',
        'SHADOWBEAST_FIRE.TGA', 'SHADOWBEAST_SMOKE.TGA', 'FIRESMOKE.TGA', 'SMK_FIRE.TGA'}
LINKS = ('em_fx_create_s', 'em_fx_coll_stat_s', 'em_fx_coll_dyn_s', 'em_fx_coll_dyn_perc_s', 'em_fx_invest_origin_s',
         'em_fx_invest_target_s')


def instances(path: Path, cls: str, kind) -> dict:
    vm = DaedalusVm.load(str(path))
    out = {}
    for symbol in vm.symbols:
        if symbol.type != DaedalusDataType.INSTANCE or symbol.name.startswith(('$', '\xff')):
            continue
        parent, depth = symbol, 0
        while parent is not None and parent.type != DaedalusDataType.CLASS and depth < 5:
            parent = vm.get_symbol_by_index(parent.parent) if parent.parent >= 0 else None
            depth += 1
        if parent is None or parent.type != DaedalusDataType.CLASS or parent.name != cls:
            continue
        inst = vm.init_instance(symbol, kind)
        fields = {}
        for name in dir(inst):
            if name.startswith('_'):
                continue
            value = getattr(inst, name)
            if isinstance(value, (int, float, str)):
                fields[name] = value
        out[symbol.name] = fields
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('gothic_dir')
    parser.add_argument('--out')
    args = parser.parse_args()
    compiled = Path(args.gothic_dir) / '_work/Data/Scripts/_compiled'
    vfx = instances(compiled / 'VISUALFX.DAT', 'CFX_BASE', DaedalusInstanceType.EFFECT_BASE)
    keys = instances(compiled / 'VISUALFX.DAT', 'C_PARTICLEFXEMITKEY', DaedalusInstanceType.PARTICLE_EFFECT_EMIT_KEY)
    pfx = instances(compiled / 'PARTICLEFX.DAT', 'C_PARTICLEFX', DaedalusInstanceType.PARTICLE_EFFECT)
    game = (compiled / 'GOTHIC.DAT').read_bytes().upper()
    canon = lambda text: text.upper().removesuffix('.PFX')
    fire = {name for name, fields in pfx.items() if fields.get('vis_name_s', '').upper() in FIRE}

    shows, children = collections.defaultdict(set), collections.defaultdict(set)
    for name, fields in vfx.items():
        if canon(fields.get('vis_name_s', '')) in pfx:
            shows[name].add(canon(fields['vis_name_s']))
        children[name] |= {fields[link].upper() for link in LINKS if fields.get(link, '').upper() in vfx}
    for name, fields in keys.items():
        match = re.match(r'(.+)_KEY_[A-Z0-9_]+$', name)
        if not match or match.group(1) not in vfx:
            continue
        if canon(fields.get('vis_name_s', '')) in pfx:
            shows[match.group(1)].add(canon(fields['vis_name_s']))
        if fields.get('em_create_fx_id', '').upper() in vfx:
            children[match.group(1)].add(fields['em_create_fx_id'].upper())

    burning = sorted(name for name in vfx if shows[name] & fire)
    used = set().union(*(shows[name] for name in burning)) & fire
    lines = ['| VisualFx | Траектория | Кость | Свет | Огненные PFX | Дочерние эффекты | В GOTHIC.DAT |', '|---|---|---|---|---|---|---|']
    for name in burning:
        fields = vfx[name]
        lines.append(f"| `{name}` | {fields['em_trj_mode_s']} | {fields['em_trj_origin_node']} | {fields['light_preset_name']} | "
                     f"{', '.join(f'`{p}`' for p in sorted(shows[name] & fire))} | "
                     f"{', '.join(f'`{c}`' for c in sorted(children[name]))} | {game.count(name.encode())} |")
    lines += ['', f'Эффектов VisualFx с огнём: {len(burning)}; огненных PFX в них: {len(used)} из {len(fire)} с огненной картинкой.',
              '', 'Огненные PFX, которых не показывает ни один VisualFx (анимации, предметы мира, меню, остатки): '
              + ', '.join(f'`{name}`' for name in sorted(fire - used)) + '.']
    text = '\n'.join(lines) + '\n'
    if args.out:
        Path(args.out).write_text(text)
    else:
        print(text)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
