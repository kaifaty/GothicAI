"""Still frames of upscale-stills.bench against the full resolution (graphics plan 1г).

    uv run --with numpy --with pillow python -I stills.py metrics <100 % dir> <dir> [<dir> ...]
    uv run --with numpy --with pillow python -I stills.py identity <dir a> <dir b>
    uv run --with numpy --with pillow python -I stills.py crops <out dir> <shot> <x,y,w,h> <label=dir> [<label=dir> ...]

metrics: for every PNG of the 100 % run that the other runs have too: the mean |difference| of the luminance with
100 % (0..255), the gradient energy against 100 % (sharp() of the TAA report's metrics.py: mean |gradient| of the
luminance, x plus y; 1.0 is as sharp as 100 %), and the shift against 100 % by phase correlation, in pixels (a frame
that moved would show it here; Apple's sign of a correct jitter is a still scene that does not move).
identity: pixels that differ between two runs of the same stills, and the largest difference.
crops: the same box of one shot from each run, 1:1, side by side with the label above each.
"""
import pathlib
import sys

import numpy as np
from PIL import Image, ImageDraw


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)


def luma(rgb):
    return rgb @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def sharp(f):
    return np.abs(np.diff(f, axis=1)).mean() + np.abs(np.diff(f, axis=0)).mean()


def shift(a, b):
    """The (dx, dy) that moves a onto b, by phase correlation with a parabola through the peak."""
    h, w = a.shape
    win = np.outer(np.hanning(h), np.hanning(w)).astype(np.float32)
    fa, fb = np.fft.rfft2((a - a.mean())*win), np.fft.rfft2((b - b.mean())*win)
    r = fb*np.conj(fa)
    r /= np.abs(r) + 1e-9
    c = np.fft.irfft2(r, s=(h, w))
    y, x = np.unravel_index(np.argmax(c), c.shape)

    def sub(cm, c0, cp):
        d = cm - 2*c0 + cp
        return 0.0 if d == 0 else 0.5*(cm - cp)/d

    dx = x + sub(c[y, (x-1) % w], c[y, x], c[y, (x+1) % w])
    dy = y + sub(c[(y-1) % h, x], c[y, x], c[(y+1) % h, x])
    if dx > w/2:
        dx -= w
    if dy > h/2:
        dy -= h
    return dx, dy


def metrics(ref, others):
    for png in sorted(ref.glob("*.png")):
        r = luma(load(png))
        cells = [f"{png.stem:22s} 100% sharp {sharp(r):6.2f}"]
        for d in others:
            o = d/png.name
            if not o.exists():
                continue
            f = luma(load(o))
            if f.shape != r.shape:
                cells.append(f"| {d.name}: size {f.shape[1]}x{f.shape[0]} != {r.shape[1]}x{r.shape[0]}")
                continue
            dx, dy = shift(r, f)
            cells.append(f"| {d.name}: diff {np.abs(f - r).mean():5.2f} sharp {sharp(f)/sharp(r):4.2f} "
                         f"shift {dx:+.2f},{dy:+.2f}")
        print(" ".join(cells))


def identity(a, b):
    for png in sorted(a.glob("*.png")):
        o = b/png.name
        if not o.exists():
            continue
        x, y = load(png), load(o)
        if x.shape != y.shape:
            print(f"{png.stem:22s} size differs")
            continue
        d = np.abs(x - y).max(axis=2)
        print(f"{png.stem:22s} {x.shape[1]}x{x.shape[0]} differing pixels {int((d > 0).sum()):8d} "
              f"({(d > 0).mean()*100:6.3f} %), largest {int(d.max()):3d}, mean |diff| {np.abs(x - y).mean():.3f}")


def crops(out, shot, box, runs):
    x, y, w, h = map(int, box.split(","))
    pad, top = 8, 22
    tiles = []
    for label, d in runs:
        img = Image.open(pathlib.Path(d)/f"{shot}.png").convert("RGB").crop((x, y, x + w, y + h))
        tile = Image.new("RGB", (w, h + top), (0, 0, 0))
        tile.paste(img, (0, top))
        ImageDraw.Draw(tile).text((4, 4), label, fill=(255, 255, 255))
        tiles.append(tile)
    sheet = Image.new("RGB", (len(tiles)*(w + pad) - pad, h + top), (40, 40, 40))
    for i, t in enumerate(tiles):
        sheet.paste(t, (i*(w + pad), 0))
    out.mkdir(parents=True, exist_ok=True)
    path = out/f"{shot}-{x}-{y}.png"
    sheet.save(path)
    print(path)


def main():
    mode = sys.argv[1]
    if mode == "metrics":
        metrics(pathlib.Path(sys.argv[2]), [pathlib.Path(p) for p in sys.argv[3:]])
    elif mode == "identity":
        identity(pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3]))
    elif mode == "crops":
        crops(pathlib.Path(sys.argv[2]), sys.argv[3], sys.argv[4], [a.split("=", 1) for a in sys.argv[5:]])
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
