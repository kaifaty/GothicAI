#!/usr/bin/env python3
"""Check city material decisions against the world texture census.

    python3 scripts/content/map_textures.py check
    python3 scripts/content/map_textures.py update

``update`` refreshes area, UV density and material scale, preserving the manual
decisions and original scalar measurements (metre periods follow UV density).
It writes only after validating the city, harbour and seven market/wall textures.
"""

from __future__ import annotations

import argparse
import csv
from decimal import Decimal, InvalidOperation, ROUND_HALF_UP, localcontext
import os
from pathlib import Path
import re
import sys
import tempfile

from pilot import CITY_3_EXCLUDED_CLASSES, selected_rows


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MAPPING = ROOT / "data/content/city-materials.csv"
DEFAULT_CENSUS = ROOT / "reports/content/2026-10-world-textures/newworld.csv"
LEGACY_FIELDS = (
    "texture", "decision", "class", "state", "scale", "area_m2",
    "uv_m_per_unit_median", "reason",
    "object", "role", "depicts", "colour", "weathering", "orientation",
    "layout", "layout_share", "anti_repeat", "target_luminance", "target_coverage",
)
DETAIL_FIELDS = (
    "target_log_spread", "target_detail_k3", "target_detail_k9", "target_detail_k27",
)
MEASUREMENT_FIELDS = (
    "target_period_u_uv", "target_period_v_uv", "target_period_u_m", "target_period_v_m",
    "target_direction", "target_direction_confidence", "target_oklab_l", "target_oklab_a", "target_oklab_b",
    *DETAIL_FIELDS,
)
MEASURED_FIELDS = (*LEGACY_FIELDS[:-2], *MEASUREMENT_FIELDS, *LEGACY_FIELDS[-2:])
COUNT_FIELDS = (
    "count_u", "count_v", "counted", "count_direction", "parts", "uv_m_u", "uv_m_v",
    "element_cm_u", "element_cm_v",
)
FIELDS = (*MEASURED_FIELDS[:-2], *COUNT_FIELDS, *MEASURED_FIELDS[-2:])
PREVIOUS_FIELDS = tuple(field for field in FIELDS if field not in DETAIL_FIELDS)
PREVIOUS_MEASURED_FIELDS = tuple(field for field in MEASURED_FIELDS if field not in DETAIL_FIELDS)
MAPPING_HEADERS = (
    FIELDS, FIELDS[:-1], FIELDS[:-2],
    PREVIOUS_FIELDS, PREVIOUS_FIELDS[:-1], PREVIOUS_FIELDS[:-2],
    MEASURED_FIELDS, MEASURED_FIELDS[:-1], MEASURED_FIELDS[:-2],
    PREVIOUS_MEASURED_FIELDS, PREVIOUS_MEASURED_FIELDS[:-1], PREVIOUS_MEASURED_FIELDS[:-2],
    LEGACY_FIELDS, LEGACY_FIELDS[:-1], LEGACY_FIELDS[:-2],
)
ALPHA_TEXTURES = frozenset({
    "NW_CITY_FLAG_01.TGA", "NW_CITY_IVY_BORDER_01.TGA", "NW_CITY_IVY_FULL_01.TGA",
})
CENSUS_FIELDS = frozenset({
    "world", "texture", "area_m2", "uv_m_per_unit_median", "tiled_area_fraction",
})
TILE_METERS = {name: Decimal(meters) for name, meters in {
    "cobble": "2", "flagstone": "3", "masonry_dressed": "3",
    "masonry_rubble": "2", "plaster": "2", "planks_floor": "2",
    "planks_wall": "2", "timber_beam": "1.5", "roof_shingle": "2",
    "roof_tiles": "2", "thatch": "2", "dirt_path": "3", "mud": "3",
}.items()}
SCOPE_SIZE = 71
MISC_TEXTURES = frozenset({
    "NW_MISC_STRONG_STONEWALL_01.TGA", "NW_MISC_STRONG_STONEWALL_SIDE_01.TGA",
    "NW_MISC_STRONG_STONEWALL_ENTRANCE_01.TGA", "NW_MISC_STRONG_STONEWALL_BLOCKBORDER_01.TGA",
    "NW_MISC_STRONG_STONEWALL_ROUNDBORDER_01.TGA", "NW_MISC_OLDSTONEPATH_01.TGA",
    "NW_MISC_OLDSTONEPATH_WAY_01.TGA",
})
PANEL_CLASSES = frozenset({
    "plaster_panel", "masonry_panel", "timber_frame_panel", "planks_panel",
    "roof_panel", "stone_panel", "painted_panel", "iron_panel", "cloth_panel",
    "foliage_panel", "net_panel", "rope_panel", "ground_panel",
})
STRIP_CLASSES = frozenset({
    "timber_strip", "stone_strip", "roof_ridge_strip", "plaster_strip",
    "rope_strip", "iron_strip", "net_strip",
})
MODULE_CLASSES = frozenset({
    "timber_frame_tile", "framed_masonry_tile", "framed_boards_tile",
    "window_wall_tile", "quay_wall_tile", "roof_structure_tile", "stair_tile",
    "pavement_border_tile", "bound_timber_tile", "path_tile", "paving_transition_tile", "banded_metal_tile",
})
LAYOUTS = ("tiled", "panel", "strip", "atlas")
LAYOUT_DECISIONS = dict(zip(LAYOUTS, ("include", "panel", "strip", "exclude")))
DECISION_CLASSES = {
    "include": TILE_METERS, "panel": PANEL_CLASSES, "strip": STRIP_CLASSES,
    "module": MODULE_CLASSES,
}
DECISIONS = (*DECISION_CLASSES, "exclude")
SEMANTIC_FIELDS = ("object", "role", "depicts", "colour", "weathering", "orientation")
SHARE_TOLERANCE = Decimal("0.00001")
STATE = re.compile(r"[A-Za-z]+(?:[-'][A-Za-z]+)*(?: [A-Za-z]+(?:[-'][A-Za-z]+)*){1,4}\Z")


