#!/usr/bin/env python3
"""Census of the fire creatures carry on their own: particle effects their animations start, and their scripts' effects.

    uv run --no-project --with zenkit python scripts/content/fire/creature_fire_census.py GOTHIC_DIR

reports/content/remaster/magic-fire. Reads the model scripts of Anims*.vdf (`*eventPFX` of every animation, the
addon's archive wins, as in the game), PARTICLEFX.DAT and VISUALFX.DAT, and runs every monster instance (prototype `MST_*`) of
GOTHIC.DAT to catch the model it sets (`Mdl_SetVisual`, overlays) and its own effect (`C_NPC.effect`). Prints, per model, every
fire particle effect its animations start, on which bone and in which animations, and which instances use the model;
how many of these creatures the world startups and the chapter entries insert (`Wld_InsertNpc`), and how many objects
of these models stand in each world.
"""
from __future__ import annotations

import argparse
import collections
import importlib.util
from pathlib import Path

import zenkit
from zenkit import DaedalusDataType, DaedalusInstance, DaedalusInstanceType, DaedalusVm

spec = importlib.util.spec_from_file_location('census', Path(__file__).with_name('magic_fire_census.py'))
census = importlib.util.module_from_spec(spec)
spec.loader.exec_module(census)

WORDS = ('FIRE', 'FLAME', 'BURN', 'LAVA', 'BREATH', 'DRAGON', 'DEMON', 'PYRO', 'MAGMA', 'EMBER')


def model_scripts(gothic: Path) -> dict[str, zenkit.ModelScript]:
    vfs = zenkit.Vfs()
    for archive in sorted((gothic / 'Data').glob('Anims*.vdf')):
        vfs.mount_disk(str(archive))
    out = {}

    def walk(node, prefix=''):
        for child in node.children:
            if child.is_dir():
                walk(child)
            elif child.name.upper().endswith(('.MSB', '.MDS')):
                key = child.name.upper().rsplit('.', 1)[0]
                if key in out and child.name.upper().endswith('.MDS'):
                    continue  # the compiled script wins, as in the game
                try:
                    out[key] = zenkit.ModelScript.load(child)
                except Exception as error:  # noqa: BLE001 - report and go on, one broken script must not stop the census
                    print(f'warning: {child.name}: {error}')
    walk(vfs.root)
    return out


def npc_visuals(gothic: Path) -> dict[str, dict]:
    vm = DaedalusVm.load(str(gothic / '_work/Data/Scripts/_compiled/GOTHIC.DAT'))
    current = {'name': None}
    seen: dict[str, dict] = collections.defaultdict(lambda: {'visual': set(), 'overlays': set(), 'body': set()})

    def set_visual(npc, visual):
        seen[current['name']]['visual'].add(visual.upper())

    def overlay(npc, mds):
        seen[current['name']]['overlays'].add(mds.upper())

    def body(npc, mesh, tex, skin, head, head_tex, teeth, armor):
        seen[current['name']]['body'].add(mesh.upper())

    vm.register_external_default(lambda symbol: None)
    vm.register_external('Mdl_SetVisual', set_visual, DaedalusInstance, str)
    vm.register_external('Mdl_ApplyOverlayMds', overlay, DaedalusInstance, str)
    vm.register_external('Mdl_SetVisualBody', body, DaedalusInstance, str, int, int, str, int, int, int)
    out = {}
    for symbol in vm.symbols:
        if symbol.type != DaedalusDataType.INSTANCE or symbol.name.startswith(('$', '\xff')):
            continue
        # Monsters only: their prototypes are MST_*. Running human instances calls externals whose return values
        # the default handler cannot fake, and humans carry no fire of their own.
        prototype = vm.get_symbol_by_index(symbol.parent) if symbol.parent >= 0 else None
        if prototype is None or prototype.type != DaedalusDataType.PROTOTYPE or not prototype.name.startswith('MST_'):
            continue
        current['name'] = symbol.name
        try:
            inst = vm.init_instance(symbol, DaedalusInstanceType.NPC)
        except Exception as error:  # noqa: BLE001
            print(f'warning: {symbol.name}: {error}')
            continue
        data = seen[symbol.name]
        out[symbol.name] = {'visual': sorted(data['visual']), 'overlays': sorted(data['overlays']),
                            'body': sorted(data['body']), 'effect': str(getattr(inst, 'effect', '') or '').upper()}
    return out


def spawns(gothic: Path) -> collections.Counter:
    """(world, chapter, instance) -> count of `Wld_InsertNpc` in the world startups and the chapter entries.

    Every other external returns a default value, so a branch on one is followed as the default takes it: a census,
    not the game's exact roster."""
    vm = DaedalusVm.load(str(gothic / '_work/Data/Scripts/_compiled/GOTHIC.DAT'))
    count, where = collections.Counter(), {'key': None}

    def insert(instance, point):
        count[(*where['key'], vm.get_symbol_by_index(instance).name)] += 1

    vm.register_external_default(lambda symbol: None)
    vm.register_external('Wld_InsertNpc', insert, int, str)
    for world in ('NEWWORLD', 'OLDWORLD', 'ADDONWORLD'):
        functions = [(f'STARTUP_{world}', 'старт')] + [(f'B_ENTER_{world}_KAPITEL_{n}', f'глава {n}') for n in range(1, 7)]
        for function, chapter in functions:
            where['key'] = (world, chapter)
            try:
                vm.call(function)
            except Exception as error:  # noqa: BLE001 - a default value can break a call; report and go on
                print(f'warning: {function}: {error}')
    return count


