#!/usr/bin/env python3
"""An original Gothic texture at its own size, and a 4x view of it, for Claude to look at.

The previews in texture-intent are cut to 256 px on the long side: the carved board
NW_CITY_WOOD_ORNAMENT_01 (128x512) showed at half its size, the floor NW_CITY_WOOD_01 (1024x256) at a
quarter, and the board's figure-eights were read as two rings. This decodes the texture's top level from
the game's archives; --upscale adds a 4x view by Real-ESRGAN on the GPU server (the user's suggestion,
2026-10-22).

Both only show the original's shapes to Claude. They never go into git, to a generator or into our maps.
The upscaler sharpens the lines but invents the wood around them: measure colour and contrast on the
native one. The output folder must be outside the repository.

    original_view.py NW_CITY_WOOD_ORNAMENT_01 [NAME ...] --out DIR [--upscale]

env: GOTHIC_DIR      game data (default: /Volumes/DevSSD/Dev/games/Gothic II)
     GOTHIC_GPU_SSH  ssh destination of the GPU server (default: dev-ubuntu); it needs
                     ~/gothic-ai/upscale/bin/realesrgan-ncnn-vulkan, as scripts/hd-textures.sh
"""
from __future__ import annotations

import argparse
import io
import os
import struct
import subprocess
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
GAME = '/Volumes/DevSSD/Dev/games/Gothic II'
REMOTE = 'gothic-ai/upscale'
FOURCC = {10: b'DXT1', 12: b'DXT3', 14: b'DXT5'}


def texture_name(name: str) -> str:
    """NW_CITY_WOOD_01, nw_city_wood_01.tga -> NW_CITY_WOOD_01-C.TEX, the compiled entry."""
    stem = name.upper().removesuffix('.TGA').removesuffix('-C.TEX')
    return stem + '-C.TEX'


def archives(game: Path) -> list[Path]:
    """Texture archives, Textures.vdf first: a later one replaces what an earlier one holds."""
    found = [p for p in (game / 'Data').iterdir() if p.name.lower().startswith('textures') and p.suffix.lower() == '.vdf']
    return sorted(found, key=lambda p: (p.name.lower() != 'textures.vdf', p.name.lower()))


def read_entries(vdf: Path, wanted: set[str]) -> dict[str, bytes]:
    out = {}
    with vdf.open('rb') as f:
        f.seek(256 + 16)
        count, _files, _time, _size, root, _entry = struct.unpack('<6I', f.read(24))
        f.seek(root)
        entries = [struct.unpack('<64s4I', f.read(80)) for _ in range(count)]
        for name, offset, size, kind, _attr in entries:
            name = name.decode('latin1').strip().upper()
            if name in wanted and not kind & 0x80000000:
                f.seek(offset)
                out[name] = f.read(size)
    return out


def decode(ztex: bytes) -> Image.Image:
    """The top mip level of a ZTEX. Its levels are stored smallest first, so the top one is the last."""
    sig, _version, fmt, width, height = struct.unpack('<4s4I', ztex[:20])
    if sig != b'ZTEX':
        raise ValueError('not a ZTEX texture')
    if fmt == 0:                                                      # B8G8R8A8
        return Image.frombytes('RGBA', (width, height), ztex[len(ztex) - width * height * 4:], 'raw', 'BGRA')
    if fmt not in FOURCC:
        raise ValueError(f'ZTEX format {fmt} is not decoded here')
    block = 8 if fmt == 10 else 16
    top = max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * block
    header = (b'DDS ' + struct.pack('<7I', 124, 0x81007, height, width, top, 0, 1) + bytes(44)
              + struct.pack('<2I4s5I', 32, 4, FOURCC[fmt], 0, 0, 0, 0, 0) + struct.pack('<5I', 0x1000, 0, 0, 0, 0))
    return Image.open(io.BytesIO(header + ztex[len(ztex) - top:])).convert('RGBA')


def native(names: list[str], game: Path, out: Path) -> list[Path]:
    wanted = {texture_name(n) for n in names}
    found = {}
    for vdf in archives(game):
        found.update(read_entries(vdf, wanted))
    missing = sorted(wanted - set(found))
    if missing:
        raise SystemExit(f'not in the texture archives: {", ".join(missing)}')
    paths = []
    for name in sorted(found):
        image = decode(found[name])
        path = out / f'{name.removesuffix("-C.TEX")}-native.png'
        (image.convert('RGB') if image.getextrema()[3] == (255, 255) else image).save(path)
        print(f'{path}  {image.width}x{image.height}')
        paths.append(path)
    return paths


def upscale(paths: list[Path], out: Path, ssh: str) -> None:
    work = f'{REMOTE}/view-{os.getpid()}'
    run = lambda *cmd: subprocess.run(cmd, check=True)
    run('ssh', ssh, f'test -x {REMOTE}/bin/realesrgan-ncnn-vulkan && mkdir -p {work}/src {work}/x4')
    try:
        run('rsync', '-a', *map(str, paths), f'{ssh}:{work}/src/')
        run('ssh', ssh, f'cd {REMOTE} && bin/realesrgan-ncnn-vulkan -i view-{os.getpid()}/src '
                        f'-o view-{os.getpid()}/x4 -m bin/models -n realesrgan-x4plus -s 4 -f png >/dev/null 2>&1')
        run('rsync', '-a', f'{ssh}:{work}/x4/', f'{out}/x4/')
    finally:
        subprocess.run(['ssh', ssh, f'rm -rf {work}'])
    for path in paths:
        view = out / f'{path.stem.removesuffix("-native")}-x4.png'
        (out / 'x4' / path.name).replace(view)
        print(view)
    (out / 'x4').rmdir()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('names', nargs='+', help='texture names, e.g. NW_CITY_WOOD_ORNAMENT_01')
    parser.add_argument('--out', required=True, type=Path, help='a folder outside the repository')
    parser.add_argument('--upscale', action='store_true', help='also a 4x view by Real-ESRGAN on the GPU server')
    args = parser.parse_args(argv)
    out = args.out.resolve()
    if out == ROOT or ROOT in out.parents:
        raise SystemExit('the original pixels stay out of the repository: give --out outside it')
    out.mkdir(parents=True, exist_ok=True)
    paths = native(args.names, Path(os.environ.get('GOTHIC_DIR', GAME)), out)
    if args.upscale:
        upscale(paths, out, os.environ.get('GOTHIC_GPU_SSH', 'dev-ubuntu'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
