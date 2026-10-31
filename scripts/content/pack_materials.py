#!/usr/bin/env python3
"""Pack generated PNG maps for the bakeoff or a complete staging material set.

    uv run --no-project --with numpy --with pillow --with scipy python \
        scripts/content/pack_materials.py --registry data/content/registry.jsonl \
        --content-root ~/.local/share/gothic-ai/content \
        --output /tmp/material-capture-content

Each resource gets a separate OPENGOTHIC_CONTENT root. Its materials.json
replaces exactly one fixture texture, leaving the other city material original.
The DDS compressor and mip generation are shared with test-material.py:
opaque albedo and ORM are DXT1; cutout albedo is DXT5 with hard alpha and
coverage-preserving mips; normals are DXT5nm (X alpha, Y green).
Input normals are XYZ encoded in [0, 1], with the generator's Y convention
is OpenGL (+Y toward the image top). Packing flips green into the engine's
positive texture-V basis, matching the synthetic fixture's height gradient. No game pixels are read.
Negative-Z normals are projected into the upper hemisphere only while packing;
the generated PNG maps stay unchanged. Each resource logs its projected pixel count.

For a full set, use ``--set city-1 --mapping data/content/city-materials.csv``
and ``--output ~/.local/share/gothic-ai/content-staging/city-1``. The output
is one OPENGOTHIC_CONTENT root, containing every include texture at its mapped
UV scale. This command never writes into the default live content directory.
"""

from __future__ import annotations

import argparse
import csv
import importlib.util
import json
import math
from pathlib import Path
import re
import sys

import numpy as np
from PIL import Image

from pilot import antitile_mode, CITY_3_SET, PLACEMENT_SETS, selected_rows


TARGETS = {
    "cobblestone": "NW_MISC_OLDSTONEPATH_01.TGA",
    "stone-wall": "NW_CITY_WALL_03.TGA",
}
PIPELINES = ("flux-marigold", "flux-filter", "imagegen-filter", "sdxl-marigold", "sdxl-filter", "stablematerials")
MAPS = ("albedo", "normal", "ao", "roughness", "metallic")
NORMAL_Z_EPSILON = 1e-6
DEFAULT_MAPPING = Path(__file__).resolve().parents[2] / "data/content/city-materials.csv"


def staging_output(path: Path) -> Path:
    """Reject the live root, including aliases through existing symlinks."""
    output = path.expanduser().resolve()
    live = (Path.home() / ".local/share/gothic-ai/content").resolve()
    if output.is_relative_to(live) or live.is_relative_to(output):
        raise ValueError("output must be outside the live ~/.local/share/gothic-ai/content directory")
    return output


def select_set_records(records: list[dict], set_name: str, mapping: Path) -> list[dict]:
    """Select exactly one completed supported filter draft per included texture.

    Attempts explicitly marked selected=false are retained only in the registry.
    A duplicate completed selection is an error, so registry ordering cannot
    silently choose a different image for a later capture.
    """
    if re.fullmatch(r"[a-z0-9][a-z0-9-]*", set_name) is None:
        raise ValueError(f"unsafe set name: {set_name!r}")
    with mapping.open(newline="", encoding="utf-8") as stream:
        expected = {}
        for row in selected_rows(list(csv.DictReader(stream)), set_name):
            texture = row["texture"]
            if texture in expected:
                raise ValueError(f"mapping repeats included texture {texture}")
            expected[texture] = row
    if not expected:
        raise ValueError("mapping has no selected textures")
    selected = {}
    ids = set()
    for record in records:
        params = record.get("parameters", {})
        if (record.get("kind") != "material" or params.get("set") != set_name
                or record.get("generation", {}).get("outcome") != "complete"
                or params.get("selected") is False):
            continue
        resource_id = record.get("id")
        if not isinstance(resource_id, str) or re.fullmatch(r"[a-z0-9][a-z0-9_-]*", resource_id) is None:
            raise ValueError(f"unsafe resource id: {resource_id!r}")
        if set_name == CITY_3_SET:
            pipelines = ("imagegen-filter",)
        else:
            pipelines = ("flux-filter", "imagegen-filter") if set_name == "city-2-imagegen" else ("flux-filter",)
        if record.get("pipeline") not in pipelines or record.get("status") != "draft":
            raise ValueError(f"{resource_id}: set material must be a draft from {' or '.join(pipelines)}")
        replaces = record.get("replaces")
        if not isinstance(replaces, list) or len(replaces) != 1 or not isinstance(replaces[0], str):
            raise ValueError(f"{resource_id}: set material must replace exactly one texture")
        texture = replaces[0]
        if texture not in expected:
            raise ValueError(f"{resource_id}: {texture} is not an include texture or selected facade texture")
        if texture in selected or resource_id in ids:
            raise ValueError(f"duplicate selected material: {texture} ({resource_id})")
        row = expected[texture]
        scale = params.get("scale")
        if isinstance(scale, bool) or not isinstance(scale, (int, float)) or not math.isfinite(scale) or scale <= 0:
            raise ValueError(f"{resource_id}: scale must be finite and positive")
        if not math.isclose(scale, float(row["scale"]), abs_tol=1e-8, rel_tol=0):
            raise ValueError(f"{resource_id}: scale differs from mapping for {texture}")
        if params.get("material_class") != row["class"]:
            raise ValueError(f"{resource_id}: material_class differs from mapping for {texture}")
        if set_name in PLACEMENT_SETS:
            target = params.get("target_luminance")
            if (isinstance(target, bool) or not isinstance(target, (int, float))
                    or not math.isfinite(target) or not 0 <= target <= 1):
                raise ValueError(f"{resource_id}: target_luminance must be finite and in [0, 1]")
            if not math.isclose(target, float(row["target_luminance"]), abs_tol=1e-10, rel_tol=0):
                raise ValueError(f"{resource_id}: target_luminance differs from mapping for {texture}")
            if params.get("antitile") != antitile_mode(row):
                raise ValueError(f"{resource_id}: antitile differs from class policy for {texture}")
            cell = params.get("antitile_cell", 1.0)
            if isinstance(cell, bool) or not isinstance(cell, (int, float)) or not math.isfinite(cell) or cell <= 0:
                raise ValueError(f"{resource_id}: antitile_cell must be finite and positive")
            if row["decision"] != "include" and scale != 1:
                raise ValueError(f"{resource_id}: architectural modules require scale=1")
        ids.add(resource_id)
        selected[texture] = record
    missing = expected.keys() - selected.keys()
    if missing:
        raise ValueError(f"set {set_name}: missing completed materials for {', '.join(sorted(missing))}")
    return [selected[texture] for texture in expected]


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
            # Preserve PNG transparency, including indexed PNG tRNS. An opaque
            # RGBA albedo still packs as DXT1 so the engine keeps it Solid.
            mode = "RGBA" if name == "albedo" and ("A" in source.getbands() or "transparency" in source.info) else (
                "RGB" if name in ("albedo", "normal") else "L")
            array = np.asarray(source.convert(mode), dtype=np.float32) / 255
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


