"""A vanilla Gothic II Gold folder for Gothic AI beside an installation with mods (Prepare-GothicAI-Game.cmd).

The base import accepts only the original archives. When Data also holds others (Union, AB_* ...), a profile
folder gets hard links to the original archives and junctions (symlinks elsewhere) to _work, system and Miles;
nothing of the game is copied or changed, and the modded installation keeps working as before. A clean
installation is used as it is. A matching profile is reused; one that no longer matches is left untouched.

    game_profile.py <Gothic II folder> [--set-config]    prints {"game_dir": ...}; --set-config also sets
                                                          game.dir through the launcher
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

REQUIRED = ('Anims.vdf', 'Anims_Addon.vdf', 'Meshes.vdf', 'Meshes_Addon.vdf',
            'Sounds.vdf', 'Sounds_Addon.vdf', 'Speech1.vdf', 'Speech2.vdf',
            'Speech_Addon.vdf', 'Textures.vdf', 'Textures_Addon.vdf',
            'Worlds.vdf', 'Worlds_Addon.vdf')
OPTIONAL = ('Fonts_Russian.vdf', 'Textures_Russian.vdf')
VDF_FLAGS_AT = 292  # the VDF header's flags; 0x50 for the uncompressed original archives
VDF_ORIGINAL = 0x50


def prepare(source: Path, profiles: Path | None = None) -> Path:
    """The folder for game.dir: `source` itself when it has no other archives, else its profile."""
    source = source.resolve(strict=True)
    data = source / 'Data'
    archives = {p.name.lower(): p for p in data.iterdir() if p.is_file() and p.suffix.lower() == '.vdf'}
    missing = [name for name in REQUIRED if name.lower() not in archives]
    if missing:
        raise ValueError('Missing original Gothic II Gold archives: ' + ', '.join(missing))
    allowed = {name.lower() for name in REQUIRED + OPTIONAL}
    selected = [archives[name.lower()] for name in REQUIRED + OPTIONAL if name.lower() in archives]
    for archive in selected:
        with archive.open('rb') as stream:
            stream.seek(VDF_FLAGS_AT)
            flags = int.from_bytes(stream.read(4), 'little')
        if flags != VDF_ORIGINAL:
            raise ValueError('Original base archive is compressed or invalid: ' + archive.name)
    if not (set(archives) - allowed):
        return source
    if profiles is None:
        local = Path(os.environ.get('LOCALAPPDATA', str(source.parent)))
        profiles = local / 'GothicAI' / 'profiles'
        if os.name == 'nt' and source.drive.lower() != profiles.drive.lower():
            profiles = source.parent / '.gothic-ai-profiles'  # hard links stay on one volume
    profiles = profiles.resolve()  # the physical folder, also where a packaged app redirects AppData
    identity = json.dumps(
        [str(source), [(p.name, p.stat().st_size, p.stat().st_mtime_ns) for p in selected]], sort_keys=True
    )
    target = profiles / ('gothic2-gold-' + hashlib.sha256(identity.encode()).hexdigest()[:16])
    if target.exists():
        if all(os.path.samefile(target / 'Data' / p.name, p) for p in selected):
            return target
        raise ValueError('Existing profile is inconsistent and was preserved: ' + str(target))
    profiles.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix='.stage-profile-', dir=profiles))
    (stage / 'Data').mkdir()
    for archive in selected:
        os.link(archive, stage / 'Data' / archive.name)
    for name in ('_work', 'system', 'Miles'):
        original = source / name
        if not original.is_dir():
            continue
        if os.name == 'nt':
            import _winapi

            _winapi.CreateJunction(str(original), str(stage / name))
        else:
            (stage / name).symlink_to(original, target_is_directory=True)
    (stage / 'gothic-ai-profile.json').write_text(
        json.dumps({'source': str(source), 'archives': [p.name for p in selected]}, indent=2), encoding='utf-8'
    )
    stage.rename(target)
    return target


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game_dir', type=Path)
    parser.add_argument('--set-config', action='store_true', help='set game.dir to the result')
    args = parser.parse_args()
    try:
        target = prepare(args.game_dir)
    except (OSError, ValueError) as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(1)
    if args.set_config:
        launcher = [sys.executable, '-c', 'from gothic_launcher.cli import main; main()']
        subprocess.run([*launcher, 'config', 'set', 'game.dir', str(target)], check=True)
    print(json.dumps({'game_dir': str(target), 'original_files_unchanged': True}, ensure_ascii=False))


if __name__ == '__main__':
    main()
