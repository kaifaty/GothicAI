"""Contact sheet of in-game takes: one row per take, a frame every <step> s. Review every frame of a take with it.

    uv run -q --with pillow python take_sheet.py <bench dir> <out.png> <step s> <duration s> <view suffix or _> <take>...
        [--start S] [--crop crop=W:H:X:Y] [--tile W,H]

Takes are <bench dir>/<take>[-<suffix>].mp4 (suffix `_` for none, or `q`, `front`, `slow`). The default crop keeps the
hero of an 800×600 take with room for a roll; --crop 280:300:260:140 with --tile 168,180 is close enough to read a
blade or a foot, --start skips to the part to zoom on.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ap = argparse.ArgumentParser()
ap.add_argument("bench", type=Path)
ap.add_argument("out")
ap.add_argument("step", type=float)
ap.add_argument("dur", type=float)
ap.add_argument("suffix")
ap.add_argument("takes", nargs="+")
ap.add_argument("--start", default="0")
ap.add_argument("--crop", default="crop=400:450:200:60")
ap.add_argument("--tile", default="150,170")
args = ap.parse_args()
F = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial Unicode.ttf", 14)
w, h = map(int, args.tile.split(","))
suf = args.suffix.strip("_")
crop = args.crop if args.crop.startswith("crop=") else "crop=" + args.crop
n = int(args.dur / args.step) + 1
sheet = Image.new("RGB", (70 + n * w, len(args.takes) * h + 20), (25, 25, 25))
d = ImageDraw.Draw(sheet)
for c in range(n):
    d.text((70 + c * w + 4, 2), f"{float(args.start) + c * args.step:.2f}s", font=F, fill=(200, 200, 200))
with tempfile.TemporaryDirectory() as tmp:
    for r, k in enumerate(args.takes):
        src = args.bench / f"{k}{'-' + suf if suf else ''}.mp4"
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-ss", args.start, "-i", str(src), "-vf", f"fps={1 / args.step},{crop},scale={w}:{h}",
                        "-frames:v", str(n), f"{tmp}/{k}-%03d.png"], check=True)
        d.text((6, 20 + r * h + h // 2), k.upper(), font=F, fill=(255, 220, 120))
        for c in range(n):
            p = Path(f"{tmp}/{k}-{c + 1:03d}.png")
            if p.exists():
                sheet.paste(Image.open(p), (70 + c * w, 20 + r * h))
sheet.save(args.out)
print(args.out)
