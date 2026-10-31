"""Review sheet of processed feat pictures: engravings at 256 px on top, below each the icon at its real
96 px inside a mock toast over a dark backdrop.

Usage: uv run --with pillow python kit_sheet.py OUT.png TITLES.json DIR...   (DIR has engraving.jpg, icon.png)
TITLES.json maps a DIR's name to the caption under its toast.
"""

import json
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

out, titles, dirs = (
    Path(sys.argv[1]),
    json.loads(Path(sys.argv[2]).read_text()),
    [Path(p) for p in sys.argv[3:]],
)
FONT = ImageFont.truetype("/System/Library/Fonts/Supplemental/Georgia Bold.ttf", 17)
CELL, GAP, TH = 256, 14, 120
cols = min(len(dirs), 4)
rows = -(-len(dirs) // cols)
sheet = Image.new("RGB", (cols * (CELL + GAP) + GAP, rows * (CELL + TH + 2 * GAP) + GAP), (30, 33, 28))
d = ImageDraw.Draw(sheet, "RGBA")
for i, p in enumerate(dirs):
    x = GAP + (i % cols) * (CELL + GAP)
    y = GAP + (i // cols) * (CELL + TH + 2 * GAP)
    sheet.paste(Image.open(p / "engraving.jpg").resize((CELL, CELL), Image.LANCZOS), (x, y))
    ty = y + CELL + GAP
    d.rounded_rectangle((x, ty, x + CELL, ty + TH), 6, fill=(16, 14, 12, 210), outline=(150, 125, 85, 255))
    sheet.paste(Image.open(p / "icon.png").resize((96, 96), Image.LANCZOS), (x + 12, ty + 12))
    d.text((x + 120, ty + 20), titles.get(p.name, p.name), font=FONT, fill=(235, 220, 190))
sheet.save(out)
print(out)
