#!/usr/bin/env python3
"""Compare the shots of scripts/bench/facet-shards.bench: the shadow test as before against the game's.

  pairs.py <run dir> [<out dir>]

For every view with <view>-old.png, <view>-new.png and <view>-old2.png (the old test shot again after the new one):
  noise   pixels that differ by more than 8 of 255 between the two old shots - the world moving, not the fix;
  change  pixels that differ between the old shot and the new one;
  fix     pixels where the new shot differs from both old ones while those two agree;
  and with <view>-diff.png (the sun, `vsm study 8`: the change of the shadow term in one frame) the pixels the fix
  made darker (magenta) and lighter (green), by more than a tenth of the sun and by more than a fiftieth.
With <out dir>, a sheet "old | new | where the fix acts" per view.
"""
import glob
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


def differs(a, b):
    return np.abs(a - b).max(axis=2) > 8


def diff_classes(img):
    """the `-diff` shot: masks (darker strong, darker weak, lighter strong, lighter weak)"""
    r, g, b = img[..., 0], img[..., 1], img[..., 2]
    magenta = (g * 2 < np.minimum(r, b)) & (np.minimum(r, b) > 24)
    green = (np.maximum(r, b) * 2 < g) & (g > 24)
    strong_m = magenta & (np.minimum(r, b) > 110)
    strong_g = green & (g > 110)
    return strong_m, magenta & ~strong_m, strong_g, green & ~strong_g


def main():
    d = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else None
    if out:
        os.makedirs(out, exist_ok=True)
    try:
        font = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 20)
    except OSError:
        font = ImageFont.load_default()
    print(f"{'view':18} {'noise %':>8} {'change %':>9} {'fix %':>7} | {'darker >0.1':>11} {'>0.02':>7} {'lighter >0.1':>12} {'>0.02':>7}")
    for old in sorted(glob.glob(os.path.join(d, "*-old.png"))):
        view = os.path.basename(old)[: -len("-old.png")]
        if view.endswith("-class"):
            continue
        a, b, a2 = load(old), load(os.path.join(d, f"{view}-new.png")), load(os.path.join(d, f"{view}-old2.png"))
        noise, change = differs(a, a2), differs(a, b)
        fix = change & differs(a2, b) & ~noise
        n = noise.size
        row = f"{view:18} {100 * noise.sum() / n:8.3f} {100 * change.sum() / n:9.3f} {100 * fix.sum() / n:7.3f} |"
        third, label = np.where(fix[..., None], 255, 0).astype(np.uint8).repeat(3, axis=2), "fix (new differs from both old)"
        diff_path = os.path.join(d, f"{view}-diff.png")
        if os.path.exists(diff_path):
            img = load(diff_path)
            ms, mw, gs, gw = diff_classes(img)
            row += f" {100 * ms.sum() / n:11.3f} {100 * mw.sum() / n:7.3f} {100 * gs.sum() / n:12.3f} {100 * gw.sum() / n:7.3f}"
            third, label = img.astype(np.uint8), "shadow change: magenta darker, green lighter"
        print(row)
        if out:
            tiles = [Image.fromarray(a.astype(np.uint8)), Image.fromarray(b.astype(np.uint8)), Image.fromarray(third)]
            w, h = tiles[0].size
            tiles = [t.resize((w * 2 // 3, h * 2 // 3), Image.LANCZOS) for t in tiles]
            sheet = Image.new("RGB", (3 * tiles[0].width + 8, tiles[0].height), (40, 40, 40))
            for i, (t, text) in enumerate(zip(tiles, ("old", "new", label))):
                dr = ImageDraw.Draw(t)
                dr.rectangle((0, 0, 14 + 11 * len(text), 28), fill=(0, 0, 0))
                dr.text((6, 3), text, fill=(255, 255, 255), font=font)
                sheet.paste(t, (i * (t.width + 4), 0))
            sheet.save(os.path.join(out, f"{view}.jpg"), quality=88)


main()
