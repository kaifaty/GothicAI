#!/usr/bin/env python3
"""Generate a synthetic, seamless PBR fixture; no game assets or models are used.

    uv run --no-project --with numpy python scripts/content/test-material.py \
        --output /tmp/gothic-pbr-content --albedo
    OPENGOTHIC_CONTENT=/tmp/gothic-pbr-content <render-bench runner>

The defaults replace the pavement beside the lamp in rf.bench's city view and
one city stone wall. --textures accepts other original Gothic texture names.
Roughness sweeps from polished to matte, with a warm metallic band between them.
Albedo is optional: without --albedo the original Gothic colour is retained.
For separate normal/AO comparisons generate otherwise identical directories
with --flat-normal or --no-ao. Removing the directory, or OPENGOTHIC_CONTENT=,
restores the original materials. Output is a local fixture, never game content
to commit. Only numpy and the Python standard library are required, including
for the DDS compressor. DXT5nm stores X in alpha, Y in green, positive Z implicit.

Antitiling fixture (always includes an albedo with one conspicuous dark spot):
    uv run --no-project --with numpy python scripts/content/test-material.py \
        --output /tmp/gothic-antitile-rotate --pattern antitile --antitile rotate
Its asymmetric raised spot makes normal-map rotation visible under sun or lamp.
For lighting-only controls use --neutral-albedo --no-ao --strength 0.16, then
repeat with --flat-normal; the albedo and ORM stay identical between the pair.
Use identical --scale/--antitile-cell in off, offset and rotate directories.

Screenshot measurements additionally require Pillow. Choose one planar surface
crop and the tile period in screen pixels from the off frame; reuse both for
every mode. The result is the largest Pearson luminance correlation in the
specified lag window (inclusive). PSNR compares full RGB frames unless cropped:
    uv run --no-project --with numpy --with Pillow python scripts/content/test-material.py \
        --autocorrelation off.png --crop 100,200,400,200 --period 60 --lag-radius 3 \
        --cross-period -3 --cross-radius 1
--cross-period shifts the other screen axis, preserving a perspective tile's
two-dimensional period without resampling the image; negative shifts are valid.
    uv run --no-project --with numpy --with Pillow python scripts/content/test-material.py \
        --psnr current.png --reference baseline.png
"""

from __future__ import annotations

import argparse
import json
import math
import struct
from pathlib import Path

import numpy as np


DEFAULT_TEXTURES = ("NW_MISC_OLDSTONEPATH_01.TGA", "NW_CITY_WALL_03.TGA")


def smoothstep(value: np.ndarray) -> np.ndarray:
    value = np.clip(value, 0, 1)
    return value * value * (3 - 2 * value)


