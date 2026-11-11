"""Tiling materials of the hero's outfit, made here: linen, coarse wool, leather, suede, rawhide, iron.

    uv run --with numpy --with pillow python -I scripts/content/hero_body/outfit_tiles.py <out dir>   # a preview sheet

`tile(kind, colour)` gives albedo (sRGB 0..1), a tangent normal (-1..1, +Y along the texture's V, rows down - what
mpfb_write.py's packer takes) and ORM (occlusion, roughness, metal) of one seamless tile, SIZE px for SPAN metres of
cloth (mpfb_export.py maps the pieces at that scale). Nothing is downloaded and nothing is a photo: the relief is a
height field in millimetres - threads over and under each other, creases and pores of leather, the nap of suede,
hammer marks of iron - and the normal is its true slope at the tile's scale, not a strength picked by eye. A relief
several times too strong was the fault of our first tiles (reports/content: prop texture relief).

The colour comes from the caller: the mean of the piece in the original, as the content rules ask. The tile adds
only what the material itself varies in - thread to thread, worn against creased - around that mean.
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

SIZE = 1024
SPAN = 0.35                      # metres of material across the tile (mpfb_export.py TILE)
MM = SPAN * 1000 / SIZE          # millimetres in a pixel


def to_lin(c):
    c = np.asarray(c, float)
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def to_srgb(c):
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)


def noise(rng, lo_mm: float, hi_mm: float, slope: float = 1.0) -> np.ndarray:
    """Seamless noise with features from lo_mm to hi_mm across, zero mean, unit deviation."""
    f = np.hypot(*np.meshgrid(np.fft.fftfreq(SIZE), np.fft.fftfreq(SIZE)))          # cycles per pixel
    lo, hi = MM / hi_mm, MM / lo_mm
    band = ((f >= lo) & (f <= hi)) / np.maximum(f, 1e-6) ** slope
    spec = band * np.exp(2j * np.pi * rng.random((SIZE, SIZE)))
    out = np.fft.ifft2(spec).real
    return (out - out.mean()) / max(out.std(), 1e-12)


def blur(a: np.ndarray, mm: float) -> np.ndarray:
    f2 = np.add.outer(np.fft.fftfreq(SIZE) ** 2, np.fft.fftfreq(SIZE) ** 2)
    return np.fft.ifft2(np.fft.fft2(a) * np.exp(-2 * (np.pi * mm / MM) ** 2 * f2)).real


def normal_of(height_mm: np.ndarray) -> np.ndarray:
    dx = (np.roll(height_mm, -1, 1) - np.roll(height_mm, 1, 1)) / (2 * MM)
    dy = (np.roll(height_mm, -1, 0) - np.roll(height_mm, 1, 0)) / (2 * MM)
    n = np.stack([-dx, -dy, np.ones_like(dx)], 2)
    return n / np.linalg.norm(n, axis=2, keepdims=True)


def cavity(height_mm: np.ndarray, mm: float) -> np.ndarray:
    """How deep a point lies under its surroundings, 0..1 (1 - open)."""
    d = blur(height_mm, mm) - height_mm
    return np.clip(1 - d / max(np.percentile(d, 99), 1e-9), 0, 1)


def weave(rng, thread_mm: float, depth_mm: float, slub: float, fuzz_mm: float):
    """A plain weave: every thread a ridge that dives under each crossing thread; thread thickness varies (slub)."""
    n = int(round(SPAN * 1000 / thread_mm / 2)) * 2          # an even count of threads, so the tile repeats
    u = np.arange(SIZE) / SIZE * n
    x, y = np.meshgrid(u, u)
    wobble = noise(rng, thread_mm * 6, thread_mm * 60) * 0.18          # threads do not run straight
    xi, yi = x + wobble, y + np.roll(wobble.T, SIZE // 3, 0)
    ix, iy = np.floor(xi).astype(int) % n, np.floor(yi).astype(int) % n
    thick_x, thick_y = 1 + slub * rng.standard_normal(n), 1 + slub * rng.standard_normal(n)
    # across a thread: a round back; along it: up over a crossing thread, down under the next
    warp = np.cos((xi % 1 - 0.5) * np.pi) ** 0.7 * (0.5 + 0.5 * np.cos(np.pi * (yi + ix))) * thick_x[ix]
    weft = np.cos((yi % 1 - 0.5) * np.pi) ** 0.7 * (0.5 + 0.5 * np.cos(np.pi * (xi + iy + 1))) * thick_y[iy]
    h = np.maximum(warp, weft) * depth_mm
    h += noise(rng, MM * 2, thread_mm * 0.7) * fuzz_mm            # fibres standing off the threads
    tone = thick_x[ix] * (warp >= weft) + thick_y[iy] * (warp < weft)   # a thicker thread took more dye
    return h, tone - tone.mean()


def tile(kind: str, colour_srgb) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    rng = np.random.default_rng(sum(map(ord, kind)) * 7919)      # the same tile every run
    base = to_lin(colour_srgb)
    metal = 0.0
    if kind in ("linen", "wool"):
        coarse = kind == "wool"
        h, tone = weave(rng, 2.4 if coarse else 1.5, 0.55 if coarse else 0.32, 0.22 if coarse else 0.14,
                        0.10 if coarse else 0.03)
        wear = noise(rng, 25, 300)                               # the cloth is not dyed or worn evenly
        h += wear * 0.05
        gain = 1 + 0.10 * tone + 0.07 * wear + (0.05 if coarse else 0.03) * noise(rng, 1, 6)
        rough = np.full((SIZE, SIZE), 0.95 if coarse else 0.90)
        occ = 0.55 + 0.45 * cavity(h, 1.2)
    elif kind in ("leather", "rawhide", "suede"):
        grain = noise(rng, 0.8, 4.0)                             # the pebbled grain
        crease = 1 - np.abs(noise(rng, 6, 60, 1.6))              # wrinkles: soft valleys, broken up so they do not ring
        crease = np.clip((crease - 0.7) / 0.3, 0, 1) ** 2 * np.clip(noise(rng, 10, 80) * 0.8 + 0.3, 0, 1)
        scuff = np.clip(noise(rng, 15, 200) * 0.6 + 0.5, 0, 1)   # rubbed and worn against untouched
        if kind == "suede":
            h = noise(rng, MM * 2, 1.2) * 0.05 + noise(rng, 20, 200) * 0.05
            gain = 1 + 0.10 * noise(rng, 8, 120) + 0.05 * noise(rng, 0.7, 3)   # the nap lies in patches
            rough = np.full((SIZE, SIZE), 0.92)
        else:
            depth = 0.10 if kind == "leather" else 0.06
            h = grain * depth - crease * (0.18 if kind == "leather" else 0.10) + noise(rng, 40, 300) * 0.08
            # rubbed leather is paler and smoother, a crease holds a little dirt
            gain = 1 + 0.16 * (scuff - 0.5) - 0.10 * crease + 0.05 * grain
            rough = np.clip((0.62 if kind == "leather" else 0.78) - 0.14 * (scuff - 0.5) + 0.1 * crease, 0.3, 1)
        occ = 0.6 + 0.4 * cavity(h, 1.5)
    elif kind == "iron":
        dents = blur(np.clip(noise(rng, 6, 30), -0.5, 3), 1.5)   # hammer marks
        pit = np.clip(noise(rng, 0.7, 3) - 1.3, 0, None)         # rust pits
        h = -dents * 0.12 - pit * 0.08
        patina = np.clip(noise(rng, 10, 150) * 0.5 + 0.5, 0, 1)
        gain = 1 + 0.25 * (patina - 0.5) - 0.3 * np.clip(pit, 0, 1)
        rough = np.clip(0.48 + 0.22 * patina + 0.3 * np.clip(pit, 0, 1), 0.2, 1)
        occ = 0.7 + 0.3 * cavity(h, 2.0)
        metal = 1.0
    else:
        raise ValueError(f"no tile {kind}")
    gain = gain * (0.82 + 0.18 * occ)                            # a little of the cavity's shade in the colour
    lin = base[None, None, :] * gain[..., None]
    lin *= base / np.maximum(lin.reshape(-1, 3).mean(0), 1e-9)   # the mean stays the caller's colour
    orm = np.stack([occ, rough, np.full_like(occ, metal)], 2)
    return to_srgb(lin), normal_of(h), orm


# which tile a piece of the outfit takes (outfit_build.py's material names)
KIND = {"trousers": "linen", "vest": "wool", "belt": "leather", "flap": "leather", "boots": "leather",
        "bracer": "leather", "guards": "leather", "straps": "leather", "bracer_trim": "rawhide", "cuffs": "suede",
        "laces": "rawhide", "buckle": "iron", "studs": "iron"}


def main():
    from PIL import Image
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    samples = [("linen", "#3b352a"), ("wool", "#282523"), ("leather", "#3a2a1e"), ("rawhide", "#8a7a62"),
               ("suede", "#5a4330"), ("iron", "#5a5a58")]
    light = np.array([-0.5, -0.6, 0.62])
    light /= np.linalg.norm(light)
    row = []
    for kind, hexc in samples:
        col = np.array([int(hexc[i:i + 2], 16) / 255 for i in (1, 3, 5)])
        alb, nrm, orm = tile(kind, col)
        lit = to_srgb(to_lin(alb) * (0.25 + 1.6 * np.clip(nrm @ light, 0, None))[..., None] * orm[..., :1])
        crop = slice(0, 384)                                     # 13 cm of the tile at full size
        row.append(np.vstack([alb[crop, crop], lit[crop, crop], np.repeat(nrm[crop, crop, :1] * 0.5 + 0.5, 3, 2)]))
        slopes = np.degrees(np.arccos(np.clip(nrm[..., 2], -1, 1)))
        print(f"{kind:8} mean colour {to_srgb(to_lin(alb).reshape(-1, 3).mean(0)).round(3)}  slope mean {slopes.mean():4.1f}°"
              f" p99 {np.percentile(slopes, 99):4.1f}°  rough {orm[..., 1].mean():.2f}")
    Image.fromarray((np.hstack(row) * 255).round().astype(np.uint8)).save(out / "tiles.png")


if __name__ == "__main__":
    main()
