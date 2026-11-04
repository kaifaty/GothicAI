"""Grid video of a move's variants from the game (variants_bench.py takes): every take side by side with its label,
side (or front) and three-quarter, at 1× and slowed ×2. The user picks variants from it.

    uv run -q --with pillow python compose_variants.py <bench dir> <labels.json> <move code> <out.mp4>

labels.json: {"<code>": {"title": "Перекат вперёд", "v1": "сбоку", "v2": "в три четверти",
                         "note": "A–E: сырой ход Kimodo…", "items": [["f0", "Сейчас в игре", "второй ряд"], ["fa", "A · …", "…"], …]}}
Write each label from the Blender side sheet of the variant, not from the small tiles: a "low roll" may be a slide.
Pass an absolute <out.mp4>: ffmpeg's concat list resolves relative paths against itself.
"""
import json
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

bench, spec, out = Path(sys.argv[1]), json.loads(Path(sys.argv[2]).read_text())[sys.argv[3]], Path(sys.argv[4])
labels, slow2 = spec["items"], spec.get("slow", 2)
F = "/System/Library/Fonts/Supplemental/Arial Unicode.ttf"
w, h, cols = 360, 405, 4
tmp = out.parent / (out.stem + "-parts")
tmp.mkdir(exist_ok=True)
SECT = [("", spec["v1"], 1), ("-q", spec["v2"], 1), ("", spec["v1"] + ", замедленно ×2", 2), ("-q", spec["v2"] + ", замедленно ×2", 2)]
parts = []
for si, (suf, view, slow) in enumerate(SECT):
    ins, filt = [], []
    for i, (key, l1, l2) in enumerate(labels):
        img = Image.new("RGBA", (w, 52), (0, 0, 0, 170))
        d = ImageDraw.Draw(img)
        d.text((8, 3), l1, font=ImageFont.truetype(F, 17), fill=(255, 220, 120, 255))
        d.text((8, 27), l2, font=ImageFont.truetype(F, 15), fill=(255, 255, 255, 255))
        lp = tmp / f"l{si}{i}.png"
        img.save(lp)
        ins += ["-i", str(bench / f"{key}{suf}.mp4"), "-i", str(lp)]
        filt.append(f"[{2 * i}:v]crop=400:450:200:60,scale={w}:{h},setpts={slow}*PTS,fps=30,tpad=stop_mode=clone:stop_duration=2[s{i}];"
                    f"[s{i}][{2 * i + 1}:v]overlay=0:0,trim=duration={2.0 * slow + 0.4}[p{i}]")
    n = len(labels)
    blank = Image.new("RGB", (w, h), (20, 20, 20))
    d = ImageDraw.Draw(blank)
    d.text((14, 20), spec["title"], font=ImageFont.truetype(F, 18), fill=(255, 255, 255))
    d.text((14, 50), view, font=ImageFont.truetype(F, 18), fill=(255, 220, 120))
    d.text((14, 90), spec["note"], font=ImageFont.truetype(F, 16), fill=(200, 200, 200))
    bp = tmp / f"b{si}.png"
    blank.save(bp)
    ins += ["-loop", "1", "-t", str(2.0 * slow + 0.4), "-i", str(bp)]
    filt.append(f"[{2 * n}:v]fps=30,format=yuv420p[p{n}]")
    lay = "|".join(f"{(k % cols) * w}_{(k // cols) * h}" for k in range(n + 1))
    filt.append("".join(f"[p{k}]" for k in range(n + 1)) + f"xstack=inputs={n + 1}:layout={lay}:fill=0x141414[o]")
    o = tmp / f"r{si}.mp4"
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", *ins, "-filter_complex", ";".join(filt), "-map", "[o]",
                    "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "21", str(o)], check=True)
    parts.append(o)
lst = tmp / "list.txt"
lst.write_text("".join(f"file '{p}'\n" for p in parts))
subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "concat", "-safe", "0", "-i", str(lst), "-c", "copy", str(out)], check=True)
print(out)
