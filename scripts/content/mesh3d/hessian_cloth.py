"""Coarse hemp sacking for a straw mattress, with sewn-on patches: the plain bed of city-furniture.

uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/hessian_cloth.py ORIGINAL OUT_DIR \
    --kind sack [--size 1024 2048] [--mm 1.2 1.12] [--seed 3]

reports/content/remaster/city-furniture/mattress-fabric.md, «Простая кровать»: the original's straw sack
(OBJ_CITY_BEDNORMAL_01) is coarse olive-brown cloth with ribs, three patches (two toward one end, one toward the other) and
painted light. Here the cloth is woven at its real size (ribbed: a 15 mm weft rib over a 6 mm warp, coarse as the original paints it, threads uneven in
width and tone, slubs), the patches sit on the top and the side toward the room, each a cloth of its own with a turned edge and a
running stitch, and the sleeper's place is darker. The colour is the original's mean (ORIGINAL, its linear mean over
the sheet); its painted light is left out, the folds of the simulation give the shade. OUT_DIR gets <kind>.png,
<kind>.normal.png (tangent, OpenGL) and <kind>.roughness.png for furniture_gen.py own_maps. --mm: millimetres per
texel across and along, as the texture lies on the bed (coverlet_sim.py's sheet).
"""
import argparse
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

SUPER = 2            # woven at twice the resolution, then averaged down: no aliasing of the 2-px threads
# A ribbed sacking, coarse as the original paints it: thick weft ribs across the sack over a finer warp. The true size
# (a 3.6 mm pitch) did not read from the usual distance; the user chose «Coarser, like original» (2026-11-03). What
# reads is set by the mip the engine samples: at the bench's distance a screen pixel is about 5.7 mm of the bed, so
# this 1.1 mm texture is sampled at mip 2 and a 9 mm rib is 1.6 pixels, gone (orig-set-11). The original's rib, about
# 13 mm on its 256-texel texture (4.5 mm a texel, mip 0), is about 3 pixels: a rib needs 14 mm and up.
PITCH_MM = (6.0, 15.0)  # warp (across the texture), weft rib (along it)
TILT_DEG = 9.0       # median slope of the weave's normal: coarse cloth, between linen and the plaster's 8
ROUGHNESS = 0.92
# three patches as the original has (two toward one end, one toward the other), placed where they are seen: on the top
# and on the side toward the room (u 1 is glTF +z, the front where the npc stands); fractions of the sheet (u across,
# v along): (u0, u1, v0, v1, tone, tilt in degrees, the patch cloth's tint)
PATCHES = [(0.40, 0.60, 0.40, 0.52, 0.55, 4.0, (1.0, 0.9, 0.8)), (0.84, 0.98, 0.62, 0.74, 1.45, -3.0, (0.95, 1.0, 1.15)),
           (0.26, 0.44, 0.12, 0.22, 0.65, 2.0, (1.1, 0.95, 0.75))]


def layers(lin, mm):
    """Log luminance split at 3 cm: (broad, fine), mm millimetres a texel."""
    log = np.log(lin.mean(2) + 1e-3)
    broad = ndimage.gaussian_filter(log, 30 / mm / 2.355)
    return broad, log - broad


def match_contrast(colour, original, mm, mm_original):
    """Bring the fine (under 3 cm) and the broad tone contrast of colour to the original's own: the original paints
    its sack with about 2.6 times our contrast (log std 0.58 and 0.40 against 0.22 and 0.15, orig-set-12), and in the
    dim room only that much reads; what moves is the luminance, the hue stays."""
    want = [float(x.std()) for x in layers(original, mm_original)]
    broad, fine = layers(colour, mm)
    k = [w / max(float(x.std()), 1e-6) for w, x in zip(want, (broad, fine))]
    gain = np.exp((k[0] - 1) * (broad - broad.mean()) + (k[1] - 1) * fine)
    print("contrast x", np.round(k, 2).tolist())
    return colour * gain[..., None]


def smooth(rng, shape, sigma):
    a = ndimage.gaussian_filter(rng.standard_normal(shape), sigma, mode="wrap")
    return a / max(float(a.std()), 1e-6)


