#!/usr/bin/env python3
"""Crops of bench shots side by side, for the sheets of the note.

  sheet.py <out.jpg> [--cols N] [--box x0,y0,x1,y1] [--h px] <title>=<shot.png> ...

A shot of the occlusion alone (`-ao-` in its name) is shown as grey: its red channel.
"""
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont


def main():
    args = sys.argv[1:]
    out = args.pop(0)
    cols, box, height, items = 4, None, 560, []
    while args:
        a = args.pop(0)
        if a == "--cols":
            cols = int(args.pop(0))
        elif a == "--box":
            box = tuple(int(v) for v in args.pop(0).split(","))
        elif a == "--h":
            height = int(args.pop(0))
        else:
            items.append(a.split("=", 1))
    try:
        font = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", 22)
    except OSError:
        font = ImageFont.load_default()
    cells = []
    for title, path in items:
        im = Image.open(path).convert("RGB")
        if "-ao-" in path:
            red = np.asarray(im)[..., 0]
            im = Image.fromarray(np.repeat(red[..., None], 3, axis=2))
        if box:
            im = im.crop(box)
        im = im.resize((round(im.width * height / im.height), height), Image.LANCZOS)
        cell = Image.new("RGB", (im.width, height + 34), (20, 20, 20))
        cell.paste(im, (0, 34))
        ImageDraw.Draw(cell).text((8, 5), title, font=font, fill=(235, 235, 235))
        cells.append(cell)
    rows = [cells[i:i + cols] for i in range(0, len(cells), cols)]
    width = max(sum(c.width for c in r) + 6 * (len(r) - 1) for r in rows)
    sheet = Image.new("RGB", (width, sum(r[0].height for r in rows) + 6 * (len(rows) - 1)), (20, 20, 20))
    y = 0
    for r in rows:
        x = 0
        for c in r:
            sheet.paste(c, (x, y))
            x += c.width + 6
        y += r[0].height + 6
    sheet.save(out, quality=90)
    print(out, sheet.size)


if __name__ == "__main__":
    main()
