"""Strip of frames a..b from rendered views: python3 strip.py <frames dir> <a> <b> <out.png> [views...]"""
import json, sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
fr, a, b, out = Path(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
views = sys.argv[5:] or ["front", "q", "side"]
c = json.loads((fr / "clips.json").read_text())[0]
font = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial Unicode.ttf", 22)
ims = [[Image.open(fr / v / f"f{c['first'] + k:04d}.png").convert("RGB") for k in range(a, b + 1)] for v in views]
w, h = ims[0][0].size
S = Image.new("RGB", ((b - a + 1) * w, len(views) * h), "white"); d = ImageDraw.Draw(S)
for r, row in enumerate(ims):
    for k, im in enumerate(row):
        S.paste(im, (k * w, r * h)); d.text((k * w + 6, r * h + 4), str(a + k), fill="black", font=font)
S.save(out)
