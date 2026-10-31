#!/usr/bin/env python3
"""Comparison grids: one JPEG per shot, the configurations' screenshots side by side with labels.
Usage: uv run --with pillow sheets.py <survey dir> <out dir> <prefix> <labels,comma,separated> <shot> [shot ...]
A label may be name=caption."""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

root, out, prefix = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
labels = [x.split("=", 1) if "=" in x else (x, x) for x in sys.argv[4].split(",")]
shots = sys.argv[5:]
out.mkdir(parents=True, exist_ok=True)
font = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial Bold.ttf", 26)
W, H = 640, 360
for shot in shots:
    tiles = [(cap, root / name / f"{shot}.png") for name, cap in labels if (root / name / f"{shot}.png").exists()]
    if not tiles:
        continue
    cols = 3 if len(tiles) > 4 else 2
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * W, rows * H), "black")
    for i, (cap, png) in enumerate(tiles):
        im = Image.open(png).convert("RGB").resize((W, H), Image.LANCZOS)
        d = ImageDraw.Draw(im)
        box = d.textbbox((10, 8), cap, font=font)
        d.rectangle((box[0] - 6, box[1] - 4, box[2] + 6, box[3] + 4), fill=(0, 0, 0))
        d.text((10, 8), cap, font=font, fill=(255, 255, 255))
        sheet.paste(im, ((i % cols) * W, (i // cols) * H))
    path = out / f"{prefix}-{shot}.jpg"
    sheet.save(path, quality=80, optimize=True)
    print(path.name, path.stat().st_size // 1024, "KB")
