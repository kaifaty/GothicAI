#!/usr/bin/env python3
"""The part of the working content that holds the smoke atlases every fire shares.

    python3 scripts/content/fire/smoke_set.py --set smoke-1 --wispy wispy03b.dds --dense wispy01.dds

reports/content/remaster/smoke/README.md. A texture belongs to one part, and the campfires, the wall torches and
the harbour's huts play the same puffs, so the atlases of smoke_atlas.py live in a part of their own:
OWN_SMOKE_WISPY.TGA (thin wisps: a campfire, a torch's soot) and OWN_SMOKE_DENSE.TGA (a hearth's smoke from a roof).
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import sys

STAGING = Path.home() / '.local/share/gothic-ai/content-staging'
ATLASES = {'wispy': ('OWN_SMOKE_WISPY.TGA', 'own-smoke-wispy/albedo.dds'),
           'dense': ('OWN_SMOKE_DENSE.TGA', 'own-smoke-dense/albedo.dds')}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--set', required=True)
    for kind in ATLASES:
        parser.add_argument(f'--{kind}', type=Path, required=True)
    args = parser.parse_args(argv)
    output = STAGING / args.set
    if output.exists():
        sys.exit(f'{output}: the staging set exists; choose a new name')
    sources = {kind: getattr(args, kind) for kind in ATLASES}
    for source in sources.values():
        if not source.is_file():
            sys.exit(f'{source}: missing')
    index = {}
    for kind, (texture, rel) in ATLASES.items():
        target = output / 'materials' / rel
        target.parent.mkdir(parents=True)
        shutil.copyfile(sources[kind], target)
        index[texture] = {'albedo': rel}
    (output / 'materials/materials.json').write_text(json.dumps(index, indent=2) + '\n')
    print(f'{output}: {", ".join(index)}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
