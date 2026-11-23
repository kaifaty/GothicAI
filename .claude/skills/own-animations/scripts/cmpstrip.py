"""Before/after rows of frames a..b: python3 cmpstrip.py <before frames> <after frames> <a> <b> <out.png> <view>..."""
import json, sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
fb, fa, a, b, out = Path(sys.argv[1]), Path(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
views = sys.argv[6:] or ["q"]
font = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial Unicode.ttf", 22)
rows = []
for v in views:
    for fr, tag in ((fb, "до"), (fa, "после")):
        c = json.loads((fr / "clips.json").read_text())[0]
        rows.append((f"{tag} {v}", [Image.open(fr / v / f"f{c['first'] + k:04d}.png").convert("RGB") for k in range(a, b + 1)]))
w, h = rows[0][1][0].size
S = Image.new("RGB", ((b - a + 1) * w, len(rows) * h), "white"); d = ImageDraw.Draw(S)
for r, (tag, ims) in enumerate(rows):
    for k, im in enumerate(ims):
        S.paste(im, (k * w, r * h)); d.text((k * w + 6, r * h + 4), f"{a + k}" + (f"  {tag}" if k == 0 else ""), fill="yellow", font=font)
S.save(out)
