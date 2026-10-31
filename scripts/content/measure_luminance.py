#!/usr/bin/env python3
"""Measure original brightness and alpha coverage as scalars.

    python scripts/content/measure_luminance.py --extractor /path/to/texture-luminance \
        --game '/path/to/Gothic II' --write

The extractor decodes Gothic textures in memory, using texdump.cpp's ZenKit
path. City-3 uses the engine's alpha>=0.5 cutoff for coverage and visible-pixel
mean linear Rec.709 luminance. Earlier sets keep their alpha-weighted scalar.
Original pixels are never exported or passed to material generation.

``--materials`` measures periods, direction, mean color, contrast and detail for
the entire mapping. It refreshes every scalar material target, preserving the
other mapping fields, including luminance and alpha coverage.
"""
from __future__ import annotations

import argparse
import csv
import io
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from map_textures import ALPHA_TEXTURES, DEFAULT_MAPPING, DETAIL_FIELDS, FIELDS, MAPPING_HEADERS, MEASUREMENT_FIELDS, _check_measurements
from pilot import selected_rows


def read_mapping(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream, strict=True)
        if tuple(reader.fieldnames or ()) not in MAPPING_HEADERS:
            raise ValueError("mapping must contain the current columns, optionally without target_coverage and target_luminance")
        rows = list(reader)
    if any(None in row or any(value is None for value in row.values()) for row in rows):
        raise ValueError("mapping has a malformed CSV row")
    return rows


def parse_measurements(value, expected, set_name="city-2"):
    reader = csv.DictReader(io.StringIO(value), strict=True)
    if reader.fieldnames != ["texture", "target_luminance"]:
        raise ValueError("extractor must return only texture,target_luminance")
    result = {}
    for row in reader:
        if None in row or any(item is None for item in row.values()):
            raise ValueError("extractor has a malformed CSV row")
        texture = row["texture"]
        if texture in result:
            raise ValueError(f"extractor repeats texture {texture}")
        luminance = float(row["target_luminance"])
        if not math.isfinite(luminance) or not 0 <= luminance <= 1:
            raise ValueError(f"{texture}: target_luminance must be finite and in [0, 1]")
        result[texture] = luminance
    if set(result) != set(expected):
        raise ValueError(f"extractor texture set differs from {set_name}")
    return result


PROPERTIES_FIELDS = (
    "texture", "target_luminance", "nonopaque_fraction", "opaque", "width", "height",
    "target_coverage", "coverage_left_half", "coverage_right_half", "coverage_top_half", "coverage_bottom_half",
)
EXTRACTED_MEASUREMENT_FIELDS = (
    "target_period_u_uv", "target_period_v_uv", "target_direction", "target_direction_confidence",
    "target_oklab_l", "target_oklab_a", "target_oklab_b", *DETAIL_FIELDS,
)


def parse_properties(value, expected, set_name="city-3"):
    reader = csv.DictReader(io.StringIO(value), strict=True)
    if reader.fieldnames != list(PROPERTIES_FIELDS):
        raise ValueError("extractor must return only the scalar properties columns")
    result = {}
    for row in reader:
        if None in row or any(item is None for item in row.values()):
            raise ValueError("extractor has a malformed CSV row")
        texture = row["texture"]
        if texture in result:
            raise ValueError(f"extractor repeats texture {texture}")
        for field in ("target_luminance", "target_coverage", "nonopaque_fraction",
                      "coverage_left_half", "coverage_right_half", "coverage_top_half", "coverage_bottom_half"):
            number = float(row[field])
            if not math.isfinite(number) or not 0 <= number <= 1:
                raise ValueError(f"{texture}: {field} must be finite and in [0, 1]")
        if row["opaque"] not in ("true", "false") or any(int(row[field]) <= 0 for field in ("width", "height")):
            raise ValueError(f"{texture}: invalid scalar texture properties")
        result[texture] = {field: float(row[field]) for field in ("target_luminance", "target_coverage")}
    if set(result) != set(expected):
        raise ValueError(f"extractor texture set differs from {set_name}")
    return result


def parse_material_measurements(value, rows):
    """Accept scalar-only rows for the complete 71-material mapping."""
    reader = csv.DictReader(io.StringIO(value), strict=True)
    if reader.fieldnames != [*PROPERTIES_FIELDS, *EXTRACTED_MEASUREMENT_FIELDS]:
        raise ValueError("extractor must return only the scalar material measurements columns")
    source = {row["texture"]: row for row in rows}
    result = {}
    for row in reader:
        if None in row or any(item is None for item in row.values()):
            raise ValueError("extractor has a malformed CSV row")
        texture = row["texture"]
        if texture in result:
            raise ValueError(f"extractor repeats texture {texture}")
        if texture not in source:
            raise ValueError(f"extractor contains unknown texture {texture}")
        values = {field: row[field] for field in EXTRACTED_MEASUREMENT_FIELDS}
        for axis in ("u", "v"):
            values[f"target_period_{axis}_m"] = format(float(row[f"target_period_{axis}_uv"]) *
                float(source[texture]["uv_m_per_unit_median"]), ".12g")
        _check_measurements(dict(source[texture], **values), texture)
        result[texture] = values
    if set(result) != set(source):
        raise ValueError("extractor texture set differs from the complete mapping")
    return result