class MappingError(ValueError):
    """An invalid mapping or census, with a location suitable for the CLI."""


def _read_csv(path: Path, *, mapping: bool) -> list[dict[str, str]]:
    try:
        with path.open(encoding="utf-8-sig", newline="") as stream:
            reader = csv.reader(stream, strict=True)
            header = next(reader, None)
            if header is None:
                raise MappingError(f"{path}: empty CSV")
            if len(header) != len(set(header)):
                raise MappingError(f"{path}: duplicate CSV column")
            if mapping and tuple(header) not in MAPPING_HEADERS:
                raise MappingError(f"{path}: expected CSV columns in order: {','.join(FIELDS)}")
            if not mapping and not CENSUS_FIELDS <= set(header):
                missing = ", ".join(sorted(CENSUS_FIELDS - set(header)))
                raise MappingError(f"{path}: missing census columns: {missing}")
            rows = []
            for values in reader:
                if len(values) != len(header):
                    raise MappingError(
                        f"{path}:{reader.line_num}: expected {len(header)} CSV fields, got {len(values)}"
                    )
                row = dict(zip(header, values))
                if mapping:
                    for field in FIELDS:
                        row.setdefault(field, "")
                rows.append(row)
            return rows
    except (OSError, UnicodeError, csv.Error) as error:
        raise MappingError(f"{path}: {error}") from error


def _number(value: str, field: str, *, positive: bool = False) -> Decimal:
    try:
        number = Decimal(value)
    except InvalidOperation as error:
        raise MappingError(f"{field}: expected a finite number, got {value!r}") from error
    if not number.is_finite() or (number <= 0 if positive else number < 0):
        sign = "positive" if positive else "nonnegative"
        raise MappingError(f"{field}: expected a finite {sign} number, got {value!r}")
    return number