def weave(shape, px, rng, pitch_mm, angle=0.0):
    """A rib weave: the height of the threads and each thread's own tone, at px millimetres per texel (across, along);
    pitch_mm (warp, weft). The thick weft rides over the warp in ribs. Threads wander a little and swell into slubs."""
    h, w = shape
    yy, xx = np.mgrid[:h, :w].astype(np.float64)
    if angle:
        a = np.radians(angle)
        xx, yy = xx * np.cos(a) - yy * np.sin(a), xx * np.sin(a) + yy * np.cos(a)
    pw, pe = pitch_mm
    # threads wander, the ribs barely: a weft rib that wanders reads as wood grain
    xm, ym = xx * px[0] + 0.25 * pw * smooth(rng, shape, 40), yy * px[1] + 0.06 * pe * smooth(rng, shape, 150)
    i, j = np.floor(xm / pw).astype(int), np.floor(ym / pe).astype(int)
    fx, fy = xm / pw - i, ym / pe - j
    n = int(max(xm.max() / pw, ym.max() / pe)) + 2
    thick_w, thick_e = rng.uniform(0.65, 1.25, n), rng.uniform(0.65, 1.25, n)
    tone_w, tone_e = rng.normal(0, 1, n), rng.normal(0, 1, n)
    slub_w = np.clip(smooth(rng, shape, (25, 2)) - 1.4, 0, None)  # thick runs along a warp thread
    slub_e = np.clip(smooth(rng, shape, (2, 25)) - 1.4, 0, None)
    warp = np.sin(np.pi * fx) ** (0.7 / thick_w[i % n]) * (1 + 0.6 * slub_w)
    weft = np.sin(np.pi * fy) ** (0.7 / thick_e[j % n]) * (1 + 0.6 * slub_e)
    over = (i + j) % 2 == 0  # where the warp shows between the ribs
    knot = warp * 0.8 * (0.4 + 0.6 * np.sin(np.pi * fy))  # the warp over a rib, a small knob
    height = np.where(over, np.maximum(weft * 0.75, knot), weft * (0.7 + 0.3 * np.sin(np.pi * fx)))
    tone = np.where(over & (knot > weft * 0.75), tone_w[i % n] + slub_w, tone_e[j % n] + slub_e)
    return height, tone


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("original", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--kind", default="sack")
    ap.add_argument("--size", type=int, nargs=2, default=(1024, 2048))
    ap.add_argument("--mm", type=float, nargs=2, default=(1.2, 1.12))
    ap.add_argument("--seed", type=int, default=3)
    a = ap.parse_args()
    rng = np.random.default_rng(a.seed)
    W, H = a.size[0] * SUPER, a.size[1] * SUPER
    px = (a.mm[0] / SUPER, a.mm[1] / SUPER)
    original = (np.asarray(Image.open(a.original).convert("RGB"), dtype=np.float32) / 255) ** 2.2
    mean = original.reshape(-1, 3).mean(0)

    height, tone = weave((H, W), px, rng, PITCH_MM)
    shade = 1 + 0.14 * tone + 0.10 * smooth(rng, (H, W), 60 / px[0]) + 0.06 * smooth(rng, (H, W), 8 / px[0])
    colour = np.ones((H, W, 3)) * shade[..., None]
    hue = smooth(rng, (H, W), 3 / px[0])[..., None] * np.array([0.03, 0.0, -0.05])  # olive and brown fibres
    colour *= 1 + hue
    v, u = np.mgrid[:H, :W] / np.array([H, W])[:, None, None]
    # the sleeper's place: sweat and wear toward the head, soft-edged
    body = np.exp(-(((u - 0.5) / 0.22) ** 2 + ((v - 0.33) / 0.2) ** 2))
    colour *= (1 - 0.18 * body)[..., None]
    raise_ = np.zeros((H, W))
    for u0, u1, v0, v1, ptone, angle, tint in PATCHES:
        cu, cv, a_ = (u0 + u1) / 2, (v0 + v1) / 2, np.radians(angle)
        du, dv = (u - cu) * W * px[0], (v - cv) * H * px[1]               # millimetres from its centre
        pu, pv = du * np.cos(a_) + dv * np.sin(a_), -du * np.sin(a_) + dv * np.cos(a_)
        hu, hv = (u1 - u0) * W * px[0] / 2, (v1 - v0) * H * px[1] / 2
        edge = np.minimum(hu - np.abs(pu), hv - np.abs(pv))                 # millimetres inside the patch's edge
        edge += 4.0 * smooth(rng, (H, W), 10 / px[0])                        # cut by hand, not by a ruler
        inside = np.clip(edge / 1.5, 0, 1)
        ph, pt = weave((H, W), px, rng, (PITCH_MM[1] * 0.7, PITCH_MM[0] * 1.2), angle + 2)  # its ribs the other way
        cloth = ptone * (1 + 0.10 * pt) * (1 + 0.05 * smooth(rng, (H, W), 10 / px[0]))
        stitch = (np.abs(edge - 8) < 1.1) & (np.mod(np.where(np.abs(pu) > np.abs(pv) * hu / hv, pv, pu), 15) < 10)
        turned = np.clip(1 - np.abs(edge - 3) / 3, 0, 1)                      # the folded-under rim
        cloth = cloth[..., None] * np.array(tint) * (1 - 0.35 * turned)[..., None]
        colour = colour * (1 - inside[..., None]) + inside[..., None] * cloth
        colour *= (1 - 0.6 * stitch * inside)[..., None]
        height = height * (1 - inside) + inside * (ph + 0.8 * turned + 0.6)
        raise_ += inside
    colour *= (0.40 + 0.80 * np.clip(height / np.percentile(height, 99), 0, 1))[..., None]  # the gaps between threads
    colour = colour.reshape(a.size[1], SUPER, a.size[0], SUPER, 3).mean((1, 3))
    height = height.reshape(a.size[1], SUPER, a.size[0], SUPER).mean((1, 3))
    colour = match_contrast(colour, original, sum(a.mm) / 2, 2300 / original.shape[0])  # its 512 texels along 2.3 m
    colour *= mean / colour.reshape(-1, 3).mean(0)
    a.out.mkdir(parents=True, exist_ok=True)
    Image.fromarray((np.clip(colour, 0, 1) ** (1 / 2.2) * 255).round().astype(np.uint8)).save(a.out / f"{a.kind}.png")
    gy, gx = np.gradient(ndimage.gaussian_filter(height, 0.5))
    k = np.tan(np.radians(TILT_DEG)) / max(float(np.median(np.hypot(gx, gy))), 1e-6)
    normal = np.dstack([-gx * k, gy * k, np.ones(height.shape)])
    normal /= np.linalg.norm(normal, axis=2, keepdims=True)
    Image.fromarray(((normal + 1) / 2 * 255).round().astype(np.uint8)).save(a.out / f"{a.kind}.normal.png")
    rough = np.full(height.shape, ROUGHNESS)
    Image.fromarray((rough * 255).round().astype(np.uint8)).save(a.out / f"{a.kind}.roughness.png")
    print("hessian", a.out, "mean", np.round(colour.reshape(-1, 3).mean(0), 4).tolist(), "target", np.round(mean, 4).tolist())


if __name__ == "__main__":
    main()
