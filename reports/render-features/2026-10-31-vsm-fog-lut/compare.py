"""Half-res vs full-res fog shadow mask with -vsm 1, against the full-res frame of the fixed build.

usage: uv run --with numpy --with pillow --with scipy python3 compare.py <shots dir> [sheet view...]
<shots dir>/<run>/shot/repeat-1/<view>.png, runs before-half, after-half, before-full, after-full.

Mean |diff| of luminance (0..255): raw, and after a 17 px box blur (by quadrant TL TR BL BR), which keeps
the large fog-shadow shapes and drops leaves, water and walkers. before-full runs the same code as
after-full, so its numbers are the noise of two runs. A sheet per view: the frames and the blurred
difference x40 under them.
"""
import sys
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw
from scipy.ndimage import uniform_filter

RUNS = ("before-half", "after-half", "before-full")
REF = "after-full"


def path(root, run, view):
    return root / run / "shot" / "repeat-1" / f"{view}.png"


def luma(root, run, view):
    return np.asarray(Image.open(path(root, run, view)).convert("L"), dtype=np.float32)


def quadrants(d):
    h, w = d.shape[0] // 2, d.shape[1] // 2
    return (d[:h, :w].mean(), d[:h, w:].mean(), d[h:, :w].mean(), d[h:, w:].mean())


root = Path(sys.argv[1])
views = sorted(p.stem for p in (root / REF / "shot" / "repeat-1").glob("*.png"))
print(f"| view | raw: {' / '.join(RUNS)} | blurred: {' / '.join(RUNS)} (TL TR BL BR) |")
print("|---|---|---|")
for view in views:
    ref = luma(root, REF, view)
    diff = {run: luma(root, run, view) - ref for run in RUNS}
    raw = " / ".join(f"{np.abs(diff[run]).mean():.2f}" for run in RUNS)
    blurred = []
    for run in RUNS:
        d = np.abs(uniform_filter(diff[run], 17))
        blurred.append(f"{d.mean():.2f} ({' '.join(f'{v:.2f}' for v in quadrants(d))})")
    print(f"| {view} | {raw} | {' / '.join(blurred)} |")

for view in sys.argv[2:]:
    ref = luma(root, REF, view)
    top, bottom = [], []
    for run in ("before-half", "after-half", REF):
        tile = Image.open(path(root, run, view)).convert("RGB").resize((640, 360), Image.LANCZOS)
        ImageDraw.Draw(tile).text((6, 344), run, fill=(255, 255, 0))
        top.append(tile)
    for run in RUNS:
        d = uniform_filter(luma(root, run, view) - ref, 9)
        tile = Image.fromarray(np.clip(128 + d * 40, 0, 255).astype(np.uint8)).convert("RGB")
        tile = tile.resize((640, 360), Image.LANCZOS)
        ImageDraw.Draw(tile).text((6, 344), f"{run} - {REF}, x40", fill=(255, 255, 0))
        bottom.append(tile)
    sheet = Image.new("RGB", (3 * 640 + 16, 2 * 360 + 8), "white")
    for i, (a, b) in enumerate(zip(top, bottom)):
        sheet.paste(a, (i * 648, 0))
        sheet.paste(b, (i * 648, 368))
    sheet.save(Path(__file__).with_name(f"{view}.jpg"), quality=85)
