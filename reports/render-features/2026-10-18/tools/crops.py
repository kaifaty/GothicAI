#!/usr/bin/env python3
"""Side-by-side zoomed crops. Usage: crops.py <out.jpg> <x> <y> <w> <h> <zoom> <caption=png> [caption=png ...]"""
import sys
from PIL import Image, ImageDraw, ImageFont
out, x, y, w, h, zoom = sys.argv[1], *map(int, sys.argv[2:6]), float(sys.argv[6])
items = [a.split("=", 1) for a in sys.argv[7:]]
font = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial Bold.ttf", 24)
W, H = int(w * zoom), int(h * zoom)
sheet = Image.new("RGB", (W * len(items), H), "black")
for i, (cap, png) in enumerate(items):
    im = Image.open(png).convert("RGB").crop((x, y, x + w, y + h)).resize((W, H), Image.NEAREST)
    d = ImageDraw.Draw(im)
    b = d.textbbox((8, 6), cap, font=font)
    d.rectangle((b[0] - 5, b[1] - 3, b[2] + 5, b[3] + 3), fill=(0, 0, 0))
    d.text((8, 6), cap, font=font, fill=(255, 255, 255))
    sheet.paste(im, (i * W, 0))
sheet.save(out, quality=85, optimize=True)
print(out, sheet.size)
