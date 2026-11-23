# /// script
# dependencies = ["pillow", "numpy"]
# ///
"""Side-by-side sheets of sky-height-shots runs and pixel differences.

uv run --no-project --with pillow --with numpy python pairs.py now=<dir> fix=<dir> ref=<dir> --out <dir>

Each sheet is one viewpoint: the runs left to right with their labels. The table compares every run
with the last one (the reference): mean and max absolute difference over RGB, and the share of
pixels off by more than 8/255.
"""
import argparse
import pathlib

import numpy as np
from PIL import Image, ImageDraw, ImageFont

LABELS = {"now": "сейчас (main, потоковая)", "fix": "с починкой (потоковая)",
          "ref": "эталон (OPENGOTHIC_STREAMING=0)"}


def font() -> ImageFont.ImageFont:
    for path in ("/System/Library/Fonts/Supplemental/Arial.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"):
        if pathlib.Path(path).exists():
            return ImageFont.truetype(path, 16)
    return ImageFont.load_default()


def shots(run: pathlib.Path) -> dict[str, pathlib.Path]:
    return {p.stem: p for p in sorted(run.rglob("*.png")) if p.stem.split("-")[-1] in ("12", "20")}


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+", help="name=dir, the last one is the reference")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    runs = [(r.split("=", 1)[0], pathlib.Path(r.split("=", 1)[1])) for r in a.runs]
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    found = [shots(d) for _, d in runs]
    names = sorted(set.intersection(*(set(f) for f in found)))
    rows = []
    for name in names:
        imgs = [Image.open(f[name]).convert("RGB") for f in found]
        arr = [np.asarray(i, dtype=np.int16) for i in imgs]
        for (label, _), x in zip(runs[:-1], arr[:-1]):
            d = np.abs(x - arr[-1])
            rows.append((name, label, float(d.mean()), int(d.max()), float((d.max(axis=2) > 8).mean() * 100)))
        w, h = imgs[0].size
        sheet = Image.new("RGB", (w * len(imgs), h + 28), (20, 20, 20))
        draw, f = ImageDraw.Draw(sheet), font()
        for k, ((label, _), img) in enumerate(zip(runs, imgs)):
            sheet.paste(img, (k * w, 28))
            draw.text((k * w + 8, 7), f"{name}: {LABELS.get(label, label)}", fill=(235, 235, 235), font=f)
        sheet.save(out / f"{name}.png")
    lines = ["| кадр | прогон | среднее | максимум | пикселей > 8/255, % |", "|---|---|---|---|---|"]
    lines += [f"| {n} | {l} | {m:.2f} | {x} | {p:.1f} |" for n, l, m, x, p in rows]
    (out / "diff.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