def tile(size: int, strength: float) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Four staggered rows; all detail repeats exactly at the UV boundaries."""
    y, x = np.mgrid[:size, :size].astype(np.float32)
    u, v = (x + 0.5) / size, (y + 0.5) / size
    row = np.floor(v * 4)
    bx = (u * 4 + (row % 2) * 0.5) % 1
    by = (v * 4) % 1
    edge = np.minimum(np.minimum(bx, 1 - bx), np.minimum(by, 1 - by))
    height = smoothstep((edge - 0.035) / 0.12)
    # Periodic analytical grain, small enough that the broad bevel remains clear.
    grain = np.sin(2 * np.pi * u * 31) * np.sin(2 * np.pi * v * 29)
    height += 0.008 * grain * height
    du = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * size * 0.5
    dv = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * size * 0.5
    normal = np.stack((-du * strength, -dv * strength, np.ones_like(du)), axis=2)
    normal /= np.linalg.norm(normal, axis=2, keepdims=True)

    # Colour contains no baked normal lighting or AO: controls isolate the maps.
    tone = 0.67 + 0.035 * grain
    albedo = np.stack((tone * 0.96, tone * 0.90, tone * 0.80), axis=2)
    ao = 0.22 + 0.78 * smoothstep((edge - 0.025) / 0.19)
    # Both ends of the UV tile are matte; its centre is polished. The broad
    # sweep survives mipmaps and gives sun, lamp and sky reflections room.
    matte = smoothstep((0.5 + 0.5 * np.cos(2 * np.pi * u) - 0.2) / 0.6)
    roughness = 0.10 + 0.88 * matte
    metal = smoothstep((0.07 - np.abs(u - 0.375)) / 0.012)
    albedo = albedo * (1 - metal[:, :, None]) + np.array((0.83, 0.59, 0.28)) * metal[:, :, None]
    orm = np.stack((ao, roughness, metal), axis=2)
    return albedo, normal, orm


def antitile_spot(size: int, strength: float) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """One periodic dark spot with an asymmetric ridge to inspect UV rotation."""
    y, x = np.mgrid[:size, :size].astype(np.float32)
    u, v = (x + 0.5) / size, (y + 0.5) / size
    dx, dy = (u - 0.31 + 0.5) % 1 - 0.5, (v - 0.43 + 0.5) % 1 - 0.5
    radius = np.sqrt((dx / 0.115) ** 2 + (dy / 0.095) ** 2)
    spot = 1 - smoothstep((radius - 0.65) / 0.55)
    grain = np.sin(2 * np.pi * u * 23) * np.sin(2 * np.pi * v * 19)
    tone = 0.82 - 0.63 * spot + 0.012 * grain
    albedo = tone[:, :, None] * np.array((1.0, 0.92, 0.79), dtype=np.float32)

    # A broad bump with a displaced narrow crest is deliberately asymmetric:
    # rotating the sampled UV and tangent normal together must rotate its light.
    height = np.exp(-0.5 * ((dx / 0.10) ** 2 + (dy / 0.07) ** 2))
    height += 0.55 * np.exp(-0.5 * (((dx - 0.035) / 0.025) ** 2 + ((dy + 0.015) / 0.07) ** 2))
    du = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * size * 0.5
    dv = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * size * 0.5
    normal = np.stack((-du * strength, -dv * strength, np.ones_like(du)), axis=2)
    normal /= np.linalg.norm(normal, axis=2, keepdims=True)
    orm = np.stack((1 - 0.20 * spot, 0.70 + 0.20 * spot, np.zeros_like(spot)), axis=2)
    return albedo, normal, orm


def image_rgb(path: Path, crop: str | None) -> np.ndarray:
    try:
        from PIL import Image
    except ImportError as error:
        raise ValueError("screenshot measurements require Pillow (--with Pillow)") from error
    with Image.open(path) as image:
        rgb = np.asarray(image.convert("RGB"), dtype=np.float64) / 255
    if crop is not None:
        try:
            x, y, width, height = (int(value) for value in crop.split(","))
        except ValueError as error:
            raise ValueError("--crop must be x,y,width,height in pixels") from error
        if x < 0 or y < 0 or width <= 0 or height <= 0 or x + width > rgb.shape[1] or y + height > rgb.shape[0]:
            raise ValueError("--crop must lie inside the image and have positive size")
        rgb = rgb[y:y + height, x:x + width]
    return rgb


def autocorrelation(rgb: np.ndarray, period: int, radius: int, axis: str,
                    cross_period: int = 0, cross_radius: int = 0) -> dict:
    # Rec. 709 luminance of screenshot code values; no exposure normalization or
    # filtering that could change the measured period between fixture modes.
    luminance = rgb @ np.array((0.2126, 0.7152, 0.0722))
    dimension = 1 if axis == "x" else 0
    if radius < 0 or cross_radius < 0:
        raise ValueError("lag radii must be non-negative")
    if period - radius < 1 or period + radius >= luminance.shape[dimension]:
        raise ValueError("period +/- lag radius must be positive and smaller than the crop along --axis")
    if abs(cross_period) + cross_radius >= luminance.shape[1 - dimension]:
        raise ValueError("cross-period +/- cross radius must leave an overlap along the other axis")

    def overlap(length: int, delta: int) -> tuple[slice, slice]:
        return slice(max(0, -delta), min(length, length - delta)), slice(max(0, delta), min(length, length + delta))

    values = []
    for lag in range(period - radius, period + radius + 1):
        for cross_lag in range(cross_period - cross_radius, cross_period + cross_radius + 1):
            dx, dy = (lag, cross_lag) if axis == "x" else (cross_lag, lag)
            ax, bx = overlap(luminance.shape[1], dx)
            ay, by = overlap(luminance.shape[0], dy)
            a, b = luminance[ay, ax], luminance[by, bx]
            a, b = a - a.mean(), b - b.mean()
            denominator = float(np.linalg.norm(a) * np.linalg.norm(b))
            if denominator <= 1e-12:
                raise ValueError("crop has no luminance variation at a requested lag")
            values.append({"lag": lag, "cross_lag": cross_lag, "correlation": float(np.sum(a * b) / denominator)})
    peak = max(values, key=lambda value: value["correlation"])
    return {"axis": axis, "period": period, "lag_radius": radius,
            "cross_period": cross_period, "cross_radius": cross_radius, "peak": peak, "samples": values}


def quantize(image: np.ndarray) -> np.ndarray:
    return np.rint(np.clip(image, 0, 1) * 255).astype(np.uint8)


def blocks(image: np.ndarray) -> np.ndarray:
    """DDS blocks are 4x4; final 2x2 and 1x1 mip levels repeat their edge."""
    h, w, channels = image.shape
    image = np.pad(image, ((0, (-h) % 4), (0, (-w) % 4), (0, 0)), mode="edge")
    return image.reshape((h + 3) // 4, 4, (w + 3) // 4, 4, channels).transpose(0, 2, 1, 3, 4).reshape(-1, 16, channels)


def rgb565(rgb: np.ndarray) -> np.ndarray:
    value = np.rint(rgb * np.array((31, 63, 31)) / 255).astype(np.uint16)
    return (value[:, 0] << 11) | (value[:, 1] << 5) | value[:, 2]


def from565(value: np.ndarray) -> np.ndarray:
    r, g, b = (value >> 11) & 31, (value >> 5) & 63, value & 31
    return np.stack(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)), axis=1).astype(np.int32)


def bc1(data: np.ndarray) -> np.ndarray:
    """A small vectorised range fit, including anticorrelated ORM channels."""
    rgb = data[:, :, :3].astype(np.float32)
    centred = rgb - rgb.mean(axis=1, keepdims=True)
    # Fit along the principal colour direction instead of independent minima:
    # AO and roughness vary in opposite directions in the mortar.
    covariance = np.einsum("npi,npj->nij", centred, centred)
    # The largest variance channel is a stable seed even for anticorrelation.
    direction = np.eye(3)[np.argmax(np.diagonal(covariance, axis1=1, axis2=2), axis=1)]
    for _ in range(6):
        direction = np.einsum("nij,nj->ni", covariance, direction)
        direction /= np.maximum(np.linalg.norm(direction, axis=1, keepdims=True), 1e-8)
    projection = np.einsum("npi,ni->np", centred, direction)
    indices = np.arange(len(rgb))
    lo = rgb565(rgb[indices, projection.argmin(axis=1)])
    hi = rgb565(rgb[indices, projection.argmax(axis=1)])
    c0, c1 = np.maximum(lo, hi), np.minimum(lo, hi)
    # Always use four-colour BC1 mode, including uniform black/white blocks.
    equal = c0 == c1
    c0 = np.where(equal & (c0 < 65535), c0 + 1, c0).astype(np.uint16)
    c1 = np.where(equal & (c1 == 65535), np.maximum(c1.astype(np.int32) - 1, 0), c1).astype(np.uint16)
    p0, p1 = from565(c0), from565(c1)
    palette = np.stack((p0, p1, (2 * p0 + p1) // 3, (p0 + 2 * p1) // 3), axis=1)
    distance = ((rgb[:, :, None, :] - palette[:, None, :, :]) ** 2).sum(axis=3)
    selector = distance.argmin(axis=2).astype(np.uint32)
    packed = np.sum(selector << (2 * np.arange(16, dtype=np.uint32)), axis=1, dtype=np.uint32)
    out = np.empty((len(rgb), 8), dtype=np.uint8)
    out[:, :2] = c0.astype("<u2").view(np.uint8).reshape(-1, 2)
    out[:, 2:4] = c1.astype("<u2").view(np.uint8).reshape(-1, 2)
    out[:, 4:] = packed.astype("<u4").view(np.uint8).reshape(-1, 4)
    return out


def bc3(data: np.ndarray) -> np.ndarray:
    alpha = data[:, :, 3].astype(np.int32)
    a0, a1 = alpha.max(axis=1), alpha.min(axis=1)
    palette = np.stack([a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)], axis=1)
    selector = np.abs(alpha[:, :, None] - palette[:, None, :]).argmin(axis=2).astype(np.uint64)
    packed = np.sum(selector << (3 * np.arange(16, dtype=np.uint64)), axis=1, dtype=np.uint64)
    out = np.empty((len(data), 16), dtype=np.uint8)
    out[:, 0], out[:, 1] = a0, a1
    out[:, 2:8] = packed.astype("<u8").view(np.uint8).reshape(-1, 8)[:, :6]
    out[:, 8:] = bc1(data)
    return out


def downsample(image: np.ndarray) -> np.ndarray:
    h, w, channels = image.shape
    return image.reshape(h // 2, 2, w // 2, 2, channels).mean(axis=(1, 3))


def write_dds(path: Path, image: np.ndarray, *, normal: bool = False) -> None:
    size = image.shape[0]
    levels = int(math.log2(size)) + 1
    # DXT5 makes the engine choose alpha-test even for an opaque texture.
    # Decide on actual holes at its cutoff, after hardening, not on PNG mode.
    cutout = not normal and image.shape[2] == 4 and np.any(image[..., 3] < 0.5)
    fourcc, block_bytes = (b"DXT5", 16) if normal or cutout else (b"DXT1", 8)
    header = [124, 0xA1007, size, size, (size // 4) ** 2 * block_bytes, 0, levels]
    header += [0] * 11
    header += [32, 4, int.from_bytes(fourcc, "little"), 0, 0, 0, 0, 0]
    header += [0x401008, 0, 0, 0, 0]
    with path.open("wb") as output:
        output.write(b"DDS " + struct.pack("<31I", *header))
        if cutout:
            from alpha_mips import cutout_mip_chain
            for mip in cutout_mip_chain(image):
                output.write(bc3(blocks(quantize(mip))).tobytes())
            return
        for level in range(levels):
            if normal:
                n = image / np.maximum(np.linalg.norm(image, axis=2, keepdims=True), 1e-8)
                encoded = np.ones((*image.shape[:2], 4), dtype=np.float32)
                encoded[:, :, 1] = n[:, :, 1] * 0.5 + 0.5
                encoded[:, :, 3] = n[:, :, 0] * 0.5 + 0.5
                output.write(bc3(blocks(quantize(encoded))).tobytes())
            else:
                output.write(bc1(blocks(quantize(image))).tobytes())
            if level + 1 < levels:
                image = downsample(image)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, default=Path.home() / ".local/share/gothic-ai/content", help="OPENGOTHIC_CONTENT root (default: %(default)s)")
    parser.add_argument("--textures", nargs="+", default=DEFAULT_TEXTURES, help="original Gothic texture names; default: %(default)s")
    parser.add_argument("--size", type=int, default=256, help="square power-of-two map size (default: %(default)s)")
    parser.add_argument("--scale", type=float, default=1.0, help="UV multiplier stored in materials.json")
    parser.add_argument("--strength", type=float, default=0.045, help="normal relief depth in UV units")
    parser.add_argument("--pattern", choices=("tiles", "antitile"), default="tiles", help="synthetic pattern (default: %(default)s)")
    parser.add_argument("--antitile", choices=("off", "offset", "rotate"), help="antitiling mode stored in materials.json; absent keeps the legacy off behavior")
    parser.add_argument("--antitile-cell", type=float, default=1.0, help="antitiling cell size in texture tiles (default: %(default)s)")
    parser.add_argument("--albedo", action="store_true", help="also replace the original colour with neutral synthetic tiles")
    parser.add_argument("--flat-normal", action="store_true", help="neutral normal map for the relief comparison")
    parser.add_argument("--neutral-albedo", action="store_true", help="constant grey albedo to inspect relief lighting without colour spots")
    parser.add_argument("--no-ao", action="store_true", help="white AO for the occlusion comparison")
    measurement = parser.add_mutually_exclusive_group()
    measurement.add_argument("--autocorrelation", type=Path, metavar="FRAME", help="measure screenshot luminance autocorrelation instead of generating maps")
    measurement.add_argument("--psnr", type=Path, metavar="FRAME", help="measure RGB PSNR against --reference instead of generating maps")
    parser.add_argument("--reference", type=Path, metavar="FRAME", help="baseline screenshot for --psnr")
    parser.add_argument("--crop", help="measurement rectangle x,y,width,height in screen pixels")
    parser.add_argument("--period", type=int, help="off-frame tile period in screen pixels for --autocorrelation")
    parser.add_argument("--lag-radius", type=int, default=0, help="search radius around the tile period in pixels (default: %(default)s)")
    parser.add_argument("--cross-period", type=int, default=0, help="signed tile-period shift along the other screen axis (default: %(default)s)")
    parser.add_argument("--cross-radius", type=int, default=0, help="search radius around --cross-period in pixels (default: %(default)s)")
    parser.add_argument("--axis", choices=("x", "y"), default="x", help="autocorrelation direction (default: %(default)s)")
    args = parser.parse_args()
    if args.autocorrelation or args.psnr:
        try:
            rgb = image_rgb(args.autocorrelation or args.psnr, args.crop)
            if args.autocorrelation:
                if args.period is None:
                    parser.error("--autocorrelation requires --period")
                result = autocorrelation(rgb, args.period, args.lag_radius, args.axis, args.cross_period, args.cross_radius)
                result["frame"] = str(args.autocorrelation)
            else:
                if args.reference is None:
                    parser.error("--psnr requires --reference")
                reference = image_rgb(args.reference, args.crop)
                if reference.shape != rgb.shape:
                    parser.error("PSNR frames must have the same dimensions")
                mse = float(np.mean((rgb - reference) ** 2))
                result = {"frame": str(args.psnr), "reference": str(args.reference), "mse": mse,
                          "psnr_db": 10 * math.log10(1 / mse) if mse > 0 else "infinity"}
            result["crop"] = args.crop
            print(json.dumps(result, indent=2))
        except (OSError, ValueError) as error:
            parser.error(str(error))
        return
    if not 16 <= args.size <= 2048 or args.size & (args.size - 1):
        parser.error("--size must be a power of two from 16 through 2048")
    if not math.isfinite(args.scale) or args.scale <= 0:
        parser.error("--scale must be finite and greater than zero")
    if not math.isfinite(args.strength) or args.strength < 0:
        parser.error("--strength must be finite and non-negative")
    if not math.isfinite(args.antitile_cell) or args.antitile_cell <= 0:
        parser.error("--antitile-cell must be finite and greater than zero")

    material_dir = args.output.expanduser().resolve() / "materials"
    fixture_dir = material_dir / "test-material"
    index_path = material_dir / "materials.json"
    # Preserve unrelated entries when adding this fixture to an existing root.
    index = json.loads(index_path.read_text()) if index_path.exists() else {}
    if not isinstance(index, dict):
        parser.error(f"{index_path} must contain a JSON object")
    pattern = antitile_spot if args.pattern == "antitile" else tile
    albedo, normal, orm = pattern(args.size, args.strength)
    if args.neutral_albedo:
        albedo[:, :, :] = 0.6
    if args.flat_normal:
        normal[:, :, :] = (0, 0, 1)
    if args.no_ao:
        orm[:, :, 0] = 1
    fixture_dir.mkdir(parents=True, exist_ok=True)
    write_dds(fixture_dir / "tile-normal.dds", normal, normal=True)
    write_dds(fixture_dir / "tile-orm.dds", orm)
    entry = {"normal": "test-material/tile-normal.dds", "orm": "test-material/tile-orm.dds", "scale": args.scale}
    if args.antitile is not None or args.pattern == "antitile":
        entry.update(antitile=args.antitile or "off", antitile_cell=args.antitile_cell)
    if args.albedo or args.pattern == "antitile" or args.neutral_albedo:
        write_dds(fixture_dir / "tile-albedo.dds", albedo)
        entry["albedo"] = "test-material/tile-albedo.dds"
    for name in args.textures:
        index[name.upper()] = entry.copy()
    index_path.write_text(json.dumps(index, indent=2, ensure_ascii=False) + "\n")
    print(f"Wrote {args.size}x{args.size} DDS maps with mipmaps to {fixture_dir}")
    print(f"Materials: {', '.join(args.textures)}")
    print(f"OPENGOTHIC_CONTENT={material_dir.parent}")


if __name__ == "__main__":
    main()
