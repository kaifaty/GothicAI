"""Procedural tileable forged iron with rust, matched to a measured mean colour.

uv run --with numpy --with pillow python iron.py OUT_DIR --target R G B [--size 1024] [--seed 1]
Writes albedo/ao/roughness/metallic/normal/height PNG. Every field is periodic,
so the tile has no seams. Mean colour: uniform per-channel linear gains, as the
stage-1 materials do.
"""
import argparse
from pathlib import Path

import numpy as np
from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument("out")
ap.add_argument("--target", nargs=3, type=float, required=True, help="linear RGB mean")
ap.add_argument("--size", type=int, default=1024)
ap.add_argument("--seed", type=int, default=1)
args = ap.parse_args()
n = args.size
rng = np.random.default_rng(args.seed)
fy, fx = np.meshgrid(np.fft.fftfreq(n) * n, np.fft.fftfreq(n) * n, indexing="ij")
freq = np.hypot(fx, fy)


def noise(lo, hi, beta=1.0):
    """Periodic band-limited noise between lo and hi cycles per tile, normalised to 0..1."""
    spectrum = np.fft.fft2(rng.standard_normal((n, n)))
    weight = np.where((freq >= lo) & (freq <= hi), 1.0 / np.maximum(freq, 1) ** beta, 0.0)
    field = np.real(np.fft.ifft2(spectrum * weight))
    return (field - field.min()) / (field.max() - field.min())


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)


# Hammer dents: periodic sum of shallow round dimples.
yy, xx = np.mgrid[0:n, 0:n] / n
dents = np.zeros((n, n))
for _ in range(260):
    px, py, r = rng.random(), rng.random(), rng.uniform(0.015, 0.05)
    dx = (xx - px + 0.5) % 1 - 0.5
    dy = (yy - py + 0.5) % 1 - 0.5
    dents -= np.exp(-(dx * dx + dy * dy) / (2 * r * r)) * rng.uniform(0.4, 1.0)
dents = (dents - dents.min()) / (dents.max() - dents.min())
grain = noise(40, 200, 0.5)
rust_field = 0.6 * noise(5, 24, 1.2) + 0.4 * noise(24, 90, 1.0)
rust = smoothstep(*np.quantile(rust_field, [0.80, 0.90]), rust_field)  # about 15% rust
pits = smoothstep(0.75, 0.9, noise(60, 250, 0.3)) * rust
height = 0.6 * dents + 0.25 * grain - 0.35 * pits + 0.15 * rust * noise(20, 120)

iron = np.stack([0.034, 0.033, 0.029]) * (0.75 + 0.5 * grain[..., None]) * (0.85 + 0.3 * dents[..., None])
rust_col = np.stack([0.058, 0.038, 0.021]) * (0.7 + 0.6 * noise(8, 80)[..., None])
albedo = iron * (1 - rust[..., None]) + rust_col * rust[..., None]
albedo *= 1 - 0.35 * pits[..., None]
gain = np.array(args.target) / albedo.reshape(-1, 3).mean(axis=0)
albedo = np.clip(albedo * gain, 0, 1)
roughness = np.clip(0.48 + 0.12 * grain + 0.35 * rust + 0.1 * pits, 0, 1)
metallic = np.clip(0.85 * (1 - rust) * (1 - pits), 0, 1)
ao = np.clip(0.75 + 0.25 * (height - height.min()) / np.ptp(height), 0, 1)

strength = 6.0
gx = (np.roll(height, -1, 1) - np.roll(height, 1, 1)) / 2  # periodic gradient
gy = (np.roll(height, -1, 0) - np.roll(height, 1, 0)) / 2
normal = np.stack([-gx * strength * n / 64, gy * strength * n / 64, np.ones_like(height)], axis=-1)
normal /= np.linalg.norm(normal, axis=-1, keepdims=True)

out = Path(args.out)
out.mkdir(parents=True, exist_ok=True)


def srgb(lin):
    return np.where(lin <= 0.0031308, 12.92 * lin, 1.055 * np.power(lin, 1 / 2.4) - 0.055)


def save(name, array):
    Image.fromarray((np.clip(array, 0, 1) * 255 + 0.5).astype(np.uint8)).save(out / f"{name}.png")


save("albedo", srgb(albedo))
save("roughness", roughness)
save("metallic", metallic)
save("ao", ao)
save("height", (height - height.min()) / np.ptp(height))
save("normal", normal * 0.5 + 0.5)


def oklab(rgb):
    m1 = np.array([[0.4122214708, 0.5363325363, 0.0514459929], [0.2119034982, 0.6806995451, 0.1073969566],
                   [0.0883024619, 0.2817188376, 0.6299787005]])
    m2 = np.array([[0.2104542553, 0.7936177850, -0.0040720468], [1.9779984951, -2.4285922050, 0.4505937099],
                   [0.0259040371, 0.7827717662, -0.8086757660]])
    return np.cbrt(rgb @ m1.T) @ m2.T


flat = albedo.reshape(-1, 3)
bare = rust.reshape(-1) < 0.1
print("iron mean linear", flat.mean(axis=0).round(4), "rust share", round(float(rust.mean()), 3),
      "gains", gain.round(3), "OKLab bare", oklab(flat[bare].mean(axis=0)).round(3),
      "OKLab rust", oklab(flat[~bare].mean(axis=0)).round(3))