def pack_set(records: list[dict], content_root: Path, output: Path) -> Path:
    """Write a single staged set root, indexing only selected generated maps."""
    output = staging_output(output)
    index = {}
    write_dds = load_compressor()
    for record in records:
        resource_id = record["id"]
        directory = (output / "materials" / resource_id).resolve()
        if not directory.is_relative_to(output):
            raise ValueError(f"output symlink escapes staging root: {resource_id}")
        # Read and validate every channel before creating this material's files.
        maps = read_maps(record, content_root)
        directory.mkdir(parents=True, exist_ok=True)
        for name in ("albedo", "normal", "orm"):
            target = directory / f"{name}.dds"
            if target.is_symlink():
                raise ValueError(f"output DDS must not be a symlink: {target}")
        write_dds(directory / "albedo.dds", maps["albedo"])
        write_dds(directory / "normal.dds", maps["normal"], normal=True)
        write_dds(directory / "orm.dds", np.stack((maps["ao"], maps["roughness"], maps["metallic"]), axis=2))
        entry = {name: f"{resource_id}/{name}.dds" for name in ("albedo", "normal", "orm")}
        entry["scale"] = record["parameters"]["scale"]
        if record["parameters"].get("set") in PLACEMENT_SETS:
            entry["antitile"] = record["parameters"]["antitile"]
            entry["antitile_cell"] = record["parameters"].get("antitile_cell", 1.0)
        index[record["replaces"][0]] = entry
    target = output / "materials" / "materials.json"
    if target.is_symlink():
        raise ValueError(f"output index must not be a symlink: {target}")
    target.write_text(json.dumps(index, indent=2) + "\n")
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--registry", type=Path, required=True)
    parser.add_argument("--content-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--id", action="append", help="capture only these resource IDs (repeatable)")
    parser.add_argument("--set", dest="set_name", help="pack one complete named material set into --output")
    parser.add_argument("--mapping", type=Path, default=DEFAULT_MAPPING, help="CSV defining every include texture for --set")
    parser.add_argument("--scale", type=float, default=1.0, help="fixed UV scale for every comparison (default: 1)")
    args = parser.parse_args()
    if not math.isfinite(args.scale) or args.scale <= 0:
        parser.error("--scale must be finite and positive")
    if args.set_name and (args.id or args.scale != 1):
        parser.error("--set uses the full mapping and each recorded scale; omit --id and --scale")
    try:
        content_root = args.content_root.expanduser().resolve()
        output = staging_output(args.output)
        records = [json.loads(line) for line in args.registry.read_text().splitlines() if line.strip()]
        if args.set_name:
            selected = select_set_records(records, args.set_name, args.mapping)
            print(pack_set(selected, content_root, output))
            return 0
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
