#!/usr/bin/env python3
"""Inventory file-backed textures traced by OPENGOTHIC_LOADPROBE during NEWWORLD loading.

Run scripts/loadprobe.sh with the remaster content and pinned base, then pass its
runs/local-work-NEWWORLD-par/game.log. Paths come from successful Pixmap loads, so
HD/content/base precedence is resolved by the game rather than reimplemented.
The CSV contains one row per resolved file/GPU layout and preserves repeated
allocations as load_count. Source payloads and actual GPU upload layouts are
separate; skipping levels never synthesizes a tail for a one-level source.
"""
import argparse
import csv
import json
from pathlib import Path
import re


TRACE = re.compile(r"\[loadprobe-texture\] data=(\S+) w=(\d+) h=(\d+) mips=(\d+) "
                   r"format=(\S+) bytes=(\d+) path=(.*)$")
UPLOAD = re.compile(r"\[loadprobe-upload\] data=(\S+) w=(\d+) h=(\d+) mips=(\d+) "
                    r"format=(\S+) bytes=(\d+)$")
BLOCKS = {"DXT1": (4, 8), "DXT3": (4, 16), "DXT5": (4, 16)}
PIXELS = {"R8": 1, "RG8": 2, "RGB8": 3, "RGBA8": 4,
          "R16": 2, "RG16": 4, "RGB16": 6, "RGBA16": 8,
          "R32F": 4, "RG32F": 8, "RGB32F": 12, "RGBA32F": 16}


