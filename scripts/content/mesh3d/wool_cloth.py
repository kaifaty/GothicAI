"""Fulled wool from a woven-cloth texture: keep its broad tone and borders, lose the coarse weave, lay a napped wool
surface on top.

uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/wool_cloth.py SRC OUT_DIR \
    --kind tick [--mm 1.2] [--seed 5]

reports/content/remaster/city-furniture/mattress-fabric.md, «Шерсть покрывала»: the image_gen edit of the coverlet
drew a basket weave of 1 cm yarn, which reads as sackcloth. A slept-under wool blanket is fulled and napped: the weave
hides under a felted nap, the fibres are a heather of light and dark, the surface has soft density blots and pills.
SRC is the woven texture (its colour across 3 mm and up survives, the weave goes); OUT_DIR gets <kind>.png,
<kind>.normal.png (tangent, OpenGL) and <kind>.roughness.png for furniture_gen.py own_maps. --mm is the texel size the
texture lies at on the bed, so the fibres keep their size in millimetres.
"""
import argparse
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

WEAVE_SIGMA_MM = 4.5    # the low pass that removes the source's 9-12 mm weave
BORDER_SIGMA_MM = 1.6   # the woven borders (green over the brown) keep their ornament, softened as fulling does
FIBRE_MM = (0.7, 5.0)   # a fibre streak: across, along
TILT_DEG = 4.0          # median slope of the nap's normal: soft, no grid
ROUGHNESS = 0.93        # dry wool, no sheen in the engine (materialSpecularWeight is 0 above 0.8)


def unit(a):
    return (a - a.mean()) / max(float(a.std()), 1e-6)


def fibres(shape, rng, px, angles=4):
    """Short fibre streaks in a few directions, each direction stronger in its own blots: the nap lies every way."""
    across, along = FIBRE_MM[0] / px, FIBRE_MM[1] / px
    out, weights = np.zeros(shape), []
    for k in range(angles):
        angle = 180 * k / angles + rng.uniform(-15, 15)
        pad = int(max(shape) * 0.42)
        noise = rng.standard_normal((shape[0] + 2 * pad, shape[1] + 2 * pad))
        noise = ndimage.rotate(noise, angle, reshape=False, order=1, mode="wrap")
        noise = ndimage.gaussian_filter(noise, (across, along), mode="wrap")
        noise = ndimage.rotate(noise, -angle, reshape=False, order=1, mode="wrap")[pad:-pad, pad:-pad]
        weights.append(np.exp(1.5 * unit(ndimage.gaussian_filter(rng.standard_normal(shape), 25 / px, mode="wrap"))))
        out += weights[-1] * unit(noise)
    return unit(out / np.sum(weights, axis=0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--kind", default="tick")
    ap.add_argument("--mm", type=float, default=1.2)
    ap.add_argument("--seed", type=int, default=5)
    a = ap.parse_args()
    rng = np.random.default_rng(a.seed)
    px = a.mm
    lin = (np.asarray(Image.open(a.src).convert("RGB"), dtype=np.float32) / 255) ** 2.2
    shape = lin.shape[:2]
    body = np.dstack([ndimage.gaussian_filter(lin[..., c], WEAVE_SIGMA_MM / px) for c in range(3)])
    sharp = np.dstack([ndimage.gaussian_filter(lin[..., c], BORDER_SIGMA_MM / px) for c in range(3)])
    green = ndimage.gaussian_filter(body[..., 1] / np.maximum(body[..., 0], 1e-4), 6 / px)
    border = np.clip((green - 0.72) / 0.06, 0, 1)  # g/r: 0.67 in the brown, 0.81-0.88 in the borders
    border = ndimage.gaussian_filter(ndimage.maximum_filter(border, int(8 / px)), 3 / px)[..., None]
    base = body + border * (sharp - body)

    felt = unit(ndimage.gaussian_filter(rng.standard_normal(shape), 4.0 / px, mode="wrap"))      # density blots
    blots = unit(ndimage.gaussian_filter(rng.standard_normal(shape), 18.0 / px, mode="wrap"))    # wear and fulling
    nap = fibres(shape, rng, px)
    heather = fibres(shape, rng, px, angles=3)       # the same nap, another draw: which fibres are light or dark
    grain = unit(ndimage.gaussian_filter(rng.standard_normal(shape), 0.5 / px, mode="wrap"))
    yy, xx = np.mgrid[:shape[0], :shape[1]]
    twill = np.sin(2 * np.pi * (xx + yy) * px / 3.2)  # a 2/2 twill's diagonal under the nap, only where it shows
    twill *= np.clip(unit(ndimage.gaussian_filter(rng.standard_normal(shape), 12 / px, mode="wrap")) - 0.3, 0, 1)
    pills = np.zeros(shape)
    n = int(shape[0] * shape[1] * px * px / 150)    # a pill every 1.5 square centimetres
    pills[rng.integers(0, shape[0], n), rng.integers(0, shape[1], n)] = 1
    pills = ndimage.gaussian_filter(pills, 0.9 / px, mode="wrap")
    pills /= pills.max()

    shade = 1 + 0.07 * felt + 0.05 * blots + 0.05 * nap + 0.05 * grain + 0.025 * twill + 0.18 * pills
    tint = np.array([1.0, 0.94, 0.80])               # dark fibres browner, light ones greyer: a heathered wool
    colour = base * shade[..., None] * (1 + 0.08 * heather[..., None] * (tint - tint.mean() + 1))
    colour *= base.reshape(-1, 3).mean(0) / colour.reshape(-1, 3).mean(0)
    a.out.mkdir(parents=True, exist_ok=True)
    Image.fromarray((np.clip(colour, 0, 1) ** (1 / 2.2) * 255).round().astype(np.uint8)).save(a.out / f"{a.kind}.png")

    height = 0.6 * felt + 0.5 * nap + 0.3 * grain + 0.25 * twill + 1.2 * pills
    gy, gx = np.gradient(height)
    slope = np.hypot(gx, gy)
    k = np.tan(np.radians(TILT_DEG)) / max(float(np.median(slope)), 1e-6)
    normal = np.dstack([-gx * k, gy * k, np.ones(shape)])  # OpenGL: +Y up the texture
    normal /= np.linalg.norm(normal, axis=2, keepdims=True)
    Image.fromarray(((normal + 1) / 2 * 255).round().astype(np.uint8)).save(a.out / f"{a.kind}.normal.png")
    rough = np.clip(ROUGHNESS + 0.03 * felt, 0.85, 1.0)
    Image.fromarray((rough * 255).round().astype(np.uint8)).save(a.out / f"{a.kind}.roughness.png")
    print("wool", a.out, "mean", np.round(colour.reshape(-1, 3).mean(0), 4).tolist(),
          "log std", round(float(np.log(colour.mean(2) + 1e-4).std()), 3))


if __name__ == "__main__":
    main()
