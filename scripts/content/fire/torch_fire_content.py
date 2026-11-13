#!/usr/bin/env python3
"""Put the wall torch's own fire (the torch at the Red Lantern) into a content folder or into a part of the working set.

    python3 scripts/content/fire/torch_fire_content.py CONTENT_DIR --atlas smallflame01.dds [--smoke albedo.dds]
    python3 scripts/content/fire/torch_fire_content.py --set torch-fire-1 --atlas smallflame01.dds

reports/content/remaster/wall-torch/README.md, items 8 and 9. Both forms take the effects
(particles.d/wall-torch-fire.json) and the layer (worlds/NEWWORLD/vobs.d/wall-torch-fire.json) from
reports/content/remaster/wall-torch/fire/content and the flame atlas of fire_atlas.py (outside git, 2.8 MB) as
OWN_TORCH_FLAME.TGA in materials/ with its line in materials.json.

CONTENT_DIR is a content root (OPENGOTHIC_CONTENT) for a look at the torch: an empty folder, or a copy of the
installed content for a shot in its real surroundings; never the installed content itself. It also gets
OWN_HUT_SMOKE.TGA, the puff of the huts' smoke that the soot reuses (default: the installed content's).

`--set NAME` writes a new staging set, a part of the working content (docs/remaster-pipeline.md, sets): the effects,
the layer and the flame atlas. The soot's OWN_HUT_SMOKE.TGA is not in it: a texture belongs to one part, and
this one is the harbour's (a `motion-*` part), so the working set must hold that part too. The game gets the
part by `building_set.py compose working-N <the current working set> NAME` and, on the user's word,
`building_set.py enable working-N`.
"""
from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
PILOT = ROOT / 'reports/content/remaster/wall-torch/fire/content'
INSTALLED = Path.home() / '.local/share/gothic-ai/content'
STAGING = INSTALLED.parent / 'content-staging'
FLAME = ('OWN_TORCH_FLAME.TGA', 'own-torch-flame/albedo.dds')
SMOKE = ('OWN_HUT_SMOKE.TGA', 'own-hut-smoke/albedo.dds')


def write(content: Path, textures: list[tuple[str, str, Path]]) -> int:
    """The effects and the layer of the torch and `textures` (name, path in materials/, source) into `content`;
    returns the number of materials in its index."""
    for source in sorted(PILOT.rglob('*.json')):
        target = content / source.relative_to(PILOT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    index_path = content / 'materials/materials.json'
    index = json.loads(index_path.read_text()) if index_path.is_file() else {}
    for texture, rel, source in textures:
        target = content / 'materials' / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        if source.resolve() != target.resolve():
            shutil.copyfile(source, target)
        index[texture] = {**index.get(texture, {}), 'albedo': rel}
    index_path.write_text(json.dumps(index, indent=2) + '\n')
    return len(index)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('content', type=Path, nargs='?')
    parser.add_argument('--set', dest='part', help='a new staging set: the torch as a part of the working content')
    parser.add_argument('--atlas', type=Path, required=True)
    parser.add_argument('--smoke', type=Path, default=INSTALLED / 'materials' / SMOKE[1])
    args = parser.parse_args(argv)
    if (args.content is None) == (args.part is None):
        raise SystemExit('give a content folder or --set NAME, one of the two')
    if not args.atlas.is_file():
        raise SystemExit(f'{args.atlas}: missing')
    if args.part is not None:
        content = STAGING / args.part
        if content.exists():
            raise SystemExit(f'{content}: the staging set exists; choose a new name')
        count = write(content, [(*FLAME, args.atlas)])
        print(f'{content}: the torch fire as a part, {count} material; the soot needs {SMOKE[0]} of a motion-* part')
        return 0
    if args.content.resolve() == INSTALLED.resolve():
        raise SystemExit(f'{args.content}: the installed content; give a copy, or make a part with --set')
    if not args.smoke.is_file():
        raise SystemExit(f'{args.smoke}: missing')
    count = write(args.content, [(*FLAME, args.atlas), (*SMOKE, args.smoke)])
    print(f'{args.content}: the torch fire, {count} materials')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
