"""The same box of one shot from several runs, twice the size, side by side with a label over each.

    uv run --with pillow python -I crops.py <out png> <shot> <x,y,w,h> <label>=<run dir> [...]
"""
import pathlib
import sys

from PIL import Image, ImageDraw, ImageFont

FONT = '/System/Library/Fonts/Supplemental/Arial Bold.ttf'
out, shot, box = sys.argv[1:4]
x, y, w, h = map(int, box.split(','))
runs = [a.split('=', 1) for a in sys.argv[4:]]
top, pad = 40, 8
font = ImageFont.truetype(FONT, 26)
img = Image.new('RGB', (len(runs)*(2*w + pad) - pad, 2*h + top), (0, 0, 0))
d = ImageDraw.Draw(img)
for i, (label, run) in enumerate(runs):
    tile = Image.open(pathlib.Path(run)/f'{shot}.png').convert('RGB').crop((x, y, x + w, y + h))
    tile = tile.resize((2*w, 2*h), Image.LANCZOS)
    x0 = i*(2*w + pad)
    img.paste(tile, (x0, top))
    d.text((x0 + 8, 6), label, font=font, fill=(255, 255, 255))
img.save(out)
print(out)
