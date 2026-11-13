#!/usr/bin/env python3
"""Put the wall torch's own fire (the pilot at the Red Lantern) into a content folder.

    python3 scripts/content/fire/torch_fire_content.py CONTENT_DIR --atlas smallflame01.dds [--smoke albedo.dds]

reports/content/remaster/wall-torch/README.md, item 8. CONTENT_DIR is a content root (OPENGOTHIC_CONTENT): an
empty folder for a quick look at the torch alone, or a copy of the installed content for a shot in its real
surroundings; never the installed content itself while the pilot is not accepted. It gets the effects
(particles.d/wall-torch-fire.json) and the layer (worlds/NEWWORLD/vobs.d/wall-torch-fire.json) from
reports/content/remaster/wall-torch/fire/content, and two textures in materials/ with their lines in
materials.json: OWN_TORCH_FLAME.TGA, the flame atlas of fire_atlas.py (outside git, 2.8 MB), and
OWN_HUT_SMOKE.TGA, the puff of the huts' smoke that the soot reuses (default: the installed content's).
"""
from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
PILOT = ROOT / 'reports/content/remaster/wall-torch/fire/content'
INSTALLED = Path.home() / '.local/share/gothic-ai/content'


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('content', type=Path)
    parser.add_argument('--atlas', type=Path, required=True)
    parser.add_argument('--smoke', type=Path, default=INSTALLED / 'materials/own-hut-smoke/albedo.dds')
    args = parser.parse_args()
    if args.content.resolve() == INSTALLED.resolve():
        raise SystemExit(f'{args.content}: the installed content; give a copy')
    for source in (args.atlas, args.smoke):
        if not source.is_file():
            raise SystemExit(f'{source}: missing')
    for source in sorted(PILOT.rglob('*.json')):
        target = args.content / source.relative_to(PILOT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    index_path = args.content / 'materials/materials.json'
    index = json.loads(index_path.read_text()) if index_path.is_file() else {}
    for texture, rel, source in (('OWN_TORCH_FLAME.TGA', 'own-torch-flame/albedo.dds', args.atlas),
                                 ('OWN_HUT_SMOKE.TGA', 'own-hut-smoke/albedo.dds', args.smoke)):
        target = args.content / 'materials' / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        if source.resolve() != target.resolve():
            shutil.copyfile(source, target)
        index[texture] = {**index.get(texture, {}), 'albedo': rel}
    index_path.write_text(json.dumps(index, indent=2) + '\n')
    print(f'{args.content}: the torch fire pilot, {len(index)} materials')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
