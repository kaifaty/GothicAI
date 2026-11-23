"""Tile rendered frames of one clip into a numbered sheet per view.

    python3 tile.py <frames dir> <clip index> <out prefix> [cols] [label]
"""
import json
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

fr, ci, outp = Path(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
cols = int(sys.argv[4]) if len(sys.argv) > 4 else 6
label = sys.argv[5] if len(sys.argv) > 5 else ""
c = json.loads((fr / "clips.json").read_text())[ci]
font = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial Unicode.ttf", 22)
for view in sorted(p.name for p in fr.iterdir() if p.is_dir()):
    ims = [Image.open(fr / view / f"f{c['first'] + k:04d}.png").convert("RGB") for k in range(c["frames"])]
    w, h = ims[0].size
    rows = (len(ims) + cols - 1) // cols
    top = 36 if label else 0
    sheet = Image.new("RGB", (cols * w, rows * h + top), "white")
    d = ImageDraw.Draw(sheet)
    if label:
        d.text((8, 4), f"{label} — {view}", fill="black", font=font)
    for k, im in enumerate(ims):
        x, y = (k % cols) * w, (k // cols) * h + top
        sheet.paste(im, (x, y))
        d.rectangle([x, y, x + w - 1, y + h - 1], outline=(180, 180, 180))
        d.text((x + 8, y + 6), f"{k}  ·  {k / c['fps']:.2f} с", fill=(200, 0, 0), font=font)
    sheet.save(f"{outp}-{view}.png")
    print(f"{outp}-{view}.png", sheet.size)