def _check_measurements(row: dict[str, str], where: str) -> None:
    for field in DETAIL_FIELDS:
        _number(row.get(field, ""), f"{where}: {field}")
    for axis in ("u", "v"):
        period = _number(row.get(f"target_period_{axis}_uv", ""), f"{where}: target_period_{axis}_uv")
        meters = _number(row.get(f"target_period_{axis}_m", ""), f"{where}: target_period_{axis}_m")
        if period > 1:
            raise MappingError(f"{where}: target_period_{axis}_uv must be at most 1 (0 means no dominant peak)")
        expected = period * _number(row["uv_m_per_unit_median"], f"{where}: uv_m_per_unit_median", positive=True)
        if abs(meters - expected) > max(Decimal("1e-9"), expected * Decimal("1e-9")):
            raise MappingError(f"{where}: stale target_period_{axis}_m; expected period UV × uv_m_per_unit_median")
    if row.get("target_direction") not in ("U", "V", "none"):
        raise MappingError(f"{where}: target_direction must be U, V or none")
    for field in ("target_direction_confidence", "target_oklab_l"):
        value = _number(row.get(field, ""), f"{where}: {field}")
        if value > 1:
            raise MappingError(f"{where}: {field} must be at most 1")
    for field in ("target_oklab_a", "target_oklab_b"):
        try:
            value = Decimal(row.get(field, ""))
        except InvalidOperation as error:
            raise MappingError(f"{where}: {field} must be finite and in [-0.5, 0.5]") from error
        if not value.is_finite() or abs(value) > Decimal("0.5"):
            raise MappingError(f"{where}: {field} must be finite and in [-0.5, 0.5]")


def _check_counts(row: dict[str, str], where: str) -> None:
    # Older tables did not record visual counts. Preserve their read/update path.
    if not any(row.get(field, "") for field in COUNT_FIELDS):
        return
    if row["decision"] == "exclude" or row["class"] in CITY_3_EXCLUDED_CLASSES:
        raise MappingError(f"{where}: excluded preparation target requires empty count fields")
    if not row.get("counted", "").strip():
        raise MappingError(f"{where}: counted must be nonempty")
    if row.get("count_direction") not in ("U", "V", "none"):
        raise MappingError(f"{where}: count_direction must be U, V or none")
    if row["decision"] in ("module", "panel", "strip") and not row.get("parts", "").strip():
        raise MappingError(f"{where}: {row['decision']} requires nonempty parts")
    for axis in ("u", "v"):
        meters = _number(row.get(f"uv_m_{axis}", ""), f"{where}: uv_m_{axis}", positive=True)
        raw_count = row.get(f"count_{axis}", "")
        raw_size = row.get(f"element_cm_{axis}", "")
        if not raw_count:
            if raw_size:
                raise MappingError(f"{where}: empty count_{axis} requires empty element_cm_{axis}")
            continue
        count = _number(raw_count, f"{where}: count_{axis}", positive=True)
        pilot_half_count = row["texture"] == "NW_HARBOUR_STAIRS_01.TGA" and axis == "u" and count == Decimal("2.5")
        if count != count.to_integral_value() and not pilot_half_count:
            raise MappingError(f"{where}: count_{axis} must be a whole number")
        size = _number(raw_size, f"{where}: element_cm_{axis}", positive=True)
        if abs(size - 100 * meters / count) > Decimal("0.01"):
            raise MappingError(f"{where}: stale element_cm_{axis}; expected 100 × uv_m_{axis} / count_{axis}")


def _scope(texture: str) -> bool:
    return texture.startswith(("NW_CITY_", "NW_HARBOUR_")) or texture in MISC_TEXTURES


def read_census(path: Path) -> dict[str, dict[str, str]]:
    """Read the fixed NEWWORLD scope, rejecting duplicate or invalid sources."""
    census = {}
    for line, row in enumerate(_read_csv(path, mapping=False), 2):
        texture = row["texture"]
        if row["world"] != "NEWWORLD.ZEN" or not _scope(texture):
            continue
        where = f"{path}:{line}: {texture}"
        if texture in census:
            raise MappingError(f"{where}: duplicate census texture")
        _number(row["area_m2"], f"{where}: area_m2")
        _number(row["uv_m_per_unit_median"], f"{where}: uv_m_per_unit_median", positive=True)
        fraction = _number(row["tiled_area_fraction"], f"{where}: tiled_area_fraction")
        if fraction > 1:
            raise MappingError(f"{where}: tiled_area_fraction must be at most 1")
        census[texture] = row
    core_size = len(census.keys() - MISC_TEXTURES)
    if core_size != SCOPE_SIZE:
        raise MappingError(f"{path}: expected {SCOPE_SIZE} city/harbour textures, got {core_size}")
    misc = census.keys() & MISC_TEXTURES
    if misc and misc != MISC_TEXTURES:
        raise MappingError(f"{path}: missing market/wall textures: {', '.join(sorted(MISC_TEXTURES - misc))}")
    return census


