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
    fourcc, block_bytes = (b"DXT5", 16) if normal else (b"DXT1", 8)
    header = [124, 0xA1007, size, size, (size // 4) ** 2 * block_bytes, 0, levels]
    header += [0] * 11
    header += [32, 4, int.from_bytes(fourcc, "little"), 0, 0, 0, 0, 0]
    header += [0x401008, 0, 0, 0, 0]
    with path.open("wb") as output:
        output.write(b"DDS " + struct.pack("<31I", *header))
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
    parser.add_argument("--albedo", action="store_true", help="also replace the original colour with neutral synthetic tiles")
    parser.add_argument("--flat-normal", action="store_true", help="neutral normal map for the relief comparison")
    parser.add_argument("--no-ao", action="store_true", help="white AO for the occlusion comparison")
    args = parser.parse_args()
    if not 16 <= args.size <= 2048 or args.size & (args.size - 1):
        parser.error("--size must be a power of two from 16 through 2048")
    if not math.isfinite(args.scale) or args.scale <= 0:
        parser.error("--scale must be finite and greater than zero")
    if not math.isfinite(args.strength) or args.strength < 0:
        parser.error("--strength must be finite and non-negative")

    material_dir = args.output.expanduser().resolve() / "materials"
    fixture_dir = material_dir / "test-material"
    index_path = material_dir / "materials.json"
    # Preserve unrelated entries when adding this fixture to an existing root.
    index = json.loads(index_path.read_text()) if index_path.exists() else {}
    if not isinstance(index, dict):
        parser.error(f"{index_path} must contain a JSON object")
    albedo, normal, orm = tile(args.size, args.strength)
    if args.flat_normal:
        normal[:, :, :] = (0, 0, 1)
    if args.no_ao:
        orm[:, :, 0] = 1
    fixture_dir.mkdir(parents=True, exist_ok=True)
    write_dds(fixture_dir / "tile-normal.dds", normal, normal=True)
    write_dds(fixture_dir / "tile-orm.dds", orm)
    entry = {"normal": "test-material/tile-normal.dds", "orm": "test-material/tile-orm.dds", "scale": args.scale}
    if args.albedo:
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
