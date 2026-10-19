#!/usr/bin/env python3
"""Pack generated PNG maps for the two city-view material comparisons.

    uv run --no-project --with numpy --with pillow python \
        scripts/content/pack_materials.py --registry data/content/registry.jsonl \
        --content-root ~/.local/share/gothic-ai/content \
        --output /tmp/material-capture-content

Each resource gets a separate OPENGOTHIC_CONTENT root. Its materials.json
replaces exactly one fixture texture, leaving the other city material original.
The DDS compressor and mip generation are shared with test-material.py:
opaque albedo and ORM are DXT1; normals are DXT5nm (X alpha, Y green).
Input normals are XYZ encoded in [0, 1], with the generator's Y convention
is OpenGL (+Y toward the image top). Packing flips green into the engine's
positive texture-V basis, matching the synthetic fixture's height gradient. No game pixels are read.
Negative-Z normals are projected into the upper hemisphere only while packing;
the generated PNG maps stay unchanged. Each resource logs its projected pixel count.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import math
from pathlib import Path
import re
import sys

import numpy as np
from PIL import Image


TARGETS = {
    "cobblestone": "NW_MISC_OLDSTONEPATH_01.TGA",
    "stone-wall": "NW_CITY_WALL_03.TGA",
}
PIPELINES = ("flux-marigold", "flux-filter", "sdxl-marigold", "sdxl-filter", "stablematerials")
MAPS = ("albedo", "normal", "ao", "roughness", "metallic")
NORMAL_Z_EPSILON = 1e-6


def load_compressor():
    path = Path(__file__).with_name("test-material.py")
    spec = importlib.util.spec_from_file_location("material_fixture", path)
    if spec is None or spec.loader is None:
        raise ValueError(f"cannot load DDS compressor from {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.write_dds


def map_path(root: Path, value: str) -> Path:
    if not isinstance(value, str) or not value:
        raise ValueError("map path must be a nonempty relative string")
    relative = Path(value)
    if relative.is_absolute() or ".." in relative.parts:
        raise ValueError(f"map path must stay inside content root: {value!r}")
    resolved = (root / relative).resolve()
    if not resolved.is_relative_to(root):
        raise ValueError(f"map symlink escapes content root: {value!r}")
    if resolved.suffix.lower() != ".png":
        raise ValueError(f"expected generated PNG map: {value!r}")
    return resolved


def read_maps(record: dict, content_root: Path) -> dict[str, np.ndarray]:
    files = record.get("files", {})
    if not isinstance(files, dict):
        raise ValueError(f"{record['id']}: files must map channel names to paths")
    maps = {}
    for name in MAPS:
        if name not in files:
            raise ValueError(f"{record['id']}: missing {name} map")
        with Image.open(map_path(content_root, files[name])) as source:
            source.load()
            array = np.asarray(source.convert("RGB" if name in ("albedo", "normal") else "L"), dtype=np.float32) / 255
        maps[name] = array
    h, w = maps["albedo"].shape[:2]
    if h != w or not 16 <= h <= 2048 or h & (h - 1):
        raise ValueError(f"{record['id']}: map size {w}x{h} must be square power of two, 16..2048")
    if any(value.shape[:2] != (h, w) for value in maps.values()):
        raise ValueError(f"{record['id']}: all maps must have the same size")
    normal = maps["normal"] * 2 - 1
    normal[:, :, 1] *= -1  # PNG +Y up -> engine bitangent follows texture V down.
    negative_z = normal[:, :, 2] < 0
    clamped_pixels = int(np.count_nonzero(negative_z))
    normal[:, :, 2][negative_z] = NORMAL_Z_EPSILON
    normal /= np.maximum(np.linalg.norm(normal, axis=2, keepdims=True), 1e-8)
    print(f"{record['id']}: normal_z_clamped={clamped_pixels}/{h * w} "
          f"epsilon={NORMAL_Z_EPSILON:g}")
    maps["normal"] = normal
    return maps


def pack(record: dict, content_root: Path, output: Path, scale: float) -> Path:
    resource_id = record.get("id", "")
    if not isinstance(resource_id, str) or re.fullmatch(r"[a-z0-9][a-z0-9_-]*", resource_id) is None:
        raise ValueError(f"unsafe resource id: {resource_id!r}")
    maps = read_maps(record, content_root)
    content = output / resource_id
    directory = content / "materials" / resource_id
    directory.mkdir(parents=True, exist_ok=True)
    write_dds = load_compressor()
    write_dds(directory / "albedo.dds", maps["albedo"])
    write_dds(directory / "normal.dds", maps["normal"], normal=True)
    write_dds(directory / "orm.dds", np.stack((maps["ao"], maps["roughness"], maps["metallic"]), axis=2))
    texture = TARGETS[record["parameters"]["material"]]
    entry = {name: f"{resource_id}/{name}.dds" for name in ("albedo", "normal", "orm")}
    entry["scale"] = scale
    (content / "materials" / "materials.json").write_text(json.dumps({texture: entry}, indent=2) + "\n")
    return content


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--registry", type=Path, required=True)
    parser.add_argument("--content-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--id", action="append", help="capture only these resource IDs (repeatable)")
    parser.add_argument("--scale", type=float, default=1.0, help="fixed UV scale for every comparison (default: 1)")
    args = parser.parse_args()
    if not math.isfinite(args.scale) or args.scale <= 0:
        parser.error("--scale must be finite and positive")
    try:
        content_root = args.content_root.expanduser().resolve()
        output = args.output.expanduser().resolve()
        records = [json.loads(line) for line in args.registry.read_text().splitlines() if line.strip()]
        selected = [record for record in records
                    if record.get("kind") == "material"
                    and record.get("parameters", {}).get("material") in TARGETS
                    and record.get("pipeline") in PIPELINES
                    and (args.id is None or record.get("id") in args.id)]
        ids = [record["id"] for record in selected]
        if len(ids) != len(set(ids)):
            raise ValueError("duplicate selected resource id")
        if args.id and set(args.id) - set(ids):
            raise ValueError(f"unknown city material IDs: {', '.join(sorted(set(args.id) - set(ids)))}")
        if not selected:
            raise ValueError("registry has no selected city materials")
        for record in selected:
            reason = record.get("skip_reason")
            if reason or record.get("generation", {}).get("outcome") == "skipped":
                print(f"skip {record['id']}: {reason or 'generation skipped'}", file=sys.stderr)
                continue
            content = pack(record, content_root, output, args.scale)
            print(content)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"pack_materials: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