def material_scale(uv: Decimal, material: str) -> Decimal:
    """UV metres per unit / physical tile metres, rounded to hundredths."""
    with localcontext() as context:
        context.prec = max(28, len(uv.as_tuple().digits) + abs(uv.adjusted()) + 10)
        return (uv / TILE_METERS[material]).quantize(Decimal("0.01"), rounding=ROUND_HALF_UP)


def layout_shares(value: str, *, where: str) -> dict[str, Decimal]:
    """Read all four area shares; the mapping stores their measured mixture."""
    shares = {}
    for item in value.split(";"):
        pair = item.split(":")
        if len(pair) != 2 or pair[0] not in LAYOUTS or pair[0] in shares:
            raise MappingError(f"{where}: layout_share must contain each of {', '.join(LAYOUTS)} once")
        layout, raw = pair
        share = _number(raw, f"{where}: layout_share {layout}")
        if share > 1:
            raise MappingError(f"{where}: layout_share {layout} must be at most 1")
        shares[layout] = share
    if set(shares) != set(LAYOUTS):
        raise MappingError(f"{where}: layout_share must contain each of {', '.join(LAYOUTS)} once")
    if abs(sum(shares.values()) - Decimal(1)) > SHARE_TOLERANCE:
        raise MappingError(f"{where}: layout_share must sum to 1 within {SHARE_TOLERANCE}")
    return shares


def _check_intent(row: dict[str, str], where: str) -> None:
    for field in SEMANTIC_FIELDS:
        value = row[field].strip()
        if not re.fullmatch(r"[ -~]+", value) or not re.search(r"[A-Za-z]", value):
            raise MappingError(f"{where}: {field} must contain nonempty English words")
    layout = row["layout"]
    if layout not in LAYOUTS:
        raise MappingError(f"{where}: layout must be one of {', '.join(LAYOUTS)}")
    shares = layout_shares(row["layout_share"], where=where)
    if shares[layout] != max(shares.values()):
        raise MappingError(f"{where}: layout must have the largest layout_share (ties allowed)")
    if row["decision"] == "module":
        if layout == "atlas":
            raise MappingError(f"{where}: atlas layout requires decision exclude")
    elif row["decision"] != "exclude" and row["decision"] != LAYOUT_DECISIONS[layout]:
        raise MappingError(f"{where}: {layout} layout requires decision {LAYOUT_DECISIONS[layout]}")
    if row["anti_repeat"] not in ("on", "off"):
        raise MappingError(f"{where}: anti_repeat must be on or off")
    if row["decision"] != "include" and row["anti_repeat"] != "off":
        raise MappingError(f"{where}: {row['decision']} requires anti_repeat=off")


def _check_coverage(rows: list[dict[str, str]], census: dict[str, dict[str, str]], path: Path) -> None:
    names = set()
    for line, row in enumerate(rows, 2):
        texture = row["texture"]
        if texture in names:
            raise MappingError(f"{path}:{line}: duplicate mapping texture {texture!r}")
        names.add(texture)
    missing = sorted(census.keys() - names)
    extra = sorted(names - census.keys())
    if missing or extra:
        details = []
        if missing:
            details.append(f"missing textures: {', '.join(missing)}")
        if extra:
            details.append(f"extra textures: {', '.join(extra)}")
        raise MappingError(f"{path}: " + "; ".join(details))


