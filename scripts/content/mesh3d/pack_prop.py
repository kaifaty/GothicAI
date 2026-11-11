#!/usr/bin/env python3
"""Pack a baked prop (bake_atlas.py output) into one OPENGOTHIC_CONTENT root.

    uv run --no-project --with numpy --with pillow python scripts/content/mesh3d/pack_prop.py \
        --bake /abs/bake-out --visual NW_HARBOUR_BARREL_01 --name mesh3d-barrel-atlas \
        --output ~/.local/share/gothic-ai/content-staging/mesh3d-barrel-atlas

The atlas is written once as DDS (stage-1 compressor: DXT1 albedo and ORM,
DXT5nm normal with the engine's green flip). Every atlas material of the GLB
gets its own materials.json key pointing at the same files, scale 1, because
the atlas UV already spans 0..1. The GLB goes to meshes/<name>/model.glb.

With --debris the bake is a prop's debris (barrel_debris.py, bake_atlas.py "nodes": "*"): its atlas goes
to materials/<name>-debris/, its keys are added to the existing materials.json, and the GLB goes next to
the whole prop as meshes/<name>/model.debris.glb. The whole prop's files and meshes.json stay as they are.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import shutil

import numpy as np
from PIL import Image


def load_compressor():
    path = Path(__file__).resolve().parents[1] / "test-material.py"
    spec = importlib.util.spec_from_file_location("material_fixture", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.write_dds


def read(folder: Path, name: str, mode: str) -> np.ndarray:
    with Image.open(folder / f"{name}.png") as image:
        return np.asarray(image.convert(mode), dtype=np.float32) / 255


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--bake", type=Path, required=True)
    parser.add_argument("--visual", required=True, help="original mesh name without extension")
    parser.add_argument("--name", required=True, help="folder name for the materials and the mesh")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--debris", action="store_true", help="the bake is the prop's debris, not the prop")
    args = parser.parse_args()
    metrics = json.loads((args.bake / "metrics.json").read_text())
    keys = metrics["materials"]
    maps = args.bake / "maps" / keys[0].lower()
    output = args.output.expanduser()
    folder = f"{args.name}-debris" if args.debris else args.name
    textures = output / "materials" / folder
    textures.mkdir(parents=True, exist_ok=True)
    write_dds = load_compressor()
    normal = read(maps, "normal", "RGB") * 2 - 1
    normal[:, :, 1] *= -1  # PNG +Y up -> engine bitangent follows texture V down, as pack_materials.py
    normal[:, :, 2] = np.maximum(normal[:, :, 2], 1e-3)
    normal /= np.linalg.norm(normal, axis=2, keepdims=True)
    write_dds(textures / "albedo.dds", read(maps, "albedo", "RGB"))
    write_dds(textures / "normal.dds", normal, normal=True)
    write_dds(textures / "orm.dds", np.stack((read(maps, "ao", "L"), read(maps, "roughness", "L"),
                                              read(maps, "metallic", "L")), axis=2))
    entry = {name: f"{folder}/{name}.dds" for name in ("albedo", "normal", "orm")}
    entry.update(scale=1.0, antitile="off", antitile_cell=1)
    table = output / "materials" / "materials.json"
    entries = json.loads(table.read_text()) if args.debris and table.exists() else {}
    entries.update({f"{key}.TGA": entry for key in keys})
    table.write_text(json.dumps(entries, indent=2) + "\n")
    meshes = output / "meshes" / args.name
    meshes.mkdir(parents=True, exist_ok=True)
    if args.debris:
        shutil.copyfile(args.bake / "model.glb", meshes / "model.debris.glb")
        print(f"Packed the debris of {args.visual}: {', '.join(keys)} -> {output}")
        return 0
    shutil.copyfile(args.bake / "model.glb", meshes / "model.glb")
    (output / "meshes" / "meshes.json").write_text(
        json.dumps({args.visual: {"glb": f"{args.name}/model.glb"}}, indent=2) + "\n")
    print(f"Packed {args.visual}: {', '.join(keys)} -> {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
