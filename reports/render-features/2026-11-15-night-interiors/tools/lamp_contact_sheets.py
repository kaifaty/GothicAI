#!/usr/bin/env python3
"""Sheets of the lamps' screen ray (../README.md, "Шаг 4") from two runs of lamp-contact.bench, without the ray
(OPENGOTHIC_VSM_STUDY=32) and with it: every view as a crop of its two shots and what the ray took away (the
difference, brightened).

    uv run --no-project --with numpy --with pillow python lamp_contact_sheets.py <without> <with> <out dir> [view ...]

With --whole a view goes on its sheet uncropped.
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

# view -> the box of the 2560x1440 shot that goes on the sheet
VIEWS = {
    'face-front': (900, 250, 1700, 1050),
    'face-34': (950, 350, 1650, 1050),
    'face-near': (700, 150, 1900, 1350),
    'tower-body': (800, 100, 1800, 1300),
    'tower-wide': (700, 300, 1900, 1200),
    'table-a': (1000, 560, 1900, 1160),
    'table-candle': (900, 750, 1700, 1200),
    'cup-a': (1000, 700, 1800, 1300),
    'lantern-feet': (700, 650, 1700, 1150),
    'lantern-back': (800, 100, 1800, 1300),
    'lantern-front': (800, 100, 1800, 1300),
    'lantern-wide': (1400, 300, 2400, 1100),
    'lantern-face-front': (900, 250, 1700, 1050),
    'lantern-face-34': (950, 350, 1650, 1050),
    'lantern-face-side': (900, 300, 1700, 1100),
    'lantern-face-far': (900, 300, 1700, 1100),
    'trap-sword-game': (300, 100, 2260, 1340),
    'trap-sword-near': (300, 100, 2260, 1340),
    'trap-edge': (0, 100, 1960, 1340),
}
HEIGHT = 560


def tile(img: Image.Image, label: str) -> Image.Image:
    img = img.resize((round(img.width * HEIGHT / img.height), HEIGHT), Image.LANCZOS)
    draw = ImageDraw.Draw(img)
    draw.rectangle((0, 0, 8 + draw.textlength(label), 16), fill=(0, 0, 0))
    draw.text((4, 2), label, fill=(255, 255, 255))
    return img


def sheets(without: Path, with_it: Path, out: Path, views: dict, names: list, whole: bool,
           labels=('no ray', 'ray'), prefix='lamp-contact-') -> None:
    """A sheet per view: the shot of each run and what the second run took away. Other notes use it with their own
    views, labels and prefix (2026-11-16-contact-ambient/tools/contact_ambient_sheets.py)."""
    out.mkdir(parents=True, exist_ok=True)
    for view in names or views:
        off = Image.open(without / f'{view}.png').convert('RGB')
        on = Image.open(with_it / f'{view}.png').convert('RGB')
        if not whole:
            off, on = off.crop(views[view]), on.crop(views[view])
        a = np.asarray(off).astype(np.int16)
        b = np.asarray(on).astype(np.int16)
        # darker in the second run: white; brighter (which the ray never does: what moved between the runs): red
        gone = np.clip((a - b).max(axis=2) * 4, 0, 255).astype(np.uint8)
        came = np.clip((b - a).max(axis=2) * 4, 0, 255).astype(np.uint8)
        diff = Image.fromarray(np.stack([np.maximum(gone, came), gone, gone], axis=2))
        tiles = [tile(off, f'{view}: {labels[0]}'), tile(on, labels[1]), tile(diff, 'difference x4')]
        sheet = Image.new('RGB', (sum(t.width for t in tiles) + 12, HEIGHT), (20, 20, 20))
        x = 0
        for t in tiles:
            sheet.paste(t, (x, 0))
            x += t.width + 6
        name = out / f'{prefix}{view}.jpg'
        sheet.save(name, quality=88)
        darker = float((gone > 16).mean()) * 100
        brighter = float((came > 16).mean()) * 100
        print(f'{name}: darker {darker:.2f} % of the crop, brighter {brighter:.2f} %')


def main() -> int:
    args = [a for a in sys.argv[1:] if a != '--whole']
    whole = '--whole' in sys.argv[1:]
    if len(args) < 3:
        print(__doc__)
        return 2
    sheets(Path(args[0]), Path(args[1]), Path(args[2]), VIEWS, args[3:], whole)
    return 0


if __name__ == '__main__':
    sys.exit(main())