def validate_mapping(
    rows: list[dict[str, str]], census: dict[str, dict[str, str]], *, path: Path
) -> None:
    """Validate manual decisions, source measurements and derived scales."""
    _check_coverage(rows, census, path)
    pilot = {row["texture"] for row in selected_rows(rows, "city-2")}
    for line, row in enumerate(rows, 2):
        texture = row["texture"]
        where = f"{path}:{line}: {texture}"
        target = row["target_luminance"]
        if target or texture in pilot or texture in ALPHA_TEXTURES:
            luminance = _number(target, f"{where}: target_luminance")
            if luminance > 1:
                raise MappingError(f"{where}: target_luminance must be at most 1")
        target_coverage = row.get("target_coverage", "")
        if target_coverage or texture in ALPHA_TEXTURES:
            coverage = _number(target_coverage, f"{where}: target_coverage")
            if coverage > 1:
                raise MappingError(f"{where}: target_coverage must be at most 1")
        if row["decision"] not in DECISIONS:
            raise MappingError(f"{where}: decision must be {', '.join(DECISIONS)}")
        if not row["reason"].strip():
            raise MappingError(f"{where}: reason must be nonempty")
        _check_intent(row, where)
        source = census[texture]
        for field in ("area_m2", "uv_m_per_unit_median"):
            actual = _number(row[field], f"{where}: {field}", positive=field != "area_m2")
            if actual != Decimal(source[field]):
                raise MappingError(f"{where}: stale {field}: {row[field]!r}; census has {source[field]!r}; run update")
        _check_measurements(row, where)
        _check_counts(row, where)
        if row["decision"] == "exclude":
            for field in ("class", "state", "scale"):
                if row[field]:
                    raise MappingError(f"{where}: exclude requires empty {field}")
            continue
        material = row["class"]
        classes = DECISION_CLASSES[row["decision"]]
        if material not in classes:
            raise MappingError(f"{where}: unknown class {material!r} for {row['decision']}; expected one of {', '.join(sorted(classes))}")
        if not STATE.fullmatch(row["state"]):
            raise MappingError(f"{where}: state must contain 2–5 English words")
        scale = _number(row["scale"], f"{where}: scale", positive=True)
        expected = material_scale(Decimal(source["uv_m_per_unit_median"]), material) if row["decision"] == "include" else Decimal(1)
        if scale != expected:
            raise MappingError(f"{where}: stale scale: {row['scale']!r}; expected {expected:.2f}; run update")


def check_mapping(mapping: Path = DEFAULT_MAPPING, census_path: Path = DEFAULT_CENSUS) -> list[dict[str, str]]:
    census = read_census(census_path)
    rows = _read_csv(mapping, mapping=True)
    validate_mapping(rows, census, path=mapping)
    return rows


def update_mapping(mapping: Path = DEFAULT_MAPPING, census_path: Path = DEFAULT_CENSUS) -> list[dict[str, str]]:
    census = read_census(census_path)
    rows = _read_csv(mapping, mapping=True)
    _check_coverage(rows, census, mapping)
    for row in rows:
        source = census[row["texture"]]
        for field in ("area_m2", "uv_m_per_unit_median"):
            row[field] = source[field]
        for axis in ("u", "v"):
            if row.get(f"target_period_{axis}_uv"):
                row[f"target_period_{axis}_m"] = format(Decimal(row[f"target_period_{axis}_uv"]) * Decimal(source["uv_m_per_unit_median"]), ".12g")
        if row["decision"] == "include" and row["class"] in TILE_METERS:
            row["scale"] = f"{material_scale(Decimal(source['uv_m_per_unit_median']), row['class']):.2f}"
        elif row["decision"] in ("panel", "strip", "module"):
            row["scale"] = "1.00"
    validate_mapping(rows, census, path=mapping)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="", dir=mapping.parent, delete=False) as stream:
            temporary = Path(stream.name)
            writer = csv.DictWriter(stream, fieldnames=FIELDS, lineterminator="\n")
            writer.writeheader()
            writer.writerows(rows)
        os.replace(temporary, mapping)
    except OSError as error:
        raise MappingError(f"{mapping}: {error}") from error
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()
    return rows


def _summary(rows: list[dict[str, str]]) -> str:
    included = [row for row in rows if row["decision"] == "include"]
    area = sum((Decimal(row["area_m2"]) for row in included), Decimal(0))
    total = sum((Decimal(row["area_m2"]) for row in rows), Decimal(0))
    share = area / total * 100 if total else Decimal(0)
    counts = " ".join(f"{decision}={sum(row['decision'] == decision for row in rows)}" for decision in DECISIONS)
    return f"rows={len(rows)} {counts}; include area={area:.2f} / {total:.2f} m2 ({share:.2f}%)"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("check", "update"))
    parser.add_argument("--mapping", type=Path, default=DEFAULT_MAPPING)
    parser.add_argument("--census", type=Path, default=DEFAULT_CENSUS)
    args = parser.parse_args(argv)
    try:
        action = check_mapping if args.command == "check" else update_mapping
        rows = action(args.mapping, args.census)
    except (MappingError, InvalidOperation) as error:
        print(f"Material mapping {args.command} failed: {error}", file=sys.stderr)
        return 1
    print(f"Material mapping {args.command} passed: {_summary(rows)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
