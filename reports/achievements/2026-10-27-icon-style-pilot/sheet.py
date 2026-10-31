"""Two review sheets of the pilot: sheet-512.jpg (styles x feats at 512 px) and sheet-96.png (each icon at
its real 96 px inside a mock toast on a dark backdrop, as it would sit over the game).

Usage: uv run --with pillow python sheet.py PILOT_DIR
"""

import json
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

root = Path(sys.argv[1])
spec = json.loads((root / "pilot.json").read_text())
styles, feats = list(spec["styles"]), list(spec["feats"])
FONTS = Path("/System/Library/Fonts/Supplemental")


def font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(str(FONTS / ("Georgia Bold.ttf" if bold else "Georgia.ttf")), size)


def load(style: str, feat: str, size: int) -> Image.Image | None:
    p = root / "out" / f"{style}-{feat}.png"
    return Image.open(p).convert("RGB").resize((size, size), Image.LANCZOS) if p.exists() else None


# 512 sheet
CELL, GAP, LEFT, TOP = 512, 24, 260, 90
W = LEFT + len(feats) * (CELL + GAP)
H = TOP + len(styles) * (CELL + GAP)
big = Image.new("RGB", (W, H), (34, 31, 28))
d = ImageDraw.Draw(big)
for j, f in enumerate(feats):
    d.text(
        (LEFT + j * (CELL + GAP), 30), spec["feats"][f]["title"], font=font(30, True), fill=(225, 210, 180)
    )
for i, s in enumerate(styles):
    y = TOP + i * (CELL + GAP)
    d.text((20, y + CELL // 2 - 20), spec["styles"][s]["name"], font=font(30, True), fill=(225, 210, 180))
    for j, f in enumerate(feats):
        x = LEFT + j * (CELL + GAP)
        im = load(s, f, CELL)
        if im:
            big.paste(im, (x, y))
        else:
            d.rectangle((x, y, x + CELL, y + CELL), outline=(120, 60, 60), width=3)
            d.text((x + 20, y + 20), "нет картинки", font=font(26), fill=(200, 120, 120))
big.save(root / "sheet-512.jpg", quality=88)

# 96 sheet: a mock toast per icon, at real size
ICON, TW, TH, PAD = 96, 470, 120, 16
cols = len(feats)
W2 = 200 + cols * (TW + PAD)
H2 = 60 + len(styles) * (TH + PAD)
small = Image.new("RGB", (W2, H2))
for y in range(H2):  # a dim muddy backdrop, roughly the night forest the toast sits over
    ImageDraw.Draw(small).line(
        [(0, y), (W2, y)], fill=(40 - y * 20 // H2, 44 - y * 20 // H2, 36 - y * 18 // H2)
    )
d = ImageDraw.Draw(small, "RGBA")
for i, s in enumerate(styles):
    y = 40 + i * (TH + PAD)
    d.text((16, y + TH // 2 - 12), spec["styles"][s]["name"], font=font(20, True), fill=(225, 210, 180))
    for j, f in enumerate(feats):
        x = 200 + j * (TW + PAD)
        d.rounded_rectangle((x, y, x + TW, y + TH), 6, fill=(16, 14, 12, 200), outline=(150, 125, 85, 255))
        im = load(s, f, ICON)
        if im:
            small.paste(im, (x + 12, y + 12))
        d.text((x + ICON + 26, y + 18), spec["feats"][f]["title"], font=font(19, True), fill=(235, 220, 190))
        words, line, lines = spec["feats"][f]["text"].split(), "", []
        for w in words:  # wrap the line to the toast's width
            if d.textlength(f"{line} {w}".strip(), font=font(15)) > TW - ICON - 44:
                lines.append(line)
                line = w
            else:
                line = f"{line} {w}".strip()
        lines.append(line)
        for k, t in enumerate(lines[:3]):
            d.text((x + ICON + 26, y + 50 + k * 20), t, font=font(15), fill=(200, 190, 170))
small.save(root / "sheet-96.png")
print(root / "sheet-512.jpg", root / "sheet-96.png")
