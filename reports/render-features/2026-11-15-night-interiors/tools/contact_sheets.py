#!/usr/bin/env python3
"""Sheets of the contact-shadow measure (../README.md, "Замер контактных теней") from three runs of
contact-shadow.bench: the game, the reference (OPENGOTHIC_VSM=0: shadows by a ray) and the run without GI
(OPENGOTHIC_GI=0: the ambient light takes SSAO).

    uv run --no-project --with pillow python contact_sheets.py <game> <reference> <without GI> <out dir>

A sheet is a row of crops of 2560x1440 shots; each crop names its run in the corner.
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

# sheet -> (height of a crop on the sheet, [(run, shot, box of the shot)])
SHEETS = {
    # the contact holds: the sun, a lantern, a torch
    'contact-holds': (420, [
        ('game', 'feet-noon', (800, 650, 1700, 1150)),
        ('game', 'feet-lantern-0', (700, 650, 1700, 1150)),
        ('game', 'tower-feet-0', (800, 650, 1800, 1150)),
    ]),
    # things by a candle: an item is not in the ray scene, so the reference shows the table without its shadow
    'contact-candle': (420, [
        ('game', 'table-a', (1000, 560, 1900, 1160)),
        ('ref', 'table-a', (1000, 560, 1900, 1160)),
        ('game', 'table-candle', (900, 750, 1700, 1200)),
        ('ref', 'table-candle', (900, 750, 1700, 1200)),
    ]),
    'contact-cup': (420, [
        ('game', 'cup-a', (1000, 700, 1800, 1300)),
        ('ref', 'cup-a', (1000, 700, 1800, 1300)),
    ]),
    # a face at a torch: the shadows of the nose, the chin and the collar
    'contact-face': (520, [
        ('game', 'face-34', (1000, 450, 1600, 950)),
        ('game', 'face-front', (950, 300, 1650, 1000)),
    ]),
    # no direct light: nothing ties the foot to the ground under the GI probes; with SSAO it is tied
    'contact-shade': (420, [
        ('game', 'feet-shade', (900, 700, 1700, 1200)),
        ('nogi', 'feet-shade', (900, 700, 1700, 1200)),
        ('game', 'feet-shade-wide', (1000, 500, 1700, 1100)),
        ('nogi', 'feet-shade-wide', (1000, 500, 1700, 1100)),
    ]),
    # grass: the blades cast no shadow on the ground or on each other
    'contact-grass': (480, [
        ('game', 'grass-noon-0', (700, 600, 1700, 1200)),
        ('game', 'grass-low-0', (700, 600, 1700, 1200)),
    ]),
}
LABELS = {'game': 'game', 'ref': 'rays (VSM off)', 'nogi': 'no GI (SSAO)'}


def main() -> int:
    if len(sys.argv) != 5:
        print(__doc__)
        return 2
    runs = {'game': Path(sys.argv[1]), 'ref': Path(sys.argv[2]), 'nogi': Path(sys.argv[3])}
    out = Path(sys.argv[4])
    out.mkdir(parents=True, exist_ok=True)
    for name, (height, crops) in SHEETS.items():
        tiles = []
        for run, shot, box in crops:
            tile = Image.open(runs[run] / f'{shot}.png').convert('RGB').crop(box)
            tile = tile.resize((round(tile.width * height / tile.height), height), Image.LANCZOS)
            draw = ImageDraw.Draw(tile)
            label = f'{shot}: {LABELS[run]}'
            draw.rectangle((0, 0, 8 + draw.textlength(label), 16), fill=(0, 0, 0))
            draw.text((4, 2), label, fill=(255, 255, 255))
            tiles.append(tile)
        gap = 6
        sheet = Image.new('RGB', (sum(t.width for t in tiles) + gap * (len(tiles) - 1), height), (20, 20, 20))
        x = 0
        for tile in tiles:
            sheet.paste(tile, (x, 0))
            x += tile.width + gap
        sheet.save(out / f'{name}.jpg', quality=88)
        print(out / f'{name}.jpg', sheet.size)
    return 0


if __name__ == '__main__':
    sys.exit(main())
