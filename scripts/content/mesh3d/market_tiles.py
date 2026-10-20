#!/usr/bin/env python3
"""Tile maps for the market props from our own image_gen texture sources.

    uv run --no-project --with numpy --with pillow --with scipy python \
        scripts/content/mesh3d/market_tiles.py --spec spec.json [--import-sources --utc <ISO>]

spec: {"output": "<content-staging>/market-set/tiles",
       "tiles": {"own_prop_cloth": {"png": "/abs/source.png", "axes": "uv", "roughness": 0.9,
                                    "metallic": 0.0, "prompt": "...", "replaces": ["OC_SAECKE_01"]},
                 "own_prop_wood_plank": {"copy": "/abs/existing/tile/dir"}}}
A source tile is quilted seamless along its axes (seam_fix.seamless), resized to
1024² and turned into the six stage-1 maps by the CPU filter (bakeoff.filtered_maps).
"crop": [l, t, r, b] with "size": [w, h] cuts a panel (a whole carpet) out of our own
concept instead and keeps its aspect; such a panel has axes "".
"copy" reuses an existing tile directory unchanged; "procedural": {"base": [r, g, b],
"noise": 0.08, "seed": 1, "relief": 0.25} makes a smooth periodic surface (wax, glass, bone) with
no image_gen; relief scales its normal, height and AO down from the filter's full strength. The atlas bake later scales
each zone to the original's measured mean colour, so a tile carries pattern and
relief, not the final colour. --import-sources keeps each source PNG once in
content-sources/mesh3d-market/ and appends one material recipe per tile.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tempfile

import numpy as np
from PIL import Image

CONTENT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(CONTENT))
import bakeoff  # noqa: E402
import registry as registry_module  # noqa: E402
from seam_fix import seamless  # noqa: E402

MAPS = ("albedo", "normal", "roughness", "metallic", "ao", "height")
SOURCE_SET = "mesh3d-market"


def build_tile(source: Path, axes: str, roughness: float, metallic: float, out: Path,
               crop: list | None = None, size: list | None = None) -> dict:
    with Image.open(source) as image:
        rgb = image.convert("RGB")
    if crop:  # a panel (a whole carpet), not a tile: cut its rectangle and keep its own aspect
        rgb = rgb.crop(tuple(crop)).resize(tuple(size), Image.Resampling.LANCZOS)
    elif rgb.size != (1024, 1024):
        side = min(rgb.size)
        left, top = (rgb.width - side) // 2, (rgb.height - side) // 2
        rgb = rgb.crop((left, top, left + side, top + side)).resize((1024, 1024), Image.Resampling.LANCZOS)
    rgb, seam = seamless(rgb, axes) if axes else (rgb, dict(applied=False, axes=""))
    maps = bakeoff.filtered_maps(rgb, {"parameters": {"tiling_axes": axes, "roughness_base": roughness}})
    maps["metallic"] = np.full_like(maps["height"], metallic)
    out.mkdir(parents=True, exist_ok=True)
    for name in MAPS:
        value = np.clip(np.asarray(maps[name], dtype=np.float32), 0, 1)
        Image.fromarray(np.rint(value * 255).astype("uint8")).save(out / f"{name}.png")
    return seam


def procedural_tile(spec: dict, out: Path) -> None:
    """A smooth, periodic noise surface (wax, glass, bone): no image_gen, reproducible from the spec."""
    from scipy.ndimage import gaussian_filter
    rng = np.random.default_rng(spec.get("seed", 1))
    field = np.zeros((1024, 1024), dtype=np.float32)
    for sigma, weight in ((48, 0.6), (12, 0.3), (3, 0.1)):
        layer = gaussian_filter(rng.standard_normal((1024, 1024)).astype(np.float32), sigma, mode="wrap")
        field += weight * layer / max(float(np.abs(layer).max()), 1e-6)
    shade = np.clip(1 + spec.get("noise", 0.08) * field, 0, None)[..., None]
    rgb = np.clip(np.array(spec["base"], dtype=np.float32) * shade, 0, 1)
    image = Image.fromarray(np.rint(rgb * 255).astype("uint8"))
    maps = bakeoff.filtered_maps(image, {"parameters": {"tiling_axes": "uv", "roughness_base": spec["roughness"]}})
    maps["metallic"] = np.full_like(maps["height"], spec.get("metallic", 0.0))
    # The stage-1 filter stretches any relief to full strength; these surfaces are smooth.
    relief = spec.get("relief", 0.25)
    flat = np.array([0.5, 0.5, 1.0], dtype=np.float32)
    normal = flat + (np.asarray(maps["normal"], dtype=np.float32) - flat) * relief
    maps["normal"] = normal
    maps["height"] = 0.5 + (np.asarray(maps["height"], dtype=np.float32) - 0.5) * relief
    maps["ao"] = 1 - (1 - np.asarray(maps["ao"], dtype=np.float32)) * relief
    out.mkdir(parents=True, exist_ok=True)
    for name in MAPS:
        value = np.clip(np.asarray(maps[name], dtype=np.float32), 0, 1)
        Image.fromarray(np.rint(value * 255).astype("uint8")).save(out / f"{name}.png")


def record_for(key: str, spec: dict, seam: dict, raw: bytes, utc: str, files_root: Path, tile_dir: Path) -> dict:
    relative = f"{SOURCE_SET}/tile-{key.removeprefix('own_prop_').replace('_', '-')}.png"
    return dict(
        id=f"{SOURCE_SET}-tile-{key.removeprefix('own_prop_').replace('_', '-')}", kind="material",
        replaces=spec["replaces"], pipeline="imagegen-filter",
        models=[dict(id="image_gen.imagegen", revision=None, license="OpenAI Terms of Use; output owned by user")],
        prompt=spec["prompt"], negative="", seed=None, utc=utc, status="draft",
        files={name: (tile_dir / f"{name}.png").relative_to(files_root).as_posix() for name in MAPS},
        source_png=relative, source_sha256=hashlib.sha256(raw).hexdigest(),
        parameters=dict(set=SOURCE_SET, zone_key=key.upper(), tiling_axes=spec["axes"], crop=spec.get("crop"),
                        roughness_base=spec["roughness"], metallic=spec.get("metallic", 0.0),
                        input="text only; no original pixels", colour_policy="final colour set by the atlas bake"),
        generation=dict(outcome="complete", seam_correction=seam,
                        model_identity="Tool image_gen.imagegen; underlying model/version not exposed",
                        seed_policy="No seed exposed by the built-in tool"),
    )


def append_records(records: list[dict], registry: Path, source_root: Path) -> None:
    existing = registry_module.check_registry(registry, source_root=source_root) if registry.exists() else []
    known = {entry["id"]: entry for entry in existing}
    fresh = []
    for record in records:
        registry_module.validate_entry(record, source_root=source_root)
        if record["id"] in known:
            if known[record["id"]] != record:
                raise ValueError(f"tile id already has a different recipe: {record['id']}")
            continue
        fresh.append(record)
    if not fresh:
        return
    data = registry.read_bytes() if registry.exists() else b""
    if data and not data.endswith(b"\n"):
        data += b"\n"
    data += b"".join((json.dumps(r, ensure_ascii=False, sort_keys=True, allow_nan=False) + "\n").encode() for r in fresh)
    with tempfile.NamedTemporaryFile(dir=registry.parent, prefix=registry.name + ".", suffix=".tmp", delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(data)
    try:
        registry_module.check_registry(temporary, source_root=source_root)
        temporary.replace(registry)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--spec", type=Path, required=True)
    parser.add_argument("--import-sources", action="store_true")
    parser.add_argument("--utc")
    parser.add_argument("--registry", type=Path, default=registry_module.DEFAULT_REGISTRY)
    parser.add_argument("--source-root", type=Path, default=registry_module.DEFAULT_SOURCE_ROOT)
    args = parser.parse_args()
    spec = json.loads(args.spec.read_text())
    output = Path(spec["output"]).expanduser()
    files_root = output.parent
    source_root = args.source_root.expanduser().resolve()
    records = []
    for key, tile in spec["tiles"].items():
        target = output / key
        if "procedural" in tile:
            shutil.rmtree(target, ignore_errors=True)
            procedural_tile({**tile["procedural"], "roughness": tile["roughness"], "metallic": tile.get("metallic", 0.0)}, target)
            print(f"{key}: procedural {tile['procedural']}")
            continue
        if "copy" in tile:
            shutil.rmtree(target, ignore_errors=True)
            shutil.copytree(Path(tile["copy"]).expanduser(), target)  # pilot tiles: wood and iron
            print(f"{key}: copied {tile['copy']}")
            continue
        png = Path(tile["png"]).expanduser()
        raw = png.read_bytes()
        seam = build_tile(png, tile["axes"], tile["roughness"], tile.get("metallic", 0.0), target,
                          tile.get("crop"), tile.get("size"))
        print(f"{key}: built from {png.name}, seam {seam.get('applied')}")
        if args.import_sources:
            record = record_for(key, tile, seam, raw, args.utc, files_root, target)
            store = source_root / record["source_png"]
            store.parent.mkdir(parents=True, exist_ok=True)
            if store.exists() and store.read_bytes() != raw:
                raise ValueError(f"refusing to overwrite retained PNG: {store}")
            if not store.exists():
                store.write_bytes(raw)
            records.append(record)
    if records:
        append_records(records, args.registry, source_root)
        print(f"registry: {len(records)} tile recipes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
