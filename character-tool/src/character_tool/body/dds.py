"""Write the Gothic material DDS mip chain (BC1 colour and BC3 normals or cutouts)."""
from __future__ import annotations

import math
import struct
from pathlib import Path

import numpy as np


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
            from .alpha_mips import cutout_mip_chain
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
