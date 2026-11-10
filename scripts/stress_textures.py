#!/usr/bin/env python3
"""Heavier copies of the base textures for the content stress measurement.

    scripts/stress_textures.py <base>/textures <out dir> --scale 2|4|8
    scripts/stress_textures.py --base <ready base> <out base> --scale 2|4|8

Every DXT1/DXT3/DXT5 texture with a mip chain gets `log2(scale)` new top levels: each 4x4 block becomes
four blocks with the same endpoints and its indices repeated 2x2. That is an exact nearest-neighbour
enlargement in the compressed domain, so nothing is re-encoded, the old top level stays as the next
mip and the picture is the same. Bytes grow about `scale^2` times.

The game reads the result with OPENGOTHIC_TEXTURES=<out dir> (reports/performance/2026-11-10-content-stress).
With --base the result is a whole ready base for OPENGOTHIC_BASE: the enlarged textures, hard links to
every other file of the source base and a rewritten file manifest; the identity and fingerprint stay.
Run such a base with its own OPENGOTHIC_BASE_CACHE: the derived caches are keyed by the fingerprint.
Textures without a mip chain (UI, fonts), other formats and sides that are not a multiple of 4 are
left out and counted: the game then reads their base file.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import sys
from pathlib import Path

import numpy as np

MANIFEST = "base-files.json"
MAGIC = b"DDS "
HEADER = 128
DDSD_MIPMAPCOUNT = 0x20000
DDSD_LINEARSIZE = 0x80000
DDSCAPS_COMPLEX = 0x8
DDSCAPS_MIPMAP = 0x400000
# fourcc -> (block bytes, index fields as (byte offset, byte length, bits per texel))
FORMATS = {
    b"DXT1": (8, ((4, 4, 2),)),
    b"DXT3": (16, ((0, 8, 4), (12, 4, 2))),
    b"DXT5": (16, ((2, 6, 3), (12, 4, 2))),
}


class Skip(Exception):
    """The texture is left to the base file; the message is the counted reason."""


def _unpack(field: np.ndarray, bits: int) -> np.ndarray:
    """(blocks, bytes) little-endian index field -> (blocks, 16) texel indices."""
    value = np.zeros(field.shape[0], dtype=np.uint64)
    for i in range(field.shape[1]):
        value |= field[:, i].astype(np.uint64) << np.uint64(8 * i)
    shifts = (np.arange(16, dtype=np.uint64) * np.uint64(bits))[None, :]
    return ((value[:, None] >> shifts) & np.uint64((1 << bits) - 1)).astype(np.uint8)


def _pack(texels: np.ndarray, bits: int, length: int) -> np.ndarray:
    """(blocks, 16) texel indices -> (blocks, length) little-endian bytes."""
    shifts = (np.arange(16, dtype=np.uint64) * np.uint64(bits))[None, :]
    value = (texels.astype(np.uint64) << shifts).sum(axis=1, dtype=np.uint64)
    out = np.empty((texels.shape[0], length), dtype=np.uint8)
    for i in range(length):
        out[:, i] = (value >> np.uint64(8 * i)) & np.uint64(0xFF)
    return out


def enlarge_level(data: bytes, width: int, height: int, fourcc: bytes) -> bytes:
    """One compressed level of width x height -> the level of 2*width x 2*height."""
    size, fields = FORMATS[fourcc]
    bw, bh = width // 4, height // 4
    blocks = np.frombuffer(data, dtype=np.uint8).reshape(bh, bw, size)
    # Endpoints and everything else that is per block: each block repeated 2x2.
    out = np.repeat(np.repeat(blocks, 2, axis=0), 2, axis=1).copy()
    flat = blocks.reshape(bh * bw, size)
    for offset, length, bits in fields:
        texels = _unpack(flat[:, offset:offset + length], bits)          # (blocks, 16), row-major 4x4
        image = texels.reshape(bh, bw, 4, 4).transpose(0, 2, 1, 3).reshape(bh * 4, bw * 4)
        image = np.repeat(np.repeat(image, 2, axis=0), 2, axis=1)
        texels = image.reshape(bh * 2, 4, bw * 2, 4).transpose(0, 2, 1, 3).reshape(bh * bw * 4, 16)
        out.reshape(bh * bw * 4, size)[:, offset:offset + length] = _pack(texels, bits, length)
    return out.tobytes()


def enlarge(dds: bytes, steps: int) -> bytes:
    """A DDS with a mip chain -> the same DDS with `steps` new top levels."""
    if len(dds) < HEADER or dds[:4] != MAGIC:
        raise Skip("not a DDS")
    header = bytearray(dds[:HEADER])
    height, width = struct.unpack_from("<II", header, 12)
    mips = struct.unpack_from("<I", header, 28)[0]
    fourcc = bytes(header[84:88])
    if fourcc not in FORMATS:
        raise Skip(f"format {fourcc!r}")
    if mips <= 1:
        raise Skip("no mip chain")
    if width % 4 or height % 4:
        raise Skip("side is not a multiple of 4")
    size = FORMATS[fourcc][0]
    top = (width // 4) * (height // 4) * size
    if len(dds) < HEADER + top:
        raise Skip("truncated")
    body = dds[HEADER:]
    for _ in range(steps):
        body = enlarge_level(body[:top], width, height, fourcc) + body
        width, height, top, mips = width * 2, height * 2, top * 4, mips + 1
    struct.pack_into("<I", header, 8, struct.unpack_from("<I", header, 8)[0] | DDSD_MIPMAPCOUNT | DDSD_LINEARSIZE)
    struct.pack_into("<II", header, 12, height, width)
    struct.pack_into("<I", header, 20, top)
    struct.pack_into("<I", header, 28, mips)
    struct.pack_into("<I", header, 108, struct.unpack_from("<I", header, 108)[0] | DDSCAPS_COMPLEX | DDSCAPS_MIPMAP)
    return bytes(header) + body


def run(source: Path, out: Path, scale: int) -> dict:
    steps = {2: 1, 4: 2, 8: 3}[scale]
    if not source.is_dir():
        raise SystemExit(f"stress-textures: no such folder: {source}")
    if out.exists() and any(out.iterdir()):
        raise SystemExit(f"stress-textures: output is not empty: {out}")
    if out.resolve() == source.resolve() or source.resolve() in out.resolve().parents:
        raise SystemExit("stress-textures: output must be outside the source folder")
    out.mkdir(parents=True, exist_ok=True)
    report = {"scale": scale, "source": str(source), "written": 0, "bytes_before": 0, "bytes_after": 0, "skipped": {}}
    for path in sorted(source.iterdir()):
        if path.name.startswith("._") or path.suffix.lower() != ".dds":
            continue
        data = path.read_bytes()
        try:
            result = enlarge(data, steps)
        except Skip as reason:
            key = str(reason)
            report["skipped"][key] = report["skipped"].get(key, 0) + 1
            continue
        (out / path.name).write_bytes(result)
        report["written"] += 1
        report["bytes_before"] += len(data)
        report["bytes_after"] += len(result)
    (out / "stress.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def _entry(path: str, size: int, sha: str) -> str:
    return f'{{"path":{json.dumps(path, ensure_ascii=False)},"size":{size},"sha256":"{sha}"}}'


def run_base(base: Path, out: Path, scale: int) -> dict:
    """A ready base -> the same base with enlarged textures; everything else is hard-linked."""
    steps = {2: 1, 4: 2, 8: 3}[scale]
    for name in ("base.json", MANIFEST):
        if not (base / name).is_file():
            raise SystemExit(f"stress-textures: not a ready base, no {name}: {base}")
    if out.exists() and any(out.iterdir()):
        raise SystemExit(f"stress-textures: output is not empty: {out}")
    target, source = out.resolve(), base.resolve()
    live = [Path.home() / ".cache/gothic-ai/base", Path.home() / ".local/share/gothic-ai"]
    for parent in [source, *(p.resolve() for p in live)]:
        if target == parent or parent in target.parents:
            raise SystemExit(f"stress-textures: output must be outside {parent}")
    manifest = json.loads((base / MANIFEST).read_text())
    report = {"scale": scale, "source": str(base), "written": 0, "bytes_before": 0, "bytes_after": 0, "skipped": {}}
    entries = {}
    for item in manifest["files"]:
        name, src, dst = item["path"], base / item["path"], out / item["path"]
        dst.parent.mkdir(parents=True, exist_ok=True)
        parts = name.split("/")
        if len(parts) == 2 and parts[0] == "textures" and name.lower().endswith(".dds"):
            data = src.read_bytes()
            try:
                result = enlarge(data, steps)
            except Skip as reason:
                report["skipped"][str(reason)] = report["skipped"].get(str(reason), 0) + 1
            else:
                dst.write_bytes(result)
                entries[name] = _entry(name, len(result), hashlib.sha256(result).hexdigest())
                report["written"] += 1
                report["bytes_before"] += len(data)
                report["bytes_after"] += len(result)
                continue
        try:
            os.link(src, dst)
        except OSError:          # another file system
            shutil.copy2(src, dst)
        entries[name] = _entry(name, item["size"], item["sha256"])
    note = out / "stress-textures.json"
    note.write_text(json.dumps(report, indent=2) + "\n")
    entries[note.name] = _entry(note.name, note.stat().st_size, _sha256(note))
    body = ",\n".join(entries[name] for name in sorted(entries))
    (out / MANIFEST).write_text('{"format":"gothic-base-files","version":1,"files":[\n' + body + "\n]}\n")
    identity = (base / "base.json").read_text()
    binding = '"manifest":' + _entry(MANIFEST, (out / MANIFEST).stat().st_size, _sha256(out / MANIFEST))
    identity, count = re.subn(r'"manifest":\{[^{}]*\}', lambda _: binding, identity)
    if count != 1:
        raise SystemExit("stress-textures: base.json has no single manifest binding")
    (out / "base.json").write_text(identity)
    return report


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("source", type=Path, help="<base>/textures, or a ready base with --base")
    parser.add_argument("out", type=Path, help="new folder for OPENGOTHIC_TEXTURES, or for OPENGOTHIC_BASE with --base")
    parser.add_argument("--scale", type=int, choices=(2, 4, 8), default=2, help="side multiplier")
    parser.add_argument("--base", action="store_true", help="write a whole ready base instead of a texture folder")
    args = parser.parse_args(argv)
    report = (run_base if args.base else run)(args.source, args.out, args.scale)
    mib = 1 << 20
    print(f"stress-textures: {report['written']} textures x{args.scale}: "
          f"{report['bytes_before'] / mib:.1f} -> {report['bytes_after'] / mib:.1f} MiB; skipped {report['skipped']}")
    if args.base:
        print(f"OPENGOTHIC_BASE={args.out.resolve()}")
        print(f"OPENGOTHIC_BASE_CACHE={args.out.resolve()}.cache")
    else:
        print(f"OPENGOTHIC_TEXTURES={args.out.resolve()}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
