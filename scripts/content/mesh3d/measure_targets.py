#!/usr/bin/env python3
"""Measure the five pilot targets; original geometry never leaves the C++ decoder.

The decoder reads Meshes/Anims/Worlds VDFs in memory and emits scalar JSON only.
To build it, reuse the ZenKit build from scripts/content/measure-originals.sh:
  python3 scripts/content/mesh3d/measure_targets.py --game '/path/to/Gothic II'
The resulting bbox uses PR #309's glTF convention: (-x, y, z) / 100 metres.
"""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
SOURCE = Path(__file__).with_name("target-measures.cpp")

# Visuals chosen from NEWWORLD.ZEN counts, without rendering or exporting originals.
TARGETS = [
    {
        "id": "barrel", "visual": "NW_HARBOUR_BARREL_01.MRM", "kind": "static_mrm", "budget_tris": 1500,
        "zones": [
            {"key": "OWN_PROP_WOOD_STAVE", "group": "WOOD", "role": "curved vertical staves"},
            {"key": "OWN_PROP_WOOD_HEAD", "group": "WOOD", "role": "inset plank ends"},
            {"key": "OWN_PROP_IRON_BAND", "group": "METAL", "role": "forged retaining hoops and rivets"},
        ],
    },
    {
        "id": "crate", "visual": "NW_HARBOUR_CRATE_01.MRM", "kind": "static_mrm", "budget_tris": 1500,
        "zones": [
            {"key": "OWN_PROP_WOOD_PLANK", "group": "WOOD", "role": "box plank faces"},
            {"key": "OWN_PROP_WOOD_BRACE", "group": "WOOD", "role": "corner and face reinforcing rails"},
            {"key": "OWN_PROP_IRON_NAIL", "group": "METAL", "role": "small forged nail heads"},
            {"key": "OWN_PROP_IRON_BAND", "group": "METAL", "role": "forged straps; unused by the 2026-10-21 crate"},
            {"key": "OWN_PROP_ROPE", "group": "WOOD", "role": "rope handles on the ends; small, so they sound and collide as wood"},
            {"key": "OWN_PROP_WOOD_INNER", "group": "WOOD", "role": "dark core box seen through the plank gaps"},
        ],
    },
    {
        "id": "table", "visual": "NW_CITY_TABLE_PEASANT_01.MRM", "kind": "static_mrm", "budget_tris": 3000,
        "zones": [
            {"key": "OWN_PROP_WOOD_TOP", "group": "WOOD", "role": "thick plank tabletop"},
            {"key": "OWN_PROP_WOOD_FRAME", "group": "WOOD", "role": "legs, aprons and lower stretchers"},
            {"key": "OWN_PROP_IRON_BRACKET", "group": "METAL", "role": "small forged joint brackets and nails"},
        ],
    },
    {
        "id": "chest", "visual": "CHESTBIG_NW_NORMAL_LOCKED.MDS", "kind": "mob_model", "budget_tris": 3000,
        "zones": [
            {"key": "OWN_PROP_WOOD_BODY", "group": "WOOD", "role": "closed box body planks"},
            {"key": "OWN_PROP_WOOD_LID", "group": "WOOD", "role": "arched lid planks"},
            {"key": "OWN_PROP_IRON_BAND", "group": "METAL", "role": "reinforcing straps and hinges"},
            {"key": "OWN_PROP_IRON_LOCK", "group": "METAL", "role": "front hasp and lock plate"},
        ],
    },
    {
        "id": "bench", "visual": "BENCH_NW_CITY_01.ASC", "kind": "mob_model", "budget_tris": 3000,
        "zones": [
            {"key": "OWN_PROP_WOOD_SEAT", "group": "WOOD", "role": "long plank seat"},
            {"key": "OWN_PROP_WOOD_FRAME", "group": "WOOD", "role": "supports and lower stretcher"},
            {"key": "OWN_PROP_IRON_BRACKET", "group": "METAL", "role": "small forged joint straps and nails"},
        ],
    },
]


def gltf_bounds(source_min: list[float], source_max: list[float]) -> tuple[list[float], list[float]]:
    """Reflect X (including bound order), preserve +Y up, convert centimetres."""
    if len(source_min) != 3 or len(source_max) != 3:
        raise ValueError("bbox needs three coordinates")
    if not all(math.isfinite(v) for v in (*source_min, *source_max)):
        raise ValueError("bbox must be finite")
    if any(lo >= hi for lo, hi in zip(source_min, source_max)):
        raise ValueError("bbox must have positive extent on every axis")
    lo = [-source_max[0] / 100, source_min[1] / 100, source_min[2] / 100]
    hi = [-source_min[0] / 100, source_max[1] / 100, source_max[2] / 100]
    return lo, hi


def target_record(target: dict, measured: dict) -> dict:
    if measured["visual"] != target["visual"]:
        raise ValueError("decoder returned a different visual")
    tris = measured["source_tris"]
    if isinstance(tris, bool) or not isinstance(tris, int) or tris <= 0:
        raise ValueError("source_tris must be a positive integer")
    lo, hi = gltf_bounds(measured["source_bbox_min_cm"], measured["source_bbox_max_cm"])
    return {
        **target,
        "resolved_visual": measured["resolved_visual"],
        "bbox_min": [round(v, 12) for v in lo],
        "bbox_max": [round(v, 12) for v in hi],
        # Fraction along each bbox extent, deliberately unclamped if origin is outside.
        "origin_in_bbox": [round(-a / (b - a), 12) for a, b in zip(lo, hi)],
        "source_tris": tris,
        "source_pose": measured["source_pose"],
        "newworld_instances": measured["newworld_instances"],
    }


def build_extractor(zenkit_build: Path, output: Path) -> None:
    library = zenkit_build / "libzenkit.a"
    if not library.is_file():
        raise FileNotFoundError("build ZenKit with scripts/content/measure-originals.sh first, or pass --extractor")
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++20", "-O2", "-DZKNO_REM=1", "-D_ZK_WITH_MMAP=1",
        f"-I{ROOT / 'OpenGothic/lib/ZenKit/include'}", f"-I{zenkit_build / 'include'}",
        str(SOURCE), str(library), str(zenkit_build / "vendor/libsquish/libsquish.a"), "-o", str(output),
    ], check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, required=True)
    parser.add_argument("--extractor", type=Path)
    parser.add_argument("--zenkit-build", type=Path, default=Path(tempfile.gettempdir()) / "gothic-original-scalar-measures/zenkit")
    parser.add_argument("--output", type=Path, default=ROOT / "data/content/mesh3d-pilot.json")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="gothic-mesh3d-measures-") as temp:
        extractor = args.extractor or Path(temp) / "target-measures"
        if args.extractor is None:
            build_extractor(args.zenkit_build, extractor)
        result = subprocess.run([str(extractor), str(args.game), *(t["visual"] for t in TARGETS)],
                                check=True, capture_output=True, text=True)
        measurements = json.loads(result.stdout)
        if len(measurements) != len(TARGETS):
            raise ValueError("decoder must return exactly the five pilot targets")
        records = [target_record(t, m) for t, m in zip(TARGETS, measurements)]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(records, indent=2, ensure_ascii=False) + "\n")
    print(f"wrote {len(records)} scalar targets to {args.output}")


if __name__ == "__main__":
    main()