def world_objects(gothic: Path, models: set[str]) -> dict[str, collections.Counter]:
    vfs = zenkit.Vfs()
    for archive in sorted((gothic / 'Data').glob('Worlds*.vdf')):
        vfs.mount_disk(str(archive))
    out = {}
    for world in ('NEWWORLD', 'OLDWORLD', 'ADDONWORLD'):
        count = collections.Counter()

        def walk(vobs):
            for vob in vobs:
                visual = vob.visual.name.upper().rsplit('.', 1)[0] if vob.visual else ''
                if visual in models:
                    count[visual] += 1
                walk(vob.children)
        walk(zenkit.World.load(vfs.find(world + '.ZEN')).root_objects)
        out[world] = count
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('gothic_dir', type=Path)
    args = parser.parse_args()
    compiled = args.gothic_dir / '_work/Data/Scripts/_compiled'
    pfx = census.instances(compiled / 'PARTICLEFX.DAT', 'C_PARTICLEFX', DaedalusInstanceType.PARTICLE_EFFECT)
    vfx = census.instances(compiled / 'VISUALFX.DAT', 'CFX_BASE', DaedalusInstanceType.EFFECT_BASE)
    picture = {name.upper(): str(fields.get('vis_name_s', '')).upper() for name, fields in pfx.items()}

    def fiery(name: str) -> bool:
        name = name.upper()
        return picture.get(name, '') in census.FIRE or any(word in name for word in WORDS)

    scripts = model_scripts(args.gothic_dir)
    npcs = npc_visuals(args.gothic_dir)
    users = collections.defaultdict(list)
    for instance, data in npcs.items():
        for model in [*data['visual'], *data['overlays']]:
            users[model.rsplit('.', 1)[0]].append(instance)

    fire_models = set()
    print('## Огонь в анимациях\n')
    print('| Модель | Эффект частиц | Картинка | Кость | Анимации | Экземпляры |')
    print('|---|---|---|---|---|---|')
    for model, script in sorted(scripts.items()):
        found = collections.defaultdict(lambda: {'bones': set(), 'anims': []})
        for anim in script.animations:
            for event in anim.particle_effects:
                if fiery(event.name):
                    entry = found[event.name.upper()]
                    entry['bones'].add(event.position or '—')
                    entry['anims'].append(anim.name.upper())
        if found:
            fire_models.add(model)
        for name, entry in sorted(found.items()):
            anims = sorted(set(entry['anims']))
            shown = ', '.join(anims[:6]) + (f' … ({len(anims)})' if len(anims) > 6 else '')
            who = sorted(users.get(model, []))
            who_text = ', '.join(who[:4]) + (f' … ({len(who)})' if len(who) > 4 else '') if who else '—'
            print(f"| `{model}` | `{name}` | `{picture.get(name, '?')}` | {', '.join(sorted(entry['bones']))} | "
                  f"{shown} | {who_text} |")

    print('\n## Свой эффект существа (`C_NPC.effect`)\n')
    print('| Экземпляр | Модель | Эффект | Частицы эффекта |')
    print('|---|---|---|---|')
    for instance, data in sorted(npcs.items()):
        if not data['effect']:
            continue
        fx = vfx.get(data['effect'], {})
        print(f"| `{instance}` | {', '.join(data['visual']) or '—'} | `{data['effect']}` | "
              f"`{fx.get('vis_name_s', '?')}` |")

    fiery_npcs = {instance for instance, data in npcs.items() if data['effect']
                  or any(model.rsplit('.', 1)[0] in fire_models and model.rsplit('.', 1)[0] != 'HUMANS'
                         for model in [*data['visual'], *data['overlays']])}
    print('\n## Сколько их вставляют скрипты (`Wld_InsertNpc`)\n')
    print('| Мир | Когда | Экземпляр | Сколько |')
    print('|---|---|---|---:|')
    for (world, chapter, instance), number in sorted(spawns(args.gothic_dir).items()):
        if instance.upper() in fiery_npcs:
            print(f'| {world} | {chapter} | `{instance}` | {number} |')

    print('\n## Объекты этих моделей в мирах\n')
    objects = world_objects(args.gothic_dir, fire_models)
    print('| Модель | ' + ' | '.join(objects) + ' |')
    print('|---|' + '---:|' * len(objects))
    for model in sorted(set().union(*objects.values())):
        print(f'| `{model}` | ' + ' | '.join(str(objects[world][model]) for world in objects) + ' |')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
