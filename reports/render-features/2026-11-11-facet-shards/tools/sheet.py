#!/usr/bin/env python3
"""Tile crops of shots side by side with labels.

  sheet.py <out.jpg> <x0,y0,x1,y1> <scale> <columns> <label>=<png> ...
"""
import sys
from PIL import Image, ImageDraw, ImageFont


def main():
    out, box, scale, cols = sys.argv[1], tuple(int(v) for v in sys.argv[2].split(",")), float(sys.argv[3]), int(sys.argv[4])
    items = [a.split("=", 1) for a in sys.argv[5:]]
    try:
        font = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 22)
    except OSError:
        font = ImageFont.load_default()
    tiles = []
    for label, path in items:
        im = Image.open(path).convert("RGB").crop(box)
        im = im.resize((round(im.width * scale), round(im.height * scale)), Image.LANCZOS)
        d = ImageDraw.Draw(im)
        d.rectangle((0, 0, im.width, 30), fill=(0, 0, 0))
        d.text((8, 3), label, fill=(255, 255, 255), font=font)
        tiles.append(im)
    w, h = tiles[0].size
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * w + (cols - 1) * 4, rows * h + (rows - 1) * 4), (40, 40, 40))
    for i, t in enumerate(tiles):
        sheet.paste(t, ((i % cols) * (w + 4), (i // cols) * (h + 4)))
    sheet.save(out, quality=90)
    print(out, sheet.size)


main()