def mip_bytes(fmt, width, height):
    if fmt in BLOCKS:
        side, size = BLOCKS[fmt]
        return ((width + side - 1) // side) * ((height + side - 1) // side) * size
    return width * height * PIXELS[fmt]


def tail(width, height, levels, fmt, first_mip, min_side):
    """Match DDS-tail selection: clamp the starting level, retain its chain."""
    floor = max(min_side, BLOCKS.get(fmt, (1, 0))[0])
    selected = 0
    while selected < first_mip and selected + 1 < levels:
        if max(1, width >> (selected + 1)) < floor or max(1, height >> (selected + 1)) < floor:
            break
        selected += 1
    size = sum(mip_bytes(fmt, max(1, width >> i), max(1, height >> i))
               for i in range(selected, levels))
    return selected, size


def classify(path, roots, map_paths):
    for category, root in roots:
        if root is None:
            continue
        try:
            relative = path.relative_to(root)
        except ValueError:
            continue
        source = "material maps" if category == "content" and path in map_paths else category
        return source, f"{category}/{relative.as_posix()}"
    raise ValueError(f"Texture is outside the supplied source roots: {path}")


def inventory(args):
    roots = [(name, path.resolve() if path else None)
             for name, path in (("base", args.base), ("content", args.content), ("HD", args.hd))]
    map_paths = set()
    if args.content:
        materials = args.content / "materials"
        table = json.loads((materials / "materials.json").read_text())
        for entry in table.values():
            for role in ("normal", "orm"):
                if role in entry:
                    map_paths.add((materials / entry[role]).resolve())
    rows = {}
    pending = {}
    generated_count = generated_bytes = 0
    session = None
    for line in args.log.read_text().splitlines():
        if "[loadprobe] stage=session_end " in line:
            session = line[line.index("[loadprobe]"):]
            break
        match = TRACE.search(line)
        if match:
            pointer, width, height, levels, fmt, payload, raw_path = match.groups()
            pending[int(pointer, 16)] = (int(width), int(height), int(levels), fmt, int(payload), raw_path)
            continue
        match = UPLOAD.search(line)
        if not match:
            continue
        pointer, gpu_width, gpu_height, gpu_levels, gpu_fmt, gpu_bytes = match.groups()
        gpu_width, gpu_height, gpu_levels, gpu_bytes = map(int, (gpu_width, gpu_height, gpu_levels, gpu_bytes))
        decoded = pending.pop(int(pointer, 16), None)
        if decoded is None:
            generated_count += 1
            generated_bytes += gpu_bytes
            continue
        width, height, levels, fmt, payload, raw_path = decoded
        if not width or not height or not levels or levels > max(width, height).bit_length():
            raise ValueError(f"Invalid texture dimensions or mip count: {raw_path}")
        if width != gpu_width or height != gpu_height:
            raise ValueError(f"Upload dimensions differ from the traced source: {raw_path}")
        path = Path(raw_path).resolve()
        source, public_path = classify(path, roots, map_paths)
        row = {"source": source, "path": public_path, "format": fmt, "width": width,
               "height": height, "mip_levels": levels, "load_count": 1,
               "chain": "none" if levels == 1 else
                        "complete" if levels == max(width, height).bit_length() else "partial",
               "source_bytes": payload, "gpu_format": gpu_fmt, "gpu_mip_levels": gpu_levels,
               "full_bytes": gpu_bytes}
        if tail(width, height, levels, fmt, 0, args.min_side)[1] != payload:
            raise ValueError(f"Decoded payload differs from mip layout: {raw_path}")
        if tail(width, height, gpu_levels, gpu_fmt, 0, args.min_side)[1] != gpu_bytes:
            raise ValueError(f"GPU upload bytes differ from mip layout: {raw_path}")
        for skip in (1, 2, 3):
            selected = tail(width, height, levels, fmt, skip, args.min_side)[0]
            size = sum(mip_bytes(gpu_fmt, max(1, width >> i), max(1, height >> i))
                       for i in range(selected, gpu_levels))
            row[f"skip{skip}_first_mip"] = selected
            row[f"skip{skip}_bytes"] = size
        key = (path, gpu_fmt, gpu_levels)
        if key in rows:
            previous = rows[key]
            row["load_count"] = previous["load_count"] + 1
            if any(previous[key] != value for key, value in row.items() if key != "load_count"):
                raise ValueError(f"Texture changed during the session: {raw_path}")
        rows[key] = row
    if session is None or not rows:
        raise ValueError("No complete session_end with file-texture traces found")
    if pending:
        raise ValueError(f"{len(pending)} successful file decodes have no matching GPU upload")
    return sorted(rows.values(), key=lambda row: (row["source"], row["path"], row["gpu_format"])), session, generated_count, generated_bytes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--base", type=Path, required=True, help="Selected base root, containing textures/")
    parser.add_argument("--content", type=Path, help="Active OPENGOTHIC_CONTENT root")
    parser.add_argument("--hd", type=Path, help="Active OPENGOTHIC_TEXTURES directory")
    parser.add_argument("--csv", type=Path, required=True)
    parser.add_argument("--min-side", type=int, default=64)
    args = parser.parse_args()
    if not args.csv.is_absolute() or args.min_side < 1:
        parser.error("--csv must be absolute and --min-side must be positive")
    rows, session, generated_count, generated_bytes = inventory(args)
    args.csv.parent.mkdir(parents=True, exist_ok=True)
    with args.csv.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    print(f"Files: {len(rows)}; successful file loads: {sum(row['load_count'] for row in rows)}; min side: {args.min_side}")
    print("GPU payload bytes; source chain: complete ends at 1x1, partial has >1 level, none has one level.")
    print("Source | Files | Loads | Full bytes | Complete bytes (%) | Partial bytes (%) | No-chain bytes (%) | Skip 1 | Skip 2 | Skip 3")
    for source in ("base", "content", "HD", "material maps", "TOTAL"):
        selected = [row for row in rows if source == "TOTAL" or row["source"] == source]
        total = sum(row["full_bytes"] * row["load_count"] for row in selected)
        cells = [source, str(len(selected)), str(sum(row["load_count"] for row in selected)), str(total)]
        for chain in ("complete", "partial", "none"):
            size = sum(row["full_bytes"] * row["load_count"] for row in selected if row["chain"] == chain)
            cells.append(f"{size} ({100 * size / total if total else 0:.2f}%)")
        for skip in (1, 2, 3):
            cells.append(str(sum(row[f"skip{skip}_bytes"] * row["load_count"] for row in selected)))
        print(" | ".join(cells))
    print(f"Unique file/layout GPU bytes: {sum(row['full_bytes'] for row in rows)}")
    print(f"Decoded source payload bytes, weighted by loads: {sum(row['source_bytes'] * row['load_count'] for row in rows)}")
    print(f"Uploads without a file source (excluded): {generated_count}; GPU bytes: {generated_bytes}")
    print(session)
    print(f"CSV: {args.csv}")


if __name__ == "__main__":
    main()
