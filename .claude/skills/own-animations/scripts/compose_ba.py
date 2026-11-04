"""Before (top row) and after (bottom row) of one move at the three 1H levels from the game, view by view: the video the
user accepts a fix from.

    uv run -q --with pillow python compose_ba.py <old bench dir> <new bench dir> <labels.json> <out.mp4>

labels.json: {"old": ["Новичок · сейчас в игре", …], "new": ["Новичок · A, правки по списку", …]} with three texts each;
optional "keys" (three take names, default nov, fig, mas as levels_bench.py records them) and "sect"
([[suffix, view, seconds], …], default side, three-quarter, front, side slowed ×3). Pass an absolute <out.mp4>.
"""
import json, subprocess, sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
old, new, lab, out = Path(sys.argv[1]), Path(sys.argv[2]), json.loads(Path(sys.argv[3]).read_text()), Path(sys.argv[4])
F = "/System/Library/Fonts/Supplemental/Arial Unicode.ttf"
KEYS = lab.get("keys", ["nov", "fig", "mas"])
SECT = [tuple(x) for x in lab.get("sect", [("", "сбоку", 2.0), ("-q", "в три четверти", 2.0), ("-front", "спереди", 2.0), ("-slow", "сбоку, замедленно ×3", 5.4)])]
w, h = 400, 450
tmp = out.parent / (out.stem + "-parts"); tmp.mkdir(exist_ok=True)
parts = []
for si, (suf, view, dur) in enumerate(SECT):
    ins, filt, n = [], [], 0
    for row, (bench, texts, tag) in enumerate(((old, lab["old"], "было"), (new, lab["new"], "стало"))):
        for i, key in enumerate(KEYS):
            img = Image.new("RGBA", (w, 54), (0, 0, 0, 170)); d = ImageDraw.Draw(img)
            d.text((10, 4), f"{tag.upper()} · {view}", font=ImageFont.truetype(F, 17), fill=(255, 220, 120, 255))
            d.text((10, 28), texts[i], font=ImageFont.truetype(F, 15), fill=(255, 255, 255, 255))
            lp = tmp / f"l{si}{row}{i}.png"; img.save(lp)
            ins += ["-i", str(bench / f"{key}{suf}.mp4"), "-i", str(lp)]
            filt.append(f"[{2 * n}:v]crop=400:450:200:60,scale={w}:{h},fps=30,tpad=stop_mode=clone:stop_duration=2[s{n}];"
                        f"[s{n}][{2 * n + 1}:v]overlay=0:0,trim=duration={dur + 0.4}[p{n}]")
            n += 1
    filt.append("".join(f"[p{k}]" for k in range(6)) + f"xstack=inputs=6:layout=0_0|{w}_0|{2 * w}_0|0_{h}|{w}_{h}|{2 * w}_{h}[o]")
    o = tmp / f"r{si}.mp4"
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", *ins, "-filter_complex", ";".join(filt), "-map", "[o]", "-c:v", "libx264",
                    "-pix_fmt", "yuv420p", "-crf", "22", str(o)], check=True)
    parts.append(o)
lst = tmp / "list.txt"; lst.write_text("".join(f"file '{p}'\n" for p in parts))
subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "concat", "-safe", "0", "-i", str(lst), "-c", "copy", str(out)], check=True)
print(out)
