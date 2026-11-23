#!/usr/bin/env python3
"""Compare the shots of scripts/bench/ssao-behind-camera.bench: the SSAO samples behind the camera read off the
screen as before 2026-11-13 against the game's.

  pairs.py <run dir> [<sheets dir>]

For every view with the occlusion alone, <view>-ao-old.png, <view>-ao-new.png, <view>-ao-old2.png (the old reading
shot again after the new one; the red channel, 255 open), and the frames <view>-old.png, -new.png, -old2.png:
  ao       the mean occlusion over the surfaces (not the sky), old -> new;
  lighter  pixels where the occlusion of the new shot is more than 8 of 255 above both old ones;
  darker   the same below both old ones - the fix only opens samples, so this is the noise of the pass;
  frame    the mean brightness of the frame (0..255), old, new, old again: what the change comes to on the screen.
The frames are not compared pixel by pixel: under TAA a still differs from the next one by the jitter's phase on
every sharp texture, and a lamp flickers; the occlusion alone has neither.
With <sheets dir>, a sheet "old | new | occlusion old | occlusion new" per view.
"""
import glob
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


def main():
    d = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else None
    if out:
        os.makedirs(out, exist_ok=True)
    try:
        font = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 20)
    except OSError:
        font = ImageFont.load_default()
    print(f"{'view':14} {'ao old':>7} {'ao new':>7} {'lighter %':>10} {'darker %':>9} | {'frame old':>9} {'new':>6} {'old2':>6}")
    for old in sorted(glob.glob(os.path.join(d, "*-ao-old.png"))):
        view = os.path.basename(old)[: -len("-ao-old.png")]
        p = lambda tag: os.path.join(d, f"{view}-{tag}.png")
        a, b, a2 = load(p("old")), load(p("new")), load(p("old2"))
        o, n, o2 = load(p("ao-old"))[..., 0], load(p("ao-new"))[..., 0], load(p("ao-old2"))[..., 0]
        surf = (o > 0) | (n > 0)
        lighter = surf & (n > np.maximum(o, o2) + 8)
        darker = surf & (n < np.minimum(o, o2) - 8)
        size = o.size
        print(f"{view:14} {o[surf].mean():7.1f} {n[surf].mean():7.1f} {100 * lighter.sum() / size:10.2f} {100 * darker.sum() / size:9.2f} | "
              f"{a.mean():9.2f} {b.mean():6.2f} {a2.mean():6.2f}")
        if not out:
            continue
        grey = lambda c: np.repeat(np.clip(c, 0, 255).astype(np.uint8)[..., None], 3, axis=2)
        cells = [("old", a.astype(np.uint8)), ("new", b.astype(np.uint8)), ("occlusion old", grey(o)), ("occlusion new", grey(n))]
        w, h = a.shape[1] // 2, a.shape[0] // 2
        sheet = Image.new("RGB", (w * 4 + 18, h + 30), (20, 20, 20))
        draw = ImageDraw.Draw(sheet)
        for i, (title, img) in enumerate(cells):
            sheet.paste(Image.fromarray(img).resize((w, h), Image.LANCZOS), (i * (w + 6), 30))
            draw.text((i * (w + 6) + 6, 4), title, font=font, fill=(235, 235, 235))
        sheet.save(os.path.join(out, f"{view}.jpg"), quality=88)


if __name__ == "__main__":
    main()