def measure_all(extractor, game, rows):
    result = subprocess.run([str(extractor), "--measurements", str(game),
                             *(row["texture"] for row in rows)], check=True, capture_output=True, text=True)
    return parse_material_measurements(result.stdout, rows)


def update_material_measurements(path, rows, values):
    if set(values) != {row["texture"] for row in rows}:
        raise ValueError("measurement texture set differs from the complete mapping")
    updated = []
    for row in rows:
        if set(values[row["texture"]]) != set(MEASUREMENT_FIELDS):
            raise ValueError("measurement must contain exactly the scalar material fields")
        merged = {field: row.get(field, "") for field in FIELDS}
        merged.update(values[row["texture"]])
        _check_measurements(merged, row["texture"])
        updated.append(merged)
    _write_mapping(path, updated)
    return updated


def _write_mapping(path, rows):
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="", dir=path.parent,
                                         delete=False) as stream:
            temporary = Path(stream.name)
            writer = csv.DictWriter(stream, fieldnames=FIELDS, lineterminator="\n")
            writer.writeheader()
            writer.writerows(rows)
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def measure(extractor, game, rows, set_name="city-2", *, with_coverage=False):
    names = [row["texture"] for row in selected_rows(rows, set_name)]
    if not names:
        raise ValueError(f"{set_name} has no textures")
    properties = set_name == "city-3" or with_coverage
    command = [str(extractor), *(["--properties"] if properties else []), str(game), *names]
    result = subprocess.run(command, check=True,
                            capture_output=True, text=True)
    if properties:
        values = parse_properties(result.stdout, names, set_name)
        luminance = {name: row["target_luminance"] for name, row in values.items()}
        coverage = {name: row["target_coverage"] for name, row in values.items() if name in ALPHA_TEXTURES}
        return (luminance, coverage) if with_coverage else luminance
    return parse_measurements(result.stdout, names, set_name)


def update_mapping(path, rows, measurements, set_name="city-2", *, coverages=None):
    names = {row["texture"] for row in selected_rows(rows, set_name)}
    if set(measurements) != names:
        raise ValueError(f"measurement texture set differs from {set_name}")
    if any(isinstance(value, bool) or not isinstance(value, (int, float))
           or not math.isfinite(value) or not 0 <= value <= 1 for value in measurements.values()):
        raise ValueError("target_luminance must be finite and in [0, 1]")
    if coverages is not None:
        if set(coverages) != names & ALPHA_TEXTURES:
            raise ValueError(f"coverage texture set differs from alpha materials in {set_name}")
        if any(isinstance(value, bool) or not isinstance(value, (int, float))
               or not math.isfinite(value) or not 0 <= value <= 1 for value in coverages.values()):
            raise ValueError("target_coverage must be finite and in [0, 1]")
    # Preserve every pre-existing field, including scalars outside this set.
    updated = [dict(row, target_luminance=(format(measurements[row["texture"]], ".12g")
                    if row["texture"] in names else row.get("target_luminance", "")),
                    target_coverage=(format(coverages[row["texture"]], ".12g")
                    if coverages is not None and row["texture"] in coverages else row.get("target_coverage", "")))
               for row in rows]
    _write_mapping(path, updated)
    return updated


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--extractor", type=Path, required=True)
    parser.add_argument("--game", type=Path, required=True)
    parser.add_argument("--mapping", type=Path, default=DEFAULT_MAPPING)
    parser.add_argument("--set", dest="set_name", choices=("city-2", "city-3"), default="city-2",
                        help="material scope to measure (default: city-2)")
    parser.add_argument("--write", action="store_true", help="atomically update target_luminance and city-3 alpha target_coverage")
    parser.add_argument("--materials", action="store_true", help="measure scalar periods, direction, OKLab, contrast and detail for all mapping rows")
    args = parser.parse_args(argv)
    try:
        rows = read_mapping(args.mapping)
        if args.materials:
            values = measure_all(args.extractor, args.game, rows)
            if args.write:
                update_material_measurements(args.mapping, rows, values)
            writer = csv.DictWriter(sys.stdout, fieldnames=("texture", *MEASUREMENT_FIELDS), lineterminator="\n")
            writer.writeheader()
            writer.writerows(dict(texture=name, **value) for name, value in values.items())
            return 0
        coverages = None
        if args.set_name == "city-3":
            values, coverages = measure(args.extractor, args.game, rows, args.set_name, with_coverage=True)
        else:
            values = measure(args.extractor, args.game, rows, args.set_name)
        if args.write:
            update_mapping(args.mapping, rows, values, args.set_name, coverages=coverages)
        writer = csv.writer(sys.stdout, lineterminator="\n")
        writer.writerow(("texture", "target_luminance", "target_coverage") if coverages is not None else ("texture", "target_luminance"))
        for texture, value in values.items():
            row = [texture, format(value, ".12g")]
            if coverages is not None:
                row.append(format(coverages[texture], ".12g") if texture in coverages else "")
            writer.writerow(row)
    except (OSError, ValueError, csv.Error, subprocess.CalledProcessError) as error:
        detail = error.stderr.strip() if isinstance(error, subprocess.CalledProcessError) and error.stderr else str(error)
        print(f"measure_luminance: {detail}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
